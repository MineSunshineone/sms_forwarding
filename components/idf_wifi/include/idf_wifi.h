#pragma once

#include <string>

#include "esp_err.h"
#include "idf_config.h"

struct IdfWifiStatus {
    bool staConnected = false;
    bool apMode = false;
    int rssi = 0;
    int channel = 0;
    std::string ssid;
    std::string ip;
    std::string gw;
    std::string mask;
    std::string dns;
    std::string mac;
    std::string bssid;
    std::string apSsid;
    std::string apIp;
};

esp_err_t idf_wifi_start(const IdfConfig& config);
esp_err_t idf_wifi_reconnect(void);
// 手动强制立即 NTP 校时(网页"立即校时"按钮)；未联网返回 ESP_ERR_INVALID_STATE
esp_err_t idf_wifi_resync_ntp(void);
esp_err_t idf_wifi_scan_json(std::string& out_json);
IdfWifiStatus idf_wifi_get_status(void);
// 是否处于配网热点(AP/APSTA)模式的轻量查询——只读内部状态快照，不触发
// esp_wifi/MAC/IP 读取，供每个请求都要判断的 Web 鉴权快速路径使用。
bool idf_wifi_is_ap_mode(void);
// 配网热点内保存 WiFi 后原地(APSTA)发起连接，不重启；连上后设备会延时自动关闭热点。
// 供 Web /wificonfig 在 AP 模式下调用，配网页轮询 /apstatus 显示获取到的 IP。
esp_err_t idf_wifi_provision_connect(const std::string& ssid, const std::string& pass);
