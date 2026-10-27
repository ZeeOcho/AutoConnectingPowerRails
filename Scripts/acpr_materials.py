"""
Auto-Connecting Power Rails — the material masters and instances, and the meshes wearing them.

Run this INSIDE the Unreal editor, AFTER acpr_meshes.py:
    Window > Developer Tools > Output Log, dropdown "Cmd" -> "Python", then:
    exec(open(r"<path>/acpr_materials.py").read())

What it makes, under /AutoConnectingPowerRails/Materials:

    MI_ACPR_Body         painted metal, a child of vanilla's MI_Factory_Base_01 with custom
                         data off, so the body never takes the swatch
    M_ACPR_Accent        a three-node master reading the swatch's primary colour straight
                         from custom primitive data; MI_ACPR_Accent is its instance
    M_ACPR_Cue           a four-parameter master with an emissive output; the state
                         instances MI_ACPR_Cue (open), MI_ACPR_CueCoupled, MI_ACPR_CuePowered,
                         MI_ACPR_CueCapped and MI_ACPR_CueSelected are its children
    M_ACPR_Link          §16.5's link line: an unlit additive master whose emissive is either a
                         faint line (Powered 0) or nothing but a bead travelling along the line
                         (Powered 1), built from the vertex's position along the line and the
                         instance's length; MI_ACPR_Link and MI_ACPR_LinkPowered are its two
                         instances, MI_ACPR_LinkEnd and MI_ACPR_LinkEndPowered the two for the
                         tube round a linked machine's socket, and the C++ swaps them per outlet
                         with the outlet's power
    M_ACPR_Range         an unlit, translucent, two-sided master with an emissive colour and
                         an opacity; MI_ACPR_Range, the range sphere, is its instance

It then puts them on every mesh acpr_meshes.py generated, by material slot NAME: "Body",
"Cue" / "Cue_+X"…, "Accent", "Power" / "Power_+X"…. Which cue state a slot wears at runtime
is the C++'s call (FACPRIndicators::Push) — the materials are swapped per slot, and no
custom data of ours is ever written, so vanilla's swatch preview keeps working.

What it reads: the body parent (vanilla's MI_Factory_Base_01), which must exist, and the
meshes acpr_meshes.py made. Every parameter name is validated against the parent before it
is set, and everything set is read back off the instance and logged, so a parameter that did
not take says so.

Verified names, from Epic's Python reference (unreal.MaterialEditingLibrary):
    set_material_instance_parent( instance, new_parent )
    set_material_instance_static_switch_parameter_value( instance, name, value, association )
    set_material_instance_scalar_parameter_value( instance, name, value, association )
    set_material_instance_vector_parameter_value( instance, name, value, association )
    get_material_instance_*_parameter_value( instance, name, association )
    get_material_default_scalar_parameter_value( material, parameter_name )
    get_material_default_vector_parameter_value( material, parameter_name )
    get_material_default_static_switch_parameter_value( material, parameter_name )
    update_material_instance( instance )
and unreal.StaticMaterial( material_interface, material_slot_name, uv_channel_data ).

Re-running is safe: existing assets are reused and re-set rather than duplicated.
"""

import os
import traceback

import unreal

# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------

MOD = "AutoConnectingPowerRails"

# WHICH FAMILY, AND WHY.
#
# The vanilla beams parent to MI_Beams_01 -> MM_FactoryBaked_Beams, a per-family master that
# the Starter Project GENERATES, and every generated master exposes ZERO static switches
# (the generator's EnsureStaticSwitchNodesPresent is commented out under "UE 5.2 now also
# marked StaticSwitchParameters as deprecated"). In Satisfactory the switches are the
# behaviour, so on that parent bUsePrimitiveCustomData cannot be set at author time.
#
# MM_Factory_Array is restored from the Starter Project, exposes all ten switches, and is what
# the docs call the primary material for buildable factory parts. The structural buildables —
# hyper tube poles, railings, pipe poles, walls — all wear MI_Factory_Base_01 or MI_Factory_01,
# which chain to it four instances deep:
#
#   MI_Factory_01 -> MI_Factory_Base_01_Emsiv_AO -> MI_Factory_Base_01
#                 -> MI_Factory2D_01 -> MM_Factory_Array
#
# So the body instances MI_Factory_Base_01, inheriting the texture choices that make a vanilla
# structural part look like one. The cue and the accent have their own masters (below).
BODY_PARENT_PATH = "/Game/FactoryGame/-Shared/Material/MI_Factory_Base_01"

MATERIAL_DIR = "/" + MOD + "/Materials"
MESH_DIR = "/" + MOD + "/Meshes"

BODY_PATH = MATERIAL_DIR + "/MI_ACPR_Body"
CUE_PATH = MATERIAL_DIR + "/MI_ACPR_Cue"
ACCENT_PATH = MATERIAL_DIR + "/MI_ACPR_Accent"

# ONE MATERIAL PER INDICATOR STATE, SWAPPED PER SLOT AT RUNTIME.
#
# With bUsePrimitiveCustomData on, the array master colours the light from the primary paint
# floats, and CDI_HasPower (index 6) is written to 1.0 by vanilla's own ApplyHasPowerCustomData
# whatever is put there. So the cue does not read custom data at all: each state is its own
# instance with fixed colours, and the C++ puts the right one in the right slot. Per-face
# Junction state comes free, because each face has its own cue slot, and no custom data of ours
# is ever written, which is what keeps vanilla's swatch PREVIEW intact.
CUE_OPEN_PATH = CUE_PATH                                 # amber: an open terminal, "unfinished here"
CUE_COUPLED_PATH = MATERIAL_DIR + "/MI_ACPR_CueCoupled"  # dim power colour: coupled, no power
CUE_POWERED_PATH = MATERIAL_DIR + "/MI_ACPR_CuePowered"  # power colour: coupled and live
CUE_CAPPED_PATH = MATERIAL_DIR + "/MI_ACPR_CueCapped"    # unlit: a capped terminal is OFF
# §12's highlight is one more state: while a hologram aims at a host, the exact target terminal
# wears Selected on its own cue surface instead of the open amber. Selected is the POWER colour,
# because that is the state the click will produce, and at eight times a powered cue it still
# stands out when the host it is on is already powered.
CUE_SELECTED_PATH = MATERIAL_DIR + "/MI_ACPR_CueSelected"  # power colour, x8: the hologram's exact target

