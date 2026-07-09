#include "idf_web.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>
#include <stdlib.h>
#include <new>

#include "driver/temperature_sensor.h"
#include "esp_core_dump.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "idf_config.h"
#include "idf_esim.h"
#include "idf_inbox.h"
#include "idf_log.h"
#include "idf_modem.h"
#include "idf_push.h"
#include "idf_sms.h"
#include "idf_wifi.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "web_assets.h"

static const char* TAG = "idf_web";
static httpd_handle_t s_server = nullptr;
static SemaphoreHandle_t s_cell_job_mutex = nullptr;
static bool s_scheduler_started = false;

struct WebAsyncJob {
    bool running = false;
    bool done = false;
    bool success = false;
    bool queued = false;
    std::string url;
    std::string action;
    std::string message;
};

struct EsimWebCache {
    std::string eid;
    std::vector<IdfEsimProfile> profiles;
    uint32_t updatedAt = 0;
    std::string message;
};

static WebAsyncJob s_ping_job;
static WebAsyncJob s_keepalive_job;
static WebAsyncJob s_esim_job;
static WebAsyncJob s_sched_job;
static int s_sched_job_index = -1;
static EsimWebCache s_esim_cache;
static bool s_modem_apply_running = false;
static bool s_web_modem_action_running = false;

static bool cell_job_lock(TickType_t ticks = pdMS_TO_TICKS(300));
static void cell_job_unlock(void);
static bool cellular_job_active_locked(void);
static bool cellular_job_active(void);
static void set_json_no_cache(httpd_req_t* req);

static void cleanup_cell_job_mutex_if_unused()
{
    if (s_scheduler_started) return;
    if (s_cell_job_mutex) {
        vSemaphoreDelete(s_cell_job_mutex);
        s_cell_job_mutex = nullptr;
    }
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

static void json_prop(std::string& out, const char* key, const std::string& value)
{
    out += "\"";
    out += key;
    out += "\":\"";
    json_escape_append(out, value);
    out += "\"";
}

static bool epoch_valid(uint32_t epoch)
{
    return epoch >= 1700000000u;
}

static bool push_key_sensitive(uint8_t type, int slot)
{
    if (slot == 1) {
        return type == 2 || type == 4 || type == 5 || type == 6 || type == 8 || type == 9;
    }
    return slot == 2 && type == 10;
}

static std::string format_tz_offset(int tz_offset_min)
{
    if (tz_offset_min == 0) return "UTC";
    char buf[16];
    int total = tz_offset_min < 0 ? -tz_offset_min : tz_offset_min;
    int hh = total / 60;
    int mm = total % 60;
    if (mm == 0) {
        snprintf(buf, sizeof(buf), "UTC%c%d", tz_offset_min < 0 ? '-' : '+', hh);
    } else {
        snprintf(buf, sizeof(buf), "UTC%c%d:%02d", tz_offset_min < 0 ? '-' : '+', hh, mm);
    }
    return std::string(buf);
}

static std::string format_epoch_local(uint32_t epoch, int tz_offset_min)
{
    if (!epoch_valid(epoch)) return std::string();
    time_t shifted = static_cast<time_t>(static_cast<int64_t>(epoch) + static_cast<int64_t>(tz_offset_min) * 60LL);
    struct tm tmv = {};
    gmtime_r(&shifted, &tmv);
    char buf[48];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d %s",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             format_tz_offset(tz_offset_min).c_str());
    return std::string(buf);
}

// 芯片内部温度(概览页显示)；驱动懒加载，失败只记一次不再重试
static bool read_chip_temp(float& out)
{
    static temperature_sensor_handle_t s_tsens = nullptr;
    static bool s_tsens_failed = false;
    if (!s_tsens && !s_tsens_failed) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK ||
            temperature_sensor_enable(s_tsens) != ESP_OK) {
            s_tsens_failed = true;
            return false;
        }
    }
    return s_tsens && temperature_sensor_get_celsius(s_tsens, &out) == ESP_OK;
}

static bool auth_matches_config(const char* auth)
{
    static constexpr const char* prefix = "Basic ";
    if (strncmp(auth, prefix, strlen(prefix)) != 0) return false;

    unsigned char decoded[384] = {};
    size_t decoded_len = 0;
    const unsigned char* encoded = reinterpret_cast<const unsigned char*>(auth + strlen(prefix));
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len, encoded, strlen(auth + strlen(prefix)));
    if (rc != 0) return false;
    decoded[decoded_len] = '\0';

    // 用窄访问器锁内比对：每个请求(含 2s 一次的 /status 轮询)做全量配置深拷贝
    // 是持续的堆抖动源
    char* colon = strchr(reinterpret_cast<char*>(decoded), ':');
    if (!colon) return false;
    *colon = '\0';
    return idf_config_check_web_auth(reinterpret_cast<char*>(decoded), colon + 1);
}

// 配网热点模式下允许免认证访问的接口白名单：只覆盖"进入独立配网页并配好 WiFi"
// 所需的最小集合。刻意不放行 /ui、/status、/config.json、/assets 等——配网页是
// 自包含的独立页面，不需要它们；不放行可避免开放热点期间暴露完整后台与已有配置。
static bool is_ap_open_uri(const char* uri)
{
    if (!uri) return false;
    // 按"路径"精确匹配，忽略 ?query
    size_t path_len = strcspn(uri, "?");
    static const char* const kOpen[] = {
        "/", "/wifiscan", "/wificonfig", "/apstatus",
    };
    for (const char* p : kOpen) {
        if (strlen(p) == path_len && strncmp(uri, p, path_len) == 0) return true;
    }
    return false;
}

static bool check_auth(httpd_req_t* req)
{
    // 配网热点(开放 AP)模式下放行配网所需接口：设备此时是开放无加密热点，系统
    // Captive Portal 探测/弹窗只能拿到 401(显示 "unauthorized")，新用户无从进入
    // 配网页面(见 issue #1)；且开放空口上 Basic 凭据本就明文可嗅探，对这几个
    // 只读/配网接口强制认证意义不大。导出/导入/OTA/发短信/收件箱等敏感接口不在
    // 白名单内，仍需认证，避免开放热点期间泄露密钥或被滥用。
    if (idf_wifi_is_ap_mode() && is_ap_open_uri(req->uri)) return true;

    char auth[512] = {};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) == ESP_OK &&
        auth_matches_config(auth)) {
        return true;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SMS Forwarding\"");
    httpd_resp_sendstr(req, "Unauthorized");
    return false;
}

static bool check_csrf(httpd_req_t* req)
{
    char token[16] = {};
    if (httpd_req_get_hdr_value_str(req, "X-SMS-CSRF", token, sizeof(token)) == ESP_OK &&
        strcmp(token, "1") == 0) {
        return true;
    }
    set_json_no_cache(req);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"跨站写入校验失败，请刷新页面后重试\"}");
    return false;
}

static bool etag_matches(httpd_req_t* req, const WebAsset& asset)
{
    char inm[96] = {};
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) != ESP_OK) {
        return false;
    }
    return strstr(inm, asset.etag) != nullptr;
}

static bool cache_has_token(const char* cache_control, const char* token)
{
    return cache_control && strstr(cache_control, token) != nullptr;
}

static void set_no_cache_headers(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
}

static void set_json_no_cache(httpd_req_t* req)
{
    httpd_resp_set_type(req, "application/json");
    set_no_cache_headers(req);
}

static bool ensure_get_or_post(httpd_req_t* req)
{
    if (req->method == HTTP_GET) return true;
    if (req->method == HTTP_POST) return check_csrf(req);
    set_json_no_cache(req);
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Allow", "GET, POST");
    httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"该接口只支持 GET/POST\"}");
    return false;
}

static esp_err_t send_gzip_asset(httpd_req_t* req, const WebAsset& asset, const char* cache_control)
{
    const bool no_store = cache_has_token(cache_control, "no-store");
    httpd_resp_set_hdr(req, "Cache-Control", cache_control);
    if (cache_has_token(cache_control, "no-cache") || no_store) {
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
    }
    if (!no_store) {
        httpd_resp_set_hdr(req, "ETag", asset.etag);
    }
    httpd_resp_set_hdr(req, "Vary", "Authorization, Accept-Encoding");

    if (!no_store && etag_matches(req, asset)) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, nullptr, 0);
    }

    httpd_resp_set_type(req, asset.mime);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, reinterpret_cast<const char*>(asset.data), asset.length);
}

static esp_err_t handle_root(httpd_req_t* req)
{
    // 配网热点模式：只下发自包含的独立配网页(免密)，不加载完整后台，杜绝已有配置暴露
    if (idf_wifi_is_ap_mode()) {
        return send_gzip_asset(req, WEB_AP, "no-store, max-age=0");
    }
    if (!check_auth(req)) return ESP_OK;
    return send_gzip_asset(req, WEB_INDEX, "no-store, max-age=0");
}

static esp_err_t handle_asset(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (strstr(req->uri, "/app.css")) {
        return send_gzip_asset(req, WEB_APP_CSS, "private, max-age=31536000, immutable");
    }
    if (strstr(req->uri, "/app.js")) {
        return send_gzip_asset(req, WEB_APP_JS, "private, max-age=31536000, immutable");
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_OK;
}

static esp_err_t handle_ui_panel(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;

    char query[96] = {};
    char panel[32] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "panel", panel, sizeof(panel)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing panel");
        return ESP_OK;
    }

    const WebAsset* asset = findWebPanelAsset(panel);
    if (!asset) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unknown panel");
        return ESP_OK;
    }
    return send_gzip_asset(req, *asset, "no-store, max-age=0");
}

static esp_err_t handle_status(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    char query[32] = {};
    char sample[8] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "sample", sample, sizeof(sample)) == ESP_OK &&
        strcmp(sample, "1") == 0) {
        idf_modem_request_status_sample();
    }
    set_json_no_cache(req);

    // 窄快照：/status 每 2s 轮询一次，避免全量配置深拷贝造成持续堆抖动
    const IdfConfigStatusView cfg = idf_config_get_status_view();
    IdfWifiStatus wifi = idf_wifi_get_status();
    IdfModemStatus modem = idf_modem_get_status();
    IdfSmsStatus sms = idf_sms_get_status();
    time_t now = time(nullptr);
    uint64_t uptime = esp_timer_get_time() / 1000000ULL;

    std::string body;
    body.reserve(2350);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"version\":\"%s\",\"idfPort\":true,\"modemReady\":%s,"
             "\"modemInitPhase\":\"%s\",\"ceregStat\":%d,"
             "\"signalFresh\":%s,\"identityFresh\":%s,"
             "\"webAssetHash\":\"%s\",",
             IDF_FW_VERSION,
             modem.modemReady ? "true" : "false",
             modem.phase.c_str(), modem.ceregStat,
             modem.signalFresh ? "true" : "false",
             modem.identityFresh ? "true" : "false",
             WEB_ASSET_HASH);
    body += buf;
    snprintf(buf, sizeof(buf), "\"tz\":%d,", cfg.tzOffsetMin);
    body += buf;
    json_prop(body, "tzName", format_tz_offset(cfg.tzOffsetMin)); body += ",";
    snprintf(buf, sizeof(buf), "\"nowEpoch\":%ld,", static_cast<long>(now));
    body += buf;
    json_prop(body, "nowLocal", format_epoch_local(static_cast<uint32_t>(now), cfg.tzOffsetMin)); body += ",";
    snprintf(buf, sizeof(buf),
              "\"uptime\":%llu,\"resetReason\":%d,"
              "\"freeHeap\":%u,\"minFreeHeap\":%u,\"maxAllocHeap\":%u,\"httpStackFree\":%u,",
              static_cast<unsigned long long>(uptime), static_cast<int>(esp_reset_reason()),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(esp_get_minimum_free_heap_size()),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    body += buf;
    snprintf(buf, sizeof(buf),
             "\"smsTotal\":%u,\"lastSmsEpoch\":%u,",
             static_cast<unsigned>(sms.total),
             static_cast<unsigned>(sms.lastSmsEpoch));
    body += buf;
    json_prop(body, "lastSmsLocal", format_epoch_local(sms.lastSmsEpoch, cfg.tzOffsetMin)); body += ",";
    snprintf(buf, sizeof(buf),
             "\"inboxCount\":%u,"
             "\"fwdQueueDepth\":%d,\"queueDepth\":%d,\"outSmsQueueDepth\":%d,\"emailQueueDepth\":%d,"
             "\"slowBusy\":%s,\"timeSynced\":%s,",
             static_cast<unsigned>(idf_inbox_count()),
             idf_push_forward_queue_depth(),
             idf_push_retry_queue_depth(),
             idf_sms_outgoing_queue_depth(),
             idf_push_email_queue_depth(),
             idf_push_busy() ? "true" : "false",
             now > 100000 ? "true" : "false");
    body += buf;
    if (modem.csq >= 0) {
        snprintf(buf, sizeof(buf), "\"csq\":%d,\"ber\":%d,", modem.csq, modem.ber);
        body += buf;
    } else {
        body += "\"csq\":null,\"ber\":99,";
    }
    if (modem.rsrp != 999) {
        snprintf(buf, sizeof(buf), "\"rsrp\":%d,", modem.rsrp);
        body += buf;
    } else {
        body += "\"rsrp\":null,";
    }
    if (modem.rsrq != 999) {
        snprintf(buf, sizeof(buf), "\"rsrq\":%d,", modem.rsrq);
        body += buf;
    } else {
        body += "\"rsrq\":null,";
    }
    if (modem.sinr != 999) {
        snprintf(buf, sizeof(buf), "\"sinr\":%d,", modem.sinr);
        body += buf;
    } else {
        body += "\"sinr\":null,";
    }

    snprintf(buf, sizeof(buf),
             "\"dataEnabled\":%s,\"emailEnabled\":%s,\"emailConfigured\":%s,"
             "\"pushEnabled\":%s,\"pushEnabledCount\":%d,\"apMode\":%s,",
             cfg.dataEnabled ? "true" : "false",
             cfg.emailEnabled ? "true" : "false",
             cfg.emailConfigured ? "true" : "false",
             cfg.pushEnabled ? "true" : "false",
             cfg.pushEnabledCount,
             wifi.apMode ? "true" : "false");
    body += buf;

    json_prop(body, "ssid", wifi.staConnected ? wifi.ssid : wifi.apSsid); body += ",";
    json_prop(body, "ip", wifi.staConnected ? wifi.ip : wifi.apIp); body += ",";
    json_prop(body, "gw", wifi.gw); body += ",";
    json_prop(body, "mask", wifi.mask); body += ",";
    json_prop(body, "dns", wifi.dns); body += ",";
    json_prop(body, "mac", wifi.mac); body += ",";
    json_prop(body, "bssid", wifi.bssid); body += ",";
    if (wifi.staConnected) {
        snprintf(buf, sizeof(buf), "\"rssi\":%d,\"chan\":%d,", wifi.rssi, wifi.channel);
        body += buf;
    } else {
        body += "\"rssi\":null,\"chan\":0,";
    }

    json_prop(body, "adminPhone", cfg.adminPhone); body += ",";
    json_prop(body, "phone", modem.phone.empty() ? cfg.phoneNumber : modem.phone); body += ",";
    json_prop(body, "apn", cfg.apn); body += ",";
    json_prop(body, "apnSim", modem.apnSim); body += ",";
    json_prop(body, "cellIp", modem.cellIp); body += ",";
    json_prop(body, "operator", modem.operatorName); body += ",";
    json_prop(body, "mfr", modem.mfr); body += ",";
    json_prop(body, "model", modem.model); body += ",";
    json_prop(body, "fwver", modem.fwver); body += ",";
    json_prop(body, "imei", modem.imei); body += ",";
    json_prop(body, "iccid", modem.iccid); body += ",";
    json_prop(body, "imsi", modem.imsi);
    float temp = 0;
    if (read_chip_temp(temp)) {
        snprintf(buf, sizeof(buf), ",\"chipTemp\":%.1f}", static_cast<double>(temp));
        body += buf;
    } else {
        body += ",\"chipTemp\":null}";
    }

    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t send_config_json(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    set_json_no_cache(req);

    const IdfConfigWebView cfg = idf_config_get_web_view();
    std::string body;
    body.reserve(4096);
    char buf[256];
    body += "{";
    json_prop(body, "wifiSsid", cfg.wifiSsid); body += ",";
    json_prop(body, "wifiSsid2", cfg.wifiSsid2); body += ",";
    json_prop(body, "webUser", cfg.webUser); body += ",";
    // 密码字段只用于表单占位，不能在配置 JSON 中回显明文。
    json_prop(body, "webPass", ""); body += ",";
    json_prop(body, "smtpServer", cfg.smtpServer); body += ",";
    snprintf(buf, sizeof(buf), "\"smtpPort\":%d,", cfg.smtpPort); body += buf;
    json_prop(body, "smtpUser", cfg.smtpUser); body += ",";
    json_prop(body, "smtpPass", ""); body += ",";
    snprintf(buf, sizeof(buf), "\"smtpPassSet\":%s,", cfg.smtpPass.empty() ? "false" : "true");
    body += buf;
    json_prop(body, "smtpSendTo", cfg.smtpSendTo); body += ",";
    json_prop(body, "adminPhone", cfg.adminPhone); body += ",";
    json_prop(body, "numberBlackList", cfg.numberBlackList); body += ",";
    json_prop(body, "forwardRules", cfg.forwardRules); body += ",";
    snprintf(buf, sizeof(buf),
             "\"emailEnabled\":%s,\"emailConfigured\":%s,\"pushEnabled\":%s,"
             "\"pushEnabledCount\":%d,\"modemReady\":%s,\"inboxMax\":50,",
             cfg.emailEnabled ? "true" : "false",
              cfg.emailConfigured ? "true" : "false",
              cfg.pushEnabled ? "true" : "false",
              cfg.pushEnabledCount,
              idf_modem_get_status().modemReady ? "true" : "false");
    body += buf;
    json_prop(body, "ntpServer", cfg.ntpServer); body += ",";
    snprintf(buf, sizeof(buf),
             "\"tzOffsetMin\":%d,\"rebootEnabled\":%s,\"rebootHour\":%d,"
             "\"hbEnabled\":%s,\"hbHour\":%d,\"dataEnabled\":%s,\"roamingEnabled\":%s,",
             cfg.tzOffsetMin,
             cfg.rebootEnabled ? "true" : "false", cfg.rebootHour,
             cfg.hbEnabled ? "true" : "false", cfg.hbHour,
             cfg.dataEnabled ? "true" : "false",
             cfg.roamingEnabled ? "true" : "false");
    body += buf;
    json_prop(body, "apn", cfg.apn); body += ",";
    json_prop(body, "phoneNumber", cfg.phoneNumber); body += ",";
    json_prop(body, "operatorPlmn", cfg.operatorPlmn); body += ",";
    json_prop(body, "kaProfile", cfg.kaProfile); body += ",";
    body += "\"netLedEnabled\":";
    body += cfg.netLedEnabled ? "true" : "false";
    body += ",";
    body += "\"callNotifyEnabled\":";
    body += cfg.callNotifyEnabled ? "true" : "false";
    body += ",";
    body += "\"pushChannels\":[";
    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        if (i) body += ",";
        const IdfPushChannel& ch = cfg.pushChannels[i];
        snprintf(buf, sizeof(buf), "{\"enabled\":%s,\"type\":%u,",
                 ch.enabled ? "true" : "false", static_cast<unsigned>(ch.type));
        body += buf;
        bool key1_redacted = push_key_sensitive(ch.type, 1);
        bool key2_redacted = push_key_sensitive(ch.type, 2);
        json_prop(body, "name", ch.name); body += ",";
        json_prop(body, "url", ch.url); body += ",";
        json_prop(body, "key1", key1_redacted ? std::string() : ch.key1); body += ",";
        snprintf(buf, sizeof(buf), "\"key1Set\":%s,\"key1Redacted\":%s,",
                 ch.key1.empty() ? "false" : "true", key1_redacted ? "true" : "false");
        body += buf;
        json_prop(body, "key2", key2_redacted ? std::string() : ch.key2); body += ",";
        snprintf(buf, sizeof(buf), "\"key2Set\":%s,\"key2Redacted\":%s,",
                 ch.key2.empty() ? "false" : "true", key2_redacted ? "true" : "false");
        body += buf;
        json_prop(body, "customBody", ch.customBody);
        body += "}";
    }
    {
        uint64_t up_s = esp_timer_get_time() / 1000000ULL;
        unsigned days = static_cast<unsigned>(up_s / 86400ULL);
        unsigned hours = static_cast<unsigned>((up_s / 3600ULL) % 24ULL);
        unsigned mins = static_cast<unsigned>((up_s / 60ULL) % 60ULL);
        char up_buf[64];
        if (days > 0) snprintf(up_buf, sizeof(up_buf), "%u天%u小时%u分", days, hours, mins);
        else if (hours > 0) snprintf(up_buf, sizeof(up_buf), "%u小时%u分", hours, mins);
        else snprintf(up_buf, sizeof(up_buf), "%u分钟", mins);
        body += "],\"uptimeText\":\"";
        body += up_buf;
        body += "\"}";
    }
    return httpd_resp_send(req, body.c_str(), body.size());
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static std::string url_decode(const char* data, size_t len)
{
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        char ch = data[i];
        if (ch == '+') {
            out += ' ';
        } else if (ch == '%' && i + 2 < len) {
            int hi = hex_value(data[i + 1]);
            int lo = hex_value(data[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                out += ch;
            }
        } else {
            out += ch;
        }
    }
    return out;
}

static esp_err_t read_body(httpd_req_t* req, std::string& body, size_t max_len = 8192)
{
    if (req->content_len > max_len) {
        set_no_cache_headers(req);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Body too large");
        return ESP_FAIL;
    }
    body.assign(req->content_len, '\0');
    size_t received = 0;
    int timeouts = 0;
    // 连续超时计数会被任何字节进展清零，涓流客户端(每十几秒发 1 字节)能把
    // httpd 唯一 worker 卡住数小时——必须叠加总时长硬上限
    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t hard_span = pdMS_TO_TICKS(30000);
    while (received < body.size()) {
        if (static_cast<TickType_t>(xTaskGetTickCount() - start_tick) >= hard_span) {
            set_no_cache_headers(req);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive too slow");
            return ESP_FAIL;
        }
        int ret = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (ret <= 0) {
            // 超时最多容忍 3 次(~15s)：httpd 单任务串行处理请求，
            // 一个挂着不发数据的客户端会把整个 Web UI 卡死到重启
            if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
            set_no_cache_headers(req);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        timeouts = 0;
        received += static_cast<size_t>(ret);
    }
    return ESP_OK;
}

static void send_modem_busy_json(httpd_req_t* req)
{
    set_json_no_cache(req);
    httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"模组或蜂窝/eSIM 任务正忙，请稍后重试\"}");
}

// Web 直接 AT 操作不持有状态锁；只设置一个短暂标记，避免 eSIM/保号事务在两条 AT 间插队。
class WebModemActionGuard {
public:
    WebModemActionGuard() = default;
    WebModemActionGuard(const WebModemActionGuard&) = delete;
    WebModemActionGuard& operator=(const WebModemActionGuard&) = delete;

    bool begin(httpd_req_t* req)
    {
        if (!cell_job_lock()) {
            send_modem_busy_json(req);
            return false;
        }
        if (cellular_job_active_locked() || !idf_modem_at_idle()) {
            cell_job_unlock();
            send_modem_busy_json(req);
            return false;
        }
        s_web_modem_action_running = true;
        m_active = true;
        cell_job_unlock();
        return true;
    }

    ~WebModemActionGuard()
    {
        if (!m_active) return;
        if (cell_job_lock(portMAX_DELAY)) {
            s_web_modem_action_running = false;
            cell_job_unlock();
        }
    }

private:
    bool m_active = false;
};

static IdfFormFields parse_urlencoded(const std::string& body)
{
    IdfFormFields fields;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        size_t eq = body.find('=', pos);
        if (eq == std::string::npos || eq > amp) eq = amp;
        std::string key = url_decode(body.data() + pos, eq - pos);
        std::string value;
        if (eq < amp) value = url_decode(body.data() + eq + 1, amp - eq - 1);
        if (!key.empty()) fields.emplace_back(std::move(key), std::move(value));
        if (amp == body.size()) break;
        pos = amp + 1;
    }
    return fields;
}

