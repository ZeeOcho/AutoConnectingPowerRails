"""
Auto-Connecting Power Rails — repair the parent links the asset generator did not write.

Run this INSIDE the Unreal editor, with any level open:
    Window > Developer Tools > Output Log, dropdown "Cmd" -> "Python", then:
    exec(open(r"<path>/acpr_fix_material_parents.py").read())

THIS ONE WRITES. It backs up every .uasset it touches first, and it touches only
MaterialInstanceConstants under CENSUS_ROOT that have no parent.

WHAT IS BROKEN

The asset generator (UEAssetToolkit) leaves nearly every MaterialInstanceConstant under
/Game/FactoryGame of the extracted base-game Content without a parent — about 1600 of them,
each a ~1.8 KB stub. Only the instances CustomAssets.txt protects (which the generator never
writes, e.g. MI_Factory2D_01 -> MM_Factory_Array) are intact. An instance without a parent
has no shader, so everything that wears one renders wrong, including the vanilla instances
our own materials are children of. This script runs once after the Content has been
generated and before acpr_materials.py.

The generator's UMaterialInstanceGenerator::PopulateSimpleAssetWithData does attempt the
parent and falls back to the engine's default surface material (the grey checkerboard) when
it cannot deserialize it; an instance with no parent at all is a package whose data
population did not complete, so the generator cannot be relied on to write it.

WHERE THE CORRECT PARENT COMES FROM

From the dump, whose JSON shape is fixed by the dumper's own source
(AssetDumper/.../ObjectHierarchySerializer.cpp and SerializationContext.cpp):

    {
      "AssetClassPath": "...",
      "AssetSerializedData": { "AssetObjectData": { "Parent": <int index>, ... } },
      "ObjectHierarchy": [
        { "ObjectIndex": n, "Type": "Import", "ClassName": "Material",
          "Outer": m, "ObjectName": "MM_FactoryBaked" },
        { "ObjectIndex": m, "Type": "Import", "ObjectName": "/Game/.../MM_FactoryBaked" },
        ...
      ]
    }

An Import's Outer chain ends at a top-level package entry with no "Outer", whose
ObjectName IS the package path. So the parent is read, never guessed, and if it cannot be
read the asset is skipped and named.
"""

import json
import os
import shutil
import time
import traceback

import unreal

# ---------------------------------------------------------------------------

# The orphaning is the whole base-game material graph, not a few assets, so every orphan is
# repaired. The parent of each is read from the dump, never assumed: the families are deeper
# than they look, e.g.
#
#   MI_Beams_01                 -> MM_FactoryBaked_Beams        (a per-family master)
#   MI_Factory_01               -> MI_Factory_Base_01_Emsiv_AO
#   MI_Factory_Base_01_Emsiv_AO -> MI_Factory_Base_01
#   MI_Factory_Base_01          -> MI_Factory2D_01 -> MM_Factory_Array
#
# EXPECT A LONG SHADER COMPILE AFTER THIS RUN. Giving ~1600 material instances a parent
# means the editor has ~1600 materials to compile permutations for, and it will start as
# soon as they are saved. That is not this script hanging.

# The env var is only so this script can be dry-run outside the editor against a fake dump;
# in the editor nothing sets it and the literal path is used.
DUMP_ROOT = os.environ.get("ACPR_DUMP_ROOT", r"G:\OtherDevelopment\Satisfactory\dump")
BACKUP_DIR_NAME = "ParentFixBackup"   # beside Content, NOT inside it — see _backup_root
LOG_NAME = "ACPR_fix_parents_log.txt"

MAX_REPAIRS = 4000         # a ceiling, not an expectation: ~1600 are orphaned
PROGRESS_EVERY = 100       # a line every N assets, so a long run is visibly alive

# Done first and reported line by line, because these four are what our own work depends on:
# MI_Factory_Base_01 is the parent of MI_ACPR_Body (acpr_materials.py), MI_Beams_01 is what
# the vanilla beams wear, and all four are slot 0 of the vanilla reference meshes.
TARGETS = (
    "/Game/FactoryGame/Buildable/Building/Pillars/Material/MI_Beams_01",
    "/Game/FactoryGame/Buildable/-Shared/Material/MI_Factory_01",
    "/Game/FactoryGame/-Shared/Material/MI_Factory_Base_01",
    "/Game/FactoryGame/-Shared/Material/MI_Factory_Base_01_Emsiv_AO",
)

