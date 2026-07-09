#include "idf_wifi.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <new>
#include <vector>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "idf_log.h"
#include "apps/esp_sntp.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char* TAG = "idf_wifi";
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
static constexpr EventBits_t WIFI_FAIL_BIT = BIT1;
static constexpr int WIFI_CONNECT_TIMEOUT_MS = 20000;
static constexpr int WIFI_FAILOVER_OFFLINE_MS = 30000;
static constexpr const char* AP_SSID_PREFIX = "SMS-Forwarder-";
static constexpr gpio_num_t PROVISION_BUTTON_PIN = GPIO_NUM_9;
static constexpr uint32_t PROVISION_BUTTON_HOLD_MS = 5000;
static constexpr uint16_t WIFI_SCAN_RECORD_LIMIT = 40;

static EventGroupHandle_t s_wifi_event_group = nullptr;
static SemaphoreHandle_t s_state_mutex = nullptr;
static esp_netif_t* s_sta_netif = nullptr;
static esp_netif_t* s_ap_netif = nullptr;
static std::atomic<bool> s_started{false};
static bool s_ap_mode = false;
static bool s_ap_manual_mode = false;
static std::atomic<bool> s_has_sta_credentials{false};
static std::atomic<bool> s_sta_configured{false};
static std::string s_ap_ssid;
static std::atomic<bool> s_dns_task_started{false};
static std::atomic<bool> s_mdns_task_started{false};
static std::atomic<bool> s_button_task_started{false};
static std::atomic<bool> s_sntp_started{false};
static std::atomic<bool> s_sta_connected{false};
static std::atomic<int> s_disconnect_streak{0};
static std::atomic<int64_t> s_last_beacon_log_us{0};
static std::atomic<uint32_t> s_suppressed_beacon_logs{0};
static std::atomic<bool> s_suppress_next_connect_log{false};
static esp_timer_handle_t s_reconnect_timer = nullptr;
static char s_ntp_server[128] = "ntp.aliyun.com";
static esp_timer_handle_t s_ap_close_timer = nullptr;
static std::atomic<bool> s_provisioning{false};  // 配网页触发的连接进行中：连上后延时关热点
static constexpr uint32_t AP_PROVISION_HOLD_MS = 20000;  // 连上后保留热点让配网页显示 IP

static esp_err_t start_provisioning_ap(bool manual = false);
static void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void schedule_ap_close(uint32_t delay_ms);
static bool schedule_sta_failover(const char* reason);

struct StaCredential {
    std::string ssid;
    std::string pass;
    int index = 1;
    bool fallback = false;
};

static std::vector<StaCredential> s_sta_candidates;
static std::atomic<int> s_current_candidate_index{0};
static std::atomic<int64_t> s_offline_since_us{0};
static std::atomic<int> s_failover_attempts_in_round{0};
static std::atomic<bool> s_failover_task_running{false};

struct ApState {
    bool mode = false;
    bool manual = false;
    std::string ssid;
};

static ApState ap_state_snapshot()
{
    ApState state;
    // portMAX_DELAY：持锁方只做几次赋值(微秒级)。带超时的版本一旦超时会把
    // "AP 开着"误报成"AP 关闭"，后果是热点该关不关/该答不答
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
        state.mode = s_ap_mode;
        state.manual = s_ap_manual_mode;
        state.ssid = s_ap_ssid;
        xSemaphoreGive(s_state_mutex);
    }
    return state;
}

static void set_ap_state(bool mode, bool manual, const std::string& ssid)
{
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_ap_mode = mode;
        s_ap_manual_mode = manual;
        s_ap_ssid = ssid;
        xSemaphoreGive(s_state_mutex);
    }
}

// 配网连接成功后延时关闭热点：给配网页时间显示 IP，再切回纯 STA
static void ap_close_timer_cb(void*)
{
    if (!ap_state_snapshot().mode) return;  // 已经关了
    if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
        set_ap_state(false, false, std::string());
        idf_log_line("配网热点已关闭，请改用设备 IP 或 http://sms.local 访问");
    }
}

static void schedule_ap_close(uint32_t delay_ms)
{
    if (!s_ap_close_timer) {
        const esp_timer_create_args_t targs = {
            .callback = &ap_close_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ap_close",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&targs, &s_ap_close_timer) != ESP_OK) {
            ap_close_timer_cb(nullptr);  // 创建失败退回立即关闭，避免卡在 AP
            return;
        }
    }
    esp_timer_stop(s_ap_close_timer);
    esp_timer_start_once(s_ap_close_timer, static_cast<uint64_t>(delay_ms) * 1000ULL);
}

static bool sta_can_connect()
{
    return s_has_sta_credentials.load(std::memory_order_relaxed) &&
           s_sta_configured.load(std::memory_order_relaxed);
}

static std::vector<StaCredential> build_sta_candidates(const IdfConfig& config)
{
    std::vector<StaCredential> candidates;
    if (!config.wifiSsid.empty()) {
        candidates.push_back({config.wifiSsid, config.wifiPass, 1, config.wifiFromFallback});
    }
    if (!config.wifiSsid2.empty() && config.wifiSsid2 != config.wifiSsid) {
        candidates.push_back({config.wifiSsid2, config.wifiPass2, 2, false});
    }
    return candidates;
}

