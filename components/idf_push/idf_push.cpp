#include "idf_push.h"

#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <inttypes.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "idf_config.h"
#include "idf_inbox.h"
#include "idf_log.h"
#include "idf_modem.h"
#include "idf_wifi.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/md.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static constexpr size_t PUSH_QUEUE_MAX = 16;
static constexpr size_t FWD_QUEUE_MAX = 10;
static constexpr size_t EMAIL_QUEUE_MAX = 5;
// 重试：20s 起步指数退避(20/40/80/160/320s)，6 次共覆盖约 10 分钟的
// 路由器重启/宽带闪断窗口——短信转发宁可迟到也不要丢
static constexpr uint8_t PUSH_RETRY_MAX = 6;
static constexpr uint32_t PUSH_RETRY_BASE_SEC = 20;
static constexpr uint32_t PUSH_RETRY_MAX_SEC = 600;
static constexpr int HTTP_TIMEOUT_MS = 5000;
static constexpr int SMTP_TIMEOUT_MS = 15000;
// 单次 SMTP 会话总时长硬上限：所有收发环节共享。没有它，"活着但极慢"的
// 服务器(每次只收几字节/逐行慢吐)能把唯一的推送 worker 卡住几分钟到无限,
// 转发队列随之塞满，且 s_busy 常真还会抑制低内存自愈重启
static constexpr int64_t SMTP_SESSION_MAX_US = 120LL * 1000LL * 1000LL;
static int64_t s_smtp_session_deadline = 0;  // 仅推送 worker 单任务访问
static constexpr uint32_t PUSH_WORKER_STACK = 10240;
// 最大可分配块低于该值时推迟 TLS 发送，避免握手中途 OOM(mbedTLS 握手峰值需 ~40KB)
static constexpr size_t TLS_MIN_FREE_HEAP = 50000;
// 通道失败冷却：每次失败即冷却 30s×连续失败数(封顶 300s)，成功清零。
// 目的：死端点的新任务不再背靠背烧满 HTTP 超时、堵住唯一的发送 worker
static constexpr uint32_t CHANNEL_COOL_STEP_SEC = 30;
static constexpr uint32_t CHANNEL_COOL_MAX_SEC = 300;

enum : uint8_t {
    PUSH_TYPE_NONE = 0,
    PUSH_TYPE_POST_JSON = 1,
    PUSH_TYPE_BARK = 2,
    PUSH_TYPE_GET = 3,
    PUSH_TYPE_DINGTALK = 4,
    PUSH_TYPE_PUSHPLUS = 5,
    PUSH_TYPE_SERVERCHAN = 6,
    PUSH_TYPE_CUSTOM = 7,
    PUSH_TYPE_FEISHU = 8,
    PUSH_TYPE_GOTIFY = 9,
    PUSH_TYPE_TELEGRAM = 10,
};

struct PushJob {
    bool used = false;
    uint8_t channel = 0;
    uint8_t attempts = 0;
    bool notify = false;  // true=自定义提醒(标题即任务名,不套"短信来自"模板) false=转发短信
    int64_t nextUs = 0;
    std::string sender;
    std::string text;
    std::string timestamp;
    uint32_t inboxId = 0;  // 关联的收件箱条目；最终放弃时回改"未转发"供手动重发
    uint32_t completionId = 0;
};

struct ForwardJob {
    bool used = false;
    bool pushQueued = false;
    uint8_t attempts = 0;
    int64_t nextUs = 0;
    std::string sender;
    std::string text;
    std::string timestamp;
    uint32_t inboxId = 0;
};

struct EmailJob {
    bool used = false;
    uint8_t attempts = 0;
    int64_t nextUs = 0;
    std::string subject;
    std::string body;
    uint32_t inboxId = 0;  // 同 PushJob：投递最终失败时回改收件箱标记
    uint32_t completionId = 0;
};

struct ForwardCompletion {
    bool used = false;
    uint32_t id = 0;
    uint32_t inboxId = 0;
    uint8_t remaining = 0;
};

struct TestJob {
    bool pending = false;
    bool running = false;
    bool done = false;
    bool success = false;
    std::string message;
};

struct ForwardDecision {
    bool matched = false;
    bool drop = false;
    uint32_t chMask = 0;
    bool email = false;
};

static SemaphoreHandle_t s_mutex = nullptr;
// worker 唤醒信号：新任务入队即刻开始处理，不等 100ms 空闲轮询
static SemaphoreHandle_t s_wake_sem = nullptr;
static std::array<PushJob, PUSH_QUEUE_MAX> s_push_jobs;
static std::array<ForwardJob, FWD_QUEUE_MAX> s_forward_jobs;
static std::array<EmailJob, EMAIL_QUEUE_MAX> s_email_jobs;
static std::array<TestJob, IDF_MAX_PUSH_CHANNELS> s_test_jobs;
static std::array<ForwardCompletion, PUSH_QUEUE_MAX + EMAIL_QUEUE_MAX> s_forward_completions;
static bool s_started = false;
static uint32_t s_next_completion_id = 0;
static std::atomic<bool> s_busy{false};
// 通道连续失败冷却状态（仅推送 worker 单任务读写，无需加锁）
static uint8_t s_channel_fails[IDF_MAX_PUSH_CHANNELS] = {};
static int64_t s_channel_cool_until_us[IDF_MAX_PUSH_CHANNELS] = {};

static bool ensure_init()
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return false;
    }
    if (!s_wake_sem) {
        s_wake_sem = xSemaphoreCreateBinary();
        if (!s_wake_sem) return false;
    }
    return true;
}

static void cleanup_start_resources()
{
    if (s_wake_sem) {
        vSemaphoreDelete(s_wake_sem);
        s_wake_sem = nullptr;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = nullptr;
    }
    s_push_jobs = {};
    s_forward_jobs = {};
    s_email_jobs = {};
    s_test_jobs = {};
    s_forward_completions = {};
    s_next_completion_id = 0;
    memset(s_channel_fails, 0, sizeof(s_channel_fails));
    memset(s_channel_cool_until_us, 0, sizeof(s_channel_cool_until_us));
    s_busy.store(false, std::memory_order_relaxed);
}

static void wake_worker()
{
    if (s_wake_sem) xSemaphoreGive(s_wake_sem);
}

// TLS 发送前的低堆保护：最大可分配块不足时推迟发送（任务留在队列里稍后重试）
static bool tls_heap_ok()
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= TLS_MIN_FREE_HEAP;
}

static void note_channel_result(uint8_t ch, bool ok)
{
    if (ch >= IDF_MAX_PUSH_CHANNELS) return;
    if (ok) {
        s_channel_fails[ch] = 0;
        s_channel_cool_until_us[ch] = 0;
        return;
    }
    if (s_channel_fails[ch] < 255) s_channel_fails[ch]++;
    uint32_t cool = CHANNEL_COOL_STEP_SEC * s_channel_fails[ch];
    if (cool > CHANNEL_COOL_MAX_SEC) cool = CHANNEL_COOL_MAX_SEC;
    s_channel_cool_until_us[ch] = esp_timer_get_time() + static_cast<int64_t>(cool) * 1000000LL;
}

static bool channel_cooling(uint8_t ch, int64_t now)
{
    return ch < IDF_MAX_PUSH_CHANNELS && s_channel_cool_until_us[ch] > now;
}

static uint32_t register_forward_completion_locked(uint32_t inbox_id, uint8_t total_targets)
{
    if (inbox_id == 0 || total_targets == 0) return 0;
    for (auto& item : s_forward_completions) {
        if (item.used) continue;
        uint32_t next_id = ++s_next_completion_id;
        if (next_id == 0) next_id = ++s_next_completion_id;
        item.used = true;
        item.id = next_id;
        item.inboxId = inbox_id;
        item.remaining = total_targets;
        return item.id;
    }
    return 0;
}

static void note_forward_target_success(uint32_t completion_id)
{
    if (completion_id == 0) return;
    uint32_t inbox_id_to_mark = 0;
    ensure_init();
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        for (auto& item : s_forward_completions) {
            if (!item.used || item.id != completion_id) continue;
            if (item.remaining > 0) --item.remaining;
            if (item.remaining == 0) {
                inbox_id_to_mark = item.inboxId;
                item = ForwardCompletion();
            }
            break;
        }
        xSemaphoreGive(s_mutex);
    }
    if (inbox_id_to_mark != 0) idf_inbox_mark_forwarded(inbox_id_to_mark);
}

static void cancel_forward_completion_locked(uint32_t completion_id)
{
    if (completion_id == 0) return;
    for (auto& item : s_forward_completions) {
        if (item.used && item.id == completion_id) {
            item = ForwardCompletion();
            break;
        }
    }
}

static void cancel_forward_completion(uint32_t completion_id)
{
    if (completion_id == 0) return;
    ensure_init();
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        cancel_forward_completion_locked(completion_id);
        xSemaphoreGive(s_mutex);
    }
}