static const std::string* find_field(const IdfFormFields& fields, const char* key)
{
    for (const auto& field : fields) {
        if (field.first == key) return &field.second;
    }
    return nullptr;
}

static bool has_field(const IdfFormFields& fields, const char* key)
{
    return find_field(fields, key) != nullptr;
}

static std::string field_text(const IdfFormFields& fields, const char* key)
{
    const std::string* value = find_field(fields, key);
    return value ? *value : std::string();
}

static bool field_blank(const std::string& value)
{
    for (char ch : value) {
        if (!isspace(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

static bool parse_int_strict(const std::string& text, int& out)
{
    errno = 0;
    char* end = nullptr;
    long parsed = strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    while (*end != '\0') {
        if (!isspace(static_cast<unsigned char>(*end))) return false;
        ++end;
    }
    out = static_cast<int>(parsed);
    return true;
}

static bool parse_u32_strict(const char* raw, uint32_t& value, bool allow_zero)
{
    if (!raw || raw[0] == '\0') return false;
    uint32_t parsed = 0;
    for (size_t i = 0; raw[i] != '\0'; ++i) {
        if (!isdigit(static_cast<unsigned char>(raw[i]))) return false;
        uint8_t digit = static_cast<uint8_t>(raw[i] - '0');
        if (parsed > 429496729U || (parsed == 429496729U && digit > 5)) return false;
        parsed = parsed * 10U + digit;
    }
    if (!allow_zero && parsed == 0) return false;
    value = parsed;
    return true;
}

static int field_int(const IdfFormFields& fields, const char* key, int fallback)
{
    const std::string* value = find_field(fields, key);
    if (!value) return fallback;
    int parsed = fallback;
    return parse_int_strict(*value, parsed) ? parsed : fallback;
}

static uint8_t field_u8(const IdfFormFields& fields, const char* key, uint8_t fallback)
{
    int parsed = field_int(fields, key, fallback);
    if (parsed < 0 || parsed > 255) return fallback;
    return static_cast<uint8_t>(parsed);
}

static bool get_query_param(httpd_req_t* req, const char* key, std::string& out, size_t max_query = 192)
{
    std::string query(max_query, '\0');
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) return false;
    char raw[128] = {};
    if (httpd_query_key_value(query.c_str(), key, raw, sizeof(raw)) != ESP_OK) return false;
    out = url_decode(raw, strlen(raw));
    return true;
}

static std::string first_line_containing(const std::string& resp, const char* needle)
{
    size_t p = resp.find(needle);
    if (p == std::string::npos) return {};
    size_t start = resp.rfind('\n', p);
    start = (start == std::string::npos) ? 0 : start + 1;
    size_t end = resp.find('\n', p);
    if (end == std::string::npos) end = resp.size();
    std::string line = resp.substr(start, end - start);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    while (!line.empty() && isspace(static_cast<unsigned char>(line.front()))) line.erase(0, 1);
    return line;
}

static std::string first_digits(const std::string& resp)
{
    size_t pos = 0;
    while (pos < resp.size()) {
        size_t end = resp.find('\n', pos);
        if (end == std::string::npos) end = resp.size();
        std::string line = resp.substr(pos, end - pos);
        while (!line.empty() && isspace(static_cast<unsigned char>(line.front()))) line.erase(0, 1);
        while (!line.empty() && isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        bool digits = !line.empty();
        for (char ch : line) digits = digits && isdigit(static_cast<unsigned char>(ch));
        if (digits && line.size() >= 14 && line.size() <= 17) return line;
        pos = end + 1;
    }

    size_t start = std::string::npos;
    for (size_t i = 0; i <= resp.size(); ++i) {
        bool digit = i < resp.size() && isdigit(static_cast<unsigned char>(resp[i]));
        if (digit && start == std::string::npos) start = i;
        if (!digit && start != std::string::npos) {
            size_t len = i - start;
            if (len >= 14 && len <= 17) return resp.substr(start, len);
            start = std::string::npos;
        }
    }
    return {};
}

static bool parse_cfun_mode_line(const std::string& line, int& mode)
{
    const char* p = strstr(line.c_str(), "+CFUN:");
    if (!p) return false;
    int parsed = -1;
    if (!parse_int_strict(std::string(p + strlen("+CFUN:")), parsed)) return false;
    if (parsed < 0 || parsed > 4) return false;
    mode = parsed;
    return true;
}

static bool parse_csq_line(const std::string& line, int& rssi, int& ber)
{
    const char* p = strstr(line.c_str(), "+CSQ:");
    if (!p) return false;
    std::string rest = p + strlen("+CSQ:");
    size_t comma = rest.find(',');
    if (comma == std::string::npos) return false;
    int parsed_rssi = 99;
    int parsed_ber = 99;
    if (!parse_int_strict(rest.substr(0, comma), parsed_rssi)) return false;
    if (!parse_int_strict(rest.substr(comma + 1), parsed_ber)) return false;
    if (!((parsed_rssi >= 0 && parsed_rssi <= 31) || parsed_rssi == 99)) return false;
    if (!((parsed_ber >= 0 && parsed_ber <= 7) || parsed_ber == 99)) return false;
    rssi = parsed_rssi;
    ber = parsed_ber;
    return true;
}

static esp_err_t handle_at(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!ensure_get_or_post(req)) return ESP_OK;
    if (req->method != HTTP_POST) {
        set_json_no_cache(req);
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"AT 指令需要 POST\"}");
    }
    char query[192] = {};
    char cmd_raw[96] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "cmd", cmd_raw, sizeof(cmd_raw)) != ESP_OK) {
        set_no_cache_headers(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing cmd");
        return ESP_OK;
    }

    std::string cmd = url_decode(cmd_raw, strlen(cmd_raw));
    if (cmd.size() > 80 || cmd.find('\r') != std::string::npos || cmd.find('\n') != std::string::npos) {
        set_no_cache_headers(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid cmd");
        return ESP_OK;
    }
    WebModemActionGuard modem_action;
    if (!modem_action.begin(req)) return ESP_OK;

    std::string resp;
    esp_err_t err = idf_modem_send_at(cmd, 5000, resp);
    std::string body = "{\"success\":";
    body += (err == ESP_OK ? "true" : "false");
    body += ",";
    json_prop(body, "message", resp.empty() ? std::string(esp_err_to_name(err)) : resp);
    body += "}";
    set_json_no_cache(req);
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_flight(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!ensure_get_or_post(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    if (action.empty()) action = "query";

    bool change_action = (action == "toggle" || action == "on" || action == "off");
    if (change_action && req->method != HTTP_POST) {
        set_json_no_cache(req);
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"飞行模式切换需要 POST\"}");
    }

    WebModemActionGuard modem_action;
    if (!modem_action.begin(req)) return ESP_OK;

    std::string resp;
    bool success = false;
    std::string message;
    int mode = -1;

    if (idf_modem_send_at("AT+CFUN?", 3000, resp) == ESP_OK) {
        std::string line = first_line_containing(resp, "+CFUN:");
        success = parse_cfun_mode_line(line, mode);
    }

    if (change_action) {
        // 查询失败(mode<0)时禁止切换：盲发 CFUN=4 会静默关掉射频、停掉短信接收
        if (mode < 0) {
            set_json_no_cache(req);
            return httpd_resp_sendstr(req,
                "{\"success\":false,\"message\":\"无法获取当前飞行模式状态，已取消切换\"}");
        }
        int target;
        if (action == "on") target = 4;          // 进入飞行模式
        else if (action == "off") target = 1;    // 退出飞行模式
        else target = (mode == 1) ? 4 : 1;       // toggle：非正常态一律切回全功能
        char cmd[20];
        snprintf(cmd, sizeof(cmd), "AT+CFUN=%d", target);
        resp.clear();
        esp_err_t err = idf_modem_send_at(cmd, 8000, resp);
        success = (err == ESP_OK);
        if (success) {
            mode = target;
            idf_logf("网页切换飞行模式：%s", target == 4 ? "开启（射频关闭，暂停收发短信）" : "关闭（恢复全功能）");
        } else {
            message = resp.empty() ? esp_err_to_name(err) : resp;
        }
    }

    if (success) {
        if (mode == 4) message = "飞行模式（射频关闭）";
        else if (mode == 1) message = "全功能模式（正常）";
        else if (mode == 0) message = "最小功能模式";
        else {
            char buf[48];
            snprintf(buf, sizeof(buf), "未知模式 (%d)", mode);
            message = buf;
        }
    } else if (message.empty()) {
        message = resp.empty() ? "无法获取飞行模式" : resp;
    }

    std::string body = "{\"success\":";
    body += success ? "true" : "false";
    body += ",";
    json_prop(body, "message", message);
    body += "}";
    set_json_no_cache(req);
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_modem_control(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!ensure_get_or_post(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    bool success = false;
    std::string message;

    WebModemActionGuard modem_action;
    bool needs_modem = (action == "restart" || action == "hardreset" ||
                        action == "signal" || action == "operator" || action == "imei");
    if ((action == "restart" || action == "hardreset") && req->method != HTTP_POST) {
        set_json_no_cache(req);
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"模组重启需要 POST\"}");
    }
    if (needs_modem && !modem_action.begin(req)) return ESP_OK;

    if (action == "restart" || action == "hardreset") {
        bool hard = action == "hardreset";
        esp_err_t err = idf_modem_request_reset(hard);
        success = (err == ESP_OK);
        if (success) idf_logf("网页触发模组%s重启", hard ? "硬" : "软");
        message = success
            ? (hard ? "正在硬重启模组，请等待约 15 秒后刷新页面" : "正在软重启模组，请等待约 15 秒后刷新页面")
            : esp_err_to_name(err);
    } else if (action == "signal") {
        std::string resp;
        esp_err_t err = idf_modem_send_at("AT+CSQ", 3000, resp);
        std::string line = first_line_containing(resp, "+CSQ:");
        int rssi = 99;
        int ber = 99;
        if (err == ESP_OK && parse_csq_line(line, rssi, ber)) {
            int dbm = (rssi == 99) ? -999 : (-113 + rssi * 2);
            char buf[96];
            snprintf(buf, sizeof(buf), "信号强度(RSSI): %d dBm, CSQ原始值: %d, BER: %d", dbm, rssi, ber);
            message = buf;
            success = true;
        } else {
            message = resp.empty() ? esp_err_to_name(err) : resp;
        }
    } else if (action == "operator") {
        std::string resp;
        // 先选长名称格式，否则自动模式下 COPS? 只回模式位(+COPS: 0)，读不到运营商名
        idf_modem_send_at("AT+COPS=3,0", 3000, resp);
        esp_err_t err = idf_modem_send_at("AT+COPS?", 5000, resp);
        std::string line = first_line_containing(resp, "+COPS:");
        if (err == ESP_OK && !line.empty()) {
            size_t q1 = line.find('"');
            size_t q2 = q1 == std::string::npos ? std::string::npos : line.find('"', q1 + 1);
            message = (q1 != std::string::npos && q2 != std::string::npos) ? line.substr(q1 + 1, q2 - q1 - 1) : line;
            success = true;
        } else {
            message = resp.empty() ? esp_err_to_name(err) : resp;
        }
    } else if (action == "imei") {
        std::string resp;
        esp_err_t err = idf_modem_send_at("AT+CGSN", 3000, resp);
        message = first_digits(resp);
        success = (err == ESP_OK && !message.empty());
        if (!success) message = resp.empty() ? esp_err_to_name(err) : resp;
    } else {
        message = "未知操作: " + action;
    }

    std::string body = "{\"success\":";
    body += success ? "true" : "false";
    body += ",";
    json_prop(body, "message", message);
    body += "}";
    set_json_no_cache(req);
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_ussd(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!ensure_get_or_post(req)) return ESP_OK;
    if (req->method != HTTP_POST) {
        set_json_no_cache(req);
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"USSD 查询需要 POST\"}");
    }
    std::string code;
    get_query_param(req, "code", code);
    bool valid = !code.empty() && code.size() <= 24;
    for (char ch : code) {
        if (!(isdigit(static_cast<unsigned char>(ch)) || ch == '*' || ch == '#')) {
            valid = false;
            break;
        }
    }
    if (!valid) {
        set_json_no_cache(req);
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"USSD 码为空或包含非法字符\"}");
    }
    WebModemActionGuard modem_action;
    if (!modem_action.begin(req)) return ESP_OK;

    idf_logf("网页发起 USSD 查询：%s", code.c_str());
    std::string cmd = "AT+CUSD=1,\"" + code + "\",15";
    std::string resp;
    esp_err_t err = idf_modem_send_at_until(cmd, "+CUSD:", 20000, resp);
    bool success = (err == ESP_OK && resp.find("+CUSD:") != std::string::npos);
    std::string message = resp.empty() ? std::string(esp_err_to_name(err)) : resp;
    std::string body = "{\"success\":";
    body += success ? "true" : "false";
    body += ",";
    json_prop(body, "message", message);
    body += "}";
    set_json_no_cache(req);
    return httpd_resp_send(req, body.c_str(), body.size());
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

static bool apn_valid_for_at(const std::string& apn)
{
    return apn.size() <= 96 && apn.find('"') == std::string::npos &&
           apn.find('\r') == std::string::npos && apn.find('\n') == std::string::npos;
}

struct ModemApplyTaskArg {
    bool dataChanged = false;
    bool operatorChanged = false;
    bool dataEnabled = false;
    bool roamingEnabled = true;
    std::string apn;
    std::string operatorPlmn;
};

static void modem_apply_task(void* raw)
{
    ModemApplyTaskArg* arg = static_cast<ModemApplyTaskArg*>(raw);
    bool data_changed = arg->dataChanged;
    bool operator_changed = arg->operatorChanged;
    bool data_enabled = arg->dataEnabled;
    bool roaming_enabled = arg->roamingEnabled;
    std::string apn = std::move(arg->apn);
    std::string operator_plmn = std::move(arg->operatorPlmn);
    delete arg;

    bool claimed = false;
    if (cell_job_lock()) {
        if (cellular_job_active_locked()) {
            cell_job_unlock();
            idf_log_line("SIM 设置已保存，蜂窝/eSIM 任务正忙，暂不立即下发 COPS/CGACT");
            vTaskDelete(nullptr);
            return;
        }
        s_modem_apply_running = true;
        claimed = true;
        cell_job_unlock();
    } else {
        idf_log_line("SIM 设置已保存，蜂窝任务状态锁忙，暂不立即下发 COPS/CGACT");
        vTaskDelete(nullptr);
        return;
    }

    auto finish = [&]() {
        if (claimed && cell_job_lock(portMAX_DELAY)) {
            s_modem_apply_running = false;
            cell_job_unlock();
        }
        vTaskDelete(nullptr);
    };

    IdfModemStatus modem = idf_modem_get_status();
    if (!modem.modemReady) {
        idf_log_line("SIM 设置已保存，模组未注册，暂不下发 COPS/CGACT");
        finish();
        return;
    }

    std::string resp;
    if (operator_changed) {
        if (!plmn_valid(operator_plmn)) {
            idf_log_line("运营商 PLMN 非法，未下发 COPS");
        } else if (operator_plmn.empty()) {
            idf_modem_send_at("AT+COPS=0", 30000, resp);
            idf_log_line("运营商: 自动注册(COPS=0)");
        } else {
            std::string cmd = "AT+COPS=1,2,\"" + operator_plmn + "\"";
            esp_err_t err = idf_modem_send_at(cmd, 30000, resp);
            idf_logf("运营商: 锁定 PLMN %s %s", operator_plmn.c_str(),
                     err == ESP_OK ? "成功" : "失败(可能不可达)");
        }
    }

    if (data_changed) {
        if (data_enabled && !roaming_enabled && modem.ceregStat == 5) {
            // 数据漫游关闭且当前漫游：不激活蜂窝数据(短信仍走 CS/IMS 不受影响)
            idf_modem_send_at("AT+CGACT=0,1", 5000, resp);
            idf_log_line("数据漫游已关闭：当前处于漫游，未激活蜂窝数据(不跑流量)");
        } else if (data_enabled) {
            if (!apn.empty() && apn_valid_for_at(apn)) {
                std::string cmd = "AT+CGDCONT=1,\"IP\",\"" + apn + "\"";
                idf_modem_send_at(cmd, 3000, resp);
            } else if (!apn.empty()) {
                idf_log_line("APN 包含非法字符，未下发 CGDCONT");
            }
            idf_modem_send_at("AT+CGACT=1,1", 10000, resp);
            std::string ip_resp;
            idf_modem_send_at("AT+CGPADDR=1", 3000, ip_resp);
            idf_logf("蜂窝数据已启用(APN=%s)", apn.empty() ? "自动" : apn.c_str());
        } else {
            idf_modem_send_at("AT+CGACT=0,1", 5000, resp);
            idf_log_line("蜂窝数据已禁用(零流量)");
        }
    }
    finish();
}

static void preserve_redacted_push_key(const IdfFormFields& fields,
                                       const IdfPushChannel& previous,
                                       int index,
                                       int slot,
                                       IdfPushChannel& next)
{
    char name[32];
    snprintf(name, sizeof(name), "push%dkey%dKeep", index, slot);
    bool keep = field_text(fields, name) == "1";
    snprintf(name, sizeof(name), "push%dkey%dClear", index, slot);
    bool clear = has_field(fields, name);
    std::string& value = (slot == 1) ? next.key1 : next.key2;
    const std::string& old_value = (slot == 1) ? previous.key1 : previous.key2;
    if (clear) {
        value.clear();
        return;
    }
    if (keep && next.type == previous.type && field_blank(value)) {
        value = old_value;
    }
}

static void parse_push_channels_form(const IdfFormFields& fields,
                                     const IdfPushChannel previous[IDF_MAX_PUSH_CHANNELS],
                                     IdfPushChannel channels[IDF_MAX_PUSH_CHANNELS])
{
    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        char key[24];
        snprintf(key, sizeof(key), "push%den", i);
        channels[i].enabled = has_field(fields, key);
        snprintf(key, sizeof(key), "push%dtype", i);
        channels[i].type = field_u8(fields, key, 1);
        snprintf(key, sizeof(key), "push%durl", i);
        channels[i].url = field_text(fields, key);
        snprintf(key, sizeof(key), "push%dname", i);
        channels[i].name = field_text(fields, key);
        snprintf(key, sizeof(key), "push%dkey1", i);
        channels[i].key1 = field_text(fields, key);
        snprintf(key, sizeof(key), "push%dkey2", i);
        channels[i].key2 = field_text(fields, key);
        snprintf(key, sizeof(key), "push%dbody", i);
        channels[i].customBody = field_text(fields, key);
        preserve_redacted_push_key(fields, previous[i], i, 1, channels[i]);
        preserve_redacted_push_key(fields, previous[i], i, 2, channels[i]);
    }
}

static void parse_sched_tasks_form(const IdfFormFields& fields,
                                   IdfSchedTask tasks[IDF_MAX_SCHED_TASKS])
{
    for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) {
        char key[24];
        snprintf(key, sizeof(key), "st%dEn", i);
        tasks[i].enabled = has_field(fields, key);
        snprintf(key, sizeof(key), "st%dName", i);
        tasks[i].name = field_text(fields, key);
        snprintf(key, sizeof(key), "st%dProf", i);
        tasks[i].profile = field_text(fields, key);
        snprintf(key, sizeof(key), "st%dBack", i);
        tasks[i].switchBack = has_field(fields, key);
        snprintf(key, sizeof(key), "st%dDays", i);
        tasks[i].intervalDays = field_int(fields, key, 30);
        snprintf(key, sizeof(key), "st%dAct", i);
        tasks[i].action = field_u8(fields, key, 0);
        snprintf(key, sizeof(key), "st%dTgt", i);
        tasks[i].target = field_text(fields, key);
        snprintf(key, sizeof(key), "st%dPay", i);
        tasks[i].payload = field_text(fields, key);
    }
}

static esp_err_t handle_save(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    std::string body;
    // 16KB：5 个自定义推送模板 + 转发规则 URL 编码后可能超过 8KB
    if (read_body(req, body, 16384) != ESP_OK) return ESP_OK;
    IdfFormFields fields = parse_urlencoded(body);

    const bool account_form = has_field(fields, "accountForm");
    const bool tz_form = has_field(fields, "tzForm");
    const bool led_form = has_field(fields, "ledForm");
    const bool email_form = has_field(fields, "emailForm");
    const bool push_form = has_field(fields, "pushForm");
    const bool filter_form = has_field(fields, "filterForm");
    const bool rules_form = has_field(fields, "rulesForm");
    const bool ka_form = has_field(fields, "kaForm");
    const bool st_form = has_field(fields, "stForm");
    const bool system_sched_form = has_field(fields, "systemSchedForm");
    const bool sim_form = has_field(fields, "simForm");
    const bool call_form = has_field(fields, "callForm");
    const int form_count = (account_form ? 1 : 0) + (tz_form ? 1 : 0) +
                           (led_form ? 1 : 0) + (email_form ? 1 : 0) +
                           (push_form ? 1 : 0) + (filter_form ? 1 : 0) +
                           (rules_form ? 1 : 0) + (ka_form ? 1 : 0) +
                           (st_form ? 1 : 0) + (system_sched_form ? 1 : 0) +
                           (sim_form ? 1 : 0) + (call_form ? 1 : 0);
    if (form_count != 1) {
        idf_log_line(form_count == 0 ? "网页保存请求缺少表单标记，已忽略"
                                     : "网页保存请求包含多个表单标记，已拒绝");
        set_no_cache_headers(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            form_count == 0 ? "unknown save form" : "multiple save forms");
        return ESP_OK;
    }

    auto fail = [&](esp_err_t err) -> esp_err_t {
        set_no_cache_headers(req);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    };
    auto ok = [&](const char* log_line) -> esp_err_t {
        httpd_resp_set_type(req, "text/plain");
        set_no_cache_headers(req);
        httpd_resp_sendstr(req, "OK");
        idf_log_line(log_line);
        return ESP_OK;
    };

    if (account_form) {
        esp_err_t err = idf_config_save_account(field_text(fields, "webUser"),
                                                field_text(fields, "webPass"));
        if (err != ESP_OK) return fail(err);
        return ok("网页保存管理账号");
    }

    if (tz_form) {
        esp_err_t err = idf_config_save_time(field_int(fields, "tzOffsetMin", 480),
                                             field_text(fields, "ntpServer"));
        if (err != ESP_OK) return fail(err);
        return ok("网页保存时间设置");
    }

    if (led_form) {
        bool enabled = has_field(fields, "netLedEnabled");
        esp_err_t err = idf_config_set_net_led_enabled(enabled);
        if (err != ESP_OK) {
            return fail(err);
        }
        httpd_resp_set_type(req, "text/plain");
        set_no_cache_headers(req);
        httpd_resp_sendstr(req, "OK");
        idf_logf("网页保存 NET 指示灯配置: %s", enabled ? "开启" : "关闭");
        return ESP_OK;
    }

    if (call_form) {
        bool enabled = has_field(fields, "callNotifyEnabled");
        esp_err_t err = idf_config_set_call_notify_enabled(enabled);
        if (err != ESP_OK) return fail(err);
        httpd_resp_set_type(req, "text/plain");
        set_no_cache_headers(req);
        httpd_resp_sendstr(req, "OK");
        idf_logf("网页保存来电通知配置: %s", enabled ? "开启" : "关闭");
        return ESP_OK;
    }

    if (email_form) {
        std::string smtp_pass = field_text(fields, "smtpPass");
        bool preserve_smtp_pass = field_blank(smtp_pass);
        esp_err_t err = idf_config_save_email(has_field(fields, "emailEnabled"),
                                              field_text(fields, "smtpServer"),
                                              field_int(fields, "smtpPort", 465),
                                              field_text(fields, "smtpUser"),
                                              smtp_pass,
                                              field_text(fields, "smtpSendTo"),
                                              preserve_smtp_pass);
        if (err != ESP_OK) return fail(err);
        return ok("网页保存邮件设置");
    }

    if (push_form) {
        IdfPushChannel channels[IDF_MAX_PUSH_CHANNELS];
        IdfConfigWebView current = idf_config_get_web_view();
        parse_push_channels_form(fields, current.pushChannels, channels);
        esp_err_t err = idf_config_save_push(has_field(fields, "pushEnabled"), channels);
        if (err != ESP_OK) return fail(err);
        return ok("网页保存推送通道");
    }

    if (filter_form) {
        // 管理员号码与黑名单已拆成两个独立表单，各自只提交自己的字段：
        // 本次未提交的字段读当前值保留，避免只存其一时把另一个清空。
        IdfConfigWebView cur = idf_config_get_web_view();
        std::string admin = has_field(fields, "adminPhone")
                                ? field_text(fields, "adminPhone") : cur.adminPhone;
        std::string blacklist = has_field(fields, "numberBlackList")
                                    ? field_text(fields, "numberBlackList") : cur.numberBlackList;
        esp_err_t err = idf_config_save_filter(admin, blacklist);
        if (err != ESP_OK) return fail(err);
        return ok("网页保存权限与过滤");
    }

    if (rules_form) {
        esp_err_t err = idf_config_save_forward_rules(field_text(fields, "forwardRules"));
        if (err != ESP_OK) return fail(err);
        return ok("网页保存转发规则");
    }

    if (ka_form) {
        esp_err_t err = idf_config_save_keepalive(has_field(fields, "kaEnabled"),
                                                  field_int(fields, "kaIntervalDays", 175),
                                                  field_u8(fields, "kaAction", 1),
                                                  field_text(fields, "kaTarget"),
                                                  field_text(fields, "kaUrl"),
                                                  field_text(fields, "kaProfile"));
        if (err != ESP_OK) return fail(err);
        return ok("网页保存 SIM 保号");
    }

    if (st_form) {
        IdfSchedTask tasks[IDF_MAX_SCHED_TASKS];
        parse_sched_tasks_form(fields, tasks);
        esp_err_t err = idf_config_save_sched_tasks(tasks);
        if (err != ESP_OK) return fail(err);
        return ok("网页保存自定义定时任务");
    }

    if (system_sched_form) {
        esp_err_t err = idf_config_save_system_schedule(has_field(fields, "rebootEnabled"),
                                                        field_int(fields, "rebootHour", 4),
                                                        has_field(fields, "hbEnabled"),
                                                        field_int(fields, "hbHour", 9));
        if (err != ESP_OK) return fail(err);
        return ok("网页保存系统定时");
    }

    if (sim_form) {
        IdfSimSettingsView before = idf_config_get_sim_settings_view();
        esp_err_t err = idf_config_save_sim(has_field(fields, "dataEnabled"),
                                            has_field(fields, "roamingEnabled"),
                                            field_text(fields, "apn"),
                                            field_text(fields, "operatorPlmn"),
                                            field_text(fields, "phoneNumber"));
        if (err != ESP_OK) return fail(err);
        IdfSimSettingsView after = idf_config_get_sim_settings_view();
        bool data_changed = before.dataEnabled != after.dataEnabled || before.apn != after.apn ||
                            before.roamingEnabled != after.roamingEnabled;
        bool operator_changed = before.operatorPlmn != after.operatorPlmn;

        httpd_resp_set_type(req, "text/plain");
        set_no_cache_headers(req);
        httpd_resp_sendstr(req, "OK");
        idf_log_line("网页保存蜂窝设置");
        if (!data_changed && !operator_changed) return ESP_OK;

        ModemApplyTaskArg* arg = new (std::nothrow) ModemApplyTaskArg();
        if (arg) {
            arg->dataChanged = data_changed;
            arg->operatorChanged = operator_changed;
            arg->dataEnabled = after.dataEnabled;
            arg->apn = after.apn;
            arg->operatorPlmn = after.operatorPlmn;
            if (xTaskCreate(modem_apply_task, "idf_sim_apply", 4096, arg, 3, nullptr) != pdPASS) {
                delete arg;
                idf_log_line("SIM 设置已保存，但后台 AT 应用任务创建失败");
            }
        } else {
            idf_log_line("SIM 设置已保存，但后台 AT 应用任务内存不足");
        }
        return ESP_OK;
    }

    return ESP_OK;
}

static esp_err_t handle_export_config(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    std::string full;
    get_query_param(req, "full", full, 64);
    std::string body = (full == "1") ? idf_config_export_text(true) : idf_config_export_text(false);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    set_no_cache_headers(req);
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=sms_config.txt");
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_import_config(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    std::string body;
    if (read_body(req, body, 16384) != ESP_OK) return ESP_OK;
    int applied = 0;
    esp_err_t err = idf_config_import_text(body, &applied);
    set_json_no_cache(req);
    if (err != ESP_OK) {
        std::string resp = "{\"success\":false,";
        json_prop(resp, "message", std::string("导入失败: ") + esp_err_to_name(err));
        resp += "}";
        return httpd_resp_send(req, resp.c_str(), resp.size());
    }
    idf_logf("网页导入配置：应用 %d 项", applied);
    char msg[96];
    snprintf(msg, sizeof(msg), "已导入 %d 项，建议重启使全部生效", applied);
    std::string resp = "{\"success\":true,";
    json_prop(resp, "message", msg);
    resp += "}";
    return httpd_resp_send(req, resp.c_str(), resp.size());
}

static void restart_task(void*)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    // 计划内重启先给模组断电：保留"重启设备可救活已卡死模组"的原有语义，
    // 模组热启动快路径只留给崩溃/看门狗等意外复位
    idf_modem_power_off_for_restart();
    esp_restart();
}

static void schedule_restart_or_now(const char* task_name)
{
    if (xTaskCreate(restart_task, task_name, 3072, nullptr, 1, nullptr) == pdPASS) return;
    idf_log_line("重启任务创建失败，改为当前任务直接重启");
    restart_task(nullptr);
}

static esp_err_t handle_factory_reset(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    esp_err_t err = idf_config_factory_reset();
    if (err == ESP_OK) idf_log_line("网页触发恢复出厂：已清除全部配置，即将重启");
    set_json_no_cache(req);
    if (err != ESP_OK) {
        std::string body = "{\"success\":false,";
        json_prop(body, "message", std::string("恢复出厂失败: ") + esp_err_to_name(err));
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }
    esp_err_t send_err = httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"已清除配置，设备即将重启为默认设置\"}");
    schedule_restart_or_now("factory_restart");
    return send_err;
}