static int find_sta_candidate_index(const StaCredential& cred)
{
    for (size_t i = 0; i < s_sta_candidates.size(); ++i) {
        const StaCredential& item = s_sta_candidates[i];
        if (item.index == cred.index && item.ssid == cred.ssid) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static void note_sta_candidate_started(const StaCredential& cred)
{
    int pos = find_sta_candidate_index(cred);
    if (pos >= 0) s_current_candidate_index.store(pos, std::memory_order_relaxed);
}

static int next_sta_candidate_index(void)
{
    size_t count = s_sta_candidates.size();
    if (count < 2) return -1;
    int current = s_current_candidate_index.load(std::memory_order_relaxed);
    if (current < 0 || current >= static_cast<int>(count)) current = 0;
    return (current + 1) % static_cast<int>(count);
}

static bool sta_failover_still_needed(void)
{
    if (s_sta_connected.load(std::memory_order_relaxed)) return false;
    int64_t offline_since = s_offline_since_us.load(std::memory_order_relaxed);
    if (offline_since <= 0) return false;
    return esp_timer_get_time() - offline_since >=
           static_cast<int64_t>(WIFI_FAILOVER_OFFLINE_MS) * 1000LL;
}

static std::string format_epoch_local(time_t epoch, int tz_offset_min)
{
    if (epoch < 1700000000) return {};
    time_t shifted = epoch + static_cast<time_t>(tz_offset_min) * 60;
    struct tm tmv = {};
    gmtime_r(&shifted, &tmv);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

static std::atomic<bool> s_ntp_first_logged{false};
static std::atomic<int64_t> s_ntp_manual_us{-1};

static void cleanup_wifi_start_resources(bool wifi_inited,
                                         bool wifi_event_registered,
                                         bool ip_event_registered)
{
    if (ip_event_registered) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);
    }
    if (wifi_event_registered) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    }
    if (wifi_inited) esp_wifi_deinit();
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = nullptr;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = nullptr;
    }
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = nullptr;
    }
    if (s_state_mutex) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = nullptr;
    }
    s_has_sta_credentials.store(false, std::memory_order_relaxed);
    s_sta_configured.store(false, std::memory_order_relaxed);
    s_sta_connected.store(false, std::memory_order_relaxed);
    s_sta_candidates.clear();
    s_current_candidate_index.store(0, std::memory_order_relaxed);
    s_offline_since_us.store(0, std::memory_order_relaxed);
    s_failover_attempts_in_round.store(0, std::memory_order_relaxed);
    s_failover_task_running.store(false, std::memory_order_relaxed);
}

static void sntp_sync_cb(timeval*)
{
    // 只在首次同步成功或手动校时后打日志：周期同步静默，避免刷屏。
    // 窗口放宽到 10 分钟(弱网下 SNTP 重试可能很慢)，命中一次即清除标记
    bool first = !s_ntp_first_logged.exchange(true, std::memory_order_relaxed);
    int64_t manual_us = s_ntp_manual_us.load(std::memory_order_relaxed);
    bool manual = manual_us >= 0 && (esp_timer_get_time() - manual_us) < 600000000LL;
    if (manual) s_ntp_manual_us.store(-1, std::memory_order_relaxed);
    if (!first && !manual) return;

    time_t now = time(nullptr);
    // 本回调在 tiT(lwip) 小栈上执行：只取时区，绝不深拷贝整个 IdfConfig(会爆栈)
    std::string local = format_epoch_local(now, idf_config_get_tz_offset());
    if (local.empty()) {
        idf_logf("NTP 时间同步成功，epoch=%ld", static_cast<long>(now));
    } else {
        idf_logf("NTP 时间同步成功：%s (epoch=%ld)", local.c_str(), static_cast<long>(now));
    }
}

static void start_sntp_once()
{
    bool expected = false;
    if (!s_sntp_started.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;

    // 在系统事件任务(栈 4KB)上执行：只取 NTP 服务器名，避免深拷贝整个 IdfConfig
    std::string server = idf_config_get_ntp_server();
    if (server.empty()) server = "ntp.aliyun.com";
    strlcpy(s_ntp_server, server.c_str(), sizeof(s_ntp_server));

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_setservername(0, s_ntp_server);
    // 备用服务器：单一 NTP 不可达时时间永远不同步，会连锁禁用所有按时间调度的任务
    // (需要 CONFIG_LWIP_SNTP_MAX_SERVERS>=3，见 sdkconfig.defaults)
    esp_sntp_setservername(1, "ntp.ntsc.ac.cn");
    esp_sntp_setservername(2, "pool.ntp.org");
    // ESP32-C3 晶振日漂移在秒级，调度粒度是"天"——每 24h 校一次足够，默认 1h 太频繁
    esp_sntp_set_sync_interval(24 * 3600 * 1000);
    esp_sntp_init();
    idf_logf("NTP 时间同步已启动：首选=%s，备用=ntp.ntsc.ac.cn,pool.ntp.org", s_ntp_server);
}

static std::string ip4_to_string(const esp_ip4_addr_t& addr)
{
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&addr));
    return std::string(buf);
}

static std::string mac_to_string(const uint8_t mac[6])
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

static const char* wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED: return "未指定";
        case WIFI_REASON_AUTH_EXPIRE: return "认证过期";
        case WIFI_REASON_AUTH_LEAVE: return "认证离开";
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY: return "空闲断开";
        case WIFI_REASON_ASSOC_TOOMANY: return "AP连接数满";
        case WIFI_REASON_ASSOC_LEAVE: return "关联离开";
        case WIFI_REASON_ASSOC_NOT_AUTHED: return "关联未认证";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "四次握手超时";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "组密钥更新超时";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "四次握手IE不匹配";
        case WIFI_REASON_802_1X_AUTH_FAILED: return "802.1X认证失败";
        case WIFI_REASON_TIMEOUT: return "链路超时";
        case WIFI_REASON_PEER_INITIATED: return "对端主动断开";
        case WIFI_REASON_AP_INITIATED: return "AP主动断开";
        case WIFI_REASON_BEACON_TIMEOUT: return "Beacon超时";
        case WIFI_REASON_NO_AP_FOUND: return "未找到AP";
        case WIFI_REASON_AUTH_FAIL: return "认证失败";
        case WIFI_REASON_ASSOC_FAIL: return "关联失败";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "握手超时";
        case WIFI_REASON_CONNECTION_FAIL: return "连接失败";
        case WIFI_REASON_AP_TSF_RESET: return "AP时钟重置";
        case WIFI_REASON_ROAMING: return "漫游切换";
        case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG: return "关联等待过长";
        case WIFI_REASON_SA_QUERY_TIMEOUT: return "SA查询超时";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "未找到兼容安全AP";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "认证方式不匹配";
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "信号低于阈值";
        default: return "未知原因";
    }
}

