"""
Auto-Connecting Power Rails — fingerprint the mod's assets, to compare two regenerations.

A check that the scripts reproduce the content exactly, for a clean checkout or after a script
changed. .uasset files cannot be hashed (every save writes a fresh package GUID), so this reads
what matters out of each asset and writes it as JSON:

    meshes      vertex / triangle / section counts per LOD, bounds, material slots and what is
                in them, simple and convex collision counts, collision complexity
    materials   class, parent, every scalar / vector / texture override, every static switch
    textures    size, compression, sRGB, LOD group, mip settings
    blueprints  the properties the scripts write (descriptors, recipes, schematic, buildables)
                and every mesh component: class, mesh, transform, material overrides, profile
    inventory   every asset under /AutoConnectingPowerRails with its class

Run inside the editor, before and after a regeneration:
    Window > Developer Tools > Output Log, dropdown "Cmd" -> "Python", then:
    exec(open(r"<plugin>/Scripts/acpr_fingerprint.py").read())

LABEL names the output, Saved/ACPR_Fingerprint/<LABEL>.json. Run on its own it writes
before.json; acpr_build_all.py runs it last with the label "after", and with a before.json
beside it the differences are printed at the end and written to diff.txt. For a clean checkout,
copy before.json from the old project's Saved/ACPR_Fingerprint/ into the new one's first.
"""

import json
import os
import re
import traceback

import unreal

# "before" in the current project, "after" in the regenerated one. acpr_build_all.py injects
# ACPR_LABEL instead of editing this line.
LABEL = globals().get("ACPR_LABEL", "before")

MOD = "AutoConnectingPowerRails"
ROOT = "/" + MOD
PRECISION = 3

# The blueprint properties the scripts write, by asset-name prefix. Read through the class
# defaults; a property a class does not have is skipped and listed as absent.
BLUEPRINT_PROPERTIES = {
    "Desc_": ("m_display_name", "m_description", "m_buildable_class", "m_small_icon",
              "m_persistent_big_icon", "m_form", "m_category", "m_sub_categories", "m_menu_priority",
              "m_uses_distance_for_zooping", "m_stack_size"),
    "Recipe_": ("m_display_name", "m_display_name_override", "m_product", "m_ingredients",
                "m_produced_in", "m_manufactoring_duration", "m_manual_manufacturing_multiplier"),
    "Schematic_": ("m_display_name", "m_description", "m_type", "m_tech_tier", "m_cost", "m_unlocks",
                   "m_schematic_dependencies", "m_schematic_icon", "m_small_schematic_icon",
                   "m_schematic_category", "m_sub_categories", "m_menu_priority",
                   "m_dependencies_blocks_schematic_access", "m_hidden_until_dependencies_met"),
    "Build_": ("m_hologram_class", "m_display_name", "m_description", "m_size",
               "m_default_length", "m_max_length", "m_length_per_cost", "m_uses_distance_for_zooping",
               "m_rail_mesh_asset", "m_standoff", "m_body_inset", "m_max_power_lines",
               "m_half_extent", "m_grid_pitch", "m_profile_scale",
               "m_managed_by_lightweight_buildable_subsystem", "m_can_contain_lightweight_instances"),
    "Holo_": ("m_nudge_distance_coarse", "m_nudge_distance_fine", "m_roll_step_degrees",
              "m_roll_step_fine_degrees", "m_terminal_snap_range", "m_surface_offset",
              "m_lane_offset", "m_hoverpack_node_spacing", "m_build_mode_diagonal",
              "m_build_mode_free_form"),
}

_lines = []


def _log(message):
    _lines.append(message)
    unreal.log("[ACPR-FINGERPRINT] " + message)


def _r(value):
    return round(float(value), PRECISION)


def _vec(v):
    return [_r(v.x), _r(v.y), _r(v.z)]


def _value(value):
    """A JSON-ready form of a property value: assets and classes by path, structs by text."""
    if value is None:
        return None
    if isinstance(value, (bool, int, float, str)):
        return _r(value) if isinstance(value, float) else value
    if isinstance(value, unreal.Array):
        return [_value(v) for v in value]
    if isinstance(value, unreal.Text):
        return str(value)
    if isinstance(value, unreal.Name):
        return str(value)
    if isinstance(value, unreal.Vector):
        return _vec(value)
    if isinstance(value, unreal.Rotator):
        return [_r(value.roll), _r(value.pitch), _r(value.yaw)]
    if isinstance(value, unreal.LinearColor):
        return [_r(value.r), _r(value.g), _r(value.b), _r(value.a)]
    if isinstance(value, unreal.Object):
        return value.get_path_name()
    if isinstance(value, unreal.StructBase):
        # Structs by their fields where the API breaks them up; the text form otherwise, with
        # the object address (which changes every run) removed.
        try:
            return [_value(v) for v in value.to_tuple()]
        except Exception:
            pass
    return re.sub(r" \(0x[0-9A-Fa-f]+\)", "", str(value))


