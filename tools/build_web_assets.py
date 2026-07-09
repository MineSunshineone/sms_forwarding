#!/usr/bin/env python3
"""生成 ESP32 Web UI 的 gzip 静态资源。

源文件位于 code/web_src/，本脚本输出 code/web_assets.h/.cpp。
设备端只发送 gzip 字节，浏览器负责解压；不要在 ESP32 上运行压缩。
生成文件不依赖运行时 API，由 ESP-IDF 的 web_assets 组件链接。
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "code" / "web_src"
OUT_H = ROOT / "code" / "web_assets.h"
OUT_CPP = ROOT / "code" / "web_assets.cpp"
PANELS = [
    "overview",
    "sim",
    "inbox",
    "settings",
    "push",
    "keepalive",
    "diagnose",
    "atterm",
    "log",
]


@dataclass(frozen=True)
class Asset:
    var: str
    route_name: str
    mime: str
    source: Path
    content: bytes
    gz: bytes
    etag: str


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def minify_css(text: str) -> str:
    text = re.sub(r"/\*[\s\S]*?\*/", "", text)
    text = re.sub(r"\s+", " ", text)
    text = re.sub(r"\s*([{}:;,>+~])\s*", r"\1", text)
    text = text.replace(";}", "}")
    return text.strip()


def minify_markup(text: str) -> str:
    text = re.sub(r"<!--[\s\S]*?-->", "", text)
    lines = [line.strip() for line in text.splitlines()]
    return "\n".join(line for line in lines if line)


def minify_js(text: str) -> str:
    # 保守处理：不删 // 注释，避免误伤字符串里的 URL；gzip 会吃掉大部分重复空白。
    lines = [line.rstrip() for line in text.splitlines()]
    return "\n".join(line for line in lines if line.strip())


def gzip_bytes(data: bytes) -> bytes:
    buf = io.BytesIO()
    # gzip.compress(..., mtime=0) 在部分 Python 版本会写入平台相关 OS 字节。
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as gz:
        gz.write(data)
    return buf.getvalue()


def asset_hash(source_texts: list[str]) -> str:
    h = hashlib.sha256()
    for text in source_texts:
        h.update(text.replace("{{ASSET_HASH}}", "__WEB_ASSET_HASH__").encode("utf-8"))
        h.update(b"\0")
    return h.hexdigest()[:12]


def make_asset(var: str, route_name: str, mime: str, source: Path, text: str, rev: str) -> Asset:
    text = text.replace("{{ASSET_HASH}}", rev)
    if source.suffix == ".css":
        text = minify_css(text)
    elif source.suffix == ".js":
        text = minify_js(text)
    else:
        text = minify_markup(text)
    content = text.encode("utf-8")
    gz = gzip_bytes(content)
    etag = hashlib.sha256(gz).hexdigest()[:16]
    return Asset(var, route_name, mime, source, content, gz, etag)


def c_array(data: bytes) -> str:
    rows = []
    for i in range(0, len(data), 16):
        rows.append("  " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    return "\n".join(rows)


def build_assets() -> tuple[str, str]:
    raw_sources = [
        read_text(SRC / "index.html"),
        read_text(SRC / "app.css"),
        read_text(SRC / "app.js"),
        read_text(SRC / "ap.html"),
    ]
    raw_sources.extend(read_text(SRC / "panels" / f"{name}.html") for name in PANELS)
    rev = asset_hash(raw_sources)

    assets: list[Asset] = [
        make_asset("WEB_INDEX", "index", "text/html", SRC / "index.html", raw_sources[0], rev),
        make_asset("WEB_APP_CSS", "app.css", "text/css", SRC / "app.css", raw_sources[1], rev),
        make_asset("WEB_APP_JS", "app.js", "application/javascript", SRC / "app.js", raw_sources[2], rev),
        # 配网热点专用独立页(自包含，AP 模式下由 handle_root 单独下发)
        make_asset("WEB_AP", "ap", "text/html", SRC / "ap.html", raw_sources[3], rev),
    ]
    for name, text in zip(PANELS, raw_sources[4:]):
        assets.append(make_asset(f"WEB_PANEL_{name.upper()}", name, "text/html", SRC / "panels" / f"{name}.html", text, rev))

    header = f"""#ifndef WEB_ASSETS_H
#define WEB_ASSETS_H

#include <stddef.h>
#include <stdint.h>

struct WebAsset {{
  const uint8_t* data;
  size_t length;
  const char* mime;
  const char* etag;
}};

extern const char WEB_ASSET_HASH[];
extern const WebAsset WEB_INDEX;
extern const WebAsset WEB_APP_CSS;
extern const WebAsset WEB_APP_JS;
extern const WebAsset WEB_AP;

const WebAsset* findWebPanelAsset(const char* name);

#endif
"""

    cpp_parts = [
        '#include "web_assets.h"',
        "#include <string.h>",
        "#if defined(ARDUINO)",
        "#include <pgmspace.h>",
        "#define WEB_ASSET_STORAGE PROGMEM",
        "#else",
        "#define WEB_ASSET_STORAGE",
        "#endif",
        "",
        "// 此文件由 tools/build_web_assets.py 生成；请修改 code/web_src/ 后重新生成。",
        f'const char WEB_ASSET_HASH[] = "{rev}";',
        "",
    ]
    for asset in assets:
        arr_name = f"{asset.var}_DATA"
        cpp_parts.append(f"static const uint8_t {arr_name}[] WEB_ASSET_STORAGE = {{")
        cpp_parts.append(c_array(asset.gz))
        cpp_parts.append("};")
        cpp_parts.append(
            f'const WebAsset {asset.var} = {{ {arr_name}, sizeof({arr_name}), "{asset.mime}", "\\"{asset.etag}\\"" }};'
        )
        cpp_parts.append("")

    cpp_parts.append("const WebAsset* findWebPanelAsset(const char* name) {")
    cpp_parts.append("  if (!name) return nullptr;")
    for name in PANELS:
        cpp_parts.append(f'  if (strcmp(name, "{name}") == 0) return &WEB_PANEL_{name.upper()};')
    cpp_parts.append("  return nullptr;")
    cpp_parts.append("}")
    cpp = "\n".join(cpp_parts) + "\n"
    return header, cpp


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="只检查生成文件是否最新")
    args = parser.parse_args()

    header, cpp = build_assets()
    if args.check:
        old_h = OUT_H.read_text(encoding="utf-8") if OUT_H.exists() else ""
        old_cpp = OUT_CPP.read_text(encoding="utf-8") if OUT_CPP.exists() else ""
        if old_h != header or old_cpp != cpp:
            print("web assets are out of date; run: python tools/build_web_assets.py", file=sys.stderr)
            return 1
        return 0

    OUT_H.write_text(header, encoding="utf-8")
    OUT_CPP.write_text(cpp, encoding="utf-8")
    print(f"generated {OUT_H.relative_to(ROOT)} and {OUT_CPP.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