static void log_wifi_disconnect_once(uint8_t reason, int8_t rssi)
{
    static constexpr int64_t BEACON_LOG_INTERVAL_US = 5LL * 60LL * 1000000LL;
    bool beacon_timeout = reason == WIFI_REASON_BEACON_TIMEOUT;
    int64_t now_us = esp_timer_get_time();
    uint32_t suppressed = 0;

    if (beacon_timeout) {
        int64_t last_us = s_last_beacon_log_us.load(std::memory_order_relaxed);
        if (last_us > 0 && now_us - last_us < BEACON_LOG_INTERVAL_US) {
            s_suppressed_beacon_logs.fetch_add(1, std::memory_order_relaxed);
            s_suppress_next_connect_log.store(true, std::memory_order_relaxed);
            return;
        }
        s_last_beacon_log_us.store(now_us, std::memory_order_relaxed);
        suppressed = s_suppressed_beacon_logs.exchange(0, std::memory_order_relaxed);
    } else {
        suppressed = s_suppressed_beacon_logs.exchange(0, std::memory_order_relaxed);
    }

    char rssi_text[24] = {};
    if (rssi < 0) {
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", static_cast<int>(rssi));
    } else {
        snprintf(rssi_text, sizeof(rssi_text), "未知");
    }
    const char* reason_name = wifi_disconnect_reason_name(reason);
    char tail[80] = {};
    if (suppressed) {
        snprintf(tail, sizeof(tail), "，此前 Beacon 瞬断%lu次已合并",
                 static_cast<unsigned long>(suppressed));
    }
    idf_logf("WiFi 断开: %s(%u)，RSSI=%s，自动重连%s",
             reason_name, static_cast<unsigned>(reason), rssi_text, tail);
    s_suppress_next_connect_log.store(false, std::memory_order_relaxed);
}

static void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (sta_can_connect()) esp_wifi_connect();
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected.store(false, std::memory_order_relaxed);
        int64_t now_us = esp_timer_get_time();
        int64_t expected = 0;
        s_offline_since_us.compare_exchange_strong(
            expected, now_us, std::memory_order_relaxed, std::memory_order_relaxed);
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        // 前几次断开立即重连(快速恢复瞬断)；持续失败后退避，交给 15s 看门狗定时器，
        // 避免错误密码/信号差时无间隔连接风暴打断配网 AP 和 WiFi 扫描
        int streak = s_disconnect_streak.fetch_add(1, std::memory_order_relaxed) + 1;
        if (streak == 1 && event_data) {
            // 只在断开链的第一次记 Web 日志；频繁 Beacon 瞬断合并，避免日志页刷屏。
            auto* ev = static_cast<wifi_event_sta_disconnected_t*>(event_data);
            log_wifi_disconnect_once(ev->reason, ev->rssi);
        }
        if (streak <= 3 && sta_can_connect()) {
            esp_wifi_connect();
            ESP_LOGW(TAG, "STA 断开，立即重连(第%d次)", streak);
        } else {
            ESP_LOGW(TAG, "STA 连续断开%d次，转入定时重连", streak);
        }
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        s_sta_connected.store(true, std::memory_order_relaxed);
        s_disconnect_streak.store(0, std::memory_order_relaxed);
        s_offline_since_us.store(0, std::memory_order_relaxed);
        s_failover_attempts_in_round.store(0, std::memory_order_relaxed);
        ESP_LOGI(TAG, "STA 已获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (!s_suppress_next_connect_log.exchange(false, std::memory_order_relaxed)) {
            idf_logf("WiFi 已连接，IP=" IPSTR, IP2STR(&event->ip_info.ip));
        }
        start_sntp_once();
        if (s_wifi_event_group) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        ApState ap = ap_state_snapshot();
        if (ap.mode && s_has_sta_credentials.load(std::memory_order_relaxed)) {
            if (s_provisioning.exchange(false, std::memory_order_relaxed)) {
                // 配网页触发的连接成功：先留住热点让配网页显示 IP，再延时自动关闭
                schedule_ap_close(AP_PROVISION_HOLD_MS);
                idf_logf("配网连接成功，%lu 秒后自动关闭热点",
                         static_cast<unsigned long>(AP_PROVISION_HOLD_MS / 1000));
            } else if (!ap.manual) {
                if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
                    set_ap_state(false, false, std::string());
                    idf_log_line("STA 已恢复连接，已关闭配网热点");
                }
            }
        }
    }
}

// 15s 周期重连看门狗：事件链(断开→重连)任何一环失败(如扫描期间 esp_wifi_connect
// 返回错误)都不会再有新事件，仅靠事件驱动设备会永久离线。这里兜底补发连接。
static void reconnect_watchdog_cb(void*)
{
    if (!s_started.load(std::memory_order_relaxed) || !sta_can_connect()) return;
    if (s_sta_connected.load(std::memory_order_relaxed)) return;
    static uint32_t tick = 0;  // esp_timer 回调串行执行，无并发
    ++tick;
    // 配网热点开启时降频到 60s：连接尝试会把 STA 拉去跑信道，影响 AP 客户端与扫描
    ApState ap = ap_state_snapshot();
    if (ap.mode && (tick % 4) != 0) return;
    int64_t now_us = esp_timer_get_time();
    int64_t offline_since = s_offline_since_us.load(std::memory_order_relaxed);
    if (offline_since <= 0) {
        int64_t expected = offline_since;
        s_offline_since_us.compare_exchange_strong(
            expected, now_us, std::memory_order_relaxed, std::memory_order_relaxed);
        offline_since = now_us;
    }
    if (now_us - offline_since >= static_cast<int64_t>(WIFI_FAILOVER_OFFLINE_MS) * 1000LL &&
        schedule_sta_failover("离线超过30秒")) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "看门狗重连发起失败: %s", esp_err_to_name(err));
    }
}

