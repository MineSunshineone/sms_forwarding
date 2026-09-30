#include "idf_daily_heartbeat.h"

#include <assert.h>
#include <stdio.h>

#include <vector>

static constexpr uint32_t kDay = 20000U;
static constexpr uint32_t kMidnight = kDay * 86400U;

struct Fixture {
    IdfDailyHeartbeat heartbeat;
    bool accepts = true;
    int enqueue_calls = 0;
    std::vector<int64_t> stored_days;
    std::vector<int> operations;

    explicit Fixture(int64_t last_day = -1) : heartbeat(last_day) {}

    bool poll(uint32_t epoch, uint64_t uptime, bool enabled = true, int hour = 8, int tz = 0)
    {
        return heartbeat.poll(enabled, hour, epoch, tz, uptime, [&]() {
            ++enqueue_calls;
            operations.push_back(1);
            return accepts;
        }, [&](int64_t day) {
            operations.push_back(2);
            stored_days.push_back(day);
        });
    }
};

static void test_at_configured_hour()
{
    Fixture f;
    assert(!f.poll(kMidnight + 8U * 3600U - 1U, 0));
    assert(f.enqueue_calls == 0);
    assert(f.poll(kMidnight + 8U * 3600U, 5000));
    assert(f.stored_days == std::vector<int64_t>{kDay});
    assert(f.operations == (std::vector<int>{1, 2}));
    assert(!f.poll(kMidnight + 8U * 3600U + 5U, 10000));
    assert(!f.poll(kMidnight + 23U * 3600U, 54000000));
    assert(f.enqueue_calls == 1);
}

static void test_delayed_time_sync_catches_up()
{
    Fixture f;
    assert(!f.poll(0, 0));
    assert(!f.poll(1699999999U, 3600000));
    assert(f.enqueue_calls == 0);
    // 校时在 09:00 才成功，旧逻辑只匹配 08 点，整天都不会再运行。
    assert(f.poll(kMidnight + 9U * 3600U, 7200000));
    assert(f.stored_days == std::vector<int64_t>{kDay});
}

static void test_late_start_and_multi_day_gap()
{
    Fixture f(kDay - 4);
    assert(f.poll(kMidnight + 18U * 3600U, 0));
    assert(f.enqueue_calls == 1);
    assert(f.stored_days == std::vector<int64_t>{kDay});
    assert(!f.poll(kMidnight + 86400U + 7U * 3600U, 46800000));
    assert(f.poll(kMidnight + 86400U + 8U * 3600U, 50400000));
    assert(f.stored_days == (std::vector<int64_t>{kDay, kDay + 1}));
}

static void test_queue_rejection_retries_without_persisting()
{
    Fixture f;
    f.accepts = false;
    const uint32_t at_eight = kMidnight + 8U * 3600U;
    assert(!f.poll(at_eight, 0));
    assert(f.enqueue_calls == 1);
    assert(f.stored_days.empty());
    assert(!f.poll(at_eight + 5U, 5000));
    assert(!f.poll(at_eight + 59U, 59999));
    assert(f.enqueue_calls == 1);
    assert(!f.poll(at_eight + 60U, 60000));
    assert(f.enqueue_calls == 2);
    assert(f.stored_days.empty());
    // 队列直到 09:00 才腾出空间，仍应入队而不是将当天标记为已完成。
    f.accepts = true;
    assert(f.poll(at_eight + 3600U, 3600000));
    assert(f.enqueue_calls == 3);
    assert(f.operations == (std::vector<int>{1, 1, 1, 2}));
    assert(f.stored_days == std::vector<int64_t>{kDay});
    assert(!f.poll(at_eight + 3660U, 3660000));
}

