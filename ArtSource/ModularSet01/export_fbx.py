# -*- coding: utf-8 -*-
"""既存の部品を変更せず、メートル単位・Y-up の独立 FBX を追加する。"""

import json
from pathlib import Path

import bpy
from mathutils import Matrix
from io_scene_fbx import parse_fbx

ROOT = Path(__file__).resolve().parent
assert bpy.app.background, "Use a separate background Blender process"
manifest = json.loads((ROOT / "asset_manifest.json").read_text(encoding="utf-8"))
entries = {entry["id"]: entry for entry in manifest["assets"]}
priority = ["SM_TV_16x9", "SM_Cabinet_Low", "SM_Wall_2m", "SM_Floor_2m", "SM_Lamp_Floor",
            "SM_TV_4x3", "SM_Plinth_Tall", "SM_Bench", "SM_Shelf_Small"]
bpy.ops.wm.open_mainfile(filepath=str(ROOT / "ModularSet01.blend"))

for identifier in priority:
    entry = entries[identifier]
    source = bpy.data.scenes[identifier]
    export_scene = bpy.data.scenes.new(identifier + "__FBXExportOnly")
    export_scene.unit_settings.system = "METRIC"
    export_scene.unit_settings.scale_length = 1.0
    bpy.context.window.scene = export_scene
    for source_obj in source.objects:
        if source_obj.name not in entry["mesh_names"]:
            continue
        # 空の親ノード由来の軸補正を避け、同じ底面原点の mesh だけを出力する。
        obj = source_obj.copy()
        obj.data = source_obj.data.copy()
        obj.parent = None
        obj.matrix_world = Matrix.Identity(4)
        export_scene.collection.objects.link(obj)
        source_obj.name = identifier + "__Source_" + source_obj.name.rsplit("__", 1)[-1]
        obj.name = entry["screen"]["mesh"] if entry["screen"] and source_obj.name.endswith("Screen") else identifier + "__Body"
        obj.data.name = obj.name
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
    path = ROOT / "Exports" / (identifier + ".fbx")
    bpy.ops.export_scene.fbx(filepath=str(path), use_selection=True, object_types={"MESH"},
                            global_scale=1.0, apply_unit_scale=True,
                            apply_scale_options="FBX_SCALE_UNITS", axis_forward="-Z", axis_up="Y",
                            use_space_transform=True, bake_space_transform=True,
                            use_mesh_modifiers=True, mesh_smooth_type="OFF", use_tspace=False,
                            use_triangles=True, add_leaf_bones=False, bake_anim=False,
                            path_mode="AUTO", embed_textures=False, use_custom_props=True)
    entry["fbx_file"] = str(path)


def child(element, name):
    return next(item for item in element.elems if item.id == name)


# FBX の生頂点がメートルと Y-up で記録されていることも確認する。
for identifier in priority:
    entry = entries[identifier]
    path = Path(entry["fbx_file"])
    document, _ = parse_fbx.parse(str(path))
    globals = {item.props[0]: item.props[-1]
               for item in child(child(document, b"GlobalSettings"), b"Properties70").elems}
    assert globals[b"UnitScaleFactor"] == 100.0
    assert globals[b"UpAxis"] == 1 and globals[b"UpAxisSign"] == 1
    geometries = [item for item in child(document, b"Objects").elems if item.id == b"Geometry"]
    vertices = []
    for geometry in geometries:
        values = child(geometry, b"Vertices").props[0]
        points = list(zip(values[0::3], values[1::3], values[2::3]))
        vertices.extend(points)
        if b"__Screen" in geometry.props[1]:
            spec = entry["screen"]
            assert all(abs(p[2] - spec["front_z_m"]) < 1e-5 for p in points)
            normals = child(child(geometry, b"LayerElementNormal"), b"Normals").props[0]
            assert all(abs(n) < 1e-5 for n in normals[0::3])
            assert all(abs(n) < 1e-5 for n in normals[1::3])
            assert all(abs(n - 1) < 1e-5 for n in normals[2::3])
    dimensions = [max(p[i] for p in vertices) - min(p[i] for p in vertices) for i in range(3)]
    assert all(abs(a - b) < 1e-5 for a, b in zip(dimensions, entry["dimensions_m"]))
    assert abs(min(p[1] for p in vertices)) < 1e-5
    for model in (item for item in child(document, b"Objects").elems if item.id == b"Model"):
        properties = {p.props[0]: p.props[-3:] for p in child(model, b"Properties70").elems}
        assert all(abs(float(v)) < 1e-5 for v in properties.get(b"Lcl Translation", [0, 0, 0]))
        assert all(abs(float(v)) < 1e-5 for v in properties.get(b"Lcl Rotation", [0, 0, 0]))
        assert all(abs(float(v) - 1) < 1e-5 for v in properties.get(b"Lcl Scaling", [1, 1, 1]))

    # 専用プロセス内で一度だけ回読する。ユーザーの .blend には保存しない。
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(path), use_anim=False)
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    assert sorted(obj.name for obj in meshes) == sorted(entry["mesh_names"])
    points = [obj.matrix_world @ v.co for obj in meshes for v in obj.data.vertices]
    size = [max(p[i] for p in points) - min(p[i] for p in points) for i in (0, 2, 1)]
    assert all(abs(a - b) < 1e-5 for a, b in zip(size, entry["dimensions_m"]))
    for obj in meshes:
        assert all(abs(v - 1) < 1e-5 for v in obj.scale)
        if obj.name.endswith("__Screen"):
            assert [m.name for m in obj.data.materials] == ["TD2_Screen"]
            screen = entry["screen"]
            for loop in obj.data.loops:
                p = obj.matrix_world @ obj.data.vertices[loop.vertex_index].co
                uv = obj.data.uv_layers.active.data[loop.index].uv
                assert abs(uv.x - (p.x / screen["width_m"] + 0.5)) < 1e-5
                assert abs(uv.y - (p.z - screen["bottom_m"]) / screen["height_m"]) < 1e-5
        else:
            assert all(mat.name != "TD2_Screen" for mat in obj.data.materials)
    print("FBX_READY", identifier, str(path), "ROUNDTRIP_PASS", flush=True)

# 既存の短い資産一覧に追加形式の入口だけを追記する。
manifest["conventions"]["fbx_units"] = "1 unit = 1 meter; UnitScaleFactor 100 cm"
manifest["conventions"]["fbx_axes"] = "+Y up, +Z front; baked mesh axes; identity node transforms"
manifest["notes"].append("FBX alternatives passed Blender roundtrip; HIKARI import still requires editor verification.")
(ROOT / "asset_manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print("ALL_FBX_READY", len(priority), flush=True)