static void dns_captive_task(void*)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        s_dns_task_started.store(false, std::memory_order_relaxed);
        idf_log_line("配网 DNS 创建失败");
        vTaskDelete(nullptr);
        return;
    }

    timeval timeout = {};
    timeout.tv_sec = 1;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        idf_log_line("配网 DNS 绑定 53 端口失败");
        close(sock);
        s_dns_task_started.store(false, std::memory_order_relaxed);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t req[512];
    int ap_off_seconds = 0;
    while (true) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, req, sizeof(req), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        // 立即报错(非超时，如 lwIP 缺内存返回 ENOMEM)时必须让出 CPU：
        // 节奏本来全靠 1s 接收超时，错误直返会变成高优先级忙等，饿死空闲任务
        if (len < 0 && errno != EWOULDBLOCK && errno != EAGAIN) vTaskDelay(pdMS_TO_TICKS(200));
        if (!ap_state_snapshot().mode) {
            // 配网结束后释放 53 端口和任务(recv 超时 1s，约 10s 后自退出；再次配网会重建)
            if (++ap_off_seconds >= 10) break;
            continue;
        }
        ap_off_seconds = 0;
        if (len < 12) continue;

        uint16_t qd = (static_cast<uint16_t>(req[4]) << 8) | req[5];
        if (qd == 0) continue;
        int pos = 12;
        while (pos < len && req[pos] != 0) {
            pos += req[pos] + 1;
        }
        if (pos + 5 > len) continue;
        int question_end = pos + 5;
        uint8_t resp[560];
        if (question_end + 16 > static_cast<int>(sizeof(resp))) continue;
        memcpy(resp, req, question_end);
        resp[2] = 0x81; resp[3] = 0x80;  // standard response, no error
        resp[4] = 0x00; resp[5] = 0x01;  // 只回带了第一个问题，qdcount 必须同步改成 1
        resp[6] = 0x00; resp[7] = 0x01;  // one A answer
        resp[8] = resp[9] = resp[10] = resp[11] = 0;
        int out = question_end;
        resp[out++] = 0xC0; resp[out++] = 0x0C;  // name pointer
        resp[out++] = 0x00; resp[out++] = 0x01;  // A
        resp[out++] = 0x00; resp[out++] = 0x01;  // IN
        resp[out++] = 0x00; resp[out++] = 0x00; resp[out++] = 0x00; resp[out++] = 0x3C;  // TTL
        resp[out++] = 0x00; resp[out++] = 0x04;
        resp[out++] = 192; resp[out++] = 168; resp[out++] = 1; resp[out++] = 1;
        sendto(sock, resp, out, 0, reinterpret_cast<sockaddr*>(&from), from_len);
    }

    close(sock);
    s_dns_task_started.store(false, std::memory_order_relaxed);
    idf_log_line("配网已结束，DNS 门户任务已退出");
    vTaskDelete(nullptr);
}

static void start_dns_task_once()
{
    bool expected = false;
    if (!s_dns_task_started.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    BaseType_t ok = xTaskCreate(dns_captive_task, "idf_dns", 3072, nullptr, 2, nullptr);
    if (ok != pdPASS) {
        s_dns_task_started.store(false, std::memory_order_relaxed);
        idf_log_line("配网 DNS 任务启动失败");
    }
}

// —— 轻量 mDNS 应答器，只服务 sms.local ——
// 针对"sms.local 先能用、几分钟后失效"做过的关键修复：
// 1) 组播成员每 60s 强制退组重加。IGMP snooping 交换机/网状路由在无查询器的
//    家庭网络里几分钟就老化转发表项，而 lwIP 对已加入的组不会重发 IGMP 报告，
//    必须周期性 DROP+ADD 逼它重发，否则设备从此收不到任何查询；
// 2) STA 与 AP 两张网卡同时加组(配网保持期手机在热点侧也能解析)；
// 3) 来源端口≠5353 的一次性/传统查询按 RFC 6762 §6.7 单播回源端口：复制查询
//    ID、回带问题段、TTL≤10s、不设 cache-flush，路由器 DNS 代理类客户端才收得到；
// 4) QU 位按单播应答；AAAA 查询回 NSEC(声明只有 A)，双栈解析器不必等超时；
// 5) 新加组后主动通告两次，客户端无需等到下次查询。

static constexpr char kMdnsName[] = "\x03sms\x05local";

static bool mdns_query_matches_sms_local(const uint8_t* packet, int len, int offset)
{
    int pos = offset;
    for (size_t i = 0; i < sizeof(kMdnsName) - 1; ++i) {
        if (pos >= len) return false;
        char a = static_cast<char>(packet[pos++]);
        char b = kMdnsName[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != b) return false;
    }
    return pos < len && packet[pos] == 0;
}

// 依据询问方地址选择要通告的 IP：热点网段的客户端给热点 IP，其余给 STA IP
static bool mdns_pick_ip(uint32_t from_addr, uint8_t out[4])
{
    bool ap = ap_state_snapshot().mode;
    if (ap && (from_addr & inet_addr("255.255.255.0")) == inet_addr("192.168.1.0")) {
        out[0] = 192; out[1] = 168; out[2] = 1; out[3] = 1;
        return true;
    }
    esp_netif_ip_info_t ip = {};
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        memcpy(out, &ip.ip.addr, 4);
        return true;
    }
    if (ap) {
        out[0] = 192; out[1] = 168; out[2] = 1; out[3] = 1;
        return true;
    }
    return false;
}

struct MdnsAnswerPlan {
    bool wantA = false;
    bool wantNsec = false;   // AAAA 查询 → NSEC"仅存在 A 记录"
    bool legacy = false;     // 来源端口≠5353 的传统查询
    bool unicast = false;    // legacy 或 QU 位
    uint16_t id = 0;
    uint16_t firstQtype = 0;
};

static int mdns_append_name(uint8_t* buf, int out)
{
    memcpy(buf + out, kMdnsName, sizeof(kMdnsName) - 1);
    out += sizeof(kMdnsName) - 1;
    buf[out++] = 0;
    return out;
}

static int mdns_append_ttl(uint8_t* buf, int out, uint32_t ttl)
{
    buf[out++] = static_cast<uint8_t>(ttl >> 24);
    buf[out++] = static_cast<uint8_t>(ttl >> 16);
    buf[out++] = static_cast<uint8_t>(ttl >> 8);
    buf[out++] = static_cast<uint8_t>(ttl);
    return out;
}

