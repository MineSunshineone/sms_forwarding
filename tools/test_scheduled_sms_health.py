#!/usr/bin/env python3
"""编译并运行实际生产头文件的短信体检/定时回归，无需 ESP-IDF 或真机。"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="sms-health-") as tmp:
    output = Path(tmp) / "test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(root / "components/idf_sms/include"),
                    "-I" + str(root / "components/idf_web"),
                    str(root / "tools/tests/test_scheduled_sms_health.cpp"), "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)
