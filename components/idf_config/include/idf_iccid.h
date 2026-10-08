#pragma once

#include <string>

// ICCID：89 电信前缀，前六位为十进制；运营商自定义部分允许 A-F（如中国移动）。
// 不截取子串或丢弃内部字符，避免把错误卡号变成另一张卡的 PIN 凭据主键。
inline std::string idf_normalize_iccid(const std::string& raw)
{
    if (raw.size() < 18 || raw.size() > 22 || raw.compare(0, 2, "89") != 0) return {};
    std::string value = raw;
    for (size_t i = 0; i < value.size(); ++i) {
        char& ch = value[i];
        if (ch >= '0' && ch <= '9') continue;
        if (i < 6) return {};
        if (ch >= 'a' && ch <= 'f') ch = static_cast<char>(ch - 'a' + 'A');
        if (ch < 'A' || ch > 'F') return {};
    }
    // EF_ICCID 为十字节；仅第 20 个半字节的 F 作为 19 位卡号的填充。
    if (value.size() == 20 && value.back() == 'F') value.pop_back();
    return value;
}