static esp_err_t handle_wifi_config(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    std::string body;
    if (read_body(req, body, 1024) != ESP_OK) return ESP_OK;
    IdfFormFields fields = parse_urlencoded(body);
    const std::string* ssid = find_field(fields, "ssid");
    const std::string* pass = find_field(fields, "pass");
    const std::string* ssid2 = find_field(fields, "ssid2");
    const std::string* pass2 = find_field(fields, "pass2");
    bool clear_pass = has_field(fields, "clearPass");
    bool clear_pass2 = has_field(fields, "clearPass2");
    esp_err_t err = idf_config_save_wifi(ssid ? *ssid : std::string(),
                                         pass ? *pass : std::string(),
                                         ssid2 ? *ssid2 : std::string(),
                                         pass2 ? *pass2 : std::string(),
                                         true, true, clear_pass, clear_pass2);
    set_json_no_cache(req);
    if (err != ESP_OK) {
        std::string msg = "{\"success\":false,\"message\":\"WiFi 配置无效\"}";
        return httpd_resp_send(req, msg.c_str(), msg.size());
    }
    if (idf_wifi_is_ap_mode()) {
        // 配网热点内：原地 APSTA 连接、不重启，配网页轮询 /apstatus 显示 IP，
        // 连上后由设备延时自动关闭热点(见 idf_wifi_provision_connect)
        idf_wifi_provision_connect(ssid_s, pass_s);
        std::string msg = "{\"success\":true,\"message\":\"已保存，正在连接\"}";
        return httpd_resp_send(req, msg.c_str(), msg.size());
    }
    std::string msg = "{\"success\":true,\"message\":\"WiFi 已保存，设备即将重启\"}";
    esp_err_t send_err = httpd_resp_send(req, msg.c_str(), msg.size());
    schedule_restart_or_now("wifi_restart");
    return send_err;
}

