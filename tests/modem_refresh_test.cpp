#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include "idf_modem_sampling.h"
#include "idf_iccid.h"

using esp_err_t = int;
using TickType_t = uint32_t;
constexpr int ESP_OK = 0, ESP_ERR_TIMEOUT = 1, ESP_FAIL = 2, ESP_ERR_INVALID_STATE = 3;
#define pdMS_TO_TICKS(ms) (ms)
int lock_depth = 0;
bool lock_available = true, s_started = true;
void* s_at_mutex = reinterpret_cast<void*>(1);
constexpr int pdTRUE = 1;
int64_t fake_time = 100000000;
uint32_t transport_overhead_ms = 0;
int64_t esp_timer_get_time() { return fake_time; }
int xSemaphoreTakeRecursive(void*, uint32_t timeout) {
    assert(timeout == 100);
    fake_time += 1000 * timeout;
    if (!lock_available) return 0;
    ++lock_depth; return pdTRUE;
}
void xSemaphoreGiveRecursive(void*) { assert(lock_depth == 1); --lock_depth; }
struct Reply { std::string command; uint32_t latency; int error; std::string body; };
std::vector<Reply> replies;
std::vector<std::string> logs;
size_t reply_index = 0;
std::string idf_util_trim_copy(const std::string& text)
{
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
const char* esp_err_to_name(int error) { return error == ESP_ERR_TIMEOUT ? "TIMEOUT" : "ERROR"; }
void idf_logf(const char* format, ...)
{
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    logs.emplace_back(buf);
}
void idf_log_line(const char* text) { logs.emplace_back(text); }
esp_err_t idf_modem_send_at(const std::string& command, uint32_t timeout, std::string& response)
{
    if (command.find("AT+COPS") == 0) assert(lock_depth == 1);
    assert(reply_index < replies.size());
    const Reply& reply = replies[reply_index++];
    assert(command == reply.command);
    fake_time += 1000 * (std::min(reply.latency, timeout) + transport_overhead_ms);
    response = reply.latency <= timeout ? reply.body : std::string();
    return reply.latency <= timeout ? reply.error : ESP_ERR_TIMEOUT;
}
// @查询函数@

struct IdfModemStatus { bool atReady = false; std::string cellIp; };
struct IdfSimSettingsView { bool dataEnabled = false; };
IdfModemSampling s_status_sampling;
std::atomic<int64_t> s_last_web_poll_us{-15000000};
constexpr int64_t WEB_POLL_ACTIVE_WINDOW_US = 15000000;
constexpr uint32_t MODEM_DATA_MODE_RETRY_GAP_MS = 10000;
constexpr uint32_t SIGNAL_INTERVAL_WEB_MS = 10000;
constexpr uint32_t SIGNAL_DETAIL_INTERVAL_WEB_MS = 30000;
constexpr uint8_t MODEM_RETRY_DELAY_COUNT = 5;
uint32_t modem_retry_delay_ms(uint8_t) { return 30000; }
IdfModemStatus status;
bool idle = true, inject_request = false, last_log_summary = false, last_network = false;
int sampled = 0;
IdfModemStatus idf_modem_get_status() { return status; }
IdfSimSettingsView idf_config_get_sim_settings_view() { return {}; }
bool startup_info_complete() { return false; }
bool startup_sampling_done() { return true; }
bool at_channel_idle_now() { return idle; }
void sample_cell_ip_once() {}
void sample_signal_once() {}
void sample_signal_detail_once() {}
void set_phase(const char*) {}
bool sample_identity_once(bool log_summary, bool network, bool refresh_missing_phone)
{
    assert(refresh_missing_phone == log_summary);
    ++sampled;
    last_log_summary = log_summary;
    last_network = network;
    if (inject_request) { inject_request = false; s_status_sampling.request(); }
    return true;
}
void run_sampling(bool sim_ready)
{
    bool registered = sim_ready, post_register_done = true;
    uint32_t now = 100000, last_identity = 0, last_cell_ip = 0, last_signal = 0, last_detail = 0;
    uint8_t identity_retry_level = 0;
    // @采样代码@
}

void test_slow_iccid_and_fallbacks()
{
    assert(is_iccid_text("898600D6991330004146"));
    const std::string hex_iccid = "898600D6991330004146";
    assert(parse_iccid_response("+MCCID: 898600d6991330004146\r\nOK") == hex_iccid);
    assert(parse_iccid_crsm_response("+CRSM: 144,0,\"9868006d993103001464\"") == hex_iccid);
    for (const std::string& good : {hex_iccid, std::string("898600AF991330004146"),
                                   std::string("8986001234567890123"), std::string("898600123456789012F")}) {
        std::string padded = good.size() == 19 ? good + "F" : good;
        std::string encoded = padded;
        for (size_t i = 0; i < encoded.size(); i += 2) std::swap(encoded[i], encoded[i + 1]);
        assert(parse_iccid_response("+ICCID: \"" + good + "\"\r\nOK") == good);
        assert(parse_iccid_response("+CCID: " + padded + "\r\nOK") == good);
        assert(parse_iccid_crsm_response("+CRSM: 144,0,\"" + encoded + "\"") == good);
        assert(idf_normalize_iccid(idf_normalize_iccid(padded)) == good);
    }
    for (const std::string bad : {"89860D66991330004146", "988600D6991330004146",
                                  "898600G6991330004146", "89860012345678901",
                                  "898600123456789012345678", "898600D6991330004146!"}) {
        assert(parse_iccid_response("+MCCID: " + bad + "\r\nOK").empty());
        assert(idf_normalize_iccid(bad).empty());
    }
    assert(parse_iccid_crsm_response("+CRSM: 144,0,\"9868006G993103004164\"").empty());
    assert(parse_iccid_crsm_response("+CRSM: 144,0,\"9868D06D993103004164\"").empty());
    for (const std::string malformed : {
            "+CRSM: 144,0x,\"9868006D993103001464\"",
            "+CRSM: 144,0 \"9868006D993103001464\"",
            "+CRSM: 144,0,\"9868006D993103001464\",extra",
            "+CRSM: 144,0,\"9868006D993103001464!\""})
        assert(parse_iccid_crsm_response(malformed).empty());
    const std::string iccid = "8986001234567890123";
    replies = {{"AT+MCCID", 2400, ESP_OK, "\r\n+MCCID: " + iccid + "\r\n\r\nOK\r\n"}};
    assert(query_current_iccid() == iccid);
    assert(reply_index == 1);
    assert(logs.back().find(iccid) == std::string::npos);
    replies = {{"AT+MCCID", 20, ESP_FAIL, "ERROR"},
               {"AT+ICCID", 3100, ESP_OK, "+ICCID: \"" + iccid + "F\"\r\nOK"}};
    reply_index = 0;
    assert(query_current_iccid() == iccid);
    assert(reply_index == 2);
    replies = {{"AT+MCCID", 5100, ESP_OK, ""}, {"AT+ICCID", 10, ESP_FAIL, "ERROR"},
               {"AT+CCID", 10, ESP_FAIL, "ERROR"},
               {"AT+CRSM=176,12258,0,0,10", 2300, ESP_OK, "+CRSM: 144,0,\"986800214365870921F3\"\r\nOK"}};
    reply_index = 0;
    assert(query_current_iccid() == iccid);
    assert(reply_index == 4);
    replies.back().body = "+CRSM: 98,4,\"986800214365870921F3\"\r\nOK";
    reply_index = 0;
    assert(query_current_iccid().empty());
    assert(logs.back().find("sw1=98 sw2=4") != std::string::npos);
    assert(parse_iccid_response("+MCCID: 0,\"" + iccid + "\",1\r\nOK") == iccid);
    assert(parse_iccid_response("AT+MCCID\r\n+CEREG: 1\r\n+MCCID: " + iccid + "\r\nOK") == iccid);
    assert(parse_iccid_response("+CMT: ,40\r\n0011223344556677889A\r\n+MCCID: invalid\r\nOK").empty());
    assert(parse_iccid_response("+CMT: ,10\r\n" + iccid + "\r\nOK").empty());
    assert(parse_iccid_response("+MCCID: X" + iccid + "Y\r\nOK").empty());
    assert(parse_iccid_response("+MCCID: 898602F61324F5013007\r\nOK") == "898602F61324F5013007");
    assert(parse_iccid_response(iccid + "\r\nOK") == iccid);
    assert(parse_iccid_crsm_response("+CRSM: 144,0,\"986800214365870921FF\"") == "898600123456789012F");
    assert(parse_iccid_crsm_response("+CRSM: 145,27,\"986800214365870921F3\"") == iccid);
    assert(parse_iccid_crsm_response("+CRSM: 145,256,\"986800214365870921F3\"").empty());
    assert(parse_iccid_crsm_response("+CRSM: 144,1,\"986800214365870921F3\"").empty());
    assert(parse_iccid_crsm_response("+CRSM: 144,0\r\n+OTHER: \"986800214365870921F3\"").empty());
}

bool manual_operator(std::string& message)
{
    bool success = false;
    // @手动运营商查询@
    return success;
}

void test_slow_operator()
{
    auto run = [](std::vector<Reply> script, const std::string& expected, bool ok) {
        replies = script; reply_index = 0; logs.clear();
        int64_t started = fake_time;
        std::string name = "stale";
        esp_err_t err = idf_modem_get_operator(name);
        assert((err == ESP_OK) == ok);
        assert(name == expected);
        assert(reply_index == replies.size());
        assert(lock_depth == 0 && fake_time - started <= 20000000);
    };
    for (const std::string body : {"+COPS: 0,0,\"CMCC\",7", "+CEREG: 1\r\n +COPS: 4,1,\"CMCC\",7,0\r\nOK"})
        run({{"AT+COPS?", 3200, ESP_OK, body}}, "CMCC", true);
    for (const std::string plmn : {"46000", "001001"})
        run({{"AT+COPS?", 1, ESP_OK, "+COPS: 1,2,\"" + plmn + "\",7"}}, "PLMN " + plmn, true);
    transport_overhead_ms = 1100;
    for (int original : {0, 1}) {
        std::vector<Reply> script = {
            {"AT+COPS?", 3200, ESP_OK, "+COPS: 0," + std::to_string(original) + ",\"\",7"},
            {"AT+COPS=3,2", 2000, ESP_OK, "OK"},
            {"AT+COPS?", 3200, ESP_OK, "+CEREG: 5\r\n+COPS: 0,2,\"46000\",7"},
            {"AT+COPS=3," + std::to_string(original), 100, ESP_OK, "OK"}};
        run(script, "PLMN 46000", true);
        for (int error : {ESP_FAIL, ESP_ERR_TIMEOUT}) {
            auto changed = script; changed[1].error = error; changed.erase(changed.begin() + 2);
            run(changed, "", false);
            changed = script; changed[2].error = error; run(changed, "", false);
            changed = script; changed[3].error = error; run(changed, "", false);
            assert(logs.back().find("恢复失败") != std::string::npos);
        }
        for (const std::string bad : {"+COPS: 0,2,\"\",7", "+COPS: 0,2,\"4600\",7", "+COPS: 0,2,\"4600000\",7", "+COPS: 0,2,\"46A00\",7", "+COPS: 0,2,\" 46000 \",7", "+COPS: 0,0,\"46000\",7", "+COPS: 0,2,\"46000", "+COPS: 0,2,\"46000\",7,junk"}) {
            auto changed = script; changed[2].body = bad; run(changed, "", false);
        }
        auto slow = script; for (auto& reply : slow) reply.latency = 5000;
        // 工作预算耗尽时不再读数字响应，但仍保留完整恢复预算。
        slow.erase(slow.begin() + 2);
        run(slow, "", false);
        slow = script; slow[1].latency = 6000; slow.erase(slow.begin() + 2);
        run(slow, "", false);
        slow = script; slow[2].latency = 6000; run(slow, "", false);
    }
    for (const std::string bad : {"+COPS: 0", "+COPS: 0,9,\"\",7", "+COPS: 3,0,\"\",7", "+COPS: ,0,\"\",7", "+COPS: 0, ,\"\",7", "junk+COPS: 0,0,\"CMCC\",7", "+OTHER: \"CMCC\"", "+COPS: 0,0,\"CMCC\",7junk", "+COPS: 0,0,\"CMCC\",7\r\n+COPS: 0,0,\"OTHER\",7", "+COPS: 0,2,\"460A0\",7"})
        run({{"AT+COPS?", 1, ESP_OK, bad}}, "", false);
    for (int error : {ESP_FAIL, ESP_ERR_TIMEOUT}) run({{"AT+COPS?", 1, error, ""}}, "", false);
    transport_overhead_ms = 0;
    lock_available = false; run({}, "", false); lock_available = true;
    s_started = false; run({}, "", false); s_started = true;
    s_at_mutex = nullptr; run({}, "", false); s_at_mutex = reinterpret_cast<void*>(1);
    run({{"AT+COPS?", 1, ESP_OK, "+COPS: 0,2,\" 46000 \",7"}}, "", false);
    replies = {{"AT+COPS?", 3200, ESP_OK, "+COPS: 0,0,\"CMCC\",7"}};
    reply_index = 0;
    assert(query_operator_from_modem() == "CMCC");
    reply_index = 0;
    std::string message;
    assert(manual_operator(message) && message == "CMCC");
    replies = {{"AT+COPS?", 1, ESP_OK, "+COPS: 0"}};
    reply_index = 0;
    assert(!manual_operator(message) && message.find("无法读取") != std::string::npos);
    replies = {{"AT+COPS?", 1, ESP_OK, "+COPS: 0,0,\"\",7"},
               {"AT+COPS=3,2", 1, ESP_OK, "OK"},
               {"AT+COPS?", 1, ESP_OK, "+COPS: 0,2,\"\",7"},
               {"AT+COPS=3,0", 1, ESP_OK, "OK"}};
    reply_index = 0;
    assert(!manual_operator(message) && reply_index == 4 && lock_depth == 0);
    replies[2].body = "+COPS: 0,2,\"46000\",7";
    reply_index = 0;
    assert(manual_operator(message) && message == "PLMN 46000" && reply_index == 4 && lock_depth == 0);
}

void test_refresh_queue()
{
    // 未就绪或 AT 被长任务占用时，请求仍在；只恢复 SIM 状态不能丢掉请求。
    uint32_t first = s_status_sampling.request();
    run_sampling(false);
    assert(sampled == 0 && s_status_sampling.pending());
    status.atReady = true;
    idle = false;
    run_sampling(false);
    assert(sampled == 0 && s_status_sampling.completed() != first);
    idle = true;
    run_sampling(false);
    assert(sampled == 1 && !s_status_sampling.pending());
    assert(last_log_summary && !last_network && !s_status_sampling.running());

    // 即使一次采样期间又收到新刷新，旧轮次也只能确认自己开始前的请求。
    uint32_t next = s_status_sampling.request();
    inject_request = true;
    run_sampling(true);
    assert(sampled == 2 && s_status_sampling.completed() == next);
    assert(s_status_sampling.pending() && !s_status_sampling.running());
    run_sampling(true);
    assert(sampled == 3 && !s_status_sampling.pending() && last_network);

    // 多次排队可合并为一轮读取，不在解锁/短信占用期忙等。
    s_status_sampling.request();
    s_status_sampling.request();
    idle = false;
    for (int i = 0; i < 20; ++i) run_sampling(true);
    assert(sampled == 3 && s_status_sampling.pending());
    idle = true;
    run_sampling(true);
    assert(sampled == 4 && !s_status_sampling.pending());
}
int main()
{
    test_slow_iccid_and_fallbacks();
    test_slow_operator();
    logs.clear();
    log_identity_response_shape("MCCID", "+MCCID: 898602F61324F5013007\r\nOK", "+MCCID:");
    assert(logs.back().find("hex=2") != std::string::npos);
    assert(logs.back().find("898602") == std::string::npos);
    log_identity_response_shape("MCCID", "\r\nOK\r\n", "+MCCID:");
    assert(logs.back().find("target=0") != std::string::npos);
    test_refresh_queue();
    std::cout << "modem refresh regression tests passed\n";
}