def _prop_raw(obj, name):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return None


def _prop(obj, name):
    try:
        return _value(obj.get_editor_property(name))
    except Exception:
        return "<absent>"


# ---------------------------------------------------------------------------

def _mesh(asset):
    subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    bounds = asset.get_bounds()
    box = asset.get_bounding_box()
    lods = []
    for lod in range(asset.get_num_lods()):
        lods.append({"vertices": asset.get_num_vertices(lod),
                     "triangles": asset.get_num_triangles(lod),
                     "sections": asset.get_num_sections(lod)})
    slots = []
    for entry in asset.get_editor_property("static_materials"):
        material = entry.get_editor_property("material_interface")
        slots.append({"slot": str(entry.get_editor_property("material_slot_name")),
                      "material": material.get_path_name() if material else None})
    return {
        "lods": lods,
        "bounds_origin": _vec(bounds.origin),
        "bounds_extent": _vec(bounds.box_extent),
        "box_min": _vec(box.min),
        "box_max": _vec(box.max),
        "slots": slots,
        "simple_collision": subsystem.get_simple_collision_count(asset),
        "convex_collision": subsystem.get_convex_collision_count(asset),
        "collision_complexity": str(subsystem.get_collision_complexity(asset)),
    }


def _master_of(material):
    seen = 0
    while material is not None and not isinstance(material, unreal.Material) and seen < 16:
        material = material.get_editor_property("parent")
        seen += 1
    return material if isinstance(material, unreal.Material) else None


def _material(asset):
    out = {"class": asset.get_class().get_name()}
    if isinstance(asset, unreal.MaterialInstance):
        parent = asset.get_editor_property("parent")
        out["parent"] = parent.get_path_name() if parent else None
        for prop in ("scalar_parameter_values", "vector_parameter_values", "texture_parameter_values"):
            values = {}
            for entry in asset.get_editor_property(prop):
                info = entry.get_editor_property("parameter_info")
                values[str(info.get_editor_property("name"))] = _value(entry.get_editor_property("parameter_value"))
            out[prop] = values
        master = _master_of(asset)
        switches = {}
        if master is not None:
            try:
                names = unreal.MaterialEditingLibrary.get_static_switch_parameter_names(master)
            except Exception as error:
                names = []
                switches["<names>"] = str(error).splitlines()[0]
            for name in names:
                try:
                    switches[str(name)] = unreal.MaterialEditingLibrary.get_material_instance_static_switch_parameter_value(asset, name)
                except Exception as error:
                    switches[str(name)] = "<{0}>".format(str(error).splitlines()[0])
        out["static_switches"] = switches
    elif isinstance(asset, unreal.Material):
        out["blend_mode"] = str(asset.get_editor_property("blend_mode"))
        out["shading_model"] = str(asset.get_editor_property("shading_model"))
        out["two_sided"] = asset.get_editor_property("two_sided")
        library = unreal.MaterialEditingLibrary
        out["scalar_parameters"] = {str(n): _r(library.get_material_default_scalar_parameter_value(asset, n))
                                    for n in library.get_scalar_parameter_names(asset)}
        out["vector_parameters"] = {str(n): _value(library.get_material_default_vector_parameter_value(asset, n))
                                    for n in library.get_vector_parameter_names(asset)}
    return out


def _texture(asset):
    return {"width": asset.blueprint_get_size_x(), "height": asset.blueprint_get_size_y(),
            "compression": str(asset.get_editor_property("compression_settings")),
            "srgb": asset.get_editor_property("srgb"),
            "lod_group": str(asset.get_editor_property("lod_group")),
            "mip_gen": str(asset.get_editor_property("mip_gen_settings"))}


def _components(blueprint):
    """Every component the blueprint declares, through the SubobjectDataSubsystem."""
    out = {}
    try:
        subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
        library = unreal.SubobjectDataBlueprintFunctionLibrary
        for handle in subsystem.k2_gather_subobject_data_for_blueprint(blueprint) or []:
            obj = library.get_object(library.get_data(handle))
            if obj is None or not isinstance(obj, unreal.ActorComponent):
                continue
            entry = {"class": obj.get_class().get_name()}
            if isinstance(obj, unreal.SceneComponent):
                entry["location"] = _prop(obj, "relative_location")
                entry["rotation"] = _prop(obj, "relative_rotation")
                entry["scale"] = _prop(obj, "relative_scale3d")
            if isinstance(obj, unreal.StaticMeshComponent):
                entry["mesh"] = _prop(obj, "static_mesh")
                entry["profile"] = str(obj.get_collision_profile_name())
                entry["materials"] = [m.get_path_name() if m else None
                                      for m in (obj.get_material(i) for i in range(obj.get_num_materials()))]
            out[obj.get_name()] = entry
    except Exception as error:
        out["<error>"] = str(error).splitlines()[0]
    return out


