#pragma once

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "esp_err.h"

static constexpr int IDF_MAX_PUSH_CHANNELS = 5;
static constexpr const char* IDF_FW_VERSION = "1.1.0";
static constexpr const char* IDF_DEFAULT_WEB_USER = "admin";
static constexpr const char* IDF_DEFAULT_WEB_PASS = "admin123";
static constexpr const char* IDF_KEEPALIVE_DEFAULT_URL = "http://gg.incrafttime.top/api/payload?size=64342";

static constexpr int IDF_MAX_SCHED_TASKS = 6;

// 历史 WiFi 列表：连接前先扫描，与列表匹配后直连在场且信号最好的一组(取代固定
// 主备槽位的顺序盲试)。槽位 0 为最近一次配网保存的网络，兼容旧版单组配置。
static constexpr int IDF_MAX_WIFI_NETWORKS = 5;

struct IdfWifiNetwork {
    std::string ssid;
    std::string pass;
};

// 进阶定时任务：可选定目标 eSIM Profile，每 N 天执行一个动作，完成后可切回原卡
struct IdfSchedTask {
    bool enabled = false;
    std::string name;        // 显示名
    std::string profile;     // 目标 eSIM Profile(ICCID/别名)；空 = 不切卡，用当前卡
    bool switchBack = true;  // 任务完成后切回执行前启用的 Profile
    int intervalDays = 30;   // 触发周期（天）
    uint8_t action = 0;      // 0=推送提醒 1=蜂窝HTTP ping 2=发短信 3=USSD
    std::string target;      // ping URL / 短信号码 / USSD 码
    std::string payload;     // 推送/短信内容
    uint32_t lastRun = 0;    // 基准时间（epoch），0=未建立
};

struct IdfPushChannel {
    bool enabled = false;
    uint8_t type = 1;
    std::string name;
    std::string url;
    std::string key1;
    std::string key2;
    std::string customBody;
};

struct IdfConfig {
    // 槽位 0 持久化在旧键 wifiSsid/wifiPass 上，其余在 wifiNSsid/wifiNPass，
    // OTA 回滚到旧固件仍能读到最近配网的网络
    IdfWifiNetwork wifiNetworks[IDF_MAX_WIFI_NETWORKS];
    bool wifiFromFallback = false;

    std::string smtpServer;
    int smtpPort = 465;
    std::string smtpUser;
    std::string smtpPass;
    std::string smtpSendTo;
    std::string adminPhone;
    bool emailEnabled = true;
    bool pushEnabled = true;

    std::string webUser = IDF_DEFAULT_WEB_USER;
    std::string webPass = IDF_DEFAULT_WEB_PASS;
    std::string numberBlackList;
    std::string forwardRules;

    bool kaEnabled = false;
    int kaIntervalDays = 175;
    uint8_t kaAction = 1;
    std::string kaTarget;
    std::string kaUrl = IDF_KEEPALIVE_DEFAULT_URL;
    std::string kaProfile;
    uint32_t kaLastTime = 0;

    int tzOffsetMin = 480;
    std::string ntpServer = "ntp.aliyun.com";
    std::string mdnsHost = "sms";  // mDNS 主机名(<host>.local)，多设备部署可改名避免互相顶替
    bool rebootEnabled = false;
    int rebootHour = 4;
    bool hbEnabled = false;
    int hbHour = 9;

    bool netLedEnabled = true;  // 模组 NET 指示灯(AT+MNETLIGHT)，关闭后重启依然保持
    bool callNotifyEnabled = true;  // 来电通知：有来电时把主叫号码按短信相同的通道推送
    bool dataEnabled = false;
    bool roamingEnabled = true;  // 允许数据漫游(同手机"数据漫游")：关闭后漫游中不激活蜂窝数据
    std::string apn;
    std::string operatorPlmn;
    std::string phoneNumber;

    IdfPushChannel pushChannels[IDF_MAX_PUSH_CHANNELS];
    IdfSchedTask schedTasks[IDF_MAX_SCHED_TASKS];
};

using IdfFormFields = std::vector<std::pair<std::string, std::string>>;

esp_err_t idf_config_load(void);
esp_err_t idf_config_save(void);
// 配网保存：同名更新密码并提到槽位 0，新网络插入槽位 0，满员挤掉最旧一组
esp_err_t idf_config_save_wifi(const std::string& ssid, const std::string& pass);
// 网页整表保存历史 WiFi 列表；preserve_blank_pass=true 时已存网络密码留空表示不修改
esp_err_t idf_config_save_wifi_networks(const IdfWifiNetwork nets[IDF_MAX_WIFI_NETWORKS],
                                        bool preserve_blank_pass);
