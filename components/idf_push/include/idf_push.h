#pragma once

#include <stdint.h>

#include <string>

#include "esp_err.h"

struct IdfForwardRuleDecision {
    bool matched = false;
    bool drop = false;
    uint32_t chMask = 0;
    bool email = false;
    int ruleIndex = 0;
};

esp_err_t idf_push_start(void);

bool idf_push_enqueue_forward(const char* sender, const char* text, const char* timestamp, uint32_t inbox_id);
int idf_push_enqueue_notify(const char* title, const char* body, const char* timestamp);
bool idf_push_enqueue_email(const char* subject, const char* body);
int idf_push_forward_queue_depth(void);
int idf_push_retry_queue_depth(void);
int idf_push_email_queue_depth(void);
bool idf_push_busy(void);

// 与实际短信转发共用同一规则引擎，供网页规则测试使用，避免浏览器正则语义与固件不一致。
IdfForwardRuleDecision idf_push_eval_forward_rules(const std::string& rules,
                                                   const std::string& sender,
                                                   const std::string& body);

bool idf_push_enqueue_test(uint8_t channel, std::string& message);
std::string idf_push_test_status_json(uint8_t channel);