# §16.5'S LINK LINE has its own master, M_ACPR_Link: unlit and ADDITIVE, so "no line" is simply
# zero emissive and a faint line is a small one — a translucent dim line would read as a dark
# streak against the sky. Its graph, in one sentence: along-length coordinate u = x / LINK_LENGTH
# from the vertex's instance-local position (the engine's LocalPosition node, origin Instance —
# the mesh is authored from x = 0 to LINK_LENGTH along +X), the line's length L from the
# instance's own transform (the unit +X vector carried from instance space to world space is
# LINK_LENGTH long on a unit instance, so its world length times LINK_LENGTH is the line's — no
# per-instance data to hand over), a bead centred at c(t) travelling from one
# bead-length before u = 0 to one bead-length past u = 1 every LINK_BEAD_PERIOD seconds — so it
# grows out of the outlet and shrinks into the machine — and
#     emissive = LinkColor * ( (1 - Powered) * max(dash(x), Solid) * LineBrightness + Powered * bead(u) * BeadBrightness ),
# where dash(x) is 1 on the first half of every 2 * BeadLength of the line and 0 on the second:
# the unpowered line is dashed, dash and gap each one bead long, so it cannot be read as a lit
# line and the powered bead is visibly the same unit of length. Solid (0 or 1) overrides the
# dash with an unbroken line: the placement preview's "this link will be made", which has to be
# read from where the player stands to judge the machines — far from the hologram.
# Powered is 0 on MI_ACPR_Link and 1 on MI_ACPR_LinkPowered; every other number is a scalar
# parameter with the defaults below, which are the visual tuning in one place. "Brightness" is
# a plain multiplier on the emissive colour — nothing falls off with distance or power.
LINK_MASTER_PATH = MATERIAL_DIR + "/M_ACPR_Link"
LINK_PATH = MATERIAL_DIR + "/MI_ACPR_Link"
LINK_POWERED_PATH = MATERIAL_DIR + "/MI_ACPR_LinkPowered"
# THE PREVIEW LINE (§16.5): the link a placement would make, solid and as bright as the selected
# cue — the two things the preview lights, the line and the aimed face, read as one.
LINK_PREVIEW_PATH = MATERIAL_DIR + "/MI_ACPR_LinkPreview"
# THE LINK END wears the same master on the tube round a linked machine's socket, with the bead
# a few centimetres long: the dashes become standing rings (unpowered) and the bead a band
# crawling up the socket (powered) — the link's own visual unit, read at the machine.
LINK_END_PATH = MATERIAL_DIR + "/MI_ACPR_LinkEnd"
LINK_END_POWERED_PATH = MATERIAL_DIR + "/MI_ACPR_LinkEndPowered"
# The inner tube, round the knob's core between its discs, half a period behind the outer one:
# the band climbs the outer tube, then the inner, so the two follow each other.
LINK_END_INNER_PATH = MATERIAL_DIR + "/MI_ACPR_LinkEndInner"
LINK_END_INNER_POWERED_PATH = MATERIAL_DIR + "/MI_ACPR_LinkEndInnerPowered"
LINK_END_INNER_PHASE = 0.5
LINK_END_RING_SPACING = 3.0     # ring and gap, uu, each; also the crawling band's length
LINK_END_RING_BRIGHTNESS = 0.0625  # the standing rings, like the faint line
LINK_END_BAND_BRIGHTNESS = 2.0     # the crawling band: half the bead, a glow on a ring a few uu wide
LINK_MASTER_PARAMS = ("LinkColor", "Powered", "LineBrightness", "BeadBrightness", "BeadLength", "BeadPeriod", "BeadPhase", "Solid")
LINK_LENGTH = 100.0         # the link unit mesh's authored length, uu (acpr_meshes.py, LINK_LENGTH)
LINK_LINE_BRIGHTNESS = 0.0625  # the faint line's emissive, as a fraction of the cue's (CUE_EMISSIVE_STRENGTH)
LINK_BEAD_BRIGHTNESS = 4.0    # the bead's peak: the line is 1.5 uu thin, so bloom — the glow — needs it well
                              # over the cue's level (1.0) to gather enough light in a pixel; the selected
                              # cue's lamp is 8
LINK_BEAD_LENGTH = 60.0     # the bead's half-length at half brightness, uu
LINK_BEAD_PERIOD = 2.0      # seconds per bead, outlet to machine

# THE RANGE SPHERE has its own master: unlit, translucent and two-sided, an emissive colour and
# an opacity, nothing else. The hologram wears it while placing and nothing built does.
RANGE_MASTER_PATH = MATERIAL_DIR + "/M_ACPR_Range"
RANGE_PATH = MATERIAL_DIR + "/MI_ACPR_Range"
RANGE_MASTER_PARAMS = ("EmissiveColor", "Opacity")
RANGE_COLOR = (0.20, 0.60, 1.00, 1.0)                    # the hologram's own blue
RANGE_OPACITY = 0.08

# THE CUE HAS ITS OWN MASTER. With bUsePrimitiveCustomData off, the array master emits nothing:
# its light colour comes from custom data or from nowhere, and GlowingLight / StandardLight /
# GlowColor are not the emissive input in the game's real graph. A readout that must not read
# paint therefore cannot wear the array master at all. M_ACPR_Cue is four parameters and nothing
# else — BaseColor, Roughness, Metallic, EmissiveColor — built with MaterialEditingLibrary.
CUE_MASTER_PATH = MATERIAL_DIR + "/M_ACPR_Cue"

# THE ACCENT HAS ITS OWN MASTER TOO. On the array master the accent's colour drifts with the
# light: the master is a black box with a reflection map in it, and a tinted reflection moves
# with the viewer. M_ACPR_Accent reads the swatch's primary colour STRAIGHT FROM CUSTOM
# PRIMITIVE DATA — a vector parameter with bUseCustomPrimitiveData at PrimitiveDataIndex 0,
# which is CDI_Primary_R/G/B — into BaseColor, with roughness 1 and metallic 0 and nothing else
# in the graph. The Customizer, the swatch preview and snap inheritance all write those floats,
# so all three work, and there is no texture for a drift to live in.
ACCENT_MASTER_PATH = MATERIAL_DIR + "/M_ACPR_Accent"
ACCENT_MASTER_PARAMS = ("PrimaryColor", "Roughness", "Metallic")
ACCENT_PRIMITIVE_DATA_INDEX = 0                          # CDI_Primary_R, FGFactoryColoringTypes.h:41
CUE_MASTER_PARAMS = ("BaseColor", "Roughness", "Metallic", "EmissiveColor")
CUE_BASE_COLOR = (0.02, 0.02, 0.02, 1.0)                 # an unlit strip is a dark inlay
CUE_EMISSIVE_STRENGTH = 6.0                               # HDR multiplier on the state colour; a dial
# OPEN IS CALMER THAN POWERED. Amber stays — it is the one fact readable across a room, and a
# dark open end would be indistinguishable from a capped one at any distance, which is exactly
# the ambiguity §4 forbids — but it is a marker you notice when you look for it, not a lamp.
# The in-game dials (acpr.CueGlow / acpr.CueOpen / acpr.CueDim) scale all of this live; this is
# the value that looks right in game, day and night.
CUE_OPEN_STRENGTH = 0.03
# The highlight strength is relative to the full strength, not to CUE_OPEN_STRENGTH: the
# selected cue is a LAMP, 8 x CUE_EMISSIVE_STRENGTH. The in-game dial acpr.CueSelect multiplies
# this live (1 = as shipped).
CUE_SELECTED_STRENGTH = 8.0
# The preview line (LINK_PREVIEW_PATH) is the selected cue's lamp, on a line.
LINK_PREVIEW_BRIGHTNESS = CUE_SELECTED_STRENGTH

# The three jobs land in slots BY NAME: "Body", "Cue" / "Cue_+X"…, "Accent", "Power" /
# "Power_+X"…, as acpr_meshes.py names them. No index here.

LOG_NAME = "acpr_materials_log.txt"

MESHES = ("SM_ACPR_RailBody", "SM_ACPR_RailTerminal", "SM_ACPR_RailTerminalBridged", "SM_ACPR_Junction",
          "SM_ACPR_Outlet",
          "SM_ACPR_OutletBaseBody", "SM_ACPR_OutletBaseRail", "SM_ACPR_OutletBaseJunction",
          # One cap per host face width.
          "SM_ACPR_CapRail", "SM_ACPR_CapJunction",
          # §16: the Wireless Outlet's body takes the three jobs by slot like the Outlet's.
          "SM_ACPR_WirelessOutlet")

# Meshes that are ONE material all over, by name: the link line and the range sphere have no
# body, cue or accent — every slot takes the one material named here.
WHOLE_MESH = {"SM_ACPR_Link": LINK_PATH, "SM_ACPR_LinkEnd": LINK_END_PATH,
              "SM_ACPR_RangeSphere": RANGE_PATH}  # the unpowered states are the meshes' own; the outlet swaps

# §12's palette. Amber is an OPEN terminal — the state the cue spends most of its life in
# while a network is being built, and the one that has to be unmistakable.
CUE_COLOR = (1.00, 0.42, 0.06, 1.0)
# The power colour is a matte, soft white: dim when coupled without power, full when powered.
CUE_POWERED_COLOR = (0.92, 0.92, 0.86, 1.0)
CUE_COUPLED_DIM = 0.03                                   # the acpr.CueDim value that looks right in game, day and night

