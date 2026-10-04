"""
Auto-Connecting Power Rails — the buildable assets: blueprints, descriptors, recipes, schematic.

Creates, for each of the four buildables (Rail, Junction, Outlet, Cap):

    /AutoConnectingPowerRails/Buildables/Build_<Stem>    parented to the C++ buildable class
    /AutoConnectingPowerRails/Buildables/Holo_<Stem>     parented to the C++ hologram class
    /AutoConnectingPowerRails/Buildables/Desc_<Stem>     a building descriptor in the Power category
    /AutoConnectingPowerRails/Recipes/Recipe_<Stem>      the §13 recipe, made by the Build Gun

and /AutoConnectingPowerRails/Schematics/Schematic_PowerRails: the AWESOME Shop package that
unlocks all four, at §13's coupon price, visible after Basic Steel Production. Meshes are bound
by acpr_bind_meshes.py and icons by acpr_icons.py; this script leaves both alone.

It reads vanilla assets from the asset registry (the ingredient descriptors by their display
name, the Build Gun, the Power build category, the AWESOME Shop category, the milestone, a
vanilla schematic to take the concrete unlock and dependency classes from, and the Painted
Beam, whose hologram carries the two beam build-mode descriptors) and edits none of them.

The Rail is a beam (B.2): its Build_ carries the Painted Beam's size and length fields, which a
C++ subclass does not inherit from a sibling Blueprint, and its Holo_ carries the vanilla beam
hologram's mBuildModeDiagonal / mBuildModeFreeForm, without which AFGBeamHologram cannot choose
between its vertical and free-form placement paths. Both are copied here, the descriptors
from the vanilla hologram itself rather than from a path written down.

Writes a log to <project>/Saved/ACPR/ACPR_buildables_log.txt as well as the Output Log.

Re-runnable: an asset that exists is kept and its properties written again; nothing is
duplicated from anything. Every write is read back from a freshly resolved class default,
after a forced save, because a write to a blueprint's defaults does not dirty its package and
the in-memory object is replaced by the recompile a save triggers.

Before running it: build Development Editor and restart the editor, so the C++ classes exist.

Run this INSIDE the Unreal editor:
    Window > Developer Tools > Output Log, switch the dropdown from "Cmd" to
    "Python", then:  exec(open(r"<plugin>/Scripts/acpr_buildables.py").read())

Property names, from the FactoryGame headers:
    UFGBuildingDescriptor::mBuildableClass       FGBuildingDescriptor.h
    UFGItemDescriptor::mDisplayName/mDescription FGItemDescriptor.h
    UFGItemDescriptor::mCategory                 FGItemDescriptor.h
    UFGBuildDescriptor::mUsesDistanceForZooping  FGBuildDescriptor.h
    AFGBuildable::mHologramClass                 FGBuildable.h
    AFGBuildable::mDisplayName/mDescription      FGBuildable.h
    UFGRecipe::mProduct/mIngredients/mProducedIn FGRecipe.h
    UFGSchematic::mType/mTechTier/mCost          FGSchematic.h
    UFGSchematic::mSchematicCategory/mUnlocks    FGSchematic.h
    UFGSchematic::mSchematicDependencies         FGSchematic.h
    UFGUnlockRecipe::mRecipes                    FGUnlockRecipe.h
    UFGSchematicPurchasedDependency::mSchematics FGSchematicPurchasedDependency.h
    AFGBuildableBeam::mSize/mDefaultLength/mMaxLength/mLengthPerCost  FGBuildableBeam.h
    AFGBeamHologram::mBuildModeDiagonal/mBuildModeFreeForm             FGBeamHologram.h
"""

import os

import unreal

MOD = "AutoConnectingPowerRails"
BASE = "/" + MOD
BP_DIR = BASE + "/Buildables"
RECIPE_DIR = BASE + "/Recipes"
SCHEMATIC_DIR = BASE + "/Schematics"
SCHEMATIC_NAME = "Schematic_PowerRails"
SCHEMATIC = SCHEMATIC_DIR + "/" + SCHEMATIC_NAME

