#pragma once

#include <stdint.h>

#include <string>

#include "esp_err.h"

struct IdfSmsStatus {
    uint32_t total = 0;
    uint32_t lastSmsEpoch = 0;
    bool receiveReady = false;
};

esp_err_t idf_sms_start(void);
esp_err_t idf_sms_send_text(const std::string& phone, const std::string& text, std::string& message, bool track_reply = false);
esp_err_t idf_sms_enqueue_outgoing(const std::string& phone, const std::string& text, std::string& message);
int idf_sms_outgoing_queue_depth(void);
IdfSmsStatus idf_sms_get_status(void);

// 自定义短信任务体检：仅一个蜂窝任务可持有窗口；超时使用单调时钟。
bool idf_sms_reply_check_begin(const std::string& sender_pattern, const std::string& body_pattern);
void idf_sms_reply_check_arm(int timeout_seconds);
// 0=等待，1=匹配成功，-1=超时/未启用。
int idf_sms_reply_check_poll(void);
void idf_sms_reply_check_cancel(void);