# Static switches. These are what make an instance behave differently from its parent.
#
# EVERY NAME BELOW IS FROM MM_Factory_Array'S OWN PARAMETER LIST, not from the baked family's.
# The two families do not share switch names — CanBePainted, UseEmissive, UseAO,
# bUseEmissivePulse do not exist on the array master. Its ten are:
#
#   Enable Contrast Reuction, bIsBuildEffect, bEnableAmotsphereBlend,
#   bUsePrimitiveCustomData, bIsLight, bShouldReactToPower, bIsFirstLod,
#   bIsSkeletal, bSupportPaintFinishes, bHasReverseLight
#
# _check_names() below validates each one against the parent before anything is set, and
# names a mismatch instead of letting a refused set pass for a working one.
#
# THE BODY IGNORES PAINT, AND THAT IS A NECESSITY, NOT A PREFERENCE. Vanilla's plain Beam and
# Painted Beam are identical in every customization property — same MI_Beams_01, same proxy,
# same flags — and both carry mColorSlot 16 where ours carry 0; a painted-family material on
# our component renders as the default swatch, orange. The body and the accent are two slots
# on ONE component, and custom primitive data is per component, so there is no way to give
# the accent the player's swatch and withhold it from the body except in the material. The
# body is therefore MI_ACPR_Body, a child of MI_Factory_Base_01 with bUsePrimitiveCustomData
# OFF: the dark-painted-metal atlas cell at its own colours, which is what vanilla walls and
# hyper tube poles wear before a swatch.
BODY_SWITCHES = {
    "bUsePrimitiveCustomData": False,   # the body never sees the swatch
    "bSupportPaintFinishes": False,     # nor finishes and patterns
    "bIsLight": False,                  # structure does not glow
    "bShouldReactToPower": False,
}
# THE ONLY PAINTABLE SURFACE ON AN ACPR BUILDABLE is the accent, on M_ACPR_Accent, where no
# switches exist and the scalars are its own two. A colour-coding line has to be the same
# colour from every angle, so it takes the swatch's COLOUR and not its finish, and is matte.
ACCENT_SCALARS = {
    "Roughness": 1.0,
    "Metallic": 0.0,
}
# The cue states are instances of OUR master (CUE_MASTER_PATH): no switches exist on it.
CUE_SWITCHES = {}

# Scalars left at the parent's default are NOT set at all (None), so vanilla's own value stands
# and the log shows what it is. Only the ones we deliberately move carry a value.
# On our own master the only scalars are the two that keep every state matte.
CUE_SCALARS = {
    "Roughness": 1.0,
    "Metallic": 0.0,
}
BODY_SCALARS = {
    "PR_Roughness": None,
    "PR_Metallic": None,
    "HasPower": None,
}

# Texture parameters reported for the body and its parent, so "did the body inherit an Albedo"
# is answered by the log rather than by looking at a render.
REPORT_TEXTURES = ("Albedo", "FringeTexture(R)", "Normal", "ReflectionMap", "AOMasks")

_lines = []
_problems = []


# ---------------------------------------------------------------------------

def _log(message):
    _lines.append(message)
    unreal.log("[ACPR] " + message)


def _info(label, detail=""):
    _log("{0}: {1}".format(label, detail))


def _problem(text):
    _problems.append(text)
    _log("PROBLEM  " + text)


def _temp_dir(*parts):
    """<mod>/Scripts/Temp — where every script writes its log and scratch output; git ignores it."""
    directory = os.path.join(os.path.abspath(str(unreal.Paths.project_dir())), "Mods", "GameFeatures", MOD,
                             "Scripts", "Temp", *parts)
    os.makedirs(directory, exist_ok=True)
    return directory


def _write_log():
    try:
        directory = _temp_dir()
        with open(os.path.join(directory, LOG_NAME), "w") as handle:
            handle.write("Auto-Connecting Power Rails — material log\n")
            handle.write("=" * 60 + "\n\n")
            for line in _lines:
                handle.write(line + "\n")
        unreal.log("[ACPR] log written to " + os.path.join(directory, LOG_NAME))
    except Exception as e:
        unreal.log("[ACPR] could not write the material log: {0}".format(e))


def _library():
    library = getattr(unreal, "MaterialEditingLibrary", None)
    if library is None:
        _problem("unreal.MaterialEditingLibrary is not available; nothing can be done")
    return library


def _load(path):
    try:
        if not unreal.EditorAssetLibrary.does_asset_exist(path):
            return None
        return unreal.EditorAssetLibrary.load_asset(path)
    except Exception as e:
        _problem("{0} could not be loaded: {1}".format(path, e))
        return None


def _make_or_load(path, asset_class, factory_class):
    existing = _load(path)
    if existing is not None:
        return existing
    helpers = getattr(unreal, "AssetToolsHelpers", None)
    factory = getattr(unreal, factory_class, None)
    if helpers is None or factory is None:
        _problem("cannot create {0}: AssetTools or unreal.{1} unavailable".format(
            path, factory_class))
        return None
    directory, _, name = path.rpartition("/")
    try:
        return helpers.get_asset_tools().create_asset(name, directory, asset_class, factory())
    except Exception as e:
        _problem("create_asset {0} failed: {1}".format(path, e))
        return None


def _save(path):
    try:
        unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    except Exception as e:
        _info("saving " + path, "failed: {0}".format(e))


# ---------------------------------------------------------------------------
# Reading parameters off a material or an instance
# ---------------------------------------------------------------------------

def _is_instance(material):
    """True for a UMaterialInstance, False for a UMaterial.

    The two take different getters: pointing the instance getters at a UMaterial raises
    "Cannot nativize 'Material' as 'Instance'" for every parameter asked."""
    klass = getattr(unreal, "MaterialInstance", None) or \
        getattr(unreal, "MaterialInstanceConstant", None)
    if klass is None:
        return True
    return isinstance(material, klass)


def _param(material, kind, name):
    """One parameter's value, using whichever getter suits the material's class."""
    library = _library()
    if library is None:
        return None, "MaterialEditingLibrary unavailable"
    instance = _is_instance(material)
    fn_name = ("get_material_instance_{0}_parameter_value" if instance
               else "get_material_default_{0}_parameter_value").format(kind)
    fn = getattr(library, fn_name, None)
    if fn is None:
        return None, "no " + fn_name + " in this build"
    try:
        return fn(material, name), None
    except Exception as e:
        return None, str(e).splitlines()[0]


def _report_textures(material):
    """What this material resolves its textures and switches to.

    Only parameters the material ACTUALLY EXPOSES are asked for. Normal, ReflectionMap and
    AOMasks exist on the baked family and not on the array master, which has only Albedo and
    FringeTexture(R); a name the material does not have is absent, not unset, and asking only
    for what exists means the answer cannot be invented."""
    library = _library()
    if library is None:
        return
    have_switch, have_scalar, _have_vector, have_texture = _available(material)
    for name in REPORT_TEXTURES:
        if have_texture and name not in have_texture:
            continue
        texture, error = _param(material, "texture", name)
        if error:
            _info("  its " + name, "unreadable: " + error)
        else:
            _info("  its " + name,
                  texture.get_path_name() if texture else
                  "<none — this parameter exists and has no texture>")
    for name in sorted(set(list(BODY_SWITCHES) + list(CUE_SWITCHES)
                           + ["UseAO", "Simple_Rough", "bUseLegacyPaintTextures",
                              "bSampleEmissiveColorFromAlbedo", "bResolveMirroredTexture"])):
        if have_switch and name not in have_switch:
            continue
        value, error = _param(material, "static_switch", name)
        _info("  its switch " + name, "unreadable: " + error if error else value)
    for name in ("PR_Roughness", "PR_Metallic", "RoughnessMultiplier",
                 "RoughRemapMin", "RoughRemapMax", "AO Multi", "AO Pow"):
        if have_scalar and name not in have_scalar:
            continue
        value, error = _param(material, "scalar", name)
        _info("  its scalar " + name, "unreadable: " + error if error else value)


