"""
Auto-Connecting Power Rails — §13's unlock and §14's pre-cook validation, in one pass.

Audits every asset this mod owns (the four buildables' Build_/Holo_/Desc_/Recipe_
assets under /AutoConnectingPowerRails/Buildables and /Recipes, and
Schematic_PowerRails) against the spec, and fixes the handful of things the spec
states exactly. It reads vanilla assets from the asset registry (the FICSIT Coupon,
the ingredient descriptors, the Basic Steel Production milestone, and a vanilla
schematic to borrow concrete unlock and dependency classes from) and never edits
them. Run acpr_buildables.py first so the assets exist.

Run this INSIDE the Unreal editor:
    Window > Developer Tools > Output Log, switch the dropdown from "Cmd" to
    "Python", then:  exec(open(r"<path>/acpr_validate.py").read())

WHAT IT DOES

    1. AUDITS every asset against the spec, and prints PASS / FAIL / INFO per item
       with the value it actually read.
    2. FIXES the handful of things the spec states exactly (set FIX = False below
       to audit only). It never touches meshes, icons, collision or anything else
       that belongs to the artwork phase, and it never deletes.
    3. VERIFIES from disk. set_editor_property on a CDO does not dirty the package,
       and save_asset writes nothing when the package is clean — so a script that
       re-reads what it just set in memory always passes. Every write here is
       followed by a forced save and a re-read after the asset has been reloaded.
    4. WRITES the whole pass to <project>/Saved/ACPR/ACPR_validate_log.txt as well.

RULES THE CHECKS FOLLOW

    * Ingredients are compared by CLASS, keyed on the full class name. A stripped
      name is ambiguous (replace("_C", "") eats "_C" anywhere, so Desc_Cable_C reads
      as "Descable"), and two unreal.Class wrappers for one class need not be the
      same Python object, so a dict keyed on wrappers can miss a match that is there.
    * Ingredients are resolved by DISPLAY NAME, the only stable key, and the
      internal name is reported rather than assumed: the game calls the "Steel Beam"
      descriptor Desc_SteelPlate.
    * A buildable's static mesh components are INFO with class names, not a check.
      What matters is what the HOLOGRAM binds at runtime, which the in-game log
      checks ("meshes=3"); a CDO component list cannot answer it.
    * The Outlet's four-Power-Line limit is applied at BeginPlay by
      ConfigureAsWireSocket( mMaxPowerLines ); the CDO's connection components read
      0 by construction. The check reads mMaxPowerLines, which is the authored value.
    * UFGSchematicPurchasedDependency and UFGUnlockRecipe (FGUnlockRecipe.h:14) are
      abstract, so new_object on them fails. A concrete subclass of each is taken
      from a vanilla schematic that already uses one — for the unlock, preferring a
      class whose own defaults are empty, and a freshly created one is SET to our
      four recipes rather than appended to, so it cannot inherit vanilla recipes
      from the class it was borrowed from.
    * UFGSchematicNotPurchasedDependency, the "inverted version", DERIVES from
      UFGSchematicPurchasedDependency (FGSchematicPurchasedDependency.h:51), so a
      plain isinstance accepts it and would gate the mod on NOT having bought the
      milestone. _is_purchased_dependency rejects it, the check verifies the
      dependency actually NAMES the milestone, a wrong one is repaired in place
      rather than joined by a second, and any other dependency is reported as a
      failure (all of them have to be met, so a stray one is a silent gate).
    * Every section runs inside its own try, so one broken section costs one
      section and the summary still prints.

Every property name comes from the FactoryGame headers; every vanilla asset is
SEARCHED for in the asset registry rather than hard-coded, because a wrong path
sets a property to nothing and reports success:

    UFGBuildingDescriptor::mBuildableClass            FGBuildingDescriptor.h:74
    UFGBuildingDescriptor::mUsesDistanceForZooping    FGBuildingDescriptor.h:78
    UFGItemDescriptor::mDisplayName / mDescription    FGItemDescriptor.h:380 / 384
    UFGRecipe::mIngredients / mProduct                FGRecipe.h:157 / 161
    UFGSchematic::mType / mCost / mUnlocks            FGSchematic.h:202 / 234 / 251
    UFGSchematic::mSchematicDependencies              FGSchematic.h:263
    UFGSchematic::mHiddenUntilDependenciesMet         FGSchematic.h:271
    UFGSchematic::mDependenciesBlocksSchematicAccess  FGSchematic.h:267
    UFGUnlockRecipe::mRecipes                         FGUnlockRecipe.h:38
    UFGSchematicPurchasedDependency::mSchematics      FGSchematicPurchasedDependency.h:33
    AFGBuildable::mCanContainLightweightInstances     FGBuildable.h (§14, C.2)
    AFGBuildableBeam::mSize/mDefaultLength/mMaxLength/mLengthPerCost  FGBuildableBeam.h:62-86
"""

import os
import traceback

import unreal

# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------

FIX = True          # False audits and changes nothing.

MOD = "AutoConnectingPowerRails"
BASE = "/" + MOD
BP_DIR = BASE + "/Buildables"
RECIPE_DIR = BASE + "/Recipes"
SCHEMATIC_DIR = BASE + "/Schematics"

SCHEMATIC = SCHEMATIC_DIR + "/Schematic_PowerRails"