// 配网专用极简状态：只回连接态与本机 STA IP，绝不包含任何已有配置
static esp_err_t handle_apstatus(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;  // AP 模式经白名单免密；STA 模式仍需登录
    IdfWifiStatus wifi = idf_wifi_get_status();
    set_json_no_cache(req);
    std::string body = "{\"apMode\":";
    body += wifi.apMode ? "true" : "false";
    body += ",\"connected\":";
    body += (wifi.staConnected && !wifi.ip.empty()) ? "true" : "false";
    body += ",\"ip\":\"";
    body += wifi.ip;
    body += "\"}";
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_wifi(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!ensure_get_or_post(req)) return ESP_OK;
    char query[64] = {};
    char action[24] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "action", action, sizeof(action));
    }
    set_json_no_cache(req);
    if (strcmp(action, "restart") == 0) {
        if (req->method != HTTP_POST) {
            return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"WiFi 重连需要 POST\"}");
        }
        esp_err_t err = idf_wifi_reconnect();
        if (err == ESP_OK) idf_log_line("网页触发 WiFi 重连");
        std::string msg = err == ESP_OK
            ? "{\"success\":true,\"message\":\"已触发 WiFi 重连\"}"
            : "{\"success\":false,\"message\":\"WiFi 尚未启动\"}";
        return httpd_resp_send(req, msg.c_str(), msg.size());
    }
    IdfWifiStatus wifi = idf_wifi_get_status();
    std::string body = "{\"success\":true,\"connected\":";
    body += wifi.staConnected ? "true" : "false";
    body += ",\"apMode\":";
    body += wifi.apMode ? "true" : "false";
    body += "}";
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_wifi_scan(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    set_json_no_cache(req);
    std::string body;
    esp_err_t err = idf_wifi_scan_json(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    return httpd_resp_send(req, body.c_str(), body.size());
}

static bool parse_multipart_boundary(const char* content_type, std::string& marker)
{
    const char* p = strstr(content_type, "boundary=");
    if (!p) return false;
    p += strlen("boundary=");
    std::string boundary = p;
    size_t semi = boundary.find(';');
    if (semi != std::string::npos) boundary.resize(semi);
    if (!boundary.empty() && boundary.front() == '"') boundary.erase(0, 1);
    if (!boundary.empty() && boundary.back() == '"') boundary.pop_back();
    if (boundary.empty() || boundary.size() > 80) return false;
    marker = "\r\n--";
    marker += boundary;
    return true;
}

static bool is_hex_sha256(const std::string& value)
{
    if (value.size() != 64) return false;
    for (char ch : value) {
        if (!isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

static std::string hex_lower(const unsigned char* data, size_t len)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0F]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

static esp_err_t handle_ota_update(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;

    std::string expected_sha256;
    get_query_param(req, "sha256", expected_sha256, 128);
    if (!expected_sha256.empty() && !is_hex_sha256(expected_sha256)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SHA-256");
        return ESP_OK;
    }
    for (char& ch : expected_sha256) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }

    char ctype[160] = {};
    std::string boundary_marker;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK ||
        !parse_multipart_boundary(ctype, boundary_marker)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing multipart boundary");
        return ESP_OK;
    }

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_OK;
    }
    // multipart 会比固件本体多一些头/边界；明显超过分区的请求提前拒绝，
    // 避免错误上传长时间占用唯一的 httpd 工作线程和 OTA 句柄。
    constexpr size_t OTA_MULTIPART_OVERHEAD_MAX = 4096;
    if (req->content_len == 0 ||
        req->content_len > static_cast<size_t>(part->size) + OTA_MULTIPART_OVERHEAD_MAX) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Firmware too large");
        return ESP_OK;
    }

    esp_ota_handle_t ota = 0;
    // 顺序擦除写入：整分区(1.9MB)预擦除会卡住 httpd 任务好几秒，浏览器端表现为长时间无响应
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }

    std::string pending;
    pending.reserve(boundary_marker.size() + 1024);
    bool in_file = false;
    bool saw_boundary = false;
    bool sha_mismatch = false;
    size_t received = 0;
    size_t written = 0;
    char buf[1024];
    const size_t keep_tail = boundary_marker.size() + 8;
    mbedtls_sha256_context sha_ctx;
    bool sha_active = !expected_sha256.empty();
    if (sha_active) {
        mbedtls_sha256_init(&sha_ctx);
        mbedtls_sha256_starts(&sha_ctx, 0);
    }

    int timeouts = 0;
    // 涓流上传防护：连续超时计数外再加总时长硬上限。局域网正常 OTA 只要几十秒，
    // 5 分钟兜底既容忍慢 WiFi，也保证 Web 不会被拖死一整天
    const TickType_t ota_start_tick = xTaskGetTickCount();
    const TickType_t ota_hard_span = pdMS_TO_TICKS(300000);
    while (received < req->content_len && err == ESP_OK) {
        if (static_cast<TickType_t>(xTaskGetTickCount() - ota_start_tick) >= ota_hard_span) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        int got = httpd_req_recv(req, buf, std::min(sizeof(buf), static_cast<size_t>(req->content_len - received)));
        if (got <= 0) {
            // 上传中断的客户端不能无限重试，否则 OTA 句柄一直占着、Web 全站失去响应
            if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        timeouts = 0;
        received += static_cast<size_t>(got);
        pending.append(buf, got);

        if (!in_file) {
            size_t header_end = pending.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                if (pending.size() > 2048) err = ESP_ERR_INVALID_RESPONSE;
                continue;
            }
            pending.erase(0, header_end + 4);
            in_file = true;
        }

        while (pending.size() > keep_tail && err == ESP_OK) {
            size_t writable = pending.size() - keep_tail;
            if (written + writable > static_cast<size_t>(part->size)) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            err = esp_ota_write(ota, pending.data(), writable);
            if (err == ESP_OK) {
                if (sha_active) {
                    mbedtls_sha256_update(&sha_ctx,
                                          reinterpret_cast<const unsigned char*>(pending.data()),
                                          writable);
                }
                written += writable;
                pending.erase(0, writable);
            }
        }
    }

    if (err == ESP_OK) {
        size_t boundary = pending.find(boundary_marker);
        if (boundary == std::string::npos) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            if (boundary > 0) {
                if (written + boundary > static_cast<size_t>(part->size)) {
                    err = ESP_ERR_INVALID_SIZE;
                } else {
                    err = esp_ota_write(ota, pending.data(), boundary);
                }
                if (err == ESP_OK) {
                    if (sha_active) {
                        mbedtls_sha256_update(&sha_ctx,
                                              reinterpret_cast<const unsigned char*>(pending.data()),
                                              boundary);
                    }
                    written += boundary;
                }
            }
            saw_boundary = true;
        }
    }

    if (err == ESP_OK && (!in_file || !saw_boundary || written == 0)) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && sha_active) {
        unsigned char digest[32] = {};
        mbedtls_sha256_finish(&sha_ctx, digest);
        std::string actual = hex_lower(digest, sizeof(digest));
        if (actual != expected_sha256) {
            sha_mismatch = true;
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (sha_active) mbedtls_sha256_free(&sha_ctx);
    if (err == ESP_OK) err = esp_ota_end(ota);
    else esp_ota_abort(ota);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);

    set_json_no_cache(req);
    if (err != ESP_OK) {
        std::string body = "{\"success\":false,";
        json_prop(body, "message", sha_mismatch ? "OTA SHA-256 校验失败，已拒绝启用该固件"
                                                : std::string("升级失败: ") + esp_err_to_name(err));
        body += "}";
        if (sha_mismatch) idf_log_line("OTA SHA-256 校验失败，已拒绝启用该固件");
        else idf_logf("OTA 失败: %s", esp_err_to_name(err));
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    idf_logf("OTA 完成: %u 字节，准备重启", static_cast<unsigned>(written));
    esp_err_t send_err = httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"升级成功，设备重启中\"}");
    schedule_restart_or_now("ota_restart");
    return send_err;
}

static esp_err_t handle_send_sms(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    std::string raw;
    if (read_body(req, raw, 2048) != ESP_OK) return ESP_OK;
    IdfFormFields fields = parse_urlencoded(raw);
    const std::string* phone = find_field(fields, "phone");
    const std::string* content = find_field(fields, "content");
    std::string msg;
    esp_err_t err = idf_sms_enqueue_outgoing(phone ? *phone : std::string(),
                                             content ? *content : std::string(),
                                             msg);
    std::string body = "{\"success\":";
    body += (err == ESP_OK ? "true" : "false");
    body += ",\"queued\":";
    body += (err == ESP_OK ? "true" : "false");
    body += ",";
    json_prop(body, "message", msg);
    body += "}";
    set_json_no_cache(req);
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_messages(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    char query[96] = {};
    char box_raw[16] = {};
    char limit_raw[16] = {};
    bool sent_box = false;
    int limit = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "box", box_raw, sizeof(box_raw)) == ESP_OK &&
            strcmp(box_raw, "sent") == 0) {
            sent_box = true;
        }
        if (httpd_query_key_value(query, "limit", limit_raw, sizeof(limit_raw)) == ESP_OK) {
            int parsed_limit = 0;
            if (parse_int_strict(limit_raw, parsed_limit)) limit = parsed_limit;
        }
    }
    set_json_no_cache(req);
    std::string body = idf_inbox_json(sent_box, limit);
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_empty_log(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    set_json_no_cache(req);
    char query[64] = {};
    char since_raw[24] = {};
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", since_raw, sizeof(since_raw)) == ESP_OK) {
        parse_u32_strict(since_raw, since, true);
    }
    std::string body = idf_log_json_since(since);
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_log_download(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    set_no_cache_headers(req);
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=sms_idf_log.txt");
    std::string body = idf_log_text_dump();
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_prev_log(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    set_no_cache_headers(req);
    char query[32] = {};
    char dl_raw[8] = {};
    // ?dl=1 时按附件下载，否则浏览器内直接查看
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "dl", dl_raw, sizeof(dl_raw)) == ESP_OK &&
        strcmp(dl_raw, "1") == 0) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=sms_idf_prev_log.txt");
    }
    std::string body = idf_log_prev_dump();
    if (body.empty()) {
        body = "（暂无上次运行日志：设备可能刚上电，且没有已保存的异常复位日志）";
    }
    return httpd_resp_send(req, body.c_str(), body.size());
}

// 下载崩溃转储原始镜像，可在电脑上用
// espcoredump.py info_corefile --core coredump.bin --core-format raw 解析完整回溯
static esp_err_t handle_coredump_download(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    size_t addr = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        set_no_cache_headers(req);
        return httpd_resp_sendstr(req, "（当前没有崩溃转储：设备上次未发生崩溃，或转储尚未写入）");
    }
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!part) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        set_no_cache_headers(req);
        return httpd_resp_sendstr(req, "（未找到 coredump 分区）");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    set_no_cache_headers(req);
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=coredump.bin");
    // 分块读发，避免一次性把整个转储(最大 64KB)搬进堆
    char chunk[1024];
    size_t offset = addr >= part->address ? addr - part->address : 0;
    size_t remaining = size;
    while (remaining > 0) {
        size_t n = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
        if (esp_partition_read(part, offset, chunk, n) != ESP_OK) {
            // 读失败时必须让传输明确失败(断开连接)：发终止分块会让浏览器
            // 把截断文件当完整下载，espcoredump.py 解析时才莫名报错
            idf_logf("崩溃转储读取失败(offset=%u)，已中断下载", static_cast<unsigned>(offset));
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) return ESP_FAIL;
        offset += n;
        remaining -= n;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t handle_coredump_clear(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    set_json_no_cache(req);
    esp_err_t err = esp_core_dump_image_erase();
    if (err == ESP_OK) {
        idf_log_line("已清除崩溃转储");
        return httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"崩溃转储已清除\"}");
    }
    char body[128];
    snprintf(body, sizeof(body), "{\"success\":false,\"message\":\"清除失败: %s\"}", esp_err_to_name(err));
    return httpd_resp_send(req, body, strlen(body));
}

static bool query_u32(httpd_req_t* req, const char* key, uint32_t& value)
{
    char query[96] = {};
    char raw[24] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    return parse_u32_strict(raw, value, false);
}