// resp 至少 128 字节：头 12 + 问题 15 + A 记录 25 + NSEC 35 = 87
static int mdns_build_response(uint8_t* resp, const MdnsAnswerPlan& plan, const uint8_t ip[4])
{
    uint32_t ttl = plan.legacy ? 10 : 120;             // RFC 6762 §6.7 传统应答 TTL≤10s
    uint16_t rrclass = plan.legacy ? 0x0001 : 0x8001;  // 传统应答不设 cache-flush 位
    int an = (plan.wantA ? 1 : 0) + (plan.wantNsec ? 1 : 0);
    int out = 0;
    resp[out++] = static_cast<uint8_t>(plan.id >> 8);
    resp[out++] = static_cast<uint8_t>(plan.id);
    resp[out++] = 0x84; resp[out++] = 0x00;            // response + authoritative
    resp[out++] = 0; resp[out++] = plan.legacy ? 1 : 0;
    resp[out++] = 0; resp[out++] = static_cast<uint8_t>(an);
    resp[out++] = 0; resp[out++] = 0;
    resp[out++] = 0; resp[out++] = 0;
    if (plan.legacy) {  // 传统应答需回带问题段
        out = mdns_append_name(resp, out);
        resp[out++] = static_cast<uint8_t>(plan.firstQtype >> 8);
        resp[out++] = static_cast<uint8_t>(plan.firstQtype);
        resp[out++] = 0; resp[out++] = 1;
    }
    if (plan.wantA) {
        out = mdns_append_name(resp, out);
        resp[out++] = 0; resp[out++] = 1;  // A
        resp[out++] = static_cast<uint8_t>(rrclass >> 8);
        resp[out++] = static_cast<uint8_t>(rrclass);
        out = mdns_append_ttl(resp, out, ttl);
        resp[out++] = 0; resp[out++] = 4;
        memcpy(resp + out, ip, 4); out += 4;
    }
    if (plan.wantNsec) {
        out = mdns_append_name(resp, out);
        resp[out++] = 0; resp[out++] = 0x2F;  // NSEC
        resp[out++] = static_cast<uint8_t>(rrclass >> 8);
        resp[out++] = static_cast<uint8_t>(rrclass);
        out = mdns_append_ttl(resp, out, ttl);
        resp[out++] = 0; resp[out++] = 14;    // rdlength = 名字 11 + 位图 3
        out = mdns_append_name(resp, out);    // next domain = 自身
        resp[out++] = 0; resp[out++] = 1; resp[out++] = 0x40;  // 窗口 0 长度 1，仅类型 1(A)
    }
    return out;
}

static void mdns_sms_task(void*)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        s_mdns_task_started.store(false, std::memory_order_relaxed);
        idf_log_line("mDNS socket 创建失败");
        vTaskDelete(nullptr);
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    timeval timeout = {};
    timeout.tv_sec = 1;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5353);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        idf_log_line("mDNS 绑定 5353 端口失败");
        close(sock);
        s_mdns_task_started.store(false, std::memory_order_relaxed);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t ttl = 255;  // RFC 6762 要求组播 TTL=255
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    auto sta_addr = []() -> uint32_t {
        esp_netif_ip_info_t ip = {};
        if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            return ip.ip.addr;  // 网络字节序
        }
        return 0;
    };
    auto ap_addr = []() -> uint32_t {
        return ap_state_snapshot().mode ? inet_addr("192.168.1.1") : 0;
    };
    // 退组再加组：既处理网卡/IP 变化，也用于周期性逼 lwIP 重发 IGMP 报告
    // (对已加入的组重复 ADD 只会加引用计数，不会重发报告)。返回"是否新加入"。
    auto refresh_membership = [&](uint32_t& tracked, uint32_t cur, bool force) -> bool {
        if (cur == tracked && !force) return false;
        bool changed = (cur != tracked);
        if (tracked != 0) {
            ip_mreq d = {};
            d.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
            d.imr_interface.s_addr = tracked;
            setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &d, sizeof(d));
            tracked = 0;
        }
        if (cur != 0) {
            ip_mreq a = {};
            a.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
            a.imr_interface.s_addr = cur;
            if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &a, sizeof(a)) == 0) {
                tracked = cur;
                if (changed) {
                    idf_log_line("mDNS 已加入组播: http://sms.local");
                    return true;
                }
            }
        }
        return false;
    };

    uint32_t joined_sta = 0;
    uint32_t joined_ap = 0;
    int64_t last_refresh_us = esp_timer_get_time();
    int announce_left = 0;
    uint8_t req[512];
    uint8_t resp[128];

    while (true) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, req, sizeof(req), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        // 同配网 DNS：立即报错时让出 CPU，防止 lwIP 内存紧张期高优先级忙等
        if (len < 0 && errno != EWOULDBLOCK && errno != EAGAIN) vTaskDelay(pdMS_TO_TICKS(200));

        // recv 超时约 1s 一轮：网卡一变立即迁移；每 60s 强制刷新对抗 snooping 老化
        int64_t now_us = esp_timer_get_time();
        bool force = (now_us - last_refresh_us) >= 60000000LL;
        if (force) last_refresh_us = now_us;
        bool newly_joined = refresh_membership(joined_sta, sta_addr(), force);
        newly_joined = refresh_membership(joined_ap, ap_addr(), force) || newly_joined;
        if (newly_joined) announce_left = 2;
        if (announce_left > 0) {
            // 主动通告：相邻两次天然间隔≥1s(recv 超时节拍)
            uint8_t ip[4] = {};
            if (mdns_pick_ip(0, ip)) {
                MdnsAnswerPlan ann;
                ann.wantA = true;
                int n = mdns_build_response(resp, ann, ip);
                sockaddr_in dst = {};
                dst.sin_family = AF_INET;
                dst.sin_port = htons(5353);
                dst.sin_addr.s_addr = inet_addr("224.0.0.251");
                sendto(sock, resp, n, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            }
            --announce_left;
        }

        if (len < 12) continue;
        if (req[2] & 0xF8) continue;  // 忽略响应包(QR=1)与非标准 opcode

        uint16_t qd = (static_cast<uint16_t>(req[4]) << 8) | req[5];
        int pos = 12;
        MdnsAnswerPlan plan;
        plan.legacy = (from.sin_port != htons(5353));
        if (plan.legacy) plan.id = (static_cast<uint16_t>(req[0]) << 8) | req[1];
        for (uint16_t q = 0; q < qd && pos < len; ++q) {
            bool matched = mdns_query_matches_sms_local(req, len, pos);
            // 跳过名字：0xC0 开头的压缩指针是 2 字节终结符，不能当长度前缀
            while (pos < len) {
                uint8_t b = req[pos];
                if (b == 0) { pos += 1; break; }
                if (b >= 0xC0) { pos += 2; break; }
                pos += b + 1;
            }
            if (pos + 4 > len) break;
            uint16_t qtype = (static_cast<uint16_t>(req[pos]) << 8) | req[pos + 1];
            uint16_t qclass = (static_cast<uint16_t>(req[pos + 2]) << 8) | req[pos + 3];
            pos += 4;
            if (!matched) continue;
            if (qtype == 1 || qtype == 255) plan.wantA = true;
            else if (qtype == 28) plan.wantNsec = true;
            else continue;
            if (plan.firstQtype == 0) plan.firstQtype = qtype;
            if (qclass & 0x8000) plan.unicast = true;  // QU 位：请求单播应答
        }
        if (!plan.wantA && !plan.wantNsec) continue;
        if (plan.legacy) plan.unicast = true;

        uint8_t ip[4] = {};
        if (!mdns_pick_ip(from.sin_addr.s_addr, ip)) continue;
        int n = mdns_build_response(resp, plan, ip);
        sockaddr_in dst = {};
        if (plan.unicast) {
            dst = from;
        } else {
            dst.sin_family = AF_INET;
            dst.sin_port = htons(5353);
            dst.sin_addr.s_addr = inet_addr("224.0.0.251");
        }
        sendto(sock, resp, n, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    }
}