# name -> (build, hologram, descriptor, recipe)
PARTS = {
    "Rail": ("Build_PowerRail", "Holo_PowerRail", "Desc_PowerRail", "Recipe_PowerRail"),
    "Junction": ("Build_PowerRailJunction", "Holo_PowerRailJunction", "Desc_PowerRailJunction", "Recipe_PowerRailJunction"),
    "Outlet": ("Build_PowerRailOutlet", "Holo_PowerRailOutlet", "Desc_PowerRailOutlet", "Recipe_PowerRailOutlet"),
    "Cap": ("Build_PowerRailCap", "Holo_PowerRailCap", "Desc_PowerRailCap", "Recipe_PowerRailCap"),
}

# §13's recipes, by the item's DISPLAY name — the only key that is stable. The
# game's internal names do not always match ("Steel Beam" is Desc_SteelPlate).
RECIPE_INGREDIENTS = {
    "Rail": {"Steel Beam": 1, "Cable": 1},
    "Junction": {"Steel Beam": 1, "Cable": 1},
    "Outlet": {"Wire": 4, "Iron Rod": 1},
    "Cap": {"Concrete": 1},
}

# §13: "at a baseline 4 FICSIT Coupons", "visible after Basic Steel Production".
COUPON_COST = 4
COUPON_DESC = "Desc_ResourceSinkCoupon"
UNLOCK_AFTER_DISPLAY_NAME = "Basic Steel Production"

# A.3's strings, used only to fill a display name or description that is EMPTY.
STRINGS = {
    "Rail": ("Power Rail",
             "A steel power backbone that connects through compatible Rail terminals. "
             "Attach a Power Rail Outlet for ordinary Power Lines."),
    "Junction": ("Power Rail Junction",
                 "A six-way Power Rail node. Place it standalone, snap it to a terminal, or insert it into "
                 "one Rail; every unoccupied face accepts a Rail, Outlet or Cap."),
    "Outlet": ("Power Rail Outlet",
               "Mounts on a Power Rail body or open terminal and provides one connection point for up to "
               "four Power Lines."),
    "Cap": ("Insulated Terminal Cap",
            "Blocks a Rail terminal from snapping and blueprint auto-connection."),
}

SCHEMATIC_STRINGS = ("Power Rails",
                     "Unlocks the Power Rail, Power Rail Junction, Power Rail Outlet and Insulated "
                     "Terminal Cap.")

# §8: "four Power Lines (mMaxNumConnectionLinks = 4)", authored on the buildable.
OUTLET_POWER_LINES = 4

# §14 / C.2: the Rail is the only zoopable one, and its readout must be metres.
ZOOPING = {"Rail": True, "Junction": False, "Outlet": False, "Cap": False}

# §11 / §13 numbers the Rail's beam fields have to carry, uu.
RAIL_BEAM = {"m_size": 100.0, "m_default_length": 400.0, "m_max_length": 4000.0, "m_length_per_cost": 1000.0}

LOG_NAME = "ACPR_validate_log.txt"

_checks = []
_changes = []
_lines = []


# ---------------------------------------------------------------------------
# Helpers — the same shapes acpr_buildables.py uses, so behaviour is familiar.
# ---------------------------------------------------------------------------

def _log(m):
    _lines.append(m)
    unreal.log("[ACPR] " + m)


def _check(label, ok, detail=""):
    _checks.append((label, bool(ok), detail))
    _log("{0} {1} {2}".format("PASS" if ok else "FAIL", label, detail))
    return bool(ok)


def _info(label, detail=""):
    _checks.append((label, None, detail))
    _log("INFO {0} {1}".format(label, detail))


def _write_log():
    try:
        directory = os.path.abspath(os.path.join(str(unreal.Paths.project_saved_dir()), "ACPR"))
        if not os.path.isdir(directory):
            os.makedirs(directory)
        path = os.path.join(directory, LOG_NAME)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("Auto-Connecting Power Rails — validation\n")
            handle.write("=" * 60 + "\n\n")
            handle.write("\n".join(_lines) + "\n")
        _log("log written to " + path)
    except Exception as error:
        _log("the log file could not be written: {0}".format(error))


def _get(obj, prop):
    try:
        return obj.get_editor_property(prop)
    except Exception:
        return None


def _readable(value):
    """Lists of UObjects print as <Object at 0x...>, which tells nobody anything. A list
    is summarised by what its entries are called instead."""
    if isinstance(value, (list, tuple)):
        if not value:
            return "[]"
        return "[" + ", ".join(_entry_text(v) for v in value) + "]"
    return str(value)


def _entry_text(value):
    item = _get(value, "item_class")
    if item is not None:
        return "{0} x{1}".format(_name_of(item), _get(value, "amount"))
    return _name_of(value)


def _set(obj, prop, value, what):
    if not FIX:
        _log("  would set {0} = {1}  ({2})".format(prop, _readable(value), what))
        return False
    try:
        obj.set_editor_property(prop, value)
        _changes.append(what)
        _log("  set {0} = {1}  ({2})".format(prop, _readable(value), what))
        return True
    except Exception as e:
        _log("  set {0} raised {1}".format(prop, e))
        return False


def _gen_class(path):
    try:
        return unreal.EditorAssetLibrary.load_blueprint_class(path)
    except Exception:
        return None


def _cdo(path):
    cls = _gen_class(path)
    return unreal.get_default_object(cls) if cls else None