static void test_restart_and_clock_rollback_do_not_duplicate()
{
    Fixture before_restart;
    assert(before_restart.poll(kMidnight + 8U * 3600U, 0));
    Fixture after_restart(before_restart.stored_days.back());
    assert(!after_restart.poll(kMidnight + 9U * 3600U, 0));
    assert(!after_restart.poll(kMidnight - 3600U, 60000));
    assert(!after_restart.poll(kMidnight + 23U * 3600U, 120000));
    assert(after_restart.enqueue_calls == 0);
    assert(after_restart.poll(kMidnight + 86400U + 8U * 3600U, 180000));
    assert(after_restart.stored_days == std::vector<int64_t>{kDay + 1});
}

static void test_timezone_boundaries()
{
    Fixture china;
    assert(!china.poll(kMidnight - 1U, 0, true, 8, 480));
    assert(china.poll(kMidnight, 5000, true, 8, 480));
    assert(china.stored_days == std::vector<int64_t>{kDay});

    Fixture west;
    assert(west.poll(kMidnight + 7U * 3600U, 0, true, 23, -480));
    assert(west.stored_days == std::vector<int64_t>{kDay - 1});
    assert(!west.poll(kMidnight + 8U * 3600U, 3600000, true, 23, -480));
    assert(west.poll(kMidnight + 86400U + 7U * 3600U, 86400000, true, 23, -480));
    assert(west.stored_days == (std::vector<int64_t>{kDay - 1, kDay}));

    Fixture east;
    assert(east.poll(kMidnight + 10U * 3600U, 0, true, 0, 840));
    assert(east.stored_days == std::vector<int64_t>{kDay + 1});

    Fixture half_hour;
    assert(!half_hour.poll(kMidnight + 2U * 3600U + 29U * 60U, 0, true, 8, 330));
    assert(half_hour.poll(kMidnight + 2U * 3600U + 30U * 60U, 5000, true, 8, 330));
}

static void test_disabled_invalid_hour_and_schedule_edits()
{
    Fixture f;
    const uint32_t at_nine = kMidnight + 9U * 3600U;
    assert(!f.poll(at_nine, 0, false));
    assert(!f.poll(at_nine, 5000, true, -1));
    assert(!f.poll(at_nine, 10000, true, 24));
    assert(!f.poll(at_nine, 15000, true, 10));
    assert(f.enqueue_calls == 0);
    assert(f.poll(at_nine, 20000));
    assert(!f.poll(at_nine, 80000, false));
    assert(!f.poll(at_nine, 140000, true, 9));
    assert(f.enqueue_calls == 1);
}

static void test_retry_across_uptime_32_bit_boundary()
{
    Fixture f;
    f.accepts = false;
    const uint64_t near_wrap = (1ULL << 32) - 10000ULL;
    const uint32_t at_eight = kMidnight + 8U * 3600U;
    assert(!f.poll(at_eight, near_wrap));
    f.accepts = true;
    assert(!f.poll(at_eight + 30U, near_wrap + 30000ULL));
    assert(f.poll(at_eight + 60U, near_wrap + 60000ULL));
    assert(f.enqueue_calls == 2);
}

static void test_week_of_daily_heartbeats()
{
    Fixture f;
    for (uint32_t i = 0; i < 7; ++i) {
        const uint32_t at_eight = kMidnight + i * 86400U + 8U * 3600U;
        const uint64_t uptime = static_cast<uint64_t>(i) * 86400000ULL;
        assert(f.poll(at_eight, uptime));
        assert(!f.poll(at_eight + 3600U, uptime + 3600000ULL));
        assert(f.stored_days.back() == kDay + i);
    }
    assert(f.enqueue_calls == 7);
    assert(f.stored_days.size() == 7);
}

int main()
{
    test_at_configured_hour();
    test_delayed_time_sync_catches_up();
    test_late_start_and_multi_day_gap();
    test_queue_rejection_retries_without_persisting();
    test_restart_and_clock_rollback_do_not_duplicate();
    test_timezone_boundaries();
    test_disabled_invalid_hour_and_schedule_edits();
    test_retry_across_uptime_32_bit_boundary();
    test_week_of_daily_heartbeats();
    puts("每日心跳：9 组主机回归测试通过");
}
