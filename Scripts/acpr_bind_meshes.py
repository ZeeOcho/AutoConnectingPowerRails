r"""
Auto-Connecting Power Rails — point the BUILDABLES at our meshes.

Run this INSIDE the Unreal editor, with any level open:
    Window > Developer Tools > Output Log, dropdown "Cmd" -> "Python", then:
    exec(open(r"<path>/acpr_bind_meshes.py").read())

THIS WRITES. It backs up every .uasset it touches first, and it only writes where it has
READ a target to write to.

WHAT IT DOES

The buildable blueprints in /AutoConnectingPowerRails/Buildables/ carry mesh components that
are not pointed at the mod's meshes by anything else: acpr_meshes.py builds the meshes into
/AutoConnectingPowerRails/Meshes/ and acpr_materials.py puts materials on those mesh ASSETS,
but the reference from each buildable's mesh component to its mesh is a write to the
blueprint, and this script is that write. Run acpr_meshes.py first; the materials are on the
mesh assets, so they come along with the meshes.

HOW IT WRITES WITHOUT GUESSING

Enumerate first, write second, and only into a target that was read:

  * every component the blueprint declares, found through the SubobjectDataSubsystem and
    then the SimpleConstructionScript's nodes, and written only where the component has a
    static_mesh property;
  * every component-shaped property on the class default object, which is where the
    components of a blueprint without an SCS (native C++ components) live;
  * mInstanceDataCDO -> instances[i].static_mesh is REPORTED, never written (see bind()).

Anything it cannot read is reported and skipped. Anything it writes is read back. Every
.uasset is copied to BuildableBindBackup/ beside Content first, and a write is refused if the
backup fails.
"""

import os
import shutil
import time
import traceback

import unreal

# ---------------------------------------------------------------------------

MOD = "AutoConnectingPowerRails"
BP_DIR = "/" + MOD + "/Buildables"
MESH_DIR = "/" + MOD + "/Meshes"
BACKUP_DIR_NAME = "BuildableBindBackup"
LOG_NAME = "ACPR_bind_log.txt"

# Asset names match acpr_validate.py's PARTS table, which is the list checked against the
# project.
# (label, blueprint, default mesh, {component-name substring: mesh}). Every mesh component of
# the buildable gets the default mesh unless its name contains one of the override keys. The
# match is by case-insensitive substring of the component's name, so "terminal" catches the
# blueprint's TerminalMeshA/B and the CDO's m_terminal_mesh_a/b alike. The override matters:
# the C++ only fills a collar component that is EMPTY, so a body mesh written into a collar
# component would win over the collar the C++ resolves.
#
# Never written, by name. BuildingMeshProxy is the blueprint SCS proxy inherited from vanilla's
# beam, with instancing NOT blocked: a mesh in it is handed to the instance manager and drawn as
# an unrotated, unscaled instance at the actor's origin, independent of everything the C++ fits.
# The C++ owns the Rail's meshes (RailMesh, TerminalMeshA/B, all proxies with instancing
# blocked); this one stays empty on purpose.
NEVER_BIND = ("BuildingMeshProxy",)

BINDINGS = (
    # The Rail has two collar components (TerminalMeshA/B) and no bridge components; the
    # bridged collar (SM_ACPR_RailTerminalBridged) is swapped in by the C++ at runtime.
    ("Rail", "Build_PowerRail", "SM_ACPR_RailBody",
     {"terminal": "SM_ACPR_RailTerminal"}),
    ("Junction", "Build_PowerRailJunction", "SM_ACPR_Junction", {}),
    # The pad is picked per host by the C++ (three meshes); the blueprint gets the body one so
    # the component is never empty in the editor.
    ("Outlet", "Build_PowerRailOutlet", "SM_ACPR_Outlet", {"base": "SM_ACPR_OutletBaseBody"}),
    # The Cap has two meshes, one per host face width, and which one a Cap wears is decided at
    # runtime by AACPRCap::ApplyCapMesh from soft paths defaulted in C++; the blueprint gets the
    # Rail one so the component is never empty in the editor.
    ("Cap", "Build_PowerRailCap", "SM_ACPR_CapRail", {}),
)