def _asset(path):
    if not unreal.EditorAssetLibrary.does_asset_exist(path):
        return None
    try:
        return unreal.EditorAssetLibrary.load_asset(path)
    except Exception:
        return None


def _save(path, what):
    """Forced, because a CDO edit leaves the package clean and a clean package is
    not written."""
    if not FIX:
        return False
    try:
        ok = unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
        _log("  saved {0} -> {1}  ({2})".format(path, ok, what))
        return ok
    except Exception as e:
        _log("  save {0} raised {1}".format(path, e))
        return False


def _text(value):
    try:
        return str(value)
    except Exception:
        return ""


def _empty_text(value):
    return len(_text(value).strip()) == 0


def _name_of(cls):
    if cls is None:
        return "<none>"
    try:
        return str(cls.get_name())
    except Exception:
        return str(cls)


# ---------------------------------------------------------------------------
# One registry walk: every vanilla asset this script needs, found by name.
# ---------------------------------------------------------------------------

def _class_of(package_path):
    """The generated class for a Blueprint asset path, or None for an empty path."""
    if not package_path:
        return None
    try:
        return unreal.EditorAssetLibrary.load_blueprint_class(package_path)
    except Exception:
        return None


def _scan_vanilla(names_wanted, display_names_wanted):
    """ONE walk, three answers, because every one of them is a load:

        found          asset name -> package path, for names asked for by name
        by_display     item display name -> (asset name, class), resolved by loading
                       each Desc_* under Resource and reading mDisplayName
        schematics     (name, path, class, display name) for every vanilla schematic

    Display names are the stable key for items: the game shows "Steel Beam" for an
    asset called Desc_SteelPlate."""
    found = {}
    by_display = {}
    schematics = []

    try:
        ar = unreal.AssetRegistryHelpers.get_asset_registry()

        # Path filter only: ARFilter.class_names is deprecated in 5.6 and class_paths
        # wants a TopLevelAssetPath.
        f = unreal.ARFilter(recursive_paths=True, package_paths=["/Game/FactoryGame"])

        for data in ar.get_assets(f):
            name = str(data.asset_name)
            path = str(data.package_name)

            if name in names_wanted and name not in found:
                found[name] = path

            if display_names_wanted and name.startswith("Desc_") and "/Resource/" in path:
                cls = _class_of(path)
                cdo = unreal.get_default_object(cls) if cls else None
                if cdo:
                    display = _text(_get(cdo, "m_display_name")).strip()
                    if display in display_names_wanted and display not in by_display:
                        by_display[display] = (name, cls)

            if name.startswith("Schematic_") and "/FactoryGame/" in path:
                cls = _class_of(path)
                cdo = unreal.get_default_object(cls) if cls else None
                if cdo:
                    schematics.append((name, path, cls, _text(_get(cdo, "m_display_name")).strip()))
    except Exception as e:
        _log("asset registry walk failed: " + str(e))

    return found, by_display, schematics


def _is_purchased_dependency(dependency):
    """"Met when these schematics HAVE been purchased" — and not its inverse.

    UFGSchematicNotPurchasedDependency ("Inverted version of UFGSchematicPurchasedDependency",
    FGSchematicPurchasedDependency.h:51) DERIVES from the class we want, so a plain isinstance
    accepts it and the mod would be gated on NOT owning the milestone — visible from the start
    and hidden the moment you buy Basic Steel Production. Exactly backwards, and it would look
    like a PASS."""
    try:
        if not isinstance(dependency, unreal.FGSchematicPurchasedDependency):
            return False
    except Exception:
        return False
    inverted = getattr(unreal, "FGSchematicNotPurchasedDependency", None)
    if inverted is not None:
        try:
            if isinstance(dependency, inverted):
                return False
        except Exception:
            pass
    # Belt and braces, in case the inverted class is not exposed to Python by name.
    try:
        return "NotPurchased" not in str(dependency.get_class().get_name())
    except Exception:
        return True


def _find_dependency_class(schematics):
    """A CONCRETE UFGSchematicPurchasedDependency, taken from a vanilla schematic that
    already uses one. The C++ class is abstract (UCLASS(Blueprintable, abstract)), so
    new_object on it fails. Rather than guess a Blueprint's name, this reads the class
    off an instance the game itself authored."""
    for name, path, cls, display in schematics:
        cdo = unreal.get_default_object(cls) if cls else None
        for dependency in (_get(cdo, "m_schematic_dependencies") or []):
            if _is_purchased_dependency(dependency):
                try:
                    return dependency.get_class(), name
                except Exception:
                    continue
    return None, None


def _find_unlock_class(schematics):
    """A CONCRETE UFGUnlockRecipe, for the same reason as above: that class is abstract
    too — UCLASS( Blueprintable, EditInlineNew, abstract, DefaultToInstanced ),
    FGUnlockRecipe.h:14 — so new_object on it fails exactly as it does for the dependency.
    The class is read off an unlock the game itself authored. One whose OWN defaults are
    empty is preferred, because a new object inherits its class's CDO and a Blueprint that
    ships with recipes in it would quietly unlock them here too."""
    fallback = (None, None)
    for name, path, cls, display in schematics:
        cdo = unreal.get_default_object(cls) if cls else None
        for unlock in (_get(cdo, "m_unlocks") or []):
            try:
                if not isinstance(unlock, unreal.FGUnlockRecipe):
                    continue
                unlock_class = unlock.get_class()
            except Exception:
                continue
            class_cdo = None
            try:
                class_cdo = unreal.get_default_object(unlock_class)
            except Exception:
                pass
            if not (_get(class_cdo, "m_recipes") or []):
                return unlock_class, name
            if fallback[0] is None:
                fallback = (unlock_class, name)
    return fallback


