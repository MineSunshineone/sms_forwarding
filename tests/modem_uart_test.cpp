#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
using esp_err_t = int;
using TickType_t = uint32_t;
constexpr int ESP_OK = 0, ESP_FAIL = 1, ESP_ERR_TIMEOUT = 2, ESP_ERR_INVALID_STATE = 3;
constexpr int pdTRUE = 1, MODEM_UART = 1;
#define pdMS_TO_TICKS(ms) (ms)
bool s_started = true;
int s_at_mutex = 1, locks = 0;
uint32_t now = 0, submit_delay = 0;
struct Chunk { uint32_t due; std::string text; };
std::deque<Chunk> chunks;
std::vector<std::string> writes;
std::string preserved;
bool omit_ok = false, trailing_error = false;
int submits = 0;
TickType_t xTaskGetTickCount() { return now; }
int xSemaphoreTakeRecursive(int, uint32_t) { ++locks; return pdTRUE; }
void xSemaphoreGiveRecursive(int) { --locks; }
std::string idf_util_trim_copy(const std::string& text)
{
    size_t begin = text.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? std::string() : text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
void preserve_uart_urcs(const uint8_t* data, size_t len) { preserved.append(reinterpret_cast<const char*>(data), len); }
void append_capped(std::string& out, const uint8_t* data, size_t len, size_t cap)
{
    out.append(reinterpret_cast<const char*>(data), len);
    if (out.size() > cap) out.erase(0, out.size() - cap);
}
int uart_get_buffered_data_len(int, size_t* size)
{
    *size = chunks.empty() || chunks.front().due > now ? 0 : chunks.front().text.size();
    return ESP_OK;
}
int uart_read_bytes(int, uint8_t* out, size_t cap, uint32_t wait)
{
    assert(locks > 0);
    if (chunks.empty() || chunks.front().due > now + wait) { now += wait; return 0; }
    now = std::max(now, chunks.front().due);
    size_t len = std::min(cap, chunks.front().text.size());
    memcpy(out, chunks.front().text.data(), len);
    chunks.front().text.erase(0, len);
    if (chunks.front().text.empty()) chunks.pop_front();
    return static_cast<int>(len);
}
int uart_write_bytes(int, const void* data, size_t len)
{
    assert(locks > 0);
    std::string text(static_cast<const char*>(data), len);
    writes.push_back(text);
    if (text == "AT+CMGS=1\r\n") chunks.push_back({now + 10, "\r\n> "});
    if (text == std::string(1, '\x1a')) {
        chunks.push_back({now + submit_delay + 20, "\r\n+CM"});
        chunks.push_back({now + submit_delay + 30, "GS: 12\r\n"});
        chunks.push_back({now + submit_delay + 90, "\r\n+CMTI: \"ME\",1\r\n"});
        if (trailing_error) chunks.push_back({now + submit_delay + 200, "\r\nERROR\r\n"});
        else if (!omit_ok) {
            chunks.push_back({now + submit_delay + 200, "\r\nO"});
            chunks.push_back({now + submit_delay + 210, "K\r\n"});
        }
    }
    if (text == "AT+MCCID\r\n") {
        chunks.push_back({now + 300, "\r\n+MCC"});
        chunks.push_back({now + 310, "ID: 8986001234567890123\r\n"});
        chunks.push_back({now + 320, "\r\nO"});
        chunks.push_back({now + 330, "K\r\n"});
    }
    return static_cast<int>(len);
}
// @收发函数@
void on_submit() { ++submits; }
int main()
{
    std::string response;
    assert(idf_modem_send_pdu("AT+CMGS=1", "00", 60000, response, on_submit) == ESP_OK);
    assert(submits == 1 && locks == 0 && chunks.empty());
    assert(response.find("OK") != std::string::npos);
    assert(preserved.find("+CMTI:") != std::string::npos);
    assert(idf_modem_send_at("AT+MCCID", 5000, response) == ESP_OK);
    assert(response.find("+MCCID: 8986001234567890123") != std::string::npos);
    // 省略 OK 不能重发已确认的短信，也不能占住通道达一分钟。
    omit_ok = true;
    uint32_t start = now;
    assert(idf_modem_send_pdu("AT+CMGS=1", "00", 60000, response, on_submit) == ESP_OK);
    assert(now - start >= 1000 && now - start < 1500 && submits == 2 && locks == 0);
    assert(idf_modem_send_at("AT+MCCID", 5000, response) == ESP_OK);
    assert(response.find("+MCCID:") != std::string::npos);
    // 已有 +CMGS 确认后，即使出现异常尾随 ERROR 也不能把短信标记为可重发。
    trailing_error = true;
    assert(idf_modem_send_pdu("AT+CMGS=1", "00", 60000, response, on_submit) == ESP_OK);
    assert(submits == 3 && locks == 0 && chunks.empty());
    // 即使 +CMGS 紧贴原始截止时间，最终 OK 的收尾窗口仍完整保留。
    trailing_error = false;
    omit_ok = false;
    submit_delay = 3970;
    assert(idf_modem_send_pdu("AT+CMGS=1", "00", 4000, response, on_submit) == ESP_OK);
    assert(submits == 4 && locks == 0 && chunks.empty());
    assert(response.find("OK") != std::string::npos);
    assert(idf_modem_send_at("AT+MCCID", 5000, response) == ESP_OK);
    assert(response.find("+MCCID:") != std::string::npos);
    // 失败响应与无响应仍区分，且每条命令后释放互斥锁。
    chunks.push_back({now + 10, "\r\n+CME ERROR: 10\r\n"});
    assert(idf_modem_send_at("AT+BAD", 500, response) == ESP_FAIL && locks == 0);
    assert(idf_modem_send_at("AT+SILENT", 500, response) == ESP_ERR_TIMEOUT && locks == 0);
    std::cout << "modem UART regression tests passed\n";
}