# ---------------------------------------------------------------------------
# The instances
# ---------------------------------------------------------------------------

def _as_sequence(value):
    """Iterable-but-not-a-string as a list. unreal.Array is neither list nor tuple, so a
    test for list-ness would count a whole array as one item."""
    if value is None:
        return []
    if isinstance(value, (str, bytes)):
        return [value]
    try:
        return [item for item in value]
    except TypeError:
        return [value]


def _available(material):
    """(switch names, scalar names, vector names, texture names) a material exposes."""
    library = _library()
    out = []
    for fn_name in ("get_static_switch_parameter_names", "get_scalar_parameter_names",
                    "get_vector_parameter_names", "get_texture_parameter_names"):
        fn = getattr(library, fn_name, None) if library else None
        names = set()
        if fn is not None:
            try:
                names = set(str(n) for n in _as_sequence(fn(material)))
            except Exception:
                names = set()
        out.append(names)
    return tuple(out)


def _check_names(label, parent, switches, scalars, vectors):
    """Name every requested parameter the parent does NOT expose, before anything is set.

    The material families do not share parameter names, and a refused set and a successful
    one are indistinguishable from the outside, so the check happens up front and by name."""
    have_switch, have_scalar, have_vector, have_texture = _available(parent)
    for kind, requested, available in (("switch", switches, have_switch),
                                       ("scalar", scalars, have_scalar),
                                       ("vector", vectors, have_vector)):
        missing = [n for n in sorted(requested) if available and n not in available]
        if missing:
            _problem("{0}: the parent does not expose {1} {2} — {3}".format(
                label, len(missing), kind + "(s)", ", ".join(missing)))
        present = [n for n in sorted(requested) if not available or n in available]
        _info("  {0} {1}s validated".format(label, kind),
              "{0} of {1} exist on the parent{2}".format(
                  len(present), len(requested),
                  "" if available else " (the parent's list could not be read)"))
    return have_texture


def _report_resolved_textures(label, instance):
    """Which texture each parameter resolves to, inherited ones included."""
    library = _library()
    getter = getattr(library, "get_material_instance_texture_parameter_value", None) \
        if library else None
    if getter is None:
        return
    _, _, _, have_texture = _available(instance)
    for name in REPORT_TEXTURES:
        # Only what this material exposes. A name the parent does not have is not "unset",
        # it is absent.
        if have_texture and name not in have_texture:
            continue
        try:
            texture = getter(instance, name)
        except Exception:
            continue
        label_text = "<UNSET — this parameter exists but has no texture>"
        if texture is not None:
            try:
                label_text = texture.get_path_name().split(".")[0].split("/")[-1]
            except Exception:
                label_text = "<unreadable>"
        _info("    {0} texture {1}".format(label, name), label_text)
    if have_texture:
        _info("    {0} exposes".format(label), ", ".join(sorted(have_texture)))


def _build_cue_master(material):
    """M_ACPR_Cue's graph: four parameters wired straight to four material outputs.

    Every call name is from Epic's Python reference for MaterialEditingLibrary. The emissive
    here is what lights the cue in game."""
    library = _library()
    if library is None:
        return False
    try:
        library.delete_all_material_expressions(material)
    except Exception:
        pass
    plan = (
        ("MaterialExpressionVectorParameter", "BaseColor",
         unreal.LinearColor(*CUE_BASE_COLOR), "MP_BASE_COLOR", -400, 0),
        ("MaterialExpressionScalarParameter", "Roughness", 1.0, "MP_ROUGHNESS", -400, 200),
        ("MaterialExpressionScalarParameter", "Metallic", 0.0, "MP_METALLIC", -400, 320),
        ("MaterialExpressionVectorParameter", "EmissiveColor",
         unreal.LinearColor(0.0, 0.0, 0.0, 1.0), "MP_EMISSIVE_COLOR", -400, 440),
    )
    made = 0
    for class_name, parameter, default, property_name, x, y in plan:
        expression_class = getattr(unreal, class_name, None)
        property_ = getattr(getattr(unreal, "MaterialProperty", None), property_name, None)
        if expression_class is None or property_ is None:
            _problem("cue master: missing unreal.{0} or MaterialProperty.{1}".format(
                class_name, property_name))
            continue
        try:
            node = library.create_material_expression(material, expression_class, x, y)
            node.set_editor_property("parameter_name", parameter)
            node.set_editor_property("default_value", default)
            library.connect_material_property(node, "", property_)
            made += 1
        except Exception as e:
            _problem("cue master {0}: {1}".format(parameter, e))
    try:
        library.recompile_material(material)
    except Exception as e:
        _info("cue master recompile", "failed: {0}".format(e))
    try:
        unreal.EditorAssetLibrary.save_asset(CUE_MASTER_PATH, only_if_is_dirty=False)
    except Exception:
        pass
    return made == len(plan)


def _build_accent_master(material):
    """M_ACPR_Accent: BaseColor from custom primitive data 0..2, matte, nothing else.

    The vector parameter node's bUseCustomPrimitiveData / PrimitiveDataIndex are Unreal's own
    (UMaterialExpressionVectorParameter, 4.26+), and they are SET AND READ BACK: a node that did
    not take the flag would be a plain orange parameter, and the log would call that success."""
    library = _library()
    if library is None:
        return False
    try:
        library.delete_all_material_expressions(material)
    except Exception:
        pass
    plan = (
        ("MaterialExpressionVectorParameter", "PrimaryColor",
         unreal.LinearColor(0.95, 0.30, 0.07, 1.0), "MP_BASE_COLOR", -400, 0, True),
        ("MaterialExpressionScalarParameter", "Roughness", 1.0, "MP_ROUGHNESS", -400, 200, False),
        ("MaterialExpressionScalarParameter", "Metallic", 0.0, "MP_METALLIC", -400, 320, False),
    )
    made = 0
    for class_name, parameter, default, property_name, x, y, from_cpd in plan:
        expression_class = getattr(unreal, class_name, None)
        property_ = getattr(getattr(unreal, "MaterialProperty", None), property_name, None)
        if expression_class is None or property_ is None:
            _problem("accent master: missing unreal.{0} or MaterialProperty.{1}".format(
                class_name, property_name))
            continue
        try:
            node = library.create_material_expression(material, expression_class, x, y)
            node.set_editor_property("parameter_name", parameter)
            node.set_editor_property("default_value", default)
            if from_cpd:
                node.set_editor_property("use_custom_primitive_data", True)
                node.set_editor_property("primitive_data_index", ACCENT_PRIMITIVE_DATA_INDEX)
                flag = node.get_editor_property("use_custom_primitive_data")
                index = node.get_editor_property("primitive_data_index")
                _info("  accent master " + parameter,
                      "use_custom_primitive_data={0} primitive_data_index={1}".format(flag, index))
                if not flag or int(index) != ACCENT_PRIMITIVE_DATA_INDEX:
                    _problem("accent master: {0} did not take custom primitive data — the accent "
                             "would be a fixed colour, not the swatch".format(parameter))
            library.connect_material_property(node, "", property_)
            made += 1
        except Exception as e:
            _problem("accent master {0}: {1}".format(parameter, e))
    try:
        library.recompile_material(material)
    except Exception as e:
        _info("accent master recompile", "failed: {0}".format(e))
    try:
        unreal.EditorAssetLibrary.save_asset(ACCENT_MASTER_PATH, only_if_is_dirty=False)
    except Exception:
        pass
    return made == len(plan)