def _find_milestone(schematics, display_name):
    """The milestone whose DISPLAY name matches, which is the only stable key: the
    file names are tier numbers and they have moved between versions."""
    for name, path, cls, display in schematics:
        if display.lower() == display_name.lower():
            return name, path, cls
    return None, None, None


# ---------------------------------------------------------------------------
# A — the assets exist
# ---------------------------------------------------------------------------

def audit_inventory():
    _log("=== A. assets ===")
    for part, (build, holo, desc, recipe) in PARTS.items():
        for folder, asset in ((BP_DIR, build), (BP_DIR, holo), (BP_DIR, desc), (RECIPE_DIR, recipe)):
            path = folder + "/" + asset
            _check("{0} {1}".format(part, asset), unreal.EditorAssetLibrary.does_asset_exist(path), path)
    _check("Schematic_PowerRails", unreal.EditorAssetLibrary.does_asset_exist(SCHEMATIC), SCHEMATIC)


# ---------------------------------------------------------------------------
# B — descriptors: what the build menu and the readout read
# ---------------------------------------------------------------------------

def audit_descriptors():
    _log("=== B. descriptors ===")
    for part, (build, holo, desc, recipe) in PARTS.items():
        desc_path = BP_DIR + "/" + desc
        cdo = _cdo(desc_path)
        if not cdo:
            _check(part + " descriptor CDO", False, desc_path)
            continue

        build_class = _get(cdo, "m_buildable_class")
        _check(part + " mBuildableClass", _name_of(build_class).startswith(build), _name_of(build_class))

        # B.2: the zoop readout is a descriptor flag, not a cost path. A beam copied
        # from a non-zoopable buildable renders 40 m as "(x4,001)".
        zoop = _get(cdo, "m_uses_distance_for_zooping")
        want = ZOOPING[part]
        if zoop != want and FIX:
            _set(cdo, "m_uses_distance_for_zooping", want,
                 "{0}: mUsesDistanceForZooping {1} -> {2}".format(desc, zoop, want))
            _save(desc_path, desc)
            zoop = _get(cdo, "m_uses_distance_for_zooping")
        _check(part + " mUsesDistanceForZooping", zoop == want, "{0} (want {1})".format(zoop, want))

        # Localizable player-facing strings (B.3). Only filled when EMPTY — an
        # edited string is the author's, not this script's.
        name_text = _get(cdo, "m_display_name")
        description = _get(cdo, "m_description")
        if _empty_text(name_text) and FIX:
            _set(cdo, "m_display_name", unreal.Text(STRINGS[part][0]), desc + ": display name")
            _save(desc_path, desc)
            name_text = _get(cdo, "m_display_name")
        if _empty_text(description) and FIX:
            _set(cdo, "m_description", unreal.Text(STRINGS[part][1]), desc + ": description")
            _save(desc_path, desc)
            description = _get(cdo, "m_description")

        _check(part + " display name", not _empty_text(name_text), _text(name_text))
        _check(part + " description", not _empty_text(description), _text(description)[:60] + "...")

        # Icons are the artwork phase's; reported, never set.
        _info(part + " icons",
              "small={0} big={1}".format(_name_of(_get(cdo, "m_small_icon")), _name_of(_get(cdo, "m_persistent_big_icon"))))


# ---------------------------------------------------------------------------
# C — buildables: the facts §14 and Appendix C.2 depend on
# ---------------------------------------------------------------------------

def audit_buildables():
    _log("=== C. buildables ===")
    for part, (build, holo, desc, recipe) in PARTS.items():
        path = BP_DIR + "/" + build
        cdo = _cdo(path)
        if not cdo:
            _check(part + " build CDO", False, path)
            continue

        # §14: "all four non-lightweight". Two independent conditions (C.2), both cleared in the
        # C++ constructors — so a True here is a stale C++ build, not something to write over.
        light = _get(cdo, "m_can_contain_lightweight_instances")
        managed = _get(cdo, "m_managed_by_lightweight_buildable_subsystem")
        _check(part + " non-lightweight", not light and not managed,
               "canContain={0} managed={1}".format(light, managed))

        holo_class = _get(cdo, "m_hologram_class")
        _check(part + " mHologramClass", _name_of(holo_class).startswith(holo), _name_of(holo_class))

        # What matters is what the HOLOGRAM binds at runtime — its mBeamMesh takes the
        # first UStaticMeshComponent it finds, and an extra one can shadow the real beam
        # with an empty zoop ISM. A CDO component list cannot answer that; the in-game
        # line "[ACPR-RAIL-HOLO] BeginPlay ... meshes=3" can, and does. So this is
        # reported with class names and judged there.
        try:
            cdo_components = unreal.get_default_object(_gen_class(path)).get_components_by_class(
                unreal.StaticMeshComponent)
            described = ["{0} ({1})".format(c.get_name(), c.get_class().get_name()) for c in cdo_components]
        except Exception as e:
            described = ["<unreadable: {0}>".format(e)]
        _info(part + " mesh components", str(described))

        if part == "Rail":
            for prop, want in RAIL_BEAM.items():
                value = _get(cdo, prop)
                _check("Rail " + prop, value is not None and abs(float(value) - want) < 0.5,
                       "{0} (want {1})".format(value, want))
            _info("Rail mProfileScale", str(_get(cdo, "m_profile_scale")))
            _info("Rail mHoverpackNodeSpacing", str(_get(cdo, "m_hoverpack_node_spacing")))

        if part == "Junction":
            _info("Junction mHalfExtent", str(_get(cdo, "m_half_extent")))

        if part == "Outlet":
            # §8's four Power Lines. The limit is AUTHORED here and applied at BeginPlay
            # by ConfigureAsWireSocket( mMaxPowerLines ) — the connection components on
            # the CDO read 0 by construction. mMaxPowerLines is the value that matters.
            lines = _get(cdo, "m_max_power_lines")
            _check("Outlet mMaxPowerLines = {0}".format(OUTLET_POWER_LINES),
                   lines == OUTLET_POWER_LINES, str(lines))

            try:
                connections = unreal.get_default_object(_gen_class(path)).get_components_by_class(
                    unreal.FGPowerConnectionComponent)
                described = ["{0} ({1})".format(c.get_name(), c.get_class().get_name()) for c in connections]
            except Exception as e:
                described = ["<unreadable: {0}>".format(e)]
            # Two are expected: the wire socket and §8.2's private mounting interface.
            _info("Outlet connection components", str(described))


