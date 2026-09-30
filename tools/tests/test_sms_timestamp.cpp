#include "idf_sms_timestamp.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>

static void test_timezones()
{
    assert(idf_sms_timestamp_epoch("26093016000032") == 1790755200U); // UTC+08:00
    assert(idf_sms_timestamp_epoch("26093009300022") == 1790740800U); // UTC+05:30
    assert(idf_sms_timestamp_epoch("26093009300094") == 1790773200U); // UTC-03:30
    assert(idf_sms_timestamp_epoch("260930040000;2") == 1790769600U); // UTC-08:00
    assert(idf_sms_timestamp_epoch("26093012000000") == 1790769600U);
    assert(idf_sms_timestamp_epoch("26093012000080") == 1790769600U); // 负零
    assert(idf_sms_timestamp_epoch("26093000150001") == 1790726400U); // UTC+00:15
    assert(idf_sms_timestamp_epoch("26092923450081") == 1790726400U); // UTC-00:15 跨日
}

static void test_calendar_and_invalid_inputs()
{
    assert(idf_sms_timestamp_epoch("24022900000000") == 1709164800U);
    assert(idf_sms_timestamp_epoch("00022900000000") == 951782400U); // 2000 年为闰年
    assert(idf_sms_timestamp_epoch("00010100000000") == 946684800U); // 古老短信保留旧时间
    assert(idf_sms_timestamp_epoch("99123123595900") == 4102444799U);
    const char* invalid[] = {
        "", "2609301200000", "260930120000000", "2x093012000000",
        "26003012000000", "26133012000000", "26090012000000", "26093112000000",
        "26043112000000", "23022912000000", "24023012000000", "26093024000000",
        "26093012600000", "26093012006000", "260930120000@0", "260930120000/0",
        "2609301200000:", "2609301200000/"
    };
    for (const char* value : invalid) assert(idf_sms_timestamp_epoch(value) == 0);
    assert(idf_sms_timestamp_epoch(static_cast<const char*>(nullptr)) == 0);
    assert(idf_sms_timestamp_epoch(std::string("26093012000000")) == 1790769600U);
    assert(idf_sms_timestamp_epoch(std::string("260930\0" "2000000", 14)) == 0);
}

static void test_every_supported_calendar_day()
{
    // 与主机 UTC 日期转换独立对照整个可表达世纪；同时覆盖正负半字节符号。
    size_t count = 0;
    for (int year = 2000; year <= 2099; ++year) {
        for (int month = 1; month <= 12; ++month) {
            for (int day = 1; day <= 31; ++day) {
                struct tm civil = {};
                civil.tm_year = year - 1900;
                civil.tm_mon = month - 1;
                civil.tm_mday = day;
                civil.tm_hour = 12;
                const time_t utc = timegm(&civil);
                if (civil.tm_mon != month - 1 || civil.tm_mday != day) continue;
                for (int quarters : {-56, -33, -14, -1, 0, 1, 14, 22, 32, 56}) {
                    const int absolute = quarters < 0 ? -quarters : quarters;
                    const int tens = absolute / 10 + (quarters < 0 ? 8 : 0);
                    char timestamp[32];
                    std::snprintf(timestamp, sizeof(timestamp), "%02d%02d%02d120000%c%c",
                                  year - 2000, month, day, '0' + tens, '0' + absolute % 10);
                    assert(idf_sms_timestamp_epoch(timestamp) == static_cast<uint32_t>(utc - quarters * 900));
                }
                ++count;
            }
        }
    }
    assert(count == 36525);
}

int main()
{
    test_timezones();
    test_calendar_and_invalid_inputs();
    test_every_supported_calendar_day();
    std::puts("SMS timestamp regressions passed (36525 dates, signed quarter-hour zones)");
}