# False makes this a pure report — enumerate every target and change nothing.
APPLY = True

_lines = []
_problems = []


def _log(message):
    _lines.append(message)
    unreal.log("[ACPR] " + message)


def _info(label, detail=""):
    _log("{0}: {1}".format(label, detail))


def _problem(text):
    _problems.append(text)
    _log("PROBLEM  " + text)


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
            handle.write("Auto-Connecting Power Rails — buildable mesh binding\n")
            handle.write("=" * 60 + "\n\n")
            handle.write("\n".join(_lines) + "\n")
        _log("log written to " + path)
    except Exception as e:
        _log("the log file could not be written: {0}".format(e))


# ---------------------------------------------------------------------------

def _content_dir():
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
    return None


def _asset_file(package_path):
    """<project>/Mods/.../Content/... for a /AutoConnectingPowerRails/ package.

    Our mod does NOT live under the project's Content folder — it is a game feature at
    Mods/GameFeatures/AutoConnectingPowerRails/Content, and /AutoConnectingPowerRails/ is
    its own mount point. Both roots are tried and whichever holds the file is used."""
    content = _content_dir()
    if content is None:
        return None
    clean = package_path.split(".")[0]
    parts = [p for p in clean.split("/") if p]
    if not parts:
        return None
    project = os.path.dirname(content)
    roots = []
    if parts[0] == "Game":
        roots.append((content, parts[1:]))
    else:
        roots.append((os.path.join(project, "Mods", "GameFeatures", parts[0], "Content"),
                      parts[1:]))
        roots.append((os.path.join(project, "Mods", parts[0], "Content"), parts[1:]))
        roots.append((content, parts))
    for root, rest in roots:
        candidate = os.path.join(root, *rest) + ".uasset"
        if os.path.isfile(candidate):
            return candidate
    return None


def _backup(package_path):
    source = _asset_file(package_path)
    if source is None:
        return False, "no .uasset found on disk for " + package_path
    content = _content_dir()
    if content is None:
        return False, "the Content folder could not be located"
    root = os.path.join(os.path.dirname(content), BACKUP_DIR_NAME)
    name = package_path.split(".")[0].strip("/").replace("/", "__") + ".uasset"
    destination = os.path.join(root, name)
    try:
        if not os.path.isdir(root):
            os.makedirs(root)
        if os.path.exists(destination):
            return True, "backup already present, kept: " + destination
        shutil.copy2(source, destination)
        return True, destination
    except Exception as e:
        return False, "copy failed: {0}".format(e)


def _get(obj, prop):
    if obj is None:
        return None
    try:
        return obj.get_editor_property(prop)
    except Exception:
        return None


def _path_of(obj):
    if obj is None:
        return "<none>"
    try:
        return obj.get_path_name()
    except Exception:
        return str(obj)


def _name_of(obj):
    if obj is None:
        return "<none>"
    for getter in ("get_name", "get_fname"):
        fn = getattr(obj, getter, None)
        if fn is not None:
            try:
                return str(fn())
            except Exception:
                continue
    return str(obj)


def _class_of(obj):
    getter = getattr(obj, "get_class", None)
    if getter is not None:
        try:
            return str(getter().get_name())
        except Exception:
            pass
    return type(obj).__name__


def _class_properties(obj):
    """Every editor property name the generated Python class documents.

    Unreal's bindings put an "Editor Properties:" block in each generated class's __doc__,
    one "- ``name`` (Type): [Read-Write]" line per property."""
    doc = getattr(type(obj), "__doc__", "") or ""
    names = []
    for line in doc.splitlines():
        line = line.strip()
        if line.startswith("- ``") and "``" in line[4:]:
            names.append(line[4:].split("``")[0])
    return names


