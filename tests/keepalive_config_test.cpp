#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "idf_config.h"

using nvs_handle_t = int;
constexpr int portMAX_DELAY = 0, pdTRUE = 1, NVS_READWRITE = 1;
const char* TAG = "test_keepalive_config";
int s_config_mutex = 1, s_persist_mutex = 2;
IdfConfig s_config;
std::map<std::string, std::string> nvs_strings, pending_strings;
std::map<std::string, int64_t> nvs_numbers, pending_numbers;
bool fail_commit = false;

template<class... Args> void ignore_log(const Args&...) {}
#define ESP_LOGW(...) ignore_log(__VA_ARGS__)
#define ESP_LOGE(...) ignore_log(__VA_ARGS__)
void idf_logf(const char*, ...) {}
const char* esp_err_to_name(esp_err_t) { return "mock error"; }
esp_err_t ensure_config_mutex() { return ESP_OK; }
int xSemaphoreTake(int, int) { return pdTRUE; }
void xSemaphoreGive(int) {}

esp_err_t nvs_open(const char*, int, nvs_handle_t* handle)
{
    *handle = 1;
    pending_strings = nvs_strings;
    pending_numbers = nvs_numbers;
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_commit(nvs_handle_t)
{
    if (fail_commit) return ESP_FAIL;
    nvs_strings = pending_strings;
    nvs_numbers = pending_numbers;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t, const char* key, const char* value)
{
    pending_strings[key] = std::string(value) + '\0';
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* value, size_t* length)
{
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
esp_err_t nvs_set_u8(nvs_handle_t, const char* key, uint8_t value)
{
    pending_numbers[key] = value;
    return ESP_OK;
}
esp_err_t nvs_set_i32(nvs_handle_t, const char* key, int32_t value)
{
    pending_numbers[key] = value;
    return ESP_OK;
}
template<class T> esp_err_t get_number(const char* key, T* value)
{
    const auto found = nvs_numbers.find(key);
    if (found == nvs_numbers.end()) return ESP_ERR_NVS_NOT_FOUND;
    *value = static_cast<T>(found->second);
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t, const char* key, uint8_t* value) { return get_number(key, value); }
esp_err_t nvs_get_i32(nvs_handle_t, const char* key, int32_t* value) { return get_number(key, value); }
esp_err_t nvs_get_u32(nvs_handle_t, const char* key, uint32_t* value) { return get_number(key, value); }

// @配置函数@

void reload_keepalive()
{
    // 清空运行时状态，再执行固件中的真实载入片段，避免只验证内存回显。
    s_config = IdfConfig();
    IdfConfig next;
    nvs_handle_t nvs = 1;
    // @载入片段@
    s_config = next;
}

void assert_view(const std::string& url)
{
    const auto view = idf_config_get_keepalive_run_view();
    assert(view.kaEnabled);
    assert(view.kaIntervalDays == 175);
    assert(view.kaAction == 3);
    assert(view.kaTarget == "10086");
    assert(view.kaProfile == "profile-a");
    assert(view.kaUrl == url);
}

int main()
{
    assert(std::string(IDF_KEEPALIVE_DEFAULT_URL) ==
           "https://gg.incrafttime.top/api/payload?size=64342");
    const std::vector<std::string> urls = {
        "https://gg.incrafttime.top/api/payload?size=128684",
        "https://gg.incrafttime.top/api/payload?size=1286840",
        "https://gg.incrafttime.top/api/payload?size=100000",
        "https://gg.incrafttime.top/api/payload?size=64342",
        "https://gg.incrafttime.top/api/payload?size=32768",
        "https://gg.incrafttime.top/api/payload?token=a%2Bb&size=128684&mode=raw",
        "https://gg.incrafttime.top/api/payload?mode=raw&size=64342&token=a%2Bb",
        "https://custom.example/api/payload?size=128684&token=unchanged",
        "https://gg.incrafttime.top.evil.example/api/payload?size=128684",
        "https://custom.example/gg.incrafttime.top/api/payload?size=128684",
        "https://custom.example/?next=https://gg.incrafttime.top/api/payload&size=64342",
        "https://gg.incrafttime.top/",
        "https://gg.incrafttime.top",
    };
    for (const auto& url : urls) {
        // 保存、NVS 持久化、重启载入与运行视图必须保留用户的完整 URL。
        assert(idf_config_save_keepalive(true, 175, 3, "10086", url, "profile-a") == ESP_OK);
        assert(nvs_strings.at("kaUrl") == url + '\0');
        assert_view(url);
        reload_keepalive();
        assert_view(url);
        assert(nvs_strings.at("kaUrl") == url + '\0');
    }

    // 空输入与未配置设备仍使用现有默认地址。
    assert(idf_config_save_keepalive(true, 175, 3, "10086", "", "profile-a") == ESP_OK);
    assert(nvs_strings.at("kaUrl") == std::string(IDF_KEEPALIVE_DEFAULT_URL) + '\0');
    assert_view(IDF_KEEPALIVE_DEFAULT_URL);
    reload_keepalive();
    assert_view(IDF_KEEPALIVE_DEFAULT_URL);
    nvs_strings.erase("kaUrl");
    reload_keepalive();
    assert_view(IDF_KEEPALIVE_DEFAULT_URL);

    // 写入失败时，已有 URL 不能在内存或持久化存储中被替换。
    assert(idf_config_save_keepalive(true, 175, 3, "10086", urls[0], "profile-a") == ESP_OK);
    fail_commit = true;
    assert(idf_config_save_keepalive(true, 175, 3, "10086", urls[1], "profile-a") == ESP_FAIL);
    assert_view(urls[0]);
    assert(nvs_strings.at("kaUrl") == urls[0] + '\0');
    reload_keepalive();
    assert_view(urls[0]);
    std::puts("Keepalive config persistence tests passed");
}