static bool query_u8_range(httpd_req_t* req, const char* key, uint8_t max_exclusive, uint8_t& value)
{
    char query[96] = {};
    char raw[8] = {};
    if (max_exclusive == 0 ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    if (raw[0] == '\0') return false;
    unsigned parsed = 0;
    for (size_t i = 0; raw[i] != '\0'; ++i) {
        if (!isdigit(static_cast<unsigned char>(raw[i]))) return false;
        parsed = parsed * 10U + static_cast<unsigned>(raw[i] - '0');
        if (parsed >= max_exclusive) return false;
    }
    value = static_cast<uint8_t>(parsed);
    return true;
}

static esp_err_t handle_delete_message(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    uint32_t id = 0;
    bool ok = query_u32(req, "id", id) && idf_inbox_delete(id);
    set_json_no_cache(req);
    return httpd_resp_sendstr(req, ok
        ? "{\"success\":true,\"message\":\"已删除\"}"
        : "{\"success\":false,\"message\":\"未找到\"}");
}

static esp_err_t handle_resend_message(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    uint32_t id = 0;
    IdfInboxEntry entry;
    bool found = query_u32(req, "id", id) && idf_inbox_get_by_id(id, entry);
    set_json_no_cache(req);
    if (!found) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"未找到该短信\"}");
    }
    bool ok = idf_push_enqueue_forward(entry.sender.c_str(), entry.text.c_str(), entry.ts.c_str(), entry.id);
    idf_logf("网页手动重发短信 id=%u: %s", static_cast<unsigned>(id), ok ? "已入队" : "入队失败");
    return httpd_resp_sendstr(req, ok
        ? "{\"success\":true,\"message\":\"已重新入队转发\"}"
        : "{\"success\":false,\"message\":\"转发队列繁忙，请稍后重试\"}");
}

static esp_err_t handle_test_push(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!ensure_get_or_post(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    uint8_t ch = 0;
    bool valid_channel = query_u8_range(req, "ch", IDF_MAX_PUSH_CHANNELS, ch);
    set_json_no_cache(req);
    if (!valid_channel) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"queued\":false,\"running\":false,\"message\":\"通道序号无效\"}");
    }
    if (action == "status") {
        std::string body = idf_push_test_status_json(ch);
        return httpd_resp_send(req, body.c_str(), body.size());
    }
    if (req->method != HTTP_POST) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"queued\":false,\"running\":false,\"message\":\"测试推送需使用 POST\"}");
    }

    std::string msg;
    bool ok = idf_push_enqueue_test(ch, msg);
    std::string body = "{\"success\":";
    body += ok ? "true" : "false";
    body += ",\"queued\":";
    body += ok ? "true" : "false";
    body += ",";
    json_prop(body, "message", msg);
    body += "}";
    return httpd_resp_send(req, body.c_str(), body.size());
}