// "YYYY-MM-DD HH:MM:SS" 本地时间；时间未同步返回空串
static std::string format_local_time(int tz_offset_min)
{
    time_t now = time(nullptr);
    if (now < 1700000000) return {};
    time_t shifted = now + static_cast<time_t>(tz_offset_min) * 60;
    struct tm tmv = {};
    gmtime_r(&shifted, &tmv);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

// 在 UTF-8 字符边界截断，避免推送/邮件主题里出现半个汉字
static std::string utf8_truncate(const std::string& value, size_t max_bytes)
{
    if (value.size() <= max_bytes) return value;
    size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) --end;
    return value.substr(0, end) + "...";
}

static std::string trim(std::string value)
{
    size_t start = 0;
    while (start < value.size() && isspace(static_cast<unsigned char>(value[start]))) ++start;
    size_t end = value.size();
    while (end > start && isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

static std::string local_phone_number()
{
    IdfModemStatus modem = idf_modem_get_status();
    if (!modem.phone.empty()) return modem.phone;
    return idf_config_get_status_view().phoneNumber;
}

static bool parse_push_channel_token(const std::string& value, uint8_t& channel)
{
    if (value.empty()) return false;
    uint32_t parsed = 0;
    for (char ch : value) {
        if (!isdigit(static_cast<unsigned char>(ch))) return false;
        parsed = parsed * 10U + static_cast<uint32_t>(ch - '0');
        if (parsed > IDF_MAX_PUSH_CHANNELS) return false;
    }
    if (parsed == 0) return false;
    channel = static_cast<uint8_t>(parsed);
    return true;
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

static std::string json_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    json_escape_append(out, value);
    return out;
}

// 短信内容不可信，进入 HTML 模板的通道(如 pushplus)必须先转义标签字符
static std::string html_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 16);
    for (char ch : value) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += ch; break;
        }
    }
    return out;
}

static void json_prop(std::string& out, const char* key, const std::string& value)
{
    out += "\"";
    out += key;
    out += "\":\"";
    json_escape_append(out, value);
    out += "\"";
}

static std::string url_encode(const std::string& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char ch : value) {
        if (isalnum(ch)) out += static_cast<char>(ch);
        else if (ch == ' ') out += '+';
        else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 0x0F];
        }
    }
    return out;
}

static std::string hmac_sha256_base64(const std::string& data, const std::string& key)
{
    unsigned char hmac[32] = {};
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, 1);
    mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char*>(key.data()), key.size());
    mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size());
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);

    unsigned char out[64] = {};
    size_t out_len = 0;
    if (mbedtls_base64_encode(out, sizeof(out), &out_len, hmac, sizeof(hmac)) != 0) return {};
    return std::string(reinterpret_cast<char*>(out), out_len);
}

static int64_t utc_millis()
{
    timeval tv = {};
    if (gettimeofday(&tv, nullptr) == 0) return static_cast<int64_t>(tv.tv_sec) * 1000LL + tv.tv_usec / 1000;
    return static_cast<int64_t>(time(nullptr)) * 1000LL;
}

