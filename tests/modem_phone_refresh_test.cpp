#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "idf_iccid.h"

// @状态结构@
IdfModemStatus s_status;
bool s_identity_static_attempted = false, s_identity_network_attempted = false;
void* s_status_mutex = reinterpret_cast<void*>(1);
constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(...) ((void)0)
int xSemaphoreTake(void*, int) { return pdTRUE; }
void xSemaphoreGive(void*) {}
void vTaskDelay(int) {}
IdfModemStatus idf_modem_get_status() { return s_status; }
void save_identity_cache(const std::string&, const std::string&) {}
std::string last_log;
void idf_logf(const char* fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    last_log = text;
}
struct Reply { std::string command, body; bool ok = true; };
std::vector<Reply> replies;
size_t next_reply = 0;
bool send_ok(const std::string& command, uint32_t timeout, std::string* response) {
    assert(timeout <= 1500);
    assert(next_reply < replies.size());
    const Reply& reply = replies[next_reply++];
    assert(command == reply.command);
    *response = reply.body;
    return reply.ok;
}
std::string query_operator_from_modem() { return "CHN-UNICOM"; }
std::string query_imei_from_modem() { assert(false); return {}; }
std::string query_current_iccid() { assert(false); return {}; }
std::string first_payload_line(const std::string&, const char*) { assert(false); return {}; }
std::string first_digits_line(const std::string&, size_t, size_t) { assert(false); return {}; }
std::string first_digit_run(const std::string&, size_t, size_t) { assert(false); return {}; }
// @真实函数@
int refresh_requests = 0;
void idf_modem_request_status_sample() { ++refresh_requests; }
std::atomic<void (*)(void)> s_sim_identity_hook{nullptr};
// @换卡函数@

void prepare() {
    s_status = {};
    s_status.mfr = "manufacturer";
    s_status.model = "model";
    s_status.fwver = "firmware";
    s_status.imei = "123456789012345";
    s_status.iccid = "8986001234567890123";
    s_status.imsi = "460001234567890";
    s_status.apnSim = "apn";
    s_status.signalFresh = true;
    reset_identity_sampling_state();
    replies.clear();
    next_reply = 0;
}
void expect_cnum(const std::string& response, bool ok = true) {
    replies = {{"AT+CNUM", response, ok}};
    next_reply = 0;
}
int main() {
    // 启动读到运营商而 CNUM 仅返回 OK，不伪造号码，也不阻塞 ready。
    prepare();
    expect_cnum("AT+CNUM\r\nOK\r\n");
    assert(sample_identity_once());
    assert(next_reply == 1 && s_status.phone.empty());
    assert(s_status.operatorName == "CHN-UNICOM" && startup_info_complete());
    // 普通后台补采/网页轮询不会因缺号码连续发 CNUM。
    for (int i = 0; i < 50; ++i) assert(!sample_identity_once());
    assert(next_reply == 1);
    // 显式刷新一次，无号码或命令失败都保留为空，并给出手动设置提示。
    for (bool ok : {true, false}) {
        expect_cnum("OK", ok);
        assert(!sample_identity_once(true, true, true));
        assert(next_reply == 1 && s_status.phone.empty());
        assert(last_log.find("可手动设置") != std::string::npos);
        for (int i = 0; i < 50; ++i) assert(!sample_identity_once());
    }
    // 日志开关本身不能触发重试；SIM 未就绪不能发 CNUM。
    assert(!sample_identity_once(true, true, false));
    assert(!sample_identity_once(true, false, true));
    assert(next_reply == 1);
    // 后续显式刷新可以读到号码，即使运营商早已读取成功。
    const std::string number = "+8613800138000";
    expect_cnum("+CNUM: \"\",\"" + number + "\",145\r\nOK");
    assert(sample_identity_once(true, true, true));
    assert(next_reply == 1 && s_status.phone == number);
    assert(last_log.find(number) == std::string::npos);
    // 非空缓存不反复查询，也不被空结果覆盖。
    for (int i = 0; i < 50; ++i) assert(!sample_identity_once(true, true, true));
    assert(next_reply == 1 && s_status.phone == number);
    // 真实换卡入口清除旧号码与网络身份，并排队刷新；配置中的手动号码不参与此状态。
    idf_modem_invalidate_sim_identity();
    assert(s_status.phone.empty() && s_status.iccid.empty() && s_status.imsi.empty());
    assert(s_status.operatorName.empty() && !s_identity_network_attempted);
    assert(refresh_requests == 1);
    s_status.iccid = "8986001234567890124";
    s_status.imsi = "460001234567891";
    s_status.apnSim = "apn";
    expect_cnum("OK");
    assert(sample_identity_once(true, true, true));
    assert(s_status.phone.empty() && next_reply == 1);
    std::cout << "phone refresh production-function tests passed\n";
}