# The control: already has a parent, is in TARGETS' sibling folder, and must come out of
# this run untouched. If the script reports a change to it, the script is wrong.
CONTROL = "/Game/FactoryGame/Buildable/-Shared/Material/MI_Factory2D_01"

CENSUS_ROOT = "/Game/FactoryGame"
CENSUS_LIMIT = 4000        # material instances to load for the orphan count
FAILURE_DETAIL_LIMIT = 40  # skipped assets printed in full, the rest grouped by reason

_lines = []


def _log(message):
    _lines.append(message)
    unreal.log("[ACPR] " + message)


def _info(label, detail=""):
    _log("{0}: {1}".format(label, detail))


def _content_dir():
    """Absolute path of the project's Content folder, or None. Three guarded routes."""
    paths = getattr(unreal, "Paths", None)
    getter = getattr(paths, "project_content_dir", None) if paths else None
    if getter is not None:
        try:
            value = str(getter())
            if value:
                return os.path.abspath(value)
        except Exception:
            pass
    saved = getattr(paths, "project_saved_dir", None) if paths else None
    if saved is not None:
        try:
            value = os.path.abspath(str(saved()))
            candidate = os.path.join(os.path.dirname(value), "Content")
            if os.path.isdir(candidate):
                return candidate
        except Exception:
            pass
    system = getattr(unreal, "SystemLibrary", None)
    getter = getattr(system, "get_project_content_directory", None) if system else None
    if getter is not None:
        try:
            value = str(getter())
            if value:
                return os.path.abspath(value)
        except Exception:
            pass
    return None


def _backup_root():
    """Backups go BESIDE Content, never inside it.

    A .uasset copied into Content is a second asset as far as the editor is concerned, with
    a duplicate name, and it would be picked up by every list_assets call in this project's
    scripts. The project directory is Content's parent."""
    content = _content_dir()
    if content is None:
        return None
    return os.path.join(os.path.dirname(content), BACKUP_DIR_NAME)


def _asset_file(package_path):
    """<Content>/<relative>.uasset for a /Game/... package, or None."""
    content = _content_dir()
    if content is None:
        return None
    clean = package_path.split(".")[0]
    if not clean.startswith("/Game/"):
        return None
    relative = clean[len("/Game/"):].replace("/", os.sep)
    candidate = os.path.join(content, relative + ".uasset")
    return candidate if os.path.isfile(candidate) else None


def _backup(package_path):
    """Copy the asset's file under the backup root, mirroring its path. (ok, detail)."""
    source = _asset_file(package_path)
    if source is None:
        return False, "no .uasset on disk for " + package_path
    root = _backup_root()
    if root is None:
        return False, "the backup folder could not be located"
    content = _content_dir()
    relative = os.path.relpath(source, content)
    destination = os.path.join(root, relative)
    try:
        directory = os.path.dirname(destination)
        if not os.path.isdir(directory):
            os.makedirs(directory)
        if os.path.exists(destination):
            # Never overwrite an existing backup: the first one is the pre-repair state,
            # and a second run must not replace it with an already-repaired copy.
            return True, "backup already present, kept: " + destination
        shutil.copy2(source, destination)
        return True, destination
    except Exception as e:
        return False, "copy failed: {0}".format(e)


def _load(path):
    try:
        clean = path.split(".")[0]
        if not unreal.EditorAssetLibrary.does_asset_exist(clean):
            return None
        return unreal.EditorAssetLibrary.load_asset(clean)
    except Exception:
        return None


def _class_of(asset):
    getter = getattr(asset, "get_class", None)
    if getter is not None:
        try:
            return str(getter().get_name())
        except Exception:
            pass
    return type(asset).__name__


def _parent_of(material):
    try:
        return material.get_editor_property("parent")
    except Exception:
        return None


# ---------------------------------------------------------------------------
# reading the dump

def _dump_file(package_path):
    """dump\\Game\\X\\Y\\Z.json for /Game/X/Y/Z, or None."""
    clean = package_path.split(".")[0]
    if not clean.startswith("/Game/"):
        return None
    relative = clean[len("/Game/"):].replace("/", os.sep)
    candidate = os.path.join(DUMP_ROOT, "Game", relative + ".json")
    return candidate if os.path.isfile(candidate) else None


