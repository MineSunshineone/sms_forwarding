#!/usr/bin/env python3
"""编译真实蜂窝 HTTP 解析/生命周期函数，使用模拟 UART/AT 做主机回归。"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/idf_modem/idf_modem.cpp').read_text()
header = (ROOT / 'components/idf_modem/include/idf_modem.h').read_text()
def function(name):
    start = source.rfind('\n', 0, source.index(name)) + 1
    return source[start:source.index('\n}', start) + 2]
fixture = (ROOT / 'tests/cellular_http_test.cpp').read_text()
fixture = fixture.replace('// @结果结构@', header[header.index('struct IdfCellularHttpResult'):header.index('esp_err_t idf_modem_start')])
fixture = fixture.replace('// @短信接收函数@', '\n'.join(function(name) for name in ['looks_like_pdu_line(', 'preserve_uart_urc_line(', 'preserve_uart_urcs(']))
fixture = fixture.replace('// @解析函数@', '\n'.join(function(name) for name in [
    'parse_long_token(', 'parse_comma_longs(', 'starts_with(', 'mhttp_error_detail(',
    'parse_mhttp_head(', 'comma_count(', 'wait_mhttp_download_locked(',
    'restore_cellular_data_locked(', 'parse_http_url(', 'apn_valid_for_at(',
    'normalize_keepalive_payload_size(', 'append_no_cache_query(',
]))
fixture = fixture.replace('// @实际HTTP函数@', '\n'.join(function(name) for name in ['hex_nibble(', 'hex_encode_ascii(', 'parse_mhttp_create_id(', 'send_mhttp_header_locked(']) + '\n' + function('fetch_mhttp_once_locked(').replace('fetch_mhttp_once_locked(', 'real_fetch_mhttp_once_locked(', 1))
fixture = fixture.replace('// @生命周期函数@', function('cellular_http_request_impl('))
with tempfile.TemporaryDirectory(prefix='test-cellular-') as temp:
    cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