def _components_from_cdo(cdo):
    """Walk the CDO's own documented properties and keep anything component-shaped."""
    out = []
    for name in _class_properties(cdo):
        value = _get(cdo, name)
        if value is None or isinstance(value, (bool, int, float, str)):
            continue
        klass = _class_of(value)
        if "Component" not in klass and "Proxy" not in klass:
            continue
        out.append((name, value))
    return out


def _components_from_blueprint(path):
    """Every component the Blueprint declares. Returns (components, how).

    SubobjectDataSubsystem is a UEngineSubsystem, so its getter is get_engine_subsystem;
    get_editor_subsystem refuses it ("Parameter must be a 'Class' not
    'SubobjectDataSubsystem'"). Both getters are tried and each reports its own result, the
    SCS route says why it found nothing, and the CDO property walk (_components_from_cdo)
    covers blueprints that declare no SCS at all."""
    try:
        asset = unreal.EditorAssetLibrary.load_asset(path)
    except Exception as e:
        return [], "asset not loadable: {0}".format(e)
    if asset is None:
        return [], "asset not loadable"

    subobject_class = getattr(unreal, "SubobjectDataSubsystem", None)
    library = getattr(unreal, "SubobjectDataBlueprintFunctionLibrary", None)
    if subobject_class is not None and library is not None:
        for getter_name in ("get_engine_subsystem", "get_editor_subsystem"):
            getter = getattr(unreal, getter_name, None)
            if getter is None:
                continue
            try:
                subsystem = getter(subobject_class)
                handles = subsystem.k2_gather_subobject_data_for_blueprint(asset)
                out = []
                for handle in handles or []:
                    data = library.get_data(handle)
                    obj = library.get_object(data)
                    if obj is not None and hasattr(obj, "get_editor_property"):
                        out.append(obj)
                if out:
                    return out, "SubobjectDataSubsystem via " + getter_name
                _info("  " + getter_name, "returned {0} handle(s), 0 usable objects".format(
                    len(list(handles or []))))
            except Exception as e:
                _info("  " + getter_name, "{0}".format(str(e).splitlines()[0]))

    scs = _get(asset, "simple_construction_script")
    if scs is None:
        _info("  simple_construction_script", "absent — this blueprint declares no SCS, so "
                                              "its components are native (C++), not added "
                                              "in the blueprint editor")
    else:
        try:
            nodes = _get(scs, "all_nodes") or []
            out = []
            for node in nodes:
                template = _get(node, "component_template")
                if template is not None:
                    out.append(template)
            if out:
                return out, "SimpleConstructionScript.all_nodes"
            _info("  SimpleConstructionScript", "{0} node(s), none with a component "
                                                "template".format(len(list(nodes))))
        except Exception as e:
            _info("  SimpleConstructionScript", "walk failed: {0}".format(e))
    return [], "no route produced components"


def _set_mesh(holder, prop, mesh):
    """Try every way of writing a mesh into a property. Returns (ok, how).

    A "[Read-Write]" mark in the generated class's docstring describes the BINDING — that a
    getter and a setter exist — and says nothing about whether the editor permits the edit.
    A property that carries the "not editable on instances" flag makes set_editor_property
    refuse it ("Property 'StaticMesh' for attribute 'static_mesh' on 'InstanceData' cannot be
    edited on instances").

    Direct attribute assignment goes through the generated descriptor instead of the editor's
    permission check, so it is tried first. Every route reports its own result and the value
    is read back afterwards regardless."""
    attempts = []
    # 1. The struct descriptor, which bypasses the editor permission path.
    try:
        setattr(holder, prop, mesh)
        attempts.append(("setattr", None))
    except Exception as e:
        attempts.append(("setattr", str(e).splitlines()[0]))
    if _path_of(_get(holder, prop)).split(".")[0].endswith(_name_of(mesh)):
        return True, "setattr"
    # 2. The editor property path.
    try:
        holder.set_editor_property(prop, mesh)
        attempts.append(("set_editor_property", None))
    except Exception as e:
        attempts.append(("set_editor_property", str(e).splitlines()[0]))
    if _path_of(_get(holder, prop)).split(".")[0].endswith(_name_of(mesh)):
        return True, "set_editor_property"
    return False, "; ".join("{0}: {1}".format(n, r or "no error but no change")
                            for n, r in attempts)