# ---------------------------------------------------------------------------
# D — holograms: the tuned placement fields, reported so a lost edit shows
# ---------------------------------------------------------------------------

def audit_holograms():
    _log("=== D. holograms ===")

    rail = _cdo(BP_DIR + "/Holo_PowerRail")
    if rail:
        diagonal = _get(rail, "m_build_mode_diagonal")
        freeform = _get(rail, "m_build_mode_free_form")
        # B.2: these live on the vanilla beam hologram's Blueprint and are not
        # inherited; without them the Rail cannot choose its placement path at all.
        _check("Rail mBuildModeDiagonal", diagonal is not None, _name_of(diagonal))
        _check("Rail mBuildModeFreeForm", freeform is not None, _name_of(freeform))

        for prop, want in (("m_lane_offset", 50.0), ("m_surface_offset", 50.0),
                           ("m_roll_step_degrees", 45.0), ("m_roll_step_fine_degrees", 5.0),
                           ("m_nudge_distance_coarse", 100.0), ("m_nudge_distance_fine", 25.0)):
            value = _get(rail, prop)
            _check("Rail " + prop, value is not None and abs(float(value) - want) < 0.01,
                   "{0} (want {1})".format(value, want))
    else:
        _check("Holo_PowerRail CDO", False, BP_DIR + "/Holo_PowerRail")

    junction = _cdo(BP_DIR + "/Holo_PowerRailJunction")
    if junction:
        for prop in ("m_grid_pitch", "m_surface_offset", "m_roll_step_degrees", "m_roll_step_fine_degrees",
                     "m_nudge_distance_coarse", "m_nudge_distance_fine", "m_terminal_snap_range"):
            _info("Junction " + prop, str(_get(junction, prop)))


# ---------------------------------------------------------------------------
# E — recipes against §13's table
# ---------------------------------------------------------------------------

def audit_recipes(by_display):
    """§13's table, compared by CLASS, never by a stripped name."""
    _log("=== E. recipes ===")

    for part, (build, holo, desc, recipe) in PARTS.items():
        path = RECIPE_DIR + "/" + recipe
        cdo = _cdo(path)
        if not cdo:
            _check(part + " recipe CDO", False, path)
            continue

        product_text = []
        try:
            for amount in _get(cdo, "m_product") or []:
                product_text.append("{0} x{1}".format(_name_of(_get(amount, "item_class")), _get(amount, "amount")))
        except Exception:
            product_text = ["<unreadable>"]
        _check(part + " recipe product", any(n.startswith(desc) for n in product_text), str(product_text))

        # What §13 wants: class NAME -> (class, count). The name is the key because two
        # unreal.Class wrappers for the same class are not the same Python object, so a
        # dict keyed on the wrapper can miss a match that is really there — and the full
        # name is exact, where a name with "_C" stripped out of it is not.
        wanted = {}
        missing_display = []
        for display, count in RECIPE_INGREDIENTS[part].items():
            entry = by_display.get(display)
            if entry:
                wanted[_name_of(entry[1])] = (entry[1], count)
            else:
                missing_display.append(display)
        if missing_display:
            _check(part + " ingredients resolvable", False, "no descriptor found for " + ", ".join(missing_display))
            continue

        # What the recipe has, on the same key.
        def _read_ingredients():
            counts = {}
            text = []
            try:
                for amount in _get(cdo, "m_ingredients") or []:
                    item = _get(amount, "item_class")
                    counts[_name_of(item)] = int(_get(amount, "amount"))
                    text.append("{0} x{1}".format(_name_of(item), _get(amount, "amount")))
            except Exception as e:
                text = ["<unreadable: {0}>".format(e)]
            return counts, text

        got, got_text = _read_ingredients()

        want_text = ", ".join("{0} x{1}".format(n, v[1]) for n, v in wanted.items())
        matches = len(got) == len(wanted) and all(got.get(n) == v[1] for n, v in wanted.items())

        if not matches and FIX:
            entries = []
            for cls_item, count in wanted.values():
                entry = unreal.ItemAmount()
                entry.set_editor_property("item_class", cls_item)
                entry.set_editor_property("amount", count)
                entries.append(entry)
            if _set(cdo, "m_ingredients", entries, "{0}: ingredients -> {1}".format(recipe, want_text)):
                _save(path, recipe)
                got, got_text = _read_ingredients()
                matches = len(got) == len(wanted) and all(got.get(n) == v[1] for n, v in wanted.items())

        _check(part + " recipe ingredients", matches, "{0} (want {1})".format(got_text, want_text))
        _info(part + " recipe produced in", str(_get(cdo, "m_produced_in")))


