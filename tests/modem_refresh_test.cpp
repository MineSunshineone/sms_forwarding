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

using esp_err_t = int;
using TickType_t = uint32_t;
constexpr int ESP_OK = 0, ESP_ERR_TIMEOUT = 1, ESP_FAIL = 2;
#define pdMS_TO_TICKS(ms) (ms)
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
esp_err_t idf_modem_send_at(const char* command, uint32_t timeout, std::string& response)
{
    assert(reply_index < replies.size());
    const Reply& reply = replies[reply_index++];
    assert(command == reply.command);
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
int64_t esp_timer_get_time() { return 100000000; }
IdfModemStatus idf_modem_get_status() { return status; }
IdfSimSettingsView idf_config_get_sim_settings_view() { return {}; }
bool startup_info_complete() { return false; }
bool startup_sampling_done() { return true; }
bool at_channel_idle_now() { return idle; }
void sample_cell_ip_once() {}
void sample_signal_once() {}
void sample_signal_detail_once() {}
void set_phase(const char*) {}
bool sample_identity_once(bool log_summary, bool network)
{
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
    assert(parse_iccid_response("+MCCID: 898602F61324F5013007\r\nOK").empty());
    assert(parse_iccid_response(iccid + "\r\nOK") == iccid);
    assert(parse_iccid_crsm_response("+CRSM: 144,0,\"986800214365870921FF\"").empty());
    assert(parse_iccid_crsm_response("+CRSM: 145,27,\"986800214365870921F3\"") == iccid);
    assert(parse_iccid_crsm_response("+CRSM: 145,256,\"986800214365870921F3\"").empty());
    assert(parse_iccid_crsm_response("+CRSM: 144,1,\"986800214365870921F3\"").empty());
    assert(parse_iccid_crsm_response("+CRSM: 144,0\r\n+OTHER: \"986800214365870921F3\"").empty());
}

void test_slow_operator()
{
    replies = {{"AT+COPS=3,0", 2000, ESP_OK, "OK"},
               {"AT+COPS?", 3200, ESP_OK, "+CEREG: 1\r\n+COPS: 0,0,\"CMCC\",7\r\nOK"}};
    reply_index = 0;
    assert(query_operator_from_modem() == "CMCC");
    replies.back().body = "+COPS: 0\r\nOK";
    reply_index = 0;
    assert(query_operator_from_modem().empty());
    assert(logs.back().find("mode=0 format=-1") != std::string::npos);
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