// 类手机行为：STA 连接成功后记住当前网络并维持"最近使用"序(LRU)：
// 已在首位直接返回(常驻网络重连零开销)；在列表但不在首位提到首位；
// 新网络/密码变更上位插入，满员挤掉末位(最久未用)的一组
esp_err_t idf_config_note_wifi_connected(const std::string& ssid, const std::string& pass);
esp_err_t idf_config_save_account(const std::string& user, const std::string& pass);
esp_err_t idf_config_save_time(int tz_offset_min, const std::string& ntp_server);
esp_err_t idf_config_save_mdns_host(const std::string& host);
esp_err_t idf_config_save_email(bool enabled, const std::string& server, int port,
                                const std::string& user, const std::string& pass,
                                const std::string& send_to, bool preserve_blank_pass);
esp_err_t idf_config_save_push(bool enabled, const IdfPushChannel channels[IDF_MAX_PUSH_CHANNELS]);
esp_err_t idf_config_save_filter(const std::string& admin_phone, const std::string& number_blacklist);
esp_err_t idf_config_validate_forward_rules(const std::string& rules, std::string* message);
// 转发规则 Perl 风格 \d \w \s 转 POSIX 字符类；保存时校验与运行时匹配共用
std::string idf_config_translate_perl_classes(const std::string& pattern);
esp_err_t idf_config_save_forward_rules(const std::string& rules);
esp_err_t idf_config_save_keepalive(bool enabled, int interval_days, uint8_t action,
                                    const std::string& target, const std::string& url,
                                    const std::string& profile);
esp_err_t idf_config_save_system_schedule(bool reboot_enabled, int reboot_hour,
                                          bool hb_enabled, int hb_hour);
esp_err_t idf_config_save_sched_tasks(const IdfSchedTask tasks[IDF_MAX_SCHED_TASKS]);
esp_err_t idf_config_save_sim(bool data_enabled, bool roaming_enabled, const std::string& apn,
                              const std::string& operator_plmn, const std::string& phone_number);
std::string idf_config_export_text(bool full_export);
esp_err_t idf_config_import_text(const std::string& text, int* applied_count);
esp_err_t idf_config_factory_reset(void);
esp_err_t idf_config_set_keepalive_last(uint32_t epoch);
esp_err_t idf_config_set_sched_last(int index, uint32_t epoch);
esp_err_t idf_config_set_net_led_enabled(bool enabled);
esp_err_t idf_config_set_call_notify_enabled(bool enabled);

// /status 高频轮询(2s)专用窄快照：只拷贝状态页用到的字段，
// 避免每次请求做全量配置深拷贝造成持续堆抖动
struct IdfConfigStatusView {
    int tzOffsetMin = 480;
    bool dataEnabled = false;
    bool emailEnabled = true;
    bool pushEnabled = true;
    bool emailConfigured = false;
    int pushEnabledCount = 0;
    std::string adminPhone;
    std::string phoneNumber;
    std::string apn;
};

// 历史 WiFi 的网页视图：只带 SSID 与"密码已设置"标记，密码明文不出配置组件
struct IdfWifiNetworkView {
    std::string ssid;
    bool passSet = false;
};

// /config.json 专用快照：不带定时任务数组，避免面板切换/保存后刷新时全量深拷贝
struct IdfConfigWebView {
    std::string webUser = IDF_DEFAULT_WEB_USER;
    std::string webPass = IDF_DEFAULT_WEB_PASS;
    std::string smtpServer;
    int smtpPort = 465;
    std::string smtpUser;
    std::string smtpPass;
    std::string smtpSendTo;
    std::string adminPhone;
    std::string numberBlackList;
    std::string forwardRules;
    bool emailEnabled = true;
    bool emailConfigured = false;
    bool pushEnabled = true;
    int pushEnabledCount = 0;
    std::string ntpServer = "ntp.aliyun.com";
    std::string mdnsHost = "sms";
    int tzOffsetMin = 480;
    bool rebootEnabled = false;
    int rebootHour = 4;
    bool hbEnabled = false;
    int hbHour = 9;
    bool dataEnabled = false;
    bool roamingEnabled = true;
    std::string apn;
    std::string phoneNumber;
    std::string operatorPlmn;
    std::string kaProfile;
    bool netLedEnabled = true;
    bool callNotifyEnabled = true;
    IdfWifiNetworkView wifiNetworks[IDF_MAX_WIFI_NETWORKS];
    IdfPushChannel pushChannels[IDF_MAX_PUSH_CHANNELS];
};

// /schedtask 状态轮询(任务执行期间每 2s)专用窄快照：
// 只拷贝任务槽 + 时区，避免全量配置(含推送通道大字符串)反复深拷贝
struct IdfSchedStatusView {
    IdfSchedTask tasks[IDF_MAX_SCHED_TASKS];
    int tzOffsetMin = 480;
};

struct IdfKeepaliveStatusView {
    bool kaEnabled = false;
    int kaIntervalDays = 175;
    uint8_t kaAction = 1;
    std::string kaTarget;
    std::string kaUrl = IDF_KEEPALIVE_DEFAULT_URL;
    std::string kaProfile;
    uint32_t kaLastTime = 0;
    int tzOffsetMin = 480;
};