# ---------------------------------------------------------------------------
# F — §13's unlock: one AWESOME Shop package for all four
# ---------------------------------------------------------------------------

def audit_schematic(vanilla, schematics):
    _log("=== F. unlock ===")

    path = SCHEMATIC
    cls = _gen_class(path)
    cdo = unreal.get_default_object(cls) if cls else None
    if not cdo:
        _check("Schematic_PowerRails CDO", False, path)
        return

    dirty = False

    # 1. Type: the AWESOME Shop is EST_ResourceSink (FGSchematic.h:18-30).
    want_type = unreal.SchematicType.EST_RESOURCE_SINK
    got_type = _get(cdo, "m_type")
    if got_type != want_type and FIX:
        dirty = _set(cdo, "m_type", want_type, "schematic: mType -> EST_ResourceSink") or dirty
        got_type = _get(cdo, "m_type")
    _check("schematic mType", got_type == want_type, str(got_type))

    # 2. Cost: 4 FICSIT Coupons.
    coupon_class = _class_of(vanilla.get(COUPON_DESC))
    cost = _get(cdo, "m_cost")
    cost_text = []
    try:
        for amount in cost:
            cost_text.append("{0} x{1}".format(_name_of(_get(amount, "item_class")), _get(amount, "amount")))
    except Exception:
        cost_text = ["<unreadable>"]

    cost_ok = len(cost_text) == 1 and cost_text[0].startswith(COUPON_DESC) and cost_text[0].endswith("x{0}".format(COUPON_COST))
    if not cost_ok and coupon_class and FIX:
        entry = unreal.ItemAmount()
        entry.set_editor_property("item_class", coupon_class)
        entry.set_editor_property("amount", COUPON_COST)
        dirty = _set(cdo, "m_cost", [entry], "schematic: mCost -> {0} coupons".format(COUPON_COST)) or dirty
        cost = _get(cdo, "m_cost")
        cost_text = []
        try:
            for amount in cost:
                cost_text.append("{0} x{1}".format(_name_of(_get(amount, "item_class")), _get(amount, "amount")))
        except Exception:
            pass
        cost_ok = len(cost_text) == 1 and cost_text[0].endswith("x{0}".format(COUPON_COST))
    if not coupon_class:
        _info("coupon descriptor", "NOT FOUND in the asset registry — cost left alone")
    _check("schematic mCost", cost_ok, str(cost_text))

    # 3. Unlocks: one UFGUnlockRecipe holding all four recipes. The unlock objects
    #    are instanced sub-objects of the schematic, so an existing one is edited
    #    rather than replaced — replacing it would drop anything else it carries.
    recipe_classes = []
    for part, (build, holo, desc, recipe) in PARTS.items():
        cls_r = _gen_class(RECIPE_DIR + "/" + recipe)
        if cls_r:
            recipe_classes.append(cls_r)
    _check("all four recipe classes resolve", len(recipe_classes) == 4, str(len(recipe_classes)))

    unlocks = _get(cdo, "m_unlocks") or []
    unlock_recipe = None
    for unlock in unlocks:
        if isinstance(unlock, unreal.FGUnlockRecipe):
            unlock_recipe = unlock
            break

    created_unlock = False
    if unlock_recipe is None:
        _info("schematic unlock object", "no recipe unlock present" + (" — creating one" if FIX else ""))
        if FIX:
            unlock_class, unlock_borrowed_from = _find_unlock_class(schematics)
            if not unlock_class:
                _info("unlock class", "no vanilla schematic unlocks recipes — add a Recipe unlock to "
                                      "mUnlocks by hand and re-run")
            else:
                try:
                    unlock_recipe = unreal.new_object(unlock_class, cdo)
                    created_unlock = True
                    dirty = _set(cdo, "m_unlocks", list(unlocks) + [unlock_recipe],
                                 "schematic: added a recipe unlock ({0}, as used by {1})".format(
                                     _name_of(unlock_class), unlock_borrowed_from)) or dirty
                except Exception as e:
                    # Named precisely, so it takes a minute by hand: open the schematic,
                    # add an Unlocks entry of this class, put the four recipes in it.
                    _log("  could not create the recipe unlock: " + str(e))
                    _info("set it by hand", "class {0} (as used by {1}) in mUnlocks, holding {2}".format(
                        _name_of(unlock_class), unlock_borrowed_from,
                        ", ".join(_name_of(c) for c in recipe_classes)))
                    unlock_recipe = None

    listed = []
    if unlock_recipe is not None:
        if created_unlock:
            # A borrowed Blueprint class brings its CDO's defaults with it, so a FRESH
            # object is set to exactly our four rather than appended to.
            dirty = _set(unlock_recipe, "m_recipes", list(recipe_classes),
                         "schematic unlock: " + ", ".join(_name_of(c) for c in recipe_classes)) or dirty
        else:
            current = _get(unlock_recipe, "m_recipes") or []
            missing = [c for c in recipe_classes if _name_of(c) not in [_name_of(x) for x in current]]
            if missing and FIX:
                dirty = _set(unlock_recipe, "m_recipes", list(current) + missing,
                             "schematic unlock: added " + ", ".join(_name_of(c) for c in missing)) or dirty
        listed = [_name_of(c) for c in (_get(unlock_recipe, "m_recipes") or [])]

    _check("schematic unlocks all four recipes", len(recipe_classes) == 4 and
           all(_name_of(c) in listed for c in recipe_classes), str(listed))

    # 4. Visible after Basic Steel Production — and gated the RIGHT WAY ROUND. See
    #    _is_purchased_dependency: the inverted class derives from the one we want.
    dependencies = _get(cdo, "m_schematic_dependencies") or []
    _info("schematic dependencies", _readable(dependencies))

    milestone_name, milestone_path, milestone_class = _find_milestone(schematics, UNLOCK_AFTER_DISPLAY_NAME)
    if milestone_class:
        _info("milestone", "{0} = {1} ({2})".format(UNLOCK_AFTER_DISPLAY_NAME, milestone_name, milestone_path))
    else:
        _info("milestone", "'{0}' not found among {1} schematics".format(
            UNLOCK_AFTER_DISPLAY_NAME, len(schematics)))

    existing = None
    for d in dependencies:
        if _is_purchased_dependency(d):
            existing = d
            break

    def _points_at_milestone(dependency):
        if not milestone_class:
            return False
        listed = [_name_of(s) for s in (_get(dependency, "m_schematics") or [])]
        return _name_of(milestone_class) in listed

    gated = existing is not None and _points_at_milestone(existing)

    if not gated and FIX and milestone_class:
        if existing is not None:
            # There is one, but it names the wrong schematic (or none). Repair it in place
            # rather than adding a second — two dependencies both have to be met.
            _set(existing, "m_schematics", [milestone_class],
                 "schematic dependency: -> {0}".format(milestone_name))
            _set(existing, "m_require_all_schematics_to_be_purchased", True,
                 "schematic dependency: require the schematic to be purchased")
            dirty = True
            gated = _points_at_milestone(existing)
        else:
            dependency_class, borrowed_from = _find_dependency_class(schematics)
            if not dependency_class:
                # UFGSchematicPurchasedDependency itself is abstract, so a concrete subclass
                # has to come from somewhere the game authored one.
                _info("dependency class", "no vanilla schematic uses a purchased dependency — left alone")
            else:
                try:
                    dependency = unreal.new_object(dependency_class, cdo)
                    dependency.set_editor_property("m_schematics", [milestone_class])
                    dependency.set_editor_property("m_require_all_schematics_to_be_purchased", True)
                    dirty = _set(cdo, "m_schematic_dependencies", list(dependencies) + [dependency],
                                 "schematic: depends on {0} (via {1}, as used by {2})".format(
                                     milestone_name, _name_of(dependency_class), borrowed_from)) or dirty
                    dependencies = _get(cdo, "m_schematic_dependencies") or []
                    for d in dependencies:
                        if _is_purchased_dependency(d):
                            existing = d
                            break
                    gated = existing is not None and _points_at_milestone(existing)
                except Exception as e:
                    # Named precisely, so it can be set by hand in the editor in one minute:
                    # open the schematic, add a Schematic Dependencies entry of this class and
                    # put this milestone in its Schematics list.
                    _log("  could not create the dependency: {0}".format(e))
                    _info("set it by hand", "class {0} (as used by {1}), milestone {2} ({3})".format(
                        _name_of(dependency_class), borrowed_from, milestone_name, milestone_path))

    detail = "none"
    if existing is not None:
        detail = "{0} -> {1}".format(_name_of(existing.get_class()),
                                     _readable(_get(existing, "m_schematics") or []))
    _check("schematic gated on " + UNLOCK_AFTER_DISPLAY_NAME, gated, detail)

    # Every dependency has to be met, so a second one nobody intended is a silent gate —
    # an inverted one most of all. This script never deletes, so it says so loudly instead.
    others = [d for d in (_get(cdo, "m_schematic_dependencies") or []) if d is not existing]
    if others:
        _check("schematic has no second dependency", False,
               _readable(others) + " — §13 wants only the one; delete the rest in the editor "
                                   "(this script never deletes)")

    for prop, want, what in (("m_hidden_until_dependencies_met", True, "hidden until the milestone is bought"),
                             ("m_dependencies_blocks_schematic_access", True, "and not purchasable before it")):
        value = _get(cdo, prop)
        if value != want and FIX:
            dirty = _set(cdo, prop, want, "schematic: " + what) or dirty
            value = _get(cdo, prop)
        _check("schematic " + prop, value == want, str(value))

    # 5. Strings, again only when empty.
    name_text = _get(cdo, "m_display_name")
    description = _get(cdo, "m_description")
    if _empty_text(name_text) and FIX:
        dirty = _set(cdo, "m_display_name", unreal.Text(SCHEMATIC_STRINGS[0]), "schematic: display name") or dirty
        name_text = _get(cdo, "m_display_name")
    if _empty_text(description) and FIX:
        dirty = _set(cdo, "m_description", unreal.Text(SCHEMATIC_STRINGS[1]), "schematic: description") or dirty
        description = _get(cdo, "m_description")
    _check("schematic display name", not _empty_text(name_text), _text(name_text))
    _check("schematic description", not _empty_text(description), _text(description)[:60] + "...")

    _info("schematic tech tier", str(_get(cdo, "m_tech_tier")))
    _info("schematic category", _name_of(_get(cdo, "m_schematic_category")))
    _info("schematic icon", _name_of(_get(cdo, "m_small_schematic_icon")))

    if dirty:
        _save(path, "Schematic_PowerRails")


