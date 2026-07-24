#include "idf_modem.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "idf_log.h"
#include "nvs.h"

static const char* TAG = "idf_modem";

static constexpr uart_port_t MODEM_UART = UART_NUM_1;
static constexpr gpio_num_t MODEM_TXD = GPIO_NUM_3;
static constexpr gpio_num_t MODEM_RXD = GPIO_NUM_4;
static constexpr gpio_num_t MODEM_EN = GPIO_NUM_5;
static constexpr int MODEM_BAUD = 115200;
static constexpr int UART_RX_BUF = 4096;
static constexpr int MODEM_POWERDOWN_MS = 1200;
static constexpr int MODEM_POWERUP_MIN_MS = 1500;
static constexpr int MODEM_POWERUP_MAX_MS = 6000;
static constexpr uint32_t CELLULAR_KEEPALIVE_MIN_BYTES = 48UL * 1024UL;
static constexpr uint32_t CELLULAR_HTTP_TIMEOUT_MS = 90000UL;
static constexpr uint32_t CELLULAR_PDP_READY_TIMEOUT_MS = 12000UL;
static constexpr uint32_t MODEM_DATA_MODE_RETRY_GAP_MS = 10000UL;
static constexpr uint8_t MODEM_DATA_MODE_RETRY_MAX = 3;
static constexpr uint32_t IDENTITY_RETRY_INTERVAL_MS = 600000UL;
// 用户显式刷新概览模组信息后才做展示型采样：启动期不主动读取身份/信号；
// 采样仍受 at_channel_idle 门控，不与收发/保号抢 AT 通道
static constexpr uint32_t SIGNAL_INTERVAL_WEB_MS = 10000UL;
static constexpr uint32_t SIGNAL_DETAIL_INTERVAL_WEB_MS = 30000UL;
static constexpr uint32_t SIM_CHECK_INTERVAL_MS = 15000UL;  // SIM 热插拔检测轮询间隔
static constexpr int64_t WEB_POLL_ACTIVE_WINDOW_US = 15LL * 1000LL * 1000LL;
static constexpr size_t URC_BUFFER_MAX = 4096;

static SemaphoreHandle_t s_at_mutex = nullptr;
static SemaphoreHandle_t s_status_mutex = nullptr;
static SemaphoreHandle_t s_urc_mutex = nullptr;
// UART 驱动事件队列：URC 一到就唤醒模组任务抓取，替代固定 500ms 轮询延时
static QueueHandle_t s_uart_evt_queue = nullptr;
// 模组事件信号：新 URC 入缓冲/网页短信入队时唤醒短信任务，消除轮询等待
static SemaphoreHandle_t s_event_sem = nullptr;
static IdfModemStatus s_status;
static std::string s_urc_buffer;
static bool s_started = false;
static std::atomic<int> s_reset_request{0};  // 1=AT软重启，2=EN硬重启；由模组任务执行
static bool s_data_mode_retry_pending = false;
static uint8_t s_data_mode_retry_count = 0;
static TickType_t s_next_data_mode_retry = 0;
static int s_logged_csq = -1;
static int s_logged_ber = -1;
static int s_logged_rsrp = 999;
static int s_logged_rsrq = 999;
static int s_logged_sinr = 999;
static bool s_logged_detail_valid = false;
static std::atomic<int> s_logged_sms_storage_code{-1};  // -1=未知，0=MT，1=ME，2=SM
static bool s_identity_static_attempted = false;
static bool s_identity_network_attempted = false;
static std::atomic<int64_t> s_last_web_poll_us{-WEB_POLL_ACTIVE_WINDOW_US};
static std::atomic<uint32_t> s_status_sample_requests{0};
static std::atomic<uint32_t> s_esim_operation_depth{0};
static std::atomic<int64_t> s_sim_refresh_guard_until_us{0};

void idf_modem_signal_event(void);

static void cleanup_start_resources()
{
    if (s_at_mutex) {
        vSemaphoreDelete(s_at_mutex);
        s_at_mutex = nullptr;
    }
    if (s_status_mutex) {
        vSemaphoreDelete(s_status_mutex);
        s_status_mutex = nullptr;
    }
    if (s_urc_mutex) {
        vSemaphoreDelete(s_urc_mutex);
        s_urc_mutex = nullptr;
    }
    if (s_event_sem) {
        vSemaphoreDelete(s_event_sem);
        s_event_sem = nullptr;
    }
    s_urc_buffer.clear();
}