def _hierarchy_index(document):
    """{ ObjectIndex: entry } for the dump's ObjectHierarchy array."""
    out = {}
    for entry in document.get("ObjectHierarchy") or []:
        if not isinstance(entry, dict):
            continue
        index = entry.get("ObjectIndex")
        if index is None:
            continue
        try:
            out[int(index)] = entry
        except Exception:
            continue
    return out


def _package_of_entry(entry, index, depth=0):
    """Walk an Import's Outer chain to the top-level package entry and return its name.

    The dumper writes a top-level UPackage import with no "Outer" field and ObjectName set
    to the package path, so the chain terminates on its own. depth is belt and braces."""
    if entry is None or depth > 16:
        return None
    if "Outer" not in entry:
        name = entry.get("ObjectName")
        return str(name) if name else None
    try:
        outer = int(entry.get("Outer"))
    except Exception:
        return None
    return _package_of_entry(index.get(outer), index, depth + 1)


def dump_parent(package_path):
    """(package, asset_name, note) for the parent the DUMP records, or (None, None, why)."""
    path = _dump_file(package_path)
    if path is None:
        clean = package_path.split(".")[0]
        expected = os.path.join(DUMP_ROOT, "Game",
                                clean[len("/Game/"):].replace("/", os.sep) + ".json") \
            if clean.startswith("/Game/") else "(not a /Game/ package)"
        return None, None, "no dump file at " + expected
    try:
        with open(path, "r", encoding="utf-8") as handle:
            document = json.load(handle)
    except Exception as e:
        return None, None, "dump file could not be read: {0}".format(e)
    serialized = document.get("AssetSerializedData") or {}
    object_data = serialized.get("AssetObjectData") or {}
    if "Parent" not in object_data:
        return None, None, ("the dump records no Parent field for this instance "
                            "(keys present: {0})".format(
                                ", ".join(sorted(object_data.keys())[:12]) or "none"))
    try:
        parent_index = int(object_data.get("Parent"))
    except Exception:
        return None, None, "the dump's Parent field is not an index: {0!r}".format(
            object_data.get("Parent"))
    if parent_index < 0:
        return None, None, "the dump records Parent as null (index {0})".format(parent_index)
    index = _hierarchy_index(document)
    entry = index.get(parent_index)
    if entry is None:
        return None, None, "index {0} is not in the dump's ObjectHierarchy".format(parent_index)
    package = _package_of_entry(entry, index)
    name = entry.get("ObjectName")
    if not package:
        return None, None, "the Outer chain of index {0} does not end at a package".format(
            parent_index)
    return str(package), (str(name) if name else None), "read from the dump"


# ---------------------------------------------------------------------------

def census():
    """Every MaterialInstanceConstant with no parent. Read only; returns their paths.

    Hands the list to repair() so the scan is done once."""
    _log("")
    _log("=== how widespread is this? every MaterialInstanceConstant under {0} ===".format(
        CENSUS_ROOT))
    try:
        paths = unreal.EditorAssetLibrary.list_assets(
            CENSUS_ROOT, recursive=True, include_folder=False) or []
    except Exception as e:
        _info("census", "list_assets failed: {0}".format(e))
        return []
    instance_class = getattr(unreal, "MaterialInstanceConstant", None)
    if instance_class is None:
        _info("census", "unreal.MaterialInstanceConstant is not available in this build")
        return []
    looked, instances, orphan_paths, examples = 0, 0, [], []
    for path in paths:
        if looked >= CENSUS_LIMIT:
            break
        clean = path.split(".")[0]
        leaf = clean.split("/")[-1]
        # A name filter, not a class filter: the isinstance check below is what decides.
        # It only exists to keep this from loading every asset in the project.
        if not (leaf.startswith("MI_") or leaf.startswith("MaterialInstance")):
            continue
        looked += 1
        asset = _load(clean)
        if asset is None or not isinstance(asset, instance_class):
            continue
        instances += 1
        if _parent_of(asset) is None:
            orphan_paths.append(clean)
            if len(examples) < 12:
                examples.append(leaf)
    _info("census", "{0} MaterialInstanceConstant(s) loaded of {1} candidate name(s); "
                    "{2} have NO parent".format(instances, looked, len(orphan_paths)))
    if instances:
        _info("    share", "{0:.1f}% orphaned".format(100.0 * len(orphan_paths) / instances))
    if examples:
        _info("    examples", ", ".join(examples))
    if looked >= CENSUS_LIMIT:
        _info("    NOTE", "the {0}-asset scan ceiling was hit, so this is a floor, "
                          "not a total".format(CENSUS_LIMIT))
    return orphan_paths


