#!/usr/bin/env python3
"""在主机编译真实保号配置函数，回归 URL 保存、NVS 重载与运行视图。"""
from pathlib import Path
import os
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/idf_config/idf_config.cpp').read_text()


def function(name):
    pattern = rf'^(?:static )?[\w:<>]+\s+{re.escape(name)}\([^;]*?\)\s*\n\{{'
    match = re.search(pattern, source, re.MULTILINE)
    if match is None:
        raise RuntimeError(f'未找到配置函数: {name}')
    end = source.index('\n}', match.end()) + 2
    return source[match.start():end]


functions = '\n\n'.join(function(name) for name in [
    'read_str', 'read_i32', 'read_u32', 'read_u8', 'read_bool', 'write_str',
    'limit_utf8_bytes', 'trim_copy', 'begin_field_save', 'commit_field_save',
    'idf_config_save_keepalive', 'idf_config_get_keepalive_run_view',
])
# 包含整个保号载入区间，防止未来新增的 URL 改写绕过回归测试。
load_start = source.index('        next.kaEnabled =')
load_end = source.index('        next.tzOffsetMin =', load_start)
fixture = (ROOT / 'tests/keepalive_config_test.cpp').read_text()
fixture = fixture.replace('// @配置函数@', functions)
fixture = fixture.replace('// @载入片段@', source[load_start:load_end])

with tempfile.TemporaryDirectory(prefix='sms-keepalive-config-') as temp:
    directory = Path(temp)
    (directory / 'esp_err.h').write_text('''#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_TIMEOUT = 1;
constexpr int ESP_ERR_NVS_NOT_FOUND = 2, ESP_ERR_NVS_INVALID_LENGTH = 3;
''')
    cpp = directory / 'test.cpp'
    binary = directory / 'test'
    cpp.write_text(fixture)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                    '-Wall', '-Wextra', '-Werror', '-I', str(directory),
                    '-I', str(ROOT / 'components/idf_config/include'),
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