static std::string trim(std::string value)
{
    size_t start = 0;
    while (start < value.size() && isspace(static_cast<unsigned char>(value[start]))) ++start;
    size_t end = value.size();
    while (end > start && isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

static bool parse_long_token(const std::string& value, long& out)
{
    std::string text = trim(value);
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long parsed = strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || errno == ERANGE) return false;
    while (*end && isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return false;
    out = parsed;
    return true;
}

static bool parse_comma_longs(const std::string& text, long* values, int max_values, int& count)
{
    count = 0;
    size_t start = 0;
    while (start < text.size() && count < max_values) {
        size_t comma = text.find(',', start);
        std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        part = trim(part);
        long value = 0;
        if (part.empty() || !parse_long_token(part, value)) return false;
        values[count++] = value;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return count > 0;
}

// tick 回绕安全的超时窗口：`now + timeout` 在 49.7 天(1000Hz tick)回绕时会溢出，
// 使 `now < deadline` 永远为假，所有 AT 读循环瞬间退出。统一改用无符号差值判断。
struct TickDeadline {
    TickType_t start;
    TickType_t span;
    explicit TickDeadline(uint32_t ms) : start(xTaskGetTickCount()), span(pdMS_TO_TICKS(ms)) {}
    bool expired() const { return static_cast<TickType_t>(xTaskGetTickCount() - start) >= span; }
    void restart(uint32_t ms) { start = xTaskGetTickCount(); span = pdMS_TO_TICKS(ms); }
};

// AT 最终结果码：1=OK，-1=ERROR/+CMS ERROR/+CME ERROR(27.005/27.007 定义的失败终结码)，0=未结束。
// 不要求末尾必须再跟 CRLF：部分长命令的最后一个 UART 块可能恰好止于 "OK"。
static int at_final_result(const std::string& resp)
{
    size_t pos = 0;
    while (pos < resp.size()) {
        size_t end = resp.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = resp.size();
        std::string line = trim(resp.substr(pos, end - pos));
        if (line == "OK") return 1;
        if (line == "ERROR" || line.rfind("+CMS ERROR", 0) == 0 ||
            line.rfind("+CME ERROR", 0) == 0) return -1;
        pos = end;
        while (pos < resp.size() && (resp[pos] == '\r' || resp[pos] == '\n')) ++pos;
    }
    return 0;
}

static bool has_cmgs_result(const std::string& resp)
{
    size_t pos = resp.find("+CMGS:");
    if (pos == std::string::npos) return false;
    pos += strlen("+CMGS:");
    while (pos < resp.size() && isspace(static_cast<unsigned char>(resp[pos]))) ++pos;
    if (pos >= resp.size() || !isdigit(static_cast<unsigned char>(resp[pos]))) return false;
    while (pos < resp.size() && isdigit(static_cast<unsigned char>(resp[pos]))) ++pos;
    return true;
}

// 取包含 token 的那一整行(不同 URC 混在同一段响应里时不能只取"第一有效行")
static std::string line_containing(const std::string& resp, size_t pos)
{
    size_t start = resp.rfind('\n', pos);
    start = (start == std::string::npos) ? 0 : start + 1;
    size_t end = resp.find('\n', pos);
    if (end == std::string::npos) end = resp.size();
    std::string line = resp.substr(start, end - start);
    size_t s = 0;
    while (s < line.size() && isspace(static_cast<unsigned char>(line[s]))) ++s;
    size_t e = line.size();
    while (e > s && isspace(static_cast<unsigned char>(line[e - 1]))) --e;
    return line.substr(s, e - s);
}

static bool line_is_payload(const std::string& line, const char* cmd)
{
    if (line.empty() || line == "OK" || line == "ERROR") return false;
    if (cmd && line == cmd) return false;
    return true;
}

static std::string first_payload_line(const std::string& resp, const char* cmd = nullptr)
{
    size_t pos = 0;
    while (pos < resp.size()) {
        size_t end = resp.find('\n', pos);
        if (end == std::string::npos) end = resp.size();
        std::string line = trim(resp.substr(pos, end - pos));
        if (line_is_payload(line, cmd)) return line;
        pos = end + 1;
    }
    return {};
}

static std::string first_digits_line(const std::string& resp, size_t min_len, size_t max_len)
{
    size_t pos = 0;
    while (pos < resp.size()) {
        size_t end = resp.find('\n', pos);
        if (end == std::string::npos) end = resp.size();
        std::string line = trim(resp.substr(pos, end - pos));
        bool digits = !line.empty();
        for (char ch : line) digits = digits && isdigit(static_cast<unsigned char>(ch));
        if (digits && line.size() >= min_len && line.size() <= max_len) return line;
        pos = end + 1;
    }
    return {};
}

static std::string first_digit_run(const std::string& resp, size_t min_len, size_t max_len)
{
    size_t start = std::string::npos;
    for (size_t i = 0; i <= resp.size(); ++i) {
        bool digit = i < resp.size() && isdigit(static_cast<unsigned char>(resp[i]));
        if (digit && start == std::string::npos) {
            start = i;
        } else if (!digit && start != std::string::npos) {
            size_t len = i - start;
            if (len >= min_len && len <= max_len) return resp.substr(start, len);
            start = std::string::npos;
        }
    }
    return {};
}

static bool is_iccid_text(const std::string& value)
{
    if (value.size() < 15 || value.size() > 22) return false;
    bool seen_digit = false;
    bool padding = false;
    uint8_t padding_count = 0;
    for (char ch : value) {
        if (isdigit(static_cast<unsigned char>(ch))) {
            if (padding) return false;
            seen_digit = true;
        } else if (ch == 'F' || ch == 'f') {
            if (!seen_digit) return false;
            padding = true;
            if (++padding_count > 1) return false;
        } else {
            return false;
        }
    }
    return seen_digit;
}

static bool is_imei_text(const std::string& value)
{
    if (value.size() < 14 || value.size() > 17) return false;
    for (char ch : value) {
        if (!isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

static bool is_imsi_text(const std::string& value)
{
    if (value.size() < 14 || value.size() > 16) return false;
    for (char ch : value) {
        if (!isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

static std::string first_quoted(const std::string& line, size_t start = 0)
{
    size_t q1 = line.find('"', start);
    if (q1 == std::string::npos) return {};
    size_t q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return line.substr(q1 + 1, q2 - q1 - 1);
}

static void set_phase(const char* phase)
{
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_status.phase = phase;
        if (strcmp(phase, "powering") == 0 || strcmp(phase, "failed") == 0) {
            s_status.atReady = false;
            s_status.modemReady = false;
        } else if (strcmp(phase, "at_ready") == 0 || strcmp(phase, "registering") == 0) {
            s_status.atReady = true;
            s_status.modemReady = false;
        } else if (strcmp(phase, "ready") == 0) {
            s_status.atReady = true;
            s_status.modemReady = true;
        }
        xSemaphoreGive(s_status_mutex);
    }
}

static void update_status(const IdfModemStatus& patch, bool identity = false, bool signal = false)
{
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    s_status.started = patch.started || s_status.started;
    bool phase_patch = !patch.phase.empty() && patch.phase != "off";
    if (phase_patch) {
        s_status.phase = patch.phase;
        if (patch.phase == "powering" || patch.phase == "failed") {
            s_status.atReady = false;
            s_status.modemReady = false;
        } else if (patch.phase == "at_ready" || patch.phase == "registering") {
            s_status.atReady = true;
            s_status.modemReady = false;
        } else if (patch.phase == "ready") {
            s_status.atReady = true;
            s_status.modemReady = true;
        }
    }
    if (patch.atReady) s_status.atReady = true;
    bool carries_registration = patch.ceregStat >= 0 || patch.modemReady ||
                                patch.phase == "ready" || patch.phase == "registering" ||
                                patch.phase == "failed";
    if (carries_registration) s_status.modemReady = patch.modemReady;
    if (patch.ceregStat >= 0) s_status.ceregStat = patch.ceregStat;
    if (patch.csq >= 0) s_status.csq = patch.csq;
    if (patch.csq >= 0 || patch.ber != 99) s_status.ber = patch.ber;
    if (patch.rsrp != 999) s_status.rsrp = patch.rsrp;
    if (patch.rsrq != 999) s_status.rsrq = patch.rsrq;
    if (patch.sinr != 999) s_status.sinr = patch.sinr;
    if (!patch.mfr.empty()) s_status.mfr = patch.mfr;
    if (!patch.model.empty()) s_status.model = patch.model;
    if (!patch.fwver.empty()) s_status.fwver = patch.fwver;
    if (!patch.imei.empty() && is_imei_text(patch.imei)) s_status.imei = patch.imei;
    if (!patch.iccid.empty() && is_iccid_text(patch.iccid)) s_status.iccid = patch.iccid;
    if (!patch.imsi.empty() && is_imsi_text(patch.imsi)) s_status.imsi = patch.imsi;
    if (!patch.operatorName.empty()) s_status.operatorName = patch.operatorName;
    if (!patch.apnSim.empty()) s_status.apnSim = patch.apnSim;
    if (!patch.cellIp.empty()) s_status.cellIp = patch.cellIp;
    if (!patch.phone.empty()) s_status.phone = patch.phone;
    if (identity) s_status.identityFresh = true;
    if (signal) s_status.signalFresh = true;
    xSemaphoreGive(s_status_mutex);
}

static bool startup_info_complete(void)
{
    bool complete = false;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        complete = s_status.signalFresh &&
                   s_status.identityFresh &&
                   !s_status.mfr.empty() &&
                   !s_status.model.empty() &&
                   !s_status.fwver.empty() &&
                   is_imei_text(s_status.imei) &&
                   is_iccid_text(s_status.iccid) &&
                   is_imsi_text(s_status.imsi) &&
                   !s_status.operatorName.empty() &&
                   s_identity_network_attempted;
        xSemaphoreGive(s_status_mutex);
    }
    return complete;
}

static void reset_identity_sampling_state(void)
{
    s_identity_static_attempted = false;
    s_identity_network_attempted = false;
}

static void set_status_cell_ip(const std::string& ip)
{
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    s_status.cellIp = ip;
    xSemaphoreGive(s_status_mutex);
}

static std::string read_nvs_string(nvs_handle_t nvs, const char* key, size_t max_len)
{
    size_t len = 0;
    esp_err_t err = nvs_get_str(nvs, key, nullptr, &len);
    if (err != ESP_OK || len == 0 || len > max_len + 1) return {};
    std::string value(len, '\0');
    err = nvs_get_str(nvs, key, value.data(), &len);
    if (err != ESP_OK) return {};
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

static void save_identity_cache(const std::string& imei, const std::string& iccid)
{
    bool valid_imei = is_imei_text(imei);
    bool valid_iccid = is_iccid_text(iccid);
    if (!valid_imei && !valid_iccid) return;
    nvs_handle_t nvs = 0;
    if (nvs_open("sms_config", NVS_READWRITE, &nvs) != ESP_OK) return;
    std::string old_imei = read_nvs_string(nvs, "modemImei", 32);
    std::string old_iccid = read_nvs_string(nvs, "modemIccid", 32);
    esp_err_t err = ESP_OK;
    bool changed = false;
    if (valid_imei && imei != old_imei) {
        err = nvs_set_str(nvs, "modemImei", imei.c_str());
        changed = err == ESP_OK;
    }
    if (err == ESP_OK && valid_iccid && iccid != old_iccid) {
        err = nvs_set_str(nvs, "modemIccid", iccid.c_str());
        changed = changed || err == ESP_OK;
    }
    if (err == ESP_OK && changed) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK && changed) idf_log_line("模组身份信息已写入缓存");
}

static void append_urc_text(const std::string& text)
{
    if (text.empty() || !s_urc_mutex) return;
    if (xSemaphoreTake(s_urc_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (text.size() >= URC_BUFFER_MAX) {
        // 长 AT 响应里夹 URC 时，整段转存会突破缓冲上限；保留尾部且尽量从完整行开始。
        size_t start = text.size() - URC_BUFFER_MAX;
        size_t nl = text.find('\n', start);
        if (nl != std::string::npos && nl + 1 < text.size()) start = nl + 1;
        s_urc_buffer.assign(text.data() + start, text.size() - start);
    } else {
        size_t need = s_urc_buffer.size() + text.size();
        if (need > URC_BUFFER_MAX) {
            size_t drop = need - URC_BUFFER_MAX;
            s_urc_buffer.erase(0, std::min(drop, s_urc_buffer.size()));
            size_t nl = s_urc_buffer.find('\n');
            if (nl != std::string::npos && nl + 1 < s_urc_buffer.size()) s_urc_buffer.erase(0, nl + 1);
        }
        s_urc_buffer += text;
    }
    xSemaphoreGive(s_urc_mutex);
    idf_modem_signal_event();  // 立即唤醒短信任务处理，不等它的轮询周期
}

static void append_capped(std::string& out, const uint8_t* data, size_t len, size_t cap);

static void capture_pending_uart_locked(uint32_t max_ms)
{
    // RX 缓冲为空时立即返回：该函数在每条 AT 命令前都会执行，
    // 空转等待 20ms×2 会拖慢所有 AT 操作(健康探测/补收批量删除/身份采样)
    size_t buffered = 0;
    if (uart_get_buffered_data_len(MODEM_UART, &buffered) == ESP_OK && buffered == 0) return;

    std::string pending;
    pending.reserve(256);
    uint8_t buf[128];
    // 静默窗口(有数据就续期)之上必须再加总时长硬上限：模组连续吐数据
    // (下载中途中止的 MHTTP 载荷、复位横幅、错误波特率乱码)时，纯续期
    // 循环永不退出，pending 无限增长直至堆耗尽。URC 缓冲本身只留 4KB 尾部，
    // 这里同样只保留尾部即可，多攒毫无意义。
    TickDeadline hard_deadline(std::max<uint32_t>(max_ms, 1000));
    TickDeadline quiet(max_ms);
    do {
        int got = uart_read_bytes(MODEM_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (got > 0) {
            append_capped(pending, buf, static_cast<size_t>(got), URC_BUFFER_MAX);
            quiet.restart(40);
        }
    } while (!quiet.expired() && !hard_deadline.expired());
    if (!pending.empty()) append_urc_text(pending);
}

// 返回是否成功抢到 AT 通道锁并完成抓取；false=通道正被长任务占用
static bool poll_unsolicited_uart(uint32_t max_ms)
{
    if (!s_at_mutex) return false;
    if (xSemaphoreTakeRecursive(s_at_mutex, 0) != pdTRUE) return false;
    capture_pending_uart_locked(max_ms);
    xSemaphoreGiveRecursive(s_at_mutex);
    return true;
}

static bool at_channel_idle_now(void)
{
    if (idf_modem_esim_operation_active()) return false;
    if (!s_at_mutex) return false;
    if (xSemaphoreTakeRecursive(s_at_mutex, 0) != pdTRUE) return false;
    xSemaphoreGiveRecursive(s_at_mutex);
    return true;
}

static void append_capped(std::string& out, const uint8_t* data, size_t len, size_t cap)
{
    if (!data || len == 0 || cap == 0) return;
    if (len >= cap) {
        out.assign(reinterpret_cast<const char*>(data + len - cap), cap);
        return;
    }
    size_t need = out.size() + len;
    if (need > cap) out.erase(0, need - cap);
    out.append(reinterpret_cast<const char*>(data), len);
}

esp_err_t idf_modem_send_at(const std::string& cmd, uint32_t timeout_ms, std::string& response)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTakeRecursive(s_at_mutex, pdMS_TO_TICKS(timeout_ms + 500)) != pdTRUE) return ESP_ERR_TIMEOUT;

    capture_pending_uart_locked(30);
    std::string wire = cmd;
    wire += "\r\n";
    uart_write_bytes(MODEM_UART, wire.data(), wire.size());

    response.clear();
    response.reserve(512);
    // 响应上限 8KB：满存储的 AT+CMGL 可达十几 KB，无上限会造成堆峰值风险；
    // 截断后 OK/ERROR 终结符仍通过重叠扫描窗口检测，漏收的短信由下轮轮询补齐。
    constexpr size_t MAX_RESPONSE = 8192;
    TickDeadline deadline(timeout_ms);
    uint8_t buf[128];
    std::string scan;  // 跨块重叠扫描窗口，保证被截断/跨块的终结符也能被识别
    esp_err_t ret = ESP_ERR_TIMEOUT;
    while (!deadline.expired()) {
        int got = uart_read_bytes(MODEM_UART, buf, sizeof(buf), pdMS_TO_TICKS(80));
        if (got > 0) {
            size_t room = MAX_RESPONSE > response.size() ? MAX_RESPONSE - response.size() : 0;
            if (room > 0) response.append(reinterpret_cast<const char*>(buf), std::min<size_t>(room, got));
            scan.append(reinterpret_cast<const char*>(buf), got);
            int final_code = at_final_result(scan);
            if (final_code != 0) {
                ret = final_code > 0 ? ESP_OK : ESP_FAIL;
                break;
            }
            if (scan.size() > 32) scan.erase(0, scan.size() - 32);
        }
    }
    if (response.find("+CMT:") != std::string::npos ||
        response.find("+CMTI:") != std::string::npos ||
        response.find("+CLIP:") != std::string::npos ||
        response.find("RING") != std::string::npos) {
        // RING/+CLIP 也要保留：否则整段响铃落在一次持锁 AT 交换内时来电通知丢失
        append_urc_text(response);
    }
    xSemaphoreGiveRecursive(s_at_mutex);
    return ret;
}

esp_err_t idf_modem_send_at_until(const std::string& cmd, const char* token, uint32_t timeout_ms, std::string& response)
{
    if (!s_started || !token || !*token) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTakeRecursive(s_at_mutex, pdMS_TO_TICKS(timeout_ms + 500)) != pdTRUE) return ESP_ERR_TIMEOUT;

    capture_pending_uart_locked(30);
    std::string wire = cmd;
    wire += "\r\n";
    uart_write_bytes(MODEM_UART, wire.data(), wire.size());

    response.clear();
    response.reserve(512);
    constexpr size_t MAX_RESPONSE = 4096;
    TickDeadline deadline(timeout_ms);
    uint8_t buf[128];
    std::string scan;
    esp_err_t ret = ESP_ERR_TIMEOUT;
    while (!deadline.expired()) {
        int got = uart_read_bytes(MODEM_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (got > 0) {
            append_capped(response, buf, static_cast<size_t>(got), MAX_RESPONSE);
            scan.append(reinterpret_cast<const char*>(buf), got);
            if (response.find(token) != std::string::npos || scan.find(token) != std::string::npos) {
                ret = ESP_OK;
                break;
            }
            if (at_final_result(scan) < 0) {
                ret = ESP_FAIL;
                break;
            }
            if (scan.size() > 64) scan.erase(0, scan.size() - 64);
        }
    }
    if (response.find("+CMT:") != std::string::npos ||
        response.find("+CMTI:") != std::string::npos ||
        response.find("+CLIP:") != std::string::npos ||
        response.find("RING") != std::string::npos) {
        // RING/+CLIP 也要保留：否则整段响铃落在一次持锁 AT 交换内时来电通知丢失
        append_urc_text(response);
    }
    xSemaphoreGiveRecursive(s_at_mutex);
    return ret;
}

esp_err_t idf_modem_send_pdu(const std::string& cmgs_cmd, const char* pdu, uint32_t timeout_ms, std::string& response)
{
    if (!s_started || !pdu) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTakeRecursive(s_at_mutex, pdMS_TO_TICKS(timeout_ms + 2000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    capture_pending_uart_locked(30);
    std::string wire = cmgs_cmd;
    wire += "\r\n";
    uart_write_bytes(MODEM_UART, wire.data(), wire.size());

    response.clear();
    response.reserve(512);
    constexpr size_t MAX_RESPONSE = 4096;
    uint8_t buf[128];
    TickDeadline prompt_deadline(5000);
    bool got_prompt = false;
    esp_err_t ret = ESP_ERR_TIMEOUT;
    std::string scan;
    while (!prompt_deadline.expired()) {
        int got = uart_read_bytes(MODEM_UART, buf, sizeof(buf), pdMS_TO_TICKS(80));
        if (got > 0) {
            append_capped(response, buf, static_cast<size_t>(got), MAX_RESPONSE);
            scan.append(reinterpret_cast<const char*>(buf), got);
            if (response.find('>') != std::string::npos || scan.find('>') != std::string::npos) {
                got_prompt = true;
                break;
            }
            if (at_final_result(scan) < 0) {
                ret = ESP_FAIL;  // 提示符阶段就报错(如未注册的 +CMS ERROR)，按失败而非超时上报
                break;
            }
            if (scan.size() > 64) scan.erase(0, scan.size() - 64);
        }
    }

    if (got_prompt) {
        size_t pdu_len = strlen(pdu);
        uart_write_bytes(MODEM_UART, pdu, pdu_len);
        // Ctrl+Z 提交 PDU；encodePDU 生成的缓冲已自带 0x1A 结尾，
        // 避免重复发送在命令模式下多注入一个孤立控制字符
        if (pdu_len == 0 || static_cast<uint8_t>(pdu[pdu_len - 1]) != 0x1A) {
            const uint8_t end = 0x1A;
            uart_write_bytes(MODEM_UART, &end, 1);
        }
        TickDeadline deadline(timeout_ms);
        scan.clear();
        while (!deadline.expired()) {
            int got = uart_read_bytes(MODEM_UART, buf, sizeof(buf), pdMS_TO_TICKS(120));
            if (got > 0) {
                append_capped(response, buf, static_cast<size_t>(got), MAX_RESPONSE);
                scan.append(reinterpret_cast<const char*>(buf), got);
                // 官方手册定义 +CMGS:<mr> 即网络已接受 SMS-SUBMIT；某些固件的尾随 OK
                // 可能没有完整 CRLF，不能因此把已经发送成功的短信等到超时。
                if (has_cmgs_result(response) || has_cmgs_result(scan)) {
                    ret = ESP_OK;
                    break;
                }
                int final_code = at_final_result(scan);
                if (final_code != 0) {
                    ret = final_code > 0 ? ESP_OK : ESP_FAIL;
                    break;
                }
                if (scan.size() > 64) scan.erase(0, scan.size() - 64);
            }
        }
    }

    if (response.find("+CMT:") != std::string::npos ||
        response.find("+CMTI:") != std::string::npos ||
        response.find("+CLIP:") != std::string::npos ||
        response.find("RING") != std::string::npos) {
        // RING/+CLIP 也要保留：否则整段响铃落在一次持锁 AT 交换内时来电通知丢失
        append_urc_text(response);
    }
    xSemaphoreGiveRecursive(s_at_mutex);
    return ret;
}

static bool send_ok(const char* cmd, uint32_t timeout_ms = 1000, std::string* out = nullptr)
{
    std::string resp;
    esp_err_t err = idf_modem_send_at(cmd, timeout_ms, resp);
    if (out) *out = resp;
    return err == ESP_OK;
}

static bool parse_csq(const std::string& resp, int& csq, int& ber)
{
    size_t p = resp.find("+CSQ:");
    if (p == std::string::npos) return false;
    std::string line = line_containing(resp, p);
    const char* token = strstr(line.c_str(), "+CSQ:");
    if (!token) return false;
    long values[2] = {};
    int count = 0;
    if (!parse_comma_longs(token + strlen("+CSQ:"), values, 2, count) || count < 2) return false;
    if (!((values[0] >= 0 && values[0] <= 31) || values[0] == 99)) return false;
    if (!((values[1] >= 0 && values[1] <= 7) || values[1] == 99)) return false;
    csq = static_cast<int>(values[0]);
    ber = static_cast<int>(values[1]);
    return true;
}

static const char* format_ber(int ber, char* buf, size_t len)
{
    if (ber >= 99) return "未知";
    snprintf(buf, len, "%d", ber);
    return buf;
}

static bool parse_cereg(const std::string& resp, int& stat)
{
    size_t p = resp.find("+CEREG:");
    if (p == std::string::npos) return false;
    std::string line = line_containing(resp, p);
    const char* token = strstr(line.c_str(), "+CEREG:");
    if (!token) return false;
    std::string rest = token + strlen("+CEREG:");
    size_t comma = rest.find(',');
    std::string first = trim(rest.substr(0, comma));
    long first_value = -1;
    if (!parse_long_token(first, first_value)) return false;

    long status_value = first_value;
    if (comma != std::string::npos) {
        size_t second_end = rest.find(',', comma + 1);
        std::string second = trim(rest.substr(
            comma + 1, second_end == std::string::npos ? std::string::npos : second_end - comma - 1));
        long second_value = -1;
        // 查询响应是 +CEREG: <n>,<stat>；URC 是 +CEREG: <stat>[,<tac>,...]，
        // 后者第二字段通常是带引号的 TAC，不能把它当注册状态。
        if (parse_long_token(second, second_value)) status_value = second_value;
    }
    if (status_value < 0 || status_value > 5) return false;
    stat = static_cast<int>(status_value);
    return true;
}

// 注意：都要解析"包含 token 的那一行"。CEREG=2 的 URC 可能与查询响应混在同一段，
// 取"第一有效行"会把 +CEREG 行错当成 +COPS/+CGDCONT/+CNUM 的内容。
static std::string parse_cops(const std::string& resp)
{
    size_t p = resp.find("+COPS:");
    if (p == std::string::npos) return {};
    std::string line = line_containing(resp, p);
    // 自动模式下若未先选名称格式，AT+COPS? 只回 "+COPS: 0"(无引号运营商名)。
    // 此时返回空让上层改用 COPS=3,0 重试，而不是把模式位当运营商缓存下来。
    return first_quoted(line);
}

static std::string parse_apn(const std::string& resp)
{
    size_t pos = 0;
    while (true) {
        size_t p = resp.find("+CGDCONT:", pos);
        if (p == std::string::npos) return {};
        std::string line = line_containing(resp, p);
        size_t q1 = line.find('"');
        if (q1 != std::string::npos) {
            size_t q2 = line.find('"', q1 + 1);
            if (q2 != std::string::npos) {
                std::string apn = first_quoted(line, q2 + 1);
                if (!apn.empty()) return apn;
            }
        }
        pos = p + strlen("+CGDCONT:");
    }
}

// 部分模组/卡会把国际 TOA 字节 0x91 误解成 BCD 数字，导致 CNUM 号码出现
// "+19..." 前缀，例如 +1944756... 实际应为 +44756...。这里只在 "19" 后
// 紧跟常见国家/地区码时剥离，避免误伤真实 NANP 号码。
static std::string normalize_msisdn(std::string phone)
{
    size_t start = 0;
    while (start < phone.size() && isspace(static_cast<unsigned char>(phone[start]))) ++start;
    size_t end = phone.size();
    while (end > start && isspace(static_cast<unsigned char>(phone[end - 1]))) --end;
    if (start >= end) return {};
    phone = phone.substr(start, end - start);

    bool had_plus = !phone.empty() && phone[0] == '+';
    std::string digits;
    digits.reserve(phone.size());
    for (size_t i = had_plus ? 1 : 0; i < phone.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(phone[i]);
        if (isdigit(ch)) digits += static_cast<char>(ch);
        else if (!isspace(ch) && ch != '-' && ch != '(' && ch != ')') {
            return phone;
        }
    }
    if (digits.size() < 8) return phone;
    if (had_plus && digits.size() == 11 && digits[0] == '1') {
        return std::string("+") + digits;
    }

    static const char* kCountryCodes[] = {
        "44", "86", "33", "49", "81", "61", "91", "39", "34", "82", "65",
        "852", "886", "853", "855", "856", "60", "62", "63", "66", "84",
        "90", "971", "966", "974", "973", "968", "965", "962", "961",
        "20", "27", "234", "254", "255", "256", "233", "212",
        "7", "380", "48", "40", "36", "30", "31", "32",
        "41", "43", "45", "46", "47", "351", "352", "353", "354", "358",
        "420", "421", "370", "371", "372", "373", "374", "375", "376",
        "52", "55", "54", "56", "57", "51", "58",
        "1",
    };
    if (digits.rfind("19", 0) == 0) {
        std::string rest = digits.substr(2);
        for (const char* cc : kCountryCodes) {
            size_t n = strlen(cc);
            if (rest.size() < n + 6) continue;
            if (rest.compare(0, n, cc) != 0) continue;
            if (strcmp(cc, "1") == 0) continue;
            return std::string("+") + rest;
        }
    }
    if (had_plus) return std::string("+") + digits;
    return phone;
}

static std::string parse_cnum_phone(const std::string& resp)
{
    size_t p = resp.find("+CNUM:");
    if (p == std::string::npos) return {};
    std::string line = line_containing(resp, p);
    std::string alpha = first_quoted(line);
    size_t after_alpha = line.find('"');
    if (after_alpha == std::string::npos) return {};
    after_alpha = line.find('"', after_alpha + 1);
    if (after_alpha == std::string::npos) return {};
    std::string phone = first_quoted(line, after_alpha + 1);
    if (phone.empty()) phone = alpha;
    return normalize_msisdn(phone);
}

static bool starts_with(const std::string& text, const char* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

static char hex_nibble(uint8_t value)
{
    value &= 0x0f;
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('A' + value - 10);
}

static std::string hex_encode_ascii(const std::string& text)
{
    std::string out;
    out.reserve(text.size() * 2);
    for (unsigned char ch : text) {
        out += hex_nibble(ch >> 4);
        out += hex_nibble(ch);
    }
    return out;
}

static bool parse_http_url(const std::string& raw_url, std::string& protocol,
                           std::string& host, std::string& path, std::string& error)
{
    std::string url = trim(raw_url);
    if (url.empty()) url = IDF_KEEPALIVE_DEFAULT_URL;
    if (url.size() > 240) {
        error = "蜂窝HTTP URL过长";
        return false;
    }

    size_t proto_end = url.find("://");
    if (proto_end == std::string::npos || proto_end == 0) {
        error = "蜂窝HTTP URL格式无效，需要 http:// 或 https://";
        return false;
    }

    protocol = url.substr(0, proto_end);
    std::transform(protocol.begin(), protocol.end(), protocol.begin(),
                   [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
    if (protocol != "http" && protocol != "https") {
        error = "蜂窝HTTP URL仅支持 http/https";
        return false;
    }

    size_t host_start = proto_end + 3;
    size_t path_start = url.find('/', host_start);
    if (path_start == std::string::npos) {
        host = url.substr(host_start);
        path = "/";
    } else {
        host = url.substr(host_start, path_start - host_start);
        path = url.substr(path_start);
    }
    size_t hash = path.find('#');
    if (hash != std::string::npos) path.resize(hash);
    host = trim(host);
    if (host.empty() || host.find('"') != std::string::npos ||
        host.find(' ') != std::string::npos || path.find('"') != std::string::npos ||
        path.find(' ') != std::string::npos) {
        error = "蜂窝HTTP URL包含非法字符";
        return false;
    }
    return true;
}

static void normalize_keepalive_payload_size(const std::string& host, std::string& path)
{
    if (host != "gg.incrafttime.top") return;
    if (!starts_with(path, "/api/payload?")) return;
    size_t pos = path.find("size=128684");
    if (pos != std::string::npos) path.replace(pos, strlen("size=128684"), "size=64342");
}

static void append_no_cache_query(std::string& path)
{
    path += (path.find('?') == std::string::npos) ? '?' : '&';
    char buf[48];
    snprintf(buf, sizeof(buf), "t=%llu&r=%08x",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000ULL),
             static_cast<unsigned>(esp_random()));
    path += buf;
}

static void preserve_sms_urc_from_response(const std::string& response)
{
    if (response.find("+CMT:") != std::string::npos ||
        response.find("+CMTI:") != std::string::npos ||
        response.find("+CLIP:") != std::string::npos ||
        response.find("RING") != std::string::npos) {
        // RING/+CLIP 也要保留：否则整段响铃落在一次持锁 AT 交换内时来电通知丢失
        append_urc_text(response);
    }
}

static esp_err_t send_at_locked(const std::string& cmd, uint32_t timeout_ms,
                                std::string& response, size_t max_capture = 1400,
                                uint32_t extra_read_ms = 50)
{
    capture_pending_uart_locked(30);
    std::string wire = cmd;
    wire += "\r\n";
    uart_write_bytes(MODEM_UART, wire.data(), wire.size());

    response.clear();
    response.reserve(std::min<size_t>(max_capture, 512));
    TickDeadline deadline(timeout_ms);
    uint8_t buf[128];
    esp_err_t ret = ESP_ERR_TIMEOUT;
    while (!deadline.expired()) {
        int got = uart_read_bytes(MODEM_UART, buf, sizeof(buf), pdMS_TO_TICKS(80));
        if (got > 0) {
            size_t room = max_capture > response.size() ? max_capture - response.size() : 0;
            if (room > 0) response.append(reinterpret_cast<const char*>(buf), std::min<size_t>(room, got));
            int final_code = at_final_result(response);
            if (final_code != 0) {
                ret = final_code > 0 ? ESP_OK : ESP_FAIL;
                // 静默续期之上加总时长硬上限：URC 持续刷屏(如下载进度)时
                // 纯续期循环会无限占住 AT 通道锁，拖死所有 AT 使用方
                TickDeadline extra_hard(extra_read_ms * 4 + 200);
                TickDeadline extra_deadline(extra_read_ms);
                while (!extra_deadline.expired() && !extra_hard.expired()) {
                    int more = uart_read_bytes(MODEM_UART, buf, sizeof(buf), pdMS_TO_TICKS(15));
                    if (more <= 0) continue;
                    room = max_capture > response.size() ? max_capture - response.size() : 0;
                    if (room > 0) response.append(reinterpret_cast<const char*>(buf), std::min<size_t>(room, more));
                    extra_deadline.restart(extra_read_ms);
                }
                break;
            }
        }
    }
    preserve_sms_urc_from_response(response);
    return ret;
}

static int parse_mhttp_create_id(const std::string& resp)
{
    size_t p = resp.find("+MHTTPCREATE:");
    if (p == std::string::npos) return -1;
    p += strlen("+MHTTPCREATE:");
    size_t end = resp.find_first_of("\r\n", p);
    std::string token = resp.substr(p, end == std::string::npos ? std::string::npos : end - p);
    size_t comma = token.find(',');
    if (comma != std::string::npos) token.resize(comma);
    long id = -1;
    if (!parse_long_token(token, id) || id < 0 || id > 255) return -1;
    return static_cast<int>(id);
}

static void parse_mhttp_head(const std::string& head, int http_id, IdfCellularHttpResult& result,
                             bool& complete, bool& error)
{
    size_t comma = head.find(',');
    if (comma == std::string::npos) return;
    if (starts_with(head, "+MHTTPURC: \"header\"")) {
        long nums[4];
        int n = 0;
        if (parse_comma_longs(head.substr(comma + 1), nums, 4, n) && n >= 2 && nums[0] == http_id) {
            result.httpStatus = static_cast<int>(nums[1]);
            idf_logf("蜂窝HTTP响应状态: %d", result.httpStatus);
        }
    } else if (starts_with(head, "+MHTTPURC: \"content\"")) {
        long nums[5];
        int n = 0;
        if (parse_comma_longs(head.substr(comma + 1), nums, 5, n) && n >= 4 && nums[0] == http_id) {
            result.expectedBytes = static_cast<uint32_t>(std::max<long>(0, nums[1]));
            result.bytesRead = static_cast<uint32_t>(std::max<long>(0, nums[2]));
            uint32_t current = static_cast<uint32_t>(std::max<long>(0, nums[3]));
            if ((result.expectedBytes > 0 && result.bytesRead >= result.expectedBytes) || current == 0) {
                complete = true;
            }
        }
    } else if (starts_with(head, "+MHTTPURC: \"err\"")) {
        long nums[3];
        int n = 0;
        if (parse_comma_longs(head.substr(comma + 1), nums, 3, n) && n >= 2 && nums[0] == http_id) {
            result.mhttpError = static_cast<int>(nums[1]);
            idf_logf("蜂窝HTTP错误码: %d%s", result.mhttpError,
                     result.mhttpError == 4 ? "(SSL握手失败)" : "");
            error = true;
            complete = true;
        }
    }
}

static int comma_count(const std::string& text)
{
    int count = 0;
    for (char ch : text) {
        if (ch == ',') ++count;
    }
    return count;
}

static bool send_mhttp_header_locked(int http_id, bool more, const std::string& line)
{
    char head[64];
    snprintf(head, sizeof(head), "AT+MHTTPHEADER=%d,%d,%u,\"",
             http_id, more ? 1 : 0, static_cast<unsigned>(line.size()));
    std::string cmd = head;
    cmd += line;
    cmd += "\"";
    std::string resp;
    return send_at_locked(cmd, 3000, resp) == ESP_OK;
}

static void append_sms_urc_line(const std::string& line)
{
    std::string text = line;
    text += "\r\n";
    append_urc_text(text);
}

static bool wait_mhttp_download_locked(int http_id, uint32_t timeout_ms, IdfCellularHttpResult& result)
{
    TickDeadline deadline(timeout_ms);
    std::string head;
    head.reserve(280);
    bool skipping_data = false;
    bool append_next_sms_payload = false;
    bool complete = false;
    bool error = false;
    uint8_t buf[128];

    while (!deadline.expired() && !complete) {
        int got = uart_read_bytes(MODEM_UART, buf, sizeof(buf), pdMS_TO_TICKS(120));
        if (got <= 0) continue;
        for (int i = 0; i < got && !complete; ++i) {
            char ch = static_cast<char>(buf[i]);
            if (skipping_data) {
                if (ch == '\n') {
                    skipping_data = false;
                    head.clear();
                }
                continue;
            }
            if (ch == '\r' || ch == '\n') {
                std::string line = trim(head);
                if (!line.empty()) {
                    if (starts_with(line, "+MHTTPURC: \"err\"")) {
                        parse_mhttp_head(line, http_id, result, complete, error);
                    } else if (append_next_sms_payload || starts_with(line, "+CMT:") || starts_with(line, "+CMTI:")) {
                        append_sms_urc_line(line);
                        append_next_sms_payload = starts_with(line, "+CMT:");
                    } else if (line == "RING" || starts_with(line, "+CLIP:")) {
                        // 保号下载最长可占用 UART ~90s，整段响铃都可能落在窗口内；
                        // 不转发这两类行来电通知就静默丢了
                        append_sms_urc_line(line);
                    }
                }
                head.clear();
                continue;
            }
            if (head.size() < 620) head += ch;  // 需容纳下载中途插入的最大 PDU 行(~600 hex 字符)

            int need_commas = 0;
            if (starts_with(head, "+MHTTPURC: \"content\"")) need_commas = 5;
            else if (starts_with(head, "+MHTTPURC: \"header\"")) need_commas = 4;
            if (need_commas > 0 && comma_count(head) >= need_commas) {
                parse_mhttp_head(head, http_id, result, complete, error);
                skipping_data = true;
                head.clear();
            }
        }
    }

    if (!complete) idf_log_line("蜂窝HTTP下载等待超时");
    return !error && complete && result.httpStatus >= 200 && result.httpStatus < 400 &&
           result.bytesRead >= CELLULAR_KEEPALIVE_MIN_BYTES;
}

static bool valid_ipv4_address(const std::string& value)
{
    int parts = 0;
    size_t pos = 0;
    bool non_zero = false;
    while (pos <= value.size() && parts < 4) {
        size_t dot = value.find('.', pos);
        std::string part = value.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (part.empty() || part.size() > 3) return false;
        if (part.size() > 1 && part[0] == '0') return false;
        long octet = -1;
        if (!parse_long_token(part, octet) || octet < 0 || octet > 255) return false;
        if (octet != 0) non_zero = true;
        ++parts;
        if (dot == std::string::npos) break;
        if (parts >= 4) return false;
        pos = dot + 1;
    }
    return parts == 4 && non_zero;
}

static bool parse_cgpaddr_ip(const std::string& resp, std::string& ip)
{
    size_t p = resp.find("+CGPADDR:");
    if (p == std::string::npos) return false;
    size_t comma = resp.find(',', p);
    size_t eol = resp.find('\n', p);
    if (eol == std::string::npos) eol = resp.size();
    if (comma == std::string::npos || comma >= eol) return false;
    ip = trim(resp.substr(comma + 1, eol - comma - 1));
    ip.erase(std::remove(ip.begin(), ip.end(), '"'), ip.end());
    if (!valid_ipv4_address(ip)) return false;
    return true;
}

static bool apn_valid_for_at(const std::string& apn)
{
    return apn.size() <= 96 && apn.find('"') == std::string::npos &&
           apn.find('\r') == std::string::npos && apn.find('\n') == std::string::npos;
}

static bool sample_cell_ip_once(void)
{
    std::string resp;
    std::string ip;
    if (send_ok("AT+CGPADDR=1", 3000, &resp) && parse_cgpaddr_ip(resp, ip)) {
        set_status_cell_ip(ip);
        idf_logf("蜂窝PDP IP: %s", ip.c_str());
        return true;
    }
    set_status_cell_ip("");
    return false;
}

static bool wait_pdp_ready_locked(uint32_t timeout_ms, std::string& ip)
{
    TickDeadline deadline(timeout_ms);
    while (!deadline.expired()) {
        std::string resp;
        if (send_at_locked("AT+CGPADDR=1", 3000, resp) == ESP_OK && parse_cgpaddr_ip(resp, ip)) {
            IdfModemStatus patch;
            patch.cellIp = ip;
            update_status(patch);
            idf_logf("蜂窝PDP已就绪，IP: %s", ip.c_str());
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    idf_log_line("蜂窝PDP等待超时：未取得有效IP");
    return false;
}

static bool parse_muestats_cell(const std::string& resp, IdfModemStatus& patch)
{
    size_t line_pos = resp.find("\"scell\"");
    if (line_pos == std::string::npos) return false;
    size_t line_end = resp.find('\n', line_pos);
    if (line_end == std::string::npos) line_end = resp.size();
    std::string line = resp.substr(line_pos, line_end - line_pos);

    std::string parts[12];
    int count = 0;
    size_t pos = 0;
    while (pos <= line.size() && count < 12) {
        size_t comma = line.find(',', pos);
        if (comma == std::string::npos) comma = line.size();
        parts[count++] = trim(line.substr(pos, comma - pos));
        if (comma == line.size()) break;
        pos = comma + 1;
    }
    if (count <= 10) return false;

    bool got = false;
    if (!parts[7].empty()) {
        long value = 0;
        if (parse_long_token(parts[7], value) && value > -32768) {
            patch.rsrp = static_cast<int>(value / 10);
            got = true;
        }
    }
    if (!parts[8].empty()) {
        long value = 0;
        if (parse_long_token(parts[8], value) && value > -32768) {
            patch.rsrq = static_cast<int>(value / 10);
            got = true;
        }
    }
    if (!parts[10].empty()) {
        long value = 0;
        if (parse_long_token(parts[10], value) && value > -32768) {
            patch.sinr = static_cast<int>(value / 10);
            got = true;
        }
    }
    return got;
}

static bool parse_cesq_signal(const std::string& resp, IdfModemStatus& patch)
{
    size_t p = resp.find("+CESQ:");
    if (p == std::string::npos) return false;
    long values[6] = {};
    int count = 0;
    size_t value_start = p + strlen("+CESQ:");
    size_t line_end = resp.find_first_of("\r\n", value_start);
    std::string line = resp.substr(
        value_start, line_end == std::string::npos ? std::string::npos : line_end - value_start);
    if (!parse_comma_longs(line, values, 6, count) || count < 6) return false;
    bool got = false;
    if (values[4] >= 0 && values[4] <= 34) {
        patch.rsrq = static_cast<int>(values[4] / 2 - 20);
        got = true;
    }
    if (values[5] >= 0 && values[5] <= 97) {
        patch.rsrp = static_cast<int>(values[5] - 141);
        got = true;
    }
    return got;
}

static void sample_signal_detail_once(void)
{
    IdfModemStatus current = idf_modem_get_status();
    if (!current.modemReady) return;
    std::string resp;
    IdfModemStatus patch;
    bool got = false;
    if (send_ok("AT+MUESTATS=\"cell\"", 2000, &resp)) {
        got = parse_muestats_cell(resp, patch);
    }
    if (!got && send_ok("AT+CESQ", 2000, &resp)) {
        got = parse_cesq_signal(resp, patch);
    }
    if (!got) return;
    int next_rsrp = patch.rsrp != 999 ? patch.rsrp : current.rsrp;
    int next_rsrq = patch.rsrq != 999 ? patch.rsrq : current.rsrq;
    int next_sinr = patch.sinr != 999 ? patch.sinr : current.sinr;
    if (next_rsrp == 999 && next_rsrq == 999 && next_sinr == 999) return;

    update_status(patch, false, true);

    bool first = !s_logged_detail_valid;
    bool duplicate = s_logged_detail_valid &&
                     next_rsrp == s_logged_rsrp &&
                     next_rsrq == s_logged_rsrq &&
                     next_sinr == s_logged_sinr;
    bool changed = !duplicate &&
                   (first ||
                    (next_rsrp != 999 && (s_logged_rsrp == 999 || abs(next_rsrp - s_logged_rsrp) >= 6)) ||
                    (next_rsrq != 999 && (s_logged_rsrq == 999 || abs(next_rsrq - s_logged_rsrq) >= 4)) ||
                    (next_sinr != 999 && (s_logged_sinr == 999 || abs(next_sinr - s_logged_sinr) >= 6)));
    if (changed) {
        s_logged_detail_valid = true;
        s_logged_rsrp = next_rsrp;
        s_logged_rsrq = next_rsrq;
        s_logged_sinr = next_sinr;
    }
}

static bool model_skips_cgact(void)
{
    IdfModemStatus status = idf_modem_get_status();
    if (status.model == "ML307Y") return true;
    std::string resp;
    if (!send_ok("AT+CGMM", 1000, &resp)) return false;
    std::string model = first_payload_line(resp, "AT+CGMM");
    if (!model.empty()) {
        IdfModemStatus patch;
        patch.model = model;
        update_status(patch);
    }
    return model == "ML307Y";
}

static bool apply_configured_data_mode_once(const IdfSimSettingsView& cfg, uint32_t active_timeout_ms,
                                            uint32_t inactive_timeout_ms)
{
    std::string resp;
    std::string apn = trim(cfg.apn);
    // 数据漫游策略：未勾选"允许数据漫游"且当前处于漫游(CEREG=5)时不激活蜂窝数据。
    // 启动阶段注册状态未知(stat=-1)会乐观激活，注册完成后由 enforce_roaming_data_policy 兜底关闭。
    bool want_data = cfg.dataEnabled &&
                     (cfg.roamingEnabled || idf_modem_get_status().ceregStat != 5);
    if (want_data) {
        if (!apn.empty() && apn_valid_for_at(apn)) {
            std::string cmd = "AT+CGDCONT=1,\"IP\",\"";
            cmd += apn;
            cmd += "\"";
            send_ok(cmd.c_str(), 3000, &resp);
        } else if (!apn.empty()) {
            idf_log_line("APN 包含非法字符，启动时未下发 CGDCONT");
        }
        bool ok = send_ok("AT+CGACT=1,1", active_timeout_ms, &resp);
        if (ok) sample_cell_ip_once();
        return ok;
    }

    bool ok = send_ok("AT+CGACT=0,1", inactive_timeout_ms, &resp);
    if (ok) set_status_cell_ip("");
    return ok;
}

// 数据漫游策略兜底：未勾选"允许数据漫游"且当前漫游(stat=5)时确保蜂窝数据关闭。
// 启动阶段拿不到注册状态会先乐观激活，注册完成后在此关闭，避免漫游误跑流量。
// 此开关只控制数据 PDP；短信是否可用由 SIM、模组和运营商短信承载共同决定，
// 不能仅凭 CEREG/CREG 中的某一个状态判断。归属网络(stat=1)不干预，按常规激活。
static void enforce_roaming_data_policy(const IdfSimSettingsView& cfg, int stat)
{
    if (!cfg.dataEnabled || cfg.roamingEnabled) return;  // 未开数据或允许漫游数据：无需干预
    if (stat != 5) return;                                // 非漫游：归属网络按常规激活即可
    if (idf_modem_get_status().cellIp.empty()) return;    // 数据本就未激活，无需再关
    std::string resp;
    if (send_ok("AT+CGACT=0,1", 3000, &resp)) {
        set_status_cell_ip("");
        idf_log_line("数据漫游已关闭：检测到漫游，已停用蜂窝数据(不跑流量)");
    }
}

static void schedule_data_mode_retry(void)
{
    s_data_mode_retry_pending = true;
    s_data_mode_retry_count = 0;
    s_next_data_mode_retry = xTaskGetTickCount() + pdMS_TO_TICKS(MODEM_DATA_MODE_RETRY_GAP_MS);
}

static void apply_startup_data_mode(void)
{
    if (model_skips_cgact()) {
        idf_log_line("该型号跳过启动 CGACT 配置");
        return;
    }
    IdfSimSettingsView cfg = idf_config_get_sim_settings_view();
    bool ok = apply_configured_data_mode_once(cfg, 6000, 2500);
    if (ok) {
        idf_log_line(cfg.dataEnabled ? "已按配置启用蜂窝数据(AT+CGACT=1,1)"
                                     : "已禁用数据连接(AT+CGACT=0,1)，防止流量消耗");
    } else {
        idf_log_line(cfg.dataEnabled ? "启动时激活数据连接未成功，转入后台重试"
                                     : "启动时禁用数据连接未确认，转入后台重试");
        schedule_data_mode_retry();
    }
}

static bool plmn_valid(const std::string& plmn)
{
    if (plmn.empty()) return true;
    if (plmn.size() < 5 || plmn.size() > 6) return false;
    for (char ch : plmn) {
        if (!isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

static void apply_operator_if_configured(const IdfSimSettingsView& cfg)
{
    if (cfg.operatorPlmn.empty()) return;
    if (!plmn_valid(cfg.operatorPlmn)) {
        idf_log_line("运营商 PLMN 非法，启动时未下发 COPS");
        return;
    }
    std::string cmd = "AT+COPS=1,2,\"";
    cmd += cfg.operatorPlmn;
    cmd += "\"";
    std::string resp;
    esp_err_t err = idf_modem_send_at(cmd, 30000, resp);
    idf_logf("运营商: 锁定 PLMN %s %s", cfg.operatorPlmn.c_str(),
             err == ESP_OK ? "成功" : "失败(可能不可达)");
}

static bool process_data_mode_retry(void)
{
    if (!s_data_mode_retry_pending) return false;
    if (static_cast<int32_t>(xTaskGetTickCount() - s_next_data_mode_retry) < 0) return false;  // 回绕安全
    if (!at_channel_idle_now()) return false;

    ++s_data_mode_retry_count;
    IdfSimSettingsView cfg = idf_config_get_sim_settings_view();
    bool ok = apply_configured_data_mode_once(cfg, 8000, 3000);
    if (ok) {
        s_data_mode_retry_pending = false;
        idf_log_line(cfg.dataEnabled ? "后台重试：蜂窝数据已启用" : "后台重试：蜂窝数据已禁用");
    } else if (s_data_mode_retry_count >= MODEM_DATA_MODE_RETRY_MAX) {
        s_data_mode_retry_pending = false;
        idf_log_line("后台重试 CGACT 仍失败，保留当前模组状态");
    } else {
        s_next_data_mode_retry = xTaskGetTickCount() + pdMS_TO_TICKS(MODEM_DATA_MODE_RETRY_GAP_MS);
        idf_log_line("后台重试 CGACT 未成功，稍后再试");
    }
    return true;
}

static bool fetch_mhttp_once_locked(const std::string& protocol, const std::string& host,
                                    const std::string& path, IdfCellularHttpResult& result)
{
    for (int i = 0; i < 4; ++i) {
        std::string ignored;
        char cmd[24];
        snprintf(cmd, sizeof(cmd), "AT+MHTTPDEL=%d", i);
        send_at_locked(cmd, 1000, ignored, 256, 10);
    }

    std::string create_cmd = "AT+MHTTPCREATE=\"";
    create_cmd += protocol;
    create_cmd += "://";
    create_cmd += host;
    create_cmd += "\"";
    std::string resp;
    if (send_at_locked(create_cmd, 10000, resp, 1600, 1200) != ESP_OK) {
        result.message = "蜂窝HTTP创建失败";
        idf_logf("蜂窝HTTP创建失败: %s", resp.c_str());
        return false;
    }

    int http_id = parse_mhttp_create_id(resp);
    if (http_id < 0) {
        result.message = "蜂窝HTTP创建失败：未返回连接ID";
        idf_logf("蜂窝HTTP创建失败: %s", resp.c_str());
        return false;
    }

    char cmd[128];
    if (protocol == "https") {
        snprintf(cmd, sizeof(cmd), "AT+MHTTPCFG=\"ssl\",%d,1,0", http_id);
        send_at_locked(cmd, 5000, resp);
    }
    snprintf(cmd, sizeof(cmd), "AT+MHTTPCFG=\"encoding\",%d,0,0", http_id);
    send_at_locked(cmd, 3000, resp);
    send_mhttp_header_locked(http_id, true, "Cache-Control: no-cache, no-store, must-revalidate");
    send_mhttp_header_locked(http_id, false, "Pragma: no-cache");
    snprintf(cmd, sizeof(cmd), "AT+MHTTPCFG=\"encoding\",%d,1,1", http_id);
    send_at_locked(cmd, 3000, resp);

    std::string request_cmd = "AT+MHTTPREQUEST=";
    request_cmd += std::to_string(http_id);
    request_cmd += ",1,0,";
    request_cmd += hex_encode_ascii(path);
    if (send_at_locked(request_cmd, 10000, resp) != ESP_OK) {
        result.message = "蜂窝HTTP请求发送失败";
        idf_logf("蜂窝HTTP请求发送失败: %s", resp.c_str());
        snprintf(cmd, sizeof(cmd), "AT+MHTTPDEL=%d", http_id);
        send_at_locked(cmd, 2000, resp, 256, 20);
        return false;
    }

    bool ok = wait_mhttp_download_locked(http_id, CELLULAR_HTTP_TIMEOUT_MS, result);
    snprintf(cmd, sizeof(cmd), "AT+MHTTPDEL=%d", http_id);
    send_at_locked(cmd, 3000, resp, 256, 20);
    if (ok) {
        result.message = "蜂窝HTTP payload 下载完成";
        idf_logf("蜂窝HTTP保号完成: HTTP %d, 已下载约 %uKB",
                 result.httpStatus, static_cast<unsigned>(result.bytesRead / 1024UL));
    } else {
        if (result.message.empty()) result.message = "蜂窝HTTP payload 下载失败";
        idf_logf("蜂窝HTTP保号失败: HTTP %d, 已下载约 %uKB/期望%uKB",
                 result.httpStatus,
                 static_cast<unsigned>(result.bytesRead / 1024UL),
                 static_cast<unsigned>(result.expectedBytes / 1024UL));
    }
    result.ok = ok;
    return ok;
}

esp_err_t idf_modem_cellular_http_get(const std::string& url, const IdfCellularHttpConfig& config,
                                      IdfCellularHttpResult& result)
{
    result = IdfCellularHttpResult();
    if (!s_started) {
        result.message = "模组尚未启动";
        return ESP_ERR_INVALID_STATE;
    }

    std::string protocol;
    std::string host;
    std::string path;
    if (!parse_http_url(url, protocol, host, path, result.message)) {
        return ESP_ERR_INVALID_ARG;
    }
    normalize_keepalive_payload_size(host, path);
    append_no_cache_query(path);

    if (xSemaphoreTakeRecursive(s_at_mutex, pdMS_TO_TICKS(CELLULAR_HTTP_TIMEOUT_MS + 45000UL)) != pdTRUE) {
        result.message = "模组串口忙，蜂窝HTTP未执行";
        return ESP_ERR_TIMEOUT;
    }

    idf_logf("准备通过蜂窝HTTP下载payload: %s://%s%s",
             protocol.c_str(), host.c_str(), path.c_str());

    std::string resp;
    std::string apn = trim(config.apn);
    if (!apn.empty() && apn.find('"') == std::string::npos) {
        std::string cmd = "AT+CGDCONT=1,\"IP\",\"";
        cmd += apn;
        cmd += "\"";
        send_at_locked(cmd, 3000, resp);
    }

    idf_log_line("激活数据连接(CGACT)...");
    esp_err_t activate_err = send_at_locked("AT+CGACT=1,1", 10000, resp);
    if (activate_err != ESP_OK) {
        idf_logf("CGACT激活未返回OK，继续等待PDP: %s", resp.c_str());
    }

    std::string ip;
    bool pdp_ready = wait_pdp_ready_locked(CELLULAR_PDP_READY_TIMEOUT_MS, ip);
    if (!pdp_ready) {
        set_status_cell_ip("");
        if (!config.dataEnabled) {
            idf_log_line("关闭PDP上下文(CGACT=0)...");
            send_at_locked("AT+CGACT=0,1", 5000, resp);
        }
        xSemaphoreGiveRecursive(s_at_mutex);
        result.message = "蜂窝PDP未取得有效IP，请查看日志";
        return ESP_FAIL;
    }
    result.cellIp = ip;

    bool ok = fetch_mhttp_once_locked(protocol, host, path, result);
    if (!ok && protocol == "https" && result.mhttpError == 4) {
        idf_log_line("HTTPS握手失败，改用HTTP重试一次；若返回301，请关闭强制HTTPS跳转");
        IdfCellularHttpResult retry;
        retry.cellIp = result.cellIp;
        ok = fetch_mhttp_once_locked("http", host, path, retry);
        result = retry;
    }

    if (!config.dataEnabled) {
        idf_log_line("关闭PDP上下文(CGACT=0)...");
        send_at_locked("AT+CGACT=0,1", 5000, resp);
        set_status_cell_ip("");
    }

    if (result.message.empty()) {
        result.message = ok ? "蜂窝HTTP payload 下载完成" : "蜂窝HTTP payload 下载失败，请查看日志";
    }
    result.ok = ok;
    xSemaphoreGiveRecursive(s_at_mutex);
    return ok ? ESP_OK : ESP_FAIL;
}

static void sample_signal_once(void)
{
    std::string resp;
    if (!send_ok("AT+CSQ", 1000, &resp)) return;
    int csq = -1;
    int ber = 99;
    if (!parse_csq(resp, csq, ber)) return;
    IdfModemStatus patch;
    patch.csq = csq;
    patch.ber = ber;
    update_status(patch, false, true);
    bool first = s_logged_csq < 0;
    bool changed = first || abs(csq - s_logged_csq) >= 3 || ber != s_logged_ber ||
                   csq == 99 || s_logged_csq == 99;
    if (changed) {
        s_logged_csq = csq;
        s_logged_ber = ber;
        char ber_buf[16];
        const char* ber_text = format_ber(ber, ber_buf, sizeof(ber_buf));
        ESP_LOGD(TAG, "%s CSQ=%d/31 BER=%s", first ? "signal" : "signal changed", csq, ber_text);
    }
}

static bool sample_identity_once(bool log_summary = false, bool include_network_fields = true)
{
    IdfModemStatus before = idf_modem_get_status();
    bool need_static = !s_identity_static_attempted ||
                       before.mfr.empty() ||
                       before.model.empty() ||
                       before.fwver.empty();
    std::string resp;
    IdfModemStatus patch;

    // 固件/厂家信息基本不变；启动采样未完整前允许补采，完整后不再反复查询。
    if (need_static && before.mfr.empty()) {
        if (send_ok("AT+CGMI", 1000, &resp)) patch.mfr = first_payload_line(resp, "AT+CGMI");
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (need_static && before.model.empty()) {
        if (send_ok("AT+CGMM", 1000, &resp)) patch.model = first_payload_line(resp, "AT+CGMM");
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (need_static && before.fwver.empty()) {
        if (send_ok("AT+CGMR", 1000, &resp)) patch.fwver = first_payload_line(resp, "AT+CGMR");
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (before.imei.size() < 14) {
        const char* imei_cmds[] = {"AT+CGSN=1", "AT+GSN=1", "AT+CGSN", "AT+GSN"};
        for (const char* cmd : imei_cmds) {
            if (!patch.imei.empty()) break;
            if (send_ok(cmd, 1000, &resp)) {
                patch.imei = first_digits_line(resp, 14, 17);
                if (patch.imei.empty()) patch.imei = first_digit_run(resp, 14, 17);
            }
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    auto parse_iccid_response = [](const std::string& raw) {
        std::string line = first_payload_line(raw);
        size_t p = line.find(':');
        std::string value = trim(p == std::string::npos ? line : line.substr(p + 1));
        value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
        for (char& ch : value) {
            if (ch == 'f') ch = 'F';
        }
        return is_iccid_text(value) ? value : std::string();
    };
    if (before.iccid.size() < 15) {
        const char* iccid_cmds[] = {"AT+MCCID", "AT+ICCID", "AT+CCID"};
        for (const char* cmd : iccid_cmds) {
            if (!patch.iccid.empty()) break;
            if (send_ok(cmd, 1500, &resp)) patch.iccid = parse_iccid_response(resp);
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (before.imsi.empty()) {
        if (send_ok("AT+CIMI", 1000, &resp)) {
            patch.imsi = first_digits_line(resp, 14, 16);
            if (patch.imsi.empty()) patch.imsi = first_digit_run(resp, 14, 16);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    // 运营商/APN 是启动展示字段，注册完成后至少要拿到运营商；本机号码很多 SIM 不返回，不阻塞启动。
    bool need_network = include_network_fields &&
                        (!s_identity_network_attempted || before.operatorName.empty());
    if (need_network) {
        if (before.operatorName.empty()) {
            // 先选长名称格式：自动模式下不设格式时 COPS? 只回模式位(+COPS: 0)，读不到运营商名
            send_ok("AT+COPS=3,0", 1500, &resp);
            if (send_ok("AT+COPS?", 1500, &resp)) patch.operatorName = parse_cops(resp);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        if (before.apnSim.empty()) {
            if (send_ok("AT+CGDCONT?", 1500, &resp)) patch.apnSim = parse_apn(resp);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        if (before.phone.empty()) {
            if (send_ok("AT+CNUM", 1500, &resp)) patch.phone = parse_cnum_phone(resp);
        }
    }

    bool static_changed = (!patch.mfr.empty() && patch.mfr != before.mfr) ||
                          (!patch.model.empty() && patch.model != before.model) ||
                          (!patch.fwver.empty() && patch.fwver != before.fwver) ||
                          (!patch.imei.empty() && patch.imei != before.imei) ||
                          (!patch.iccid.empty() && patch.iccid != before.iccid) ||
                          (!patch.imsi.empty() && patch.imsi != before.imsi);
    bool network_changed = (!patch.operatorName.empty() && patch.operatorName != before.operatorName) ||
                           (!patch.apnSim.empty() && patch.apnSim != before.apnSim) ||
                           (!patch.phone.empty() && patch.phone != before.phone);
    bool material_static_change = (!patch.imei.empty() && !before.imei.empty() && patch.imei != before.imei) ||
                                  (!patch.iccid.empty() && !before.iccid.empty() && patch.iccid != before.iccid) ||
                                  (!patch.imsi.empty() && !before.imsi.empty() && patch.imsi != before.imsi);
    bool changed = static_changed || network_changed;
    update_status(patch, true, false);
    IdfModemStatus after = idf_modem_get_status();
    s_identity_static_attempted = !after.mfr.empty() && !after.model.empty() && !after.fwver.empty();
    if (include_network_fields) {
        s_identity_network_attempted = !after.operatorName.empty();
    }
    if (material_static_change) {
        ESP_LOGI(TAG, "identity changed imei=%s iccid=%s imsi=%s",
                 after.imei.empty() ? "-" : after.imei.c_str(),
                 after.iccid.empty() ? "-" : after.iccid.c_str(),
                 after.imsi.empty() ? "-" : after.imsi.c_str());
        idf_logf("模组身份变化 IMEI=%s ICCID=%s IMSI=%s",
                 after.imei.empty() ? "-" : after.imei.c_str(),
                 after.iccid.empty() ? "-" : after.iccid.c_str(),
                 after.imsi.empty() ? "-" : after.imsi.c_str());
    }
    save_identity_cache(patch.imei, patch.iccid);
    return changed;
}

static void modem_en_gpio_init(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << MODEM_EN;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
}

// 只拉高 EN 保持/接通供电，不做断电周期(热启动探测用)。
// 先写输出寄存器再配方向：gpio_config 切到输出的瞬间即输出高，
// 避免默认输出 0 造成 EN 瞬时拉低、给在运行的模组一个断电毛刺
static void modem_power_hold_on(void)
{
    gpio_set_level(MODEM_EN, 1);
    modem_en_gpio_init();
    gpio_set_level(MODEM_EN, 1);
}

static void modem_power_cycle(void)
{
    modem_en_gpio_init();

    set_phase("powering");
    gpio_set_level(MODEM_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(MODEM_POWERDOWN_MS));
    gpio_set_level(MODEM_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(MODEM_POWERUP_MIN_MS));
}

static bool wait_at_ready(void)
{
    TickDeadline deadline(MODEM_POWERUP_MAX_MS);
    while (!deadline.expired()) {
        if (send_ok("AT", 700)) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return false;
}

static bool modem_hot_start_allowed(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_CPU_LOCKUP:
            return true;
        default:
            return false;
    }
}

static bool modem_quick_start_allowed(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:
        case ESP_RST_SW:
        case ESP_RST_EXT:
            return true;
        default:
            return false;
    }
}

// 解析 AT+CPMS 设置命令的响应 "+CPMS: <used1>,<total1>,<used2>,<total2>,<used3>,<total3>"，
// 输出 <mem1> 总容量。查询响应(+CPMS: "SM",0,0,...)带引号存储名会解析失败，恰好只匹配设置响应。
static bool parse_cpms_total(const std::string& resp, long& total)
{
    size_t p = resp.find("+CPMS:");
    if (p == std::string::npos) return false;
    std::string line = line_containing(resp, p);
    const char* token = strstr(line.c_str(), "+CPMS:");
    if (!token) return false;
    long values[6] = {};
    int count = 0;
    if (!parse_comma_longs(token + strlen("+CPMS:"), values, 6, count) || count < 2) return false;
    total = values[1];
    return true;
}

static int sms_storage_code(const char* name)
{
    if (strcmp(name, "MT") == 0) return 0;
    if (strcmp(name, "ME") == 0) return 1;
    if (strcmp(name, "SM") == 0) return 2;
    return -1;
}

static void log_sms_storage_if_changed(const char* name)
{
    int current = sms_storage_code(name);
    int previous = s_logged_sms_storage_code.exchange(current, std::memory_order_relaxed);
    if (previous == current) return;
    // 首次 MT 是常规路径不提示；非 MT 或后续类型变化才记录，避免周期性 CPMS 重申刷屏。
    if (previous != -1 || current != 0) idf_logf("短信存储使用 %s", name);
}

// 按优先级选择短信存储：MT → ME → SM。部分可写 eSIM 的 SM 存储返回 OK 但容量为
// 0,0(issue #3)，此时短信实际无处可存，必须视为不可用并继续尝试下一候选；
// 响应里解析不出容量的按可用处理(不同固件设置命令可能只回 OK)。
static bool select_sms_storage(void)
{
    static const struct { const char* cmd; const char* name; } kCandidates[] = {
        {"AT+CPMS=\"MT\",\"MT\",\"MT\"", "MT"},
        {"AT+CPMS=\"ME\",\"ME\",\"ME\"", "ME"},
        {"AT+CPMS=\"SM\",\"SM\",\"SM\"", "SM"},
    };
    for (const auto& c : kCandidates) {
        std::string resp;
        if (!send_ok(c.cmd, 1500, &resp)) continue;
        long total = -1;
        if (parse_cpms_total(resp, total) && total <= 0) {
            idf_logf("短信存储 %s 容量为 0，尝试下一候选", c.name);
            continue;
        }
        log_sms_storage_if_changed(c.name);
        return true;
    }
    idf_log_line("警告: 无可用短信存储(MT/ME/SM 均不可用)，短信接收可能失败");
    return false;
}

// 存储选择失败待补跑标志：冷启动时 CPMS 早于 SIM 就绪执行，SIM 初始化慢的开机
// 可能三个候选全失败(SIM busy)。注册成功意味着 SIM 必已就绪，届时补跑一次。
// 只在 modem_task 上下文读写，无需原子量。
static bool s_sms_storage_pending = false;

static void retry_sms_storage_if_pending(void)
{
    if (!s_sms_storage_pending) return;
    idf_log_line("SIM 已就绪，补跑短信存储选择");
    s_sms_storage_pending = !select_sms_storage();
}

void idf_modem_reassert_sms_storage(void)
{
    // 供短信任务周期性重申：模组自发复位(未被 ESP 侧察觉)后 CPMS 会回落到固件
    // 默认存储(常为 SM)；SM 容量为 0 的 eSIM 上短信从此无处可存，接收静默死亡，
    // 而 AT 探测/发送一切正常——"先能收后失效"的典型固件侧成因。
    // select_sms_storage 内部走加锁的 AT 通道，跨任务调用安全。
    select_sms_storage();
}

static bool configure_sms_and_registration(void)
{
    send_ok("ATE0", 1000);
    send_ok("AT+CMEE=1", 1200);  // 明确返回 +CMS/+CME 数字错误，避免只有笼统 ERROR
    bool pdu_mode_ok = send_ok("AT+CMGF=0", 1200);
    // 统一收/存/读的短信存储位置：CNMI mt=1 投递到 <mem3>，CMGL/CMGR 读 <mem1>，
    // 两者不一致时 +CMTI 索引和补收轮询会看不同的存储，短信被静默丢失
    bool storage_ok = select_sms_storage();
    s_sms_storage_pending = !storage_ok;
    bool cnmi_ok = send_ok("AT+CNMI=2,1,0,0,0", 1200);
    send_ok("AT+CEREG=2", 1200);
    // 开启主叫号码上报：来电时模组主动上报 RING + +CLIP: "号码",...，供来电通知使用。
    // 无语音能力的卡/模组下该指令可能 ERROR，忽略即可(收不到来电就不会有 URC)。
    send_ok("AT+CLIP=1", 1200);
    // NET 指示灯开关(ML307R: AT+MLED=0,<0/1>)：每次初始化按保存的配置下发，
    // 覆盖模组记住的上次状态
    send_ok(idf_config_net_led_enabled() ? "AT+MLED=0,1" : "AT+MLED=0,0", 1200);
    bool sms_ready = pdu_mode_ok && storage_ok && cnmi_ok;
    if (!sms_ready) idf_log_line("短信收发配置未完整生效，将在后续健康检查中重试");
    return sms_ready;
}

static bool response_has_compact(std::string response, const char* expected)
{
    response.erase(std::remove_if(response.begin(), response.end(), [](unsigned char ch) {
        return isspace(ch);
    }), response.end());
    return response.find(expected) != std::string::npos;
}

static void query_sms_receive_config(bool& pdu, bool& cnmi)
{
    std::string resp;
    pdu = send_ok("AT+CMGF?", 1200, &resp) && response_has_compact(resp, "+CMGF:0");
    cnmi = send_ok("AT+CNMI?", 1200, &resp) &&
           response_has_compact(resp, "+CNMI:2,1,0,0,0");
}

bool idf_modem_sms_health_check(std::string& summary)
{
    summary.clear();
    if (!idf_modem_get_status().atReady) {
        idf_modem_request_reset(true);
        summary = "模组 AT 未就绪，已请求模组硬重启";
        idf_logf("每日短信体检异常: %s", summary.c_str());
        return false;
    }

    std::string resp;
    int stat = -1;
    bool registered = send_ok("AT+CEREG?", 1200, &resp) && parse_cereg(resp, stat) &&
                      (stat == 1 || stat == 5);
    bool pdu = false;
    bool cnmi = false;
    query_sms_receive_config(pdu, cnmi);
    bool storage = select_sms_storage();
    bool initially_ok = registered && pdu && cnmi && storage;

    if (!pdu || !cnmi || !storage) {
        configure_sms_and_registration();
        stat = -1;
        registered = send_ok("AT+CEREG?", 1200, &resp) && parse_cereg(resp, stat) &&
                     (stat == 1 || stat == 5);
        query_sms_receive_config(pdu, cnmi);
        storage = select_sms_storage();
    }

    bool final_ok = registered && pdu && cnmi && storage;
    char state[160];
    snprintf(state, sizeof(state), "注册=%s，PDU=%s，CNMI=%s，存储=%s（不含运营商端到端投递）",
             registered ? "正常" : "异常", pdu ? "正常" : "异常", cnmi ? "正常" : "异常",
             storage ? "正常" : "异常");

    if (initially_ok) {
        summary = std::string("正常：") + state;
        idf_logf("每日短信体检通过: %s", summary.c_str());
        return true;
    }
    if (final_ok) {
        summary = std::string("发现异常，已修复：") + state;
    } else {
        idf_modem_request_reset(true);
        summary = std::string("异常未恢复，已请求模组硬重启：") + state;
    }
    idf_logf("每日短信体检异常: %s", summary.c_str());
    return false;
}

// 查询 SIM 是否就绪：AT+CPIN? 返回 +CPIN: READY 视为有卡可用；无卡/卡故障时模组回
// +CME ERROR(10=无卡,13=卡故障)，send_ok 返回 false。用于运行中热插拔检测。
static bool query_sim_ready(void)
{
    std::string resp;
    if (!send_ok("AT+CPIN?", 1500, &resp)) return false;
    return resp.find("+CPIN: READY") != std::string::npos ||
           resp.find("+CPIN:READY") != std::string::npos;
}

// 重启后 AT 握手失败时置位：一旦后续任何探测发现 AT 恢复，立即补跑完整初始化
// (ATE0/CMGF/CNMI/CEREG/CGACT)。否则模组以默认配置运行——回显开着、URC 不上报、
// 数据连接按模组默认自动激活(产生流量费，恰是本项目要防止的)。
static bool s_reinit_pending = false;

static bool handle_reset_request_if_any(void)
{
    // 逻辑通道尚未关闭时重启会把 eSIM 操作截断；REFRESH 后上层主动请求的重启仍允许执行。
    if (s_esim_operation_depth.load(std::memory_order_relaxed) != 0) return false;
    int request = s_reset_request.exchange(0, std::memory_order_relaxed);
    if (request == 0) return false;

    IdfModemStatus patch;
    patch.phase = "powering";
    patch.atReady = false;
    patch.modemReady = false;
    update_status(patch);
    reset_identity_sampling_state();
    if (request == 2) {
        idf_log_line("执行模组硬重启");
        modem_power_cycle();
    } else {
        idf_log_line("执行模组软重启");
        send_ok("AT+CFUN=1,1", 15000);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    if (!wait_at_ready()) {
        set_phase("failed");
        idf_log_line("模组重启后 AT 握手失败，等待恢复后补跑初始化");
        s_reinit_pending = true;
        return true;
    }
    patch = {};
    patch.started = true;
    patch.atReady = true;
    patch.modemReady = false;
    patch.phase = "at_ready";
    update_status(patch);
    configure_sms_and_registration();
    set_phase("registering");
    apply_startup_data_mode();
    s_reinit_pending = false;
    return true;
}

// AT 恢复后的补初始化（配合 s_reinit_pending）
static void run_pending_reinit_if_recovered(void)
{
    if (!s_reinit_pending) return;
    if (!at_channel_idle_now()) return;
    if (!send_ok("AT", 700)) return;
    idf_log_line("模组 AT 已恢复，补跑短信/注册/数据配置");
    IdfModemStatus patch;
    patch.started = true;
    patch.atReady = true;
    patch.modemReady = false;
    patch.phase = "at_ready";
    update_status(patch);
    configure_sms_and_registration();
    set_phase("registering");
    apply_startup_data_mode();
    s_reinit_pending = false;
}

static void modem_task(void*)
{
    IdfModemStatus patch;
    patch.started = true;
    patch.phase = "powering";
    update_status(patch);
    reset_identity_sampling_state();

    bool at_ready = false;
    esp_reset_reason_t reset_reason = esp_reset_reason();
    // 热启动快路径只留给崩溃/看门狗等意外复位；正常上电/软件重启先快速拉高 EN 探测，
    // 能直接 AT 就省掉强制断电周期。USB/串口复位仍冷启动，避免烧录后沿用半初始化状态。
    if (modem_hot_start_allowed(reset_reason)) {
        modem_power_hold_on();
        if (wait_at_ready()) {
            at_ready = true;
            idf_log_line("模组已在运行，跳过断电上电(热启动)");
        } else {
            idf_log_line("意外复位后热启动探测失败，改为模组冷启动");
        }
    } else if (modem_quick_start_allowed(reset_reason)) {
        modem_power_hold_on();
        if (wait_at_ready()) {
            at_ready = true;
            idf_logf("复位原因 %d，模组快速上电完成", static_cast<int>(reset_reason));
        } else {
            idf_logf("复位原因 %d，模组快速上电超时，改为冷启动", static_cast<int>(reset_reason));
        }
    } else {
        idf_logf("复位原因 %d，模组执行冷启动", static_cast<int>(reset_reason));
    }

    if (!at_ready) {
        // 启动握手：失败绝不放弃(Arduino 版靠 modemHealthTick 无限恢复)。任务一旦退出，
        // 网页重启模组、URC 轮询、健康探测全部失效，设备只能整机断电才能恢复。
        modem_power_cycle();
        int round = 0;
        uint32_t retry_gap_ms = 5000;
        while (!wait_at_ready()) {
            set_phase("failed");
            ++round;
            ESP_LOGE(TAG, "AT 握手超时(第%d轮)", round);
            idf_logf("模组 AT 握手超时(第%d轮)，稍后重新上电重试", round);
            s_reset_request.store(0, std::memory_order_relaxed);  // 重启请求由本轮上电一并满足
            vTaskDelay(pdMS_TO_TICKS(retry_gap_ms));
            if (retry_gap_ms < 60000) retry_gap_ms *= 2;  // 5s→10s→…→60s 封顶，避免热循环
            modem_power_cycle();
        }
    }

    patch = {};
    patch.started = true;
    patch.atReady = true;
    patch.phase = "at_ready";
    update_status(patch);
    ESP_LOGI(TAG, "AT 已就绪");
    idf_log_line("模组 AT 已就绪");

    configure_sms_and_registration();

    set_phase("registering");
    apply_startup_data_mode();

    int check_count = 0;
    int stat = -1;
    while (check_count++ < 30) {
        std::string resp;
        if (send_ok("AT+CEREG?", 1200, &resp) && parse_cereg(resp, stat)) {
            IdfModemStatus reg_patch;
            reg_patch.ceregStat = stat;
            reg_patch.phase = (stat == 1 || stat == 5) ? "sampling" : "registering";
            reg_patch.modemReady = (stat == 1 || stat == 5);
            update_status(reg_patch);
            if (reg_patch.modemReady) break;
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    bool registered = (stat == 1 || stat == 5);
    bool post_register_done = false;
    if (!registered) {
        set_phase("failed");
    } else {
        retry_sms_storage_if_pending();
        IdfSimSettingsView cfg = idf_config_get_sim_settings_view();
        apply_operator_if_configured(cfg);
        if (cfg.dataEnabled) sample_cell_ip_once();
        enforce_roaming_data_policy(cfg, stat);
        // 注册成功后立即做一轮首页基础信息采样；Web/WiFi 已先启动，不会阻塞页面打开。
        sample_signal_once();
        sample_signal_detail_once();
        sample_identity_once(false, true);
        post_register_done = startup_info_complete();
        set_phase(post_register_done ? "ready" : "sampling");
    }

    TickType_t last_signal = 0;
    TickType_t last_identity = 0;
    TickType_t last_detail = 0;
    TickType_t last_health = 0;
    int health_fail_count = 0;
    int dereg_count = 0;
    TickType_t last_sim_check = 0;
    int64_t sim_check_not_before_us = 0;  // 模组重启后给 SIM/CPIN 充分上电时间，避免误判二次拔插
    int sim_present = -1;  // -1=未知(仅记基线) 0=无卡 1=有卡
    bool sms_reconfigure_pending = false;  // 换卡/掉网恢复后在注册成功点再次重申短信栈
    while (true) {
        bool reset_handled = handle_reset_request_if_any();
        run_pending_reinit_if_recovered();
        TickType_t now = xTaskGetTickCount();
        if (reset_handled) {
            // 运行中重启不能沿用重启前的局部注册状态；否则 status 已进入 registering，
            // 但本任务仍以 registered=true 按 60 秒慢周期探测，换卡后恢复会被无谓拖延。
            registered = false;
            post_register_done = false;
            health_fail_count = 0;
            dereg_count = 0;
            last_health = 0;
            last_sim_check = now;  // 给 SIM 上电初始化留出一个完整检测周期
            sim_check_not_before_us = esp_timer_get_time() + 30LL * 1000LL * 1000LL;
            sim_present = -1;
            sms_reconfigure_pending = true;
        }
        if (process_data_mode_retry()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        // 用户手动刷新会绕过常规间隔并尽快跑一轮；若 AT 正忙，请求保留到下轮空闲时执行。
        bool force_sample = s_status_sample_requests.load(std::memory_order_relaxed) > 0;
        bool web_active = force_sample ||
                          (esp_timer_get_time() -
                           s_last_web_poll_us.load(std::memory_order_relaxed)) < WEB_POLL_ACTIVE_WINDOW_US;
        bool startup_sampling = registered && !post_register_done;
        if ((web_active || startup_sampling) && at_channel_idle_now()) {
            if (force_sample) {
                s_status_sample_requests.store(0, std::memory_order_relaxed);
            }
            if (startup_sampling || force_sample || last_signal == 0 ||
                now - last_signal > pdMS_TO_TICKS(SIGNAL_INTERVAL_WEB_MS)) {
                sample_signal_once();
                last_signal = now;
            }
            if (startup_sampling || force_sample || last_detail == 0 ||
                now - last_detail > pdMS_TO_TICKS(SIGNAL_DETAIL_INTERVAL_WEB_MS)) {
                sample_signal_detail_once();
                last_detail = now;
            }
            if ((startup_sampling || force_sample || !startup_info_complete()) &&
                (startup_sampling || force_sample || last_identity == 0 ||
                 now - last_identity > pdMS_TO_TICKS(IDENTITY_RETRY_INTERVAL_MS))) {
                sample_identity_once(false, true);
                last_identity = now;
            }
            if (startup_sampling && startup_info_complete()) {
                set_phase("ready");
                post_register_done = true;
            }
        }
        // 正常态按 60s 健康探测；未注册但并非已确认无卡时缩短到 5s，
        // 让热插拔/自动重启后的注册恢复不必最多再等一分钟。
        uint32_t health_interval_ms = 60000UL;
        if (!registered && sim_present != 0) health_interval_ms = 5000UL;
        else if (sms_reconfigure_pending && sim_present != 0) health_interval_ms = 15000UL;
        if ((reset_handled || now - last_health > pdMS_TO_TICKS(health_interval_ms)) &&
            at_channel_idle_now()) {
            last_health = now;
            std::string resp;
            if (send_ok("AT+CEREG?", 1200, &resp) && parse_cereg(resp, stat)) {
                health_fail_count = 0;
                bool now_ready = (stat == 1 || stat == 5);
                IdfModemStatus reg_patch;
                reg_patch.ceregStat = stat;
                reg_patch.modemReady = now_ready;
                reg_patch.phase = now_ready ? (post_register_done ? "ready" : "sampling") : "registering";
                update_status(reg_patch);
                if (now_ready) {
                    registered = true;
                    dereg_count = 0;
                    bool sms_reconfigured_now = false;
                    if (sms_reconfigure_pending) {
                        // CPIN READY 之后模组仍可能异步重建短信栈并回落默认设置；
                        // 必须在真正注册成功的稳定点再次写入 PDU/CNMI/CPMS。
                        idf_log_line("网络重新注册成功，重申短信收发配置");
                        sms_reconfigure_pending = !configure_sms_and_registration();
                        sms_reconfigured_now = true;
                    }
                    if (!post_register_done) {
                        // 迟到/恢复的注册也要补跑必须的网络配置和首页基础信息。
                        // 掉网后恢复可能意味着模组自发复位过：存储选择无条件重跑，
                        // 不能只看 pending 标志(初次成功后它恒为 false)
                        if (!sms_reconfigured_now) s_sms_storage_pending = !select_sms_storage();
                        IdfSimSettingsView cfg = idf_config_get_sim_settings_view();
                        apply_operator_if_configured(cfg);
                        if (cfg.dataEnabled) sample_cell_ip_once();
                        enforce_roaming_data_policy(cfg, stat);
                        sample_signal_once();
                        sample_signal_detail_once();
                        sample_identity_once(false, true);
                        post_register_done = startup_info_complete();
                        set_phase(post_register_done ? "ready" : "sampling");
                    }
                } else {
                    registered = false;
                    post_register_done = false;
                    sms_reconfigure_pending = true;
                    // 未注册态改为 5 秒快探测后，仍保持约 5 分钟再重启，避免普通的小区
                    // 重选/漫游注册过程被过早打断；已确认无卡时不做无意义的周期重启。
                    if (sim_present == 0) {
                        dereg_count = 0;
                    } else if (++dereg_count >= 60) {
                        dereg_count = 0;
                        idf_log_line("模组长时间未注册网络，触发硬重启恢复");
                        s_reset_request.store(2, std::memory_order_relaxed);
                    }
                }
            } else if (++health_fail_count >= 3) {
                health_fail_count = 0;
                idf_log_line("模组健康探测连续失败，触发硬重启恢复");
                s_reset_request.store(2, std::memory_order_relaxed);
            }
        }
        // SIM 热插拔检测：低频轮询 AT+CPIN?，识别运行中插卡/拔卡。
        // 插入(无卡→有卡)：自动硬重启模组 + 作废旧身份，让新卡从干净状态初始化；
        // 拔出(有卡→无卡)：标记未就绪并清空身份，避免概览沿用旧卡信息。
        int64_t sim_check_now_us = esp_timer_get_time();
        if (sim_check_now_us >= sim_check_not_before_us &&
            sim_check_now_us >= s_sim_refresh_guard_until_us.load(std::memory_order_relaxed) &&
            (last_sim_check == 0 || now - last_sim_check > pdMS_TO_TICKS(SIM_CHECK_INTERVAL_MS)) &&
            at_channel_idle_now()) {
            last_sim_check = now;
            int present_now = query_sim_ready() ? 1 : 0;
            if (sim_present == -1) {
                sim_present = present_now;  // 首次仅记基线，不当作插拔事件
            } else if (present_now != sim_present) {
                sim_present = present_now;
                if (present_now == 1) {
                    // 仅在 CPIN READY 时立即重发 CMGF/CNMI 不够可靠：部分 ML307 固件会在
                    // 换卡后继续异步重建协议栈，随后把短信模式恢复默认值。自动硬重启一次，
                    // 等价于用户手动断电恢复，同时保留“换卡无需重启 ESP32”的体验。
                    idf_log_line("检测到 SIM 卡插入，自动硬重启模组以完整初始化短信栈");
                    idf_modem_invalidate_sim_identity();
                    registered = false;
                    post_register_done = false;
                    dereg_count = 0;
                    sms_reconfigure_pending = true;
                    idf_modem_request_reset(true);
                } else {
                    idf_log_line("检测到 SIM 卡移除");
                    registered = false;
                    post_register_done = false;
                    sms_reconfigure_pending = true;
                    idf_modem_invalidate_sim_identity();
                    set_phase("registering");
                }
            }
        }
        for (int i = 0; i < 10; ++i) {
            // 采样请求只有 AT 空闲时才会被外层消费；通道被长任务(保号下载/eSIM)
            // 占用期间若无条件 break，外层 while 会变成无延时热自旋，饿死 idle 任务
            if (s_status_sample_requests.load(std::memory_order_relaxed) > 0 &&
                at_channel_idle_now()) break;
            // 事件驱动等待：UART 一有数据(URC/短信直推)立刻醒来抓取；
            // 无事件时 500ms 超时兜底轮询，节奏与原轮询一致
            uart_event_t evt;
            if (s_uart_evt_queue) {
                if (xQueueReceive(s_uart_evt_queue, &evt, pdMS_TO_TICKS(500)) == pdTRUE) {
                    while (xQueueReceive(s_uart_evt_queue, &evt, 0) == pdTRUE) {}  // 合并积压事件
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            // AT 通道被长任务(保号下载/大批量 CMGL)占用时抢不到锁：小睡再试，
            // 避免下载期间每个 RX 块事件都空转唤醒(数据由持锁方消费，URC 也由其转存)
            if (!poll_unsolicited_uart(20)) vTaskDelay(pdMS_TO_TICKS(100));
            // 重启请求/AT恢复补初始化尽快响应，不等满 5s 轮询窗
            if (s_reset_request.load(std::memory_order_relaxed) != 0) break;
            if (s_status_sample_requests.load(std::memory_order_relaxed) > 0 &&
                at_channel_idle_now()) break;
        }
    }
}

esp_err_t idf_modem_start(const IdfConfig& config)
{
    if (s_started) return ESP_OK;
    cleanup_start_resources();
    // eSIM 需跨多条 CCHO/CGLA/CCHC 独占 AT 通道，同任务内的单条 AT 再递归取锁。
    s_at_mutex = xSemaphoreCreateRecursiveMutex();
    s_status_mutex = xSemaphoreCreateMutex();
    s_urc_mutex = xSemaphoreCreateMutex();
    if (!s_event_sem) s_event_sem = xSemaphoreCreateBinary();
    if (!s_at_mutex || !s_status_mutex || !s_urc_mutex || !s_event_sem) {
        cleanup_start_resources();
        return ESP_ERR_NO_MEM;
    }
    if (!config.dataEnabled) set_status_cell_ip("");

    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = MODEM_BAUD;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    // 带事件队列安装：RX 数据到达即产生事件，模组任务空闲期可被立刻唤醒
    esp_err_t err = uart_driver_install(MODEM_UART, UART_RX_BUF, 0, 16, &s_uart_evt_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        idf_logf("模组 UART 驱动安装失败: %s", esp_err_to_name(err));
        cleanup_start_resources();
        return err;
    }
    err = uart_param_config(MODEM_UART, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(err));
        idf_logf("模组 UART 参数配置失败: %s", esp_err_to_name(err));
        uart_driver_delete(MODEM_UART);
        s_uart_evt_queue = nullptr;
        cleanup_start_resources();
        return err;
    }
    err = uart_set_pin(MODEM_UART, MODEM_TXD, MODEM_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART pin config failed: %s", esp_err_to_name(err));
        idf_logf("模组 UART 引脚配置失败: %s", esp_err_to_name(err));
        uart_driver_delete(MODEM_UART);
        s_uart_evt_queue = nullptr;
        cleanup_start_resources();
        return err;
    }
    uart_flush_input(MODEM_UART);
    s_started = true;

    BaseType_t ok = xTaskCreate(modem_task, "idf_modem", 8192, nullptr, 4, nullptr);
    if (ok != pdPASS) {
        s_started = false;
        uart_driver_delete(MODEM_UART);
        s_uart_evt_queue = nullptr;
        cleanup_start_resources();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

IdfModemStatus idf_modem_get_status(void)
{
    IdfModemStatus copy;
    if (!s_status_mutex) return copy;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        copy = s_status;
        xSemaphoreGive(s_status_mutex);
    }
    return copy;
}

esp_err_t idf_modem_request_reset(bool hard_reset)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    s_reset_request.store(hard_reset ? 2 : 1, std::memory_order_relaxed);
    set_phase("powering");
    return ESP_OK;
}

bool idf_modem_at_idle(void)
{
    return at_channel_idle_now();
}

void idf_modem_request_status_sample(void)
{
    s_last_web_poll_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    s_status_sample_requests.fetch_add(1, std::memory_order_relaxed);
}

void idf_modem_begin_esim_operation(void)
{
    s_esim_operation_depth.fetch_add(1, std::memory_order_relaxed);
    if (s_at_mutex) xSemaphoreTakeRecursive(s_at_mutex, portMAX_DELAY);
}

void idf_modem_end_esim_operation(void)
{
    if (s_at_mutex) xSemaphoreGiveRecursive(s_at_mutex);
    s_esim_operation_depth.fetch_sub(1, std::memory_order_relaxed);
}

bool idf_modem_esim_operation_active(void)
{
    return s_esim_operation_depth.load(std::memory_order_relaxed) != 0;
}

void idf_modem_expect_sim_refresh(void)
{
    // eSIM REFRESH 会让 CPIN 短暂掉线；若按物理拔插处理，恢复 READY 时会误触发硬重启。
    s_sim_refresh_guard_until_us.store(esp_timer_get_time() + 90LL * 1000LL * 1000LL,
                                       std::memory_order_relaxed);
}

static std::atomic<void (*)(void)> s_sim_identity_hook{nullptr};

void idf_modem_set_sim_identity_hook(void (*hook)(void))
{
    s_sim_identity_hook.store(hook, std::memory_order_relaxed);
}

void idf_modem_invalidate_sim_identity(void)
{
    // 清除随卡变化的身份字段：切/启/禁 eSIM Profile 后当前生效的卡已不同，
    // 但 sample_identity_once 对非空字段跳过重读，会一直沿用旧卡的号码/ICCID/运营商。
    if (s_status_mutex && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_status.iccid.clear();
        s_status.imsi.clear();
        s_status.phone.clear();
        s_status.operatorName.clear();
        s_status.apnSim.clear();
        s_status.identityFresh = false;
        xSemaphoreGive(s_status_mutex);
    }
    // 复位采样"已尝试"标志，让网络字段(运营商/号码)重新查询；静态字段(型号/IMEI)非空仍跳过
    reset_identity_sampling_state();
    idf_modem_request_status_sample();
    // 通知上层(idf_esim)：热插拔换卡后 EID 等缓存也要失效
    void (*hook)(void) = s_sim_identity_hook.load(std::memory_order_relaxed);
    if (hook) hook();
}

void idf_modem_power_off_for_restart(void)
{
    // 先写输出寄存器再配方向，切到输出的瞬间即输出低
    gpio_set_level(MODEM_EN, 0);
    modem_en_gpio_init();
    gpio_set_level(MODEM_EN, 0);
    // 保证断电时间足够(与 modem_power_cycle 一致)，ESP 重启后是干净的模组冷启动
    vTaskDelay(pdMS_TO_TICKS(MODEM_POWERDOWN_MS));
}

bool idf_modem_wait_event(uint32_t timeout_ms)
{
    if (!s_event_sem) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return false;
    }
    return xSemaphoreTake(s_event_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void idf_modem_signal_event(void)
{
    if (s_event_sem) xSemaphoreGive(s_event_sem);
}

bool idf_modem_take_urc(std::string& out)
{
    out.clear();
    if (!s_urc_mutex) return false;
    if (xSemaphoreTake(s_urc_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (!s_urc_buffer.empty()) {
        out.swap(s_urc_buffer);
    }
    xSemaphoreGive(s_urc_mutex);
    return !out.empty();
}