def control_check():
    """The control must have a parent before and after. Read only."""
    asset = _load(CONTROL)
    if asset is None:
        _info("control", CONTROL + " NOT FOUND — the control cannot be checked")
        return None
    parent = _parent_of(asset)
    _info("control", "{0} [{1}] parent {2}".format(
        CONTROL.split("/")[-1], _class_of(asset),
        parent.get_path_name().split(".")[0] if parent is not None else "NONE"))
    return parent


def _repair_one(path, update, verbose):
    """(outcome, parent_package_or_reason). outcome is "changed" or "skipped"."""
    leaf = path.split("/")[-1]
    asset = _load(path)
    if asset is None:
        return "skipped", "NOT FOUND"
    instance_class = getattr(unreal, "MaterialInstanceConstant", None)
    if instance_class is not None and not isinstance(asset, instance_class):
        return "skipped", "is a {0}, not a MaterialInstanceConstant".format(_class_of(asset))
    existing = _parent_of(asset)
    if existing is not None:
        try:
            return "skipped", "already has parent " + existing.get_path_name().split(".")[0]
        except Exception:
            return "skipped", "already has a parent"

    package, name, note = dump_parent(path)
    if package is None:
        return "skipped", "the dump does not give a parent: " + note
    target = _load(package)
    if target is None:
        return "skipped", "the dump's parent {0} does not exist in this project".format(package)
    interface = getattr(unreal, "MaterialInterface", None)
    if interface is not None and not isinstance(target, interface):
        return "skipped", "the resolved parent {0} is a {1}, not a material".format(
            package, _class_of(target))

    ok, detail = _backup(path)
    if not ok:
        return "skipped", "NOT backed up ({0}) — nothing written".format(detail)
    if verbose:
        _info("    backup", detail)

    try:
        asset.set_editor_property("parent", target)
    except Exception as e:
        return "skipped", "setting the parent failed: {0}".format(e)
    if update is not None:
        try:
            update(asset)
        except Exception as e:
            if verbose:
                _info("    " + leaf, "update_material_instance raised: {0}".format(e))
    saved = "not attempted"
    try:
        saved = str(unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False))
    except Exception as e:
        saved = "save failed: {0}".format(e)
    # Read it back: "the call succeeded" is not evidence, so the parent is re-read from the
    # asset.
    after = _parent_of(asset)
    if after is None:
        return "skipped", "set and saved={0} but the parent reads back as NONE".format(saved)
    try:
        landed = after.get_path_name().split(".")[0]
    except Exception:
        landed = package
    if verbose:
        _info("    " + leaf, "saved={0}  parent now {1}".format(saved, landed))
    return "changed", landed


