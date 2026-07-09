#pragma once

#include <stdint.h>

#include <string>

#include "esp_err.h"
#include "idf_config.h"

struct IdfModemStatus {
    bool started = false;
    bool atReady = false;
    bool modemReady = false;
    bool signalFresh = false;
    bool identityFresh = false;
    std::string phase = "off";
    int ceregStat = -1;
    int csq = -1;
    int ber = 99;
    int rsrp = 999;
    int rsrq = 999;
    int sinr = 999;
    std::string mfr;
    std::string model;
    std::string fwver;
    std::string imei;
    std::string iccid;
    std::string imsi;
    std::string operatorName;
    std::string apnSim;
    std::string cellIp;
    std::string phone;
};

struct IdfCellularHttpResult {
    bool ok = false;
    int httpStatus = -1;
    uint32_t bytesRead = 0;
    uint32_t expectedBytes = 0;
    int mhttpError = 0;
    std::string cellIp;
    std::string message;
};

struct IdfCellularHttpConfig {
    bool dataEnabled = false;
    std::string apn;
};

esp_err_t idf_modem_start(const IdfConfig& config);
esp_err_t idf_modem_send_at(const std::string& cmd, uint32_t timeout_ms, std::string& response);
esp_err_t idf_modem_send_at_until(const std::string& cmd, const char* token, uint32_t timeout_ms, std::string& response);
esp_err_t idf_modem_send_pdu(const std::string& cmgs_cmd, const char* pdu, uint32_t timeout_ms, std::string& response);
esp_err_t idf_modem_cellular_http_get(const std::string& url, const IdfCellularHttpConfig& config, IdfCellularHttpResult& result);
esp_err_t idf_modem_request_reset(bool hard_reset);
bool idf_modem_take_urc(std::string& out);
// 等待模组事件(新 URC 入缓冲/外部唤醒)，超时返回 false；用于替代固定轮询延时
bool idf_modem_wait_event(uint32_t timeout_ms);
// 唤醒等待者(短信任务)：URC 入缓冲、网页短信入队等场景调用
void idf_modem_signal_event(void);
IdfModemStatus idf_modem_get_status(void);
// AT 通道当前是否空闲（Web 路由用于"模组正忙"快速返回，避免长时间阻塞 httpd 任务）
bool idf_modem_at_idle(void);
// 用户显式请求刷新概览模组信息时，短时间打开展示型身份/信号采样窗口。
void idf_modem_request_status_sample(void);
// 启用/切换/禁用 eSIM Profile 后调用：清除缓存的卡相关身份(号码/ICCID/IMSI/运营商/APN)
// 并请求一次采样，使概览重读新生效 Profile 的信息，而不是沿用旧卡缓存值。
void idf_modem_invalidate_sim_identity(void);
// 计划内 ESP 重启(网页重启/每日定时重启/低堆重启)前调用：拉低 EN 让模组彻底断电，
// 重启后走全新上电。保留"重启设备可救活 AT 正常但收信已死的模组"的原有语义；
// 热启动快路径只服务于崩溃/看门狗等意外复位
void idf_modem_power_off_for_restart(void);