def _connect(library, nodes, from_name, from_output, to_name, to_input, failures):
    """connect_material_expressions, by the input's exact name — a multi-input node's pin is
    never guessed, because a wrong pin compiles and renders the wrong thing (a SmoothStep fed on
    its Min instead of its Value is lit everywhere). The one fallback is the empty name, which
    the library takes as "the only input", and it is allowed for single-input nodes alone
    (to_input "Input"). Every connection is logged with the name that took it."""
    source = nodes.get(from_name)
    target = nodes.get(to_name)
    if source is None or target is None:
        failures.append("{0} -> {1}.{2}: node missing".format(from_name, to_name, to_input))
        return
    names = (to_input, "") if to_input == "Input" else (to_input,)
    for name in names:
        try:
            if library.connect_material_expressions(source, from_output, target, name):
                _info("    link " + from_name + " -> " + to_name, "pin '{0}'".format(name or "<only>"))
                return
        except Exception as e:
            _info("    link " + from_name + " -> " + to_name, "pin '{0}' raised {1}".format(name, e))
    failures.append("{0} -> {1}.{2}".format(from_name, to_name, to_input))


def _build_link_master(material):
    """M_ACPR_Link's graph — see LINK_MASTER_PATH for the sentence it computes. Node class and
    property names are from Epic's Python reference (MaterialExpressionLocalPosition with
    local_origin LocalPositionOrigin.INSTANCE, MaterialExpressionComponentMask,
    MaterialExpressionTransform with transform_source_type
    MaterialVectorCoordTransformSource.TRANSFORMSOURCE_INSTANCE and transform_type
    MaterialVectorCoordTransform.TRANSFORM_WORLD, MaterialExpressionConstant3Vector,
    MaterialExpressionDotProduct, MaterialExpressionSquareRoot, MaterialExpressionTime,
    MaterialExpressionMax, MaterialExpressionSaturate, the arithmetic nodes with const_a /
    const_b); input pins are A/B for the arithmetic, the single pin otherwise."""
    library = _library()
    if library is None:
        return False
    try:
        library.delete_all_material_expressions(material)
    except Exception:
        pass
    try:
        material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
        material.set_editor_property("two_sided", False)
    except Exception as e:
        _problem("link master: material properties: {0}".format(e))

    colour = tuple(c * CUE_EMISSIVE_STRENGTH for c in CUE_POWERED_COLOR[:3]) + (1.0,)
    # name: (class, {editor properties}, x, y)
    local_origin = getattr(getattr(unreal, "LocalPositionOrigin", None), "INSTANCE", None)
    from_instance = getattr(getattr(unreal, "MaterialVectorCoordTransformSource", None), "TRANSFORMSOURCE_INSTANCE", None)
    to_world = getattr(getattr(unreal, "MaterialVectorCoordTransform", None), "TRANSFORM_WORLD", None)
    transform_props = {}
    if from_instance is not None and to_world is not None:
        transform_props = {"transform_source_type": from_instance, "transform_type": to_world}
    plan = {
        "position":     ("MaterialExpressionLocalPosition",
                         {"local_origin": local_origin} if local_origin is not None else {}, -1600, -200),
        "x":            ("MaterialExpressionComponentMask", {"r": True, "g": False, "b": False, "a": False}, -1400, -200),
        "u":            ("MaterialExpressionDivide", {"const_b": LINK_LENGTH}, -1200, -200),
        # L: the instance's +X unit vector in world space is the instance's X scale long, and the
        # mesh is LINK_LENGTH on a unit instance
        "unitX":        ("MaterialExpressionConstant3Vector", {"constant": unreal.LinearColor(1.0, 0.0, 0.0, 1.0)}, -2000, 0),
        "axis":         ("MaterialExpressionTransform", transform_props, -1800, 0),
        "axisDot":      ("MaterialExpressionDotProduct", {}, -1600, 0),
        "scaleX":       ("MaterialExpressionSquareRoot", {}, -1400, 0),
        "worldLength":  ("MaterialExpressionMultiply", {"const_b": LINK_LENGTH}, -1200, 0),
        # never divided by zero
        "length":       ("MaterialExpressionMax", {"const_b": 1.0}, -1000, 0),
        "time":         ("MaterialExpressionTime", {}, -1400, 200),
        "period":       ("MaterialExpressionScalarParameter",
                         {"parameter_name": "BeadPeriod", "default_value": LINK_BEAD_PERIOD}, -1400, 300),
        "beadLength":   ("MaterialExpressionScalarParameter",
                         {"parameter_name": "BeadLength", "default_value": LINK_BEAD_LENGTH}, -1400, 400),
        "powered":      ("MaterialExpressionScalarParameter",
                         {"parameter_name": "Powered", "default_value": 0.0}, -1400, 500),
        "lineBrightness": ("MaterialExpressionScalarParameter",
                         {"parameter_name": "LineBrightness", "default_value": LINK_LINE_BRIGHTNESS}, -1400, 600),
        "beadBrightness": ("MaterialExpressionScalarParameter",
                         {"parameter_name": "BeadBrightness", "default_value": LINK_BEAD_BRIGHTNESS}, -1400, 700),
        "colour":       ("MaterialExpressionVectorParameter",
                         {"parameter_name": "LinkColor", "default_value": unreal.LinearColor(*colour)}, -1400, 800),
        "beadPhase":    ("MaterialExpressionScalarParameter",
                         {"parameter_name": "BeadPhase", "default_value": 0.0}, -1400, 900),
        # t = frac(time / period + BeadPhase)
        "phase":        ("MaterialExpressionDivide", {}, -1100, 200),
        "shifted":      ("MaterialExpressionAdd", {}, -1000, 200),
        "t":            ("MaterialExpressionFrac", {}, -900, 200),
        # hu = beadLength / length ; span = 1 + 2 hu ; c = t * span - hu
        "hu":           ("MaterialExpressionDivide", {}, -1100, 400),
        "twoHu":        ("MaterialExpressionMultiply", {"const_b": 2.0}, -900, 400),
        "span":         ("MaterialExpressionAdd", {"const_b": 1.0}, -700, 400),
        "travel":       ("MaterialExpressionMultiply", {}, -500, 300),
        "centre":       ("MaterialExpressionSubtract", {}, -300, 300),
        # d = |u - c| / hu ; s = saturate(1 - d) ; bead = s * s * (3 - 2 s)  (the smoothstep
        # polynomial, written out on A/B pins: no node whose pins could be mistaken for each other)
        "offset":       ("MaterialExpressionSubtract", {}, -100, -100),
        "distance":     ("MaterialExpressionAbs", {}, 100, -100),
        "d":            ("MaterialExpressionDivide", {}, 300, -100),
        "nearness":     ("MaterialExpressionOneMinus", {}, 500, -100),
        "s":            ("MaterialExpressionSaturate", {}, 700, -100),
        "s2":           ("MaterialExpressionMultiply", {}, 900, -200),
        "twoS":         ("MaterialExpressionMultiply", {"const_b": 2.0}, 900, -50),
        "threeMinus":   ("MaterialExpressionSubtract", {"const_a": 3.0}, 1100, -50),
        "bead":         ("MaterialExpressionMultiply", {}, 1300, -150),
        # dash(x) = 1 where frac(x / (2 beadLength)) < 0.5: x in uu along the line is u * L
        "xAlong":       ("MaterialExpressionMultiply", {}, -900, 800),
        "dashPeriod":   ("MaterialExpressionMultiply", {"const_b": 2.0}, -900, 900),
        "dashPhase":    ("MaterialExpressionDivide", {}, -700, 850),
        "dashFrac":     ("MaterialExpressionFrac", {}, -500, 850),
        "dashHalf":     ("MaterialExpressionSubtract", {"const_a": 0.5}, -300, 850),
        "dashSteep":    ("MaterialExpressionMultiply", {"const_b": 1000.0}, -100, 850),
        "dash":         ("MaterialExpressionSaturate", {}, 100, 850),
        # line = max(dash, Solid): Solid 1 is an unbroken line, whatever the bead length
        "solid":        ("MaterialExpressionScalarParameter",
                         {"parameter_name": "Solid", "default_value": 0.0}, -1400, 1000),
        "line":         ("MaterialExpressionMax", {}, 200, 700),
        # emissive = colour * ((1 - powered) * line * lineBrightness + powered * bead * beadBrightness)
        "unpowered":    ("MaterialExpressionOneMinus", {}, -1100, 500),
        "lineOn":       ("MaterialExpressionMultiply", {}, 300, 600),
        "lineTerm":     ("MaterialExpressionMultiply", {}, 500, 600),
        "beadOn":       ("MaterialExpressionMultiply", {}, 1500, 0),
        "beadTerm":     ("MaterialExpressionMultiply", {}, 1700, 100),
        "brightness":   ("MaterialExpressionAdd", {}, 1900, 300),
        "emissive":     ("MaterialExpressionMultiply", {}, 2100, 500),
    }
    links = (
        ("position", "", "x", "Input"),
        ("x", "", "u", "A"),
        ("unitX", "", "axis", "Input"),
        ("axis", "", "axisDot", "A"), ("axis", "", "axisDot", "B"),
        ("axisDot", "", "scaleX", "Input"),
        ("scaleX", "", "worldLength", "A"),
        ("worldLength", "", "length", "A"),
        ("time", "", "phase", "A"), ("period", "", "phase", "B"),
        ("phase", "", "shifted", "A"), ("beadPhase", "", "shifted", "B"),
        ("shifted", "", "t", "Input"),
        ("beadLength", "", "hu", "A"), ("length", "", "hu", "B"),
        ("hu", "", "twoHu", "A"),
        ("twoHu", "", "span", "A"),
        ("t", "", "travel", "A"), ("span", "", "travel", "B"),
        ("travel", "", "centre", "A"), ("hu", "", "centre", "B"),
        ("u", "", "offset", "A"), ("centre", "", "offset", "B"),
        ("offset", "", "distance", "Input"),
        ("distance", "", "d", "A"), ("hu", "", "d", "B"),
        ("d", "", "nearness", "Input"),
        ("nearness", "", "s", "Input"),
        ("s", "", "s2", "A"), ("s", "", "s2", "B"),
        ("s", "", "twoS", "A"),
        ("twoS", "", "threeMinus", "B"),
        ("s2", "", "bead", "A"), ("threeMinus", "", "bead", "B"),
        ("u", "", "xAlong", "A"), ("length", "", "xAlong", "B"),
        ("beadLength", "", "dashPeriod", "A"),
        ("xAlong", "", "dashPhase", "A"), ("dashPeriod", "", "dashPhase", "B"),
        ("dashPhase", "", "dashFrac", "Input"),
        ("dashFrac", "", "dashHalf", "B"),
        ("dashHalf", "", "dashSteep", "A"),
        ("dashSteep", "", "dash", "Input"),
        ("powered", "", "unpowered", "Input"),
        ("dash", "", "line", "A"), ("solid", "", "line", "B"),
        ("unpowered", "", "lineOn", "A"), ("line", "", "lineOn", "B"),
        ("lineOn", "", "lineTerm", "A"), ("lineBrightness", "", "lineTerm", "B"),
        ("bead", "", "beadOn", "A"), ("powered", "", "beadOn", "B"),
        ("beadOn", "", "beadTerm", "A"), ("beadBrightness", "", "beadTerm", "B"),
        ("lineTerm", "", "brightness", "A"), ("beadTerm", "", "brightness", "B"),
        ("colour", "", "emissive", "A"), ("brightness", "", "emissive", "B"),
    )

    nodes = {}
    failures = []
    if local_origin is None:
        failures.append("unreal.LocalPositionOrigin.INSTANCE is not available — the bead would read the component's position, not the instance's")
    if not transform_props:
        failures.append("MaterialVectorCoordTransformSource.TRANSFORMSOURCE_INSTANCE / MaterialVectorCoordTransform.TRANSFORM_WORLD are not available — the line's length cannot be read")
    for name, (class_name, props, x, y) in plan.items():
        expression_class = getattr(unreal, class_name, None)
        if expression_class is None:
            failures.append("missing unreal.{0}".format(class_name))
            continue
        try:
            node = library.create_material_expression(material, expression_class, x, y)
            for key, value in props.items():
                node.set_editor_property(key, value)
            nodes[name] = node
        except Exception as e:
            failures.append("{0}: {1}".format(name, e))
    for from_name, from_output, to_name, to_input in links:
        _connect(library, nodes, from_name, from_output, to_name, to_input, failures)
    emissive = nodes.get("emissive")
    property_ = getattr(getattr(unreal, "MaterialProperty", None), "MP_EMISSIVE_COLOR", None)
    if emissive is None or property_ is None:
        failures.append("emissive output")
    else:
        try:
            if not library.connect_material_property(emissive, "", property_):
                failures.append("emissive -> EmissiveColor")
        except Exception as e:
            failures.append("emissive -> EmissiveColor: {0}".format(e))
    for failure in failures:
        _problem("link master: " + failure)
    try:
        library.recompile_material(material)
    except Exception as e:
        _info("link master recompile", "failed: {0}".format(e))
    try:
        unreal.EditorAssetLibrary.save_asset(LINK_MASTER_PATH, only_if_is_dirty=False)
    except Exception:
        pass
    return not failures


