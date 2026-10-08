#!/usr/bin/env python3
"""以分块 UART 假设备执行真实 AT/PDU 收发函数，防止尾随 OK 串入身份查询。"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/idf_modem/idf_modem.cpp').read_text()

def function(name):
    start = source.rfind('\n', 0, source.index(name)) + 1
    return source[start:source.index('\n}', start) + 2]

names = ['at_final_result(', 'has_cmgs_result(', 'capture_pending_uart_locked(',
         'esp_err_t idf_modem_send_at(', 'esp_err_t idf_modem_send_pdu(']
functions = '\n\n'.join(function(name) for name in names)
a = source.index('struct TickDeadline {')
functions = source[a:source.index('\n};', a) + 3] + '\n' + functions
fixture = (ROOT / 'tests/modem_uart_test.cpp').read_text().replace('// @收发函数@', functions)
with tempfile.TemporaryDirectory(prefix='modem-uart-') as temp:
    cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
