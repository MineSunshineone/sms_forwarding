#!/usr/bin/env python3
"""在主机上编译真实模组函数和调度片段，回归慢卡读取与刷新排队行为。"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/idf_modem/idf_modem.cpp').read_text()


def function(name):
    start = source.rfind('\n', 0, source.index(name)) + 1
    end = source.index('\n}', start) + 2
    return source[start:end]


functions = '\n\n'.join(function(name) for name in [
    'line_containing(', 'is_iccid_text(',
    'parse_iccid_response(', 'parse_iccid_crsm_response(',
    'log_identity_response_shape(', 'query_current_iccid(', 'parse_cops(', 'cops_display(', 'idf_modem_get_operator(', 'query_operator_from_modem(',
])
start = source.index('        bool force_sample =')
end = source.index('        // 正常态', start)
sampling = source[start:end]
fixture = (ROOT / 'tests/modem_refresh_test.cpp').read_text()
fixture = fixture.replace('// @查询函数@', functions)
fixture = fixture.replace('// @采样代码@', sampling)
web = (ROOT / 'components/idf_web/idf_web.cpp').read_text()
start = web.index('        esp_err_t err =', web.index('} else if (action == "operator")'))
end = web.index('    } else if (action == "imei")', start)
fixture = fixture.replace('// @手动运营商查询@', web[start:end])
(ROOT / 'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='test-modem-', dir=ROOT / 'build') as temp:
    cpp = Path(temp) / 'test.cpp'
    binary = Path(temp) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'components/idf_modem/include'),
                    '-I', str(ROOT / 'components/idf_config/include'),
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