def _blueprint(asset, name):
    out = {"class": asset.get_class().get_name()}
    generated = unreal.EditorAssetLibrary.load_blueprint_class(asset.get_path_name().split(".")[0])
    if generated is None:
        out["<error>"] = "no generated class"
        return out
    out["parent_class"] = _prop(asset, "parent_class")
    cdo = unreal.get_default_object(generated)
    out["components"] = _components(asset) if isinstance(cdo, unreal.Actor) else {}
    for prefix, props in BLUEPRINT_PROPERTIES.items():
        if name.startswith(prefix):
            out["properties"] = {p: _prop(cdo, p) for p in props}
    if name.startswith("Schematic_"):
        out["unlock_recipes"] = [[_value(r) for r in (_prop_raw(u, "m_recipes") or [])]
                                 for u in (_prop_raw(cdo, "m_unlocks") or [])]
        out["dependency_schematics"] = [[u.get_class().get_name()] + [_value(r) for r in (_prop_raw(u, "m_schematics") or [])]
                                        for u in (_prop_raw(cdo, "m_schematic_dependencies") or [])]
    return out


# ---------------------------------------------------------------------------

def fingerprint():
    result = {"inventory": {}, "meshes": {}, "materials": {}, "textures": {}, "blueprints": {}}
    paths = sorted(unreal.EditorAssetLibrary.list_assets(ROOT, recursive=True, include_folder=False))
    for path in paths:
        object_path = path.split(".")[0]
        name = object_path.rsplit("/", 1)[-1]
        asset = unreal.EditorAssetLibrary.load_asset(object_path)
        if asset is None:
            result["inventory"][object_path] = "<not loadable>"
            continue
        result["inventory"][object_path] = asset.get_class().get_name()
        try:
            if isinstance(asset, unreal.StaticMesh):
                result["meshes"][name] = _mesh(asset)
            elif isinstance(asset, unreal.MaterialInterface):
                result["materials"][name] = _material(asset)
            elif isinstance(asset, unreal.Texture2D):
                result["textures"][name] = _texture(asset)
            elif isinstance(asset, unreal.Blueprint):
                result["blueprints"][name] = _blueprint(asset, name)
        except Exception as error:
            result.setdefault("errors", {})[name] = str(error).splitlines()[0]
    return result


def _flatten(node, prefix=""):
    if isinstance(node, dict):
        out = {}
        for key, value in node.items():
            out.update(_flatten(value, prefix + "/" + str(key)))
        return out
    if isinstance(node, list):
        out = {}
        for index, value in enumerate(node):
            out.update(_flatten(value, prefix + "[" + str(index) + "]"))
        return out
    return {prefix: node}


def diff(before, after):
    a, b = _flatten(before), _flatten(after)
    lines = []
    for key in sorted(set(a) | set(b)):
        if key not in a:
            lines.append("+ {0} = {1}".format(key, b[key]))
        elif key not in b:
            lines.append("- {0} = {1}".format(key, a[key]))
        elif a[key] != b[key]:
            lines.append("~ {0}: {1} -> {2}".format(key, a[key], b[key]))
    return lines


def main():
    directory = os.path.join(os.path.abspath(str(unreal.Paths.project_saved_dir())), "ACPR_Fingerprint")
    os.makedirs(directory, exist_ok=True)
    result = fingerprint()
    path = os.path.join(directory, LABEL + ".json")
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(result, handle, indent=2, sort_keys=True)
    _log("{0}: {1} assets, {2} meshes, {3} materials, {4} textures, {5} blueprints -> {6}".format(
        LABEL, len(result["inventory"]), len(result["meshes"]), len(result["materials"]),
        len(result["textures"]), len(result["blueprints"]), path))
    for name, error in result.get("errors", {}).items():
        _log("  error reading {0}: {1}".format(name, error))

    before = os.path.join(directory, "before.json")
    if LABEL == "after" and os.path.exists(before):
        with open(before, "r", encoding="utf-8") as handle:
            lines = diff(json.load(handle), result)
        with open(os.path.join(directory, "diff.txt"), "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
        _log("{0} difference(s) against before.json{1}".format(len(lines), ":" if lines else ""))
        for line in lines[:200]:
            _log("  " + line)
        if len(lines) > 200:
            _log("  ... the rest is in diff.txt")


try:
    main()
except Exception:
    unreal.log_error("[ACPR-FINGERPRINT] " + traceback.format_exc())
