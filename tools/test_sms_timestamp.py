#!/usr/bin/env python3
"""编译实际 SCTS 时间转换头文件，验证时区符号、非法日期与旧短信时间。"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="sms-timestamp-") as tmp:
        output = Path(tmp) / "test"
        subprocess.run([
            *shlex.split(os.environ.get("CXX", "c++")),
            "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I" + str(root / "components/idf_sms/include"),
            str(root / "tools/tests/test_sms_timestamp.cpp"), "-o", str(output)
        ], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    main()
