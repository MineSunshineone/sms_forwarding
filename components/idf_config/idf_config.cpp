#include "idf_config.h"

#include <algorithm>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "idf_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#if IDF_HAS_WIFI_CONFIG_H
#include "wifi_config.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

static const char* TAG = "idf_config";
static IdfConfig s_config;
static SemaphoreHandle_t s_config_mutex = nullptr;
static SemaphoreHandle_t s_persist_mutex = nullptr;

static esp_err_t save_config_to_nvs(const IdfConfig& c);
static esp_err_t commit_config_update(IdfConfig& next, const IdfConfig& base);

static esp_err_t ensure_config_mutex()
{
    if (!s_config_mutex) {
        s_config_mutex = xSemaphoreCreateMutex();
        if (!s_config_mutex) return ESP_ERR_NO_MEM;
    }
    if (!s_persist_mutex) {
        s_persist_mutex = xSemaphoreCreateMutex();
        if (!s_persist_mutex) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static IdfConfig config_snapshot()
{
    if (ensure_config_mutex() != ESP_OK) return IdfConfig();
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    IdfConfig copy = s_config;
    xSemaphoreGive(s_config_mutex);
    return copy;
}

static esp_err_t replace_config(const IdfConfig& next)
{
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    s_config = next;
    xSemaphoreGive(s_config_mutex);
    return ESP_OK;
}

static std::string channel_default_name(int idx)
{
    char buf[20];
    snprintf(buf, sizeof(buf), "通道%d", idx + 1);
    return std::string(buf);
}

static bool is_blank(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

static std::string read_str(nvs_handle_t nvs, const char* key, const char* fallback, size_t max_len = 1024)
{
    size_t len = 0;
    esp_err_t err = nvs_get_str(nvs, key, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return fallback ? std::string(fallback) : std::string();
    if (err != ESP_OK || len == 0) {
        if (err != ESP_OK) ESP_LOGW(TAG, "读取 NVS 字符串 %s 失败: %s", key, esp_err_to_name(err));
        return fallback ? std::string(fallback) : std::string();
    }
    std::string value(len, '\0');
    err = nvs_get_str(nvs, key, value.data(), &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取 NVS 字符串 %s 内容失败: %s", key, esp_err_to_name(err));
        return fallback ? std::string(fallback) : std::string();
    }
    if (!value.empty() && value.back() == '\0') value.pop_back();
    if (value.size() > max_len) {
        // 截断保留而不是丢弃：Arduino 版对字符串没有长度上限，OTA 迁移过来的
        // 超长规则/黑名单若被静默清空，下次保存还会把 NVS 里完好的原值覆盖掉
        size_t end = max_len;
        while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
        value.resize(end);
        ESP_LOGE(TAG, "NVS 字符串 %s 超长，已截断保留前 %u 字节", key, static_cast<unsigned>(end));
        idf_logf("配置项 %s 超长，已截断保留(建议在网页里检查该项)", key);
    }
    return value;
}

static int32_t read_i32(nvs_handle_t nvs, const char* key, int32_t fallback)
{
    int32_t value = fallback;
    esp_err_t err = nvs_get_i32(nvs, key, &value);
    return err == ESP_OK ? value : fallback;
}

static uint32_t read_u32(nvs_handle_t nvs, const char* key, uint32_t fallback)
{
    uint32_t value = fallback;
    esp_err_t err = nvs_get_u32(nvs, key, &value);
    return err == ESP_OK ? value : fallback;
}

static uint8_t read_u8(nvs_handle_t nvs, const char* key, uint8_t fallback)
{
    uint8_t value = fallback;
    esp_err_t err = nvs_get_u8(nvs, key, &value);
    return err == ESP_OK ? value : fallback;
}

static bool read_bool(nvs_handle_t nvs, const char* key, bool fallback)
{
    return read_u8(nvs, key, fallback ? 1 : 0) != 0;
}

static esp_err_t write_str(nvs_handle_t nvs, const char* key, const std::string& value)
{
    return nvs_set_str(nvs, key, value.c_str());
}

static int clamp_int(int value, int lo, int hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

static void limit_utf8_bytes(std::string& value, size_t max_len)
{
    if (value.size() <= max_len) return;
    size_t end = max_len;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
    value.resize(end);
}

static void normalize_config(IdfConfig& c)
{
    limit_utf8_bytes(c.wifiSsid, 64);
    limit_utf8_bytes(c.wifiPass, 128);
    limit_utf8_bytes(c.wifiSsid2, 64);
    limit_utf8_bytes(c.wifiPass2, 128);
    limit_utf8_bytes(c.smtpServer, 128);
    c.smtpPort = (c.smtpPort > 0 && c.smtpPort <= 65535) ? c.smtpPort : 465;
    limit_utf8_bytes(c.smtpUser, 128);
    limit_utf8_bytes(c.smtpPass, 256);
    limit_utf8_bytes(c.smtpSendTo, 256);
    limit_utf8_bytes(c.adminPhone, 64);
    limit_utf8_bytes(c.webUser, 64);
    limit_utf8_bytes(c.webPass, 128);
    if (is_blank(c.webUser)) c.webUser = IDF_DEFAULT_WEB_USER;
    if (is_blank(c.webPass)) c.webPass = IDF_DEFAULT_WEB_PASS;
    limit_utf8_bytes(c.numberBlackList, 1024);
    limit_utf8_bytes(c.forwardRules, 2048);

    c.kaIntervalDays = clamp_int(c.kaIntervalDays, 1, 3650);
    if (c.kaAction > 3) c.kaAction = 1;
    limit_utf8_bytes(c.kaTarget, 64);
    limit_utf8_bytes(c.kaUrl, 256);
    if (c.kaUrl.empty()) c.kaUrl = IDF_KEEPALIVE_DEFAULT_URL;
    limit_utf8_bytes(c.kaProfile, 64);

    c.tzOffsetMin = clamp_int(c.tzOffsetMin, -720, 840);
    limit_utf8_bytes(c.ntpServer, 128);
    c.rebootHour = clamp_int(c.rebootHour, 0, 23);
    c.hbHour = clamp_int(c.hbHour, 0, 23);

    limit_utf8_bytes(c.apn, 96);
    limit_utf8_bytes(c.operatorPlmn, 16);
    limit_utf8_bytes(c.phoneNumber, 64);

    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        IdfPushChannel& ch = c.pushChannels[i];
        ch.type = (ch.type >= 1 && ch.type <= 10) ? ch.type : 1;
        limit_utf8_bytes(ch.name, 64);
        if (ch.name.empty()) ch.name = channel_default_name(i);
        limit_utf8_bytes(ch.url, 512);
        limit_utf8_bytes(ch.key1, 256);
        limit_utf8_bytes(ch.key2, 512);
        limit_utf8_bytes(ch.customBody, 1024);
    }

    for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) {
        IdfSchedTask& t = c.schedTasks[i];
        limit_utf8_bytes(t.name, 32);
        limit_utf8_bytes(t.profile, 64);
        t.intervalDays = clamp_int(t.intervalDays, 1, 3650);
        if (t.action > 3) t.action = 0;
        limit_utf8_bytes(t.target, 128);
        limit_utf8_bytes(t.payload, 128);
    }
}

static std::string trim_copy(const std::string& value)
{
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static bool looks_like_url(const std::string& value)
{
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

static std::string cfg_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else out += ch;
    }
    return out;
}

static std::string cfg_unescape(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        char ch = value[i];
        if (ch == '\\' && i + 1 < value.size()) {
            char next = value[i + 1];
            if (next == 'n') { out += '\n'; ++i; }
            else if (next == 'r') { out += '\r'; ++i; }
            else if (next == '\\') { out += '\\'; ++i; }
            else out += ch;
        } else {
            out += ch;
        }
    }
    return out;
}

static bool bool_from_text(const std::string& value)
{
    return value == "1" || value == "true" || value == "on" || value == "yes";
}

static bool parse_int_strict(const std::string& value, int& out)
{
    const char* start = value.c_str();
    while (isspace(static_cast<unsigned char>(*start))) ++start;
    if (*start == '\0') return false;
    errno = 0;
    char* end = nullptr;
    long parsed = strtol(start, &end, 10);
    if (end == start || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) return false;
    while (isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return false;
    out = static_cast<int>(parsed);
    return true;
}

static bool parse_u32_strict(const std::string& value, uint32_t& out)
{
    const char* p = value.c_str();
    while (isspace(static_cast<unsigned char>(*p))) ++p;
    if (!isdigit(static_cast<unsigned char>(*p))) return false;
    uint64_t parsed = 0;
    while (isdigit(static_cast<unsigned char>(*p))) {
        parsed = parsed * 10ULL + static_cast<unsigned>(*p - '0');
        if (parsed > 0xffffffffULL) return false;
        ++p;
    }
    while (isspace(static_cast<unsigned char>(*p))) ++p;
    if (*p != '\0') return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

static void import_int_field(int& target, const std::string& value)
{
    int parsed = 0;
    if (parse_int_strict(value, parsed)) target = parsed;
}

static void import_u8_field(uint8_t& target, const std::string& value)
{
    int parsed = 0;
    if (parse_int_strict(value, parsed) && parsed >= 0 && parsed <= 255) {
        target = static_cast<uint8_t>(parsed);
    }
}

static void import_u32_field(uint32_t& target, const std::string& value)
{
    uint32_t parsed = 0;
    if (parse_u32_strict(value, parsed)) target = parsed;
}

static void append_kv(std::string& out, const char* key, const std::string& value)
{
    out += key;
    out += "=";
    out += cfg_escape(value);
    out += "\n";
}

static void append_kv_i(std::string& out, const char* key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    append_kv(out, key, buf);
}

static void append_kv_u32(std::string& out, const char* key, uint32_t value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(value));
    append_kv(out, key, buf);
}

esp_err_t idf_config_load(void)
{
    esp_err_t mutex_err = ensure_config_mutex();
    if (mutex_err != ESP_OK) return mutex_err;

    IdfConfig next;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("sms_config", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS 配置不存在，使用默认值");
        idf_log_line("NVS 配置不存在，使用默认值");
    } else if (err != ESP_OK) {
        // 不提前返回：继续走默认值+编译期 WiFi 回退，至少保证设备能上网可救
        ESP_LOGE(TAG, "打开 NVS 配置失败: %s，使用默认值", esp_err_to_name(err));
        idf_logf("打开 NVS 配置失败: %s，使用默认值", esp_err_to_name(err));
    }

    if (err == ESP_OK) {
        next.wifiSsid = read_str(nvs, "wifiSsid", "", 64);
        next.wifiPass = read_str(nvs, "wifiPass", "", 128);
        next.wifiSsid2 = read_str(nvs, "wifiSsid2", "", 64);
        next.wifiPass2 = read_str(nvs, "wifiPass2", "", 128);
        next.smtpServer = read_str(nvs, "smtpServer", "", 128);
        next.smtpPort = read_i32(nvs, "smtpPort", 465);
        next.smtpUser = read_str(nvs, "smtpUser", "", 128);
        next.smtpPass = read_str(nvs, "smtpPass", "", 256);
        next.smtpSendTo = read_str(nvs, "smtpSendTo", "", 256);
        next.adminPhone = read_str(nvs, "adminPhone", "", 64);
        next.webUser = read_str(nvs, "webUser", IDF_DEFAULT_WEB_USER, 64);
        next.webPass = read_str(nvs, "webPass", IDF_DEFAULT_WEB_PASS, 128);
        next.numberBlackList = read_str(nvs, "numBlkList", "", 1024);
        next.forwardRules = read_str(nvs, "fwdRules", "", 2048);
        next.emailEnabled = read_bool(nvs, "emailEn", true);
        next.pushEnabled = read_bool(nvs, "pushEn", true);

        next.kaEnabled = read_bool(nvs, "kaEn", false);
        next.kaIntervalDays = read_i32(nvs, "kaDays", 175);
        next.kaAction = read_u8(nvs, "kaAct", 1);
        next.kaTarget = read_str(nvs, "kaTarget", "", 64);
        next.kaUrl = read_str(nvs, "kaUrl", IDF_KEEPALIVE_DEFAULT_URL, 256);
        next.kaProfile = read_str(nvs, "kaProfile", "", 64);
        next.kaLastTime = read_u32(nvs, "kaLast", 0);

        next.tzOffsetMin = read_i32(nvs, "tzMin", 480);
        next.ntpServer = read_str(nvs, "ntpSrv", "ntp.aliyun.com", 128);
        next.rebootEnabled = read_bool(nvs, "rbEn", false);
        next.rebootHour = read_i32(nvs, "rbHour", 4);
        next.hbEnabled = read_bool(nvs, "hbEn", false);
        next.hbHour = read_i32(nvs, "hbHour", 9);

        next.netLedEnabled = read_bool(nvs, "netLed", true);
        next.callNotifyEnabled = read_bool(nvs, "callNotify", true);
        next.dataEnabled = read_bool(nvs, "dataEn", false);
        next.roamingEnabled = read_bool(nvs, "roamEn", true);
        next.apn = read_str(nvs, "apn", "", 96);
        next.operatorPlmn = read_str(nvs, "opPlmn", "", 16);
        next.phoneNumber = read_str(nvs, "phoneNum", "", 64);

        for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
            char prefix[12];
            snprintf(prefix, sizeof(prefix), "push%d", i);
            auto key = [&](const char* suffix) {
                char buf[24];
                snprintf(buf, sizeof(buf), "%s%s", prefix, suffix);
                return std::string(buf);
            };
            IdfPushChannel& ch = next.pushChannels[i];
            ch.enabled = read_bool(nvs, key("en").c_str(), false);
            ch.type = read_u8(nvs, key("type").c_str(), 1);
            ch.url = read_str(nvs, key("url").c_str(), "", 512);
            ch.name = read_str(nvs, key("name").c_str(), channel_default_name(i).c_str(), 64);
            ch.key1 = read_str(nvs, key("k1").c_str(), "", 256);
            ch.key2 = read_str(nvs, key("k2").c_str(), "", 512);
            ch.customBody = read_str(nvs, key("body").c_str(), "", 1024);
        }

        for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) {
            auto key = [&](const char* suffix) {
                char buf[16];
                snprintf(buf, sizeof(buf), "st%d%s", i, suffix);
                return std::string(buf);
            };
            IdfSchedTask& t = next.schedTasks[i];
            t.enabled = read_bool(nvs, key("En").c_str(), false);
            t.name = read_str(nvs, key("Name").c_str(), "", 32);
            t.profile = read_str(nvs, key("Prof").c_str(), "", 64);
            t.switchBack = read_bool(nvs, key("Back").c_str(), true);
            t.intervalDays = read_i32(nvs, key("Days").c_str(), 30);
            t.action = read_u8(nvs, key("Act").c_str(), 0);
            t.target = read_str(nvs, key("Tgt").c_str(), "", 128);
            t.payload = read_str(nvs, key("Pay").c_str(), "", 128);
            t.lastRun = read_u32(nvs, key("Last").c_str(), 0);
        }

        // 旧版(多通道之前)单通道配置一次性迁移：httpUrl/barkMode → 通道1
        if (!next.pushChannels[0].enabled && next.pushChannels[0].url.empty()) {
            std::string legacy_url = read_str(nvs, "httpUrl", "", 512);
            if (!legacy_url.empty()) {
                next.pushChannels[0].enabled = true;
                next.pushChannels[0].type = read_bool(nvs, "barkMode", false) ? 2 : 1;  // 2=Bark 1=POST JSON
                next.pushChannels[0].url = legacy_url;
                idf_log_line("已迁移旧版单通道推送配置到通道1");
            }
        }
        nvs_close(nvs);
    }

    if (next.wifiSsid.empty() && WIFI_SSID[0]) {
        next.wifiSsid = WIFI_SSID;
        next.wifiPass = WIFI_PASS;
        next.wifiFromFallback = true;
    }
    normalize_config(next);

    std::string log_wifi = next.wifiSsid;
    bool log_fallback = next.wifiFromFallback;
    std::string log_web_user = next.webUser;
    esp_err_t set_err = replace_config(next);
    if (set_err != ESP_OK) return set_err;
    ESP_LOGI(TAG, "配置已加载: wifi=%s%s, webUser=%s",
             log_wifi.empty() ? "(none)" : log_wifi.c_str(),
             log_fallback ? " (fallback)" : "",
             log_web_user.c_str());
    idf_logf("配置已加载: wifi=%s%s, webUser=%s",
             log_wifi.empty() ? "(none)" : log_wifi.c_str(),
             log_fallback ? " (fallback)" : "",
             log_web_user.c_str());
    return ESP_OK;
}

std::string idf_config_export_text(void)
{
    IdfConfig c = idf_config_get();
    std::string out;
    out.reserve(4096);

    append_kv(out, "wifiSsid", c.wifiSsid);
    append_kv(out, "wifiPass", c.wifiPass);
    append_kv(out, "wifiSsid2", c.wifiSsid2);
    append_kv(out, "wifiPass2", c.wifiPass2);
    append_kv(out, "smtpServer", c.smtpServer);
    append_kv_i(out, "smtpPort", c.smtpPort);
    append_kv(out, "smtpUser", c.smtpUser);
    append_kv(out, "smtpPass", c.smtpPass);
    append_kv(out, "smtpSendTo", c.smtpSendTo);
    append_kv(out, "adminPhone", c.adminPhone);
    append_kv(out, "webUser", c.webUser);
    append_kv(out, "webPass", c.webPass);
    append_kv(out, "numBlkList", c.numberBlackList);
    append_kv(out, "fwdRules", c.forwardRules);
    append_kv_i(out, "emailEnabled", c.emailEnabled ? 1 : 0);
    append_kv_i(out, "pushEnabled", c.pushEnabled ? 1 : 0);

    append_kv_i(out, "tzOffsetMin", c.tzOffsetMin);
    append_kv(out, "ntpServer", c.ntpServer);
    append_kv_i(out, "rebootEnabled", c.rebootEnabled ? 1 : 0);
    append_kv_i(out, "rebootHour", c.rebootHour);
    append_kv_i(out, "hbEnabled", c.hbEnabled ? 1 : 0);
    append_kv_i(out, "hbHour", c.hbHour);

    append_kv_i(out, "kaEnabled", c.kaEnabled ? 1 : 0);
    append_kv_i(out, "kaIntervalDays", c.kaIntervalDays);
    append_kv_i(out, "kaAction", c.kaAction);
    append_kv(out, "kaTarget", c.kaTarget);
    append_kv(out, "kaUrl", c.kaUrl);
    append_kv(out, "kaProfile", c.kaProfile);
    append_kv_u32(out, "kaLastTime", c.kaLastTime);

    append_kv_i(out, "netLedEnabled", c.netLedEnabled ? 1 : 0);
    append_kv_i(out, "callNotifyEnabled", c.callNotifyEnabled ? 1 : 0);
    append_kv_i(out, "dataEnabled", c.dataEnabled ? 1 : 0);
    append_kv_i(out, "roamingEnabled", c.roamingEnabled ? 1 : 0);
    append_kv(out, "apn", c.apn);
    append_kv(out, "operatorPlmn", c.operatorPlmn);
    append_kv(out, "phoneNumber", c.phoneNumber);

    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        char key[24];
        const IdfPushChannel& ch = c.pushChannels[i];
        snprintf(key, sizeof(key), "push%den", i);
        append_kv_i(out, key, ch.enabled ? 1 : 0);
        snprintf(key, sizeof(key), "push%dtype", i);
        append_kv_i(out, key, ch.type);
        snprintf(key, sizeof(key), "push%durl", i);
        append_kv(out, key, ch.url);
        snprintf(key, sizeof(key), "push%dname", i);
        append_kv(out, key, ch.name);
        snprintf(key, sizeof(key), "push%dk1", i);
        append_kv(out, key, ch.key1);
        snprintf(key, sizeof(key), "push%dk2", i);
        append_kv(out, key, ch.key2);
        snprintf(key, sizeof(key), "push%dbody", i);
        append_kv(out, key, ch.customBody);
    }

    for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) {
        char key[24];
        const IdfSchedTask& t = c.schedTasks[i];
        snprintf(key, sizeof(key), "st%dEn", i);
        append_kv_i(out, key, t.enabled ? 1 : 0);
        snprintf(key, sizeof(key), "st%dName", i);
        append_kv(out, key, t.name);
        snprintf(key, sizeof(key), "st%dProf", i);
        append_kv(out, key, t.profile);
        snprintf(key, sizeof(key), "st%dBack", i);
        append_kv_i(out, key, t.switchBack ? 1 : 0);
        snprintf(key, sizeof(key), "st%dDays", i);
        append_kv_i(out, key, t.intervalDays);
        snprintf(key, sizeof(key), "st%dAct", i);
        append_kv_i(out, key, t.action);
        snprintf(key, sizeof(key), "st%dTgt", i);
        append_kv(out, key, t.target);
        snprintf(key, sizeof(key), "st%dPay", i);
        append_kv(out, key, t.payload);
        snprintf(key, sizeof(key), "st%dLast", i);
        append_kv_u32(out, key, t.lastRun);
    }
    return out;
}

