"""SDK 补丁应用的主机回归测试，不需要安装 ESP-IDF。"""

import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("apply_idf_patches", ROOT / "tools/apply_idf_patches.py")
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)


def patch_preimages(patch):
    """从 unified diff 构造精确上下文；未涉及的行使用占位文本。"""
    files = {}
    lines = patch.read_text(encoding="utf-8").splitlines(keepends=True)
    current = None
    pos = 0
    for line in lines:
        if line.startswith("--- a/"):
            current = line[6:].strip()
            files[current] = []
        elif line.startswith("@@ "):
            pos = int(re.match(r"@@ -(\d+)", line).group(1)) - 1
            while len(files[current]) < pos:
                files[current].append(f"/* 未修改的行 {len(files[current]) + 1} */\n")
        elif current and line[:1] in (" ", "-"):
            if pos == len(files[current]):
                files[current].append(line[1:])
            else:
                assert files[current][pos] == line[1:]
            pos += 1
    return {name: "".join(content) for name, content in files.items()}


class PatchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.idf = Path(self.tmp.name)
        subprocess.run(["git", "init", "-q", str(self.idf)], check=True)
        self.sources = []
        for target, patch in patcher.PATCHES:
            target = self.idf / target
            target.mkdir(parents=True, exist_ok=True)
            # SDK 子模块有独立 Git 根目录，和真实 ESP-IDF 保持一致。
            if target != self.idf:
                subprocess.run(["git", "init", "-q", str(target)], check=True)
            for name, content in patch_preimages(ROOT / patch).items():
                path = target / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
                self.sources.append(path)

    def snapshot(self):
        return {str(p): p.read_bytes() for p in self.sources}

    def test_check_only_does_not_modify(self):
        before = self.snapshot()
        patcher.apply_patch(ROOT, self.idf, True)
        self.assertEqual(before, self.snapshot())

    def test_all_patches_apply_and_are_idempotent(self):
        before = self.snapshot()
        patcher.apply_patch(ROOT, self.idf, False)
        after = self.snapshot()
        self.assertNotEqual(before, after)
        for target, patch in patcher.PATCHES:
            self.assertTrue(patcher.git_apply_check(self.idf / target, ROOT / patch, reverse=True)[0])
        patcher.apply_patch(ROOT, self.idf, False)
        patcher.apply_patch(ROOT, self.idf, True)
        self.assertEqual(after, self.snapshot())

    def test_mixed_applied_and_unapplied(self):
        target, patch = patcher.PATCHES[0]
        patcher.apply_one_patch(self.idf / target, ROOT / patch, False)
        patcher.apply_patch(ROOT, self.idf, False)
        for target, patch in patcher.PATCHES:
            self.assertTrue(patcher.git_apply_check(self.idf / target, ROOT / patch, reverse=True)[0])

    def test_bad_second_patch_does_not_apply_first(self):
        self.sources[-1].write_text("incompatible SDK\n", encoding="utf-8")
        before = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, "拒绝 fuzzy/部分应用"):
            patcher.apply_patch(ROOT, self.idf, False)
        self.assertEqual(before, self.snapshot())

    def test_partially_applied_patch_is_rejected(self):
        path = self.sources[-1]
        source = path.read_text(encoding="utf-8")
        path.write_text(source.replace("2018-2025 Espressif", "2018-2026 Espressif"), encoding="utf-8")
        before = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, "拒绝 fuzzy/部分应用"):
            patcher.apply_patch(ROOT, self.idf, False)
        self.assertEqual(before, self.snapshot())

    def test_missing_sdk(self):
        with self.assertRaisesRegex(RuntimeError, "ESP-IDF 路径不存在"):
            patcher.apply_patch(ROOT, self.idf / "missing", False)

    def test_missing_patch(self):
        with self.assertRaisesRegex(RuntimeError, "项目 patch 不存在"):
            patcher.apply_one_patch(self.idf, self.idf / "missing.patch", False)

    def test_actual_sdk_when_available(self):
        # CI 镜像提供 v6.0.2 源码；检查真实 SDK，上述测试只使用合成上下文。
        import os
        if not os.environ.get("IDF_PATH"):
            self.skipTest("未安装 ESP-IDF；真实源码应用由固件 CI 验证")
        patcher.apply_patch(ROOT, Path(os.environ["IDF_PATH"]), True)


if __name__ == "__main__":
    unittest.main()