# stem, C++ buildable, C++ hologram, display name, description, ingredients by DISPLAY name
# (the only stable key: "Steel Beam" is Desc_SteelPlate), whether the build-gun readout is
# a length (B.2: only the Rail is zoopable).
PARTS = (
    ("PowerRail", "ACPRRail", "ACPRRailHologram",
     "Power Rail",
     "A steel power backbone that connects through compatible Rail terminals. "
     "Attach a Power Rail Outlet for ordinary Power Lines.",
     (("Steel Beam", 1), ("Cable", 1)), True),
    ("PowerRailJunction", "ACPRJunction", "ACPRJunctionHologram",
     "Power Rail Junction",
     "A six-way Power Rail node. Place it standalone, snap it to a terminal, or insert it into "
     "one Rail; every unoccupied face accepts a Rail, Outlet or Cap.",
     (("Steel Beam", 1), ("Cable", 1)), False),
    ("PowerRailOutlet", "ACPROutlet", "ACPROutletHologram",
     "Power Rail Outlet",
     "Mounts on a Power Rail body or open terminal and provides one connection point for up to "
     "four Power Lines.",
     (("Wire", 4), ("Iron Rod", 1)), False),
    ("PowerRailCap", "ACPRCap", "ACPRCapHologram",
     "Insulated Terminal Cap",
     "Blocks a Rail terminal from snapping and blueprint auto-connection.",
     (("Concrete", 1), ), False),
)

SCHEMATIC_DISPLAY = "Power Rails"
SCHEMATIC_DESCRIPTION = ("Unlocks the Power Rail, Power Rail Junction, Power Rail Outlet and "
                         "Insulated Terminal Cap.")
SCHEMATIC_TIER = 1
COUPON_COST = 4
COUPON_DESC = "Desc_ResourceSinkCoupon"
MILESTONE_DISPLAY = "Basic Steel Production"

# Vanilla assets found by name in the registry walk.
BUILD_GUN = "BP_BuildGun"
BUILD_CATEGORY = "BC_Power"              # the build menu's Power tab ...
BUILD_SUB_CATEGORY = "SC_PowerPoles"     # ... and the Power Poles group in it, beside the Wall Outlet
SHOP_CATEGORY = "SC_RSS_Management"      # the AWESOME Shop's Management tab ...
SHOP_SUB_CATEGORY = "SC_RSS_Power"       # ... and its Power group
SHOP_MENU_PRIORITY = 11.0                # after vanilla's own entries in that group
VANILLA_BEAM = "Build_Beam_Painted"      # the beam whose hologram carries the build-mode descriptors

# The Rail's beam fields (AFGBuildableBeam, uu). The first three are the Painted Beam's own
# values — a 1 m cross-section field that drives neither mesh nor snapping, 4 m to start a drag,
# 40 m at most (§11). mLengthPerCost is §13's ⌈L / 10 m⌉ and deliberately not vanilla's 400.
RAIL_BEAM = (("mSize", 100.0), ("mDefaultLength", 400.0), ("mMaxLength", 4000.0), ("mLengthPerCost", 1000.0))
RAIL_STEM = "PowerRail"

LOG_NAME = "ACPR_buildables_log.txt"

_tools = unreal.AssetToolsHelpers.get_asset_tools()
_checks = []
_lines = []


def _log(message):
    _lines.append(message)
    unreal.log("[ACPR-BUILDABLES] " + message)


def _write_log():
    try:
        directory = os.path.abspath(os.path.join(str(unreal.Paths.project_saved_dir()), "ACPR"))
        if not os.path.isdir(directory):
            os.makedirs(directory)
        path = os.path.join(directory, LOG_NAME)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("Auto-Connecting Power Rails — buildable assets\n")
            handle.write("=" * 60 + "\n\n")
            handle.write("\n".join(_lines) + "\n")
        _log("log written to " + path)
    except Exception as error:
        _log("the log file could not be written: {0}".format(error))