static std::string trim_copy(std::string value)
{
    size_t start = 0;
    while (start < value.size() && isspace(static_cast<unsigned char>(value[start]))) ++start;
    size_t end = value.size();
    while (end > start && isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

static bool keepalive_url_valid(const std::string& raw_url, std::string& err)
{
    std::string url = trim_copy(raw_url);
    if (url.size() > 240) {
        err = "URL过长";
        return false;
    }
    if (!(url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)) {
        err = "URL需要以 http:// 或 https:// 开头";
        return false;
    }
    if (url.find('"') != std::string::npos || url.find(' ') != std::string::npos) {
        err = "URL包含非法字符";
        return false;
    }
    return true;
}

static bool cell_job_lock(TickType_t ticks)
{
    return s_cell_job_mutex && xSemaphoreTake(s_cell_job_mutex, ticks) == pdTRUE;
}

static void cell_job_unlock(void)
{
    xSemaphoreGive(s_cell_job_mutex);
}

static bool cellular_job_active_locked(void)
{
    return s_ping_job.running || s_ping_job.queued ||
           s_keepalive_job.running || s_keepalive_job.queued ||
           s_esim_job.running || s_esim_job.queued ||
           s_sched_job.running || s_sched_job.queued ||
           s_modem_apply_running ||
           s_web_modem_action_running;
}

static std::string cellular_http_message(const IdfCellularHttpResult& result)
{
    char buf[192];
    if (result.ok) {
        snprintf(buf, sizeof(buf), "HTTP %d，已通过蜂窝下载约 %uKB payload",
                 result.httpStatus, static_cast<unsigned>(result.bytesRead / 1024UL));
    } else if (result.httpStatus >= 0) {
        snprintf(buf, sizeof(buf), "%s(HTTP %d，%uKB)",
                 result.message.empty() ? "蜂窝HTTP payload 下载失败" : result.message.c_str(),
                 result.httpStatus, static_cast<unsigned>(result.bytesRead / 1024UL));
    } else {
        snprintf(buf, sizeof(buf), "%s",
                 result.message.empty() ? "蜂窝HTTP payload 下载失败" : result.message.c_str());
    }
    return std::string(buf);
}

static IdfCellularHttpConfig cellular_http_config(bool data_enabled, const std::string& apn)
{
    IdfCellularHttpConfig cfg;
    cfg.dataEnabled = data_enabled;
    cfg.apn = apn;
    return cfg;
}

struct PingTaskArg {
    std::string url;
    IdfCellularHttpConfig cellular;
};

struct EsimTaskArg {
    std::string action;
    std::string identifier;
    std::string nickname;
};

static std::string esim_action_label(const std::string& action)
{
    if (action == "refresh") return "刷新 eSIM Profile";
    if (action == "info") return "查询 eSIM 信息";
    if (action == "enable") return "启用 eSIM Profile";
    if (action == "disable") return "禁用 eSIM Profile";
    if (action == "delete") return "删除 eSIM Profile";
    if (action == "nickname") return "更新 eSIM 昵称";
    if (action == "switch") return "切换 eSIM Profile";
    return "eSIM 操作";
}

static void copy_esim_cache(std::string& eid,
                            std::vector<IdfEsimProfile>& profiles,
                            uint32_t& updated_at,
                            std::string& message)
{
    if (cell_job_lock()) {
        eid = s_esim_cache.eid;
        profiles = s_esim_cache.profiles;
        updated_at = s_esim_cache.updatedAt;
        message = s_esim_cache.message;
        cell_job_unlock();
    }
}

static void append_esim_profile_json(std::string& body, const IdfEsimProfile& p)
{
    body += "{";
    json_prop(body, "iccid", p.iccid); body += ",";
    json_prop(body, "isdpAid", p.isdpAid); body += ",";
    json_prop(body, "state", p.state); body += ",";
    json_prop(body, "nickname", p.nickname); body += ",";
    json_prop(body, "serviceProvider", p.serviceProvider); body += ",";
    json_prop(body, "profileName", p.profileName); body += ",";
    json_prop(body, "profileClass", p.profileClass);
    body += "}";
}

static void esim_task(void* arg_raw)
{
    EsimTaskArg* arg = static_cast<EsimTaskArg*>(arg_raw);
    std::string action = arg->action;
    std::string identifier = arg->identifier;
    std::string nickname = arg->nickname;
    delete arg;

    if (cell_job_lock()) {
        s_esim_job.queued = false;
        s_esim_job.running = true;
        s_esim_job.message = esim_action_label(action) + "执行中";
        cell_job_unlock();
    }

    std::string message;
    std::string eid;
    std::vector<IdfEsimProfile> profiles;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool cache_eid_only = false;
    if (action == "refresh") {
        err = idf_esim_list_profiles(profiles, eid, message);
    } else if (action == "info") {
        err = idf_esim_get_eid(eid, message);
        cache_eid_only = (err == ESP_OK);
    } else if (action == "enable") {
        err = idf_esim_enable_profile(identifier, true, message);
    } else if (action == "disable") {
        err = idf_esim_disable_profile(identifier, true, message);
    } else if (action == "delete") {
        err = idf_esim_delete_profile(identifier, message);
    } else if (action == "nickname") {
        err = idf_esim_set_nickname(identifier, nickname, message);
    } else if (action == "switch") {
        err = idf_esim_switch_profile(identifier, true, message);
    } else {
        message = "未知 eSIM 操作";
    }

    bool ok = (err == ESP_OK);
    // 启用/切换/禁用改变当前生效的卡：让模组重读号码/ICCID/运营商，避免概览沿用旧卡缓存
    if (ok && (action == "enable" || action == "switch" || action == "disable")) {
        idf_modem_invalidate_sim_identity();
    }
    bool cache_ready = false;
    if (ok && action != "refresh" && action != "info") {
        std::string refresh_msg;
        std::vector<IdfEsimProfile> refreshed;
        std::string refreshed_eid;
        esp_err_t refresh_err = idf_esim_list_profiles(refreshed, refreshed_eid, refresh_msg);
        if (refresh_err == ESP_OK) {
            profiles = std::move(refreshed);
            eid = std::move(refreshed_eid);
            cache_ready = true;
        } else {
            message += "；操作后刷新列表失败: " + refresh_msg;
        }
    } else if (ok) {
        cache_ready = true;
    }

    std::string final_message = message.empty()
        ? (ok ? "eSIM 操作已完成" : "eSIM 操作失败")
        : message;
    if (cell_job_lock(portMAX_DELAY)) {
        if (cache_eid_only) {
            s_esim_cache.eid = eid;
            s_esim_cache.updatedAt = static_cast<uint32_t>(time(nullptr));
            s_esim_cache.message = message;
        } else if (cache_ready) {
            s_esim_cache.eid = eid;
            s_esim_cache.profiles = profiles;
            s_esim_cache.updatedAt = static_cast<uint32_t>(time(nullptr));
            s_esim_cache.message = message;
        } else if (ok) {
            // 启用/禁用/删除成功但操作后刷新失败(REFRESH 复位期间常见)：
            // 清掉旧列表而不是留着过期状态误导用户重复操作
            s_esim_cache.profiles.clear();
            s_esim_cache.updatedAt = static_cast<uint32_t>(time(nullptr));
            s_esim_cache.message = "操作已完成，列表待刷新";
        }
        s_esim_job.running = false;
        s_esim_job.queued = false;
        s_esim_job.done = true;
        s_esim_job.success = ok;
        s_esim_job.message = final_message;
        cell_job_unlock();
    }
    idf_logf("%s: %s", esim_action_label(action).c_str(), ok ? "完成" : final_message.c_str());
    vTaskDelete(nullptr);
}

static bool start_esim_job(const std::string& action,
                           const std::string& identifier,
                           const std::string& nickname,
                           std::string& message,
                           bool& already_running)
{
    already_running = false;
    if (!cell_job_lock()) {
        message = "eSIM 任务状态锁繁忙";
        return false;
    }
    if (cellular_job_active_locked()) {
        already_running = true;
        message = "已有蜂窝/eSIM 任务正在后台执行";
        cell_job_unlock();
        return false;
    }
    s_esim_job = WebAsyncJob();
    s_esim_job.queued = true;
    s_esim_job.done = false;
    s_esim_job.success = false;
    s_esim_job.action = action;
    s_esim_job.message = esim_action_label(action) + "已排队";
    cell_job_unlock();

    EsimTaskArg* arg = new (std::nothrow) EsimTaskArg();
    if (!arg) {
        if (cell_job_lock(portMAX_DELAY)) {
            s_esim_job.queued = false;
            s_esim_job.done = true;
            s_esim_job.success = false;
            s_esim_job.message = "创建 eSIM 任务失败：内存不足";
            cell_job_unlock();
        }
        message = "创建 eSIM 任务失败：内存不足";
        return false;
    }
    arg->action = action;
    arg->identifier = identifier;
    arg->nickname = nickname;
    if (xTaskCreate(esim_task, "idf_esim", 8192, arg, 3, nullptr) != pdPASS) {
        delete arg;
        if (cell_job_lock(portMAX_DELAY)) {
            s_esim_job.queued = false;
            s_esim_job.done = true;
            s_esim_job.success = false;
            s_esim_job.message = "创建 eSIM 任务失败";
            cell_job_unlock();
        }
        message = "创建 eSIM 任务失败";
        return false;
    }
    message = esim_action_label(action) + "已排队";
    idf_logf("%s已入队", esim_action_label(action).c_str());
    return true;
}

static void ping_task(void* arg_raw)
{
    PingTaskArg* arg = static_cast<PingTaskArg*>(arg_raw);
    if (cell_job_lock()) {
        s_ping_job.queued = false;
        s_ping_job.running = true;
        s_ping_job.message = "后台HTTP payload 下载中";
        cell_job_unlock();
    }

    IdfCellularHttpResult result;
    esp_err_t err = idf_modem_cellular_http_get(arg->url, arg->cellular, result);
    bool ok = (err == ESP_OK && result.ok);
    std::string message = cellular_http_message(result);

    // 终态写入用无限等锁，理由同 keepalive_task：状态卡在 running 会永久拒绝后续任务
    if (cell_job_lock(portMAX_DELAY)) {
        s_ping_job.running = false;
        s_ping_job.queued = false;
        s_ping_job.done = true;
        s_ping_job.success = ok;
        s_ping_job.message = message;
        cell_job_unlock();
    }
    delete arg;
    vTaskDelete(nullptr);
}

static bool valid_ussd_code(const std::string& code)
{
    if (code.empty() || code.size() > 24) return false;
    for (char ch : code) {
        if (!(isdigit(static_cast<unsigned char>(ch)) || ch == '*' || ch == '#')) return false;
    }
    return true;
}

struct KeepAliveTaskArg {
    IdfKeepaliveRunView config;
};

static std::string keepalive_profile_note(const IdfKeepaliveRunView& cfg)
{
    if (cfg.kaProfile.empty()) return std::string();
    return std::string("目标 eSIM: ") + idf_esim_mask_profile_id(cfg.kaProfile);
}

static void enqueue_maintenance_notice(int tz_offset_min, bool email_enabled, const char* title,
                                       const std::string& body, uint32_t now)
{
    std::string ts = format_epoch_local(now, tz_offset_min);
    int pushed = idf_push_enqueue_notify(title, body.c_str(), ts.c_str());
    if (pushed > 0) idf_logf("%s推送已入队: %d 个通道", title, pushed);
    else idf_logf("%s无有效推送通道", title);

    if (!email_enabled) return;
    idf_push_enqueue_email(title, body.c_str());
}

static void keepalive_set_job_message(const std::string& message)
{
    if (cell_job_lock()) {
        s_keepalive_job.message = message;
        cell_job_unlock();
    }
}

static bool cereg_registered(const std::string& resp)
{
    std::string line = first_line_containing(resp, "+CEREG:");
    const char* p = strchr(line.c_str(), ':');
    if (!p) return false;
    std::string rest = p + 1;
    size_t comma = rest.find(',');
    int first = -1;
    if (!parse_int_strict(rest.substr(0, comma), first)) return false;

    int stat = first;
    if (comma != std::string::npos) {
        size_t second_end = rest.find(',', comma + 1);
        std::string second = rest.substr(
            comma + 1, second_end == std::string::npos ? std::string::npos : second_end - comma - 1);
        int second_value = -1;
        // 查询响应是 +CEREG: <n>,<stat>；URC 是 +CEREG: <stat>[,<tac>,...]。
        // URC 的第二字段可能是带引号的 TAC，不能把解析失败当作整行无效。
        if (parse_int_strict(second, second_value)) stat = second_value;
    }
    return stat == 1 || stat == 5;
}

static bool wait_registered_for(uint32_t timeout_ms)
{
    uint64_t deadline = esp_timer_get_time() + static_cast<uint64_t>(timeout_ms) * 1000ULL;
    while (esp_timer_get_time() < deadline) {
        std::string resp;
        if (idf_modem_send_at("AT+CEREG?", 3000, resp) == ESP_OK && cereg_registered(resp)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    return false;
}

static bool wait_registered_after_esim_switch(std::string& message)
{
    if (wait_registered_for(60000)) {
        message = "eSIM 切换后网络已注册";
        return true;
    }
    // REFRESH 后部分模组不会自动重新附着，重启模组栈再等一轮。
    idf_logf("eSIM 切换后 60s 未注册，重启模组重新附着");
    if (idf_modem_request_reset(false) == ESP_OK && wait_registered_for(90000)) {
        message = "eSIM 切换后经模组重启已注册";
        return true;
    }
    message = "eSIM 已切换，但等待网络注册超时";
    return false;
}

// 切卡记录：任务前切到目标 Profile，任务后按需切回原 Profile
struct EsimJobSwitch {
    bool switched = false;
    std::string original;  // 执行前启用的 Profile ICCID（空=未知，无法切回）
};

// profile 非空时切换到目标 Profile 并等待网络注册；用同一份列表同时确定
// "当前启用的卡"与"目标卡"，避免两次读卡。目标已启用则不动作。
static bool esim_prepare_profile(const std::string& profile,
                                 EsimJobSwitch& sw,
                                 std::string& message)
{
    sw = EsimJobSwitch();
    if (profile.empty()) return true;

    std::vector<IdfEsimProfile> profiles;
    std::string eid;
    std::string list_msg;
    if (idf_esim_list_profiles(profiles, eid, list_msg) != ESP_OK) {
        message = "读取 eSIM Profile 列表失败: " + list_msg;
        return false;
    }
    const IdfEsimProfile* target = nullptr;
    for (const IdfEsimProfile& p : profiles) {
        if (p.state == "enabled" && sw.original.empty()) sw.original = p.iccid;
        if (!target && idf_esim_profile_matches(p, profile)) target = &p;
    }
    std::string enable_id;
    if (target) {
        if (target->state == "enabled") {
            message = "目标 eSIM Profile 已在使用: " + idf_esim_mask_profile_id(target->iccid);
            return true;
        }
        // 部分卡的列表条目可能缺 ICCID(5A)，退回 ISD-P AID 启用
        enable_id = target->iccid.empty() ? target->isdpAid : target->iccid;
        if (enable_id.empty()) {
            message = "目标 Profile 缺少 ICCID/AID，无法切换";
            return false;
        }
    } else {
        // 列表匹配不到时按原始输入直接尝试(与 /esim 手动切换一致)：
        // 某些卡列表字段读不全，但 ICCID/AID 直接启用仍有效
        enable_id = profile;
    }

    idf_logf("任务准备切换 eSIM: %s", idf_esim_mask_profile_id(enable_id).c_str());
    std::string switch_msg;
    if (idf_esim_enable_profile(enable_id, true, switch_msg) != ESP_OK) {
        message = (target ? "eSIM 切换失败: " : "未找到目标 Profile 且直接启用失败: ") + switch_msg;
        return false;
    }
    sw.switched = true;
    std::string wait_msg;
    if (!wait_registered_after_esim_switch(wait_msg)) {
        message = wait_msg;
        return false;  // sw.switched 保持 true，调用方仍应尝试切回
    }
    std::string display_id = target ? (target->iccid.empty() ? enable_id : target->iccid) : enable_id;
    message = "已切换到 " + idf_esim_mask_profile_id(display_id) + "；" + wait_msg;
    return true;
}

static void esim_restore_profile(const EsimJobSwitch& sw, std::string& message)
{
    if (!sw.switched) return;
    if (sw.original.empty()) {
        message = "原 Profile 未知，保持在目标 Profile";
        return;
    }
    std::string masked = idf_esim_mask_profile_id(sw.original);
    idf_logf("任务完成，切回原 eSIM: %s", masked.c_str());
    std::string msg;
    if (idf_esim_enable_profile(sw.original, true, msg) != ESP_OK) {
        message = "切回原 Profile 失败: " + msg;
        return;
    }
    std::string wait_msg;
    wait_registered_after_esim_switch(wait_msg);
    message = "已切回原 Profile " + masked + "；" + wait_msg;
}

static bool keepalive_prepare_esim(const IdfKeepaliveRunView& cfg, EsimJobSwitch& sw, std::string& message)
{
    if (cfg.kaProfile.empty()) return true;
    keepalive_set_job_message("保号动作执行中，正在切换 eSIM: " + idf_esim_mask_profile_id(cfg.kaProfile));
    return esim_prepare_profile(cfg.kaProfile, sw, message);
}

static void keepalive_task(void* arg_raw)
{
    KeepAliveTaskArg* arg = static_cast<KeepAliveTaskArg*>(arg_raw);
    IdfKeepaliveRunView cfg = std::move(arg->config);
    delete arg;

    if (cell_job_lock()) {
        s_keepalive_job.queued = false;
        s_keepalive_job.running = true;
        std::string note = keepalive_profile_note(cfg);
        s_keepalive_job.message = note.empty() ? "保号动作执行中" : std::string("保号动作执行中，") + note;
        cell_job_unlock();
    }

    bool ok = false;
    std::string message;
    std::string profile_note = keepalive_profile_note(cfg);
    if (!profile_note.empty()) idf_logf("保号任务%s", profile_note.c_str());
    EsimJobSwitch esim_switch;
    bool esim_ready = keepalive_prepare_esim(cfg, esim_switch, message);
    if (!esim_ready) {
        ok = false;
    } else if (cfg.kaAction == 2) {
        if (cfg.kaTarget.empty()) {
            message = "保号短信目标号码为空";
        } else {
            std::string prefix = message;
            std::string sms_msg;
            esp_err_t err = idf_sms_send_text(cfg.kaTarget, "keepalive", sms_msg);
            ok = (err == ESP_OK);
            message = prefix.empty() ? sms_msg : (prefix + "；" + sms_msg);
        }
    } else if (cfg.kaAction == 3) {
        if (!valid_ussd_code(cfg.kaTarget)) {
            message = "USSD 码为空或包含非法字符";
        } else {
            std::string prefix = message.empty() ? std::string() : (message + "；");
            std::string cmd = "AT+CUSD=1,\"" + cfg.kaTarget + "\",15";
            std::string resp;
            esp_err_t err = idf_modem_send_at_until(cmd, "+CUSD:", 20000, resp);
            ok = (err == ESP_OK && resp.find("+CUSD:") != std::string::npos);
            message = prefix + (resp.empty() ? std::string(esp_err_to_name(err)) : resp);
        }
    } else {
        IdfCellularHttpResult result;
        std::string url = cfg.kaUrl.empty() ? std::string(IDF_KEEPALIVE_DEFAULT_URL) : cfg.kaUrl;
        esp_err_t err = idf_modem_cellular_http_get(url, cellular_http_config(cfg.dataEnabled, cfg.apn), result);
        ok = (err == ESP_OK && result.ok);
        message = message.empty() ? cellular_http_message(result) : (message + "；" + cellular_http_message(result));
    }
    // 保号完成后切回执行前启用的 Profile：设备主业是主卡收短信转发
    if (esim_switch.switched) {
        keepalive_set_job_message("保号动作完成，正在切回原 eSIM Profile");
        std::string back_msg;
        esim_restore_profile(esim_switch, back_msg);
        if (!back_msg.empty()) message += "；" + back_msg;
    }

    if (!profile_note.empty()) {
        message = message.empty() ? profile_note : (profile_note + "；" + message);
    }

    if (ok) {
        uint32_t now = static_cast<uint32_t>(time(nullptr));
        if (now >= 1700000000u && idf_config_set_keepalive_last(now) == ESP_OK) {
            message += "，已更新保号基准日";
        }
        idf_log_line("保号动作成功");
        std::string notice = "保号动作已成功执行。\n方式: ";
        notice += (cfg.kaAction == 2 ? "发送短信" : (cfg.kaAction == 3 ? "USSD 查询" : "蜂窝数据流量"));
        notice += "\n结果: " + message;
        enqueue_maintenance_notice(cfg.tzOffsetMin, cfg.emailEnabled, "保号动作已执行", notice, now);
    } else {
        idf_logf("保号动作失败: %s", message.c_str());
        // 带切卡的保号失败改为次日重试：每小时重试意味着反复切卡+模组重启，
        // 主卡每小时掉线数分钟，比错过一次保号代价更大（普通保号仍每小时重试）
        uint32_t now = static_cast<uint32_t>(time(nullptr));
        if (!cfg.kaProfile.empty() && epoch_valid(now) && cfg.kaIntervalDays > 0) {
            uint64_t back = static_cast<uint64_t>(cfg.kaIntervalDays - 1) * 86400ULL;
            uint32_t base = back < now ? now - static_cast<uint32_t>(back) : now;
            if (idf_config_set_keepalive_last(base) == ESP_OK) {
                idf_log_line("保号失败已退避，次日重试");
            }
        }
    }

    // 终态必须写进去：这里拿不到锁就放弃的话，任务状态永远停在 running，
    // 后续所有保号/诊断请求都会被"已有任务在执行"拒绝，直到重启
    if (cell_job_lock(portMAX_DELAY)) {
        s_keepalive_job.running = false;
        s_keepalive_job.queued = false;
        s_keepalive_job.done = true;
        s_keepalive_job.success = ok;
        s_keepalive_job.message = ok ? (message.empty() ? "保号动作已完成" : message)
                                     : (message.empty() ? "保号动作失败，请查看日志" : message);
        cell_job_unlock();
    }
    vTaskDelete(nullptr);
}

static bool start_keepalive_job(const IdfKeepaliveRunView& cfg, const char* queued_message,
                                std::string& message, bool& already_running)
{
    already_running = false;
    if (!cell_job_lock()) {
        message = "保号任务状态锁繁忙";
        return false;
    }
    if (cellular_job_active_locked()) {
        already_running = true;
        message = "已有蜂窝/保号任务正在后台执行";
        cell_job_unlock();
        return false;
    }
    s_keepalive_job = WebAsyncJob();
    s_keepalive_job.queued = true;
    s_keepalive_job.done = false;
    s_keepalive_job.success = false;
    std::string queued = queued_message ? queued_message : "";
    std::string note = keepalive_profile_note(cfg);
    if (!note.empty()) queued += "，" + note;
    s_keepalive_job.message = queued;
    cell_job_unlock();

    KeepAliveTaskArg* arg = new (std::nothrow) KeepAliveTaskArg();
    if (!arg) {
        if (cell_job_lock(portMAX_DELAY)) {
            s_keepalive_job.queued = false;
            s_keepalive_job.done = true;
            s_keepalive_job.success = false;
            s_keepalive_job.message = "创建保号任务失败：内存不足";
            cell_job_unlock();
        }
        message = "创建保号任务失败：内存不足";
        return false;
    }
    arg->config = cfg;
    // 8192：kaProfile 路径会走完整 eSIM 列表/切换/TLV 解析链，6144 余量不足
    if (xTaskCreate(keepalive_task, "idf_keepalive", 8192, arg, 3, nullptr) != pdPASS) {
        delete arg;
        if (cell_job_lock(portMAX_DELAY)) {
            s_keepalive_job.queued = false;
            s_keepalive_job.done = true;
            s_keepalive_job.success = false;
            s_keepalive_job.message = "创建保号任务失败";
            cell_job_unlock();
        }
        message = "创建保号任务失败";
        return false;
    }
    message = queued;
    idf_log_line("保号动作已入队");
    return true;
}

static bool keepalive_due(uint32_t last_ts, uint32_t now, uint32_t interval_days)
{
    if (!epoch_valid(now) || interval_days == 0) return false;
    if (!epoch_valid(last_ts)) return true;
    if (last_ts > now) return false;  // NTP 回拨后 now-last 会下溢成"立即到期"
    return (now - last_ts) >= interval_days * 86400u;
}

static bool system_idle_for_maintenance()
{
    return !idf_push_busy() &&
           idf_push_forward_queue_depth() == 0 &&
           idf_push_retry_queue_depth() == 0 &&
           idf_sms_outgoing_queue_depth() == 0 &&
           idf_push_email_queue_depth() == 0 &&
           !cellular_job_active();
}

static bool cellular_job_active()
{
    // 拿不到锁按"忙"处理(fail-safe)：该函数用于维护性重启前的空闲判断，
    // 误判空闲会在 eSIM 切换/保号中途 esp_restart，设备可能停在错误的卡上
    bool active = true;
    if (cell_job_lock()) {
        active = cellular_job_active_locked();
        cell_job_unlock();
    }
    return active;
}

// ========== 进阶定时任务：选卡→执行→切回 ==========

struct SchedTaskArg {
    IdfSchedRunView config;
    int index = 0;
};

static const char* sched_action_name(uint8_t action)
{
    switch (action) {
        case 0: return "推送提醒";
        case 1: return "蜂窝HTTP";
        case 2: return "发送短信";
        case 3: return "USSD 查询";
        default: return "未知动作";
    }
}

static std::string sched_task_label(const IdfSchedTask& t, int index)
{
    if (!t.name.empty()) return t.name;
    char buf[32];
    snprintf(buf, sizeof(buf), "定时任务%d", index + 1);
    return buf;
}

static void sched_set_job_message(const std::string& message)
{
    if (cell_job_lock()) {
        s_sched_job.message = message;
        cell_job_unlock();
    }
}

static bool sched_run_action(const IdfSchedRunView& cfg,
                             const IdfSchedTask& t,
                             const std::string& label,
                             std::string& message)
{
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    switch (t.action) {
        case 0: {  // 推送自定义提醒（走 WiFi，不依赖蜂窝）
            std::string body = t.payload.empty() ? ("定时提醒触发：" + label) : t.payload;
            std::string ts = format_epoch_local(now, cfg.tzOffsetMin);
            int pushed = idf_push_enqueue_notify(label.c_str(), body.c_str(), ts.c_str());
            bool email = cfg.emailEnabled && cfg.emailConfigured;
            if (email) idf_push_enqueue_email(label.c_str(), body.c_str());
            if (pushed > 0 || email) {
                char buf[96];
                snprintf(buf, sizeof(buf), "提醒已入队: %d 个推送通道%s", pushed, email ? " + 邮件" : "");
                message = buf;
                return true;
            }
            message = "没有可用的推送通道或邮件配置";
            return false;
        }
        case 1: {  // 蜂窝 HTTP 下载(ping)
            std::string url = t.target;
            if (url.empty()) url = cfg.kaUrl.empty() ? std::string(IDF_KEEPALIVE_DEFAULT_URL) : cfg.kaUrl;
            IdfCellularHttpResult result;
            esp_err_t err = idf_modem_cellular_http_get(url, cellular_http_config(cfg.dataEnabled, cfg.apn), result);
            message = cellular_http_message(result);
            return err == ESP_OK && result.ok;
        }
        case 2: {  // 发送短信
            if (t.target.empty()) {
                message = "短信目标号码为空";
                return false;
            }
            std::string sms_msg;
            esp_err_t err = idf_sms_send_text(t.target,
                                              t.payload.empty() ? std::string("scheduled task") : t.payload,
                                              sms_msg);
            message = sms_msg;
            return err == ESP_OK;
        }
        case 3: {  // USSD 查询
            if (!valid_ussd_code(t.target)) {
                message = "USSD 码为空或包含非法字符";
                return false;
            }
            std::string cmd = "AT+CUSD=1,\"" + t.target + "\",15";
            std::string resp;
            esp_err_t err = idf_modem_send_at_until(cmd, "+CUSD:", 20000, resp);
            message = resp.empty() ? std::string(esp_err_to_name(err)) : resp;
            return err == ESP_OK && resp.find("+CUSD:") != std::string::npos;
        }
        default:
            message = "未知动作类型";
            return false;
    }
}

static void sched_task_worker(void* arg_raw)
{
    SchedTaskArg* arg = static_cast<SchedTaskArg*>(arg_raw);
    IdfSchedRunView cfg = std::move(arg->config);
    int index = arg->index;
    delete arg;
    const IdfSchedTask& t = cfg.task;
    std::string label = sched_task_label(t, index);

    if (cell_job_lock()) {
        s_sched_job.queued = false;
        s_sched_job.running = true;
        s_sched_job.message = label + " 执行中";
        cell_job_unlock();
    }
    idf_logf("定时任务开始: %s(%s)", label.c_str(), sched_action_name(t.action));

    std::string message;
    bool ok = false;
    EsimJobSwitch sw;
    bool prep_ok = true;
    if (!t.profile.empty()) {
        sched_set_job_message(label + ": 正在切换 eSIM Profile");
        std::string prep_msg;
        prep_ok = esim_prepare_profile(t.profile, sw, prep_msg);
        if (!prep_msg.empty()) message = prep_msg;
    }
    if (prep_ok) {
        sched_set_job_message(label + ": 正在执行" + sched_action_name(t.action));
        std::string act_msg;
        ok = sched_run_action(cfg, t, label, act_msg);
        if (!act_msg.empty()) message = message.empty() ? act_msg : (message + "；" + act_msg);
    }

    if (sw.switched && t.switchBack) {
        sched_set_job_message(label + ": 正在切回原 eSIM Profile");
        std::string back_msg;
        esim_restore_profile(sw, back_msg);
        if (!back_msg.empty()) message += "；" + back_msg;
    }

    uint32_t now = static_cast<uint32_t>(time(nullptr));
    if (epoch_valid(now)) {
        if (ok) {
            idf_config_set_sched_last(index, now);
        } else {
            // 失败改为"明天重试"：每小时重试会反复切卡/发短信/跑流量，代价太高
            uint32_t days = t.intervalDays > 0 ? static_cast<uint32_t>(t.intervalDays) : 1u;
            uint64_t back = static_cast<uint64_t>(days - 1) * 86400ULL;
            uint32_t base = back < now ? now - static_cast<uint32_t>(back) : now;
            idf_config_set_sched_last(index, base);
        }
    }

    // 推送型任务成功时本身就是通知，不再重复推结果；其余情况推一次执行结果
    if (t.action != 0 || !ok) {
        std::string notice = "任务: " + label +
            "\n动作: " + std::string(sched_action_name(t.action)) +
            "\n结果: " + (message.empty() ? (ok ? "成功" : "失败") : message);
        enqueue_maintenance_notice(cfg.tzOffsetMin, cfg.emailEnabled,
                                   ok ? "定时任务已执行" : "定时任务失败", notice, now);
    }

    if (cell_job_lock(portMAX_DELAY)) {
        s_sched_job.running = false;
        s_sched_job.queued = false;
        s_sched_job.done = true;
        s_sched_job.success = ok;
        s_sched_job.message = message.empty() ? (ok ? "定时任务已完成" : "定时任务失败") : message;
        cell_job_unlock();
    }
    if (ok) idf_logf("定时任务完成: %s", label.c_str());
    else idf_logf("定时任务失败: %s: %s", label.c_str(), message.c_str());
    vTaskDelete(nullptr);
}

static bool start_sched_job(const IdfSchedRunView& cfg, int index, std::string& message, bool& already_running)
{
    already_running = false;
    if (index < 0 || index >= IDF_MAX_SCHED_TASKS) {
        message = "任务序号无效";
        return false;
    }
    if (!cfg.valid) {
        message = "任务配置不可用";
        return false;
    }
    if (!cell_job_lock()) {
        message = "任务状态锁繁忙";
        return false;
    }
    if (cellular_job_active_locked()) {
        already_running = true;
        message = "已有蜂窝/eSIM 任务正在后台执行";
        cell_job_unlock();
        return false;
    }
    s_sched_job = WebAsyncJob();
    s_sched_job.queued = true;
    s_sched_job.message = "定时任务已排队";
    s_sched_job_index = index;
    cell_job_unlock();

    SchedTaskArg* arg = new (std::nothrow) SchedTaskArg();
    if (!arg) {
        if (cell_job_lock(portMAX_DELAY)) {
            s_sched_job.queued = false;
            s_sched_job.done = true;
            s_sched_job.success = false;
            s_sched_job.message = "创建定时任务失败：内存不足";
            cell_job_unlock();
        }
        message = "创建定时任务失败：内存不足";
        return false;
    }
    arg->config = cfg;
    arg->index = index;
    if (xTaskCreate(sched_task_worker, "idf_schedtask", 8192, arg, 3, nullptr) != pdPASS) {
        delete arg;
        if (cell_job_lock(portMAX_DELAY)) {
            s_sched_job.queued = false;
            s_sched_job.done = true;
            s_sched_job.success = false;
            s_sched_job.message = "创建定时任务失败";
            cell_job_unlock();
        }
        message = "创建定时任务失败";
        return false;
    }
    message = "定时任务已排队";
    return true;
}

static void scheduler_task(void*)
{
    uint32_t last_ka_check_ms = 0;
    bool prev_ka_enabled = false;
    int64_t hb_last_day = -1;
    int64_t rb_last_day = -1;

    while (true) {
        // —— 低堆守护：无条件运行(5s 周期)。放在 NTP 门控里的话，配网模式/断网
        // 期间恰恰是最容易内存紧张的场景，却完全失去自愈能力 ——
        if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 20000U && system_idle_for_maintenance()) {
            idf_logf("空闲堆低于阈值(%u<20000)，准备有序重启",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
            vTaskDelay(pdMS_TO_TICKS(300));
            idf_modem_power_off_for_restart();
            esp_restart();
        }

        uint32_t now = static_cast<uint32_t>(time(nullptr));
        if (epoch_valid(now)) {
            IdfSchedulerView cfg = idf_config_get_scheduler_view();
            uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);

            // 保号刚被启用时立即检查一次（旧行为），否则按小时节拍
            if (cfg.kaEnabled && !prev_ka_enabled) last_ka_check_ms = 0;
            prev_ka_enabled = cfg.kaEnabled;
            if (last_ka_check_ms == 0 || now_ms - last_ka_check_ms >= 3600000UL) {
                last_ka_check_ms = now_ms;
                bool retry_due_soon = false;
                if (cfg.kaEnabled && !epoch_valid(cfg.kaLastTime)) {
                    // 首次启用/无基准日：只建立基准日，绝不立即执行——
                    // 否则刚开启保号就真发短信/USSD/跑流量，可能直接扣费
                    idf_config_set_keepalive_last(now);
                    idf_log_line("保号基准日已建立(首次启用不执行动作)");
                } else if (cfg.kaEnabled && cfg.kaIntervalDays > 0 &&
                           keepalive_due(cfg.kaLastTime, now, static_cast<uint32_t>(cfg.kaIntervalDays))) {
                    std::string msg;
                    bool already = false;
                    IdfKeepaliveRunView run_cfg = idf_config_get_keepalive_run_view();
                    bool still_due = run_cfg.kaEnabled && run_cfg.kaIntervalDays > 0 &&
                        keepalive_due(run_cfg.kaLastTime, now, static_cast<uint32_t>(run_cfg.kaIntervalDays));
                    if (still_due) {
                        if (start_keepalive_job(run_cfg, "定时保号动作已排队", msg, already)) {
                            idf_log_line("保号到期，触发动作");
                        } else {
                            // 到期但蜂窝/eSIM 互斥任务正忙或内存暂时不足，不要等整整一小时才重试
                            retry_due_soon = true;
                        }
                    }
                }
                for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) {
                    const IdfSchedTask& t = cfg.schedTasks[i];
                    if (!t.enabled || t.intervalDays <= 0) continue;
                    if (!epoch_valid(t.lastRun)) {
                        // 老配置/时间未同步时启用的任务：先建立基准日，不立即执行
                        idf_config_set_sched_last(i, now);
                        continue;
                    }
                    if (!keepalive_due(t.lastRun, now, static_cast<uint32_t>(t.intervalDays))) continue;
                    std::string msg;
                    bool already = false;
                    IdfSchedRunView run_cfg = idf_config_get_sched_run_view(i);
                    const IdfSchedTask& latest = run_cfg.task;
                    bool still_due = run_cfg.valid && latest.enabled && latest.intervalDays > 0 &&
                        epoch_valid(latest.lastRun) &&
                        keepalive_due(latest.lastRun, now, static_cast<uint32_t>(latest.intervalDays));
                    if (still_due) {
                        if (start_sched_job(run_cfg, i, msg, already)) {
                            idf_logf("定时任务%d到期，已排队执行", i + 1);
                        } else {
                            retry_due_soon = true;
                        }
                    }
                    break;  // 一轮只启动一个任务(蜂窝互斥)，其余下轮再查
                }
                if (retry_due_soon) {
                    // 下个 5s tick 再查到期任务，避免保号/定时任务因互斥忙而最多延后一小时
                    last_ka_check_ms = now_ms - 3595000UL;
                }
            }

            int64_t local = static_cast<int64_t>(now) + static_cast<int64_t>(cfg.tzOffsetMin) * 60LL;
            int hour = static_cast<int>((local / 3600LL) % 24LL);
            if (hour < 0) hour += 24;
            int64_t day = local / 86400LL;
            if (local < 0 && (local % 86400LL) != 0) --day;

            // 用 > 而非 !=：NTP 回拨跨过本地午夜会让 day 变小，!= 会当天重复触发
            if (cfg.hbEnabled && hour == cfg.hbHour && day > hb_last_day) {
                hb_last_day = day;
                IdfSmsStatus sms = idf_sms_get_status();
                char body[192];
                snprintf(body, sizeof(body), "设备运行正常。\n累计转发: %u 条\n空闲堆: %u KB",
                         static_cast<unsigned>(sms.total),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024U));
                enqueue_maintenance_notice(cfg.tzOffsetMin, cfg.emailEnabled, "设备每日心跳", body, now);
            }

            uint64_t uptime_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
            if (cfg.rebootEnabled && hour == cfg.rebootHour && day > rb_last_day &&
                uptime_ms >= 7200000ULL && system_idle_for_maintenance()) {
                rb_last_day = day;
                idf_log_line("每日定时重启...");
                vTaskDelay(pdMS_TO_TICKS(300));
                // 每日重启是无人值守设备的兜底自愈手段，必须连模组一起冷启动
                idf_modem_power_off_for_restart();
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static esp_err_t handle_ping(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!ensure_get_or_post(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    set_json_no_cache(req);

    if (action == "status") {
        WebAsyncJob job;
        if (cell_job_lock()) {
            job = s_ping_job;
            cell_job_unlock();
        }
        std::string body = "{\"running\":";
        body += job.running ? "true" : "false";
        body += ",\"done\":";
        body += job.done ? "true" : "false";
        body += ",\"success\":";
        body += job.success ? "true" : "false";
        body += ",";
        json_prop(body, "url", job.url);
        body += ",";
        json_prop(body, "message", job.message);
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"payload 下载需要 POST\"}");
    }

    std::string raw;
    if (read_body(req, raw, 512) != ESP_OK) return ESP_OK;
    IdfFormFields fields = parse_urlencoded(raw);
    const std::string* url_field = find_field(fields, "url");
    std::string url = trim_copy(url_field ? *url_field : std::string());
    IdfKeepaliveRunView current = idf_config_get_keepalive_run_view();
    if (url.empty()) url = current.kaUrl.empty() ? std::string(IDF_KEEPALIVE_DEFAULT_URL) : current.kaUrl;

    std::string err_msg;
    if (!keepalive_url_valid(url, err_msg)) {
        std::string body = "{\"success\":false,";
        json_prop(body, "message", err_msg);
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    if (!cell_job_lock()) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"蜂窝任务状态锁繁忙\"}");
    }
    if (cellular_job_active_locked()) {
        cell_job_unlock();
        return httpd_resp_sendstr(req, "{\"success\":false,\"running\":true,\"message\":\"已有蜂窝任务正在执行，请稍候\"}");
    }

    s_ping_job = WebAsyncJob();
    s_ping_job.running = true;
    s_ping_job.queued = true;
    s_ping_job.url = url;
    s_ping_job.message = "后台HTTP payload 下载中";
    cell_job_unlock();

    PingTaskArg* arg = new (std::nothrow) PingTaskArg();
    if (!arg) {
        // 终态必须写进去(portMAX_DELAY)：300ms 拿锁失败就放弃的话,
        // running 永远为 true，之后所有蜂窝任务被"任务正忙"挡到重启为止
        if (cell_job_lock(portMAX_DELAY)) {
            s_ping_job.running = false;
            s_ping_job.queued = false;
            s_ping_job.done = true;
            s_ping_job.success = false;
            s_ping_job.message = "创建蜂窝HTTP任务失败：内存不足";
            cell_job_unlock();
        }
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"创建任务失败：内存不足\"}");
    }
    arg->url = url;
    arg->cellular = cellular_http_config(current.dataEnabled, current.apn);
    if (xTaskCreate(ping_task, "idf_ping_http", 6144, arg, 3, nullptr) != pdPASS) {
        delete arg;
        if (cell_job_lock(portMAX_DELAY)) {
            s_ping_job.running = false;
            s_ping_job.queued = false;
            s_ping_job.done = true;
            s_ping_job.success = false;
            s_ping_job.message = "创建蜂窝HTTP任务失败";
            cell_job_unlock();
        }
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"创建任务失败\"}");
    }

    idf_logf("网页端发起后台HTTP payload 请求: %s", url.c_str());
    return httpd_resp_sendstr(req, "{\"success\":true,\"running\":true,\"message\":\"已开始后台HTTP payload下载，可继续刷新网页\"}");
}

static esp_err_t handle_esim(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!ensure_get_or_post(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    if (action.empty()) action = "status";
    set_json_no_cache(req);

    if (action == "status") {
        WebAsyncJob job;
        std::string eid;
        std::vector<IdfEsimProfile> profiles;
        uint32_t updated_at = 0;
        std::string cache_message;
        if (cell_job_lock()) {
            job = s_esim_job;
            cell_job_unlock();
        }
        copy_esim_cache(eid, profiles, updated_at, cache_message);
        IdfConfigStatusView cfg = idf_config_get_status_view();
        std::string body;
        body.reserve(760 + profiles.size() * 260);
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"jobQueued\":%s,\"jobRunning\":%s,\"jobDone\":%s,"
                 "\"jobSuccess\":%s,\"success\":%s,\"queued\":%s,"
                 "\"updatedAt\":%u,\"profileCount\":%u,",
                 job.queued ? "true" : "false",
                 job.running ? "true" : "false",
                 job.done ? "true" : "false",
                 job.success ? "true" : "false",
                 job.success ? "true" : "false",
                 (job.queued || job.running) ? "true" : "false",
                 static_cast<unsigned>(updated_at),
                 static_cast<unsigned>(profiles.size()));
        body += buf;
        json_prop(body, "action", job.action); body += ",";
        json_prop(body, "eid", eid); body += ",";
        json_prop(body, "updatedLocal", format_epoch_local(updated_at, cfg.tzOffsetMin)); body += ",";
        json_prop(body, "cacheMessage", cache_message); body += ",";
        json_prop(body, "jobMessage", job.message); body += ",";
        json_prop(body, "message", job.message.empty() ? cache_message : job.message); body += ",";
        body += "\"profiles\":[";
        for (size_t i = 0; i < profiles.size(); ++i) {
            if (i) body += ",";
            append_esim_profile_json(body, profiles[i]);
        }
        body += "]}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    if (req->method != HTTP_POST) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"eSIM 操作需要 POST\"}");
    }
    if (!(action == "refresh" || action == "info" || action == "enable" || action == "disable" ||
          action == "delete" ||
          action == "nickname" || action == "switch")) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"未知 eSIM 操作\"}");
    }

    std::string raw;
    if (read_body(req, raw, 768) != ESP_OK) return ESP_OK;
    IdfFormFields fields = parse_urlencoded(raw);
    std::string identifier;
    std::string nickname;
    if (const std::string* v = find_field(fields, "id")) identifier = trim_copy(*v);
    if (const std::string* v = find_field(fields, "nickname")) nickname = trim_copy(*v);
    if (action != "refresh" && action != "info" && identifier.empty()) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Profile 标识为空\"}");
    }
    if (action == "nickname" && nickname.size() > 64) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"昵称最长 64 字节\"}");
    }

    std::string message;
    bool already_running = false;
    bool ok = start_esim_job(action, identifier, nickname, message, already_running);
    std::string body = "{\"success\":";
    body += (ok || already_running) ? "true" : "false";
    body += ",\"queued\":";
    body += (ok || already_running) ? "true" : "false";
    body += ",";
    json_prop(body, "message", message);
    body += "}";
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_keepalive(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!ensure_get_or_post(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    set_json_no_cache(req);
    if (action == "reset") {
        if (req->method != HTTP_POST) {
            return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"该操作需要 POST\"}");
        }
        uint32_t now = static_cast<uint32_t>(time(nullptr));
        if (now < 1700000000u) {
            // 时间未同步时写 0 会让 keepalive_due 立即判定"到期"，触发一次
            // 计划外的蜂窝流量/短信保号动作——拒绝而不是照单全收
            return httpd_resp_sendstr(req,
                "{\"success\":false,\"message\":\"设备时间未同步，暂不能重置基准日，请等待 NTP 同步后重试\"}");
        }
        esp_err_t err = idf_config_set_keepalive_last(now);
        if (err == ESP_OK) {
            int tz = idf_config_get_tz_offset();
            std::string local = format_epoch_local(now, tz);
            idf_logf("网页重置保号基准日为 %s", local.empty() ? "当前时间" : local.c_str());
            std::string body = "{\"success\":true,";
            json_prop(body, "message", local.empty() ? "基准日已重置" : std::string("基准日已重置为 ") + local);
            char buf[64];
            snprintf(buf, sizeof(buf), ",\"lastTime\":%u,", static_cast<unsigned>(now));
            body += buf;
            json_prop(body, "lastTimeLocal", local);
            body += "}";
            return httpd_resp_send(req, body.c_str(), body.size());
        }
        std::string body = "{\"success\":false,";
        json_prop(body, "message", std::string("基准日重置失败: ") + esp_err_to_name(err));
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }
    if (action == "run") {
        if (req->method != HTTP_POST) {
            return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"该操作需要 POST\"}");
        }
        std::string message;
        bool already_running = false;
        IdfKeepaliveRunView run_cfg = idf_config_get_keepalive_run_view();
        bool ok = start_keepalive_job(run_cfg, "保号动作已排队，可继续刷新网页",
                                      message, already_running);
        std::string body = "{\"success\":";
        body += (ok || already_running) ? "true" : "false";
        body += ",\"queued\":";
        body += (ok || already_running) ? "true" : "false";
        body += ",";
        json_prop(body, "message", message);
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    const IdfKeepaliveStatusView cfg = idf_config_get_keepalive_status_view();
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    bool time_valid = now >= 1700000000u;
    int days_left = 0;
    uint32_t next_time = 0;
    if (time_valid && cfg.kaLastTime >= 1700000000u && cfg.kaIntervalDays > 0) {
        uint64_t next64 = static_cast<uint64_t>(cfg.kaLastTime) + static_cast<uint64_t>(cfg.kaIntervalDays) * 86400ULL;
        next_time = next64 > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(next64);
        uint32_t elapsed_days = now > cfg.kaLastTime ? (now - cfg.kaLastTime) / 86400u : 0;
        days_left = cfg.kaIntervalDays > static_cast<int>(elapsed_days)
            ? cfg.kaIntervalDays - static_cast<int>(elapsed_days)
            : 0;
    }
    WebAsyncJob job;
    if (cell_job_lock()) {
        job = s_keepalive_job;
        cell_job_unlock();
    }
    std::string body;
    body.reserve(840);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"enabled\":%s,\"intervalDays\":%d,\"action\":%u,",
             cfg.kaEnabled ? "true" : "false", cfg.kaIntervalDays,
             static_cast<unsigned>(cfg.kaAction));
    body += buf;
    json_prop(body, "target", cfg.kaTarget); body += ",";
    json_prop(body, "url", cfg.kaUrl); body += ",";
    json_prop(body, "profile", cfg.kaProfile); body += ",";
    snprintf(buf, sizeof(buf),
             "\"timeValid\":%s,\"tz\":%d,\"nowEpoch\":%u,",
             time_valid ? "true" : "false",
             cfg.tzOffsetMin,
             static_cast<unsigned>(time_valid ? now : 0));
    body += buf;
    json_prop(body, "tzName", format_tz_offset(cfg.tzOffsetMin)); body += ",";
    json_prop(body, "nowLocal", format_epoch_local(time_valid ? now : 0, cfg.tzOffsetMin)); body += ",";
    snprintf(buf, sizeof(buf),
             "\"lastTime\":%u,\"nextTime\":%u,\"daysLeft\":%d,",
             static_cast<unsigned>(cfg.kaLastTime),
             static_cast<unsigned>(next_time),
             days_left);
    body += buf;
    json_prop(body, "lastTimeLocal", format_epoch_local(cfg.kaLastTime, cfg.tzOffsetMin)); body += ",";
    json_prop(body, "nextTimeLocal", format_epoch_local(next_time, cfg.tzOffsetMin)); body += ",";
    snprintf(buf, sizeof(buf),
             "\"jobQueued\":%s,\"jobRunning\":%s,\"jobDone\":%s,"
             "\"jobSuccess\":%s,\"success\":%s,\"queued\":%s,",
             job.queued ? "true" : "false",
             job.running ? "true" : "false",
             job.done ? "true" : "false",
             job.success ? "true" : "false",
             job.success ? "true" : "false",
             (job.queued || job.running) ? "true" : "false");
    body += buf;
    json_prop(body, "jobMessage", job.message); body += ",";
    json_prop(body, "message", job.message);
    body += "}";
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_schedtask(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!ensure_get_or_post(req)) return ESP_OK;
    std::string action;
    get_query_param(req, "action", action);
    set_json_no_cache(req);

    if (action == "run" || action == "reset") {
        // 变更类动作只收 POST：GET 可被预取/跨站链接静默触发切卡/发短信
        if (req->method != HTTP_POST) {
            return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"该操作需要 POST\"}");
        }
        std::string idx_str;
        get_query_param(req, "index", idx_str);
        int index = -1;
        if (idx_str.size() == 1 && idx_str[0] >= '0' && idx_str[0] <= '9') index = idx_str[0] - '0';
        if (index < 0 || index >= IDF_MAX_SCHED_TASKS) {
            return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"任务序号无效\"}");
        }
        if (action == "reset") {
            uint32_t now = static_cast<uint32_t>(time(nullptr));
            if (!epoch_valid(now)) {
                return httpd_resp_sendstr(req,
                    "{\"success\":false,\"message\":\"设备时间未同步，暂不能重置基准日\"}");
            }
            bool ok = idf_config_set_sched_last(index, now) == ESP_OK;
            idf_logf("定时任务%d基准日已重置为今天", index + 1);
            std::string body = "{\"success\":";
            body += ok ? "true" : "false";
            body += ",\"message\":\"";
            body += ok ? "基准日已重置为今天" : "写入失败";
            body += "\"}";
            return httpd_resp_send(req, body.c_str(), body.size());
        }
        std::string message;
        bool already = false;
        IdfSchedRunView run_cfg = idf_config_get_sched_run_view(index);
        bool ok = start_sched_job(run_cfg, index, message, already);
        if (ok) idf_logf("网页手动触发定时任务%d", index + 1);
        std::string body = "{\"success\":";
        body += (ok || already) ? "true" : "false";
        body += ",\"queued\":";
        body += (ok || already) ? "true" : "false";
        body += ",";
        json_prop(body, "message", message);
        body += "}";
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    // status：任务配置 + 倒计时 + 后台执行状态（窄快照，此端点任务期间被 2s 轮询）
    IdfSchedStatusView cfg = idf_config_get_sched_view();
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    bool time_valid = epoch_valid(now);
    WebAsyncJob job;
    int job_index = -1;
    if (cell_job_lock()) {
        job = s_sched_job;
        job_index = s_sched_job_index;
        cell_job_unlock();
    }

    std::string body;
    body.reserve(512 + IDF_MAX_SCHED_TASKS * 420);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"timeValid\":%s,\"jobIndex\":%d,"
             "\"jobQueued\":%s,\"jobRunning\":%s,\"jobDone\":%s,\"jobSuccess\":%s,",
             time_valid ? "true" : "false", job_index,
             job.queued ? "true" : "false",
             job.running ? "true" : "false",
             job.done ? "true" : "false",
             job.success ? "true" : "false");
    body += buf;
    json_prop(body, "jobMessage", job.message); body += ",";
    body += "\"tasks\":[";
    for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) {
        const IdfSchedTask& t = cfg.tasks[i];
        int days_left = -1;  // -1=未建立基准日
        if (time_valid && epoch_valid(t.lastRun) && t.intervalDays > 0) {
            uint32_t elapsed_days = now > t.lastRun ? (now - t.lastRun) / 86400u : 0;
            days_left = t.intervalDays > static_cast<int>(elapsed_days)
                ? t.intervalDays - static_cast<int>(elapsed_days)
                : 0;
        }
        if (i) body += ",";
        snprintf(buf, sizeof(buf),
                 "{\"enabled\":%s,\"switchBack\":%s,\"intervalDays\":%d,"
                 "\"action\":%u,\"daysLeft\":%d,\"lastTime\":%u,",
                 t.enabled ? "true" : "false",
                 t.switchBack ? "true" : "false",
                 t.intervalDays,
                 static_cast<unsigned>(t.action),
                 days_left,
                 static_cast<unsigned>(t.lastRun));
        body += buf;
        json_prop(body, "name", t.name); body += ",";
        json_prop(body, "profile", t.profile); body += ",";
        json_prop(body, "target", t.target); body += ",";
        json_prop(body, "payload", t.payload); body += ",";
        json_prop(body, "lastLocal", format_epoch_local(t.lastRun, cfg.tzOffsetMin));
        body += "}";
    }
    body += "]}";
    return httpd_resp_send(req, body.c_str(), body.size());
}

