#include <cassert>
#include <cstdio>
#include "idf_sms_reply_check.h"
#include "scheduled_task_time.h"

int main()
{
    using State = IdfSmsReplyCheck::State;
    IdfSmsReplyCheck check;
    assert(!check.begin("", ""));
    assert(!check.begin("[", ""));
    assert(!check.begin("^10086$", "["));
    assert(check.poll(0) == State::Idle);
    assert(check.begin("", "余额为.*元"));
    check.submitted(1, 1700000000u);
    check.arm(1000000, 300);
    check.observe("10086", "您的余额为25.00元", 61000000, 61000000);
    assert(check.poll(61000000) == State::Matched);
    assert(check.poll(999000000) == State::Matched);

    // 无匹配/无短信直到边界超时，之后的迟到回复不能把失败改成成功。
    assert(check.begin("^10086$", "余额为.*元"));
    check.submitted(1, 1700000000u);
    check.arm(0, 300);
    check.observe("10010", "无关短信", 299999999, 299999999);
    assert(check.poll(299999999) == State::Waiting);
    check.observe("10086", "余额为10元", 300000000, 300000000);
    assert(check.poll(300000000) == State::TimedOut);
    check.observe("10086", "余额为10元", 301000000, 301000000);
    assert(check.poll(301000000) == State::TimedOut);

    // 发件人或正文任意一个命中即成功；未设置的正则不能匹配所有短信。
    assert(check.begin("^10086$", "余额为.*元"));
    check.submitted(1, 1700000000u);
    check.arm(0, 300);
    check.observe("10086", "其他内容", 1, 1);
    assert(check.poll(1) == State::Matched);
    assert(check.begin("^10086$", "余额为.*元"));
    check.submitted(1, 1700000000u);
    check.arm(0, 300);
    check.observe("1008611", "余额为1元", 1, 1);
    assert(check.poll(1) == State::Matched);
    assert(check.begin("^10086$", ""));
    check.submitted(1, 1700000000u);
    check.arm(0, 300);
    check.observe("1008611", "余额为1元", 1, 1);
    assert(check.poll(1) == State::Waiting);

    // +CMGS 返回前的快速回复不会丢失，取消和下次执行清除上次状态。
    assert(check.begin("^10086$", ""));
    check.observe("10086", "回复", 99, 99);
    assert(check.poll(99) == State::Prepared);
    check.submitted(1, 1700000000u);
    check.observe("10086", "回复", 100, 100);
    check.arm(200, 300);
    assert(check.poll(200) == State::Matched);
    check.cancel();
    assert(check.poll(200) == State::Idle);
    check.observe("10086", "回复", 201, 201);
    assert(check.begin("^10086$", ""));
    check.submitted(1, 1700000000u);
    check.arm(202, 300);
    assert(check.poll(202) == State::Waiting);

    // 处理延迟不能把发送前已到达的 URC 变成新回复；存储补收必须有可信新时间。
    assert(check.begin("^10086$", ""));
    check.submitted(1000, 1789344000);
    check.arm(2000, 300);
    check.observe("10086", "旧回复", 3000, 999);
    assert(check.poll(3000) == State::Waiting);
    check.observe("10086", "旧补收", 3001, 0, 1789343999, 1789344001);
    check.observe("10086", "未来时间", 3002, 0, 1789349000, 1789344001);
    check.observe("10086", "缺失分段", 3003, -1, 1789344001, 1789344001);
    assert(check.poll(3003) == State::Waiting);
    check.observe("10086", "混合旧分段", 3004, 999, 1789344000, 1789344001, true);
    check.observe("10086", "混合旧SMSC", 3004, 2000, 1789343999, 1789344001, true);
    assert(check.poll(3004) == State::Waiting);
    check.observe("10086", "新补收", 3004, 0, 1789344001, 1789344001);
    assert(check.poll(3004) == State::Matched);
    assert(check.begin("^10086$", ""));
    check.submitted(1000, 0);
    check.arm(2000, 300);
    check.observe("10086", "未校时补收", 3000, 0, 1789344001, 1789344001);
    assert(check.poll(3000) == State::Waiting);
    check.observe("10086", "新直推", 3001, 2001);
    assert(check.poll(3001) == State::Matched);

    // 未开启/未同步不发送，旧任务仍按精确间隔时长，回拨不重复。
    const uint32_t midnight = 1789344000; // 2026-09-14 UTC
    const uint32_t last = midnight + 15 * 3600;
    assert(!idf_scheduled_task_due(0, last, 7, 480, 480));
    assert(!idf_scheduled_task_due(last, last - 1, 7, 480, 480));
    assert(!idf_scheduled_task_due(last, last + 7 * 86400 - 1, 7, -1, 480));
    assert(idf_scheduled_task_due(last, last + 7 * 86400, 7, -1, 480));
    // 本地 23:00 建立基准日，7 天后的 08:00 到期，而非多等到 23:00。
    assert(!idf_scheduled_task_due(last, midnight + 7 * 86400 - 1, 7, 480, 480));
    assert(idf_scheduled_task_due(last, midnight + 7 * 86400, 7, 480, 480));
    assert(idf_scheduled_due_epoch(last, 7, 480, 480) == midnight + 7 * 86400);
    assert(idf_scheduled_task_due(last, midnight + 7 * 86400 + 3600, 7, 480, 480));
    // 半小时时区、本地日期跨 UTC 日期，最长周期乘法不溢出。
    const uint32_t negative_last = midnight + 3 * 3600; // UTC-03:30 的前一天 23:30
    const uint32_t negative_due = midnight + 6 * 86400 + 11 * 3600 + 30 * 60;
    assert(!idf_scheduled_task_due(negative_last, negative_due - 1, 7, 480, -210));
    assert(idf_scheduled_task_due(negative_last, negative_due, 7, 480, -210));
    assert(!idf_scheduled_task_due(last, last + 86400, 3650, -1, 0));
    const uint32_t start_late = midnight + 23 * 3600 + 59 * 60;
    const uint32_t finish_next_day = midnight + 86400 + 60;
    const uint32_t anchor = idf_scheduled_success_anchor(1439, start_late, finish_next_day);
    assert(anchor == start_late);
    assert(idf_scheduled_task_due(anchor, start_late + 86400, 1, 1439, 0));
    assert(idf_scheduled_success_anchor(-1, start_late, finish_next_day) == finish_next_day);
    std::puts("scheduled SMS health and local-time regressions passed");
}
