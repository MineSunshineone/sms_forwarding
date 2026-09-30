#pragma once

#include <stdint.h>
#include <string.h>
#include <string>

// PDU::getTimeStamp() 已交换 SCTS 的半字节，前 12 位为 YYMMDDhhmmss。
// 最后两位仍保留时区符号位：首字符减 '0' 后的 bit 3 表示负时区，
// 例如 +08:00 为 "32"，-08:00 为 ";2"，不能直接按十进制字符串解析。
// 两位年份按本固件工作年代解释为 2000..2099；调用方另行验证消息的新鲜度。
// 不依赖设备/主机时区；格式错误、非法日期返回 0。
inline uint32_t idf_sms_timestamp_epoch(const char* timestamp)
{
    if (!timestamp || strlen(timestamp) != 14) return 0;
    int values[6] = {};
    for (int i = 0; i < 6; ++i) {
        const unsigned char tens = static_cast<unsigned char>(timestamp[i * 2]);
        const unsigned char units = static_cast<unsigned char>(timestamp[i * 2 + 1]);
        if (tens < '0' || tens > '9' || units < '0' || units > '9') return 0;
        values[i] = (tens - '0') * 10 + (units - '0');
    }
    const int year = 2000 + values[0];
    const int month = values[1];
    const int day = values[2];
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    static constexpr int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || day < 1 ||
        day > month_days[month - 1] + (month == 2 && leap ? 1 : 0) ||
        values[3] > 23 || values[4] > 59 || values[5] > 59) return 0;

    const int zone_tens = static_cast<unsigned char>(timestamp[12]) - '0';
    const int zone_units = static_cast<unsigned char>(timestamp[13]) - '0';
    if (zone_tens < 0 || zone_tens > 15 || zone_units < 0 || zone_units > 9) return 0;
    int quarters = (zone_tens & 7) * 10 + zone_units;
    if ((zone_tens & 8) != 0) quarters = -quarters;

    // 公历累计日数，显式计算避免 mktime 随设备时区和夏令时而变化。
    int64_t days = static_cast<int64_t>(year - 1970) * 365 +
        (year - 1) / 4 - 1969 / 4 - ((year - 1) / 100 - 1969 / 100) +
        (year - 1) / 400 - 1969 / 400;
    for (int m = 1; m < month; ++m) days += month_days[m - 1] + (m == 2 && leap ? 1 : 0);
    days += day - 1;
    const int64_t epoch = days * 86400LL + values[3] * 3600 + values[4] * 60 + values[5] -
                          static_cast<int64_t>(quarters) * 900;
    return epoch > 0 && epoch <= UINT32_MAX ? static_cast<uint32_t>(epoch) : 0;
}

inline uint32_t idf_sms_timestamp_epoch(const std::string& timestamp)
{
    return timestamp.size() == 14 ? idf_sms_timestamp_epoch(timestamp.c_str()) : 0;
}
