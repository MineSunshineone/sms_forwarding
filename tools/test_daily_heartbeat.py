#!/usr/bin/env python3
"""编译并运行每日心跳主机回归测试，不依赖 ESP-IDF 或模组硬件。"""

from pathlib import Path
import os
import shlex
import subprocess


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "host"


def main() -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    binary = BUILD / "test_daily_heartbeat"
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    subprocess.run(
        compiler
        + [
            "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(ROOT / "components" / "idf_web" / "include"),
            str(ROOT / "tests" / "host" / "test_daily_heartbeat.cpp"),
            "-o", str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
