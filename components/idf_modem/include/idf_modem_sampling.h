#pragma once

#include <atomic>
#include <stdint.h>

// 用请求代次确认完成：AT 忙时保留请求，采样期间新到的请求也不能被旧轮次清掉。
class IdfModemSampling {
public:
    uint32_t request() { return requested_.fetch_add(1) + 1; }
    bool pending() const { return requested() != completed(); }
    uint32_t requested() const { return requested_.load(); }
    uint32_t completed() const { return completed_.load(); }
    bool running() const { return running_.load(); }
    uint32_t begin()
    {
        const uint32_t request = requested();
        running_.store(true);
        return request;
    }
    void finish(uint32_t request)
    {
        completed_.store(request);
        running_.store(false);
    }

private:
    std::atomic<uint32_t> requested_{0};
    std::atomic<uint32_t> completed_{0};
    std::atomic<bool> running_{false};
};

// 自动采样与网页 AT 调试使用相同的读超时，避免慢卡只能手动读出身份。
static constexpr uint32_t IDF_MODEM_IDENTITY_TIMEOUT_MS = 5000;
