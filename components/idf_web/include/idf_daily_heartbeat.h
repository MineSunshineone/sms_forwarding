#pragma once

#include <stdint.h>

// 心跳调度只负责入队：网络发送及其重试仍由推送/邮件 worker 负责。
// 独立于 ESP-IDF，便于在主机上验证跨日、校时和队列繁忙等边界。
class IdfDailyHeartbeat {
public:
    explicit IdfDailyHeartbeat(int64_t last_queued_day) : last_queued_day_(last_queued_day) {}

    template <typename Enqueue, typename Persist>
    bool poll(bool enabled, int scheduled_hour, uint32_t now, int tz_offset_min,
              uint64_t uptime_ms, Enqueue enqueue, Persist persist)
    {
        if (!enabled || now < 1700000000U || scheduled_hour < 0 || scheduled_hour > 23) return false;

        const int64_t local = static_cast<int64_t>(now) + static_cast<int64_t>(tz_offset_min) * 60LL;
        const int64_t day = local / 86400LL;
        const int hour = static_cast<int>((local / 3600LL) % 24LL);
        // 到点后当天持续补发，避免晚校时、重启或任务阻塞错过整个小时。
        // 保留严格递增的日期标记，NTP 回拨跨过午夜时也不会重复入队。
        if (hour < scheduled_hour || day <= last_queued_day_) return false;
        if (attempted_ && uptime_ms - last_attempt_ms_ < 60000ULL) return false;

        attempted_ = true;
        last_attempt_ms_ = uptime_ms;
        // 队列满/暂时无可用通道时不消耗今日额度，每分钟重试一次，避免刷屏。
        if (!enqueue()) return false;

        last_queued_day_ = day;
        persist(day);
        return true;
    }

private:
    int64_t last_queued_day_;
    uint64_t last_attempt_ms_ = 0;
    bool attempted_ = false;
};
