# -*- coding: utf-8 -*-
"""LevelSet01 の FBX をプロジェクトへ登録する（編集器なしで実行できる）。

- Exports/*.fbx を Assets/Models/LevelSet01/ へ複写し、資産メタを ProjectSettings/AssetMeta に作る。
- GUID は "LevelSet01/<部品名>" の MD5 から決める。再実行しても同じ GUID になり、重複資産を作らない。
- 取り込み（HMODEL 化）は次回のエンジン起動時（編集器または hikari headless）にエンジン自身が行う。
- 既存のメタがあれば GUID と取り込み設定を保ち、ソースだけ更新する。
実行: python install_to_project.py
"""

import hashlib
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent
DEST = PROJECT / "Assets" / "Models" / "LevelSet01"
META = PROJECT / "ProjectSettings" / "AssetMeta" / "Assets" / "Models" / "LevelSet01"
# 既存の工厂部品と同じ取り込み設定を使う（場景用クラスタ、ライトマップ UV 自動生成）。
TEMPLATE = PROJECT / "ProjectSettings" / "AssetMeta" / "Assets" / "Models" / "FactorySet01" / "SM_F_Desk.fbx.hikari.asset.json"


def guid_for(name: str) -> str:
    return hashlib.md5(f"LevelSet01/{name}".encode("utf-8")).hexdigest()


def main() -> None:
    template = json.loads(TEMPLATE.read_text(encoding="utf-8-sig"))
    DEST.mkdir(parents=True, exist_ok=True)
    META.mkdir(parents=True, exist_ok=True)
    guids = {}
    for fbx in sorted((ROOT / "Exports").glob("*.fbx")):
        name = fbx.stem
        target = DEST / fbx.name
        changed = not target.exists() or target.read_bytes() != fbx.read_bytes()
        if changed:
            shutil.copyfile(fbx, target)
        meta_path = META / f"{fbx.name}.hikari.asset.json"
        if meta_path.exists():
            meta = json.loads(meta_path.read_text(encoding="utf-8-sig"))
        else:
            meta = {"displayName": name, "guid": guid_for(name), "importSettings": template["importSettings"],
                    "importerId": template["importerId"], "importerVersion": template["importerVersion"],
                    "metaVersion": template["metaVersion"], "sourcePath": f"Assets/Models/LevelSet01/{fbx.name}",
                    "type": "Model"}
            meta_path.write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        guids[name] = meta["guid"]
        print(("updated " if changed else "same    ") + name, meta["guid"])
    (ROOT / "Temp").mkdir(exist_ok=True)
    (ROOT / "Temp" / "installed_guids.json").write_text(json.dumps(guids, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
