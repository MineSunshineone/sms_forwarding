#pragma once

#include <stdint.h>
#include <regex.h>
#include <string>

// 调用者负责同步；只保留正则和结果，不保留收到的号码或正文。
class IdfSmsReplyCheck {
public:
    enum class State { Idle, Prepared, Waiting, Matched, TimedOut };
    ~IdfSmsReplyCheck() { cancel(); }
    IdfSmsReplyCheck() = default;
    IdfSmsReplyCheck(const IdfSmsReplyCheck&) = delete;
    IdfSmsReplyCheck& operator=(const IdfSmsReplyCheck&) = delete;

    bool begin(const std::string& sender, const std::string& body) {
        cancel();
        if (sender.empty() && body.empty()) return false;
        if (!sender.empty()) {
            if (regcomp(&sender_, sender.c_str(), REG_EXTENDED | REG_NOSUB) != 0) return false;
            sender_valid_ = true;
        }
        if (!body.empty()) {
            if (regcomp(&body_, body.c_str(), REG_EXTENDED | REG_NOSUB) != 0) {
                cancel();
                return false;
            }
            body_valid_ = true;
        }
        state_ = State::Prepared;
        return true;
    }
    // 仅提交最后一段 PDU 后开启匹配，准备 AT/等待提示符期间的短信不能误报成功。
    void submitted(int64_t now_us, uint32_t epoch) {
        if (state_ != State::Prepared) return;
        submitted_us_ = now_us;
        submitted_epoch_ = epoch;
        state_ = State::Waiting;
    }
    // +CMGS 返回前的快速回复也可匹配；发送成功后开始倒计时。
    void arm(int64_t now_us, int timeout_seconds) {
        if (state_ == State::Waiting) deadline_ = now_us + static_cast<int64_t>(timeout_seconds) * 1000000LL;
    }
    void observe(const char* sender, const char* body, int64_t now_us, int64_t received_us, uint32_t sms_epoch = 0, uint32_t now_epoch = 0, bool require_smsc = false) {
        if (poll(now_us) != State::Waiting) return;
        // URC 以实际到达时刻为准；仅存储轮询补收时，要求有效的 SMSC 时间证明确实是新短信。
        if (received_us < 0) return;
        if (received_us > 0) {
            if (received_us < submitted_us_) return;
        }
        if ((received_us == 0 || require_smsc) &&
            (submitted_epoch_ < 1700000000u || sms_epoch < submitted_epoch_ || sms_epoch > now_epoch)) return;
        if ((sender_valid_ && regexec(&sender_, sender, 0, nullptr, 0) == 0) ||
            (body_valid_ && regexec(&body_, body, 0, nullptr, 0) == 0)) state_ = State::Matched;
    }
    State poll(int64_t now_us) {
        if (state_ == State::Waiting && deadline_ != 0 && now_us >= deadline_) state_ = State::TimedOut;
        return state_;
    }
    void cancel() {
        if (sender_valid_) regfree(&sender_);
        if (body_valid_) regfree(&body_);
        sender_valid_ = body_valid_ = false;
        deadline_ = 0;
        submitted_us_ = 0;
        submitted_epoch_ = 0;
        state_ = State::Idle;
    }
private:
    regex_t sender_{};
    regex_t body_{};
    bool sender_valid_ = false;
    bool body_valid_ = false;
    int64_t deadline_ = 0;
    int64_t submitted_us_ = 0;
    uint32_t submitted_epoch_ = 0;
    State state_ = State::Idle;
};
