#!/usr/bin/env python3
"""check_tooth.py —— 校验 tooth.json（Lip 包清单）与仓库实际产物一致。

`format_uuid` 是格式标记，不是每个包自己的 UUID。Lip 官方 schema 把它固定为常量
`289f771f-2c9a-4d73-9f3f-8492495a924d`：格式 3 的每个包都必须是这个值。
包的身份由 `tooth` 字段（仓库路径）标识。

Lip 按 tooth.json 下载 GitHub Release 资产并解压到目标目录。以下四处一旦对不上，
`lip install` 会装上残缺的包或落错目录，所以放进校验：

  1. format_version / format_uuid 是格式常量
  2. version 与 manifest.json 一致（Release tag 是 v<version>）
  3. 资产 URL 指向 MeowSidebar.zip，且本地 bin/MeowSidebar.zip 的根目录是 MeowSidebar/
  4. 放置规则 MeowSidebar/ -> plugins/MeowSidebar 与实际布局一致

用法:
  python tools/check_tooth.py
"""
from __future__ import annotations

import json
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOTH = os.path.join(ROOT, "tooth.json")
MANIFEST = os.path.join(ROOT, "manifest.json")
LOCAL_ZIP = os.path.join(ROOT, "bin", "MeowSidebar.zip")

EXPECTED = {
    "format_version": 3,
    "format_uuid": "289f771f-2c9a-4d73-9f3f-8492495a924d",
    "tooth": "github.com/Ieckseyr/MeowSidebar",
    "zip_name": "MeowSidebar.zip",
    "zip_root": "MeowSidebar/",
    "dest": "plugins/MeowSidebar",
    "platform": "win-x64",
}


def fail(msg: str) -> int:
    print("[tooth] " + msg)
    return 1


def main() -> int:
    if not os.path.isfile(TOOTH):
        return fail("缺少 tooth.json")

    try:
        tooth = json.load(open(TOOTH, encoding="utf-8"))
        manifest = json.load(open(MANIFEST, encoding="utf-8"))
    except Exception as err:  # noqa: BLE001 - 逐条报出便于定位
        return fail(f"清单不是合法 JSON: {err}")

    checks = [
        ("format_version", tooth.get("format_version"), EXPECTED["format_version"]),
        ("format_uuid", tooth.get("format_uuid"), EXPECTED["format_uuid"]),
        ("tooth", tooth.get("tooth"), EXPECTED["tooth"]),
        ("version（应与 manifest.json 一致）", tooth.get("version"), manifest.get("version")),
        ("info.name", (tooth.get("info") or {}).get("name"), manifest.get("name")),
    ]
    for label, got, want in checks:
        if got != want:
            return fail(f"{label} 不一致：tooth={got!r}，应为 {want!r}")

    variants = tooth.get("variants") or []
    if len(variants) != 1:
        return fail(f"variants 应为 1 个（仅服务端 win-x64），当前 {len(variants)} 个")

    variant = variants[0]
    if variant.get("label"):
        return fail("服务端变体不应带 label")
    if variant.get("platform") != EXPECTED["platform"]:
        return fail(f"platform 应为 {EXPECTED['platform']}，当前 {variant.get('platform')!r}")

    deps = variant.get("dependencies") or {}
    for dep in ("github.com/LiteLDev/LeviLamina", "github.com/Ieckseyr/HologramLib"):
        if dep not in deps:
            return fail(f"缺少依赖 {dep}")

    assets = variant.get("assets") or []
    if len(assets) != 1 or assets[0].get("type") != "zip":
        return fail("assets 应为 1 个 zip")

    urls = assets[0].get("urls") or []
    if len(urls) != 1:
        return fail("assets.urls 应为 1 条")
    if urls[0] != "https://{{tooth}}/releases/download/v{{version}}/" + EXPECTED["zip_name"]:
        return fail(f"资产地址应为 releases/download/v{{{{version}}}}/{EXPECTED['zip_name']}，当前 {urls[0]!r}")

    placements = assets[0].get("placements") or []
    if len(placements) != 1:
        return fail("placements 应为 1 条")
    place = placements[0]
    if place.get("type") != "dir":
        return fail("placements.type 应为 dir")
    if place.get("src") != EXPECTED["zip_root"]:
        return fail(f"placements.src 应为 {EXPECTED['zip_root']}，当前 {place.get('src')!r}")
    if (place.get("dest") or "").replace("\\", "/").rstrip("/") != EXPECTED["dest"]:
        return fail(f"placements.dest 应为 {EXPECTED['dest']}，当前 {place.get('dest')!r}")

    # 本地有发布包时顺带校验解压后的根目录（CI 里由构建产物提供，本地可没有）
    if os.path.isfile(LOCAL_ZIP):
        with zipfile.ZipFile(LOCAL_ZIP) as zf:
            names = zf.namelist()
        roots = {name.split("/", 1)[0] + "/" for name in names if "/" in name}
        if roots != {EXPECTED["zip_root"]}:
            return fail(f"{EXPECTED['zip_name']} 根目录应为 {EXPECTED['zip_root']}，当前 {sorted(roots)}")
        if EXPECTED["zip_root"] + "manifest.json" not in names:
            return fail(f"{EXPECTED['zip_name']} 里缺少 {EXPECTED['zip_root']}manifest.json")

    print("[tooth] OK: tooth.json / manifest.json / 发布包布局一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