static void apply_import_key(IdfConfig& c, const std::string& key, const std::string& value)
{
    if (key == "wifiSsid") c.wifiSsid = value;
    else if (key == "wifiPass") c.wifiPass = value;
    else if (key == "wifiSsid2") c.wifiSsid2 = value;
    else if (key == "wifiPass2") c.wifiPass2 = value;
    else if (key == "smtpServer") c.smtpServer = value;
    else if (key == "smtpPort") import_int_field(c.smtpPort, value);
    else if (key == "smtpUser") c.smtpUser = value;
    else if (key == "smtpPass") c.smtpPass = value;
    else if (key == "smtpSendTo") c.smtpSendTo = value;
    else if (key == "adminPhone") c.adminPhone = value;
    else if (key == "webUser" && !is_blank(value)) c.webUser = value;
    else if (key == "webPass" && !is_blank(value)) c.webPass = value;
    else if (key == "numBlkList" || key == "numberBlackList") c.numberBlackList = value;
    else if (key == "fwdRules" || key == "forwardRules") c.forwardRules = value;
    else if (key == "emailEnabled") c.emailEnabled = bool_from_text(value);
    else if (key == "pushEnabled") c.pushEnabled = bool_from_text(value);
    else if (key == "tzOffsetMin") import_int_field(c.tzOffsetMin, value);
    else if (key == "ntpServer") c.ntpServer = value;
    else if (key == "rebootEnabled") c.rebootEnabled = bool_from_text(value);
    else if (key == "rebootHour") import_int_field(c.rebootHour, value);
    else if (key == "hbEnabled") c.hbEnabled = bool_from_text(value);
    else if (key == "hbHour") import_int_field(c.hbHour, value);
    else if (key == "kaEnabled") c.kaEnabled = bool_from_text(value);
    else if (key == "kaIntervalDays") import_int_field(c.kaIntervalDays, value);
    else if (key == "kaAction") import_u8_field(c.kaAction, value);
    else if (key == "kaTarget") c.kaTarget = value;
    else if (key == "kaUrl") c.kaUrl = value.empty() ? IDF_KEEPALIVE_DEFAULT_URL : value;
    else if (key == "kaProfile") c.kaProfile = value;
    else if (key == "kaLastTime") import_u32_field(c.kaLastTime, value);
    else if (key == "netLedEnabled") c.netLedEnabled = bool_from_text(value);
    else if (key == "callNotifyEnabled") c.callNotifyEnabled = bool_from_text(value);
    else if (key == "dataEnabled") c.dataEnabled = bool_from_text(value);
    else if (key == "roamingEnabled") c.roamingEnabled = bool_from_text(value);
    else if (key == "apn") c.apn = value;
    else if (key == "operatorPlmn") c.operatorPlmn = value;
    else if (key == "phoneNumber") c.phoneNumber = value;
    else if (key.rfind("st", 0) == 0 && key.size() > 3 && isdigit(static_cast<unsigned char>(key[2]))) {
        int idx = key[2] - '0';
        if (idx < 0 || idx >= IDF_MAX_SCHED_TASKS) return;
        std::string suffix = key.substr(3);
        IdfSchedTask& t = c.schedTasks[idx];
        if (suffix == "En") t.enabled = bool_from_text(value);
        else if (suffix == "Name") t.name = value;
        else if (suffix == "Prof") t.profile = value;
        else if (suffix == "Back") t.switchBack = bool_from_text(value);
        else if (suffix == "Days") import_int_field(t.intervalDays, value);
        else if (suffix == "Act") import_u8_field(t.action, value);
        else if (suffix == "Tgt") t.target = value;
        else if (suffix == "Pay") t.payload = value;
        else if (suffix == "Last") import_u32_field(t.lastRun, value);
    }
    else if (key.rfind("push", 0) == 0) {
        size_t pos = 4;
        int idx = 0;
        bool has_digit = false;
        while (pos < key.size() && isdigit(static_cast<unsigned char>(key[pos]))) {
            has_digit = true;
            idx = idx * 10 + (key[pos] - '0');
            ++pos;
        }
        if (!has_digit || idx < 0 || idx >= IDF_MAX_PUSH_CHANNELS) return;
        std::string suffix = key.substr(pos);
        IdfPushChannel& ch = c.pushChannels[idx];
        if (suffix == "en") ch.enabled = bool_from_text(value);
        else if (suffix == "type") import_u8_field(ch.type, value);
        else if (suffix == "url") ch.url = value;
        else if (suffix == "name") ch.name = value.empty() ? channel_default_name(idx) : value;
        else if (suffix == "k1") ch.key1 = value;
        else if (suffix == "k2") ch.key2 = value;
        else if (suffix == "body") ch.customBody = value;
    }
}

