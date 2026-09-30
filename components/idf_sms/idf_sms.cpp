#include "idf_sms.h"
#include "idf_sms_reply_check.h"
#include "idf_sms_timestamp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <new>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "idf_config.h"
#include "idf_inbox.h"
#include "idf_log.h"
#include "idf_modem.h"
#include "idf_push.h"
#include "idf_util.h"
#include "pdulib.h"

static constexpr size_t MAX_PDU_HEX_CHARS = 600;
static constexpr size_t INDEX_QUEUE_MAX = 8;
static constexpr size_t OUT_SMS_QUEUE_MAX = 3;
static constexpr size_t SEEN_RING_MAX = 32;
static constexpr size_t CONCAT_SLOTS = 5;
static constexpr size_t CONCAT_PARTS = 10;
// 30s(Arduino 原值)在真实网络下太短：SIM 满被拒收的分段要等 SMSC 按分钟级
// 间隔重投，+CMTI 丢失时也要等轮询兜底，30s 一到就强拼出"[缺失分段]"残句。
// 直投仍可能遇到运营商分钟级重投，等待 15 分钟再带缺段标记转发。
static constexpr int64_t CONCAT_TIMEOUT_US = 15LL * 60LL * 1000LL * 1000LL;
// 存在未拼完的长短信时的加速轮询间隔：缺失分段若已落在 SIM 里(通知丢失)，
// 10s 内即可捞回，而不是等最长 60s 的常规轮询
static constexpr uint32_t CONCAT_HUNT_POLL_MS = 10000;
static constexpr uint32_t SMS_POLL_INTERVAL_MS = 60000;
static constexpr uint32_t SMS_STARTUP_POLL_INTERVAL_MS = 8000;
static constexpr uint32_t SMS_STARTUP_FAST_WINDOW_MS = 120000;
static constexpr uint8_t SMS_CNMI_REASSERT_EVERY = 5;
// 漫游/小区重选时 ML307 的 +CMGS 最终响应可能明显超过 20 秒；网页发送本身已异步，
// 这里允许等到 60 秒，避免 PDU 已交给模组却被固件过早判成失败。
static constexpr uint32_t SMS_SUBMIT_TIMEOUT_MS = 60000;

struct ConcatPart {
    bool valid = false;
    std::string text;
    std::string timestamp;
};

struct ConcatSlot {
    bool active = false;
    int ref = 0;
    int total = 0;
    int received = 0;
    std::string sender;
    std::string timestamp;
    int64_t lastUs = 0;
    int64_t receivedUs = 0;
    uint32_t receivedEpoch = 0;
    bool needsSmsc = false;
    std::array<ConcatPart, CONCAT_PARTS> parts;
};

struct OutgoingSmsJob {
    std::string phone;
    std::string text;
};

struct DecodedSms {
    std::string sender;
    std::string text;
    std::string timestamp;
    int concat[3] = {};
    int64_t receivedUs = 0;
    uint32_t receivedEpoch = 0;
};

static SemaphoreHandle_t s_status_mutex = nullptr;
static SemaphoreHandle_t s_pdu_mutex = nullptr;
static SemaphoreHandle_t s_out_mutex = nullptr;
static IdfSmsStatus s_status;
static IdfSmsReplyCheck s_reply_check;
static bool s_started = false;
static PDU s_pdu(4096);
struct SmsIndex { int index = 0; int64_t receivedUs = 0; };
static std::array<SmsIndex, INDEX_QUEUE_MAX> s_index_queue = {};
static size_t s_index_count = 0;
static std::array<OutgoingSmsJob, OUT_SMS_QUEUE_MAX> s_out_queue = {};
static size_t s_out_head = 0;
static size_t s_out_count = 0;
static std::array<uint32_t, SEEN_RING_MAX> s_seen = {};
static size_t s_seen_next = 0;
static size_t s_seen_filled = 0;
static std::array<ConcatSlot, CONCAT_SLOTS> s_concat = {};
static std::string s_urc_carry;
static bool s_wait_pdu = false;
static int64_t s_urc_received_us = 0;
static int64_t s_wait_pdu_received_us = 0;
static int64_t s_wait_pdu_until_us = 0;   // +CMT 后等 PDU 行的窗口截止(3s，对齐 Arduino)
static bool s_backfill_pending = false;   // 索引队列溢出/CMGR 失败时，请求一次近期 CMGL 兜底
static bool s_cnma_error_logged = false;  // 避免异常固件每条直推短信都刷同一条确认失败日志
static std::atomic<bool> s_admin_sms_busy{false};
static std::atomic<bool> s_admin_reset_pending{false};

static void cleanup_start_resources()
{
    if (s_status_mutex) {
        vSemaphoreDelete(s_status_mutex);
        s_status_mutex = nullptr;
    }
    if (s_pdu_mutex) {
        vSemaphoreDelete(s_pdu_mutex);
        s_pdu_mutex = nullptr;
    }
    if (s_out_mutex) {
        vSemaphoreDelete(s_out_mutex);
        s_out_mutex = nullptr;
    }
    s_admin_sms_busy.store(false, std::memory_order_relaxed);
    s_admin_reset_pending.store(false, std::memory_order_relaxed);
}

// SIM 存储索引 0 是合法值；必须严格解析，不能让乱码被宽松转换成 0。
static bool parse_sms_index_token(const char* start, size_t len, int& index)
{
    if (!start || len == 0 || len > 5) return false;
    int parsed = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = static_cast<unsigned char>(start[i]);
        if (!isdigit(ch)) return false;
        parsed = parsed * 10 + static_cast<int>(ch - '0');
    }
    index = parsed;
    return true;
}