// 保号执行任务专用快照：只带动作所需字段，避免把整份配置拷进后台任务参数
struct IdfKeepaliveRunView {
    bool kaEnabled = false;
    int kaIntervalDays = 175;
    uint8_t kaAction = 1;
    std::string kaTarget;
    std::string kaUrl = IDF_KEEPALIVE_DEFAULT_URL;
    std::string kaProfile;
    uint32_t kaLastTime = 0;
    int tzOffsetMin = 480;
    bool emailEnabled = true;
    bool dataEnabled = false;
    std::string apn;
};

// 单个自定义定时任务执行快照：手动触发和 scheduler 只需要当前槽位
struct IdfSchedRunView {
    bool valid = false;
    IdfSchedTask task;
    std::string kaUrl = IDF_KEEPALIVE_DEFAULT_URL;
    int tzOffsetMin = 480;
    bool emailEnabled = true;
    bool emailConfigured = false;
    bool dataEnabled = false;
    std::string apn;
};

struct IdfSimSettingsView {
    bool dataEnabled = false;
    bool roamingEnabled = true;
    std::string apn;
    std::string operatorPlmn;
};

struct IdfSmsProcessView {
    std::string adminPhone;
    std::string numberBlackList;
    int tzOffsetMin = 480;
};

struct IdfPushForwardView {
    std::string forwardRules;
    bool pushEnabled = true;
    bool emailEnabled = true;
    bool emailConfigured = false;
    IdfPushChannel pushChannels[IDF_MAX_PUSH_CHANNELS];
};

struct IdfPushNotifyView {
    bool pushEnabled = true;
    int tzOffsetMin = 480;
    IdfPushChannel pushChannels[IDF_MAX_PUSH_CHANNELS];
};

struct IdfEmailSettingsView {
    bool emailEnabled = true;
    bool emailConfigured = false;
    std::string smtpServer;
    int smtpPort = 465;
    std::string smtpUser;
    std::string smtpPass;
    std::string smtpSendTo;
};

struct IdfSchedulerView {
    bool kaEnabled = false;
    int kaIntervalDays = 175;
    uint32_t kaLastTime = 0;
    int tzOffsetMin = 480;
    bool rebootEnabled = false;
    int rebootHour = 4;
    bool hbEnabled = false;
    int hbHour = 9;
    bool emailEnabled = true;
    IdfSchedTask schedTasks[IDF_MAX_SCHED_TASKS];
};

IdfConfig idf_config_get(void);
IdfConfigStatusView idf_config_get_status_view(void);
IdfConfigWebView idf_config_get_web_view(void);
IdfSchedStatusView idf_config_get_sched_view(void);
IdfKeepaliveStatusView idf_config_get_keepalive_status_view(void);
IdfKeepaliveRunView idf_config_get_keepalive_run_view(void);
IdfSchedRunView idf_config_get_sched_run_view(int index);
IdfSimSettingsView idf_config_get_sim_settings_view(void);
IdfSmsProcessView idf_config_get_sms_process_view(void);
IdfPushForwardView idf_config_get_push_forward_view(void);
IdfPushNotifyView idf_config_get_push_notify_view(void);
IdfEmailSettingsView idf_config_get_email_settings_view(void);
IdfSchedulerView idf_config_get_scheduler_view(void);
bool idf_config_get_push_channel(uint8_t channel, IdfPushChannel& out);
bool idf_config_has_sta_credentials(void);
int idf_config_enabled_push_count(void);
bool idf_config_email_configured(void);
// 锁内直接比对 Web 凭据，避免每个 HTTP 请求做一次全量配置深拷贝
bool idf_config_check_web_auth(const char* user, const char* pass);
// 模组初始化只需这一个开关，锁内求值避免在模组任务栈上放全量配置副本
bool idf_config_net_led_enabled(void);
bool idf_config_call_notify_enabled(void);
// 时区/NTP 窄访问器：给运行在小栈任务(tiT/lwip、系统事件)的 SNTP 回调用，
// 避免深拷贝整个 IdfConfig 到小栈上导致爆栈
int idf_config_get_tz_offset(void);
std::string idf_config_get_ntp_server(void);
// mDNS 主机名窄访问器：应答任务(3KB 栈)每秒节拍轮询，写入定长缓冲，
// 既避免全量配置深拷贝又避免每秒一次 std::string 堆分配
void idf_config_copy_mdns_host(char* out, size_t cap);
// 历史 WiFi 访问器：选网任务取列表(只含非空槽位)；计数器给 15s 重连看门狗
// (esp_timer 任务)判断是否需要扫描选网，锁内只数个数
std::vector<IdfWifiNetwork> idf_config_get_wifi_networks(void);
int idf_config_wifi_network_count(void);
