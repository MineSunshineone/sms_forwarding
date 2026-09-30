#!/usr/bin/env python3
"""为 ESP-IDF 应用仓库维护的 SDK patch。"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


# 每个 patch 的目标目录相对于 ESP-IDF 根目录；Mbed TLS 是独立子模块。
PATCHES = (
    (Path("components/mbedtls/mbedtls"), Path(
        "patches/mbedtls/0001-x509-parse-all-certificate-policy-oids.patch"
    )),
    (Path("."), Path(
        "patches/esp-idf/0001-crt-bundle-cross-signed-memory-leak.patch"
    )),
)


def git_apply_check(repo: Path, patch: Path, reverse: bool = False) -> tuple[bool, str]:
    args = ["apply", "--check", "--whitespace=error"]
    if reverse:
        args.append("--reverse")
    args.append(str(patch))
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    detail = (result.stderr or result.stdout).strip()
    return result.returncode == 0, detail


def resolve_idf_path(value: str | None) -> Path:
    if value:
        return Path(value).expanduser().resolve()
    env_value = os.environ.get("IDF_PATH")
    if env_value:
        return Path(env_value).expanduser().resolve()
    raise RuntimeError("未提供 --idf-path，且环境变量 IDF_PATH 未设置")


def apply_one_patch(target: Path, patch_path: Path, check_only: bool) -> None:
    if not target.is_dir():
        raise RuntimeError(f"SDK patch 目标目录不存在: {target}")
    if not patch_path.is_file():
        raise RuntimeError(f"项目 patch 不存在: {patch_path}")

    # 不绑定外部 SDK 的 Git commit；只要求 patch 上下文完整匹配。
    can_apply, apply_detail = git_apply_check(target, patch_path)
    if can_apply:
        if check_only:
            print(f"patch 可应用: {patch_path}")
            return

        result = subprocess.run(
            [
                "git",
                "-C",
                str(target),
                "apply",
                "--whitespace=error",
                str(patch_path),
            ],
            check=False,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError("应用 ESP-IDF patch 失败")
        print(f"已应用 patch: {patch_path}")
        return

    already_applied, reverse_detail = git_apply_check(
        target, patch_path, reverse=True
    )
    if already_applied:
        print(f"patch 已经应用: {patch_path}")
        return

    detail = apply_detail or reverse_detail or "无详细错误"
    raise RuntimeError(
        "patch 既不能正向应用，也不能识别为已应用状态；"
        "拒绝 fuzzy/部分应用。\n"
        f"patch: {patch_path}\n{detail}"
    )


def apply_patch(repo_root: Path, idf_path: Path, check_only: bool) -> None:
    if not idf_path.is_dir():
        raise RuntimeError(f"ESP-IDF 路径不存在: {idf_path}")
    # 先检查全部 patch，避免上下文不匹配时留下部分打补丁的 SDK。
    for target, patch in PATCHES:
        apply_one_patch(idf_path / target, repo_root / patch, True)
    if not check_only:
        for target, patch in PATCHES:
            apply_one_patch(idf_path / target, repo_root / patch, False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", help="ESP-IDF 根目录；默认读取 IDF_PATH")
    parser.add_argument(
        "--check-only",
        action="store_true",
        help="只检查 patch 状态，不修改 SDK",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    try:
        idf_path = resolve_idf_path(args.idf_path)
        apply_patch(repo_root, idf_path, args.check_only)
    except RuntimeError as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