def _ensure_instanced_usage(material, path):
    """The master must be allowed on instanced static meshes, or the packaged game draws the
    engine's default material — a flat gray — wherever an instanced component wears it. The
    editor sets this flag itself when it sees such a use, but only in the material asset it then
    has to save, and the scripts save blueprints and meshes, not the masters. So it is set here,
    read back, and saved with the master."""
    try:
        before = bool(material.get_editor_property("used_with_instanced_static_meshes"))
        if not before:
            material.set_editor_property("used_with_instanced_static_meshes", True)
        after = bool(material.get_editor_property("used_with_instanced_static_meshes"))
        _info("  " + path.split("/")[-1] + " used_with_instanced_static_meshes",
              "{0}{1}".format(after, "" if before else " (set now)"))
        if not after:
            _problem(path + ": used_with_instanced_static_meshes did not take — instanced link lines would draw gray")
        elif not before:
            unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    except Exception as e:
        _problem(path + ": used_with_instanced_static_meshes could not be read or set: {0}".format(e))


def _link_master():
    """M_ACPR_Link, rebuilt EVERY run like the accent master: it is a graph of arithmetic, and
    rebuilding is how a changed tuning constant actually lands."""
    material = _make_or_load(LINK_MASTER_PATH, unreal.Material, "MaterialFactoryNew")
    if material is None:
        _problem("link master " + LINK_MASTER_PATH + " could not be made")
        return None
    ok = _build_link_master(material)
    _ensure_instanced_usage(material, LINK_MASTER_PATH)
    have = _cue_master_params(material)
    _info("link master", "{0} — graph {1}; exposes: {2}".format(
        LINK_MASTER_PATH, "built" if ok else "INCOMPLETE", ", ".join(sorted(have)) or "nothing"))
    missing = [p for p in LINK_MASTER_PARAMS if p not in have]
    if missing:
        _problem("link master does not expose " + ", ".join(missing))
    if not ok:
        _problem("link master graph is incomplete; the link lines will not draw as designed")
    return material


def _build_range_master(material):
    """M_ACPR_Range's graph: an emissive colour parameter and an opacity parameter, on an unlit,
    translucent, two-sided material. The same calls as the cue master, plus the three material
    properties acpr_icons.py sets the same way for its hologram material."""
    library = _library()
    if library is None:
        return False
    try:
        library.delete_all_material_expressions(material)
    except Exception:
        pass
    try:
        material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        material.set_editor_property("two_sided", True)
    except Exception as e:
        _problem("range master: material properties: {0}".format(e))
    plan = (
        ("MaterialExpressionVectorParameter", "EmissiveColor",
         unreal.LinearColor(*RANGE_COLOR), "MP_EMISSIVE_COLOR", -400, 0),
        ("MaterialExpressionScalarParameter", "Opacity", RANGE_OPACITY, "MP_OPACITY", -400, 200),
    )
    made = 0
    for class_name, parameter, default, property_name, x, y in plan:
        expression_class = getattr(unreal, class_name, None)
        property_ = getattr(getattr(unreal, "MaterialProperty", None), property_name, None)
        if expression_class is None or property_ is None:
            _problem("range master: missing unreal.{0} or MaterialProperty.{1}".format(class_name, property_name))
            continue
        try:
            node = library.create_material_expression(material, expression_class, x, y)
            node.set_editor_property("parameter_name", parameter)
            node.set_editor_property("default_value", default)
            library.connect_material_property(node, "", property_)
            made += 1
        except Exception as e:
            _problem("range master {0}: {1}".format(parameter, e))
    try:
        library.recompile_material(material)
    except Exception as e:
        _info("range master recompile", "failed: {0}".format(e))
    try:
        unreal.EditorAssetLibrary.save_asset(RANGE_MASTER_PATH, only_if_is_dirty=False)
    except Exception:
        pass
    return made == len(plan)