esp_err_t idf_config_import_text(const std::string& text, int* applied_count)
{
    if (text.empty()) return ESP_ERR_INVALID_ARG;
    IdfConfig base = idf_config_get();
    IdfConfig next = base;
    int applied = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = line.substr(0, eq);
            std::string value = cfg_unescape(line.substr(eq + 1));
            key.erase(0, key.find_first_not_of(" \t\r\n"));
            size_t keep = key.find_last_not_of(" \t\r\n");
            if (keep != std::string::npos) key.erase(keep + 1);
            if (!key.empty()) {
                apply_import_key(next, key, value);
                ++applied;
            }
        }
        if (end == text.size()) break;
        pos = end + 1;
    }

    next.wifiFromFallback = false;
    esp_err_t err = commit_config_update(next, base);
    if (err == ESP_OK && applied_count) *applied_count = applied;
    return err;
}

esp_err_t idf_config_factory_reset(void)
{
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_persist_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t nvs = 0;
    err = nvs_open("sms_config", NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = replace_config(IdfConfig());
        xSemaphoreGive(s_persist_mutex);
        return err;
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_persist_mutex);
        return err;
    }
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        esp_err_t set_err = replace_config(IdfConfig());
        if (set_err != ESP_OK) {
            err = set_err;
        } else {
            idf_log_line("配置已恢复出厂设置");
        }
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_set_keepalive_last(uint32_t epoch)
{
    // 只改这一个字段并只写这一个 NVS key：
    // 1) 快照-改-回写会和并发的 /save 互相覆盖(丢失更新)；2) 全量回写浪费 NVS 寿命
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_persist_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t nvs = 0;
    err = nvs_open("sms_config", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "kaLast", epoch);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }

    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.kaLastTime = epoch;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_set_sched_last(int index, uint32_t epoch)
{
    if (index < 0 || index >= IDF_MAX_SCHED_TASKS) return ESP_ERR_INVALID_ARG;
    // 同 idf_config_set_keepalive_last：单字段单 key 更新，避免丢失更新与全量回写
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_persist_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t nvs = 0;
    err = nvs_open("sms_config", NVS_READWRITE, &nvs);
    char key[16];
    snprintf(key, sizeof(key), "st%dLast", index);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, key, epoch);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }

    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.schedTasks[index].lastRun = epoch;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_set_net_led_enabled(bool enabled)
{
    // NET 灯只是一个布尔开关，单 key 写入即可；避免小表单走完整配置深拷贝/全量 NVS 回写
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_persist_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t nvs = 0;
    err = nvs_open("sms_config", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "netLed", enabled ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }

    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.netLedEnabled = enabled;
        xSemaphoreGive(s_config_mutex);
    } else {
        ESP_LOGE(TAG, "保存 NET 指示灯配置失败: %s", esp_err_to_name(err));
        idf_logf("保存 NET 指示灯配置失败: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_set_call_notify_enabled(bool enabled)
{
    // 来电通知同样是单布尔开关，单 key 写入即可，避免小表单走全量 NVS 回写
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_persist_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t nvs = 0;
    err = nvs_open("sms_config", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "callNotify", enabled ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }

    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.callNotifyEnabled = enabled;
        xSemaphoreGive(s_config_mutex);
    } else {
        ESP_LOGE(TAG, "保存来电通知配置失败: %s", esp_err_to_name(err));
        idf_logf("保存来电通知配置失败: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

static esp_err_t save_config_to_nvs(const IdfConfig& c)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("sms_config", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = ESP_OK;
    if (err == ESP_OK) err = write_str(nvs, "wifiSsid", c.wifiSsid);
    if (err == ESP_OK) err = write_str(nvs, "wifiPass", c.wifiPass);
    if (err == ESP_OK) err = write_str(nvs, "wifiSsid2", c.wifiSsid2);
    if (err == ESP_OK) err = write_str(nvs, "wifiPass2", c.wifiPass2);
    if (err == ESP_OK) err = write_str(nvs, "smtpServer", c.smtpServer);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "smtpPort", c.smtpPort);
    if (err == ESP_OK) err = write_str(nvs, "smtpUser", c.smtpUser);
    if (err == ESP_OK) err = write_str(nvs, "smtpPass", c.smtpPass);
    if (err == ESP_OK) err = write_str(nvs, "smtpSendTo", c.smtpSendTo);
    if (err == ESP_OK) err = write_str(nvs, "adminPhone", c.adminPhone);
    if (err == ESP_OK) err = write_str(nvs, "webUser", c.webUser);
    if (err == ESP_OK) err = write_str(nvs, "webPass", c.webPass);
    if (err == ESP_OK) err = write_str(nvs, "numBlkList", c.numberBlackList);
    if (err == ESP_OK) err = write_str(nvs, "fwdRules", c.forwardRules);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "emailEn", c.emailEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "pushEn", c.pushEnabled ? 1 : 0);

    if (err == ESP_OK) err = nvs_set_u8(nvs, "kaEn", c.kaEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "kaDays", c.kaIntervalDays);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "kaAct", c.kaAction);
    if (err == ESP_OK) err = write_str(nvs, "kaTarget", c.kaTarget);
    if (err == ESP_OK) err = write_str(nvs, "kaUrl", c.kaUrl.empty() ? IDF_KEEPALIVE_DEFAULT_URL : c.kaUrl);
    if (err == ESP_OK) err = write_str(nvs, "kaProfile", c.kaProfile);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "kaLast", c.kaLastTime);

    if (err == ESP_OK) err = nvs_set_i32(nvs, "tzMin", c.tzOffsetMin);
    if (err == ESP_OK) err = write_str(nvs, "ntpSrv", c.ntpServer);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "rbEn", c.rebootEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "rbHour", c.rebootHour);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "hbEn", c.hbEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "hbHour", c.hbHour);

    if (err == ESP_OK) err = nvs_set_u8(nvs, "netLed", c.netLedEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "callNotify", c.callNotifyEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "dataEn", c.dataEnabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "roamEn", c.roamingEnabled ? 1 : 0);
    if (err == ESP_OK) err = write_str(nvs, "apn", c.apn);
    if (err == ESP_OK) err = write_str(nvs, "opPlmn", c.operatorPlmn);
    if (err == ESP_OK) err = write_str(nvs, "phoneNum", c.phoneNumber);

    for (int i = 0; err == ESP_OK && i < IDF_MAX_PUSH_CHANNELS; ++i) {
        char prefix[12];
        snprintf(prefix, sizeof(prefix), "push%d", i);
        auto key = [&](const char* suffix) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%s%s", prefix, suffix);
            return std::string(buf);
        };
        const IdfPushChannel& ch = c.pushChannels[i];
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("en").c_str(), ch.enabled ? 1 : 0);
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("type").c_str(), ch.type);
        if (err == ESP_OK) err = write_str(nvs, key("url").c_str(), ch.url);
        if (err == ESP_OK) err = write_str(nvs, key("name").c_str(), ch.name);
        if (err == ESP_OK) err = write_str(nvs, key("k1").c_str(), ch.key1);
        if (err == ESP_OK) err = write_str(nvs, key("k2").c_str(), ch.key2);
        if (err == ESP_OK) err = write_str(nvs, key("body").c_str(), ch.customBody);
    }

    for (int i = 0; err == ESP_OK && i < IDF_MAX_SCHED_TASKS; ++i) {
        auto key = [&](const char* suffix) {
            char buf[16];
            snprintf(buf, sizeof(buf), "st%d%s", i, suffix);
            return std::string(buf);
        };
        const IdfSchedTask& t = c.schedTasks[i];
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("En").c_str(), t.enabled ? 1 : 0);
        if (err == ESP_OK) err = write_str(nvs, key("Name").c_str(), t.name);
        if (err == ESP_OK) err = write_str(nvs, key("Prof").c_str(), t.profile);
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("Back").c_str(), t.switchBack ? 1 : 0);
        if (err == ESP_OK) err = nvs_set_i32(nvs, key("Days").c_str(), t.intervalDays);
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("Act").c_str(), t.action);
        if (err == ESP_OK) err = write_str(nvs, key("Tgt").c_str(), t.target);
        if (err == ESP_OK) err = write_str(nvs, key("Pay").c_str(), t.payload);
        if (err == ESP_OK) err = nvs_set_u32(nvs, key("Last").c_str(), t.lastRun);
    }

    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存配置失败: %s", esp_err_to_name(err));
        idf_logf("保存配置失败: %s", esp_err_to_name(err));
    } else {
        idf_log_line("配置已保存");
    }
    return err;
}

