#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "idf_config.h"
#include "idf_iccid.h"

using nvs_handle_t = int;
constexpr int portMAX_DELAY = 0, pdTRUE = 1, NVS_READWRITE = 1;
const char* TAG = "test_sim_credentials";
int s_config_mutex = 1, s_persist_mutex = 2;
IdfConfig s_config;
std::map<std::string, std::string> nvs_strings, pending_strings;
std::map<std::string, uint8_t> nvs_bytes, pending_bytes;
int nvs_opens = 0, nvs_writes = 0, nvs_reads = 0;
bool fail_open = false, fail_write = false, fail_commit = false;
bool fail_read_size = false, fail_read_value = false;

template<class... Args> void ignore_log(const Args&...) {}
#define ESP_LOGW(...) ignore_log(__VA_ARGS__)
#define ESP_LOGE(...) ignore_log(__VA_ARGS__)
void idf_logf(const char*, ...) {}
void idf_log_line(const char*) {}
const char* esp_err_to_name(esp_err_t) { return "mock error"; }
esp_err_t ensure_config_mutex() { return ESP_OK; }
int xSemaphoreTake(int, int) { return pdTRUE; }
void xSemaphoreGive(int) {}

esp_err_t nvs_open(const char*, int, nvs_handle_t* handle)
{
    ++nvs_opens;
    if (fail_open) return ESP_FAIL;
    *handle = 1;
    pending_strings = nvs_strings;
    pending_bytes = nvs_bytes;
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t)
{
    if (fail_commit) return ESP_FAIL;
    nvs_strings = pending_strings;
    nvs_bytes = pending_bytes;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t, const char* key, const char* value)
{
    ++nvs_writes;
    if (fail_write) return ESP_FAIL;
    pending_strings[key] = std::string(value) + '\0';
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t, const char* key, uint8_t value)
{
    ++nvs_writes;
    if (fail_write) return ESP_FAIL;
    pending_bytes[key] = value;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* value, size_t* length)
{
    ++nvs_reads;
    if ((!value && fail_read_size) || (value && fail_read_value)) return ESP_FAIL;
    const auto found = nvs_strings.find(key);
    if (found == nvs_strings.end()) return ESP_ERR_NVS_NOT_FOUND;
    const std::string& raw = found->second;
    if (value && *length < raw.size()) {
        *length = raw.size();
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (value) std::memcpy(value, raw.data(), raw.size());
    *length = raw.size();
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t, const char* key, uint8_t* value)
{
    const auto found = nvs_bytes.find(key);
    if (found == nvs_bytes.end()) return ESP_ERR_NVS_NOT_FOUND;
    *value = found->second;
    return ESP_OK;
}

// @配置函数@

const std::string official_lower = "898600d6991330004146";
const std::string official_upper = "898600D6991330004146";
const std::string other_iccid = "898600E6991330004146";
const std::string padded_iccid = "898600123456789012FF";
const std::string unpadded_iccid = "898600123456789012F";

void reset_fixture()
{
    s_config = IdfConfig();
    nvs_strings.clear();
    nvs_bytes.clear();
    pending_strings.clear();
    pending_bytes.clear();
    nvs_opens = nvs_writes = nvs_reads = 0;
    fail_open = fail_write = fail_commit = fail_read_size = fail_read_value = false;
}

IdfSimCredential credential(const std::string& iccid,
                            const std::string& pin = "1234", const std::string& puk = "12345678")
{
    IdfSimCredential result;
    result.iccid = iccid;
    result.pin = pin;
    result.puk = puk;
    result.pinMaxAttempts = 2;
    result.pukMaxAttempts = 5;
    return result;
}

esp_err_t save(const IdfSimCredential credentials[IDF_MAX_SIM_CREDENTIALS])
{
    return idf_config_save_sim(true, false, "cmnet", "46000", "", credentials);
}

void load_credentials()
{
    IdfConfig next;
    nvs_handle_t nvs = 1;
    // @载入片段@
    sanitize_sim_credentials(next);
    s_config = next;
}

void test_normalization()
{
    assert(idf_normalize_iccid(official_lower) == official_upper);
    assert(idf_normalize_iccid(official_upper) == official_upper);
    assert(idf_normalize_iccid(other_iccid) == other_iccid);
    assert(idf_normalize_iccid("898600abcdef12345678") == "898600ABCDEF12345678");
    assert(idf_normalize_iccid(padded_iccid) == unpadded_iccid);
    assert(idf_normalize_iccid(unpadded_iccid) == unpadded_iccid);
    assert(idf_normalize_iccid("8986001234567890123f") == "8986001234567890123");
    for (size_t length = 18; length <= 22; ++length) {
        const std::string decimal = "898600" + std::string(length - 6, '1');
        assert(idf_normalize_iccid(decimal) == decimal);
        const std::string trailing_f = decimal.substr(0, length - 1) + "F";
        assert(idf_normalize_iccid(trailing_f) ==
               (length == 20 ? trailing_f.substr(0, 19) : trailing_f));
    }
    for (const std::string& bad : std::vector<std::string>{
             "", "89860012345678901", "89860012345678901234567",
             "888600D6991330004146", "89A600D6991330004146", "89860fD6991330004146",
             "898600G6991330004146", "898600D69913300041-6", "898600D69913300041 6",
             " " + official_upper, official_upper + " ", official_upper + "ABCD",
             std::string("898600\0" "6991330004146", 20),
             std::string("898600\xFF" "6991330004146", 20)}) {
        assert(idf_normalize_iccid(bad).empty());
    }
}

void test_sanitize()
{
    reset_fixture();
    s_config.simCredentials[0] = credential(official_lower);
    s_config.simCredentials[0].pinFailedAttempts = 1;
    s_config.simCredentials[0].pukFailedAttempts = 4;
    s_config.simCredentials[1] = credential(official_upper, "9999");
    s_config.simCredentials[2] = credential(other_iccid, "12AF", "1234abcd");
    s_config.simCredentials[2].pinMaxAttempts = 255;
    s_config.simCredentials[2].pukMaxAttempts = 0;
    s_config.simCredentials[2].pinFailedAttempts = 255;
    s_config.simCredentials[2].pukFailedAttempts = 255;
    s_config.simCredentials[3] = credential("898600G6991330004146");
    s_config.simCredentials[4] = credential(padded_iccid);
    sanitize_sim_credentials(s_config);
    assert(s_config.simCredentials[0].iccid == official_upper);
    assert(s_config.simCredentials[0].pin == "1234");
    assert(s_config.simCredentials[0].pinFailedAttempts == 1);
    assert(s_config.simCredentials[0].pukFailedAttempts == 4);
    assert(s_config.simCredentials[1].iccid == other_iccid);
    assert(s_config.simCredentials[1].pin.empty());
    assert(s_config.simCredentials[1].puk.empty());
    assert(s_config.simCredentials[1].pinMaxAttempts == 2);
    assert(s_config.simCredentials[1].pukMaxAttempts == 1);
    assert(s_config.simCredentials[1].pinFailedAttempts == 2);
    assert(s_config.simCredentials[1].pukFailedAttempts == 1);
    assert(s_config.simCredentials[2].iccid == unpadded_iccid);
    for (int i = 3; i < IDF_MAX_SIM_CREDENTIALS; ++i) {
        assert(s_config.simCredentials[i].iccid.empty());
        assert(s_config.simCredentials[i].pin.empty());
        assert(s_config.simCredentials[i].puk.empty());
        assert(s_config.simCredentials[i].pinFailedAttempts == 0);
        assert(s_config.simCredentials[i].pukFailedAttempts == 0);
    }
    sanitize_sim_credentials(s_config);
    assert(s_config.simCredentials[2].iccid == unpadded_iccid);
}

void test_save_case_and_counter_preservation()
{
    reset_fixture();
    IdfSimCredential rows[IDF_MAX_SIM_CREDENTIALS];
    rows[0] = credential(official_lower);
    rows[1] = credential(other_iccid, "5678", "87654321");
    rows[2] = credential(padded_iccid);
    assert(save(rows) == ESP_OK);
    assert(s_config.simCredentials[0].iccid == official_upper);
    assert(nvs_strings.at("sim0Iccid") == official_upper + '\0');
    assert(nvs_strings.at("sim2Iccid") == unpadded_iccid + '\0');
    assert(idf_config_record_sim_unlock_result(official_lower, false, false) == ESP_OK);
    assert(idf_config_record_sim_unlock_result(official_lower, true, false) == ESP_OK);
    assert(idf_config_record_sim_unlock_result(other_iccid, true, false) == ESP_OK);
    assert(idf_config_record_sim_unlock_result(other_iccid, true, false) == ESP_OK);
    // 网页密码留空且卡号只改变大小写时，沿用各自密码与失败计数。
    rows[0] = credential(official_upper, "", "");
    rows[1] = credential("898600e6991330004146", "", "");
    assert(save(rows) == ESP_OK);
    assert(s_config.simCredentials[0].pin == "1234");
    assert(s_config.simCredentials[0].puk == "12345678");
    assert(s_config.simCredentials[0].pinFailedAttempts == 1);
    assert(s_config.simCredentials[0].pukFailedAttempts == 1);
    assert(s_config.simCredentials[1].pin == "5678");
    assert(s_config.simCredentials[1].puk == "87654321");
    assert(s_config.simCredentials[1].pinFailedAttempts == 0);
    assert(s_config.simCredentials[1].pukFailedAttempts == 2);
    assert(s_config.simCredentials[2].iccid == unpadded_iccid);
    rows[0].iccid = official_lower;
    assert(save(rows) == ESP_OK);
    assert(s_config.simCredentials[0].pinFailedAttempts == 1);
    assert(s_config.simCredentials[0].pukFailedAttempts == 1);
    // 显式重置 PIN 不应重置 PUK，修改密码只重置对应的失败计数。
    rows[0].pinFailedAttempts = UINT8_MAX;
    assert(save(rows) == ESP_OK);
    assert(s_config.simCredentials[0].pinFailedAttempts == 0);
    assert(s_config.simCredentials[0].pukFailedAttempts == 1);
    rows[0].pinFailedAttempts = 0;
    rows[0].puk = "98765432";
    assert(save(rows) == ESP_OK);
    assert(s_config.simCredentials[0].pukFailedAttempts == 0);
}

void test_reject_invalid_save()
{
    reset_fixture();
    IdfSimCredential rows[IDF_MAX_SIM_CREDENTIALS];
    rows[0] = credential(official_upper);
    assert(save(rows) == ESP_OK);
    const auto saved_strings = nvs_strings;
    const int writes_before = nvs_writes, opens_before = nvs_opens;
    for (const std::string& bad : std::vector<std::string>{
             "898600G6991330004146", "888600D6991330004146", "89A600D6991330004146",
             official_upper + "ABCD", "89860012345678901234567",
             std::string("898600D6991330004146\0", 21)}) {
        rows[0] = credential(bad);
        assert(save(rows) == ESP_ERR_INVALID_ARG);
        assert(nvs_writes == writes_before && nvs_opens == opens_before);
        assert(s_config.simCredentials[0].iccid == official_upper);
        assert(nvs_strings == saved_strings);
        s_config.simCredentials[1] = credential(bad);
        sanitize_sim_credentials(s_config);
        assert(s_config.simCredentials[1].iccid.empty());
    }
    rows[0] = credential(official_upper);
    rows[1] = credential(official_lower);
    assert(save(rows) == ESP_ERR_INVALID_ARG);
    rows[0] = credential(padded_iccid);
    rows[1] = credential(unpadded_iccid);
    assert(save(rows) == ESP_ERR_INVALID_ARG);
    rows[1] = IdfSimCredential();
    for (const char* pin : {"12AF", "12ab", "123", "123456789", "12 4", "+1234"}) {
        rows[0] = credential(official_lower, pin);
        assert(save(rows) == ESP_ERR_INVALID_ARG);
    }
    for (const char* puk : {"1234ABCD", "1234abcd", "1234567", "123456789", "1234 678"}) {
        rows[0] = credential(official_lower, "1234", puk);
        assert(save(rows) == ESP_ERR_INVALID_ARG);
    }
    assert(nvs_writes == writes_before && nvs_opens == opens_before);
    assert(nvs_strings == saved_strings);
}

void test_lookup_and_attempt_isolation()
{
    reset_fixture();
    IdfSimCredential rows[IDF_MAX_SIM_CREDENTIALS];
    rows[0] = credential(official_lower);
    rows[1] = credential(other_iccid, "5678", "87654321");
    rows[2] = credential(padded_iccid);
    assert(save(rows) == ESP_OK);
    assert(idf_config_get_sim_unlock_view(official_lower).credential.iccid == official_upper);
    assert(idf_config_get_sim_unlock_view(official_upper).credential.pin == "1234");
    assert(idf_config_get_sim_unlock_view("898600e6991330004146").credential.pin == "5678");
    for (const std::string& padded : {padded_iccid, unpadded_iccid}) {
        assert(idf_config_get_sim_unlock_view(padded).credential.iccid == unpadded_iccid);
        assert(idf_config_record_sim_unlock_result(padded, false, false) == ESP_OK);
    }
    assert(s_config.simCredentials[2].pinFailedAttempts == 2);
    for (const std::string& missing : std::vector<std::string>{
             "", "898600G6991330004146", "898600D6991330004147",
             "8986006991330004146", official_upper + "ABCD"}) {
        assert(!idf_config_get_sim_unlock_view(missing).found);
        assert(idf_config_record_sim_unlock_result(missing, false, false) != ESP_OK);
    }
    assert(s_config.simCredentials[0].pinFailedAttempts == 0);
    assert(s_config.simCredentials[1].pinFailedAttempts == 0);
    for (int i = 0; i < 8; ++i) {
        assert(idf_config_record_sim_unlock_result(official_lower, false, false) == ESP_OK);
        assert(idf_config_record_sim_unlock_result(official_lower, true, false) == ESP_OK);
    }
    assert(s_config.simCredentials[0].pinFailedAttempts == 2);
    assert(s_config.simCredentials[0].pukFailedAttempts == 5);
    assert(s_config.simCredentials[1].pinFailedAttempts == 0);
    assert(s_config.simCredentials[1].pukFailedAttempts == 0);
    assert(nvs_bytes.at("sim0PinFail") == 2);
    assert(nvs_bytes.at("sim0PukFail") == 5);
    assert(idf_config_record_sim_unlock_result(official_lower, false, true) == ESP_OK);
    assert(s_config.simCredentials[0].pinFailedAttempts == 0);
    assert(s_config.simCredentials[0].pukFailedAttempts == 5);
    assert(idf_config_record_sim_unlock_result(official_lower, true, true) == ESP_OK);
    assert(s_config.simCredentials[0].pukFailedAttempts == 0);
}

void test_nvs_load_without_truncation()
{
    reset_fixture();
    assert(read_iccid(1, "missing").empty());
    nvs_strings["sim0Iccid"] = official_lower + '\0';
    nvs_strings["sim0Pin"] = std::string("1234") + '\0';
    nvs_strings["sim0Puk"] = std::string("12345678") + '\0';
    nvs_bytes["sim0PinFail"] = 1;
    nvs_strings["sim1Iccid"] = official_upper + '\0';
    nvs_strings["sim2Iccid"] = padded_iccid + '\0';
    nvs_strings["sim3Iccid"] = "89860012345678901234567" + std::string(1, '\0');
    nvs_strings["sim4Iccid"] = std::string("898600D69913300041\0" "6", 20) + '\0';
    load_credentials();
    assert(s_config.simCredentials[0].iccid == official_upper);
    assert(s_config.simCredentials[0].pin == "1234");
    assert(s_config.simCredentials[0].puk == "12345678");
    assert(s_config.simCredentials[0].pinFailedAttempts == 1);
    assert(s_config.simCredentials[1].iccid == unpadded_iccid);
    assert(s_config.simCredentials[2].iccid.empty());
    for (size_t length = 18; length <= 22; ++length) {
        const std::string valid = "898600" + std::string(length - 6, '2');
        nvs_strings["probe"] = valid + '\0';
        assert(read_iccid(1, "probe") == valid);
    }
    nvs_reads = 0;
    nvs_strings["probe"] = official_upper + std::string(10000, '1') + '\0';
    assert(read_iccid(1, "probe").empty());
    assert(nvs_reads == 1);
    // 不完整的 NVS 字符串必须拒绝，不能靠缓冲区初值伪造终止符。
    nvs_strings["probe"] = official_upper;
    assert(read_iccid(1, "probe").empty());
    nvs_strings["probe"] = official_lower + '\0';
    fail_read_size = true;
    assert(read_iccid(1, "probe").empty());
    fail_read_size = false;
    fail_read_value = true;
    assert(read_iccid(1, "probe").empty());
}

void test_failed_persistence_does_not_publish_changes()
{
    for (int stage = 0; stage < 3; ++stage) {
        reset_fixture();
        IdfSimCredential rows[IDF_MAX_SIM_CREDENTIALS];
        rows[0] = credential(official_lower);
        assert(save(rows) == ESP_OK);
        fail_open = stage == 0;
        fail_write = stage == 1;
        fail_commit = stage == 2;
        rows[0] = credential(other_iccid);
        assert(save(rows) == ESP_FAIL);
        assert(s_config.simCredentials[0].iccid == official_upper);
        assert(idf_config_record_sim_unlock_result(official_lower, false, false) == ESP_FAIL);
        assert(s_config.simCredentials[0].pinFailedAttempts == 0);
    }
}

void test_modem_identity_cache()
{
    reset_fixture();
    save_identity_cache("", official_lower);
    assert(nvs_strings.at("modemIccid") == official_upper + '\0');
    const int writes_before = nvs_writes;
    save_identity_cache("", official_lower);
    assert(nvs_writes == writes_before);
    for (const std::string& bad : std::vector<std::string>{
             "", "898600G6991330004146", "888600D6991330004146", official_upper + "ABCD"}) {
        save_identity_cache("", bad);
        assert(nvs_writes == writes_before);
        assert(nvs_strings.at("modemIccid") == official_upper + '\0');
    }
    // 合法 IMEI 的缓存更新不能带入无效 ICCID 或覆盖已知卡号。
    save_identity_cache("123456789012345", "898600G6991330004146");
    assert(nvs_strings.at("modemImei") == std::string("123456789012345") + '\0');
    assert(nvs_strings.at("modemIccid") == official_upper + '\0');
    nvs_strings["modemIccid"] = official_lower + '\0';
    save_identity_cache("", official_lower);
    assert(nvs_strings.at("modemIccid") == official_upper + '\0');
    save_identity_cache("", padded_iccid);
    assert(nvs_strings.at("modemIccid") == unpadded_iccid + '\0');
    save_identity_cache("", unpadded_iccid);
    assert(nvs_strings.at("modemIccid") == unpadded_iccid + '\0');
    fail_commit = true;
    save_identity_cache("", other_iccid);
    assert(nvs_strings.at("modemIccid") == unpadded_iccid + '\0');
}

int main()
{
    test_normalization();
    test_sanitize();
    test_save_case_and_counter_preservation();
    test_reject_invalid_save();
    test_lookup_and_attempt_isolation();
    test_nvs_load_without_truncation();
    test_failed_persistence_does_not_publish_changes();
    test_modem_identity_cache();
    std::puts("SIM credential normalization, NVS persistence and unlock regressions passed");
}