def _check(label, ok, detail=""):
    _checks.append((label, bool(ok), detail))
    _log("{0} {1} {2}".format("PASS" if ok else "FAIL", label, detail))
    return bool(ok)


def _text(value):
    return str(value) if value is not None else ""


def _set(obj, prop, value):
    try:
        obj.set_editor_property(prop, value)
        return True
    except Exception as error:
        _log("  set {0} raised {1}".format(prop, error))
        return False


def _get(obj, prop):
    try:
        return obj.get_editor_property(prop)
    except Exception:
        return None


def _find_cpp_class(name):
    try:
        found = unreal.load_object(None, "/Script/{0}.{1}".format(MOD, name))
        if found:
            return found
    except Exception:
        pass
    return getattr(unreal, name, None)


def _gen_class(bp_path):
    """The class a blueprint generates: what every TSubclassOf property and new_object want."""
    try:
        return unreal.EditorAssetLibrary.load_blueprint_class(bp_path)
    except Exception:
        return None


def _cdo(bp_path):
    cls = _gen_class(bp_path)
    return unreal.get_default_object(cls) if cls else None


def _save(path):
    try:
        return bool(unreal.EditorAssetLibrary.save_asset(path, False))
    except Exception as error:
        _log("  save_asset({0}) raised {1}".format(path, error))
        return False


def _make_bp(name, folder, parent_class):
    """A blueprint of `parent_class`; an existing one is kept."""
    path = folder + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        _log("exists, keeping: " + path)
        return path
    if parent_class is None:
        _log("cannot create {0}: parent class missing".format(path))
        return None
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("ParentClass", parent_class)
    asset = _tools.create_asset(name, folder, None, factory)
    if asset is None:
        _log("FAILED to create " + path)
        return None
    unreal.EditorAssetLibrary.save_loaded_asset(asset)
    _log("created " + path)
    return path


# ---------------------------------------------------------------------------
# What vanilla provides
# ---------------------------------------------------------------------------

def _class_of(package_path):
    try:
        return unreal.EditorAssetLibrary.load_blueprint_class(package_path)
    except Exception:
        return None


def _scan_vanilla(names, item_displays):
    """One registry walk: blueprint classes by asset name, item descriptor classes by display
    name (under Resource), and every vanilla schematic with its display name."""
    by_name, by_display, schematics = {}, {}, []
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    # A path filter only: class filters want TopLevelAssetPaths in 5.6 and the type check is
    # cheap in Python.
    for data in registry.get_assets(unreal.ARFilter(recursive_paths=True, package_paths=["/Game/FactoryGame"])):
        name = str(data.asset_name)
        path = str(data.package_name)
        if name in names and name not in by_name:
            by_name[name] = _class_of(path)
        if name.startswith("Desc_") and "/Resource/" in path:
            cls = _class_of(path)
            cdo = unreal.get_default_object(cls) if cls else None
            display = _text(_get(cdo, "mDisplayName")).strip() if cdo else ""
            if display in item_displays and display not in by_display:
                by_display[display] = cls
        if name.startswith("Schematic_") and "/FactoryGame/" in path:
            cls = _class_of(path)
            cdo = unreal.get_default_object(cls) if cls else None
            if cdo:
                schematics.append((name, cls, _text(_get(cdo, "mDisplayName")).strip()))
    return by_name, by_display, schematics


def _is_purchased_dependency(dependency):
    """UFGSchematicNotPurchasedDependency derives from UFGSchematicPurchasedDependency, so the
    class test alone would accept the inverse and gate the package on NOT owning the milestone."""
    try:
        if not isinstance(dependency, unreal.FGSchematicPurchasedDependency):
            return False
        return "NotPurchased" not in dependency.get_class().get_name()
    except Exception:
        return False


def _dependency_class(schematics):
    """A concrete UFGSchematicPurchasedDependency: the C++ class is abstract, so the class is
    read off a dependency a vanilla schematic carries."""
    for name, cls, display in schematics:
        for dependency in (_get(unreal.get_default_object(cls), "mSchematicDependencies") or []):
            if _is_purchased_dependency(dependency):
                return dependency.get_class(), name
    return None, None