def _recompile(path):
    library = getattr(unreal, "BlueprintEditorLibrary", None)
    fn = getattr(library, "compile_blueprint", None) if library else None
    if fn is None:
        return "BlueprintEditorLibrary.compile_blueprint not in this build"
    try:
        asset = unreal.EditorAssetLibrary.load_asset(path)
        fn(asset)
        return "recompiled"
    except Exception as e:
        return "recompile failed: {0}".format(e)


# ---------------------------------------------------------------------------

def _load_mesh(label, mesh_name):
    """Loads one of our meshes by name; returns None (and records a problem) if it is missing."""
    mesh_path = MESH_DIR + "/" + mesh_name
    mesh = None
    try:
        if unreal.EditorAssetLibrary.does_asset_exist(mesh_path):
            mesh = unreal.EditorAssetLibrary.load_asset(mesh_path)
    except Exception as e:
        _problem("{0}: {1} could not be loaded: {2}".format(label, mesh_path, e))
        return None
    if mesh is None:
        _problem("{0}: {1} does not exist — run acpr_meshes.py first".format(label, mesh_path))
        return None
    return mesh


def _mesh_for(description, default_name, overrides):
    """Which mesh NAME a component gets: the first override whose key is a substring of the
    component's name (case-insensitive), else the buildable's default."""
    lowered = description.lower()
    for key, name in overrides.items():
        if key.lower() in lowered:
            return name
    return default_name