static void merge_runtime_markers(IdfConfig& next, const IdfConfig& base)
{
    // kaLast/stXLast 是运行时进度。若本次保存没有主动改它们，就带上最新 RAM 值，
    // 避免慢速整包 NVS 保存把后台调度器刚写入的进度回滚。
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    if (next.kaLastTime == base.kaLastTime) next.kaLastTime = s_config.kaLastTime;
    for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) {
        if (next.schedTasks[i].lastRun == base.schedTasks[i].lastRun) {
            next.schedTasks[i].lastRun = s_config.schedTasks[i].lastRun;
        }
    }
    xSemaphoreGive(s_config_mutex);
}

static esp_err_t begin_field_save(nvs_handle_t* out)
{
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_persist_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t nvs = 0;
    err = nvs_open("sms_config", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        xSemaphoreGive(s_persist_mutex);
        return err;
    }
    *out = nvs;
    return ESP_OK;
}

static esp_err_t commit_field_save(nvs_handle_t nvs, esp_err_t err, const char* label)
{
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存%s失败: %s", label, esp_err_to_name(err));
        idf_logf("保存%s失败: %s", label, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t commit_config_update(IdfConfig& next, const IdfConfig& base)
{
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    normalize_config(next);

    if (xSemaphoreTake(s_persist_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    merge_runtime_markers(next, base);
    err = save_config_to_nvs(next);
    if (err == ESP_OK) err = replace_config(next);
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save(void)
{
    esp_err_t err = ensure_config_mutex();
    if (err != ESP_OK) return err;
    // 快照必须在 persist 锁内取：锁外快照到提交之间完成的单字段保存会被整体回写覆盖
    if (xSemaphoreTake(s_persist_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    IdfConfig next = idf_config_get();
    normalize_config(next);
    err = save_config_to_nvs(next);
    if (err == ESP_OK) err = replace_config(next);
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save_wifi(const std::string& ssid, const std::string& pass,
                               const std::string& ssid2, const std::string& pass2)
{
    if ((ssid.empty() && ssid2.empty()) || ssid.size() > 32 || ssid2.size() > 32 ||
        pass.size() > 64 || pass2.size() > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    std::string next_ssid = ssid;
    std::string next_pass = pass;
    std::string next_ssid2 = ssid2;
    std::string next_pass2 = pass2;
    limit_utf8_bytes(next_ssid, 64);
    limit_utf8_bytes(next_pass, 128);
    limit_utf8_bytes(next_ssid2, 64);
    limit_utf8_bytes(next_pass2, 128);

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = write_str(nvs, "wifiSsid", next_ssid);
    if (err == ESP_OK) err = write_str(nvs, "wifiPass", next_pass);
    if (err == ESP_OK) err = write_str(nvs, "wifiSsid2", next_ssid2);
    if (err == ESP_OK) err = write_str(nvs, "wifiPass2", next_pass2);
    err = commit_field_save(nvs, err, "WiFi 配置");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.wifiSsid = next_ssid;
        s_config.wifiPass = next_pass;
        s_config.wifiSsid2 = next_ssid2;
        s_config.wifiPass2 = next_pass2;
        s_config.wifiFromFallback = false;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save_account(const std::string& user, const std::string& pass)
{
    esp_err_t mutex_err = ensure_config_mutex();
    if (mutex_err != ESP_OK) return mutex_err;

    std::string next_user = user;
    std::string next_pass = pass;
    limit_utf8_bytes(next_user, 64);
    limit_utf8_bytes(next_pass, 128);

    if (is_blank(next_user) || is_blank(next_pass)) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        if (is_blank(next_user)) next_user = s_config.webUser;
        if (is_blank(next_pass)) next_pass = s_config.webPass;
        xSemaphoreGive(s_config_mutex);
    }
    if (is_blank(next_user)) next_user = IDF_DEFAULT_WEB_USER;
    if (is_blank(next_pass)) next_pass = IDF_DEFAULT_WEB_PASS;

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = write_str(nvs, "webUser", next_user);
    if (err == ESP_OK) err = write_str(nvs, "webPass", next_pass);
    err = commit_field_save(nvs, err, "管理账号");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.webUser = next_user;
        s_config.webPass = next_pass;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save_time(int tz_offset_min, const std::string& ntp_server)
{
    int next_tz = clamp_int(tz_offset_min, -720, 840);
    std::string next_ntp = ntp_server;
    limit_utf8_bytes(next_ntp, 128);

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = nvs_set_i32(nvs, "tzMin", next_tz);
    if (err == ESP_OK) err = write_str(nvs, "ntpSrv", next_ntp);
    err = commit_field_save(nvs, err, "时间设置");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.tzOffsetMin = next_tz;
        s_config.ntpServer = next_ntp;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save_email(bool enabled, const std::string& server, int port,
                                const std::string& user, const std::string& pass,
                                const std::string& send_to, bool preserve_blank_pass)
{
    esp_err_t mutex_err = ensure_config_mutex();
    if (mutex_err != ESP_OK) return mutex_err;

    std::string next_server = trim_copy(server);
    std::string next_user = user;
    std::string next_pass = pass;
    std::string next_send_to = send_to;
    int next_port = (port > 0 && port <= 65535) ? port : 465;
    limit_utf8_bytes(next_server, 128);
    limit_utf8_bytes(next_user, 128);
    limit_utf8_bytes(next_pass, 256);
    limit_utf8_bytes(next_send_to, 256);

    if (preserve_blank_pass && is_blank(next_pass)) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        next_pass = s_config.smtpPass;
        xSemaphoreGive(s_config_mutex);
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = write_str(nvs, "smtpServer", next_server);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "smtpPort", next_port);
    if (err == ESP_OK) err = write_str(nvs, "smtpUser", next_user);
    if (err == ESP_OK) err = write_str(nvs, "smtpPass", next_pass);
    if (err == ESP_OK) err = write_str(nvs, "smtpSendTo", next_send_to);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "emailEn", enabled ? 1 : 0);
    err = commit_field_save(nvs, err, "邮件配置");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.smtpServer = next_server;
        s_config.smtpPort = next_port;
        s_config.smtpUser = next_user;
        s_config.smtpPass = next_pass;
        s_config.smtpSendTo = next_send_to;
        s_config.emailEnabled = enabled;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

static void normalize_push_channel_for_save(IdfPushChannel& ch, int index)
{
    ch.type = (ch.type >= 1 && ch.type <= 10) ? ch.type : 1;
    ch.url = trim_copy(ch.url);
    ch.name = trim_copy(ch.name);
    ch.key1 = trim_copy(ch.key1);
    ch.key2 = trim_copy(ch.key2);
    if (ch.name.empty()) ch.name = channel_default_name(index);
    if (ch.type == 2 && ch.key1.empty() && !ch.url.empty() && !looks_like_url(ch.url)) {
        ch.key1 = ch.url;
        ch.url.clear();
    }
    if (ch.type == 6 && ch.key1.empty() && !ch.url.empty() && !looks_like_url(ch.url)) {
        ch.key1 = ch.url;
        ch.url.clear();
    }
    limit_utf8_bytes(ch.name, 64);
    limit_utf8_bytes(ch.url, 512);
    limit_utf8_bytes(ch.key1, 256);
    limit_utf8_bytes(ch.key2, 512);
    limit_utf8_bytes(ch.customBody, 1024);
}

esp_err_t idf_config_save_push(bool enabled, const IdfPushChannel channels[IDF_MAX_PUSH_CHANNELS])
{
    IdfPushChannel next[IDF_MAX_PUSH_CHANNELS];
    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        next[i] = channels[i];
        normalize_push_channel_for_save(next[i], i);
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = nvs_set_u8(nvs, "pushEn", enabled ? 1 : 0);
    for (int i = 0; err == ESP_OK && i < IDF_MAX_PUSH_CHANNELS; ++i) {
        char prefix[12];
        snprintf(prefix, sizeof(prefix), "push%d", i);
        auto key = [&](const char* suffix) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%s%s", prefix, suffix);
            return std::string(buf);
        };
        const IdfPushChannel& ch = next[i];
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("en").c_str(), ch.enabled ? 1 : 0);
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("type").c_str(), ch.type);
        if (err == ESP_OK) err = write_str(nvs, key("url").c_str(), ch.url);
        if (err == ESP_OK) err = write_str(nvs, key("name").c_str(), ch.name);
        if (err == ESP_OK) err = write_str(nvs, key("k1").c_str(), ch.key1);
        if (err == ESP_OK) err = write_str(nvs, key("k2").c_str(), ch.key2);
        if (err == ESP_OK) err = write_str(nvs, key("body").c_str(), ch.customBody);
    }
    err = commit_field_save(nvs, err, "推送通道");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.pushEnabled = enabled;
        for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) s_config.pushChannels[i] = next[i];
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save_filter(const std::string& admin_phone, const std::string& number_blacklist)
{
    std::string next_admin = admin_phone;
    std::string next_blacklist = number_blacklist;
    limit_utf8_bytes(next_admin, 64);
    limit_utf8_bytes(next_blacklist, 1024);

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = write_str(nvs, "adminPhone", next_admin);
    if (err == ESP_OK) err = write_str(nvs, "numBlkList", next_blacklist);
    err = commit_field_save(nvs, err, "权限与过滤");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.adminPhone = next_admin;
        s_config.numberBlackList = next_blacklist;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save_forward_rules(const std::string& rules)
{
    std::string next_rules = rules;
    limit_utf8_bytes(next_rules, 2048);

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = write_str(nvs, "fwdRules", next_rules);
    err = commit_field_save(nvs, err, "转发规则");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.forwardRules = next_rules;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save_keepalive(bool enabled, int interval_days, uint8_t action,
                                    const std::string& target, const std::string& url,
                                    const std::string& profile)
{
    esp_err_t mutex_err = ensure_config_mutex();
    if (mutex_err != ESP_OK) return mutex_err;

    int next_days = interval_days;
    uint8_t next_action = action;
    if (next_days < 1 || next_days > 3650 || next_action > 3) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        if (next_days < 1 || next_days > 3650) next_days = s_config.kaIntervalDays;
        if (next_action > 3) next_action = s_config.kaAction <= 3 ? s_config.kaAction : 1;
        xSemaphoreGive(s_config_mutex);
    }

    std::string next_target = target;
    std::string next_url = url.empty() ? std::string(IDF_KEEPALIVE_DEFAULT_URL) : url;
    std::string next_profile = trim_copy(profile);
    limit_utf8_bytes(next_target, 64);
    limit_utf8_bytes(next_url, 256);
    limit_utf8_bytes(next_profile, 64);

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = nvs_set_u8(nvs, "kaEn", enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "kaDays", next_days);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "kaAct", next_action);
    if (err == ESP_OK) err = write_str(nvs, "kaTarget", next_target);
    if (err == ESP_OK) err = write_str(nvs, "kaUrl", next_url);
    if (err == ESP_OK) err = write_str(nvs, "kaProfile", next_profile);
    err = commit_field_save(nvs, err, "保号任务");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.kaEnabled = enabled;
        s_config.kaIntervalDays = next_days;
        s_config.kaAction = next_action;
        s_config.kaTarget = next_target;
        s_config.kaUrl = next_url;
        s_config.kaProfile = next_profile;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save_system_schedule(bool reboot_enabled, int reboot_hour,
                                          bool hb_enabled, int hb_hour)
{
    int next_reboot_hour = clamp_int(reboot_hour, 0, 23);
    int next_hb_hour = clamp_int(hb_hour, 0, 23);

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = nvs_set_u8(nvs, "rbEn", reboot_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "rbHour", next_reboot_hour);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "hbEn", hb_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(nvs, "hbHour", next_hb_hour);
    err = commit_field_save(nvs, err, "系统定时");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.rebootEnabled = reboot_enabled;
        s_config.rebootHour = next_reboot_hour;
        s_config.hbEnabled = hb_enabled;
        s_config.hbHour = next_hb_hour;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save_sched_tasks(const IdfSchedTask tasks[IDF_MAX_SCHED_TASKS])
{
    esp_err_t mutex_err = ensure_config_mutex();
    if (mutex_err != ESP_OK) return mutex_err;

    IdfSchedTask next[IDF_MAX_SCHED_TASKS];
    bool was_enabled[IDF_MAX_SCHED_TASKS] = {};
    uint32_t last_run[IDF_MAX_SCHED_TASKS] = {};
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) {
        was_enabled[i] = s_config.schedTasks[i].enabled;
        last_run[i] = s_config.schedTasks[i].lastRun;
    }
    xSemaphoreGive(s_config_mutex);

    uint32_t now = static_cast<uint32_t>(time(nullptr));
    for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) {
        next[i] = tasks[i];
        limit_utf8_bytes(next[i].name, 32);
        limit_utf8_bytes(next[i].profile, 64);
        next[i].intervalDays = clamp_int(next[i].intervalDays, 1, 3650);
        if (next[i].action > 3) next[i].action = 0;
        limit_utf8_bytes(next[i].target, 128);
        limit_utf8_bytes(next[i].payload, 128);
        next[i].lastRun = last_run[i];
        if (next[i].enabled && !was_enabled[i] && next[i].lastRun == 0 && now >= 1700000000u) {
            next[i].lastRun = now;
        }
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    for (int i = 0; err == ESP_OK && i < IDF_MAX_SCHED_TASKS; ++i) {
        auto key = [&](const char* suffix) {
            char buf[16];
            snprintf(buf, sizeof(buf), "st%d%s", i, suffix);
            return std::string(buf);
        };
        const IdfSchedTask& t = next[i];
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("En").c_str(), t.enabled ? 1 : 0);
        if (err == ESP_OK) err = write_str(nvs, key("Name").c_str(), t.name);
        if (err == ESP_OK) err = write_str(nvs, key("Prof").c_str(), t.profile);
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("Back").c_str(), t.switchBack ? 1 : 0);
        if (err == ESP_OK) err = nvs_set_i32(nvs, key("Days").c_str(), t.intervalDays);
        if (err == ESP_OK) err = nvs_set_u8(nvs, key("Act").c_str(), t.action);
        if (err == ESP_OK) err = write_str(nvs, key("Tgt").c_str(), t.target);
        if (err == ESP_OK) err = write_str(nvs, key("Pay").c_str(), t.payload);
        if (err == ESP_OK) err = nvs_set_u32(nvs, key("Last").c_str(), t.lastRun);
    }
    err = commit_field_save(nvs, err, "自定义定时任务");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) s_config.schedTasks[i] = next[i];
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

esp_err_t idf_config_save_sim(bool data_enabled, bool roaming_enabled, const std::string& apn,
                              const std::string& operator_plmn, const std::string& phone_number)
{
    std::string next_apn = apn;
    std::string next_operator = operator_plmn;
    std::string next_phone = phone_number;
    limit_utf8_bytes(next_apn, 96);
    limit_utf8_bytes(next_operator, 16);
    limit_utf8_bytes(next_phone, 64);

    nvs_handle_t nvs = 0;
    esp_err_t err = begin_field_save(&nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = nvs_set_u8(nvs, "dataEn", data_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "roamEn", roaming_enabled ? 1 : 0);
    if (err == ESP_OK) err = write_str(nvs, "apn", next_apn);
    if (err == ESP_OK) err = write_str(nvs, "opPlmn", next_operator);
    if (err == ESP_OK) err = write_str(nvs, "phoneNum", next_phone);
    err = commit_field_save(nvs, err, "蜂窝设置");
    if (err == ESP_OK) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
        s_config.dataEnabled = data_enabled;
        s_config.roamingEnabled = roaming_enabled;
        s_config.apn = next_apn;
        s_config.operatorPlmn = next_operator;
        s_config.phoneNumber = next_phone;
        xSemaphoreGive(s_config_mutex);
    }
    xSemaphoreGive(s_persist_mutex);
    return err;
}

IdfConfig idf_config_get(void)
{
    return config_snapshot();
}

static bool email_configured_locked()
{
    return !s_config.smtpServer.empty() && !s_config.smtpUser.empty() &&
           !s_config.smtpPass.empty() && !s_config.smtpSendTo.empty();
}

static int enabled_push_count_locked()
{
    int count = 0;
    for (const auto& ch : s_config.pushChannels) {
        if (ch.enabled) ++count;
    }
    return count;
}

IdfConfigStatusView idf_config_get_status_view(void)
{
    IdfConfigStatusView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.tzOffsetMin = s_config.tzOffsetMin;
    view.dataEnabled = s_config.dataEnabled;
    view.emailEnabled = s_config.emailEnabled;
    view.pushEnabled = s_config.pushEnabled;
    view.adminPhone = s_config.adminPhone;
    view.phoneNumber = s_config.phoneNumber;
    view.apn = s_config.apn;
    // 一次持锁取齐 /status 所需派生值：既省两次锁往返，也保证同一快照内自洽
    view.emailConfigured = email_configured_locked();
    view.pushEnabledCount = enabled_push_count_locked();
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfConfigWebView idf_config_get_web_view(void)
{
    IdfConfigWebView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.wifiSsid = s_config.wifiSsid;
    view.wifiSsid2 = s_config.wifiSsid2;
    view.webUser = s_config.webUser;
    view.webPass = s_config.webPass;
    view.smtpServer = s_config.smtpServer;
    view.smtpPort = s_config.smtpPort;
    view.smtpUser = s_config.smtpUser;
    view.smtpPass = s_config.smtpPass;
    view.smtpSendTo = s_config.smtpSendTo;
    view.adminPhone = s_config.adminPhone;
    view.numberBlackList = s_config.numberBlackList;
    view.forwardRules = s_config.forwardRules;
    view.emailEnabled = s_config.emailEnabled;
    view.pushEnabled = s_config.pushEnabled;
    view.ntpServer = s_config.ntpServer;
    view.tzOffsetMin = s_config.tzOffsetMin;
    view.rebootEnabled = s_config.rebootEnabled;
    view.rebootHour = s_config.rebootHour;
    view.hbEnabled = s_config.hbEnabled;
    view.hbHour = s_config.hbHour;
    view.dataEnabled = s_config.dataEnabled;
    view.roamingEnabled = s_config.roamingEnabled;
    view.apn = s_config.apn;
    view.phoneNumber = s_config.phoneNumber;
    view.operatorPlmn = s_config.operatorPlmn;
    view.kaProfile = s_config.kaProfile;
    view.netLedEnabled = s_config.netLedEnabled;
    view.callNotifyEnabled = s_config.callNotifyEnabled;
    view.emailConfigured = email_configured_locked();
    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        view.pushChannels[i] = s_config.pushChannels[i];
    }
    view.pushEnabledCount = enabled_push_count_locked();
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfSchedStatusView idf_config_get_sched_view(void)
{
    IdfSchedStatusView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) view.tasks[i] = s_config.schedTasks[i];
    view.tzOffsetMin = s_config.tzOffsetMin;
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfKeepaliveStatusView idf_config_get_keepalive_status_view(void)
{
    IdfKeepaliveStatusView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.kaEnabled = s_config.kaEnabled;
    view.kaIntervalDays = s_config.kaIntervalDays;
    view.kaAction = s_config.kaAction;
    view.kaTarget = s_config.kaTarget;
    view.kaUrl = s_config.kaUrl;
    view.kaProfile = s_config.kaProfile;
    view.kaLastTime = s_config.kaLastTime;
    view.tzOffsetMin = s_config.tzOffsetMin;
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfKeepaliveRunView idf_config_get_keepalive_run_view(void)
{
    IdfKeepaliveRunView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.kaEnabled = s_config.kaEnabled;
    view.kaIntervalDays = s_config.kaIntervalDays;
    view.kaAction = s_config.kaAction;
    view.kaTarget = s_config.kaTarget;
    view.kaUrl = s_config.kaUrl;
    view.kaProfile = s_config.kaProfile;
    view.kaLastTime = s_config.kaLastTime;
    view.tzOffsetMin = s_config.tzOffsetMin;
    view.emailEnabled = s_config.emailEnabled;
    view.dataEnabled = s_config.dataEnabled;
    view.apn = s_config.apn;
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfSchedRunView idf_config_get_sched_run_view(int index)
{
    IdfSchedRunView view;
    if (index < 0 || index >= IDF_MAX_SCHED_TASKS) return view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.valid = true;
    view.task = s_config.schedTasks[index];
    view.kaUrl = s_config.kaUrl;
    view.tzOffsetMin = s_config.tzOffsetMin;
    view.emailEnabled = s_config.emailEnabled;
    view.emailConfigured = email_configured_locked();
    view.dataEnabled = s_config.dataEnabled;
    view.apn = s_config.apn;
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfSimSettingsView idf_config_get_sim_settings_view(void)
{
    IdfSimSettingsView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.dataEnabled = s_config.dataEnabled;
    view.roamingEnabled = s_config.roamingEnabled;
    view.apn = s_config.apn;
    view.operatorPlmn = s_config.operatorPlmn;
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfSmsProcessView idf_config_get_sms_process_view(void)
{
    IdfSmsProcessView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.adminPhone = s_config.adminPhone;
    view.numberBlackList = s_config.numberBlackList;
    view.tzOffsetMin = s_config.tzOffsetMin;
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfPushForwardView idf_config_get_push_forward_view(void)
{
    IdfPushForwardView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.forwardRules = s_config.forwardRules;
    view.pushEnabled = s_config.pushEnabled;
    view.emailEnabled = s_config.emailEnabled;
    view.emailConfigured = email_configured_locked();
    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) view.pushChannels[i] = s_config.pushChannels[i];
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfPushNotifyView idf_config_get_push_notify_view(void)
{
    IdfPushNotifyView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.pushEnabled = s_config.pushEnabled;
    view.tzOffsetMin = s_config.tzOffsetMin;
    for (int i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) view.pushChannels[i] = s_config.pushChannels[i];
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfEmailSettingsView idf_config_get_email_settings_view(void)
{
    IdfEmailSettingsView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.emailEnabled = s_config.emailEnabled;
    view.emailConfigured = email_configured_locked();
    view.smtpServer = s_config.smtpServer;
    view.smtpPort = s_config.smtpPort;
    view.smtpUser = s_config.smtpUser;
    view.smtpPass = s_config.smtpPass;
    view.smtpSendTo = s_config.smtpSendTo;
    xSemaphoreGive(s_config_mutex);
    return view;
}

IdfSchedulerView idf_config_get_scheduler_view(void)
{
    IdfSchedulerView view;
    if (ensure_config_mutex() != ESP_OK) return view;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    view.kaEnabled = s_config.kaEnabled;
    view.kaIntervalDays = s_config.kaIntervalDays;
    view.kaLastTime = s_config.kaLastTime;
    view.tzOffsetMin = s_config.tzOffsetMin;
    view.rebootEnabled = s_config.rebootEnabled;
    view.rebootHour = s_config.rebootHour;
    view.hbEnabled = s_config.hbEnabled;
    view.hbHour = s_config.hbHour;
    view.emailEnabled = s_config.emailEnabled;
    for (int i = 0; i < IDF_MAX_SCHED_TASKS; ++i) view.schedTasks[i] = s_config.schedTasks[i];
    xSemaphoreGive(s_config_mutex);
    return view;
}

bool idf_config_get_push_channel(uint8_t channel, IdfPushChannel& out)
{
    if (channel >= IDF_MAX_PUSH_CHANNELS) return false;
    if (ensure_config_mutex() != ESP_OK) return false;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    out = s_config.pushChannels[channel];
    xSemaphoreGive(s_config_mutex);
    return true;
}

// 以下布尔/小字段访问器都在锁内直接求值：全量快照要深拷贝 42 个 std::string，
// 在每个 HTTP 请求上都做一次会造成持续的堆分配抖动与碎片化
bool idf_config_has_sta_credentials(void)
{
    if (ensure_config_mutex() != ESP_OK) return false;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    bool ok = !s_config.wifiSsid.empty() || !s_config.wifiSsid2.empty();
    xSemaphoreGive(s_config_mutex);
    return ok;
}

bool idf_config_net_led_enabled(void)
{
    if (ensure_config_mutex() != ESP_OK) return true;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    bool on = s_config.netLedEnabled;
    xSemaphoreGive(s_config_mutex);
    return on;
}

bool idf_config_call_notify_enabled(void)
{
    if (ensure_config_mutex() != ESP_OK) return true;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    bool on = s_config.callNotifyEnabled;
    xSemaphoreGive(s_config_mutex);
    return on;
}

// 时区/NTP 窄访问器：SNTP 同步回调跑在 tiT(lwip) 任务、事件回调跑在系统事件任务，
// 两者栈都很小(约 3.5KB / 4KB)。全量 idf_config_get() 会把含多个定时任务的大 IdfConfig
// 深拷贝上这些小栈——任务数上调后直接爆栈(Stack protection fault)。这里只锁内取所需字段。
int idf_config_get_tz_offset(void)
{
    if (ensure_config_mutex() != ESP_OK) return 480;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    int tz = s_config.tzOffsetMin;
    xSemaphoreGive(s_config_mutex);
    return tz;
}

std::string idf_config_get_ntp_server(void)
{
    if (ensure_config_mutex() != ESP_OK) return std::string();
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    std::string server = s_config.ntpServer;
    xSemaphoreGive(s_config_mutex);
    return server;
}

bool idf_config_email_configured(void)
{
    if (ensure_config_mutex() != ESP_OK) return false;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    bool ok = email_configured_locked();
    xSemaphoreGive(s_config_mutex);
    return ok;
}

int idf_config_enabled_push_count(void)
{
    if (ensure_config_mutex() != ESP_OK) return 0;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    int count = enabled_push_count_locked();
    xSemaphoreGive(s_config_mutex);
    return count;
}

// 常数时间比对：逐字节累积差异，不因首字节不匹配提前返回，避免计时侧信道
static bool timing_safe_equals(const std::string& expected, const char* actual)
{
    size_t actual_len = strlen(actual);
    unsigned char diff = (expected.size() == actual_len) ? 0 : 1;
    for (size_t i = 0; i < expected.size(); ++i) {
        unsigned char b = actual_len ? static_cast<unsigned char>(actual[i % actual_len]) : 0;
        diff |= static_cast<unsigned char>(expected[i]) ^ b;
    }
    return diff == 0;
}

bool idf_config_check_web_auth(const char* user, const char* pass)
{
    if (!user || !pass) return false;
    if (ensure_config_mutex() != ESP_OK) return false;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    bool user_ok = timing_safe_equals(s_config.webUser, user);
    bool pass_ok = timing_safe_equals(s_config.webPass, pass);
    xSemaphoreGive(s_config_mutex);
    return user_ok && pass_ok;
}