def _unlock_class(schematics):
    """A concrete UFGUnlockRecipe (abstract in C++ too), preferring one whose own defaults list
    no recipes, since a new object starts from its class's defaults."""
    fallback = (None, None)
    for name, cls, display in schematics:
        for unlock in (_get(unreal.get_default_object(cls), "mUnlocks") or []):
            try:
                if not isinstance(unlock, unreal.FGUnlockRecipe):
                    continue
                unlock_class = unlock.get_class()
            except Exception:
                continue
            if not (_get(unreal.get_default_object(unlock_class), "mRecipes") or []):
                return unlock_class, name
            if fallback[0] is None:
                fallback = (unlock_class, name)
    return fallback


def _milestone(schematics, display):
    for name, cls, shown in schematics:
        if shown.lower() == display.lower():
            return cls, name
    return None, None


def _beam_build_modes(by_name):
    """The two build-mode descriptor classes, read off the vanilla Painted Beam's own hologram
    (Build_Beam_Painted -> mHologramClass -> mBuildModeDiagonal / mBuildModeFreeForm), so no
    descriptor path is written down here. Returns ((property, class), ...) — empty if the
    chain breaks, and says where."""
    beam_cls = by_name.get(VANILLA_BEAM)
    if beam_cls is None:
        _check("vanilla beam hologram read", False, VANILLA_BEAM + " not in the registry")
        return ()
    beam = unreal.get_default_object(beam_cls)
    holo_cls = _get(beam, "mHologramClass")
    holo = unreal.get_default_object(holo_cls) if holo_cls else None
    if holo is None:
        _check("vanilla beam hologram read", False, VANILLA_BEAM + " has no mHologramClass")
        return ()
    modes = []
    for prop in ("mBuildModeDiagonal", "mBuildModeFreeForm"):
        descriptor = _get(holo, prop)
        if descriptor is not None:
            modes.append((prop, descriptor))
        _check("vanilla beam hologram " + prop, descriptor is not None,
               descriptor.get_name() if descriptor is not None else "<none> on " + holo_cls.get_name())
    # For the record beside our own values: what the vanilla beam carries.
    _log("  {0}: {1}".format(VANILLA_BEAM, ", ".join(
        "{0}={1}".format(prop, _get(beam, prop)) for prop, _ in RAIL_BEAM)))
    return tuple(modes)


# ---------------------------------------------------------------------------
# The buildables
# ---------------------------------------------------------------------------

def _write_and_verify(path, writes, what):
    """Writes `writes` ((property, value), ...) onto the asset's class defaults, saves, resolves
    the class again and reads every property back off the new defaults."""
    cdo = _cdo(path)
    if cdo is None:
        _check(what + ": class defaults resolved", False)
        return False
    for prop, value in writes:
        _set(cdo, prop, value)
    saved = _save(path)
    back = _cdo(path)
    ok = saved and back is not None
    for prop, value in writes:
        got = _get(back, prop) if back else None
        if isinstance(value, (str, unreal.Text)):
            good = _text(got).strip() == _text(value).strip()
        elif isinstance(value, (list, tuple)):
            good = got is not None and len(got) == len(value)
        elif isinstance(value, bool):
            good = got is value
        else:
            good = got is not None and (got == value or str(got) == str(value))
        if not good:
            _log("  {0}.{1} read back as {2!r}".format(what, prop, got))
        ok = ok and good
    _check(what + ": written and read back", ok, "saved" if saved else "save failed")
    return ok