static bool starts_with(const std::string& text, const char* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

static bool is_hex_string(const std::string& line)
{
    if (line.empty() || line.size() > MAX_PDU_HEX_CHARS || (line.size() & 1U)) return false;
    for (char ch : line) {
        if (!isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

// 仅供本次开机内的去重环使用，不要求跨重启稳定，直接用标准库哈希
static uint32_t hash32(const std::string& text)
{
    return static_cast<uint32_t>(std::hash<std::string>{}(text));
}

static bool seen_recently(uint32_t hash)
{
    // 用 filled 计数而不是排除 0 值：hash 恰为 0 的短信也要参与去重(对齐 Arduino)
    for (size_t i = 0; i < s_seen_filled; ++i) {
        if (s_seen[i] == hash) return true;
    }
    s_seen[s_seen_next] = hash;
    s_seen_next = (s_seen_next + 1) % SEEN_RING_MAX;
    if (s_seen_filled < SEEN_RING_MAX) ++s_seen_filled;
    return false;
}

static std::string canonical_phone(const std::string& num)
{
    size_t start = 0;
    while (start < num.size() && isspace(static_cast<unsigned char>(num[start]))) ++start;
    bool explicit_cn_prefix = start < num.size() && num[start] == '+';

    std::string out;
    out.reserve(num.size());
    for (size_t i = 0; i < num.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(num[i]);
        if (isdigit(ch)) out += static_cast<char>(ch);
    }
    if (out.rfind("86", 0) == 0 && out.size() > 2 && (explicit_cn_prefix || out.size() == 13)) {
        return out.substr(2);
    }
    return out;
}

static bool number_blacklisted(const std::string& list, const std::string& sender)
{
    if (list.empty()) return false;
    std::string target = canonical_phone(sender);
    if (target.empty()) return false;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos) end = list.size();
        std::string line = idf_util_trim_copy(list.substr(pos, end - pos));
        if (!line.empty() && canonical_phone(line) == target) {
            return true;
        }
        if (end == list.size()) break;
        pos = end + 1;
    }
    return false;
}

static bool is_valid_phone_number(const std::string& phone)
{
    if (phone.size() < 3 || phone.size() > 20) return false;
    for (size_t i = 0; i < phone.size(); ++i) {
        char ch = phone[i];
        if (i == 0 && ch == '+') continue;
        if (!isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

static bool is_admin_sender(const std::string& sender, const IdfSmsProcessView& cfg)
{
    if (cfg.adminPhone.empty()) return false;
    std::string admin = canonical_phone(cfg.adminPhone);
    return !admin.empty() && canonical_phone(sender) == admin;
}

struct AdminSmsTaskArg {
    std::string target;
    std::string content;
    std::string command;
};

static void admin_sms_task(void* raw)
{
    AdminSmsTaskArg* arg = static_cast<AdminSmsTaskArg*>(raw);
    if (!arg) {
        s_admin_sms_busy.store(false, std::memory_order_relaxed);
        vTaskDelete(nullptr);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
    std::string send_message;
    esp_err_t err = idf_sms_send_text(arg->target, arg->content, send_message);
    bool ok = (err == ESP_OK);
    std::string subject = ok ? "短信发送成功" : "短信发送失败";
    std::string body = "管理员命令执行结果:\n命令: " + arg->command +
                       "\n目标号码: " + arg->target +
                       "\n短信内容: " + arg->content +
                       "\n执行结果: " + (ok ? "成功" : "失败") +
                       "\n详情: " + send_message;
    idf_push_enqueue_email(subject.c_str(), body.c_str());
    delete arg;
    s_admin_sms_busy.store(false, std::memory_order_relaxed);
    vTaskDelete(nullptr);
}

static void admin_reset_task(void*)
{
    int64_t deadline = esp_timer_get_time() + 5LL * 1000LL * 1000LL;
    while ((idf_push_email_queue_depth() > 0 || idf_push_busy()) && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    idf_modem_request_reset(true);
    vTaskDelay(pdMS_TO_TICKS(1500));
    idf_log_line("正在重启ESP32...");
    // RESET 命令语义是"模组+ESP32 都彻底重启"：确保模组断电后再重启 ESP，
    // 避免热启动快路径把它当健康模组沿用
    idf_modem_power_off_for_restart();
    esp_restart();
}

static bool process_admin_command(const std::string& sender, const std::string& text)
{
    std::string cmd = idf_util_trim_copy(text);
    if (!(starts_with(cmd, "SMS:") || cmd == "RESET")) return false;

    idf_logf("处理管理员命令 from=%s", sender.c_str());
    if (starts_with(cmd, "SMS:")) {
        size_t first = cmd.find(':');
        size_t second = first == std::string::npos ? std::string::npos : cmd.find(':', first + 1);
        if (second == std::string::npos || second <= first + 1) {
            idf_log_line("SMS命令格式错误");
            idf_push_enqueue_email("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
            return true;
        }
        std::string target = idf_util_trim_copy(cmd.substr(first + 1, second - first - 1));
        std::string content = idf_util_trim_copy(cmd.substr(second + 1));
        idf_logf("管理员命令目标号码: %s", target.c_str());
        if (!is_valid_phone_number(target)) {
            idf_log_line("目标号码非法，拒绝执行");
            idf_push_enqueue_email("命令执行失败", "SMS命令目标号码非法（应为 3-20 位数字，可带 + 前缀）");
            return true;
        }
        if (content.empty() || content.size() > 300) {
            idf_log_line("短信内容为空或超长，拒绝执行");
            idf_push_enqueue_email("命令执行失败", "SMS命令内容为空或超过 300 字符");
            return true;
        }
        bool expected = false;
        if (!s_admin_sms_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            idf_log_line("已有管理员短信任务在执行，拒绝新的 SMS 命令");
            idf_push_enqueue_email("命令执行失败", "已有管理员短信任务在执行，请稍后重试");
            return true;
        }

        AdminSmsTaskArg* arg = new (std::nothrow) AdminSmsTaskArg();
        if (!arg) {
            s_admin_sms_busy.store(false, std::memory_order_release);
            idf_log_line("管理员短信任务创建失败：内存不足");
            idf_push_enqueue_email("命令执行失败", "管理员短信任务创建失败：内存不足");
            return true;
        }
        arg->target = target;
        arg->content = content;
        arg->command = cmd;
        if (xTaskCreate(admin_sms_task, "admin_sms", 4096, arg, 3, nullptr) != pdPASS) {
            delete arg;
            s_admin_sms_busy.store(false, std::memory_order_release);
            idf_log_line("管理员短信任务创建失败");
            idf_push_enqueue_email("命令执行失败", "管理员短信任务创建失败");
        }
        return true;
    }

    if (esp_timer_get_time() < 60LL * 1000LL * 1000LL) {
        idf_log_line("设备刚启动，忽略RESET命令（防重启风暴）");
        idf_push_enqueue_email("RESET已忽略", "设备启动不足60秒，已忽略RESET命令以防重启风暴。请稍后重试。");
        return true;
    }
    bool expected = false;
    if (!s_admin_reset_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        idf_log_line("RESET任务已在执行，忽略重复命令");
        idf_push_enqueue_email("RESET已忽略", "RESET任务已在执行，已忽略重复命令。");
        return true;
    }
    idf_log_line("执行RESET命令");
    idf_push_enqueue_email("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");
    if (xTaskCreate(admin_reset_task, "admin_reset", 3072, nullptr, 3, nullptr) != pdPASS) {
        idf_log_line("RESET任务创建失败，直接重启ESP32");
        esp_restart();
    }
    return true;
}

static void update_status(bool receive_ready, bool got_sms)
{
    if (!s_status_mutex) return;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    s_status.receiveReady = receive_ready || s_status.receiveReady;
    if (got_sms) {
        ++s_status.total;
        s_status.lastSmsEpoch = static_cast<uint32_t>(time(nullptr));
    }
    xSemaphoreGive(s_status_mutex);
}

static void enqueue_index(int idx, int64_t received_us)
{
    if (idx < 0) {
        s_backfill_pending = true;  // 非法索引：改由近期 CMGL 兜底(对齐 Arduino storedSmsPending)
        return;
    }
    for (size_t i = 0; i < s_index_count; ++i) {
        if (s_index_queue[i].index == idx) return;
    }
    if (s_index_count < INDEX_QUEUE_MAX) {
        s_index_queue[s_index_count++] = {idx, received_us};
    } else {
        s_backfill_pending = true;  // 队列满：丢索引但请求 CMGL 兜底，避免最长等 60s
    }
}

static bool pop_index(int& idx, int64_t& received_us)
{
    if (s_index_count == 0) return false;
    idx = s_index_queue[0].index;
    received_us = s_index_queue[0].receivedUs;
    for (size_t i = 1; i < s_index_count; ++i) s_index_queue[i - 1] = s_index_queue[i];
    --s_index_count;
    return true;
}

static int outgoing_depth_locked()
{
    return static_cast<int>(s_out_count);
}

static bool pop_outgoing_sms(OutgoingSmsJob& job)
{
    if (!s_out_mutex || xSemaphoreTake(s_out_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (s_out_count == 0) {
        xSemaphoreGive(s_out_mutex);
        return false;
    }
    job = std::move(s_out_queue[s_out_head]);
    s_out_queue[s_out_head] = OutgoingSmsJob();
    s_out_head = (s_out_head + 1) % OUT_SMS_QUEUE_MAX;
    --s_out_count;
    xSemaphoreGive(s_out_mutex);
    return true;
}

static int parse_cmti_index(const std::string& line)
{
    size_t comma = line.rfind(',');
    if (comma == std::string::npos || comma + 1 >= line.size()) return -1;
    std::string num = idf_util_trim_copy(line.substr(comma + 1));
    int index = -1;
    if (!parse_sms_index_token(num.c_str(), num.size(), index)) return -1;
    return index;
}

static int parse_cmgl_index(const std::string& line)
{
    const char* p = strchr(line.c_str(), ':');
    if (!p) return -1;
    while (*++p && isspace(static_cast<unsigned char>(*p))) {}
    const char* start = p;
    while (*p && isdigit(static_cast<unsigned char>(*p))) ++p;
    int index = -1;
    if (!parse_sms_index_token(start, static_cast<size_t>(p - start), index)) return -1;
    while (*p && isspace(static_cast<unsigned char>(*p))) ++p;
    if (*p != ',' && *p != '\0') return -1;
    return index;
}

static void clear_concat_slot(ConcatSlot& slot)
{
    slot.active = false;
    slot.ref = 0;
    slot.total = 0;
    slot.received = 0;
    slot.sender.clear();
    slot.timestamp.clear();
    slot.lastUs = 0;
    for (auto& part : slot.parts) {
        part.valid = false;
        part.text.clear();
        part.timestamp.clear();
    }
}

static std::string assemble_concat(const ConcatSlot& slot)
{
    std::string text;
    for (int i = 0; i < slot.total && i < static_cast<int>(CONCAT_PARTS); ++i) {
        if (slot.parts[i].valid) {
            text += slot.parts[i].text;
        } else {
            // 超时强制合并时标记缺口，收件人能看出内容不完整(对齐 Arduino)
            text += "[缺失分段";
            text += std::to_string(i + 1);
            text += "]";
        }
    }
    return text;
}

static void process_sms_content(const char* sender_raw, const char* text_raw, const char* timestamp_raw, int64_t received_us, uint32_t received_epoch, bool require_smsc = false);

// 已成功合并的长短信登记环：SIM 满时部分分段被拒收，SMSC 会整条重投(同 ref)。
// 槽位合并完成后迟到的重复分段若不识别，会重新开槽并在超时后拼出一条
// "[缺失分段]"幽灵消息。按 (ref,发件人,总段数,分段内容哈希) 精确匹配才丢弃，
// 避免运营商短期复用 ref 时误杀真实新消息。
struct ConcatDone {
    bool used = false;
    int ref = 0;
    int total = 0;
    std::string sender;
    std::array<uint32_t, CONCAT_PARTS> partHash = {};
    std::array<std::string, CONCAT_PARTS> partTimestamp;
    int64_t doneUs = 0;
};
static constexpr int64_t CONCAT_DONE_TTL_US = 10LL * 60 * 1000 * 1000;
static std::array<ConcatDone, 4> s_concat_done = {};
static size_t s_concat_done_next = 0;

static bool concat_recently_done(int ref, const std::string& sender, int total,
                                 int part, const std::string& text, const std::string& timestamp)
{
    int64_t now = esp_timer_get_time();
    for (const auto& d : s_concat_done) {
        if (!d.used || now - d.doneUs > CONCAT_DONE_TTL_US) continue;
        if (d.ref == ref && d.total == total && d.sender == sender &&
            d.partHash[part - 1] == hash32(text) && d.partTimestamp[part - 1] == timestamp) {
            return true;
        }
    }
    return false;
}

static void record_concat_done(const ConcatSlot& slot)
{
    ConcatDone& d = s_concat_done[s_concat_done_next];
    s_concat_done_next = (s_concat_done_next + 1) % s_concat_done.size();
    d.used = true;
    d.ref = slot.ref;
    d.total = slot.total;
    d.sender = slot.sender;
    d.partHash.fill(0);
    for (auto& timestamp : d.partTimestamp) timestamp.clear();
    for (int i = 0; i < slot.total && i < static_cast<int>(CONCAT_PARTS); ++i) {
        if (slot.parts[i].valid) {
            d.partHash[i] = hash32(slot.parts[i].text);
            d.partTimestamp[i] = slot.parts[i].timestamp;
        }
    }
    d.doneUs = esp_timer_get_time();
}

static ConcatSlot& find_concat_slot(int ref, const std::string& sender, int total)
{
    int64_t now = esp_timer_get_time();
    for (auto& slot : s_concat) {
        if (slot.active && slot.ref == ref && slot.sender == sender && slot.total == total) return slot;
    }
    for (auto& slot : s_concat) {
        if (!slot.active || now - slot.lastUs > CONCAT_TIMEOUT_US) {
            clear_concat_slot(slot);
            slot.active = true;
            slot.ref = ref;
            slot.total = total;
            slot.sender = sender;
            slot.lastUs = now;
            return slot;
        }
    }
    auto oldest = std::min_element(s_concat.begin(), s_concat.end(),
        [](const ConcatSlot& a, const ConcatSlot& b) { return a.lastUs < b.lastUs; });
    // 槽位全忙被挤占：已收到的分段不能悄悄扔掉——分段可能已从 SIM 删除，丢了
    // 就再也拿不回。与超时路径一致，先按现状合并转发(缺口有"[缺失分段]"标记)
    {
        std::string partial = assemble_concat(*oldest);
        if (!partial.empty()) {
            idf_logf("长短信槽位耗尽，先合并已收 %d/%d 段再复用槽位",
                     oldest->received, oldest->total);
            process_sms_content(oldest->sender.c_str(), partial.c_str(), oldest->timestamp.c_str(), -1, 0);
        }
    }
    clear_concat_slot(*oldest);
    oldest->active = true;
    oldest->ref = ref;
    oldest->total = total;
    oldest->sender = sender;
    oldest->lastUs = now;
    return *oldest;
}

static void process_sms_content(const char* sender_raw, const char* text_raw, const char* timestamp_raw, int64_t received_us, uint32_t received_epoch, bool require_smsc)
{
    std::string sender = sender_raw ? sender_raw : "";
    std::string text = text_raw ? text_raw : "";
    std::string timestamp = timestamp_raw ? timestamp_raw : "";
    const IdfSmsProcessView cfg = idf_config_get_sms_process_view();

    if (number_blacklisted(cfg.numberBlackList, sender)) {
        idf_logf("短信发件人 %s 在黑名单中，已忽略", sender.c_str());
        return;
    }

    // 去重键用 PDU 原始时间戳(双通道 URC+CMGL 收到的是同一原始值)；
    // 展示/转发用本地可读时间，未同步时才退回原始数字串
    uint32_t hash = hash32(sender + "|" + timestamp + "|" + text);
    if (seen_recently(hash)) {
        idf_logf("重复短信 %s 已忽略", sender.c_str());
        return;
    }

    // 去重后的完整短信参与验证，不消费短信，后续收件箱/转发行为保持不变。
    if (s_status_mutex && xSemaphoreTake(s_status_mutex, portMAX_DELAY) == pdTRUE) {
        s_reply_check.observe(sender.c_str(), text.c_str(), esp_timer_get_time(), received_us, received_epoch, static_cast<uint32_t>(time(nullptr)), require_smsc);
        xSemaphoreGive(s_status_mutex);
    }

    if (is_admin_sender(sender, cfg)) {
        idf_log_line("收到管理员短信，检查命令...");
        if (process_admin_command(sender, text)) {
            update_status(true, true);
            return;
        }
    }

    std::string display_ts = idf_util_format_epoch_local(static_cast<uint32_t>(time(nullptr)), cfg.tzOffsetMin);
    if (display_ts.empty()) display_ts = timestamp;

    uint32_t id = idf_inbox_add(sender.c_str(), text.c_str(), display_ts.c_str());
    update_status(true, true);
    // 默认日志展示完整发件人(便于确认是谁)，正文仍不落盘(在收发短信页查看)
    idf_logf("收到短信 id=%u 来自 %s，入队转发中", static_cast<unsigned>(id), sender.c_str());
    if (!idf_push_enqueue_forward(sender.c_str(), text.c_str(), display_ts.c_str(), id)) {
        idf_logf("转发入口队列已满，短信 id=%u 保持未转发，可稍后手动重发", static_cast<unsigned>(id));
    }
}

static void handle_decoded_pdu(const DecodedSms& sms)
{
    const char* sender = sms.sender.c_str();
    const char* text = sms.text.c_str();
    const char* ts = sms.timestamp.c_str();
    int ref = sms.concat[0];
    int part = sms.concat[1];
    int total = sms.concat[2];

    if (total > 1 && part > 0) {
        if (total > static_cast<int>(CONCAT_PARTS) || part > total) {
            idf_logf("长短信分段参数超限 part=%d total=%d，按单条处理", part, total);
            process_sms_content(sender, text, ts, -1, 0);
            return;
        }
        if (concat_recently_done(ref, sender ? sender : "", total, part,
                                 text ? text : "", ts ? ts : "")) {
            idf_logf("长短信分段 ref=%d %d/%d 与近期已合并消息重复，忽略", ref, part, total);
            return;
        }
        ConcatSlot& slot = find_concat_slot(ref, sender ? sender : "", total);
        int idx = part - 1;
        if (slot.parts[idx].valid && slot.parts[idx].text != (text ? text : "")) {
            // 8 位引用号可能被运营商快速复用；同一分段号正文已变化时不能继续混拼旧消息。
            idf_logf("长短信引用号 ref=%d 已复用，丢弃旧的不完整 %d/%d 段并重新归组",
                     ref, slot.received, slot.total);
            clear_concat_slot(slot);
            slot.active = true;
            slot.ref = ref;
            slot.total = total;
            slot.sender = sender ? sender : "";
            slot.lastUs = esp_timer_get_time();
        }
        if (!slot.parts[idx].valid) {
            slot.parts[idx].valid = true;
            slot.parts[idx].text = text ? text : "";
            slot.parts[idx].timestamp = ts ? ts : "";
            // 保留已知片段的最早到达时间；未知片段另作 SMSC 校验，不能覆盖已知旧片段证据。
            if (slot.received == 0) {
                slot.receivedUs = sms.receivedUs;
                slot.needsSmsc = sms.receivedUs == 0;
            } else {
                if (sms.receivedUs > 0) slot.receivedUs = slot.receivedUs > 0 ? std::min(slot.receivedUs, sms.receivedUs) : sms.receivedUs;
                slot.needsSmsc = slot.needsSmsc || sms.receivedUs == 0;
            }
            slot.receivedEpoch = slot.received == 0 ? sms.receivedEpoch : std::min(slot.receivedEpoch, sms.receivedEpoch);
            slot.received++;
            slot.lastUs = esp_timer_get_time();
            if (slot.timestamp.empty()) slot.timestamp = ts ? ts : "";
            idf_logf("收到长短信分段 ref=%d %d/%d", ref, part, total);
        }
        if (slot.received >= slot.total) {
            record_concat_done(slot);
            std::string full = assemble_concat(slot);
            process_sms_content(slot.sender.c_str(), full.c_str(), slot.timestamp.c_str(), slot.receivedUs, slot.receivedEpoch, slot.needsSmsc);
            clear_concat_slot(slot);
        }
        return;
    }

    process_sms_content(sender, text, ts, sms.receivedUs, sms.receivedEpoch);
}

static bool decode_pdu_line(const std::string& line, int64_t received_us = 0)
{
    if (!is_hex_string(line)) return false;
    if (!s_pdu_mutex) return false;
    // 解码器忙(网页发短信正在编码)时重试而不是放弃：返回 false 会被调用方
    // 当成"PDU 损坏"——直推短信直接丢失，存储短信还会被误删
    bool locked = false;
    for (int attempt = 0; attempt < 3 && !locked; ++attempt) {
        locked = xSemaphoreTake(s_pdu_mutex, pdMS_TO_TICKS(2000)) == pdTRUE;
        if (!locked) idf_log_line("PDU 解码器忙，等待重试...");
    }
    if (!locked) return false;
    DecodedSms decoded;
    if (!s_pdu.decodePDU(line.c_str())) {
        xSemaphoreGive(s_pdu_mutex);
        idf_logf("PDU 解析失败(hex=%u)", static_cast<unsigned>(line.size()));
        return false;
    }
    decoded.sender = s_pdu.getSender();
    decoded.text = s_pdu.getText();
    decoded.timestamp = s_pdu.getTimeStamp();
    decoded.receivedUs = received_us;
    decoded.receivedEpoch = idf_sms_timestamp_epoch(decoded.timestamp.c_str());
    int* concat = s_pdu.getConcatInfo();
    if (concat) {
        decoded.concat[0] = concat[0];
        decoded.concat[1] = concat[1];
        decoded.concat[2] = concat[2];
    }
    xSemaphoreGive(s_pdu_mutex);
    // PDU 解码器是全局复用对象；业务处理放到锁外，避免入库/转发时阻塞网页发短信编码。
    handle_decoded_pdu(decoded);
    return true;
}

// 是否有等待补齐的长短信槽位(仅 sms_task 内访问，无需加锁)
static bool concat_waiting()
{
    for (const auto& slot : s_concat) {
        if (slot.active && slot.received < slot.total) return true;
    }
    return false;
}

static void expire_concat_slots()
{
    int64_t now = esp_timer_get_time();
    for (auto& slot : s_concat) {
        if (!slot.active || now - slot.lastUs <= CONCAT_TIMEOUT_US) continue;
        std::string full = assemble_concat(slot);
        if (!full.empty()) {
            idf_logf("长短信等待超时，已合并现有 %d/%d 段", slot.received, slot.total);
            process_sms_content(slot.sender.c_str(), full.c_str(), slot.timestamp.c_str(), -1, 0);
        }
        clear_concat_slot(slot);
    }
}

// ===== 来电通知 =====
// +CLIP 是不阻塞的主路径；缺失时等 3 秒再用 CLCC 查询一次，兼容不主动上报 CLIP 的固件。
static int64_t s_call_started_us = 0;
static int64_t s_call_last_urc_us = 0;
static bool s_call_notified = false;
static bool s_call_saw_ring = false;
static std::string s_call_number;
static constexpr int64_t CALL_GAP_US = 30LL * 1000 * 1000;
static constexpr int64_t CALL_CLCC_DELAY_US = 3LL * 1000 * 1000;

static std::string parse_clip_number(const std::string& line)
{
    size_t q1 = line.find('"');
    if (q1 == std::string::npos) return {};
    size_t q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return line.substr(q1 + 1, q2 - q1 - 1);
}

static std::string query_clcc_number()
{
    std::string resp;
    if (idf_modem_send_at("AT+CLCC", 1500, resp) != ESP_OK) return {};
    size_t pos = 0;
    while (pos < resp.size()) {
        size_t eol = resp.find('\n', pos);
        if (eol == std::string::npos) eol = resp.size();
        std::string line = idf_util_trim_copy(resp.substr(pos, eol - pos));
        int direction = -1;
        int state = -1;
        if (sscanf(line.c_str(), "+CLCC: %*d,%d,%d", &direction, &state) == 2 &&
            direction == 1 && (state == 4 || state == 5)) {
            return parse_clip_number(line);
        }
        pos = eol + 1;
    }
    return {};
}

static void notify_incoming_call(const std::string& number)
{
    if (!idf_config_call_notify_enabled()) return;
    // 黑名单同时作用于来电：命中则不通知(未知号码无从匹配，照常通知)
    if (!number.empty()) {
        const IdfSmsProcessView cfg = idf_config_get_sms_process_view();
        if (number_blacklisted(cfg.numberBlackList, number)) {
            idf_logf("来电 %s 在黑名单中，已忽略", number.c_str());
            return;
        }
    }
    std::string num = number.empty() ? std::string("未知号码") : number;
    std::string body = "来电：" + num;
    std::string display_ts = idf_util_format_epoch_local(static_cast<uint32_t>(time(nullptr)),
                                                         idf_config_get_tz_offset());
    idf_logf("来电通知：%s，入队转发中", num.c_str());
    if (!idf_push_enqueue_forward(num.c_str(), body.c_str(), display_ts.c_str(), 0)) {
        idf_log_line("转发入口队列已满，来电通知未能入队");
    }
}

static void handle_call_urc_line(const std::string& line)
{
    int64_t now = esp_timer_get_time();
    if (s_call_last_urc_us == 0 || now - s_call_last_urc_us > CALL_GAP_US) {
        s_call_started_us = now;
        s_call_notified = false;
        s_call_saw_ring = false;
        s_call_number.clear();
    }
    s_call_last_urc_us = now;
    if (starts_with(line, "+CLIP:")) {
        std::string number = parse_clip_number(line);
        if (!number.empty()) s_call_number = number;
        if (!s_call_notified && !s_call_number.empty()) {
            s_call_notified = true;
            notify_incoming_call(s_call_number);
        }
    } else {
        s_call_saw_ring = true;
    }
}

static void flush_pending_call_notify(void)
{
    if (!s_call_saw_ring || s_call_notified ||
        esp_timer_get_time() - s_call_started_us < CALL_CLCC_DELAY_US) return;
    s_call_notified = true;
    s_call_number = query_clcc_number();
    notify_incoming_call(s_call_number);
}

static void process_urc_line(const std::string& raw)
{
    std::string line = idf_util_trim_copy(raw);
    if (line.empty()) return;
    if (starts_with(line, "@RXUS:")) {
        s_urc_received_us = strtoll(line.c_str() + 6, nullptr, 10);
        return;
    }

    // 来电通知：RING/+CLIP 优先处理并返回，独立于短信 PDU 等待逻辑
    if (line == "RING" || starts_with(line, "+CLIP:")) {
        handle_call_urc_line(line);
        return;
    }

    if (s_wait_pdu) {
        if (starts_with(line, "+CMT:")) {
            // 窗口内又来一条 +CMT：前一条的 PDU 已丢(直推短信不落存储无从兜底)，
            // 重臂窗口至少保住新的这条，并记一笔便于排查
            idf_log_line("等待直推 PDU 时又收到 +CMT，前一条可能丢失");
            size_t comma = line.rfind(',');
            idf_logf("收到直推短信头，TPDU 长度=%d",
                     comma == std::string::npos ? -1 : atoi(line.c_str() + comma + 1));
            s_wait_pdu_received_us = s_urc_received_us;
            s_wait_pdu_until_us = esp_timer_get_time() + 3LL * 1000LL * 1000LL;
            return;
        }
        if (starts_with(line, "+CMTI:")) {
            enqueue_index(parse_cmti_index(line), s_urc_received_us);
            return;
        }
        if (decode_pdu_line(line, s_wait_pdu_received_us)) {
            s_wait_pdu = false;
            // ML307R 的 +CNMI 文档要求直推短信通过 +CNMA 确认，确认后模组才会可靠地
            // 继续上报同一条长短信的后续分段。存储读取(CMGR/CMGL)不走此分支。
            std::string ack_resp;
            if (idf_modem_send_at("AT+CNMA=0", 1500, ack_resp) == ESP_OK) {
                s_cnma_error_logged = false;
            } else if (!s_cnma_error_logged) {
                s_cnma_error_logged = true;
                idf_log_line("直推短信确认失败，后续分段可能由 SIM 存储轮询补收");
            }
            return;
        }
        if (line != "OK" && line != "ERROR") {
            idf_log_line("等待直推 PDU 时收到非 PDU 行，已关闭接收窗口");
            s_wait_pdu = false;
            s_backfill_pending = true;  // 若这条其实是被截断的 PDU，兜底轮询还能救回存储中的副本
        }
        return;
    }

    if (starts_with(line, "+CMT:")) {
        size_t comma = line.rfind(',');
        idf_logf("收到直推短信头，TPDU 长度=%d",
                 comma == std::string::npos ? -1 : atoi(line.c_str() + comma + 1));
        s_wait_pdu = true;
        s_wait_pdu_received_us = s_urc_received_us;
        s_wait_pdu_until_us = esp_timer_get_time() + 3LL * 1000LL * 1000LL;  // 3s 窗口
    } else if (starts_with(line, "+CMTI:")) {
        enqueue_index(parse_cmti_index(line), s_urc_received_us);
    }
}

// +CMT 后迟迟等不到 PDU 行时关闭窗口，避免后续无关的十六进制样 URC 被误当短信解码
static void expire_wait_pdu_window()
{
    if (s_wait_pdu && esp_timer_get_time() > s_wait_pdu_until_us) {
        s_wait_pdu = false;
        s_backfill_pending = true;  // 直推丢失的短信大概率还在存储里，请求兜底补收
    }
}

static void process_urc_text(const std::string& text)
{
    s_urc_carry += text;
    size_t pos = 0;
    while (true) {
        size_t nl = s_urc_carry.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = s_urc_carry.substr(pos, nl - pos);
        process_urc_line(line);
        pos = nl + 1;
    }
    if (pos > 0) s_urc_carry.erase(0, pos);
    if (s_urc_carry.size() > MAX_PDU_HEX_CHARS + 128) {
        s_urc_carry.clear();
        s_wait_pdu = false;
    }
}

static bool extract_first_stored_pdu(const std::string& resp, const char* header, std::string& pdu_line)
{
    pdu_line.clear();
    size_t h = resp.find(header);
    if (h == std::string::npos) return false;
    size_t pos = resp.find('\n', h);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < resp.size()) {
        size_t nl = resp.find('\n', pos);
        if (nl == std::string::npos) nl = resp.size();
        std::string line = idf_util_trim_copy(resp.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line == "OK" || line == "ERROR" ||
            starts_with(line, "+") || starts_with(line, "AT")) {
            continue;
        }
        pdu_line = line;
        return true;
    }
    return false;
}

static void fetch_stored_sms_by_index(int idx, int64_t received_us)
{
    if (idx < 0) return;
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idx);
    std::string resp;
    esp_err_t err = idf_modem_send_at(cmd, 3000, resp);
    bool has_header = resp.find("+CMGR:") != std::string::npos;
    std::string pdu_line;
    bool has_pdu_line = extract_first_stored_pdu(resp, "+CMGR:", pdu_line);
    bool decoded = has_pdu_line && decode_pdu_line(pdu_line, received_us);
    if (has_header) {
        if (decoded) {
            snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", idx);
            std::string ignored;
            idf_modem_send_at(cmd, 2000, ignored);
        } else if (has_pdu_line) {
            idf_logf("PDU 无法解析(索引=%d)，保留 SIM 记录等待后续重试", idx);
            s_backfill_pending = true;
        } else {
            idf_logf("索引=%d 的短信缺少 PDU 行，等待 CMGL 兜底", idx);
            s_backfill_pending = true;
        }
    } else if (err != ESP_OK) {
        // 不重排队：空槽位(补收已删)/+CMS ERROR 会形成无限重试环，把收发队列全部饿死。
        // 改为请求一次 CMGL 兜底——真有短信它一定还在存储列表里。
        s_backfill_pending = true;
    }
}

static void backfill_stored_sms(bool announce)
{
    std::string resp;
    esp_err_t err = idf_modem_send_at("AT+CMGL=4", 4000, resp);
    if (err != ESP_OK && resp.find("+CMGL:") == std::string::npos) {
        if (announce) idf_logf("SIM 暂存短信读取失败: %s", esp_err_to_name(err));
        return;
    }

    // 每轮最多处理 5 条就归还控制权：处理间隙主循环能继续排空 URC、检查长短信超时、
    // 发送网页短信。剩余的通过 s_backfill_pending 在 ~500ms 后接着处理。
    constexpr int BATCH_MAX = 5;
    // 连续多轮"有缺 PDU 行的条目且毫无进展"的计数：损坏记录若只重试不删除，
    // 会形成每 ~200ms 一次 AT+CMGL 的永久轮询，把整个 AT 通道饿死
    static uint8_t s_nopdu_stall_rounds = 0;
    static uint8_t s_decode_stall_rounds = 0;
    int nopdu_idx[BATCH_MAX];
    int decode_fail_idx[BATCH_MAX];
    int nopdu_count = 0;
    int decode_fail_count = 0;
    int processed = 0;
    int handled = 0;
    bool more_left = false;
    size_t pos = 0;
    while (pos < resp.size()) {
        size_t nl = resp.find('\n', pos);
        if (nl == std::string::npos) nl = resp.size();
        std::string line = idf_util_trim_copy(resp.substr(pos, nl - pos));
        pos = nl + 1;
        if (!starts_with(line, "+CMGL:")) continue;
        if (handled >= BATCH_MAX) {
            more_left = true;
            break;
        }

        int idx = parse_cmgl_index(line);
        std::string pdu_line;
        bool has_pdu_line = false;
        while (pos < resp.size()) {
            nl = resp.find('\n', pos);
            if (nl == std::string::npos) nl = resp.size();
            std::string candidate = idf_util_trim_copy(resp.substr(pos, nl - pos));
            if (starts_with(candidate, "+CMGL:")) break;  // PDU 缺失：别把下一条的头当 PDU 吞掉
            pos = nl + 1;
            if (candidate.empty() || candidate == "OK" || candidate == "ERROR" ||
                starts_with(candidate, "+") || starts_with(candidate, "AT")) {
                continue;
            }
            if (!candidate.empty()) {
                pdu_line = candidate;
                has_pdu_line = true;
                break;
            }
        }

        bool decoded = has_pdu_line && decode_pdu_line(pdu_line);
        if (decoded) ++processed;
        if (idx >= 0 && decoded) {
            char cmd[24];
            snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", idx);
            std::string ignored;
            idf_modem_send_at(cmd, 2000, ignored);
            ++handled;
        } else if (idx >= 0 && has_pdu_line) {
            idf_logf("PDU 无法解析(索引=%d)，保留 SIM 记录等待后续重试", idx);
            if (decode_fail_count < BATCH_MAX) decode_fail_idx[decode_fail_count++] = idx;
        } else if (idx >= 0) {
            idf_logf("CMGL 索引=%d 缺少 PDU 行，暂不删除", idx);
            if (nopdu_count < BATCH_MAX) nopdu_idx[nopdu_count++] = idx;
        }
    }

    if (nopdu_count > 0) {
        if (handled > 0 || processed > 0) {
            // 本轮有进展：缺行大概率是 8KB 响应截断，删掉已处理的条目后下轮自然恢复
            s_nopdu_stall_rounds = 0;
            s_backfill_pending = true;
        } else if (++s_nopdu_stall_rounds >= 3) {
            // 连续 3 轮原地踏步 = 记录本身损坏，删除释放存储，终结轮询死循环
            for (int i = 0; i < nopdu_count; ++i) {
                idf_logf("索引=%d 连续多轮缺少 PDU 行(记录损坏)，删除以恢复正常轮询", nopdu_idx[i]);
                char cmd[24];
                snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", nopdu_idx[i]);
                std::string ignored;
                idf_modem_send_at(cmd, 2000, ignored);
            }
            s_nopdu_stall_rounds = 0;
        } else {
            s_backfill_pending = true;
        }
    } else {
        s_nopdu_stall_rounds = 0;
    }

    if (decode_fail_count > 0) {
        if (handled > 0 || processed > 0) {
            s_decode_stall_rounds = 0;
            s_backfill_pending = true;
        } else if (++s_decode_stall_rounds >= 3) {
            for (int i = 0; i < decode_fail_count; ++i) {
                idf_logf("索引=%d 连续多轮 PDU 解析失败(记录损坏)，删除以恢复正常轮询", decode_fail_idx[i]);
                char cmd[24];
                snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", decode_fail_idx[i]);
                std::string ignored;
                idf_modem_send_at(cmd, 2000, ignored);
            }
            s_decode_stall_rounds = 0;
        } else {
            s_backfill_pending = true;
        }
    } else {
        s_decode_stall_rounds = 0;
    }

    if (more_left) s_backfill_pending = true;
    if (processed > 0) idf_logf("SIM 暂存短信处理并删除 %d 条", processed);
}

static void sms_task(void*)
{
    uint64_t last_poll_ms = 0;  // 64 位毫秒：32 位在 49.7 天回绕会重进启动提频窗口
    uint64_t start_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
    uint32_t poll_count = 0;
    uint8_t reassert_step = 0;  // 1=CSMS, 2=CMGF, 3=CPMS, 4=CNMI；拆帧避免一次堆多条 AT
    bool backfill_after_reassert = false;
    bool configured = false;
    bool first_backfill = true;

    while (true) {
        std::string urc;
        if (idf_modem_take_urc(urc)) process_urc_text(urc);
        expire_wait_pdu_window();
        flush_pending_call_notify();
        expire_concat_slots();

        // eSIM 逻辑通道期间只收 URC，不插入 CPMS、CMGL 或短信发送 AT。
        if (idf_modem_esim_operation_active()) {
            idf_modem_wait_event(500);
            continue;
        }

        IdfModemStatus modem = idf_modem_get_status();
        if (modem.atReady && !configured) {
            std::string ignored;
            idf_modem_send_at("AT+CSMS=1", 1200, ignored);
            idf_modem_send_at("AT+CMGF=0", 1200, ignored);
            idf_modem_send_at("AT+CNMI=2,2,0,0,0", 1200, ignored);
            configured = true;
            update_status(true, false);
            idf_log_line("短信接收(PDU/存储通知)已配置");
        }

        if (configured && modem.modemReady) {
            if (reassert_step != 0) {
                std::string ignored;
                if (reassert_step == 1) {
                    idf_modem_send_at("AT+CSMS=1", 1200, ignored);
                    reassert_step = 2;
                } else if (reassert_step == 2) {
                    idf_modem_send_at("AT+CMGF=0", 1200, ignored);
                    reassert_step = 3;
                } else if (reassert_step == 3) {
                    // CPMS 也要重申：模组自发复位后存储会回落固件默认，
                    // SM 容量为 0 的 eSIM 上接收会静默死亡(只 CMGF/CNMI 救不回来)
                    idf_modem_reassert_sms_storage();
                    reassert_step = 4;
                } else {
                    idf_modem_send_at("AT+CNMI=2,2,0,0,0", 1200, ignored);
                    reassert_step = 0;
                    backfill_after_reassert = true;
                }
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }

            if (backfill_after_reassert) {
                backfill_after_reassert = false;
                backfill_stored_sms(false);
                last_poll_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            int idx = -1;
            int64_t received_us = 0;
            if (pop_index(idx, received_us)) {
                fetch_stored_sms_by_index(idx, received_us);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            OutgoingSmsJob out;
            if (pop_outgoing_sms(out)) {
                std::string send_message;
                idf_logf("网页短信出队发送: %s len=%u",
                         out.phone.c_str(), static_cast<unsigned>(out.text.size()));
                idf_sms_send_text(out.phone, out.text, send_message);
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            if (s_backfill_pending) {
                s_backfill_pending = false;
                backfill_stored_sms(false);
                last_poll_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
            uint32_t interval = (now_ms - start_ms < SMS_STARTUP_FAST_WINDOW_MS)
                ? SMS_STARTUP_POLL_INTERVAL_MS
                : SMS_POLL_INTERVAL_MS;
            // 长短信缺段等待期加速轮询：缺失分段可能已静默落在 SIM 里
            // (+CMTI 混进大响应被截断丢失)，主动补收而不是干等常规轮询
            if (concat_waiting() && interval > CONCAT_HUNT_POLL_MS) interval = CONCAT_HUNT_POLL_MS;
            if (first_backfill || now_ms - last_poll_ms >= interval) {
                if (!first_backfill && (poll_count % SMS_CNMI_REASSERT_EVERY) == 0) {
                    reassert_step = 1;
                    ++poll_count;
                    last_poll_ms = now_ms;
                    continue;
                }
                backfill_stored_sms(first_backfill);
                first_backfill = false;
                last_poll_ms = now_ms;
                ++poll_count;
            }
        }

        // 事件等待：URC 到达/网页短信入队立即唤醒；无事件 500ms 兜底(与原轮询节奏一致)
        idf_modem_wait_event(500);
    }
}

esp_err_t idf_sms_start(void)
{
    if (s_started) return ESP_OK;
    cleanup_start_resources();
    s_status_mutex = xSemaphoreCreateMutex();
    s_pdu_mutex = xSemaphoreCreateMutex();
    s_out_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex || !s_pdu_mutex || !s_out_mutex) {
        cleanup_start_resources();
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(sms_task, "idf_sms", 8192, nullptr, 3, nullptr);
    if (ok != pdPASS) {
        cleanup_start_resources();
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

// —— 长短信发送辅助 ——
// 保守判定"纯 GSM7"：仅 ASCII 可打印字符与 \r\n。其余(含中文)按 UCS2 处理。
// 保守的意义：切分假设 GSM7 但实际落入 UCS2 会导致该段超长编码失败，反之只是少装几个字
static bool sms_text_basic_gsm7(const std::string& text)
{
    for (unsigned char c : text) {
        if (c == '\r' || c == '\n') continue;
        if (c < 0x20 || c >= 0x7F) return false;
        // 反引号不属于 GSM 03.38；PDU 编码器会为整段改用 UCS2。
        // 若这里误判为 GSM7，71~160 字符的混合文本会直到编码阶段才超长失败。
        if (c == '`') return false;
    }
    return true;
}

// 按每段配额切分，不打断 UTF-8 字符。gsm7 按 septet 计(扩展表字符占 2)，
// UCS2 按 UTF-16 码元计(BMP 外字符即代理对占 2)
static std::vector<std::string> sms_split_parts(const std::string& text, bool gsm7, size_t per_part)
{
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t units = 0;
        size_t end = pos;
        while (end < text.size()) {
            unsigned char lead = static_cast<unsigned char>(text[end]);
            size_t clen = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
            if (end + clen > text.size()) clen = text.size() - end;
            size_t u;
            if (gsm7) u = (clen == 1 && strchr("[]{}\\^~|", text[end])) ? 2 : 1;
            else u = (clen == 4) ? 2 : 1;
            if (units + u > per_part) break;
            units += u;
            end += clen;
        }
        if (end == pos) break;  // 防御：不应发生(单字符不会超过任何配额)
        parts.push_back(text.substr(pos, end - pos));
        pos = end;
    }
    return parts;
}

// 编码单个(或长短信某段)PDU；csms=0 表示普通单条
static esp_err_t sms_encode_one(const std::string& phone, const std::string& body,
                                uint16_t csms, uint8_t total, uint8_t part,
                                std::string& sms_pdu, int& pdu_len, std::string& message)
{
    if (!s_pdu_mutex || xSemaphoreTake(s_pdu_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        message = "PDU 编码器忙";
        return ESP_ERR_TIMEOUT;
    }
    s_pdu.setSCAnumber();
    pdu_len = s_pdu.encodePDU(phone.c_str(), body.c_str(), csms, total, part);
    if (pdu_len >= 0) sms_pdu.assign(s_pdu.getSMS(), strlen(s_pdu.getSMS()));
    xSemaphoreGive(s_pdu_mutex);
    if (pdu_len < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "PDU 编码失败(%d)", pdu_len);
        message = buf;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static std::string sms_response_error_line(const std::string& response)
{
    static const char* kTokens[] = {"+CMS ERROR", "+CME ERROR", "ERROR"};
    size_t pos = 0;
    while (pos < response.size()) {
        size_t end = response.find('\n', pos);
        if (end == std::string::npos) end = response.size();
        std::string line = idf_util_trim_copy(response.substr(pos, end - pos));
        for (const char* token : kTokens) {
            if (line.find(token) != std::string::npos) return line;
        }
        pos = end + 1;
    }
    return {};
}

static std::string sms_submit_failure_detail(esp_err_t err, const std::string& response)
{
    std::string error_line = sms_response_error_line(response);
    if (!error_line.empty()) return error_line;
    if (err == ESP_ERR_TIMEOUT) {
        if (response.find('>') != std::string::npos) {
            return "短信提交超时（已提交 PDU，但模组未返回 +CMGS/OK，发送结果未知）";
        }
        return "等待短信输入提示符超时（PDU 尚未提交）";
    }
    if (err == ESP_OK) return "模组响应格式异常";
    return esp_err_to_name(err);
}

static void reply_check_submitted()
{
    if (!s_status_mutex) return;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_reply_check.submitted(esp_timer_get_time(), static_cast<uint32_t>(time(nullptr)));
    xSemaphoreGive(s_status_mutex);
}

esp_err_t idf_sms_send_text(const std::string& phone_raw, const std::string& text_raw, std::string& message, bool track_reply)
{
    message.clear();
    std::string phone = idf_util_trim_copy(phone_raw);
    std::string text = idf_util_trim_copy(text_raw);
    if (!is_valid_phone_number(phone) || text.empty() || text.size() > 300) {
        message = "号码或内容无效";
        return ESP_ERR_INVALID_ARG;
    }
    IdfModemStatus modem = idf_modem_get_status();
    if (!modem.modemReady) {
        message = "模组尚未注册网络";
        return ESP_ERR_INVALID_STATE;
    }

    // 单条容量：GSM7 160 septet / UCS2 70 码元；超出则切分长短信。
    // 分段配额扣除 7 字节 UDH(16 位参考号)：GSM7 152 septet / UCS2 66 码元
    bool gsm7 = sms_text_basic_gsm7(text);
    std::vector<std::string> parts = sms_split_parts(text, gsm7, gsm7 ? 160 : 70);
    if (parts.empty()) {
        message = "号码或内容无效";
        return ESP_ERR_INVALID_ARG;
    }
    if (parts.size() > 1) parts = sms_split_parts(text, gsm7, gsm7 ? 152 : 66);

    uint16_t csms = 0;
    if (parts.size() > 1) {
        // 16 位参考号从熵源取初值后自增，0 保留为"非长短信"
        static std::atomic<uint16_t> s_csms_ref{static_cast<uint16_t>(esp_timer_get_time() & 0x7FFF)};
        csms = s_csms_ref.fetch_add(1, std::memory_order_relaxed);
        if (csms == 0) csms = s_csms_ref.fetch_add(1, std::memory_order_relaxed);
    }

    esp_err_t err = ESP_OK;
    // 热插拔/模组内部协议栈复位可能把 CMGF 恢复成文本模式。文本模式同样会返回
    // `>`，但随后把十六进制 PDU 当普通正文处理，最终表现为无 +CMGS 的超时。
    // 每次发送前重申一次 PDU 模式，代价很小，却能直接封住这一类静默状态漂移。
    std::string mode_resp;
    err = idf_modem_send_at("AT+CMGF=0", 3000, mode_resp);
    if (err != ESP_OK) {
        message = "无法恢复短信 PDU 模式: " + sms_submit_failure_detail(err, mode_resp);
        idf_sent_add(phone.c_str(), text.c_str(), false);
        idf_logf("发送前短信模式配置失败，触发模组硬重启: %s", message.c_str());
        idf_modem_request_reset(true);
        return err;
    }

    for (size_t i = 0; i < parts.size(); ++i) {
        std::string sms_pdu;
        int pdu_len = -1;
        // encodePDU 要求 csms/numparts/partnumber 三者同为 0(单条)或同非 0(分段)
        err = sms_encode_one(phone, parts[i], csms,
                             csms ? static_cast<uint8_t>(parts.size()) : 0,
                             csms ? static_cast<uint8_t>(i + 1) : 0,
                             sms_pdu, pdu_len, message);
        if (err != ESP_OK) {
            idf_sent_add(phone.c_str(), text.c_str(), false);
            return err;
        }
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", pdu_len);
        std::string resp;
        err = idf_modem_send_pdu(cmd, sms_pdu.c_str(), SMS_SUBMIT_TIMEOUT_MS, resp,
                                 track_reply && i + 1 == parts.size() ? reply_check_submitted : nullptr);
        if (err != ESP_OK) {
            std::string detail = sms_submit_failure_detail(err, resp);
            if (parts.size() > 1) {
                char prefix[64];
                snprintf(prefix, sizeof(prefix), "长短信第 %u/%u 段发送失败: ",
                         static_cast<unsigned>(i + 1), static_cast<unsigned>(parts.size()));
                message = std::string(prefix) + detail;
            } else {
                message = detail;
            }
            if (err == ESP_ERR_TIMEOUT) {
                // Ctrl+Z 后超时属于结果不确定态，不能自动重发（否则可能重复扣费/重复送达）；
                // 但必须把模组拉回干净状态，保证下一条短信不被残留 CMGS 会话污染。
                idf_log_line("短信提交超时，自动硬重启模组恢复短信通道；本条不自动重发");
                idf_modem_request_reset(true);
            }
            idf_sent_add(phone.c_str(), text.c_str(), false);
            idf_logf("网页发送短信失败: %s", message.c_str());
            return err;
        }
        // 分段之间稍作停顿，给模组喘息并让 URC 有机会被处理
        if (i + 1 < parts.size()) vTaskDelay(pdMS_TO_TICKS(300));
    }

    idf_sent_add(phone.c_str(), text.c_str(), true);
    if (parts.size() > 1) {
        char buf[64];
        snprintf(buf, sizeof(buf), "长短信发送成功(共 %u 段)", static_cast<unsigned>(parts.size()));
        message = buf;
    } else {
        message = "短信发送成功";
    }
    idf_logf("网页发送短信成功: %s len=%u parts=%u", phone.c_str(),
             static_cast<unsigned>(text.size()), static_cast<unsigned>(parts.size()));
    return ESP_OK;
}

esp_err_t idf_sms_enqueue_outgoing(const std::string& phone_raw, const std::string& text_raw, std::string& message)
{
    std::string phone = idf_util_trim_copy(phone_raw);
    std::string text = idf_util_trim_copy(text_raw);
    message.clear();
    if (phone.empty()) {
        message = "错误：请输入目标号码";
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_valid_phone_number(phone)) {
        message = "错误：目标号码非法（3-20 位数字，可带 + 前缀）";
        return ESP_ERR_INVALID_ARG;
    }
    if (text.empty()) {
        message = "错误：请输入短信内容";
        return ESP_ERR_INVALID_ARG;
    }
    if (text.size() > 300) {
        message = "错误：短信内容超过 300 字符";
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_out_mutex || xSemaphoreTake(s_out_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        message = "发送队列繁忙，请稍后再试";
        return ESP_ERR_TIMEOUT;
    }
    if (s_out_count >= OUT_SMS_QUEUE_MAX) {
        xSemaphoreGive(s_out_mutex);
        message = "发送队列已满，请稍后再试";
        return ESP_ERR_NO_MEM;
    }

    size_t tail = (s_out_head + s_out_count) % OUT_SMS_QUEUE_MAX;
    s_out_queue[tail].phone = phone;
    s_out_queue[tail].text = text;
    ++s_out_count;
    int depth = outgoing_depth_locked();
    xSemaphoreGive(s_out_mutex);

    idf_modem_signal_event();  // 唤醒短信任务立即出队发送，不等轮询周期
    idf_logf("网页短信已入队，当前待发=%d", depth);
    message = "已加入发送队列，请稍后在已发送列表查看结果";
    return ESP_OK;
}

int idf_sms_outgoing_queue_depth(void)
{
    if (!s_out_mutex || xSemaphoreTake(s_out_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    int n = outgoing_depth_locked();
    xSemaphoreGive(s_out_mutex);
    return n;
}

IdfSmsStatus idf_sms_get_status(void)
{
    IdfSmsStatus copy;
    if (!s_status_mutex) return copy;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        copy = s_status;
        xSemaphoreGive(s_status_mutex);
    }
    return copy;
}

bool idf_sms_reply_check_begin(const std::string& sender_pattern, const std::string& body_pattern)
{
    if (!s_status_mutex) return false;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    bool ok = s_reply_check.begin(idf_config_translate_perl_classes(sender_pattern),
                                  idf_config_translate_perl_classes(body_pattern));
    xSemaphoreGive(s_status_mutex);
    return ok;
}

void idf_sms_reply_check_arm(int timeout_seconds)
{
    if (!s_status_mutex) return;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_reply_check.arm(esp_timer_get_time(), timeout_seconds);
    xSemaphoreGive(s_status_mutex);
}

int idf_sms_reply_check_poll(void)
{
    if (!s_status_mutex) return -1;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    auto state = s_reply_check.poll(esp_timer_get_time());
    xSemaphoreGive(s_status_mutex);
    return state == IdfSmsReplyCheck::State::Matched ? 1 :
           state == IdfSmsReplyCheck::State::Waiting ? 0 : -1;
}

void idf_sms_reply_check_cancel(void)
{
    if (!s_status_mutex) return;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_reply_check.cancel();
    xSemaphoreGive(s_status_mutex);
}