static bool is_url_like(const std::string& value)
{
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

static size_t url_path_start(const std::string& url)
{
    size_t scheme = url.find("://");
    size_t host = scheme == std::string::npos ? 0 : scheme + 3;
    return url.find('/', host);
}

static std::string url_path_without_query(const std::string& url)
{
    size_t start = url_path_start(url);
    if (start == std::string::npos) return {};
    size_t end = url.find_first_of("?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static bool bark_url_path_is_push(const std::string& url)
{
    std::string path = url_path_without_query(url);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path == "/push" || (path.size() > 5 && path.compare(path.size() - 5, 5, "/push") == 0);
}

static bool bark_url_has_key_path(const std::string& url)
{
    std::string path = url_path_without_query(url);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return !path.empty() && path != "/" && !bark_url_path_is_push(url);
}

static std::string bark_push_endpoint(std::string base)
{
    size_t suffix_pos = base.find_first_of("?#");
    std::string suffix;
    if (suffix_pos != std::string::npos) {
        suffix = base.substr(suffix_pos);
        base.erase(suffix_pos);
    }
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/push" + suffix;
}

struct BarkTarget {
    bool ok = false;
    std::string url;
    std::string device_key;
};

static BarkTarget bark_target_from_channel(const IdfPushChannel& ch)
{
    BarkTarget target;
    std::string key = trim(ch.key1);
    std::string raw_url = trim(ch.url);
    if (!key.empty()) {
        if (raw_url.empty()) raw_url = "https://api.day.app";
        if (!is_url_like(raw_url)) return target;
        target.url = bark_url_path_is_push(raw_url) ? raw_url : bark_push_endpoint(raw_url);
        target.device_key = key;
        target.ok = true;
        return target;
    }

    // 兼容旧配置：URL 框里直接填 https://api.day.app/<key> 时仍按原端点发送。
    if (raw_url.empty() || !is_url_like(raw_url) || !bark_url_has_key_path(raw_url)) return target;
    target.url = raw_url;
    target.ok = true;
    return target;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static std::string url_decode_component(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        char ch = value[i];
        if (ch == '+') {
            out += ' ';
        } else if (ch == '%' && i + 2 < value.size()) {
            int hi = hex_value(value[i + 1]);
            int lo = hex_value(value[i + 2]);
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

static bool is_integer_literal(const std::string& value)
{
    if (value.empty()) return false;
    size_t i = (value[0] == '-' || value[0] == '+') ? 1 : 0;
    if (i >= value.size()) return false;
    for (; i < value.size(); ++i) {
        if (!isdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

static bool bark_numeric_param(const std::string& key)
{
    return key == "badge" || key == "volume" || key == "ttl";
}

static bool bark_reserved_param(const std::string& key)
{
    return key == "title" || key == "body" || key == "device_key" || key == "device_keys";
}

static std::string apply_push_placeholders(const std::string& value, const std::string& sender,
                                           const std::string& text, const std::string& timestamp,
                                           const std::string& receiver)
{
    // 单次扫描替换，避免发送者/内容里出现的 {message} 等字面量被二次展开
    std::string out;
    out.reserve(value.size() + text.size() + receiver.size());
    size_t pos = 0;
    while (pos < value.size()) {
        size_t brace = value.find('{', pos);
        if (brace == std::string::npos) {
            out.append(value, pos, std::string::npos);
            break;
        }
        out.append(value, pos, brace - pos);
        if (value.compare(brace, 8, "{sender}") == 0) { out += sender; pos = brace + 8; }
        else if (value.compare(brace, 9, "{message}") == 0) { out += text; pos = brace + 9; }
        else if (value.compare(brace, 11, "{timestamp}") == 0) { out += timestamp; pos = brace + 11; }
        else if (value.compare(brace, 10, "{receiver}") == 0) { out += receiver; pos = brace + 10; }
        else if (value.compare(brace, 14, "{local_number}") == 0) { out += receiver; pos = brace + 14; }
        else { out += '{'; pos = brace + 1; }
    }
    return out;
}

static void append_bark_params(std::string& json, const std::string& params,
                               const std::string& sender, const std::string& text,
                               const std::string& timestamp, const std::string& receiver)
{
    std::string spec = trim(params);
    if (!spec.empty() && spec[0] == '?') spec.erase(0, 1);
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t end = spec.find_first_of("&\n", pos);
        if (end == std::string::npos) end = spec.size();
        std::string item = trim(spec.substr(pos, end - pos));
        pos = end + (end < spec.size() ? 1 : 0);
        if (item.empty()) {
            if (end == spec.size()) break;
            continue;
        }

        size_t eq = item.find('=');
        std::string key = trim(url_decode_component(eq == std::string::npos ? item : item.substr(0, eq)));
        if (key.empty() || bark_reserved_param(key)) {
            if (end == spec.size()) break;
            continue;
        }

        std::string value = eq == std::string::npos ? "1" : url_decode_component(item.substr(eq + 1));
        value = apply_push_placeholders(trim(value), sender, text, timestamp, receiver);
        json += ",";
        json += "\"";
        json_escape_append(json, key);
        if (bark_numeric_param(key) && is_integer_literal(value)) {
            json += "\":";
            json += value;
        } else {
            json += "\":\"";
            json_escape_append(json, value);
            json += "\"";
        }

        if (end == spec.size()) break;
    }
}

static bool channel_valid(const IdfPushChannel& ch)
{
    if (!ch.enabled || ch.type == PUSH_TYPE_NONE) return false;
    if (ch.type == PUSH_TYPE_BARK) return bark_target_from_channel(ch).ok;
    bool needs_url = ch.type == PUSH_TYPE_POST_JSON || ch.type == PUSH_TYPE_GET || ch.type == PUSH_TYPE_DINGTALK ||
                     ch.type == PUSH_TYPE_CUSTOM || ch.type == PUSH_TYPE_FEISHU ||
                     ch.type == PUSH_TYPE_GOTIFY;
    if (needs_url && ch.url.empty()) return false;
    if (ch.type == PUSH_TYPE_CUSTOM && ch.customBody.empty()) return false;
    if (ch.type == PUSH_TYPE_PUSHPLUS && ch.key1.empty()) return false;
    if (ch.type == PUSH_TYPE_SERVERCHAN && ch.key1.empty() && ch.url.empty()) return false;
    if (ch.type == PUSH_TYPE_GOTIFY && ch.key1.empty()) return false;
    if (ch.type == PUSH_TYPE_TELEGRAM && (ch.key1.empty() || ch.key2.empty())) return false;
    return true;
}

// POSIX ERE 不认识 Perl 风格 \d \w \s；Arduino 版自研引擎支持这些转义，
// 迁移前用户已存的转发规则依赖它们，这里先翻译成 POSIX 字符类再编译。
static std::string translate_perl_classes(const std::string& pattern)
{
    std::string out;
    out.reserve(pattern.size() + 16);
    bool in_bracket = false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        char ch = pattern[i];
        if (ch == '[' && !in_bracket) { in_bracket = true; out += ch; continue; }
        if (ch == ']' && in_bracket) { in_bracket = false; out += ch; continue; }
        if (ch != '\\' || i + 1 >= pattern.size()) { out += ch; continue; }
        char next = pattern[i + 1];
        const char* body = nullptr;   // 字符类内部内容
        const char* neg = nullptr;    // 取反形式(仅括号外可表达)
        switch (next) {
            case 'd': body = "0-9"; break;
            case 'D': neg = "0-9"; break;
            case 'w': body = "A-Za-z0-9_"; break;
            case 'W': neg = "A-Za-z0-9_"; break;
            case 's': body = " \t\r\n\f\v"; break;
            case 'S': neg = " \t\r\n\f\v"; break;
            default: out += ch; out += next; ++i; continue;
        }
        if (in_bracket) {
            // 括号内只能展开正类；取反形式无法表达，原样保留
            if (body) { out += body; ++i; }
            else { out += ch; out += next; ++i; }
        } else {
            out += '[';
            if (neg) { out += '^'; out += neg; }
            else out += body;
            out += ']';
            ++i;
        }
    }
    return out;
}

static bool regex_search_case_insensitive(const std::string& pattern, const std::string& text)
{
    std::string posix = translate_perl_classes(pattern);
    regex_t re = {};
    if (regcomp(&re, posix.c_str(), REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0) return false;
    bool hit = regexec(&re, text.c_str(), 0, nullptr, 0) == 0;
    regfree(&re);
    return hit;
}

static ForwardDecision eval_forward_rules(const std::string& rules, const std::string& sender, const std::string& body)
{
    ForwardDecision d;
    size_t pos = 0;
    while (pos < rules.size()) {
        size_t end = rules.find('\n', pos);
        if (end == std::string::npos) end = rules.size();
        std::string line = trim(rules.substr(pos, end - pos));
        pos = end + (end < rules.size() ? 1 : 0);
        if (line.empty()) continue;

        size_t t1 = line.find('\t');
        size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) continue;
        size_t t3 = line.find('\t', t2 + 1);
        std::string type = line.substr(0, t1);
        std::string pat = line.substr(t1 + 1, t2 - t1 - 1);
        std::string action = t3 == std::string::npos ? line.substr(t2 + 1) : line.substr(t2 + 1, t3 - t2 - 1);
        std::string enabled = t3 == std::string::npos ? "1" : trim(line.substr(t3 + 1));
        if (enabled == "0" || pat.empty()) continue;

        bool hit = false;
        if (type == "kw") hit = body.find(pat) != std::string::npos;
        else if (type == "from") hit = regex_search_case_insensitive(pat, sender);
        else if (type == "re") hit = regex_search_case_insensitive(pat, body);
        if (!hit) continue;

        d.matched = true;
        size_t ap = 0;
        while (ap <= action.size()) {
            size_t comma = action.find(',', ap);
            if (comma == std::string::npos) comma = action.size();
            std::string tok = trim(action.substr(ap, comma - ap));
            if (tok == "drop") d.drop = true;
            else if (tok == "email") d.email = true;
            else {
                uint8_t ch = 0;
                if (parse_push_channel_token(tok, ch)) d.chMask |= 1u << (ch - 1);
            }
            if (comma == action.size()) break;
            ap = comma + 1;
        }
        return d;
    }
    return d;
}

static uint32_t backoff_seconds(uint8_t attempts, uint32_t seed)
{
    uint32_t step = PUSH_RETRY_BASE_SEC;
    for (uint8_t i = 1; i < attempts && step < PUSH_RETRY_MAX_SEC; ++i) step <<= 1;
    if (step > PUSH_RETRY_MAX_SEC) step = PUSH_RETRY_MAX_SEC;
    uint32_t jitter = step / 4;
    return step + (jitter ? (seed % (jitter + 1)) : 0);
}

static esp_err_t http_request(const std::string& url, const char* method,
                              const char* content_type, const std::string& body,
                              int& status_code)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;
    // 默认 TX 缓冲 512B 放不下长短信的 GET 请求行(百分号编码后可达 1.4KB+)；
    // 长合并短信(中文×百分号编码可膨胀 9 倍)按实际 URL 长度放宽，避免必败重试
    size_t tx_need = url.size() + 512;
    cfg.buffer_size_tx = static_cast<int>(tx_need < 2048 ? 2048 : tx_need);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        if (content_type) esp_http_client_set_header(client, "Content-Type", content_type);
        esp_http_client_set_post_field(client, body.c_str(), body.size());
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }

    esp_err_t err = esp_http_client_perform(client);
    status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

static std::string base64_encode_string(const std::string& value)
{
    if (value.empty()) return {};
    size_t out_cap = ((value.size() + 2) / 3) * 4 + 1;
    std::string out(out_cap, '\0');
    size_t out_len = 0;
    int rc = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &out_len,
                                   reinterpret_cast<const unsigned char*>(value.data()), value.size());
    if (rc != 0) return {};
    out.resize(out_len);
    return out;
}

struct SmtpConn {
    esp_tls_t* implicitTls = nullptr;
    int sock = -1;
    bool startTlsActive = false;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctrDrbg;
    mbedtls_entropy_context entropy;

    SmtpConn()
    {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ctr_drbg_init(&ctrDrbg);
        mbedtls_entropy_init(&entropy);
    }
};

static void smtp_conn_close(SmtpConn& conn)
{
    if (conn.implicitTls) {
        esp_tls_conn_destroy(conn.implicitTls);
        conn.implicitTls = nullptr;
    }
    if (conn.startTlsActive) {
        mbedtls_ssl_close_notify(&conn.ssl);
        conn.startTlsActive = false;
    }
    mbedtls_ssl_free(&conn.ssl);
    mbedtls_ssl_config_free(&conn.conf);
    mbedtls_ctr_drbg_free(&conn.ctrDrbg);
    mbedtls_entropy_free(&conn.entropy);
    if (conn.sock >= 0) {
        close(conn.sock);
        conn.sock = -1;
    }
    conn.net.fd = -1;
}

static void set_socket_timeouts(int sock, int timeout_ms)
{
    struct timeval tv = {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static bool socket_connect_with_timeout(int sock, const struct sockaddr* addr, socklen_t addr_len, int timeout_ms)
{
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) flags = 0;
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, addr, addr_len);
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);
        return true;
    }
    if (errno != EINPROGRESS) {
        fcntl(sock, F_SETFL, flags);
        return false;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv = {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    rc = select(sock + 1, nullptr, &wfds, nullptr, &tv);
    if (rc <= 0) {
        fcntl(sock, F_SETFL, flags);
        return false;
    }

    int err = 0;
    socklen_t err_len = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
        fcntl(sock, F_SETFL, flags);
        return false;
    }

    fcntl(sock, F_SETFL, flags);
    return true;
}

static bool smtp_tcp_connect(const std::string& host, int port, SmtpConn& conn)
{
    char port_buf[12];
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), port_buf, &hints, &res);
    if (gai != 0 || !res) {
        idf_logf("SMTP DNS解析失败: %s", host.c_str());
        return false;
    }

    bool ok = false;
    for (struct addrinfo* ai = res; ai && !ok; ai = ai->ai_next) {
        int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;
        set_socket_timeouts(sock, SMTP_TIMEOUT_MS);
        if (socket_connect_with_timeout(sock, ai->ai_addr, ai->ai_addrlen, SMTP_TIMEOUT_MS)) {
            conn.sock = sock;
            conn.net.fd = sock;
            ok = true;
        } else {
            close(sock);
        }
    }
    freeaddrinfo(res);
    if (!ok) idf_log_line("SMTP明文连接失败");
    return ok;
}

static bool smtp_starttls_upgrade(SmtpConn& conn, const std::string& host)
{
    const char* pers = "sms-smtp-starttls";
    int ret = mbedtls_ctr_drbg_seed(&conn.ctrDrbg, mbedtls_entropy_func, &conn.entropy,
                                    reinterpret_cast<const unsigned char*>(pers), strlen(pers));
    if (ret != 0) {
        idf_logf("STARTTLS随机源初始化失败: -0x%04x", -ret);
        return false;
    }
    ret = mbedtls_ssl_config_defaults(&conn.conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        idf_logf("STARTTLS配置初始化失败: -0x%04x", -ret);
        return false;
    }
    mbedtls_ssl_conf_rng(&conn.conf, mbedtls_ctr_drbg_random, &conn.ctrDrbg);
    mbedtls_ssl_conf_authmode(&conn.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    if (esp_crt_bundle_attach(&conn.conf) != ESP_OK) {
        idf_log_line("STARTTLS证书包挂载失败");
        return false;
    }
    ret = mbedtls_ssl_setup(&conn.ssl, &conn.conf);
    if (ret != 0) {
        idf_logf("STARTTLS会话初始化失败: -0x%04x", -ret);
        return false;
    }
    ret = mbedtls_ssl_set_hostname(&conn.ssl, host.c_str());
    if (ret != 0) {
        idf_logf("STARTTLS主机名设置失败: -0x%04x", -ret);
        return false;
    }
    mbedtls_ssl_set_bio(&conn.ssl, &conn.net, mbedtls_net_send, mbedtls_net_recv, nullptr);

    int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(SMTP_TIMEOUT_MS) * 1000LL;
    while ((ret = mbedtls_ssl_handshake(&conn.ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char errbuf[96];
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            idf_logf("STARTTLS握手失败: -0x%04x %s", -ret, errbuf);
            return false;
        }
        if (esp_timer_get_time() >= deadline) {
            idf_log_line("STARTTLS握手超时");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint32_t flags = mbedtls_ssl_get_verify_result(&conn.ssl);
    if (flags != 0) {
        char info[128];
        mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
        idf_logf("STARTTLS证书校验失败: %s", info);
        return false;
    }
    conn.startTlsActive = true;
    return true;
}

// 返回值语义：>0 数据；0 暂无数据(可重试)；-1 连接关闭/致命错误(不要再等)
static ssize_t smtp_conn_read(SmtpConn& conn, char* buf, size_t len)
{
    if (conn.implicitTls) {
        ssize_t ret = esp_tls_conn_read(conn.implicitTls, buf, len);
        if (ret > 0) return ret;
        if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
        return -1;  // 0 = 对端关闭，负值 = 错误
    }
    if (conn.startTlsActive) {
        int ret = mbedtls_ssl_read(&conn.ssl, reinterpret_cast<unsigned char*>(buf), len);
        if (ret > 0) return ret;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        return -1;  // 0 = EOF，其余负值 = 错误(含 close_notify)
    }
    if (conn.sock >= 0) {
        ssize_t ret = recv(conn.sock, buf, len, 0);
        if (ret > 0) return ret;
        if (ret == 0) return -1;  // 对端关闭(灰名单/限速服务器直接掐线很常见)
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) return 0;
        return -1;
    }
    return -1;
}

static bool smtp_conn_write_all(SmtpConn& conn, const std::string& data)
{
    size_t sent = 0;
    int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(SMTP_TIMEOUT_MS) * 1000LL;
    while (sent < data.size()) {
        ssize_t n = -1;
        if (conn.implicitTls) {
            ssize_t ret = esp_tls_conn_write(conn.implicitTls, data.data() + sent, data.size() - sent);
            if (ret > 0) n = ret;
            else if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) n = 0;
            else return false;  // 连接已断，立即失败
        } else if (conn.startTlsActive) {
            int ret = mbedtls_ssl_write(&conn.ssl,
                                        reinterpret_cast<const unsigned char*>(data.data() + sent),
                                        data.size() - sent);
            if (ret > 0) n = ret;
            else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) n = 0;
            else return false;
        } else if (conn.sock >= 0) {
            ssize_t ret = send(conn.sock, data.data() + sent, data.size() - sent, 0);
            if (ret > 0) n = ret;
            else if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)) n = 0;
            else return false;
        } else {
            return false;
        }

        if (n > 0) {
            sent += static_cast<size_t>(n);
            deadline = esp_timer_get_time() + static_cast<int64_t>(SMTP_TIMEOUT_MS) * 1000LL;
            // 有进展也要看会话总限：每次只肯收几字节的服务器不能无限续期
            if (s_smtp_session_deadline && esp_timer_get_time() >= s_smtp_session_deadline) return false;
            continue;
        }
        if (esp_timer_get_time() >= deadline) return false;
        if (s_smtp_session_deadline && esp_timer_get_time() >= s_smtp_session_deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

static bool smtp_final_line_code(const std::string& line, int& code)
{
    if (line.size() < 3 ||
        !isdigit(static_cast<unsigned char>(line[0])) ||
        !isdigit(static_cast<unsigned char>(line[1])) ||
        !isdigit(static_cast<unsigned char>(line[2]))) {
        return false;
    }
    code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    // RFC 5321 允许仅三位码无文本的终结行；第 4 字符为 '-' 才是多行中间行
    if (line.size() == 3) return true;
    return line[3] == ' ';
}

static int smtp_read_response(SmtpConn& conn, std::string& response, uint32_t timeout_ms = SMTP_TIMEOUT_MS)
{
    response.clear();
    response.reserve(256);
    char buf[128];
    int final_code = -1;
    int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000LL;
    if (s_smtp_session_deadline && s_smtp_session_deadline < deadline) deadline = s_smtp_session_deadline;
    while (esp_timer_get_time() < deadline) {
        ssize_t n = smtp_conn_read(conn, buf, sizeof(buf));
        if (n < 0) return final_code;  // 连接已关闭/出错，不再空等整个超时窗口
        if (n > 0) {
            response.append(buf, static_cast<size_t>(n));
            size_t pos = 0;
            while (pos < response.size()) {
                size_t eol = response.find('\n', pos);
                if (eol == std::string::npos) break;
                std::string line = response.substr(pos, eol - pos);
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                int code = -1;
                if (smtp_final_line_code(line, code)) {
                    final_code = code;
                    return final_code;
                }
                pos = eol + 1;
            }
            // 丢弃已解析的行，只保留未完结的尾行：既限住内存，
            // 又保证超长多行横幅(>2KB)之后的终结状态行仍能被识别
            if (pos > 0) response.erase(0, pos);
            if (response.size() >= 2048) return final_code;  // 单行超 2KB=协议垃圾
        } else {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    return final_code;
}

static bool smtp_code_ok(int code, int a, int b = -1, int c = -1)
{
    return code == a || (b >= 0 && code == b) || (c >= 0 && code == c);
}

static bool smtp_expect(SmtpConn& conn, int ok1, int ok2 = -1, int ok3 = -1)
{
    std::string resp;
    int code = smtp_read_response(conn, resp);
    if (smtp_code_ok(code, ok1, ok2, ok3)) return true;
    idf_logf("SMTP响应异常 code=%d resp=%s", code, resp.c_str());
    return false;
}

static bool smtp_command(SmtpConn& conn, const std::string& command, int ok1, int ok2 = -1, int ok3 = -1)
{
    std::string wire = command;
    wire += "\r\n";
    if (!smtp_conn_write_all(conn, wire)) return false;
    return smtp_expect(conn, ok1, ok2, ok3);
}

static std::string header_safe(std::string value)
{
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n') ch = ' ';
    }
    return value;
}

static std::string email_addr_only(std::string value)
{
    value = trim(value);
    size_t lt = value.find('<');
    size_t gt = value.find('>', lt == std::string::npos ? 0 : lt + 1);
    if (lt != std::string::npos && gt != std::string::npos && gt > lt + 1) {
        value = value.substr(lt + 1, gt - lt - 1);
    }
    return header_safe(trim(value));
}

static std::string smtp_date_utc()
{
    time_t now = time(nullptr);
    struct tm tm_utc = {};
    gmtime_r(&now, &tm_utc);
    char buf[64];
    if (strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S +0000", &tm_utc) == 0) return {};
    return std::string(buf);
}

// RFC 2047: 单个 encoded-word 不得超 75 字符，长主题按 UTF-8 边界分段并折行
static std::string encode_subject_words(const std::string& subject)
{
    std::string out;
    size_t pos = 0;
    while (pos < subject.size()) {
        size_t take = subject.size() - pos;
        if (take > 45) take = 45;  // base64(45 字节)=60 字符，加前后缀共 72 < 75
        // 不从 UTF-8 连续字节中间截断
        while (take > 1 && pos + take < subject.size() &&
               (static_cast<unsigned char>(subject[pos + take]) & 0xC0) == 0x80) {
            --take;
        }
        std::string chunk64 = base64_encode_string(subject.substr(pos, take));
        if (chunk64.empty()) return {};
        if (!out.empty()) out += "\r\n ";
        out += "=?UTF-8?B?" + chunk64 + "?=";
        pos += take;
    }
    return out;
}

// 正文统一 base64 编码(76 字符折行): 同时规避 8BITMIME 兼容性与 998 字节行长上限，
// base64 字符集不含 '.'，也无需再做点填充
static std::string base64_wrap76(const std::string& data)
{
    std::string b64 = base64_encode_string(data);
    std::string out;
    out.reserve(b64.size() + b64.size() / 76 * 2 + 4);
    for (size_t i = 0; i < b64.size(); i += 76) {
        size_t take = b64.size() - i;
        if (take > 76) take = 76;
        out.append(b64, i, take);
        out += "\r\n";
    }
    return out;
}

static bool send_smtp_email(const IdfEmailSettingsView& cfg, const std::string& subject, const std::string& body)
{
    if (!cfg.emailConfigured) {
        idf_log_line("邮件配置不完整，跳过发送");
        return false;
    }
    if (!idf_wifi_get_status().staConnected) {
        idf_log_line("WiFi未连接，邮件稍后重试");
        return false;
    }

    std::string server = trim(cfg.smtpServer);
    std::string from = email_addr_only(cfg.smtpUser);
    std::string to = email_addr_only(cfg.smtpSendTo);
    if (server.empty() || from.empty() || to.empty()) {
        idf_log_line("邮件配置不完整，跳过发送");
        return false;
    }
    esp_tls_cfg_t tls_cfg = {};
    tls_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    tls_cfg.timeout_ms = SMTP_TIMEOUT_MS;

    bool ok = false;
    SmtpConn conn;
    // 会话总限从连接前开始计：DNS/TCP/TLS/9 轮命令共享 120s，任何一环慢都不能无限拖
    s_smtp_session_deadline = esp_timer_get_time() + SMTP_SESSION_MAX_US;
    idf_logf("连接SMTP服务器: %s:%d", server.c_str(), cfg.smtpPort);
    if (cfg.smtpPort == 465) {
        conn.implicitTls = esp_tls_init();
        if (!conn.implicitTls) {
            idf_log_line("SMTP TLS句柄分配失败");
            return false;
        }
        if (esp_tls_conn_new_sync(server.c_str(), static_cast<int>(server.size()), cfg.smtpPort, &tls_cfg, conn.implicitTls) != 1) {
            idf_log_line("邮件服务器TLS连接失败");
            smtp_conn_close(conn);
            return false;
        }
    } else {
        if (!smtp_tcp_connect(server, cfg.smtpPort, conn)) {
            smtp_conn_close(conn);
            return false;
        }
    }

    std::string user64 = base64_encode_string(cfg.smtpUser);
    std::string pass64 = base64_encode_string(cfg.smtpPass);
    std::string subject_words = encode_subject_words(header_safe(subject));
    std::string safe_subject = subject_words.empty() ? header_safe(subject) : subject_words;
    std::string message;
    message.reserve(body.size() * 2 + 512);
    message += "From: sms notify <" + from + ">\r\n";
    message += "To: <" + to + ">\r\n";
    message += "Subject: " + safe_subject + "\r\n";
    message += "Date: " + smtp_date_utc() + "\r\n";
    message += "MIME-Version: 1.0\r\n";
    message += "Content-Type: text/plain; charset=UTF-8\r\n";
    message += "Content-Transfer-Encoding: base64\r\n\r\n";
    message += base64_wrap76(body);
    message += ".\r\n";

    ok = smtp_expect(conn, 220) &&
         smtp_command(conn, "EHLO sms-forwarder", 250);
    if (ok && cfg.smtpPort != 465) {
        ok = smtp_command(conn, "STARTTLS", 220) &&
             smtp_starttls_upgrade(conn, server) &&
             smtp_command(conn, "EHLO sms-forwarder", 250);
    }
    ok = ok &&
         smtp_command(conn, "AUTH LOGIN", 334) &&
         smtp_command(conn, user64, 334) &&
         smtp_command(conn, pass64, 235) &&
         smtp_command(conn, "MAIL FROM:<" + from + ">", 250) &&
         smtp_command(conn, "RCPT TO:<" + to + ">", 250, 251) &&
         smtp_command(conn, "DATA", 354) &&
         smtp_conn_write_all(conn, message) &&
         smtp_expect(conn, 250);

    if (ok) smtp_command(conn, "QUIT", 221);
    else smtp_conn_write_all(conn, "QUIT\r\n");  // 失败路径不再等 QUIT 响应，避免死连接再吃 15s
    smtp_conn_close(conn);
    s_smtp_session_deadline = 0;
    idf_log_line(ok ? "邮件发送完成" : "邮件发送失败");
    return ok;
}

static bool send_to_channel(const IdfPushChannel& channel, const char* sender_raw,
                            const char* text_raw, const char* timestamp_raw,
                            bool notify = false)
{
    if (!channel_valid(channel)) return false;

    std::string sender = sender_raw ? sender_raw : "";
    std::string text = text_raw ? text_raw : "";
    std::string timestamp = timestamp_raw ? timestamp_raw : "";
    std::string receiver = notify ? std::string() : local_phone_number();
    std::string sender_json = json_escape(sender);
    std::string text_json = json_escape(text);
    std::string ts_json = json_escape(timestamp);
    std::string receiver_json = json_escape(receiver);
    std::string receiver_line_json = receiver.empty() ? std::string() : ("\\n本机号码: " + receiver_json);
    std::string receiver_block_json = receiver.empty() ? std::string() : ("\\n\\n本机号码: " + receiver_json);
    std::string receiver_html_json = receiver.empty() ? std::string() : ("<br><b>本机号码:</b> " + json_escape(html_escape(receiver)));
    std::string time_line_json = "\\n时间: " + ts_json;
    std::string time_block_json = "\\n\\n时间: " + ts_json;
    std::string time_html_json = "<br><b>时间:</b> " + ts_json;
    // 自定义提醒(notify)：sender 里放的是任务名，直接当标题用，不再套"短信来自/发送者/内容"这些短信味道的模板
    std::string title = notify ? sender : ("短信来自: " + sender);
    std::string title_json = json_escape(title);
    std::string url;
    std::string body;
    const char* content_type = "application/json";
    const char* method = "POST";

    switch (channel.type) {
        case PUSH_TYPE_POST_JSON:
            url = channel.url;
            body = "{\"sender\":\"" + sender_json + "\",\"receiver\":\"" + receiver_json +
                   "\",\"message\":\"" + text_json + "\",\"timestamp\":\"" + ts_json + "\"}";
            break;
        case PUSH_TYPE_BARK: {
            BarkTarget bark = bark_target_from_channel(channel);
            if (!bark.ok) return false;
            url = bark.url;
            body = "{";
            if (!bark.device_key.empty()) {
                json_prop(body, "device_key", bark.device_key);
                body += ",";
            }
            json_prop(body, "title", title);
            body += ",";
            json_prop(body, "body", text + (receiver.empty() ? std::string() : ("\n\n本机号码: " + receiver)) +
                                   "\n\n时间: " + timestamp);
            append_bark_params(body, channel.key2, sender, text, timestamp, receiver);
            body += "}";
            break;
        }
        case PUSH_TYPE_GET:
            method = "GET";
            url = channel.url + (channel.url.find('?') == std::string::npos ? "?" : "&") +
                  "sender=" + url_encode(sender) + "&receiver=" + url_encode(receiver) +
                  "&message=" + url_encode(text) + "&timestamp=" + url_encode(timestamp);
            break;
        case PUSH_TYPE_DINGTALK: {
            url = channel.url;
            if (!channel.key1.empty()) {
                int64_t ts = utc_millis();
                std::string sign_data = std::to_string(ts) + "\n" + channel.key1;
                std::string sign = url_encode(hmac_sha256_base64(sign_data, channel.key1));
                url += (url.find('?') == std::string::npos ? "?" : "&");
                url += "timestamp=" + std::to_string(ts) + "&sign=" + sign;
            }
            body = notify
                ? ("{\"msgtype\":\"text\",\"text\":{\"content\":\"" + title_json + "\\n" +
                   text_json + time_line_json + "\"}}")
                : ("{\"msgtype\":\"text\",\"text\":{\"content\":\"短信通知\\n发送者: " +
                   sender_json + receiver_line_json + "\\n内容: " + text_json + time_line_json + "\"}}");
            break;
        }
        case PUSH_TYPE_PUSHPLUS: {
            url = channel.url.empty() ? "https://www.pushplus.plus/send" : channel.url;
            std::string push_channel = channel.key2.empty() ? "wechat" : channel.key2;
            if (push_channel != "wechat" && push_channel != "extension" && push_channel != "app") push_channel = "wechat";
            std::string text_html = json_escape(html_escape(text));
            std::string sender_html = json_escape(html_escape(sender));
            std::string pp_content = notify
                ? (text_html + time_html_json)
                : ("<b>发送者:</b> " + sender_html + receiver_html_json + time_html_json + "<br><b>内容:</b><br>" + text_html);
            body = "{\"token\":\"" + json_escape(channel.key1) + "\",\"title\":\"" + title_json +
                   "\",\"content\":\"" + pp_content + "\",\"channel\":\"" + push_channel + "\"}";
            break;
        }
        case PUSH_TYPE_SERVERCHAN: {
            std::string send_key = trim(channel.key1);
            std::string base = trim(channel.url);
            if (send_key.empty() && !base.empty() && !is_url_like(base)) {
                send_key = base;
                base.clear();
            }
            if (base.empty()) base = "https://sctapi.ftqq.com";
            if (is_url_like(send_key)) {
                base = send_key;
                send_key.clear();
            }
            while (!base.empty() && base.back() == '/') base.pop_back();
            url = base.find(".send") != std::string::npos ? base : (base + "/" + send_key + ".send");
            content_type = "application/x-www-form-urlencoded";
            std::string sc_desp = notify
                ? ("**时间:** " + timestamp + "\n\n" + text)
                : ("**发送者:** " + sender +
                   (receiver.empty() ? std::string() : ("\n\n**本机号码:** " + receiver)) +
                   "\n\n**时间:** " + timestamp + "\n\n**内容:**\n\n" + text);
            body = "title=" + url_encode(title) + "&desp=" + url_encode(sc_desp);
            break;
        }
        case PUSH_TYPE_CUSTOM:
            url = channel.url;
            body = apply_push_placeholders(channel.customBody, sender_json, text_json, ts_json, receiver_json);
            break;
        case PUSH_TYPE_FEISHU:
            url = channel.url;
            body = "{";
            if (!channel.key1.empty()) {
                int64_t ts = time(nullptr);
                // 飞书签名与钉钉相反: 以 ts+"\n"+secret 为密钥、空串为消息做 HMAC-SHA256
                std::string sign = hmac_sha256_base64("", std::to_string(ts) + "\n" + channel.key1);
                body += "\"timestamp\":\"" + std::to_string(ts) + "\",\"sign\":\"" + sign + "\",";
            }
            body += notify
                ? ("\"msg_type\":\"text\",\"content\":{\"text\":\"" + title_json + "\\n" +
                   text_json + time_line_json + "\"}}")
                : ("\"msg_type\":\"text\",\"content\":{\"text\":\"短信通知\\n发送者: " +
                   sender_json + receiver_line_json + "\\n内容: " + text_json + time_line_json + "\"}}");
            break;
        case PUSH_TYPE_GOTIFY:
            url = channel.url;
            if (!url.empty() && url.back() != '/') url += "/";
            url += "message?token=" + url_encode(channel.key1);
            body = "{\"title\":\"" + title_json + "\",\"message\":\"" + text_json +
                   receiver_block_json + time_block_json + "\",\"priority\":5}";
            break;
        case PUSH_TYPE_TELEGRAM: {
            std::string base = channel.url.empty() ? "https://api.telegram.org" : channel.url;
            while (!base.empty() && base.back() == '/') base.pop_back();
            url = base + "/bot" + channel.key2 + "/sendMessage";
            body = notify
                ? ("{\"chat_id\":\"" + json_escape(channel.key1) + "\",\"text\":\"" + title_json + "\\n" +
                   text_json + time_line_json + "\"}")
                : ("{\"chat_id\":\"" + json_escape(channel.key1) + "\",\"text\":\"短信通知\\n发送者: " +
                   sender_json + receiver_line_json + "\\n内容: " + text_json + time_line_json + "\"}");
            break;
        }
        default:
            return false;
    }

    std::string name = channel.name.empty() ? ("通道" + std::to_string(channel.type)) : channel.name;
    int code = 0;
    esp_err_t err = http_request(url, method, content_type, body, code);
    bool ok = (err == ESP_OK && code >= 200 && code < 300);
    // 发送中+响应码两行合并为一行结果，降噪同时保留通道名与成败
    if (err == ESP_OK) idf_logf("%s 推送%s (HTTP %d)", name.c_str(), ok ? "成功" : "失败", code);
    else idf_logf("%s 推送失败: %s", name.c_str(), esp_err_to_name(err));
    return ok;
}

static bool enqueue_push_job_locked(uint8_t ch, const std::string& sender, const std::string& text,
                                    const std::string& timestamp, uint8_t attempts, uint32_t delay_sec,
                                    bool notify = false, uint32_t inbox_id = 0,
                                    uint32_t completion_id = 0)
{
    int slot = -1;
    for (size_t i = 0; i < s_push_jobs.size(); ++i) {
        if (!s_push_jobs[i].used) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        // 旧任务可能已经对应收件箱里的"已排队"短信，不能为了新任务静默踢掉。
        return false;
    }
    PushJob& job = s_push_jobs[slot];
    job.used = true;
    job.channel = ch;
    job.attempts = attempts;
    job.notify = notify;
    job.nextUs = esp_timer_get_time() + static_cast<int64_t>(delay_sec) * 1000000LL;
    job.sender = sender;
    job.text = text;
    job.timestamp = timestamp;
    job.inboxId = inbox_id;
    job.completionId = completion_id;
    return true;
}

static int push_queue_free_locked()
{
    int n = 0;
    for (const auto& job : s_push_jobs) if (!job.used) ++n;
    return n;
}

static bool enqueue_email_job_locked(const std::string& subject, const std::string& body,
                                     uint8_t attempts = 0, uint32_t delay_sec = 0,
                                     uint32_t inbox_id = 0, uint32_t completion_id = 0)
{
    int slot = -1;
    for (size_t i = 0; i < s_email_jobs.size(); ++i) {
        if (!s_email_jobs[i].used) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        // 与 Arduino 一致：队列满时拒绝新邮件，而不是踢掉正在退避重试的旧邮件
        idf_log_line("邮件队列已满，新邮件未入队");
        return false;
    }
    EmailJob& job = s_email_jobs[slot];
    job.used = true;
    job.attempts = attempts;
    job.nextUs = esp_timer_get_time() + static_cast<int64_t>(delay_sec) * 1000000LL;
    job.subject = subject;
    job.body = body;
    job.inboxId = inbox_id;
    job.completionId = completion_id;
    return true;
}

static int queue_depth_locked(const std::array<PushJob, PUSH_QUEUE_MAX>& jobs)
{
    int n = 0;
    for (const auto& job : jobs) if (job.used) ++n;
    return n;
}

static int email_queue_depth_locked()
{
    int n = 0;
    for (const auto& job : s_email_jobs) if (job.used) ++n;
    return n;
}

static bool enqueue_forward_job_locked(const ForwardJob& src, uint32_t delay_sec)
{
    for (auto& job : s_forward_jobs) {
        if (job.used) continue;
        job = src;
        job.used = true;
        job.nextUs = esp_timer_get_time() + static_cast<int64_t>(delay_sec) * 1000000LL;
        return true;
    }
    return false;
}

static bool enqueue_forward_locked(const char* sender, const char* text, const char* timestamp, uint32_t inbox_id)
{
    ForwardJob job;
    job.sender = sender ? sender : "";
    job.text = text ? text : "";
    job.timestamp = timestamp ? timestamp : "";
    job.inboxId = inbox_id;
    return enqueue_forward_job_locked(job, 0);
}

static bool pop_forward_job(ForwardJob& out)
{
    ensure_init();
    if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;
    bool found = false;
    int64_t now = esp_timer_get_time();
    for (auto& job : s_forward_jobs) {
        if (!job.used || job.nextUs > now) continue;
        out = job;
        job = ForwardJob();
        found = true;
        break;
    }
    xSemaphoreGive(s_mutex);
    return found;
}

static void requeue_forward_later(ForwardJob& job, const char* reason, bool count_attempt = true)
{
    // 下游队列繁忙不算投递失败：真正的发送一次都没试过，
    // 不该消耗重试预算——否则拥塞高峰期短信会被"零投递"地放弃
    if (count_attempt) job.attempts++;
    if (job.attempts >= PUSH_RETRY_MAX) {
        idf_logf("%s，转发 id=%u 已重试%u次，保持未转发等待手动重发",
                 reason,
                 static_cast<unsigned>(job.inboxId),
                 static_cast<unsigned>(job.attempts));
        return;
    }

    uint32_t delay = backoff_seconds(job.attempts, job.inboxId + static_cast<uint32_t>(job.text.size()));
    bool queued = false;
    ensure_init();
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        queued = enqueue_forward_job_locked(job, delay);
        xSemaphoreGive(s_mutex);
    }
    if (queued) {
        idf_logf("%s，转发 id=%u 延后%u秒重试",
                 reason,
                 static_cast<unsigned>(job.inboxId),
                 static_cast<unsigned>(delay));
        wake_worker();
    } else {
        idf_logf("%s，且转发队列已满，短信 id=%u 保持未转发",
                 reason,
                 static_cast<unsigned>(job.inboxId));
    }
}

static bool process_forward_one()
{
    // busy 覆盖"已出队未入队"的窗口，避免维护性重启(每日重启/低堆重启)吃掉在途短信
    s_busy.store(true, std::memory_order_relaxed);
    ForwardJob job;
    if (!pop_forward_job(job)) {
        s_busy.store(false, std::memory_order_relaxed);
        return false;
    }

    const IdfPushForwardView cfg = idf_config_get_push_forward_view();
    const std::string receiver = local_phone_number();
    ForwardDecision fd = eval_forward_rules(cfg.forwardRules, job.sender, job.text);
    if (fd.matched && fd.drop) {
        idf_logf("转发规则命中：丢弃短信 id=%u", static_cast<unsigned>(job.inboxId));
        idf_inbox_mark_forwarded(job.inboxId);
        s_busy.store(false, std::memory_order_relaxed);
        return true;
    }

    uint32_t mask = fd.matched ? fd.chMask : 0xFFFFFFFFu;
    bool email_selected = fd.matched ? fd.email : true;
    if (!cfg.pushEnabled) mask = 0;
    int dispatched = 0;
    bool email_queued = false;
    bool enqueue_failed = false;
    bool push_queue_busy = false;
    bool email_queue_busy = false;
    bool had_queued_target = job.pushQueued;

    std::string targets;  // 命中的通道名列表，用于一行日志点名去向
    ensure_init();
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        int push_targets = 0;
        if (!job.pushQueued) {
            for (uint8_t i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
                if (!(mask & (1u << i))) continue;
                if (!channel_valid(cfg.pushChannels[i])) continue;
                ++push_targets;
            }
        }
        bool will_queue_email = email_selected && cfg.emailEnabled && cfg.emailConfigured;
        uint8_t total_targets = static_cast<uint8_t>(push_targets + (will_queue_email ? 1 : 0));
        if (push_targets > push_queue_free_locked()) {
            enqueue_failed = true;
            push_queue_busy = true;
        }
        if (will_queue_email && email_queue_depth_locked() >= static_cast<int>(EMAIL_QUEUE_MAX)) {
            enqueue_failed = true;
            email_queue_busy = true;
        }
        uint32_t completion_id = enqueue_failed ? 0 : register_forward_completion_locked(job.inboxId, total_targets);
        if (!enqueue_failed && total_targets > 0 && job.inboxId != 0 && completion_id == 0) enqueue_failed = true;
        if (!job.pushQueued) {
            for (uint8_t i = 0; i < IDF_MAX_PUSH_CHANNELS && !enqueue_failed; ++i) {
                if (!(mask & (1u << i))) continue;
                if (!channel_valid(cfg.pushChannels[i])) continue;
                if (enqueue_push_job_locked(i, job.sender, job.text, job.timestamp, 0, 0, false,
                                            job.inboxId, completion_id)) {
                    ++dispatched;
                    had_queued_target = true;
                    if (!targets.empty()) targets += "、";
                    targets += cfg.pushChannels[i].name.empty()
                                   ? ("通道" + std::to_string(i + 1)) : cfg.pushChannels[i].name;
                } else {
                    enqueue_failed = true;
                    push_queue_busy = true;
                }
            }
            if (dispatched > 0) job.pushQueued = true;
        }
        if (!enqueue_failed && will_queue_email) {
            // 主题只放正文前若干字符(全文在正文里)，避免超长 Subject 被严格 MTA 拒收
            std::string subject = "短信";
            subject += job.sender;
            subject += ",";
            subject += utf8_truncate(job.text, 48);
            std::string body = "来自：";
            body += job.sender;
            if (!receiver.empty()) {
                body += "，本机号码：";
                body += receiver;
            }
            if (!job.timestamp.empty()) {
                body += "，时间：";
                body += job.timestamp;
            }
            body += "，内容：";
            body += job.text;
            email_queued = enqueue_email_job_locked(subject, body, 0, 0, job.inboxId, completion_id);
            if (email_queued) had_queued_target = true;
            else {
                enqueue_failed = true;
                email_queue_busy = true;
            }
        }
        if (enqueue_failed && completion_id != 0) {
            cancel_forward_completion_locked(completion_id);
        }
        xSemaphoreGive(s_mutex);
    } else {
        enqueue_failed = true;
    }
    if (enqueue_failed) {
        const char* reason = push_queue_busy && email_queue_busy ? "推送/邮件队列繁忙" :
                             (email_queue_busy ? "邮件队列繁忙" :
                              (push_queue_busy ? "推送队列繁忙" : "转发队列繁忙"));
        requeue_forward_later(job, reason, false);
        s_busy.store(false, std::memory_order_relaxed);
        return true;
    }

    // 一行点名去向：收到→转发到哪些通道/邮件，一眼看清
    if (email_queued) {
        if (!targets.empty()) targets += "、";
        targets += "邮件";
    }
    if (!targets.empty()) {
        idf_logf("转发 id=%u → %s", static_cast<unsigned>(job.inboxId), targets.c_str());
    } else if (had_queued_target) {
        idf_logf("转发 id=%u → 已排队目标", static_cast<unsigned>(job.inboxId));
    } else {
        idf_logf("转发 id=%u 无有效目标(未启用通道/未配置邮件)", static_cast<unsigned>(job.inboxId));
    }
    s_busy.store(false, std::memory_order_relaxed);
    return true;
}

static bool low_heap_defer()
{
    static bool warned = false;
    if (tls_heap_ok()) {
        warned = false;
        return false;
    }
    if (!warned) {
        warned = true;
        idf_logf("可用堆不足(最大块<%u)，推迟 TLS 发送", static_cast<unsigned>(TLS_MIN_FREE_HEAP));
    }
    return true;
}

static bool process_push_one()
{
    if (!idf_wifi_get_status().staConnected) return false;
    if (low_heap_defer()) return false;  // 任务留在队列里，等堆恢复再发
    int picked = -1;
    PushJob job;
    int64_t now = esp_timer_get_time();

    ensure_init();
    s_busy.store(true, std::memory_order_relaxed);
    if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        s_busy.store(false, std::memory_order_relaxed);
        return false;
    }
    for (size_t i = 0; i < s_push_jobs.size(); ++i) {
        if (!s_push_jobs[i].used || s_push_jobs[i].nextUs > now) continue;
        if (channel_cooling(s_push_jobs[i].channel, now)) continue;  // 连续失败的通道冷却期内跳过
        picked = static_cast<int>(i);
        job = s_push_jobs[i];
        s_push_jobs[i] = PushJob();
        break;
    }
    xSemaphoreGive(s_mutex);
    if (picked < 0) {
        s_busy.store(false, std::memory_order_relaxed);
        return false;
    }

    IdfPushChannel channel;
    if (!idf_config_get_push_channel(job.channel, channel) || !channel_valid(channel)) {
        cancel_forward_completion(job.completionId);
        s_busy.store(false, std::memory_order_relaxed);
        return true;
    }

    bool ok = send_to_channel(channel, job.sender.c_str(), job.text.c_str(),
                              job.timestamp.c_str(), job.notify);
    note_channel_result(job.channel, ok);
    if (ok) {
        note_forward_target_success(job.completionId);
        s_busy.store(false, std::memory_order_relaxed);
        return true;
    }

    job.attempts++;
    if (job.attempts >= PUSH_RETRY_MAX) {
        idf_logf("通道%u 重试%u次仍失败，放弃", static_cast<unsigned>(job.channel + 1), static_cast<unsigned>(job.attempts));
        cancel_forward_completion(job.completionId);
        // 投递最终失败：把收件箱条目改回"未转发"，让丢失可见、可手动重发
        if (job.inboxId) idf_inbox_set_forwarded(job.inboxId, false);
        s_busy.store(false, std::memory_order_relaxed);
        return true;
    }
    uint32_t delay = backoff_seconds(job.attempts, static_cast<uint32_t>(job.channel * 7 + job.attempts));
    bool requeued = false;
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        requeued = enqueue_push_job_locked(job.channel, job.sender, job.text, job.timestamp,
                                           job.attempts, delay, job.notify, job.inboxId, job.completionId);
        xSemaphoreGive(s_mutex);
    }
    if (!requeued) {
        cancel_forward_completion(job.completionId);
        idf_logf("推送重试队列已满，通道%u 本次重试未保留", static_cast<unsigned>(job.channel + 1));
        if (job.inboxId) idf_inbox_set_forwarded(job.inboxId, false);
    }
    s_busy.store(false, std::memory_order_relaxed);
    return true;
}

static bool process_email_one()
{
    if (!idf_wifi_get_status().staConnected) return false;

    IdfEmailSettingsView cfg = idf_config_get_email_settings_view();
    ensure_init();
    if (!cfg.emailEnabled) {
        // 与 Arduino 一致：邮件功能被关闭时清空积压队列
        if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            bool purged = false;
            for (auto& j : s_email_jobs) {
                if (j.used) purged = true;
                cancel_forward_completion_locked(j.completionId);
                j = EmailJob();
            }
            xSemaphoreGive(s_mutex);
            if (purged) idf_log_line("邮件功能已关闭，清空待发邮件队列");
        }
        return false;
    }
    if (low_heap_defer()) return false;

    int picked = -1;
    EmailJob job;
    int64_t now = esp_timer_get_time();

    s_busy.store(true, std::memory_order_relaxed);
    if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        s_busy.store(false, std::memory_order_relaxed);
        return false;
    }
    for (size_t i = 0; i < s_email_jobs.size(); ++i) {
        if (!s_email_jobs[i].used || s_email_jobs[i].nextUs > now) continue;
        picked = static_cast<int>(i);
        job = s_email_jobs[i];
        s_email_jobs[i] = EmailJob();
        break;
    }
    xSemaphoreGive(s_mutex);
    if (picked < 0) {
        s_busy.store(false, std::memory_order_relaxed);
        return false;
    }

    bool ok = send_smtp_email(cfg, job.subject, job.body);
    if (ok) {
        note_forward_target_success(job.completionId);
        s_busy.store(false, std::memory_order_relaxed);
        return true;
    }

    job.attempts++;
    if (job.attempts >= PUSH_RETRY_MAX) {
        idf_logf("邮件重试%u次仍失败，放弃", static_cast<unsigned>(job.attempts));
        cancel_forward_completion(job.completionId);
        // 同推送：最终失败回改"未转发"，避免收件箱假装已送达
        if (job.inboxId) idf_inbox_set_forwarded(job.inboxId, false);
        s_busy.store(false, std::memory_order_relaxed);
        return true;
    }
    uint32_t delay = backoff_seconds(job.attempts, static_cast<uint32_t>(job.subject.size() + job.body.size()));
    bool requeued = false;
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        requeued = enqueue_email_job_locked(job.subject, job.body, job.attempts, delay,
                                            job.inboxId, job.completionId);
        xSemaphoreGive(s_mutex);
    }
    if (!requeued) {
        cancel_forward_completion(job.completionId);
        idf_log_line("邮件重试队列已满，本次重试未保留");
        if (job.inboxId) idf_inbox_set_forwarded(job.inboxId, false);
    }
    s_busy.store(false, std::memory_order_relaxed);
    return true;
}

