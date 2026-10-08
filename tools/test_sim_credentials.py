#!/usr/bin/env python3
"""在主机编译真实配置函数，回归十六进制 ICCID 的读取、保存和解锁匹配。"""
from pathlib import Path
import os
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/idf_config/idf_config.cpp').read_text()


def function(name, text=source):
    pattern = rf'^(?:static )?[\w:<>]+\s+{re.escape(name)}\([^;]*?\)\s*\n\{{'
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f'未找到配置函数: {name}')
    end = text.index('\n}', match.end()) + 2
    return text[match.start():end]


functions = '\n\n'.join(function(name) for name in [
    'read_str', 'read_iccid', 'read_u8', 'write_str',
    'clamp_int', 'limit_utf8_bytes', 'digits_only', 'sanitize_sim_credentials',
    'config_snapshot', 'begin_field_save', 'commit_field_save',
    'idf_config_get_sim_settings_view', 'idf_config_get_sim_unlock_view',
    'idf_config_save_sim', 'idf_config_record_sim_unlock_result',
])
modem_source = (ROOT / 'components/idf_modem/idf_modem.cpp').read_text()
functions += '\n\n' + '\n\n'.join(function(name, modem_source) for name in [
    'is_imei_text', 'read_nvs_string', 'save_identity_cache',
])

# 保留真实 NVS 载入循环，防止调用方又改回会截断凭据主键的通用读取器。
load_start = source.rfind('        for (', 0, source.index('IdfSimCredential& item = next.simCredentials[i];'))
load_end = source.index('\n        }', load_start) + len('\n        }')
fixture = (ROOT / 'tests/sim_credentials_test.cpp').read_text()
fixture = fixture.replace('// @配置函数@', functions)
fixture = fixture.replace('// @载入片段@', source[load_start:load_end])

with tempfile.TemporaryDirectory(prefix='sms-sim-credentials-') as temp:
    directory = Path(temp)
    (directory / 'esp_err.h').write_text('''#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 1;
constexpr int ESP_ERR_NOT_FOUND = 2, ESP_ERR_TIMEOUT = 3;
constexpr int ESP_ERR_NVS_NOT_FOUND = 4, ESP_ERR_NVS_INVALID_LENGTH = 5;
''')
    cpp = directory / 'test.cpp'
    binary = directory / 'test'
    cpp.write_text(fixture)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                    '-Wall', '-Wextra', '-Werror', '-I', str(directory),
                    '-I', str(ROOT / 'components/idf_config/include'),
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
