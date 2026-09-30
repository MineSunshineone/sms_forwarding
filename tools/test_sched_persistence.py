#!/usr/bin/env python3
"""注入 NVS 写失败，验证真实执行标记函数仍抑制当前运行中的收费短信重试风暴。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'components/idf_config/idf_config.cpp').read_text()
start = source.index('esp_err_t idf_config_set_sched_last(')
end = source.index('\nesp_err_t idf_config_set_net_led_enabled', start)
function = source[start:end]
fixture = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include "scheduled_task_time.h"
using esp_err_t = int;
using nvs_handle_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_ARG=1, ESP_ERR_TIMEOUT=2, NVS_READWRITE=1;
constexpr int IDF_MAX_SCHED_TASKS=6, portMAX_DELAY=0, pdTRUE=1;
int s_persist_mutex=0, s_config_mutex=0, fail_stage=0, closed=0;
struct Task { uint32_t lastRun=0; };
struct { Task schedTasks[IDF_MAX_SCHED_TASKS]; } s_config;
int ensure_config_mutex() { return ESP_OK; }
int xSemaphoreTake(int, int) { return pdTRUE; }
void xSemaphoreGive(int) {}
int nvs_open(const char*, int, int*) { return fail_stage == 1 ? 9 : ESP_OK; }
int nvs_set_u32(int, const char*, uint32_t) { return fail_stage == 2 ? 9 : ESP_OK; }
int nvs_commit(int) { return fail_stage == 3 ? 9 : ESP_OK; }
void nvs_close(int) { ++closed; }
void idf_logf(const char*, ...) {}
const char* esp_err_to_name(int) { return "mock failure"; }
// @真实函数@
int main() {
  const uint32_t now = 1789344000;
  for (fail_stage=0; fail_stage<=3; ++fail_stage) {
    s_config.schedTasks[0].lastRun = now - 7 * 86400;
    assert(idf_scheduled_task_due(s_config.schedTasks[0].lastRun, now, 7, -1, 0));
    const int error = idf_config_set_sched_last(0, now);
    assert((error == ESP_OK) == (fail_stage == 0));
    assert(s_config.schedTasks[0].lastRun == now);
    for (uint32_t seconds=5; seconds<=3600; seconds+=5)
      assert(!idf_scheduled_task_due(s_config.schedTasks[0].lastRun, now+seconds, 7, -1, 0));
    // 失败动作的次日重试基准也必须在写盘失败时生效。
    const uint32_t retry_base = now - 6 * 86400;
    idf_config_set_sched_last(0, retry_base);
    assert(!idf_scheduled_task_due(s_config.schedTasks[0].lastRun, now+5, 7, -1, 0));
    assert(idf_scheduled_task_due(s_config.schedTasks[0].lastRun, now+86400, 7, -1, 0));
  }
  assert(idf_config_set_sched_last(-1, now) == ESP_ERR_INVALID_ARG);
  assert(idf_config_set_sched_last(6, now) == ESP_ERR_INVALID_ARG);
  assert(closed == 6);
  std::puts("scheduled task NVS failure regressions passed");
}
'''.replace('// @真实函数@', function)
with tempfile.TemporaryDirectory(prefix='sms-persist-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    binary = Path(tmp) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'components/idf_web'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