def _buildable(part, vanilla, build_modes):
    stem, cpp_build, cpp_holo, display, description, ingredients, zooping = part
    by_name, by_display = vanilla
    _log("")
    _log("--- " + display + " ---")

    build_cpp = _find_cpp_class(cpp_build)
    holo_cpp = _find_cpp_class(cpp_holo)
    if not _check(display + ": C++ classes present", build_cpp and holo_cpp,
                  "build Development Editor and restart the editor if this fails"):
        return None

    build_path = _make_bp("Build_" + stem, BP_DIR, build_cpp)
    holo_path = _make_bp("Holo_" + stem, BP_DIR, holo_cpp)
    desc_path = _make_bp("Desc_" + stem, BP_DIR, unreal.FGBuildingDescriptor)
    recipe_path = _make_bp("Recipe_" + stem, RECIPE_DIR, unreal.FGRecipe)
    if not all((build_path, holo_path, desc_path, recipe_path)):
        return None

    holo_cls = _gen_class(holo_path)
    build_cls = _gen_class(build_path)
    desc_cls = _gen_class(desc_path)
    recipe_cls = _gen_class(recipe_path)
    if not _check(display + ": generated classes resolved", holo_cls and build_cls and desc_cls and recipe_cls):
        return None

    # The buildable: its hologram, and its own name and description (the pair the build menu
    # shows for a buildable; the descriptor's pair is what an item view reads).
    writes = [("mHologramClass", holo_cls),
              ("mDisplayName", unreal.Text(display)),
              ("mDescription", unreal.Text(description))]
    if stem == RAIL_STEM:
        writes.extend(RAIL_BEAM)
    _write_and_verify(build_path, writes, "Build_" + stem)

    # The Rail's hologram: the vanilla beam's two build-mode descriptors (B.2).
    if stem == RAIL_STEM:
        if _check(display + ": beam build modes to copy", len(build_modes) == 2, "{0} found".format(len(build_modes))):
            _write_and_verify(holo_path, build_modes, "Holo_" + stem)

    # The descriptor: the buildable, the Power category, the readout kind, name and description.
    writes = [("mBuildableClass", build_cls),
              ("mUsesDistanceForZooping", zooping),
              ("mDisplayName", unreal.Text(display)),
              ("mDescription", unreal.Text(description))]
    if by_name.get(BUILD_CATEGORY) is not None:
        writes.insert(1, ("mCategory", by_name[BUILD_CATEGORY]))
    if by_name.get(BUILD_SUB_CATEGORY) is not None:
        writes.insert(2, ("mSubCategories", [by_name[BUILD_SUB_CATEGORY]]))
    _write_and_verify(desc_path, writes, "Desc_" + stem)

    # The recipe: one product, §13's ingredients, made by the Build Gun.
    missing = [name for name, _ in ingredients if by_display.get(name) is None]
    _check(display + ": ingredients found", not missing, ", ".join(missing) if missing else "")
    writes = [("mDisplayName", unreal.Text(display)),
              ("mManufactoringDuration", 1.0),
              ("mProduct", [unreal.ItemAmount(item_class=desc_cls, amount=1)]),
              ("mIngredients", [unreal.ItemAmount(item_class=by_display[name], amount=amount)
                                for name, amount in ingredients if by_display.get(name) is not None])]
    if by_name.get(BUILD_GUN) is not None:
        writes.append(("mProducedIn", [by_name[BUILD_GUN]]))
    _write_and_verify(recipe_path, writes, "Recipe_" + stem)
    return recipe_cls


# ---------------------------------------------------------------------------
# The schematic
# ---------------------------------------------------------------------------