// NET 指示灯立即开关：只发 AT 不写配置；模组自身可能记住该状态，
// 但每次模组初始化固件都会按已保存的配置重新下发覆盖
static esp_err_t handle_netled(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    set_json_no_cache(req);
    if (req->method != HTTP_POST) {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"该操作需要 POST\"}");
    }
    std::string action;
    get_query_param(req, "action", action);
    bool on = (action == "on");
    if (!on && action != "off") {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"未知动作\"}");
    }
    WebModemActionGuard modem_action;
    if (!modem_action.begin(req)) return ESP_OK;
    std::string resp;
    esp_err_t err = idf_modem_send_at(on ? "AT+MLED=0,1" : "AT+MLED=0,0", 3000, resp);
    bool ok = (err == ESP_OK && resp.find("OK") != std::string::npos);
    idf_logf("%s NET 指示灯: %s", on ? "开启" : "关闭", ok ? "成功" : "失败(模组不支持或忙)");
    std::string body = "{\"success\":";
    body += ok ? "true" : "false";
    body += ",";
    json_prop(body, "message", ok ? (on ? "NET 灯已开启（设备重启后按已保存的设置重新下发）"
                                        : "NET 灯已关闭（设备重启后按已保存的设置重新下发）")
                                  : "操作失败：模组不支持 AT+MLED 或正忙");
    body += "}";
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_ntp(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    set_json_no_cache(req);
    esp_err_t err = idf_wifi_resync_ntp();
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    const IdfConfigStatusView cfg = idf_config_get_status_view();
    std::string body = "{\"success\":";
    body += (err == ESP_OK) ? "true" : "false";
    body += ",";
    if (err == ESP_OK) {
        json_prop(body, "message", "已发起校时，同步成功后设备时间随即更新");
    } else {
        json_prop(body, "message", "WiFi 未连接，暂不能校时");
    }
    body += ",";
    json_prop(body, "nowLocal", format_epoch_local(now, cfg.tzOffsetMin));
    body += "}";
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t handle_reboot(httpd_req_t* req)
{
    if (!check_auth(req)) return ESP_OK;
    if (!check_csrf(req)) return ESP_OK;
    idf_log_line("网页触发设备重启");
    set_json_no_cache(req);
    esp_err_t send_err = httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"设备即将重启\"}");
    schedule_restart_or_now("web_restart");
    return send_err;
}