def _range_master():
    """M_ACPR_Range, built if it does not exist or does not carry its two parameters."""
    material = _make_or_load(RANGE_MASTER_PATH, unreal.Material, "MaterialFactoryNew")
    if material is None:
        _problem("range master " + RANGE_MASTER_PATH + " could not be made")
        return None
    have = _cue_master_params(material)
    if any(p not in have for p in RANGE_MASTER_PARAMS):
        ok = _build_range_master(material)
        have = _cue_master_params(material)
        _info("range master", "{0} — graph {1}; exposes: {2}".format(
            RANGE_MASTER_PATH, "built" if ok else "INCOMPLETE", ", ".join(sorted(have)) or "nothing"))
        if not ok:
            _problem("range master graph is incomplete; the range sphere will not show")
    else:
        _info("range master", "{0} — present; exposes: {1}".format(RANGE_MASTER_PATH, ", ".join(sorted(have))))
    return material


def _accent_master():
    """M_ACPR_Accent, rebuilt EVERY run — it is three nodes, and rebuilding is how a changed
    ACCENT_PRIMITIVE_DATA_INDEX or a repaired flag actually lands rather than being skipped for
    an asset that merely exists."""
    material = _make_or_load(ACCENT_MASTER_PATH, unreal.Material, "MaterialFactoryNew")
    if material is None:
        _problem("accent master " + ACCENT_MASTER_PATH + " could not be made")
        return None
    ok = _build_accent_master(material)
    have = _cue_master_params(material)
    _info("accent master", "{0} — graph {1}; exposes: {2}".format(
        ACCENT_MASTER_PATH, "built" if ok else "INCOMPLETE", ", ".join(sorted(have)) or "nothing"))
    if not ok:
        _problem("accent master graph is incomplete; the accent will not take the swatch")
    return material


def _cue_master_params(material):
    library = _library()
    if library is None:
        return []
    try:
        have = [str(n) for n in (library.get_vector_parameter_names(material) or [])]
        have += [str(n) for n in (library.get_scalar_parameter_names(material) or [])]
        return have
    except Exception:
        return []


def _cue_master():
    """M_ACPR_Cue, built if it does not exist or does not carry its four parameters — and READ
    BACK: the parameter names the asset exposes are what says the graph is there."""
    material = _make_or_load(CUE_MASTER_PATH, unreal.Material, "MaterialFactoryNew")
    if material is None:
        _problem("cue master " + CUE_MASTER_PATH + " could not be made")
        return None
    have = _cue_master_params(material)
    if any(p not in have for p in CUE_MASTER_PARAMS):
        ok = _build_cue_master(material)
        have = _cue_master_params(material)
        _info("cue master", "{0} — graph {1}; exposes: {2}".format(
            CUE_MASTER_PATH, "built" if ok else "INCOMPLETE", ", ".join(sorted(have)) or "nothing"))
        if not ok:
            _problem("cue master graph is incomplete; the cue will not light")
    else:
        _info("cue master", "{0} — present; exposes: {1}".format(
            CUE_MASTER_PATH, ", ".join(sorted(have))))
    _ensure_instanced_usage(material, CUE_MASTER_PATH)
    return material


def _apply(instance, path, master, switches, scalars, vectors):
    """Set an instance's parameters, then READ EVERY ONE BACK and log it.

    A `set` can silently do nothing, so nothing here is reported as done because the call
    returned."""
    library = _library()
    if library is None or instance is None:
        return False

    try:
        library.set_material_instance_parent(instance, master)
    except Exception as e:
        _problem("{0}: could not parent to the master: {1}".format(path, e))
        return False

    for name, value in sorted(switches.items()):
        try:
            library.set_material_instance_static_switch_parameter_value(instance, name, value)
        except Exception as e:
            _problem("{0}: switch {1} refused: {2}".format(path, name, e))

    for name, value in sorted(scalars.items()):
        if value is None:
            continue
        try:
            library.set_material_instance_scalar_parameter_value(instance, name, value)
        except Exception as e:
            _problem("{0}: scalar {1} refused: {2}".format(path, name, e))

    for name, value in sorted(vectors.items()):
        try:
            library.set_material_instance_vector_parameter_value(
                instance, name, unreal.LinearColor(value[0], value[1], value[2], value[3]))
        except Exception as e:
            _problem("{0}: vector {1} refused: {2}".format(path, name, e))

    try:
        library.update_material_instance(instance)
    except Exception as e:
        _info(path, "update_material_instance failed: {0}".format(e))

    _log("  " + path + " reads back as:")
    for name in sorted(switches):
        try:
            _info("    switch " + name,
                  library.get_material_instance_static_switch_parameter_value(instance, name))
        except Exception:
            _info("    switch " + name, "<unreadable>")
    for name in sorted(scalars):
        try:
            _info("    scalar " + name,
                  library.get_material_instance_scalar_parameter_value(instance, name))
        except Exception:
            _info("    scalar " + name, "<unreadable>")
    for name in sorted(vectors):
        try:
            _info("    vector " + name,
                  library.get_material_instance_vector_parameter_value(instance, name))
        except Exception:
            _info("    vector " + name, "<unreadable>")
    _report_resolved_textures(path.split("/")[-1], instance)
    _save(path)
    return True


# ---------------------------------------------------------------------------
# Wearing them
# ---------------------------------------------------------------------------

def _assign(mesh_name, body, cue, accent, power=None, whole=None):
    """Body, Cue(s), Accent, Power(s), assigned BY SLOT NAME. The slot table is per mesh and
    acpr_meshes.py names it. With `whole`, every slot takes that one material (WHOLE_MESH).

    The materials go on the MESH ASSET rather than on a component, so they arrive the same
    way for every buildable."""
    path = MESH_DIR + "/" + mesh_name
    mesh = _load(path)
    if mesh is None:
        _problem(mesh_name + " does not exist — run acpr_meshes.py first")
        return False

    try:
        slots = list(mesh.get_editor_property("static_materials") or [])
    except Exception as e:
        _problem("{0}: static_materials unreadable: {1}".format(mesh_name, e))
        return False

    if not slots:
        _problem(mesh_name + " has no material slots at all")
        return False

    # BY NAME. acpr_meshes.py compacts each mesh's slots to the ones with triangles and names
    # them — Body, Cue / Cue_+X…, Accent, Power / Power_+X… — so a slot's index means nothing
    # here and its name means everything. An unnamed or unknown slot gets the cue (nothing
    # renders as checkerboard) and is reported as a problem: the mesh is not one
    # acpr_meshes.py built.
    wanted = []
    for index, entry in enumerate(slots):
        try:
            slot_name = str(entry.get_editor_property("material_slot_name"))
        except Exception:
            slot_name = ""
        if whole is not None:
            material = whole
            slot_name = slot_name or "Slot_{0}".format(index)
        elif slot_name == "Body":
            material = body
        elif slot_name == "Accent":
            material = accent
        elif slot_name.startswith("Power"):
            material = power or cue
        elif slot_name.startswith("Cue"):
            material = cue
        else:
            material = cue
            _problem("{0}: slot {1} is named '{2}', which is not a name acpr_meshes.py writes — "
                     "re-run acpr_meshes.py".format(mesh_name, index, slot_name or "<none>"))
            slot_name = slot_name or "Slot_{0}".format(index)
        try:
            wanted.append(unreal.StaticMaterial(material_interface=material,
                                                material_slot_name=slot_name))
        except Exception as e:
            _problem("{0}: StaticMaterial({1}) failed: {2}".format(mesh_name, slot_name, e))
            return False

    try:
        mesh.set_editor_property("static_materials", wanted)
    except Exception as e:
        _problem("{0}: could not set static_materials: {1}".format(mesh_name, e))
        return False

    try:
        back = list(mesh.get_editor_property("static_materials") or [])
        names = []
        for entry in back:
            try:
                material = entry.get_editor_property("material_interface")
                names.append(material.get_name() if material else "<none>")
            except Exception:
                names.append("<unreadable>")
        _info("  " + mesh_name, "{0} slot(s): {1}".format(len(back), ", ".join(names)))
    except Exception:
        pass

    _save(path)
    return True


