#!/usr/bin/env python3
"""仅供 staging CI：安全地运行一次性补丁引导器；最终提交前会恢复原生成器。"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "tools" / "stage_apply.py"

subprocess.run(["git", "config", "--global", "--add", "safe.directory", str(ROOT)], check=True)
spec = importlib.util.spec_from_file_location("stage_apply", HELPER)
if spec is None or spec.loader is None:
    raise RuntimeError("无法加载 staging helper")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.ROOT = ROOT
HELPER.unlink()
raise SystemExit(module.main())