static void start_mdns_task_once()
{
    bool expected = false;
    if (!s_mdns_task_started.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    BaseType_t ok = xTaskCreate(mdns_sms_task, "idf_mdns", 3072, nullptr, 2, nullptr);
    if (ok != pdPASS) {
        s_mdns_task_started.store(false, std::memory_order_relaxed);
        idf_log_line("mDNS responder 启动失败");
    }
}

static esp_err_t configure_ap_netif(void)
{
    esp_netif_ip_info_t ip_info = {};
    esp_netif_set_ip4_addr(&ip_info.ip, 192, 168, 1, 1);
    esp_netif_set_ip4_addr(&ip_info.gw, 192, 168, 1, 1);
    esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ap_netif));
    esp_err_t err = esp_netif_set_ip_info(s_ap_netif, &ip_info);
    if (err != ESP_OK) return err;

    const char* captive_uri = "http://192.168.1.1/";
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
        s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
        const_cast<char*>(captive_uri), strlen(captive_uri)));
    err = esp_netif_dhcps_start(s_ap_netif);
    return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? ESP_OK : err;
}

static esp_err_t start_provisioning_ap(bool manual)
{
    ApState ap = ap_state_snapshot();
    if (ap.mode) {
        if (manual && !ap.manual) {
            set_ap_state(true, true, ap.ssid);
            idf_log_line("BOOT长按触发：保留当前配网热点，已切为手动配网模式");
        }
        start_dns_task_once();
        idf_logf("配网热点已开启，请访问 http://192.168.1.1/");
        return ESP_OK;
    }

    uint8_t mac[6] = {};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        idf_logf("配网热点启动失败: 读取 MAC 失败 %s", esp_err_to_name(err));
        return err;
    }
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
    err = configure_ap_netif();
    if (err != ESP_OK) {
        idf_logf("配网热点启动失败: 配置 AP IP 失败 %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t ap_config = {};
    strlcpy(reinterpret_cast<char*>(ap_config.ap.ssid), ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        idf_logf("配网热点启动失败: 设置 APSTA 模式失败 %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        idf_logf("配网热点启动失败: 设置 AP 参数失败 %s", esp_err_to_name(err));
        return err;
    }
    if (sta_can_connect()) ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    set_ap_state(true, manual, ssid);
    start_dns_task_once();
    ESP_LOGW(TAG, "%s: %s, http://192.168.1.1/",
             manual ? "BOOT长按触发，已开启配网热点" : "已开启配网热点", ssid);
    idf_logf("%s: %s, http://192.168.1.1/",
             manual ? "BOOT长按触发，已开启配网热点" : "已开启配网热点", ssid);
    return ESP_OK;
}

static void provision_button_task(void*)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PROVISION_BUTTON_PIN;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    int64_t down_since = 0;
    bool triggered = false;
    while (true) {
        bool pressed = gpio_get_level(PROVISION_BUTTON_PIN) == 0;
        int64_t now_ms = esp_timer_get_time() / 1000LL;
        if (!pressed) {
            down_since = 0;
            triggered = false;
        } else {
            if (down_since == 0) down_since = now_ms;
            if (!triggered && now_ms - down_since >= PROVISION_BUTTON_HOLD_MS) {
                triggered = true;
                start_provisioning_ap(true);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// 只发起 STA 连接不等待结果；首连成败由 sta_connect_watch_task 后台判定。
// 这样 app_main 不再被首连阻塞，Web/推送/模组/短信与 WiFi 连接并行启动。
static esp_err_t connect_sta_begin(const StaCredential& cred, bool disconnect_first)
{
    bool was_configured = s_sta_configured.load(std::memory_order_relaxed);
    s_sta_configured.store(false, std::memory_order_relaxed);
    if (disconnect_first) {
        esp_err_t disc_err = esp_wifi_disconnect();
        if (disc_err != ESP_OK) {
            ESP_LOGW(TAG, "切换 WiFi 前断开 STA 失败: %s", esp_err_to_name(disc_err));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_wifi_event_group) xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t sta_config = {};
    strlcpy(reinterpret_cast<char*>(sta_config.sta.ssid), cred.ssid.c_str(), sizeof(sta_config.sta.ssid));
    strlcpy(reinterpret_cast<char*>(sta_config.sta.password), cred.pass.c_str(), sizeof(sta_config.sta.password));
    sta_config.sta.scan_method = WIFI_FAST_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(ap_state_snapshot().mode ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) {
        s_sta_configured.store(was_configured, std::memory_order_relaxed);
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        s_sta_configured.store(was_configured, std::memory_order_relaxed);
        return err;
    }
    s_sta_configured.store(true, std::memory_order_relaxed);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_sta_configured.store(was_configured, std::memory_order_relaxed);
        return err;
    }
    note_sta_candidate_started(cred);
    ESP_LOGI(TAG, "连接 WiFi %d: %s%s", cred.index, cred.ssid.c_str(), cred.fallback ? " (fallback)" : "");
    idf_logf("连接 WiFi %d: %s%s", cred.index, cred.ssid.c_str(), cred.fallback ? " (fallback)" : "");
    return ESP_OK;
}

static void failover_switch_task(void* arg)
{
    int* target_arg = static_cast<int*>(arg);
    int target = target_arg ? *target_arg : -1;
    delete target_arg;

    if (!sta_failover_still_needed()) {
        s_failover_task_running.store(false, std::memory_order_relaxed);
        vTaskDelete(nullptr);
        return;
    }

    if (target >= 0 && target < static_cast<int>(s_sta_candidates.size())) {
        int attempt = s_failover_attempts_in_round.fetch_add(1, std::memory_order_relaxed) + 1;
        if (attempt >= static_cast<int>(s_sta_candidates.size())) {
            if (!ap_state_snapshot().mode) {
                idf_log_line("两组 WiFi 均已尝试失败，开启配网热点");
                ESP_ERROR_CHECK_WITHOUT_ABORT(start_provisioning_ap(false));
            }
            s_failover_attempts_in_round.store(0, std::memory_order_relaxed);
        }

        const StaCredential cred = s_sta_candidates[target];
        idf_logf("WiFi 离线超过30秒，切换到 WiFi %d 重试", cred.index);
        esp_err_t err = connect_sta_begin(cred, true);
        if (err == ESP_OK) {
            s_offline_since_us.store(esp_timer_get_time(), std::memory_order_relaxed);
        } else {
            idf_logf("切换到 WiFi %d 失败: %s", cred.index, esp_err_to_name(err));
        }
    }

    s_failover_task_running.store(false, std::memory_order_relaxed);
    vTaskDelete(nullptr);
}

static bool schedule_sta_failover(const char* reason)
{
    int target = next_sta_candidate_index();
    if (target < 0) return false;

    bool expected = false;
    if (!s_failover_task_running.compare_exchange_strong(
            expected, true, std::memory_order_relaxed, std::memory_order_relaxed)) {
        return true;
    }

    int* arg = new (std::nothrow) int(target);
    if (!arg) {
        s_failover_task_running.store(false, std::memory_order_relaxed);
        return false;
    }

    BaseType_t ok = xTaskCreate(failover_switch_task, "wifi_failover", 4096, arg, 2, nullptr);
    if (ok != pdPASS) {
        delete arg;
        s_failover_task_running.store(false, std::memory_order_relaxed);
        ESP_LOGW(TAG, "WiFi 故障转移任务创建失败: %s", reason ? reason : "unknown");
        return false;
    }
    return true;
}

static bool sta_wait_connect_result(const StaCredential& cred)
{
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        set_ap_state(false, false, std::string());
        return true;
    }
    ESP_LOGW(TAG, "WiFi %d 首次连接超时: %s", cred.index, cred.ssid.c_str());
    idf_logf("WiFi %d 首次连接超时: %s", cred.index, cred.ssid.c_str());
    return false;
}

// 等待首连结果：按配置顺序尝试 WiFi 1/2；全部失败后回退配网 AP。
static void sta_try_candidates(std::vector<StaCredential>& candidates)
{
    for (size_t i = 0; i < candidates.size(); ++i) {
        esp_err_t err = connect_sta_begin(candidates[i], i > 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WiFi %d 连接发起失败(%s)", candidates[i].index, esp_err_to_name(err));
            idf_logf("WiFi %d 连接发起失败(%s)", candidates[i].index, esp_err_to_name(err));
            continue;
        }
        if (sta_wait_connect_result(candidates[i])) return;
        if (i + 1 < candidates.size()) idf_log_line("准备尝试下一组 WiFi");
    }
    ESP_LOGW(TAG, "全部 WiFi 首次连接失败，进入 APSTA 配网模式");
    idf_log_line("全部 WiFi 首次连接失败，进入 APSTA 配网模式");
    ESP_ERROR_CHECK_WITHOUT_ABORT(start_provisioning_ap(false));
}

static void sta_connect_watch_task(void* arg)
{
    std::vector<StaCredential>* candidates = static_cast<std::vector<StaCredential>*>(arg);
    if (candidates) {
        sta_try_candidates(*candidates);
        delete candidates;
    }
    vTaskDelete(nullptr);
}

esp_err_t idf_wifi_start(const IdfConfig& config)
{
    if (s_started.load(std::memory_order_relaxed)) return ESP_OK;

    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) return ESP_ERR_NO_MEM;

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        cleanup_wifi_start_resources(false, false, false);
        return ESP_ERR_NO_MEM;
    }
    std::vector<StaCredential> candidates = build_sta_candidates(config);
    s_sta_candidates = candidates;
    s_current_candidate_index.store(0, std::memory_order_relaxed);
    s_offline_since_us.store(0, std::memory_order_relaxed);
    s_failover_attempts_in_round.store(0, std::memory_order_relaxed);
    s_failover_task_running.store(false, std::memory_order_relaxed);
    s_has_sta_credentials.store(!candidates.empty(), std::memory_order_relaxed);

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) {
        cleanup_wifi_start_resources(false, false, false);
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    bool wifi_inited = false;
    bool wifi_event_registered = false;
    bool ip_event_registered = false;
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        idf_logf("WiFi 初始化失败: %s", esp_err_to_name(err));
        cleanup_wifi_start_resources(false, false, false);
        return err;
    }
    wifi_inited = true;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(err));
        idf_logf("WiFi 存储模式配置失败: %s", esp_err_to_name(err));
        cleanup_wifi_start_resources(wifi_inited, wifi_event_registered, ip_event_registered);
        return err;
    }
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_ps failed: %s", esp_err_to_name(err));
        idf_logf("WiFi 省电模式配置失败: %s", esp_err_to_name(err));
        cleanup_wifi_start_resources(wifi_inited, wifi_event_registered, ip_event_registered);
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WIFI_EVENT handler register failed: %s", esp_err_to_name(err));
        idf_logf("WiFi 事件处理器注册失败: %s", esp_err_to_name(err));
        cleanup_wifi_start_resources(wifi_inited, false, false);
        return err;
    }
    wifi_event_registered = true;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP_EVENT handler register failed: %s", esp_err_to_name(err));
        idf_logf("IP 事件处理器注册失败: %s", esp_err_to_name(err));
        cleanup_wifi_start_resources(wifi_inited, wifi_event_registered, false);
        return err;
    }
    ip_event_registered = true;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        idf_logf("WiFi 启动失败: %s", esp_err_to_name(err));
        cleanup_wifi_start_resources(wifi_inited, wifi_event_registered, ip_event_registered);
        return err;
    }
    s_started.store(true, std::memory_order_relaxed);
    start_mdns_task_once();
    // 3072：任务里 start_provisioning_ap 会用到 wifi_config_t(~132B) + 日志格式化缓冲
    {
        bool expected = false;
        if (s_button_task_started.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            BaseType_t ok = xTaskCreate(provision_button_task, "idf_boot_ap", 3072, nullptr, 2, nullptr);
            if (ok != pdPASS) {
                s_button_task_started.store(false, std::memory_order_relaxed);
                idf_log_line("BOOT 配网按键任务启动失败");
            }
        }
    }

    if (!s_reconnect_timer) {
        const esp_timer_create_args_t targs = {
            .callback = &reconnect_watchdog_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_watchdog",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&targs, &s_reconnect_timer) == ESP_OK) {
            esp_timer_start_periodic(s_reconnect_timer, 15ULL * 1000 * 1000);
        } else {
            idf_log_line("WiFi 重连看门狗创建失败");
        }
    }

    if (candidates.empty()) {
        ESP_LOGW(TAG, "未配置 WiFi，进入配网 AP");
        idf_log_line("未配置 WiFi，进入配网 AP");
        return start_provisioning_ap(false);
    }

    // 3072：任务里 start_provisioning_ap 会用到 wifi_config_t + 日志格式化缓冲
    auto* task_candidates = new (std::nothrow) std::vector<StaCredential>(candidates);
    if (!task_candidates ||
        xTaskCreate(sta_connect_watch_task, "idf_sta_watch", 4096, task_candidates, 2, nullptr) != pdPASS) {
        if (task_candidates) {
            delete task_candidates;
        }
        sta_try_candidates(candidates);  // 任务创建失败退回同步等待，保证配网回退不丢
    }
    return ESP_OK;
}