static esp_err_t handle_not_found(httpd_req_t* req)
{
    if (idf_wifi_get_status().apMode) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.1.1/");
        return httpd_resp_send(req, nullptr, 0);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_OK;
}

static esp_err_t register_handler(httpd_handle_t server, const char* uri, int method,
                                  esp_err_t (*handler)(httpd_req_t*))
{
    httpd_uri_t item = {};
    item.uri = uri;
    item.method = static_cast<httpd_method_t>(method);
    item.handler = handler;
    return httpd_register_uri_handler(server, &item);
}

esp_err_t idf_web_start(void)
{
    if (s_server) return ESP_OK;
    if (!s_cell_job_mutex) {
        s_cell_job_mutex = xSemaphoreCreateMutex();
        if (!s_cell_job_mutex) return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    // 多方法接口使用 HTTP_ANY 单路由，实际路由约 37 个；保留余量同时减少 HTTPD 路由表内存
    config.max_uri_handlers = 48;
    config.stack_size = 8192;
    // HTTPD 内部预留 3 个 socket；此外本固件常驻 mDNS/DNS/SNTP，推送/SMTP
    // 也需要余量。已有 build/sdkconfig 可能仍是旧值(如 LWIP=10)，硬设 13
    // 会让 httpd_start 失败，设 7 又会在访问时 accept(ENFILE=23)。
    int reserved_non_http_sockets = 3;
    int http_socket_cap = CONFIG_LWIP_MAX_SOCKETS - 3 - reserved_non_http_sockets;
    if (http_socket_cap < 2) http_socket_cap = 2;
    int desired_http_sockets = 13;
    config.max_open_sockets = desired_http_sockets < http_socket_cap ? desired_http_sockets : http_socket_cap;
    ESP_LOGI(TAG, "HTTP sockets: open=%d, lwip=%d", config.max_open_sockets, CONFIG_LWIP_MAX_SOCKETS);
    // WiFi 掉线/标签页休眠留下的半开死连接靠 TCP keepalive 回收(约 24s)，
    // 否则它们占住 socket 只能等 LRU 被动淘汰
    config.keep_alive_enable = CONFIG_LWIP_MAX_SOCKETS > 10;
    config.keep_alive_idle = 15;      // 空闲 15s 后开始探测
    config.keep_alive_interval = 3;   // 每 3s 一次
    config.keep_alive_count = 3;      // 3 次无响应即断开

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        cleanup_cell_job_mutex_if_unused();
        return err;
    }

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = handle_root;

    httpd_uri_t assets = {};
    assets.uri = "/assets/*";
    assets.method = HTTP_GET;
    assets.handler = handle_asset;

    httpd_uri_t ui = {};
    ui.uri = "/ui";
    ui.method = HTTP_GET;
    ui.handler = handle_ui_panel;

    httpd_uri_t status = {};
    status.uri = "/status";
    status.method = HTTP_GET;
    status.handler = handle_status;

    httpd_uri_t config_json = {};
    config_json.uri = "/config.json";
    config_json.method = HTTP_GET;
    config_json.handler = send_config_json;

    httpd_uri_t save = {};
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = handle_save;

    httpd_uri_t wifi_scan = {};
    wifi_scan.uri = "/wifiscan";
    wifi_scan.method = HTTP_GET;
    wifi_scan.handler = handle_wifi_scan;

    httpd_uri_t wifi_config = {};
    wifi_config.uri = "/wificonfig";
    wifi_config.method = HTTP_POST;
    wifi_config.handler = handle_wifi_config;

    httpd_uri_t messages = {};
    messages.uri = "/messages";
    messages.method = HTTP_GET;
    messages.handler = handle_messages;

    httpd_uri_t log = {};
    log.uri = "/log";
    log.method = HTTP_GET;
    log.handler = handle_empty_log;

    httpd_uri_t netled_post = {};
    netled_post.uri = "/netled";
    netled_post.method = HTTP_POST;
    netled_post.handler = handle_netled;

    httpd_uri_t reboot = {};
    reboot.uri = "/reboot";
    reboot.method = HTTP_POST;
    reboot.handler = handle_reboot;

    httpd_uri_t not_found = {};
    not_found.uri = "/*";
    not_found.method = HTTP_GET;
    not_found.handler = handle_not_found;

    auto register_checked = [&](const char* name, esp_err_t reg_err) -> esp_err_t {
        if (reg_err == ESP_OK) return ESP_OK;
        ESP_LOGE(TAG, "注册 HTTP 路由 %s 失败: %s", name, esp_err_to_name(reg_err));
        idf_logf("注册 HTTP 路由 %s 失败: %s", name, esp_err_to_name(reg_err));
        httpd_stop(s_server);
        s_server = nullptr;
        cleanup_cell_job_mutex_if_unused();
        return reg_err;
    };

#define IDF_WEB_TRY_REGISTER(name, expr) do { \
        esp_err_t _reg_err = register_checked((name), (expr)); \
        if (_reg_err != ESP_OK) return _reg_err; \
    } while (0)

    IDF_WEB_TRY_REGISTER("/", httpd_register_uri_handler(s_server, &root));
    IDF_WEB_TRY_REGISTER("/tools", register_handler(s_server, "/tools", HTTP_GET, handle_root));
    IDF_WEB_TRY_REGISTER("/sms", register_handler(s_server, "/sms", HTTP_GET, handle_root));
    IDF_WEB_TRY_REGISTER("/assets/*", httpd_register_uri_handler(s_server, &assets));
    IDF_WEB_TRY_REGISTER("/ui", httpd_register_uri_handler(s_server, &ui));
    IDF_WEB_TRY_REGISTER("/status", httpd_register_uri_handler(s_server, &status));
    IDF_WEB_TRY_REGISTER("/config.json", httpd_register_uri_handler(s_server, &config_json));
    IDF_WEB_TRY_REGISTER("/save", httpd_register_uri_handler(s_server, &save));
    IDF_WEB_TRY_REGISTER("/wifi", register_handler(s_server, "/wifi", HTTP_ANY, handle_wifi));
    IDF_WEB_TRY_REGISTER("/ntp", register_handler(s_server, "/ntp", HTTP_POST, handle_ntp));
    IDF_WEB_TRY_REGISTER("/wifiscan", httpd_register_uri_handler(s_server, &wifi_scan));
    IDF_WEB_TRY_REGISTER("/wificonfig", httpd_register_uri_handler(s_server, &wifi_config));
    IDF_WEB_TRY_REGISTER("/apstatus", register_handler(s_server, "/apstatus", HTTP_GET, handle_apstatus));
    IDF_WEB_TRY_REGISTER("/messages", httpd_register_uri_handler(s_server, &messages));
    IDF_WEB_TRY_REGISTER("/log", httpd_register_uri_handler(s_server, &log));
    IDF_WEB_TRY_REGISTER("/keepalive", register_handler(s_server, "/keepalive", HTTP_ANY, handle_keepalive));
    IDF_WEB_TRY_REGISTER("/esim", register_handler(s_server, "/esim", HTTP_ANY, handle_esim));
    IDF_WEB_TRY_REGISTER("/schedtask", register_handler(s_server, "/schedtask", HTTP_ANY, handle_schedtask));
    IDF_WEB_TRY_REGISTER("/netled POST", httpd_register_uri_handler(s_server, &netled_post));
    IDF_WEB_TRY_REGISTER("/reboot", httpd_register_uri_handler(s_server, &reboot));
    IDF_WEB_TRY_REGISTER("/at", register_handler(s_server, "/at", HTTP_ANY, handle_at));
    IDF_WEB_TRY_REGISTER("/ping", register_handler(s_server, "/ping", HTTP_ANY, handle_ping));
    IDF_WEB_TRY_REGISTER("/testpush", register_handler(s_server, "/testpush", HTTP_ANY, handle_test_push));
    IDF_WEB_TRY_REGISTER("/ussd", register_handler(s_server, "/ussd", HTTP_ANY, handle_ussd));
    IDF_WEB_TRY_REGISTER("/flight", register_handler(s_server, "/flight", HTTP_ANY, handle_flight));
    IDF_WEB_TRY_REGISTER("/modem", register_handler(s_server, "/modem", HTTP_ANY, handle_modem_control));
    IDF_WEB_TRY_REGISTER("/sendsms", register_handler(s_server, "/sendsms", HTTP_POST, handle_send_sms));
    IDF_WEB_TRY_REGISTER("/resend", register_handler(s_server, "/resend", HTTP_POST, handle_resend_message));
    IDF_WEB_TRY_REGISTER("/delete", register_handler(s_server, "/delete", HTTP_POST, handle_delete_message));
    IDF_WEB_TRY_REGISTER("/factory", register_handler(s_server, "/factory", HTTP_POST, handle_factory_reset));
    IDF_WEB_TRY_REGISTER("/import", register_handler(s_server, "/import", HTTP_POST, handle_import_config));
    IDF_WEB_TRY_REGISTER("/update", register_handler(s_server, "/update", HTTP_POST, handle_ota_update));
    IDF_WEB_TRY_REGISTER("/export", register_handler(s_server, "/export", HTTP_GET, handle_export_config));
    IDF_WEB_TRY_REGISTER("/logdownload", register_handler(s_server, "/logdownload", HTTP_GET, handle_log_download));
    IDF_WEB_TRY_REGISTER("/prevlog", register_handler(s_server, "/prevlog", HTTP_GET, handle_prev_log));
    IDF_WEB_TRY_REGISTER("/coredump", register_handler(s_server, "/coredump", HTTP_GET, handle_coredump_download));
    IDF_WEB_TRY_REGISTER("/coredump/clear", register_handler(s_server, "/coredump/clear", HTTP_POST, handle_coredump_clear));
    IDF_WEB_TRY_REGISTER("/*", httpd_register_uri_handler(s_server, &not_found));

#undef IDF_WEB_TRY_REGISTER
    if (!s_scheduler_started) {
        BaseType_t ok = xTaskCreate(scheduler_task, "idf_sched", 8192, nullptr, 2, nullptr);
        if (ok == pdPASS) {
            s_scheduler_started = true;
            idf_log_line("定时任务 scheduler 已启动");
        } else {
            ESP_LOGW(TAG, "scheduler task start failed");
            idf_log_line("定时任务 scheduler 启动失败");
        }
    }
    ESP_LOGI(TAG, "ESP-IDF web server registered UI and bootstrap dynamic routes");
    idf_log_line("HTTP 服务器已启动");
    return ESP_OK;
}

void idf_web_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = nullptr;
}
