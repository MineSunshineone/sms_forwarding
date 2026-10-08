#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=1, ESP_ERR_INVALID_STATE=2, ESP_ERR_INVALID_ARG=3, ESP_ERR_TIMEOUT=4;
constexpr int MODEM_UART=1, s_at_mutex=1, pdTRUE=1;
constexpr uint32_t CELLULAR_HTTP_TIMEOUT_MS=90000, CELLULAR_PDP_READY_TIMEOUT_MS=12000;
const char* IDF_KEEPALIVE_DEFAULT_URL="http://gg.incrafttime.top/api/payload?size=64342";
#define pdMS_TO_TICKS(x) (x)
bool s_started=true, pdp_ready=true, fetch_ok=true;
int lock_depth=0, close_failures=0, fetch_count=0;
uint32_t now=0;
std::string uart_data, status_ip, download, request_reply;
std::vector<std::string> commands, sms;
struct TickDeadline {
    uint32_t end;
    explicit TickDeadline(uint32_t ms):end(now+ms) {}
    bool expired() const { return now>=end; }
};
int64_t esp_timer_get_time() { return now*1000ULL; }
unsigned esp_random() { return 1; }
void idf_log_line(const char*) {}
void idf_logf(const char*, ...) {}
std::string idf_util_trim_copy(const std::string& s) {
    auto p=s.find_first_not_of(" \t\r\n");
    return p==std::string::npos ? "" : s.substr(p,s.find_last_not_of(" \t\r\n")-p+1);
}
int uart_read_bytes(int, uint8_t* buf, size_t size, unsigned delay) {
    now+=delay;
    size_t n=std::min(size, uart_data.size());
    memcpy(buf,uart_data.data(),n); uart_data.erase(0,n); return static_cast<int>(n);
}
std::string s_uart_line_carry;
int64_t s_uart_line_started_us=0, s_uart_wait_cmt_until_us=0;
bool s_uart_wait_cmt_pdu=false;
void append_urc_text(const std::string& s, int64_t) { sms.push_back(s); }
// @短信接收函数@
void set_status_cell_ip(const std::string& s) { status_ip=s; }
int send_at_locked(const std::string& cmd, uint32_t, std::string& resp, size_t =1400, uint32_t extra=50) {
    commands.push_back(cmd); resp="OK\r\n";
    if(cmd.rfind("AT+MHTTPCREATE=",0)==0) resp="+MHTTPCREATE: 0\r\nOK\r\n";
    if(cmd.rfind("AT+MHTTPREQUEST=",0)==0) { assert(extra==0); resp=request_reply; }
    if(cmd=="AT+CGACT=0,1" && close_failures>0) { --close_failures; return ESP_FAIL; }
    return ESP_OK;
}
int xSemaphoreTakeRecursive(int, uint32_t) { ++lock_depth; return pdTRUE; }
void xSemaphoreGiveRecursive(int) { --lock_depth; }
bool wait_pdp_ready_locked(uint32_t, std::string& ip) { ip="10.2.3.4"; status_ip=ip; return pdp_ready; }
// @结果结构@
// @解析函数@
bool use_real_fetch=false;
static bool real_fetch_mhttp_once_locked(const std::string&, const std::string&, const std::string&, const char*, const char*, const std::string&, uint32_t, IdfCellularHttpResult&);
bool fetch_mhttp_once_locked(const std::string& protocol, const std::string& host, const std::string& path, const char* method, const char* type, const std::string& body, uint32_t minimum, IdfCellularHttpResult& r) {
    ++fetch_count;
    if(use_real_fetch) return real_fetch_mhttp_once_locked(protocol,host,path,method,type,body,minimum,r);
    r.ok=fetch_ok;
    if(!fetch_ok) { r.mhttpError=1; r.message="DNS failure"; }
    return fetch_ok;
}
bool send_at_data_locked(const std::string&, const std::string&, std::string&) { return true; }
// @实际HTTP函数@
// @生命周期函数@
void reset() { now=0; s_uart_line_carry.clear(); s_uart_wait_cmt_pdu=false; commands.clear(); sms.clear(); uart_data.clear(); status_ip="10.2.3.4"; close_failures=0; fetch_count=0; fetch_ok=true; pdp_ready=true; assert(lock_depth==0); }
int main() {
    IdfCellularHttpResult r;
    // 请求 OK 同包的 DNS 错误必须保留，不能误报超时。
    reset();
    assert(!wait_mhttp_download_locked(0,1000,48*1024,r,"OK\r\n+MHTTPURC: \"err\",0,1\r\n"));
    assert(r.mhttpError==1 && r.message.find("域名解析失败")!=std::string::npos && now==0);
    // 任意分包边界，含首包中部分 HTTP 头。
    const std::string response="OK\r\n+MHTTPURC: \"header\",0,200,0,\r\n+MHTTPURC: \"content\",0,64342,64342,0,\r\n";
    for(size_t split=0; split<=response.size(); ++split) {
        reset(); r={}; uart_data=response.substr(split);
        assert(wait_mhttp_download_locked(0,1000,48*1024,r,response.substr(0,split)));
        assert(r.bytesRead==64342 && r.httpStatus==200);
    }
    // 首包和后续 UART 任意分界的短信头/PDU，只入队一次。
    const std::string sms_head="+CMT: ,23\r\n", pdu="00112233445566778899AABBCCDDEEFF00\r\n";
    const std::string sms_response=sms_head+pdu+response;
    for(size_t split=0; split<=sms_head.size()+pdu.size(); ++split) {
        reset(); r={}; std::string initial=sms_response.substr(0,split);
        preserve_uart_urcs(reinterpret_cast<const uint8_t*>(initial.data()),initial.size());
        uart_data=sms_response.substr(split);
        assert(wait_mhttp_download_locked(0,1000,48*1024,r,initial));
        assert(sms.size()==2 && sms[0]==sms_head && sms[1]==pdu);
    }
    // 其它连接错误不污染本连接；短 payload 和超时提供可操作的原因。
    reset(); r={}; uart_data="+MHTTPURC: \"err\",3,1\r\n"+response;
    assert(wait_mhttp_download_locked(0,1000,48*1024,r,""));
    reset(); r={};
    assert(!wait_mhttp_download_locked(0,1000,48*1024,r,"+MHTTPURC: \"header\",0,200,0,\r\n+MHTTPURC: \"content\",0,258,258,0,\r\n"));
    assert(r.message.find("下载量不足")!=std::string::npos);
    reset(); r={}; assert(!wait_mhttp_download_locked(0,240,48*1024,r,""));
    assert(r.message.find("超时")!=std::string::npos);
    // 实际请求函数必须把同步响应中的 HTTP URC 交给下载器。
    reset(); r={}; request_reply="OK\r\n+MHTTPURC: \"err\",0,1\r\n";
    assert(!real_fetch_mhttp_once_locked("http","example.com","/","GET",nullptr,"",48*1024,r));
    assert(r.mhttpError==1 && commands.back()=="AT+MHTTPDEL=0");
    reset(); r={}; request_reply=response;
    assert(real_fetch_mhttp_once_locked("https","example.com","/","GET",nullptr,"",48*1024,r));
    assert(r.bytesRead==64342 && commands.back()=="AT+MHTTPDEL=0");
    // 数据原来关闭：成功、DNS 失败和 PDP 失败均应尝试关闭。
    IdfCellularHttpConfig cfg;
    // 从运行入口到真实 AT 请求：只补缓存参数，不重写显式 payload 大小或查询顺序。
    struct UrlCase { const char* url; const char* origin; const char* path; };
    const UrlCase cases[] = {
        {"http://gg.incrafttime.top/api/payload?size=128684", "https://gg.incrafttime.top", "/api/payload?size=128684"},
        {"https://gg.incrafttime.top/api/payload?size=64342", "https://gg.incrafttime.top", "/api/payload?size=64342"},
        {"http://gg.incrafttime.top/api/payload?size=1286840", "https://gg.incrafttime.top", "/api/payload?size=1286840"},
        {"http://gg.incrafttime.top/api/payload?x=a%2Fb&size=100000&x=z", "https://gg.incrafttime.top", "/api/payload?x=a%2Fb&size=100000&x=z"},
        {"http://gg.incrafttime.top/custom?size=128684", "http://gg.incrafttime.top", "/custom?size=128684"},
        {"http://example.com/api/payload?size=128684", "http://example.com", "/api/payload?size=128684"},
        {"http://example.com/gg.incrafttime.top/api/payload?size=128684", "http://example.com", "/gg.incrafttime.top/api/payload?size=128684"},
        {"http://gg.incrafttime.top.evil.test/api/payload?size=128684", "http://gg.incrafttime.top.evil.test", "/api/payload?size=128684"},
        {"http://gg.incrafttime.top/", "https://gg.incrafttime.top", "/api/payload?size=64342"},
        {"", "https://gg.incrafttime.top", "/api/payload?size=64342"},
    };
    for(const auto& item: cases) {
        reset(); use_real_fetch=true; request_reply=response;
        assert(cellular_http_request_impl(item.url,"GET",nullptr,"",cfg,48*1024,true,r)==ESP_OK);
        const std::string create="AT+MHTTPCREATE=\""+std::string(item.origin)+"\"";
        std::string expected_path=item.path;
        append_no_cache_query(expected_path);
        const std::string request="AT+MHTTPREQUEST=0,1,0,"+hex_encode_ascii(expected_path);
        assert(std::find(commands.begin(),commands.end(),create)!=commands.end());
        assert(std::find(commands.begin(),commands.end(),request)!=commands.end());
        assert(r.bytesRead==64342 && commands.back()=="AT+CGACT=0,1");
        use_real_fetch=false;
    }
    for(int scenario=0; scenario<3; ++scenario) {
        reset(); r={}; fetch_ok=scenario!=1; pdp_ready=scenario!=2;
        int ret=cellular_http_request_impl("http://example.com/payload","GET",nullptr,"",cfg,48*1024,true,r);
        assert(ret==(scenario==0?ESP_OK:ESP_FAIL));
        assert(commands.front()=="AT+CGACT=1,1" && commands.back()=="AT+CGACT=0,1");
        assert(status_ip.empty() && lock_depth==0);
    }
    // 关闭失败不能谎报成功，也不能抹掉仍可能有效的 IP；允许一次有限重试。
    reset(); close_failures=2;
    assert(cellular_http_request_impl("http://example.com/payload","GET",nullptr,"",cfg,48*1024,true,r)==ESP_FAIL);
    assert(!r.ok && !status_ip.empty() && r.message.find("数据可能仍开启")!=std::string::npos);
    assert(commands.size()==3 && lock_depth==0);
    reset(); close_failures=1;
    assert(cellular_http_request_impl("http://example.com/payload","GET",nullptr,"",cfg,48*1024,true,r)==ESP_OK);
    assert(status_ip.empty() && commands.size()==3);
    // 数据原来开启：不关闭；普通推送不能越过数据关闭设置。
    reset(); cfg.dataEnabled=true;
    assert(cellular_http_request_impl("http://example.com/payload","GET",nullptr,"",cfg,48*1024,true,r)==ESP_OK);
    assert(commands.size()==1);
    reset(); cfg.dataEnabled=false;
    assert(cellular_http_request_impl("http://example.com/payload","GET",nullptr,"",cfg,0,false,r)==ESP_ERR_INVALID_STATE);
    assert(commands.empty() && fetch_count==0);
    std::cout << "cellular HTTP regressions passed\n";
}
