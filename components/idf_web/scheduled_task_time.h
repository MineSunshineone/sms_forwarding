#pragma once
#include <stdint.h>

// 同一个到期时间用于执行和网页倒计时，避免当地时刻与 N×24h 展示不一致。
inline uint64_t idf_scheduled_due_epoch(uint32_t last, int days, int start_minute, int tz_minutes)
{
    if (last < 1700000000u || days <= 0) return 0;
    if (start_minute < 0) return static_cast<uint64_t>(last) + static_cast<uint64_t>(days) * 86400ULL;
    int64_t offset = static_cast<int64_t>(tz_minutes) * 60;
    return ((static_cast<int64_t>(last) + offset) / 86400 + days) * 86400 + start_minute * 60 - offset;
}

// 指定本地时刻按日历日调度，旧任务保留精确间隔时长；NTP 回拨不重复执行。
inline bool idf_scheduled_task_due(uint32_t last, uint32_t now, int days, int start_minute, int tz_minutes)
{
    uint64_t due = idf_scheduled_due_epoch(last, days, start_minute, tz_minutes);
    return due != 0 && now >= last && now >= due;
}

// 固定时刻以开始执行日为基准，等待回复跨午夜也不跳过下次到期日。
inline uint32_t idf_scheduled_success_anchor(int start_minute, uint32_t started, uint32_t finished)
{
    return start_minute >= 0 && started >= 1700000000u ? started : finished;
}