def _schematic(recipe_classes, by_name, by_display, schematics):
    _log("")
    _log("--- " + SCHEMATIC_DISPLAY + " ---")
    path = _make_bp(SCHEMATIC_NAME, SCHEMATIC_DIR, unreal.FGSchematic)
    if path is None:
        return
    cdo = _cdo(path)
    if cdo is None:
        _check("schematic: class defaults resolved", False)
        return

    writes = [("mDisplayName", unreal.Text(SCHEMATIC_DISPLAY)),
              ("mDescription", unreal.Text(SCHEMATIC_DESCRIPTION)),
              ("mType", unreal.SchematicType.EST_RESOURCE_SINK),
              ("mTechTier", SCHEMATIC_TIER),
              ("mMenuPriority", SHOP_MENU_PRIORITY),
              ("mDependenciesBlocksSchematicAccess", True),
              ("mHiddenUntilDependenciesMet", True)]
    coupon = by_name.get(COUPON_DESC)
    if _check("schematic: coupon descriptor found", coupon is not None, COUPON_DESC):
        writes.append(("mCost", [unreal.ItemAmount(item_class=coupon, amount=COUPON_COST)]))
    if _check("schematic: shop category found", by_name.get(SHOP_CATEGORY) is not None, SHOP_CATEGORY):
        writes.append(("mSchematicCategory", by_name[SHOP_CATEGORY]))
    if _check("schematic: shop sub-category found", by_name.get(SHOP_SUB_CATEGORY) is not None, SHOP_SUB_CATEGORY):
        writes.append(("mSubCategories", [by_name[SHOP_SUB_CATEGORY]]))

    # The unlock and the dependency are instanced subobjects of the schematic's defaults, of
    # concrete classes vanilla's own schematics use. An existing one is reused and re-pointed.
    unlock_cls, unlock_from = _unlock_class(schematics)
    dependency_cls, dependency_from = _dependency_class(schematics)
    milestone, milestone_name = _milestone(schematics, MILESTONE_DISPLAY)
    _check("schematic: unlock class found", unlock_cls is not None, "from " + str(unlock_from))
    _check("schematic: dependency class found", dependency_cls is not None, "from " + str(dependency_from))
    _check("schematic: milestone found", milestone is not None, str(milestone_name))

    if unlock_cls is not None:
        unlock = next((u for u in (_get(cdo, "mUnlocks") or []) if isinstance(u, unreal.FGUnlockRecipe)), None)
        if unlock is None:
            unlock = unreal.new_object(unlock_cls, cdo)
        _set(unlock, "mRecipes", list(recipe_classes))
        writes.append(("mUnlocks", [unlock]))
    if dependency_cls is not None and milestone is not None:
        dependency = next((d for d in (_get(cdo, "mSchematicDependencies") or []) if _is_purchased_dependency(d)), None)
        if dependency is None:
            dependency = unreal.new_object(dependency_cls, cdo)
        _set(dependency, "mSchematics", [milestone])
        writes.append(("mSchematicDependencies", [dependency]))

    _write_and_verify(path, writes, SCHEMATIC_NAME)

    back = _cdo(path)
    unlocks = [u for u in (_get(back, "mUnlocks") or []) if isinstance(u, unreal.FGUnlockRecipe)]
    listed = list(_get(unlocks[0], "mRecipes") or []) if unlocks else []
    _check("schematic: unlocks the {0} recipes".format(len(recipe_classes)),
           len(unlocks) == 1 and all(r in listed for r in recipe_classes), "{0} listed".format(len(listed)))
    dependencies = [d for d in (_get(back, "mSchematicDependencies") or []) if _is_purchased_dependency(d)]
    gated = dependencies and milestone in list(_get(dependencies[0], "mSchematics") or [])
    _check("schematic: gated on " + MILESTONE_DISPLAY, bool(gated))


def main():
    _log("acpr_buildables.py")
    names = {BUILD_GUN, BUILD_CATEGORY, BUILD_SUB_CATEGORY, SHOP_CATEGORY, SHOP_SUB_CATEGORY, COUPON_DESC,
             VANILLA_BEAM}
    displays = {name for part in PARTS for name, _ in part[5]}
    by_name, by_display, schematics = _scan_vanilla(names, displays)
    for name in sorted(names):
        _check("vanilla asset found: " + name, by_name.get(name) is not None)
    for name in sorted(displays):
        _check("vanilla item found: " + name, by_display.get(name) is not None)
    build_modes = _beam_build_modes(by_name)

    recipes = []
    for part in PARTS:
        recipe = _buildable(part, (by_name, by_display), build_modes)
        if recipe is not None:
            recipes.append(recipe)
    _schematic(recipes, by_name, by_display, schematics)

    failed = [label for label, ok, _ in _checks if not ok]
    _log("")
    _log("{0} check(s), {1} failed".format(len(_checks), len(failed)))
    for label in failed:
        _log("  FAIL " + label)


try:
    main()
finally:
    _write_log()