static bool process_test_one()
{
    if (!idf_wifi_get_status().staConnected) return false;
    if (low_heap_defer()) return false;
    int picked = -1;
    IdfPushChannel channel;
    const IdfPushNotifyView cfg = idf_config_get_push_notify_view();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    for (uint8_t i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        if (!s_test_jobs[i].pending) continue;
        picked = i;
        s_test_jobs[i].pending = false;
        s_test_jobs[i].running = true;
        s_test_jobs[i].done = false;
        s_test_jobs[i].success = false;
        s_test_jobs[i].message = "测试推送发送中";
        channel = cfg.pushChannels[i];
        break;
    }
    xSemaphoreGive(s_mutex);
    if (picked < 0) return false;

    bool ok = false;
    std::string result;
    if (!channel_valid(channel)) {
        result = "通道配置已变更或未启用，测试取消";
    } else {
        s_busy.store(true, std::memory_order_relaxed);
        std::string ts = format_local_time(cfg.tzOffsetMin);
        ok = send_to_channel(channel, "测试", "这是一条来自 SMS Forwarder 的测试推送",
                             ts.empty() ? "时间未同步" : ts.c_str());
        s_busy.store(false, std::memory_order_relaxed);
        result = ok ? "测试推送已发送" : "测试推送失败，请查看日志";
    }

    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        TestJob& job = s_test_jobs[picked];
        job.running = false;
        job.done = true;
        job.success = ok;
        job.message = result;
        xSemaphoreGive(s_mutex);
    }
    return true;
}