def repair(orphans):
    _log("")
    _log("=== repair: set each orphan's parent to what the DUMP records ===")
    root = _backup_root()
    _info("backups", root or "could not be located — nothing will be written")
    if root is None:
        return
    if not os.path.isdir(DUMP_ROOT):
        _info("dump", DUMP_ROOT + " does not exist")
        _log("    The dump is where the correct parent is recorded. Without it this script")
        _log("    has nothing to read and will not guess: if the dump has been cleaned up")
        _log("    it has to be re-made, or the parents set by hand in the editor.")
        return
    _info("dump", DUMP_ROOT)

    library = getattr(unreal, "MaterialEditingLibrary", None)
    update = getattr(library, "update_material_instance", None) if library else None
    _info("post-set refresh", "MaterialEditingLibrary.update_material_instance"
                              if update is not None else
                              "not available in this build — the asset is saved without it")

    # TARGETS first and reported line by line, because those four are the ones our own
    # work depends on and their parents are worth reading individually. Then the rest,
    # summarised — 1600 lines of "ok" is not information.
    queue = [p for p in TARGETS]
    for path in orphans:
        if path not in queue:
            queue.append(path)
    queue = queue[:MAX_REPAIRS]
    _info("to process", "{0} asset(s)".format(len(queue)))

    changed, skipped, landings, failures = 0, 0, {}, []
    started = time.time()
    for position, path in enumerate(queue):
        verbose = path in TARGETS
        leaf = path.split("/")[-1]
        if verbose:
            _log("-- " + leaf)
        try:
            outcome, detail = _repair_one(path, update, verbose)
        except Exception as e:
            outcome, detail = "skipped", "raised {0}: {1}".format(type(e).__name__, e)
        if outcome == "changed":
            changed += 1
            landings[detail] = landings.get(detail, 0) + 1
            if verbose:
                _info("    " + leaf, "parent now " + detail)
        else:
            skipped += 1
            failures.append((path, detail))
            if verbose:
                _info("    " + leaf, "SKIPPED — " + detail)
        if (position + 1) % PROGRESS_EVERY == 0:
            _info("    progress", "{0}/{1}  {2} changed, {3} skipped, {4:.0f}s elapsed".format(
                position + 1, len(queue), changed, skipped, time.time() - started))

    _log("")
    _info("repair", "{0} changed, {1} skipped, {2:.0f}s".format(
        changed, skipped, time.time() - started))

    if landings:
        _log("")
        _log("Where they landed — the parent each repaired instance now has, by count.")
        _log("This is the base game's material family map, read out of the dump.")
        for package in sorted(landings, key=lambda k: (-landings[k], k)):
            _info("    {0:5d}".format(landings[package]), package)

    if failures:
        _log("")
        _info("skipped, with reasons", "{0} asset(s)".format(len(failures)))
        reasons = {}
        for _path, detail in failures:
            # Group by the shape of the reason, not its exact text, so 1500 variants of
            # "no dump file at <path>" collapse into one line with a count.
            key = detail.split(":")[0].split(" at ")[0].strip()
            reasons[key] = reasons.get(key, 0) + 1
        for key in sorted(reasons, key=lambda k: (-reasons[k], k)):
            _info("    {0:5d}".format(reasons[key]), key)
        _log("")
        _log("    First {0} in full:".format(FAILURE_DETAIL_LIMIT))
        for path, detail in failures[:FAILURE_DETAIL_LIMIT]:
            _log("     {0}  <-  {1}".format(path, detail))


def _write_log():
    try:
        directory = os.path.join(str(unreal.Paths.project_saved_dir()), "ACPR")
    except Exception:
        directory = os.path.join(os.path.expanduser("~"), "ACPR")
    directory = os.path.abspath(directory)
    try:
        if not os.path.isdir(directory):
            os.makedirs(directory)
        path = os.path.join(directory, LOG_NAME)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("Auto-Connecting Power Rails — material parent repair\n")
            handle.write("=" * 60 + "\n\n")
            handle.write("\n".join(_lines) + "\n")
        _log("log written to " + path)
    except Exception as e:
        _log("the log file could not be written: {0}".format(e))


def main():
    _log("material parent repair  {0}".format(time.strftime("%Y-%m-%d %H:%M:%S")))
    _log("Writes. Backs up every file it touches first. Touches only material instances "
         "with no parent.")
    _log("")
    before = control_check()
    orphans = []
    try:
        orphans = census() or []
    except Exception as e:
        _info("census — section raised", "{0}: {1}".format(type(e).__name__, e))
        for line in traceback.format_exc().splitlines():
            _log("    " + line)
    try:
        repair(orphans)
    except Exception as e:
        _info("repair — section raised", "{0}: {1}".format(type(e).__name__, e))
        for line in traceback.format_exc().splitlines():
            _log("    " + line)
    _log("")
    _log("=== control, after ===")
    after = control_check()
    if before is not None and after is not None:
        same = False
        try:
            same = before.get_path_name() == after.get_path_name()
        except Exception:
            same = False
        _info("control unchanged", "yes" if same else "NO — this script did something it "
                                                      "should not have")
    _write_log()


try:
    main()
except Exception:
    _log("the repair stopped early:")
    for _line in traceback.format_exc().splitlines():
        _log("    " + _line)
    _write_log()