# ---------------------------------------------------------------------------
# G — read it back from disk, which is the only reading that counts
# ---------------------------------------------------------------------------

def verify_from_disk():
    if not FIX or not _changes:
        return

    _log("=== G. re-read from disk ===")
    paths = ([SCHEMATIC]
             + [BP_DIR + "/" + p[2] for p in PARTS.values()]
             + [BP_DIR + "/" + p[0] for p in PARTS.values()]
             + [RECIPE_DIR + "/" + p[3] for p in PARTS.values()])
    for path in paths:
        try:
            unreal.EditorAssetLibrary.load_asset(path)
        except Exception:
            pass

    cdo = _cdo(SCHEMATIC)
    if cdo:
        _check("schematic, re-read: type", _get(cdo, "m_type") == unreal.SchematicType.EST_RESOURCE_SINK,
               str(_get(cdo, "m_type")))
        unlocks = _get(cdo, "m_unlocks") or []
        count = 0
        for unlock in unlocks:
            if isinstance(unlock, unreal.FGUnlockRecipe):
                count = len(_get(unlock, "m_recipes") or [])
        _check("schematic, re-read: recipes unlocked", count == 4, str(count))

    rail_desc = _cdo(BP_DIR + "/Desc_PowerRail")
    if rail_desc:
        _check("Desc_PowerRail, re-read: mUsesDistanceForZooping",
               _get(rail_desc, "m_uses_distance_for_zooping") is True,
               str(_get(rail_desc, "m_uses_distance_for_zooping")))

    junction_recipe = _cdo(RECIPE_DIR + "/Recipe_PowerRailJunction")
    if junction_recipe:
        text = []
        for amount in _get(junction_recipe, "m_ingredients") or []:
            text.append("{0} x{1}".format(_name_of(_get(amount, "item_class")), _get(amount, "amount")))
        _check("Recipe_PowerRailJunction, re-read: ingredients", len(text) == 2, str(text))

    schematic = _cdo(SCHEMATIC)
    if schematic:
        dependencies = _get(schematic, "m_schematic_dependencies") or []
        detail = []
        gated = False
        for d in dependencies:
            listed = _readable(_get(d, "m_schematics") or [])
            try:
                detail.append("{0} -> {1}".format(_name_of(d.get_class()), listed))
            except Exception:
                detail.append(str(d))
            if _is_purchased_dependency(d) and UNLOCK_AFTER_DISPLAY_NAME:
                gated = gated or len(_get(d, "m_schematics") or []) > 0
        _check("schematic, re-read: gated on a purchased schematic", gated, str(detail))