static void push_task(void*)
{
    while (true) {
        bool did = process_forward_one();
        if (!did) did = process_push_one();
        if (!did) did = process_email_one();
        if (!did) did = process_test_one();
        if (did) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (s_wake_sem) {
            // 空闲时等唤醒信号：入队即处理；100ms 超时兜底以按时拾取退避重试任务
            xSemaphoreTake(s_wake_sem, pdMS_TO_TICKS(100));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

esp_err_t idf_push_start(void)
{
    if (s_started) return ESP_OK;
    if (!ensure_init()) {
        cleanup_start_resources();
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(push_task, "idf_push", PUSH_WORKER_STACK, nullptr, 3, nullptr);
    if (ok != pdPASS) {
        cleanup_start_resources();
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    idf_log_line("推送后台 worker 已启动");
    return ESP_OK;
}

bool idf_push_enqueue_forward(const char* sender, const char* text, const char* timestamp, uint32_t inbox_id)
{
    if (!ensure_init()) return false;
    // 临界区都很短(纯内存操作)，无限等锁保证收到的短信绝不因锁竞争被丢
    if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;
    bool ok = enqueue_forward_locked(sender, text, timestamp, inbox_id);
    xSemaphoreGive(s_mutex);
    if (ok) wake_worker();
    else idf_log_line("转发队列已满，短信暂未转发");
    return ok;
}

int idf_push_enqueue_notify(const char* title, const char* body, const char* timestamp)
{
    if (!ensure_init()) return 0;
    const IdfPushNotifyView cfg = idf_config_get_push_notify_view();
    if (!cfg.pushEnabled) return 0;

    std::string ts = timestamp ? timestamp : "";
    if (ts.empty()) ts = format_local_time(cfg.tzOffsetMin);
    if (ts.empty()) ts = "时间未同步";

    int dispatched = 0;
    if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return 0;
    for (uint8_t i = 0; i < IDF_MAX_PUSH_CHANNELS; ++i) {
        if (!channel_valid(cfg.pushChannels[i])) continue;
        if (enqueue_push_job_locked(i, title ? title : "系统通知", body ? body : "", ts, 0, 0, true)) {
            ++dispatched;
        }
    }
    xSemaphoreGive(s_mutex);
    if (dispatched > 0) wake_worker();
    return dispatched;
}

bool idf_push_enqueue_email(const char* subject, const char* body)
{
    if (!ensure_init()) return false;
    if (!idf_config_email_configured()) {
        idf_log_line("邮件配置不完整，邮件未入队");
        return false;
    }
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    bool ok = enqueue_email_job_locked(subject ? subject : "", body ? body : "");
    int depth = email_queue_depth_locked();
    xSemaphoreGive(s_mutex);
    if (ok) {
        wake_worker();
        idf_logf("邮件已入队，当前待发=%d", depth);
    }
    return ok;
}

int idf_push_forward_queue_depth(void)
{
    if (!ensure_init()) return 0;
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    int n = 0;
    for (const auto& job : s_forward_jobs) if (job.used) ++n;
    xSemaphoreGive(s_mutex);
    return n;
}

int idf_push_retry_queue_depth(void)
{
    if (!ensure_init()) return 0;
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    int n = queue_depth_locked(s_push_jobs);
    xSemaphoreGive(s_mutex);
    return n;
}

int idf_push_email_queue_depth(void)
{
    if (!ensure_init()) return 0;
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    int n = email_queue_depth_locked();
    xSemaphoreGive(s_mutex);
    return n;
}

bool idf_push_busy(void)
{
    return s_busy.load(std::memory_order_relaxed);
}

bool idf_push_enqueue_test(uint8_t channel, std::string& message)
{
    if (channel >= IDF_MAX_PUSH_CHANNELS) {
        message = "通道序号无效";
        return false;
    }
    if (!idf_wifi_get_status().staConnected) {
        message = "WiFi 未连接，暂不能测试推送";
        return false;
    }
    if (!ensure_init()) {
        message = "推送队列初始化失败";
        return false;
    }
    IdfPushChannel push_channel;
    bool valid = idf_config_get_push_channel(channel, push_channel) && channel_valid(push_channel);
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        message = "推送队列忙";
        return false;
    }
    TestJob& job = s_test_jobs[channel];
    bool busy = job.pending || job.running;
    if (valid && !busy) {
        job.pending = true;
        job.running = false;
        job.done = false;
        job.success = false;
        job.message = "测试推送已排队，可继续刷新网页";
        message = job.message;
    }
    xSemaphoreGive(s_mutex);
    if (!valid) {
        message = "该通道未启用或配置不完整(请先保存)";
        return false;
    }
    if (busy) {
        message = "该通道测试已在后台进行";
        return true;
    }
    wake_worker();
    return true;
}

std::string idf_push_test_status_json(uint8_t channel)
{
    if (channel >= IDF_MAX_PUSH_CHANNELS) {
        return "{\"queued\":false,\"running\":false,\"done\":true,\"success\":false,\"message\":\"通道序号无效\"}";
    }
    if (!ensure_init()) {
        return "{\"queued\":false,\"running\":false,\"done\":true,\"success\":false,\"message\":\"推送队列初始化失败\"}";
    }
    TestJob copy;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        copy = s_test_jobs[channel];
        xSemaphoreGive(s_mutex);
    }
    std::string msg = copy.message.empty() ? "未开始测试" : copy.message;
    std::string out = "{";
    out += "\"queued\":"; out += copy.pending ? "true" : "false"; out += ",";
    out += "\"running\":"; out += copy.running ? "true" : "false"; out += ",";
    out += "\"done\":"; out += copy.done ? "true" : "false"; out += ",";
    out += "\"success\":"; out += copy.success ? "true" : "false"; out += ",";
    json_prop(out, "message", msg);
    out += "}";
    return out;
}