# ---------------------------------------------------------------------------

def _run():
    library = _library()
    if library is None:
        return

    # The body parent is LOADED and checked, never assumed. A missing one is named and the
    # pass stops rather than quietly falling back to something that renders differently.
    body_parent = _load(BODY_PARENT_PATH)
    if body_parent is None:
        _problem("body parent not found at " + BODY_PARENT_PATH)
        return
    try:
        _info("body parent", body_parent.get_path_name().split(".")[0])
    except Exception:
        _info("body parent", "<unreadable path>")
    _report_textures(body_parent)

    _log("=== the body ===")
    body = _make_or_load(BODY_PATH, unreal.MaterialInstanceConstant,
                         "MaterialInstanceConstantFactoryNew")
    _info("body material", BODY_PATH + " (child of " + BODY_PARENT_PATH + ", custom data OFF)")

    _log("=== instances ===")
    cue = _make_or_load(CUE_PATH, unreal.MaterialInstanceConstant,
                        "MaterialInstanceConstantFactoryNew")
    accent = _make_or_load(ACCENT_PATH, unreal.MaterialInstanceConstant,
                           "MaterialInstanceConstantFactoryNew")
    if body is None or cue is None or accent is None:
        return

    _check_names("body", body_parent, BODY_SWITCHES, BODY_SCALARS, {})
    _apply(body, BODY_PATH, body_parent, BODY_SWITCHES, BODY_SCALARS, {})
    _report_resolved_textures("body", body)
    accent_master = _accent_master()
    if accent_master is None:
        return

    # THE CUE STATES, on our own master. Each is its EmissiveColor and nothing else; which slot
    # wears which is the C++'s call (FACPRIndicators::Push), so a state that never shows up in
    # game is a C++ question, not a material one.
    cue_master = _cue_master()
    if cue_master is None:
        return

    def _emissive(colour, scale=1.0):
        return {"EmissiveColor": tuple(c * CUE_EMISSIVE_STRENGTH * scale for c in colour[:3]) + (1.0,),
                "BaseColor": CUE_BASE_COLOR}

    _check_names("cue", cue_master, CUE_SWITCHES, CUE_SCALARS, _emissive(CUE_COLOR))
    _apply(cue, CUE_PATH, cue_master, CUE_SWITCHES, CUE_SCALARS, _emissive(CUE_COLOR, CUE_OPEN_STRENGTH))

    # The other four states. Same parent, same switches, a different colour each — and the
    # capped one with its light off.
    for path, vectors in ((CUE_POWERED_PATH,  _emissive(CUE_POWERED_COLOR)),
                          (CUE_COUPLED_PATH,  _emissive(CUE_POWERED_COLOR, CUE_COUPLED_DIM)),
                          (CUE_CAPPED_PATH,   _emissive((0.0, 0.0, 0.0), 0.0)),
                          (CUE_SELECTED_PATH, _emissive(CUE_POWERED_COLOR, CUE_SELECTED_STRENGTH))):
        instance = _make_or_load(path, unreal.MaterialInstanceConstant,
                                 "MaterialInstanceConstantFactoryNew")
        if instance is None:
            _problem(path + " could not be made")
            continue
        _apply(instance, path, cue_master, CUE_SWITCHES, CUE_SCALARS, vectors)

    # The accent on its own master: no switches exist there, the two scalars keep it matte, and
    # no colour is set on it at all — the swatch IS its colour.
    _check_names("accent", accent_master, {}, ACCENT_SCALARS, {})
    _apply(accent, ACCENT_PATH, accent_master, {}, ACCENT_SCALARS, {})
    _report_resolved_textures("accent", accent)

    # §16.5's link line: the two states of the link master, Powered 0 and 1, and the preview's
    # solid line. Every other parameter is left at the master's default, so the tuning lives in one
    # place.
    link_master = _link_master()
    if link_master is not None:
        end_scalars = {"BeadLength": LINK_END_RING_SPACING, "LineBrightness": LINK_END_RING_BRIGHTNESS,
                       "BeadBrightness": LINK_END_BAND_BRIGHTNESS}
        for path, scalars in ((LINK_PATH, {"Powered": 0.0}), (LINK_POWERED_PATH, {"Powered": 1.0}),
                              (LINK_PREVIEW_PATH, {"Powered": 0.0, "Solid": 1.0, "LineBrightness": LINK_PREVIEW_BRIGHTNESS}),
                              (LINK_END_PATH, dict(end_scalars, Powered=0.0)),
                              (LINK_END_POWERED_PATH, dict(end_scalars, Powered=1.0)),
                              (LINK_END_INNER_PATH, dict(end_scalars, Powered=0.0, BeadPhase=LINK_END_INNER_PHASE)),
                              (LINK_END_INNER_POWERED_PATH, dict(end_scalars, Powered=1.0, BeadPhase=LINK_END_INNER_PHASE))):
            link = _make_or_load(path, unreal.MaterialInstanceConstant, "MaterialInstanceConstantFactoryNew")
            if link is None:
                _problem(path + " could not be made")
                continue
            _check_names("link", link_master, {}, scalars, {})
            _apply(link, path, link_master, {}, scalars, {})

    # The range sphere, on its own translucent master.
    range_master = _range_master()
    range_instance = _make_or_load(RANGE_PATH, unreal.MaterialInstanceConstant,
                                   "MaterialInstanceConstantFactoryNew") if range_master is not None else None
    if range_instance is not None:
        _check_names("range", range_master, {}, {"Opacity": RANGE_OPACITY}, {"EmissiveColor": RANGE_COLOR})
        _apply(range_instance, RANGE_PATH, range_master, {}, {"Opacity": RANGE_OPACITY}, {"EmissiveColor": RANGE_COLOR})
    else:
        _problem(RANGE_PATH + " could not be made")

    _log("=== meshes ===")
    power = _load(CUE_COUPLED_PATH)
    for name in MESHES:
        _assign(name, body, cue, accent, power)
    for name, material_path in WHOLE_MESH.items():
        _assign(name, body, cue, accent, power, whole=_load(material_path))


def main():
    """The summary always prints, whatever the run did — a pass that gives up early is
    exactly the one whose reason needs to be at the bottom of the log where it is read."""
    _log("material pass")
    _run()
    _log("=== summary ===")
    _log("{0} problem(s)".format(len(_problems)))
    for problem in _problems:
        _log("  PROBLEM  " + problem)
    if not _problems:
        _log("Slot 0 is painted metal and slot 1 is the cue, on every buildable mesh. Which cue state")
        _log("a slot shows is swapped per slot by the C++ at runtime, and so is the link line's")
        _log("state. The range sphere is one material all over.")


try:
    main()
    _write_log()
except Exception:
    _log("the material pass stopped early:")
    for _line in traceback.format_exc().splitlines():
        _log("    " + _line)
    _write_log()