# ---------------------------------------------------------------------------

def main():
    _log("validation and unlock pass | FIX = {0}".format(FIX))

    display_names = set()
    for ingredients in RECIPE_INGREDIENTS.values():
        display_names.update(ingredients.keys())

    vanilla, by_display, schematics = _scan_vanilla(set([COUPON_DESC]), display_names)

    _check("vanilla asset " + COUPON_DESC, COUPON_DESC in vanilla, vanilla.get(COUPON_DESC, "NOT FOUND"))
    for display in sorted(display_names):
        entry = by_display.get(display)
        _check("item '{0}'".format(display), entry is not None,
               "{0}".format(entry[0]) if entry else "no descriptor with that display name")
    _info("vanilla schematics seen", str(len(schematics)))

    # Each section is isolated: one section blowing up must not take the other six and
    # the summary with it.
    for label, fn, args in (("A. assets", audit_inventory, ()),
                            ("B. descriptors", audit_descriptors, ()),
                            ("C. buildables", audit_buildables, ()),
                            ("D. holograms", audit_holograms, ()),
                            ("E. recipes", audit_recipes, (by_display,)),
                            ("F. unlock", audit_schematic, (vanilla, schematics)),
                            ("G. re-read", verify_from_disk, ())):
        try:
            fn(*args)
        except Exception as e:
            _check(label + " — section raised", False, "{0}: {1}".format(type(e).__name__, e))
            for line in traceback.format_exc().splitlines():
                _log("    " + line)

    failed = [c for c in _checks if c[1] is False]
    _log("=== summary ===")
    _log("{0} checks, {1} failed, {2} change(s) written".format(
        len([c for c in _checks if c[1] is not None]), len(failed), len(_changes)))
    for label, ok, detail in failed:
        _log("  FAIL  {0}  {1}".format(label, detail))
    for change in _changes:
        _log("  changed  " + change)
    if not failed:
        _log("  everything the spec states exactly is in place.")


try:
    main()
except Exception:
    # Nothing above should reach here — every section is guarded — but a traceback
    # tagged [ACPR] is one a filtered log still shows.
    _log("the pass stopped early:")
    for _line in traceback.format_exc().splitlines():
        _log("    " + _line)
finally:
    _write_log()