def bind(label, buildable, mesh_name, overrides=None):
    overrides = overrides or {}
    path = BP_DIR + "/" + buildable
    _log("")
    _log("=== {0} — {1} ===".format(label, path))

    # Every mesh this buildable can need, loaded up front so a missing one stops the whole
    # buildable rather than half-binding it.
    meshes = {}
    for name in [mesh_name] + sorted(set(overrides.values())):
        mesh = _load_mesh(label, name)
        if mesh is None:
            return
        meshes[name] = mesh
    _info("  our mesh", MESH_DIR + "/" + mesh_name)
    for key, name in overrides.items():
        _info("  components named *{0}* get".format(key), MESH_DIR + "/" + name)

    try:
        cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
        cdo = unreal.get_default_object(cls) if cls else None
    except Exception as e:
        _problem("{0}: blueprint class not loadable: {1}".format(label, e))
        return
    if cdo is None:
        _problem("{0}: no class default object".format(label))
        return

    targets = []      # (description, setter, reader)

    # 1. The abstract-instance data — reported, never written, and not a target.
    #
    # Python cannot write it: the refusal is on the ARRAY, not the element
    # ("AbstractInstanceDataObject: Property 'Instances' for attribute 'instances' on
    # 'AbstractInstanceDataObject' cannot be edited on instances"). And it would not matter if
    # it could: the Rail's instance-data hooks are never called, because they belong to the
    # lightweight subsystem and this Rail sets mManagedByLightweightBuildableSubsystem = false.
    # The Rail renders from a mesh COMPONENT, like the other buildables.
    data = _get(cdo, "m_instance_data_cdo")
    _info("  mInstanceDataCDO", _path_of(data))
    if data is not None:
        entries = _get(data, "instances")
        try:
            entries = list(entries) if entries is not None else []
        except Exception:
            entries = []
        _info("  instances", "{0} entry/entries — reported only, see the note above".format(
            len(entries)))
        for index, entry in enumerate(entries):
            current = _get(entry, "static_mesh")
            _info("    instances[{0}].static_mesh".format(index), _path_of(current))

    # 2. Every component the blueprint declares.
    components, how = _components_from_blueprint(path)
    _info("  blueprint components", "{0} via {1}".format(len(components), how))
    for component in components:
        holder = _get(component, "static_mesh")
        klass = _class_of(component)
        has_slot = holder is not None or "Mesh" in klass or "Mesh" in _name_of(component)
        if not has_slot:
            continue
        if _name_of(component) in NEVER_BIND:
            _info("    {0} [{1}]".format(_name_of(component), klass),
                  "{0}   LEFT ALONE (NEVER_BIND)".format(_path_of(holder)))
            continue
        _info("    {0} [{1}]".format(_name_of(component), klass), _path_of(holder))
        targets.append((_name_of(component) + " [" + klass + "]", component, "static_mesh"))

    # 3. The CDO's OWN properties. The Junction and Outlet declare no SCS, so their
    # components are native and live as properties on the class default object.
    native = _components_from_cdo(cdo)
    _info("  native components on the CDO", "{0} found".format(len(native)))
    for name, component in native:
        holder = _get(component, "static_mesh")
        klass = _class_of(component)
        _info("    {0} [{1}]".format(name, klass), _path_of(holder))
        if holder is not None or "Mesh" in klass or "Proxy" in klass:
            targets.append((name + " [" + klass + "]", component, "static_mesh"))
    if not native:
        # Say what WAS on the CDO, so "nothing found" is a fact about the object rather
        # than about how hard this script looked.
        names = _class_properties(cdo)
        _info("  the CDO documents", "{0} propert(ies){1}".format(
            len(names), ": " + ", ".join(names[:25]) if names else " — docstring empty"))

    if not targets:
        _problem("{0}: nothing with a static_mesh was found to write to.".format(label))
        return

    if not APPLY:
        _info("  APPLY is False", "{0} target(s) found, nothing written".format(len(targets)))
        return

    ok, detail = _backup(path)
    if not ok:
        _problem("{0}: NOT backed up ({1}) — nothing written".format(label, detail))
        return
    _info("  backup", detail)

    changed = 0
    for description, holder, prop in targets:
        wanted = _mesh_for(description, mesh_name, overrides)
        before = _path_of(_get(holder, prop))
        ok, how = _set_mesh(holder, prop, meshes[wanted])
        after = _path_of(_get(holder, prop))
        landed = after.split(".")[0].endswith(wanted)
        _info("    " + description,
              "{0}  ->  {1}   {2}".format(
                  before, after, "OK via " + how if landed else "DID NOT TAKE"))
        if landed:
            changed += 1
        else:
            _problem("{0}: {1} could not be written — {2}".format(label, description, how))

    if changed:
        _info("  recompile", _recompile(path))
        try:
            saved = unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
            _info("  saved", str(saved))
        except Exception as e:
            _problem("{0}: save failed: {1}".format(label, e))
    what = mesh_name if not overrides else mesh_name + " / " + ", ".join(sorted(set(overrides.values())))
    _info("  " + label, "{0} of {1} target(s) now point at {2}".format(
        changed, len(targets), what))


def main():
    _log("buildable mesh binding  {0}".format(time.strftime("%Y-%m-%d %H:%M:%S")))
    _log("APPLY = {0}. Backs up every .uasset before writing, reads every write back.".format(
        APPLY))
    for label, buildable, mesh_name, overrides in BINDINGS:
        try:
            bind(label, buildable, mesh_name, overrides)
        except Exception as e:
            _problem("{0} — raised {1}: {2}".format(label, type(e).__name__, e))
            for line in traceback.format_exc().splitlines():
                _log("    " + line)
    _log("")
    _log("=== summary ===")
    _log("{0} problem(s)".format(len(_problems)))
    for problem in _problems:
        _log("  PROBLEM  " + problem)
    if not _problems:
        _log("Every buildable references its ACPR mesh. Alpakit and look —")
        _log("the materials are on the mesh assets, so they come along with them.")
    _write_log()


try:
    main()
except Exception:
    _log("the binding pass stopped early:")
    for _line in traceback.format_exc().splitlines():
        _log("    " + _line)
    _write_log()