esp_err_t idf_wifi_resync_ntp(void)
{
    if (!s_started.load(std::memory_order_relaxed)) return ESP_ERR_INVALID_STATE;
    if (!s_sta_connected.load(std::memory_order_relaxed)) return ESP_ERR_INVALID_STATE;  // 未联网无法校时
    s_ntp_manual_us.store(esp_timer_get_time(), std::memory_order_relaxed);  // 手动校时的结果要打日志
    if (!s_sntp_started.load(std::memory_order_relaxed)) {
        start_sntp_once();  // 尚未启动则启动(内部会立即发起一次请求)
        idf_logf("网页触发 NTP 校时：SNTP 已启动");
    } else {
        esp_sntp_restart();  // 已启动则强制立即重新同步
        idf_logf("网页触发 NTP 立即校时");
    }
    return ESP_OK;
}

esp_err_t idf_wifi_reconnect(void)
{
    if (!s_started.load(std::memory_order_relaxed)) return ESP_ERR_INVALID_STATE;
    if (!sta_can_connect()) return ESP_ERR_INVALID_STATE;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    return esp_wifi_connect();
}

esp_err_t idf_wifi_provision_connect(const std::string& ssid, const std::string& pass)
{
    if (!s_started.load(std::memory_order_relaxed)) return ESP_ERR_INVALID_STATE;
    wifi_config_t sta_config = {};
    strlcpy(reinterpret_cast<char*>(sta_config.sta.ssid), ssid.c_str(), sizeof(sta_config.sta.ssid));
    strlcpy(reinterpret_cast<char*>(sta_config.sta.password), pass.c_str(), sizeof(sta_config.sta.password));
    sta_config.sta.scan_method = WIFI_FAST_SCAN;
    sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    // 保持 APSTA：配网页仍连在热点上，连上后由 IP 事件调度延时关热点(见 wifi_event_handler)
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    esp_wifi_disconnect();  // 断开可能存在的旧 STA 连接，确保按新凭据重连
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) return err;
    s_has_sta_credentials.store(!ssid.empty(), std::memory_order_relaxed);
    s_sta_configured.store(true, std::memory_order_relaxed);
    s_provisioning.store(true, std::memory_order_relaxed);
    if (s_wifi_event_group) xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    err = esp_wifi_connect();
    ESP_LOGI(TAG, "配网：保存并连接 %s", ssid.c_str());
    idf_logf("配网热点内保存 WiFi 并尝试连接: %s", ssid.c_str());
    return err;
}

static void json_escape_append(std::string& out, const std::string& value)
{
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
}

esp_err_t idf_wifi_scan_json(std::string& out_json)
{
    if (!s_started.load(std::memory_order_relaxed)) return ESP_ERR_INVALID_STATE;
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) return err;
    if (mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) return err;
    }

    wifi_scan_config_t scan_cfg = {};
    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) return err;

    uint16_t total = 0;
    err = esp_wifi_scan_get_ap_num(&total);
    if (err != ESP_OK) return err;
    uint16_t count = std::min<uint16_t>(total, WIFI_SCAN_RECORD_LIMIT);
    std::vector<wifi_ap_record_t> records(count);
    if (count) {
        err = esp_wifi_scan_get_ap_records(&count, records.data());
        if (err != ESP_OK) return err;
    }

    out_json.clear();
    out_json.reserve(1024);
    out_json += "[";
    bool first = true;
    for (uint16_t i = 0; i < count; ++i) {
        const char* ssid = reinterpret_cast<const char*>(records[i].ssid);
        if (!ssid[0]) continue;
        bool duplicate = false;
        for (uint16_t k = 0; k < count; ++k) {
            if (k == i) continue;
            const char* other = reinterpret_cast<const char*>(records[k].ssid);
            if (strcmp(ssid, other) != 0) continue;
            if (records[k].rssi > records[i].rssi ||
                (records[k].rssi == records[i].rssi && k < i)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        if (!first) out_json += ",";
        first = false;
        out_json += "{\"ssid\":\"";
        json_escape_append(out_json, ssid);
        char tail[96];
        snprintf(tail, sizeof(tail), "\",\"rssi\":%d,\"enc\":%d}",
                 records[i].rssi,
                 records[i].authmode == WIFI_AUTH_OPEN ? 0 : 1);
        out_json += tail;
    }
    out_json += "]";
    return ESP_OK;
}

bool idf_wifi_is_ap_mode(void)
{
    return ap_state_snapshot().mode;
}

IdfWifiStatus idf_wifi_get_status(void)
{
    IdfWifiStatus s;
    ApState ap_state = ap_state_snapshot();
    s.apMode = ap_state.mode;
    s.apSsid = ap_state.ssid;
    if (ap_state.mode) s.apIp = "192.168.1.1";

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) s.mac = mac_to_string(mac);

    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s.staConnected = true;
        s.ssid = reinterpret_cast<const char*>(ap.ssid);
        s.rssi = ap.rssi;
        s.channel = ap.primary;
        s.bssid = mac_to_string(ap.bssid);
    }

    esp_netif_ip_info_t ip = {};
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        s.ip = ip4_to_string(ip.ip);
        s.gw = ip4_to_string(ip.gw);
        s.mask = ip4_to_string(ip.netmask);
    }

    esp_netif_dns_info_t dns = {};
    if (s_sta_netif && esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.u_addr.ip4.addr != 0) {
        s.dns = ip4_to_string(dns.ip.u_addr.ip4);
    }

    return s;
}
