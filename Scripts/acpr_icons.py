"""
Auto-Connecting Power Rails — render the build-menu icons and the mod icon from the meshes.

Run inside the Unreal editor (it opens a blank map for its stage and leaves the editor on a fresh
blank map afterwards; an open map with unsaved changes stops it, so nothing of yours is discarded):
    Window > Developer Tools > Output Log, dropdown "Cmd" -> "Python", then:
    exec(open(r"<plugin>/Scripts/acpr_icons.py").read())

Run after acpr_meshes.py, acpr_materials.py and acpr_bind_meshes.py: it reads the meshes those
made, the materials they carry, and the fit numbers from the buildable blueprints' defaults.

WHAT IT MAKES

    Content/Icons/T_ACPR_Icon_<Name>.uasset         512 x 512, the descriptor's big icon
    Content/Icons/T_ACPR_IconSmall_<Name>.uasset    64 x 64, the descriptor's small icon
    Resources/Icon128.png                           the plugin icon
    Resources/ModIcon512.png                        the image for the ficsit.app mod page, on a background
    Saved/ACPR_Icons/*.png                          every rendered image, for looking at
    Saved/ACPR/ACPR_icons_log.txt                   this log

and writes the textures into Desc_PowerRail / Desc_PowerRailJunction / Desc_PowerRailOutlet /
Desc_PowerRailCap (mSmallIcon, mPersistentBigIcon) and Schematic_PowerRails (mSchematicIcon,
mSmallSchematicIcon: the Rail's icon, as vanilla's AWESOME Shop entries show one buildable).
The mod icon exists only as the two PNGs under Resources/. Re-runnable: every step reads back
what it wrote.

HOW

Each icon is an assembly of the real meshes, placed as the buildables place them (the Rail's
collars pitched onto its axis with the body between them, the Outlet's cylinder and pad fitted
onto a mount plane, a Cap in a Rail end's pocket), lit by four temporary lights, and captured
by a SceneCapture2D that renders ONLY the assembly's actors. Two kinds of capture per icon: the
scene colour, whose alpha channel is inverse opacity — that alpha is what makes the background
transparent — and the final colour, re-taken with the exposure bias stepped until the subject's
mean brightness is where an icon reads well without clipping. The pixels are read back and
written as PNG here, because the engine's own export has no alpha to give.

Nothing permanent is touched except the icon textures, the four descriptors' icon fields and the
schematic's; the stage is a blank map that is never saved.

unreal.Rotator is constructed as (roll, pitch, yaw) — the opposite order to C++'s FRotator — so every
rotation here is written with keywords.
"""

import math
import os
import struct
import traceback
import zlib

import unreal

MOD = "AutoConnectingPowerRails"
MESH_DIR = "/" + MOD + "/Meshes"
MATERIAL_DIR = "/" + MOD + "/Materials"
BP_DIR = "/" + MOD + "/Buildables"
SCHEMATIC_PATH = "/" + MOD + "/Schematics/Schematic_PowerRails"
ICON_DIR = "/" + MOD + "/Icons"

LABEL_PREFIX = "ACPR_Icon_"
LOG_NAME = "ACPR_icons_log.txt"

# Rendered above the big icon's size and box-filtered down: the capture has no anti-aliasing of
# its own, so the edges' smoothness comes from the supersample — 4 samples per big pixel for a
# buildable, 16 for the mod icon with its many thin edges, 256 per small pixel.
RENDER_SIZE = 1024
MOD_RENDER_SIZE = 2048
BIG_SIZE = 512
SMALL_SIZE = 64
PLUGIN_ICON_SIZE = 128
MOD_PAGE_SIZE = 512
# The mod page image is the one icon shown on a background not ours (ficsit.app), so it carries
# one: a light neutral grey the dark parts stand off from, darkening gently towards the corners
# so the eye stays on the middle.
MOD_PAGE_BACKGROUND = (0.80, 0.81, 0.83)
MOD_PAGE_VIGNETTE = 0.22

STAGE_Z = 20000.0
FOV_ANGLE = 32.0
FRAME_FILL = 0.96   # of the frame's width or height, whichever the subject's projection fills first
VIEW_DIRECTION = (0.9, -1.0, -0.55)

LIGHTS = (
    ("Key", unreal.Rotator(roll=0.0, pitch=-40.0, yaw=35.0), 40.0),
    ("Fill", unreal.Rotator(roll=0.0, pitch=-15.0, yaw=-150.0), 14.0),
    ("Rim", unreal.Rotator(roll=0.0, pitch=-65.0, yaw=160.0), 22.0),
)
# The sky is the cubemap the editor's own thumbnail renderer lights asset thumbnails with: a stage
# in a blank map has nothing for a captured sky to see, and metal with nothing to reflect reads
# as flat grey.
SKY_CUBEMAP = "/Engine/MapTemplates/Sky/DaylightAmbientCubemap"
SKYLIGHT_INTENSITY = 1.0

# The auto-exposure is locked, and the lights are what the capture responds to (an exposure bias
# in the capture's post-process settings has no effect on it): they are scaled together
# until the subject's mean brightness is near the aim, and down whenever more of the subject
# clips than an icon can afford. The indicator is emissive and does not scale with them, which is
# right: it is the one thing meant to glow. A dark infrastructure part reads dark with highlights.
EXPOSURE_LOCK = 0.10
EXPOSURE_AIM = 0.26
EXPOSURE_TOLERANCE = 0.06
EXPOSURE_TRIES = 4
CLIP_LEVEL = 0.98
CLIP_LIMIT = 0.02

# Custom primitive data: the accent's swatch colour, floats 0..2, and nothing beyond them. A
# fresh part wears the Foundation group's default swatch, dark infrastructure. Every float the
# component carries replaces the material's own default for that index, and the array master
# reads more indices than the colours (TX2D_FactoryBase_BC is a texture array; which slice the
# body shows is one of them), so a zero in an index nobody meant selects slice 0, a sheet of grey
# strips. Three floats leave every other index at its default, which is what the body shows in
# the game. The indicator state is a material swap, not a float (_dress).
CDI_PRIMARY = (0, 1, 2)
SWATCH_PRIMARY = (0.16, 0.17, 0.18)

RAIL_LENGTH = 300.0

# The mod icon's motif: two powered Rails joined by a Rail hologram, the front Rail's open
# terminal towards the camera and an Outlet on its midpoint with a cable leading away across the
# rails. The gallery variant adds the four buildables above it, laid out like their icons.
MOTIF_RAIL_LENGTH = 300.0
MOTIF_HOLOGRAM_LENGTH = 200.0
CABLE_RUN = unreal.Vector(0.0, -320.0, 60.0)   # from the Outlet's top, across the rails and away
CABLE_SAG = 45.0
CABLE_SEGMENTS = 12
CABLE_DIAMETER = 3.0
CABLE_MESH = "/Engine/BasicShapes/Cylinder"     # authored 100 uu tall along Z, centred
GALLERY_RAIL_LENGTH = 400.0
GALLERY_RISE = 380.0                            # the gallery row above the motif
GALLERY_PITCH = 250.0                           # spacing along the screen's horizontal
MOTIF_YAW = 35.0                                # the motif runs diagonally across the square frame

# The mod's name in the top third, drawn into the frame after the render: two lines of a stroke
# alphabet defined below (the engine's text renderer needs font assets the starter project does
# not carry), the second line the larger, with the accent's orange rule under them. Drawn as a
# coverage layer, so it is white on the transparent icon and dark on the mod page. Fractions are
# of the frame; the parts are framed into the band below the title.
TITLE_LINES = (("AUTO-CONNECTING", 0.054, 0.095), ("POWER RAILS", 0.084, 0.198))   # text, cap height, centre line
TITLE_RULE = (0.272, 0.005)                      # centre line, thickness; as wide as the lower line
TITLE_STROKE = 0.16                              # of the cap height
TITLE_LIGHT = (0.96, 0.96, 0.97)
TITLE_DARK = (0.10, 0.10, 0.11)
RULE_COLOUR = (0.95, 0.30, 0.07)                 # the accent swatch's own orange
PARTS_BAND = (0.330, 0.985)                      # the frame's vertical span the parts fill

# The hologram and the link mark are materials made for the render and deleted after it: the
# starter project's factory settings carry none of vanilla's (the log says so when they do).
HOLOGRAM_MATERIAL_PATH = ICON_DIR + "/M_ACPR_IconHologram"
MARK_MATERIAL_PATH = ICON_DIR + "/M_ACPR_IconMark"
HOLOGRAM_COLOUR = (0.20, 0.60, 1.00)
HOLOGRAM_OPACITY = 0.45
MARK_COLOUR = (0.55, 0.85, 1.00)
LINK_SIZE = (25.0, 39.0)                        # a link of the chain mark: across, along the axis
LINK_BAR = 3.5
LINK_RAISE = 48.0                               # the mark floats above the joint (rail top is at 25)
LINK_MESH = "/Engine/BasicShapes/Cube"          # authored 100 uu, centred

# The indicator is a material slot the buildables swap by state (ACPRIndicatorMaterial): the icons
# show it powered, except an open terminal, which keeps its open colour. The accent is a tinted
# reflection by design; the mod icons wear the body material there instead, so that under the
# sky it is not read as a second light strip.
CUE_PATH = MATERIAL_DIR + "/MI_ACPR_Cue"
CUE_POWERED_PATH = MATERIAL_DIR + "/MI_ACPR_CuePowered"
ACCENT_PATH = MATERIAL_DIR + "/MI_ACPR_Accent"
BODY_PATH = MATERIAL_DIR + "/MI_ACPR_Body"

# Composition: the Rail-like assemblies lie across the frame; the Cap, a plate, is turned so the
# camera sees its face three-quarters on.
ASSEMBLY_YAW = 20.0
CAP_YAW = 110.0

_log_lines = []
_problems = []


def _log(message):
    _log_lines.append(message)
    unreal.log("[ACPR-ICONS] " + message)


def _info(label, detail=""):
    _log("{0:<30} {1}".format(label, detail))


def _problem(text):
    _problems.append(text)
    _log("PROBLEM: " + text)


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

def _project_dir():
    return os.path.abspath(str(unreal.Paths.project_dir()))


def _plugin_dir():
    return os.path.join(_project_dir(), "Mods", "GameFeatures", MOD)


def _saved_dir(name):
    directory = os.path.join(os.path.abspath(str(unreal.Paths.project_saved_dir())), name)
    os.makedirs(directory, exist_ok=True)
    return directory


def _output_dir():
    return _saved_dir("ACPR_Icons")


# ---------------------------------------------------------------------------
# Editor access
# ---------------------------------------------------------------------------

def _actor_subsystem():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def _blank_stage():
    """A blank map to spawn the stage into, so no level of the project is touched. An open map with
    unsaved changes stops the script rather than being discarded; a clean one is simply replaced."""
    utils = unreal.EditorLoadingAndSavingUtils
    dirty = utils.get_dirty_map_packages()
    if len(dirty) > 0:
        raise RuntimeError("the open map has unsaved changes ({0}); save or discard them first".format(
            ", ".join(package.get_name() for package in dirty)))
    return utils.new_blank_map(False)


def _load(path, what):
    if not unreal.EditorAssetLibrary.does_asset_exist(path):
        _problem("{0} does not exist: {1}".format(what, path))
        return None
    return unreal.EditorAssetLibrary.load_asset(path)


def _mesh(name):
    return _load(MESH_DIR + "/" + name, "mesh " + name)


def _cdo(blueprint):
    cls = unreal.EditorAssetLibrary.load_blueprint_class(BP_DIR + "/" + blueprint)
    return unreal.get_default_object(cls) if cls else None


def _default(blueprint, prop, fallback):
    try:
        cdo = _cdo(blueprint)
        value = cdo.get_editor_property(prop) if cdo else None
        return float(value) if value is not None else fallback
    except Exception as e:
        _info("{0}.{1}".format(blueprint, prop), "not readable ({0}); using {1}".format(e, fallback))
        return fallback


def _spawn(subsystem, cls, location, rotation, label):
    actor = subsystem.spawn_actor_from_class(cls, location, rotation)
    if actor is not None:
        actor.set_actor_label(LABEL_PREFIX + label)
    return actor


def _destroy(subsystem, actors):
    for actor in actors:
        try:
            subsystem.destroy_actor(actor)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Assemblies — the meshes placed as the buildables place them
# ---------------------------------------------------------------------------

def _bounds_of(mesh):
    bounds = mesh.get_bounds()
    return bounds.origin, bounds.box_extent


def _compose(local, assembly):
    """World transform of a part given its transform in the assembly and the assembly's."""
    return unreal.MathLibrary.compose_transforms(local, assembly)


class Assembly(object):
    """A list of parts: mesh, transform relative to the assembly origin, the material every
    slot wears instead of the mesh's own (None keeps the mesh's), and whether the indicator
    shows powered."""

    def __init__(self, name, plain_accents=False):
        self.name = name
        self.parts = []
        self.plain_accents = plain_accents

    def add(self, mesh, location, rotation, scale=None, material=None, powered=True):
        if mesh is None:
            return
        transform = unreal.Transform(location, rotation,
                                     scale if scale is not None else unreal.Vector(1.0, 1.0, 1.0))
        self.parts.append((mesh, transform, material, powered))

    def extend(self, other):
        self.parts.extend(other.parts)

    def spawn(self, subsystem, origin, rotation):
        assembly = unreal.Transform(origin, rotation, unreal.Vector(1.0, 1.0, 1.0))
        actors = []
        for index, (mesh, local, material, powered) in enumerate(self.parts):
            world = _compose(local, assembly)
            actor = _spawn(subsystem, unreal.StaticMeshActor,
                           world.translation, world.rotation.rotator(),
                           "{0}_{1}".format(self.name, index))
            if actor is None:
                continue
            actor.set_actor_scale3d(world.scale3d)
            component = actor.get_editor_property("static_mesh_component")
            component.set_static_mesh(mesh)
            _custom_data(component)
            if material is not None:
                for slot in range(component.get_num_materials()):
                    component.set_material(slot, material)
            else:
                _dress(component, powered, self.plain_accents)
            actors.append(actor)
        return actors


def _dress(component, powered, plain_accents):
    """The state the buildables would put on this mesh: the indicator slot swapped to its powered
    instance, and, for the mod icons, the accent slot to the body's."""
    swaps = []
    if powered:
        swaps.append((CUE_PATH, CUE_POWERED_PATH))
    if plain_accents:
        swaps.append((ACCENT_PATH, BODY_PATH))
    for slot in range(component.get_num_materials()):
        worn = component.get_material(slot)
        worn_path = worn.get_path_name().split(".")[0] if worn is not None else ""
        for authored, wanted in swaps:
            if worn_path == authored:
                component.set_material(slot, _load(wanted, "material"))


def _placed(assembly, location, rotation):
    """`assembly` re-based at `location`/`rotation`, as parts of a larger scene."""
    frame = unreal.Transform(location, rotation, unreal.Vector(1.0, 1.0, 1.0))
    out = Assembly(assembly.name, assembly.plain_accents)
    for mesh, local, material, powered in assembly.parts:
        world = _compose(local, frame)
        out.parts.append((mesh, unreal.Transform(world.translation, world.rotation.rotator(), world.scale3d),
                          material, powered))
    return out


def _unlit_material(path, colour, opacity):
    """An unlit material of one emissive colour, translucent when `opacity` < 1 — built the way
    acpr_materials.py builds the cue master, with the same calls, and never saved."""
    directory, _, name = path.rpartition("/")
    library = unreal.MaterialEditingLibrary
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, directory, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        _problem("{0} could not be made".format(path))
        return None
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)
    if opacity < 1.0:
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    emissive = library.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -400, 0)
    emissive.set_editor_property("constant", unreal.LinearColor(colour[0], colour[1], colour[2], 1.0))
    library.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if opacity < 1.0:
        alpha = library.create_material_expression(material, unreal.MaterialExpressionConstant, -400, 200)
        alpha.set_editor_property("r", opacity)
        library.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
    library.recompile_material(material)
    return material


def _discard(material):
    if material is not None:
        unreal.EditorAssetLibrary.delete_loaded_asset(material)


def _add_link_marks(assembly, points, material):
    """A chain link at each point the hologram couples to — the auto-link the mod is about: two
    rectangular frames of bars, one upright and one flat, interlocked along the rail's axis."""
    if not unreal.EditorAssetLibrary.does_asset_exist(LINK_MESH):
        _problem("link mesh {0} not found — no link marks".format(LINK_MESH))
        return
    cube = unreal.EditorAssetLibrary.load_asset(LINK_MESH)
    origin, extent = _bounds_of(cube)
    unit = extent.x * 2.0
    across, along = LINK_SIZE

    def frame(centre_x, flat):
        # Bars: two along the axis (at ±across/2) and two across it (at ±along/2), in the
        # vertical plane (XZ) or, flat, the horizontal one (XY).
        for sign in (-1.0, 1.0):
            offset = sign * across * 0.5
            at = unreal.Vector(centre_x, offset if flat else 0.0, LINK_RAISE + (0.0 if flat else offset))
            assembly.add(cube, at, unreal.Rotator(),
                         unreal.Vector(along / unit, LINK_BAR / unit, LINK_BAR / unit), material=material)
            at = unreal.Vector(centre_x + sign * along * 0.5, 0.0, LINK_RAISE)
            assembly.add(cube, at, unreal.Rotator(),
                         unreal.Vector(LINK_BAR / unit, across / unit if flat else LINK_BAR / unit,
                                       LINK_BAR / unit if flat else across / unit), material=material)

    for x in points:
        frame(x - along * 0.25, False)
        frame(x + along * 0.25, True)


def _custom_data(component):
    values = [0.0] * (max(CDI_PRIMARY) + 1)
    for index, value in zip(CDI_PRIMARY, SWATCH_PRIMARY):
        values[index] = value
    data = unreal.CustomPrimitiveData()
    data.set_editor_property("data", values)
    component.set_editor_property("custom_primitive_data", data)


def _z_authored_onto_x(mesh, target_x_depth=None, standoff=0.0, pitch=-90.0):
    """The buildables' rule for a mesh authored along +Z that must lie along local +X:
    pitch -90, centred on the axis, its base at `standoff`."""
    origin, extent = _bounds_of(mesh)
    rotation = unreal.Rotator(roll=0.0, pitch=pitch, yaw=0.0)
    centre = unreal.MathLibrary.quat_rotate_vector(rotation.quaternion(), origin)
    return unreal.Vector(standoff, -centre.y, -centre.z), rotation


def _add_rail(assembly, length, at_x=0.0, bridged_a=False, bridged_b=False, material=None):
    plain = _mesh("SM_ACPR_RailTerminal")
    bridged = _mesh("SM_ACPR_RailTerminalBridged")
    body = _mesh("SM_ACPR_RailBody")
    if not (plain and bridged and body):
        return
    collar_a = bridged if bridged_a else plain
    collar_b = bridged if bridged_b else plain
    origin, extent = _bounds_of(plain)
    inset = origin.z + extent.z
    body_origin, body_extent = _bounds_of(body)
    body_length = body_extent.z * 2.0
    span = max(length - 2.0 * inset, 1.0)

    assembly.add(collar_a, unreal.Vector(at_x, 0.0, 0.0), unreal.Rotator(roll=0.0, pitch=-90.0, yaw=0.0),
                 material=material, powered=bridged_a)
    assembly.add(collar_b, unreal.Vector(at_x + length, 0.0, 0.0), unreal.Rotator(roll=0.0, pitch=90.0, yaw=0.0),
                 material=material, powered=bridged_b)
    scale = unreal.Vector(1.0, 1.0, span / body_length if body_length > 0.01 else 1.0)
    rotation = unreal.Rotator(roll=0.0, pitch=-90.0, yaw=0.0)
    centre = unreal.MathLibrary.quat_rotate_vector(
        rotation.quaternion(), unreal.Vector(body_origin.x * scale.x, body_origin.y * scale.y, 0.0))
    assembly.add(body, unreal.Vector(at_x + inset - centre.x, -centre.y, -centre.z), rotation, scale, material=material)


def _add_outlet(assembly, mount, mount_rotation):
    """An Outlet on a Rail body: cylinder and pad on a mount plane whose normal is the
    assembly-local direction `mount_rotation` turns +X into."""
    cylinder = _mesh("SM_ACPR_Outlet")
    pad = _mesh("SM_ACPR_OutletBaseBody")
    if not (cylinder and pad):
        return
    standoff = _default("Build_PowerRailOutlet", "m_standoff", 0.0)
    inset = _default("Build_PowerRailOutlet", "m_body_inset", 0.1)
    frame = unreal.Transform(mount, mount_rotation, unreal.Vector(1.0, 1.0, 1.0))
    for mesh, offset in ((cylinder, standoff - inset), (pad, -inset)):
        location, rotation = _z_authored_onto_x(mesh, standoff=offset)
        world = _compose(unreal.Transform(location, rotation, unreal.Vector(1.0, 1.0, 1.0)), frame)
        assembly.add(mesh, world.translation, world.rotation.rotator())
    # The cylinder's top centre, where a cable attaches: its authored height above the mount.
    origin, extent = _bounds_of(cylinder)
    top = unreal.Transform(unreal.Vector(standoff - inset + origin.z + extent.z, 0.0, 0.0),
                           unreal.Rotator(), unreal.Vector(1.0, 1.0, 1.0))
    return _compose(top, frame).translation


def _add_cable(assembly, start, run, sag):
    """A cable from `start` to `start + run`, hanging in a parabola of `sag` at its middle, as
    straight segments of the engine's cylinder. Dark, like the body swatch."""
    if not unreal.EditorAssetLibrary.does_asset_exist(CABLE_MESH):
        _problem("cable mesh {0} not found — the motif has no cable".format(CABLE_MESH))
        return
    cylinder = unreal.EditorAssetLibrary.load_asset(CABLE_MESH)
    material = _load(MATERIAL_DIR + "/MI_ACPR_Body", "cable material")
    origin, extent = _bounds_of(cylinder)
    unit_length = extent.z * 2.0
    unit_width = extent.x * 2.0

    def point(t):
        return unreal.Vector(start.x + run.x * t, start.y + run.y * t,
                             start.z + run.z * t - 4.0 * sag * t * (1.0 - t))

    for i in range(CABLE_SEGMENTS):
        a = point(float(i) / CABLE_SEGMENTS)
        b = point(float(i + 1) / CABLE_SEGMENTS)
        axis = unreal.Vector(b.x - a.x, b.y - a.y, b.z - a.z)
        length = math.sqrt(axis.x ** 2 + axis.y ** 2 + axis.z ** 2)
        if length < 1e-3:
            continue
        rotation = unreal.MathLibrary.make_rot_from_z(axis)
        middle = unreal.Vector((a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5)
        scale = unreal.Vector(CABLE_DIAMETER / unit_width, CABLE_DIAMETER / unit_width, length / unit_length)
        assembly.add(cylinder, middle, rotation, scale, material=material)


def build_assemblies(materials):
    rail = Assembly("Rail")
    _add_rail(rail, RAIL_LENGTH)

    junction = Assembly("Junction")
    junction.add(_mesh("SM_ACPR_Junction"), unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator())

    # The Outlet on a mount plane whose normal is +Z: pitch +90 turns the mount frame's +X up.
    outlet = Assembly("Outlet")
    _add_outlet(outlet, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(roll=0.0, pitch=90.0, yaw=0.0))

    # The Cap alone, as authored: a plate lying in XY, pitched upright so its face is what the icon shows.
    cap = Assembly("Cap")
    cap.add(_mesh("SM_ACPR_CapRail"), unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(roll=0.0, pitch=-90.0, yaw=0.0))

    across = unreal.Rotator(roll=0.0, pitch=0.0, yaw=ASSEMBLY_YAW)
    upright = unreal.Rotator(roll=0.0, pitch=0.0, yaw=CAP_YAW)
    up = unreal.Rotator(roll=0.0, pitch=90.0, yaw=0.0)

    # The motif, along +X: the front Rail with its open terminal at the origin (nearest the
    # camera), the hologram bridging to the second Rail; the Outlet on the front Rail's midpoint.
    motif = Assembly("Motif", plain_accents=True)
    _add_rail(motif, MOTIF_RAIL_LENGTH, bridged_b=True)
    _add_rail(motif, MOTIF_HOLOGRAM_LENGTH, at_x=MOTIF_RAIL_LENGTH, bridged_a=True, bridged_b=True,
              material=materials["hologram"])
    _add_rail(motif, MOTIF_RAIL_LENGTH, at_x=MOTIF_RAIL_LENGTH + MOTIF_HOLOGRAM_LENGTH, bridged_a=True)
    _add_link_marks(motif, (MOTIF_RAIL_LENGTH, MOTIF_RAIL_LENGTH + MOTIF_HOLOGRAM_LENGTH), materials["mark"])
    top = _add_outlet(motif, unreal.Vector(MOTIF_RAIL_LENGTH * 0.5, 0.0, 25.0), up)
    if top is not None:
        _add_cable(motif, top, CABLE_RUN, CABLE_SAG)
    diagonal = unreal.Rotator(roll=0.0, pitch=0.0, yaw=MOTIF_YAW)

    # The mod icon: the motif, and the four buildables in a row above it, laid out along the
    # screen's horizontal (the camera's right vector lies in the ground plane) and centred on it.
    gallery = Assembly("Mod", plain_accents=True)
    gallery.extend(_placed(motif, unreal.Vector(0.0, 0.0, 0.0), diagonal))
    gallery_rail = Assembly("GalleryRail")
    _add_rail(gallery_rail, GALLERY_RAIL_LENGTH)
    items = [(gallery_rail, across), (junction, unreal.Rotator()), (outlet, across)]
    d = unreal.Vector(*VIEW_DIRECTION)
    right = unreal.Vector(-d.y, d.x, 0.0)
    norm = math.sqrt(right.x ** 2 + right.y ** 2)
    right = unreal.Vector(right.x / norm, right.y / norm, 0.0)
    motif_centre = unreal.MathLibrary.quat_rotate_vector(
        diagonal.quaternion(), unreal.Vector((2.0 * MOTIF_RAIL_LENGTH + MOTIF_HOLOGRAM_LENGTH) * 0.5, 0.0, 0.0))
    for index, (item, rotation) in enumerate(items):
        offset = (index - (len(items) - 1) * 0.5) * GALLERY_PITCH
        at = unreal.Vector(motif_centre.x + right.x * offset, motif_centre.y + right.y * offset, GALLERY_RISE)
        gallery.extend(_placed(item, at, rotation))
    gallery.band = PARTS_BAND

    return [("PowerRail", rail, across),
            ("PowerRailJunction", junction, unreal.Rotator()),
            ("PowerRailOutlet", outlet, across),
            ("PowerRailCap", cap, upright),
            ("Mod", gallery, unreal.Rotator())]


# ---------------------------------------------------------------------------
# Lights
# ---------------------------------------------------------------------------

def _light_component(actor):
    for prop in ("light_component", "directional_light_component", "sky_light_component"):
        try:
            component = actor.get_editor_property(prop)
            if component is not None:
                return component
        except Exception:
            continue
    return None


def _add_lighting(subsystem):
    made = []
    origin = unreal.Vector(0.0, 0.0, STAGE_Z + 1000.0)
    for name, rotation, intensity in LIGHTS:
        actor = _spawn(subsystem, unreal.DirectionalLight, origin, rotation, "Light_" + name)
        component = _light_component(actor)
        component.set_mobility(unreal.ComponentMobility.MOVABLE)
        component.set_editor_property("intensity", intensity)
        made.append((actor, intensity))
    sky = _spawn(subsystem, unreal.SkyLight, origin, unreal.Rotator(), "Light_Sky")
    component = _light_component(sky)
    component.set_mobility(unreal.ComponentMobility.MOVABLE)
    component.set_editor_property("intensity", SKYLIGHT_INTENSITY)
    if unreal.EditorAssetLibrary.does_asset_exist(SKY_CUBEMAP):
        component.set_editor_property("source_type", unreal.SkyLightSourceType.SLS_SPECIFIED_CUBEMAP)
        component.set_cubemap(unreal.EditorAssetLibrary.load_asset(SKY_CUBEMAP))
    else:
        _problem("sky cubemap {0} not found — the sky light has nothing to light with".format(SKY_CUBEMAP))
    made.append((sky, SKYLIGHT_INTENSITY))
    return made


def _scale_lights(lights, factor):
    for actor, base in lights:
        _light_component(actor).set_editor_property("intensity", base * factor)


# ---------------------------------------------------------------------------
# Camera and capture
# ---------------------------------------------------------------------------

def _corners(actors):
    """The eight corners of every actor's bounds, as (x, y, z) tuples."""
    out = []
    for actor in actors:
        origin, extent = actor.get_actor_bounds(False)
        for sx in (-1.0, 1.0):
            for sy in (-1.0, 1.0):
                for sz in (-1.0, 1.0):
                    out.append((origin.x + sx * extent.x, origin.y + sy * extent.y, origin.z + sz * extent.z))
    return out


def _frame(actors, band=(0.0, 1.0)):
    """Where the camera stands and what it looks at, so that the subject's PROJECTION fills
    FRAME_FILL of the frame: the corners are projected through the camera, the look-at point is
    moved to the projection's centre and the distance set from its extent, a few times over,
    because the projection depends on both."""
    d = unreal.Vector(*VIEW_DIRECTION)
    n = math.sqrt(d.x ** 2 + d.y ** 2 + d.z ** 2)
    f = (d.x / n, d.y / n, d.z / n)
    n = math.sqrt(f[0] ** 2 + f[1] ** 2)
    r = (-f[1] / n, f[0] / n, 0.0)                                  # up x forward
    u = (f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0])   # forward x right
    corners = _corners(actors)
    centre = [sum(c[i] for c in corners) / len(corners) for i in range(3)]
    # `band`: the frame's vertical span (fractions from the top) the projection is to fill.
    full = math.tan(math.radians(FOV_ANGLE) * 0.5)
    limit_x = FRAME_FILL * full
    limit_y = FRAME_FILL * full * (band[1] - band[0])
    aim_y = -((band[0] + band[1]) * 0.5 - 0.5) * 2.0 * full
    distance = 1000.0
    for _ in range(6):
        xs, ys = [], []
        for c in corners:
            rel = (c[0] - centre[0], c[1] - centre[1], c[2] - centre[2])
            depth = distance + sum(rel[i] * f[i] for i in range(3))
            xs.append(sum(rel[i] * r[i] for i in range(3)) / max(depth, 1.0))
            ys.append(sum(rel[i] * u[i] for i in range(3)) / max(depth, 1.0))
        mid_x, mid_y = (min(xs) + max(xs)) * 0.5, (min(ys) + max(ys)) * 0.5
        half_x, half_y = (max(xs) - min(xs)) * 0.5, (max(ys) - min(ys)) * 0.5
        for i in range(3):
            centre[i] += (r[i] * mid_x + u[i] * (mid_y - aim_y)) * distance
        distance *= max(half_x / limit_x, half_y / limit_y, 1e-6)
    look_at = unreal.Vector(*centre)
    location = unreal.Vector(centre[0] - f[0] * distance, centre[1] - f[1] * distance, centre[2] - f[2] * distance)
    return location, look_at


def _make_camera(subsystem, world, actors, name, size, band):
    location, centre = _frame(actors, band)
    rotation = unreal.MathLibrary.find_look_at_rotation(location, centre)
    camera = _spawn(subsystem, unreal.SceneCapture2D, location, rotation, "Camera_" + name)
    component = camera.get_editor_property("capture_component2d")
    component.set_editor_property("fov_angle", FOV_ANGLE)
    component.set_editor_property("capture_every_frame", False)
    component.set_editor_property("capture_on_movement", False)
    # The show-only list is filled through the component's own call, not the array property
    # (that property cannot be edited on a spawned component).
    component.set_editor_property("primitive_render_mode",
                                  unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST)
    for actor in actors:
        component.show_only_actor_components(actor, True)

    _lock_exposure(component)

    target = unreal.RenderingLibrary.create_render_target2d(
        world, width=size, height=size,
        format=unreal.TextureRenderTargetFormat.RTF_RGBA8)
    component.set_editor_property("texture_target", target)
    return camera, component, target


def _lock_exposure(component):
    settings = unreal.PostProcessSettings()
    for prop, value in (("override_auto_exposure_min_brightness", True),
                        ("auto_exposure_min_brightness", EXPOSURE_LOCK),
                        ("override_auto_exposure_max_brightness", True),
                        ("auto_exposure_max_brightness", EXPOSURE_LOCK)):
        settings.set_editor_property(prop, value)
    component.set_editor_property("post_process_settings", settings)


def _capture(component, source):
    component.set_editor_property("capture_source", source)
    for _ in range(2):
        component.capture_scene()


PIXEL_SCALE = 255.0


def _read_pixels(world, target, size, expect_midtones):
    """The whole target as a flat list of (r, g, b, a) in 0..1, row-major from the top left.

    read_render_target_raw_pixel_area reads "as-is": for an 8-bit target that is the byte value,
    0..255, carried in a LinearColor (read as 0..1, every channel is 0 or >= 1). The area call's
    upper bounds are exclusive; the count and the range are checked, not assumed. A tonemapped
    frame has midtones, so one without a value strictly between 0 and 255 means the range is not
    the one this code was written against; the scene-colour pass is linear and unexposed, so in
    8 bits its colour is all 0 or 255 by nature and only its alpha is read."""
    raw = unreal.RenderingLibrary.read_render_target_raw_pixel_area(world, target, 0, 0, size, size, True)
    if len(raw) != size * size:
        raise RuntimeError("read back {0} pixels for a {1} x {1} target".format(len(raw), size))
    pixels = []
    peak = 0.0
    midtones = 0
    for c in raw:
        r, g, b, a = c.r, c.g, c.b, c.a
        peak = max(peak, r, g, b, a)
        if 0.0 < r < PIXEL_SCALE or 0.0 < g < PIXEL_SCALE or 0.0 < b < PIXEL_SCALE:
            midtones += 1
        pixels.append((r / PIXEL_SCALE, g / PIXEL_SCALE, b / PIXEL_SCALE, a / PIXEL_SCALE))
    if peak > PIXEL_SCALE or (expect_midtones and midtones == 0):
        raise RuntimeError("pixel values are not 0..{0} as expected (peak {1}, midtones {2})".format(
            PIXEL_SCALE, peak, midtones))
    return pixels


def _render_mask(world, component, target, size):
    """Per-pixel opacity: scene colour's alpha is inverse opacity, 1 where nothing was drawn.
    Independent of lighting, so read once per icon."""
    _capture(component, unreal.SceneCaptureSource.SCS_SCENE_COLOR_HDR)
    opacity = _read_pixels(world, target, size, expect_midtones=False)
    return [min(max(1.0 - o[3], 0.0), 1.0) for o in opacity]


def _render_colour(world, component, target, size):
    _capture(component, unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    return _read_pixels(world, target, size, expect_midtones=True)


def _subject_stats(colour, mask):
    """Mean luminance of the subject's pixels and the fraction of them with a clipped channel."""
    total = 0.0
    clipped = 0
    count = 0
    for (r, g, b, _), a in zip(colour, mask):
        if a > 0.5:
            total += 0.2126 * r + 0.7152 * g + 0.0722 * b
            if max(r, g, b) >= CLIP_LEVEL:
                clipped += 1
            count += 1
    if not count:
        return None, 0.0, 0
    return total / count, float(clipped) / count, count


def _corner_alpha(mask, size):
    corners = (0, size - 1, (size - 1) * size, size * size - 1)
    return max(mask[i] for i in corners)


# ---------------------------------------------------------------------------
# Images — written here, since the engine's export carries no alpha
# ---------------------------------------------------------------------------

def _resample(pixels, size, new_size):
    """Box filter on premultiplied colour; exact for the integer ratios used here."""
    if new_size == size:
        return pixels
    ratio = size // new_size
    out = []
    for y in range(new_size):
        for x in range(new_size):
            r = g = b = a = 0.0
            for dy in range(ratio):
                row = (y * ratio + dy) * size
                for dx in range(ratio):
                    pr, pg, pb, pa = pixels[row + x * ratio + dx]
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
            n = float(ratio * ratio)
            if a > 0.0:
                out.append((r / a, g / a, b / a, a / n))
            else:
                out.append((0.0, 0.0, 0.0, 0.0))
    return out


def _on_background(pixels, size):
    """The pixels composited over MOD_PAGE_BACKGROUND with its vignette: opaque."""
    out = []
    half = size * 0.5
    for y in range(size):
        for x in range(size):
            r, g, b, a = pixels[y * size + x]
            d = ((x + 0.5 - half) ** 2 + (y + 0.5 - half) ** 2) / (half * half)
            shade = 1.0 - MOD_PAGE_VIGNETTE * min(d, 1.0)
            out.append((r * a + MOD_PAGE_BACKGROUND[0] * shade * (1.0 - a),
                        g * a + MOD_PAGE_BACKGROUND[1] * shade * (1.0 - a),
                        b * a + MOD_PAGE_BACKGROUND[2] * shade * (1.0 - a), 1.0))
    return out


def _write_png(path, pixels, size):
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        for x in range(size):
            r, g, b, a = pixels[y * size + x]
            raw.extend((int(round(r * 255)), int(round(g * 255)), int(round(b * 255)), int(round(a * 255))))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        handle.write(chunk(b"IEND", b""))


# ---------------------------------------------------------------------------
# Textures and descriptors
# ---------------------------------------------------------------------------

def _import_texture(png_path, asset_name):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", png_path)
    task.set_editor_property("destination_path", ICON_DIR)
    task.set_editor_property("destination_name", asset_name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    path = ICON_DIR + "/" + asset_name
    texture = _load(path, "texture " + asset_name)
    if texture is None:
        return None
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    texture.set_editor_property("srgb", True)
    texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
    # With a mip chain, a widget that draws the 512 at icon size samples a fitting mip instead of
    # one texel in several — which is what reads as pixelated in the build menu.
    texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_SIMPLE_AVERAGE)
    texture.set_editor_property("never_stream", True)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    _info("texture", "{0} ({1} x {2})".format(path, texture.blueprint_get_size_x(), texture.blueprint_get_size_y()))
    return texture


def _assign(blueprint_path, writes):
    cls = unreal.EditorAssetLibrary.load_blueprint_class(blueprint_path)
    cdo = unreal.get_default_object(cls) if cls else None
    if cdo is None:
        _problem("no class default object for " + blueprint_path)
        return
    for prop, value in writes:
        cdo.set_editor_property(prop, value)
        back = cdo.get_editor_property(prop)
        if isinstance(back, unreal.SlateBrush):
            back = back.get_editor_property("resource_object")
        _info("  " + prop, back.get_path_name() if back is not None else "<none>")
    asset = unreal.EditorAssetLibrary.load_asset(blueprint_path)
    unreal.BlueprintEditorLibrary.compile_blueprint(asset)
    unreal.EditorAssetLibrary.save_asset(blueprint_path, only_if_is_dirty=False)


def _brush(texture):
    # FSlateBrush.ImageSize is an FDeprecateSlateVector2D, a struct the Python API does not expose, so
    # the brush cannot be filled field by field from here. UWidgetBlueprintLibrary::MakeBrushFromTexture
    # (unreal.WidgetLibrary) builds the whole brush in C++; width/height <= 0 take the texture's own size.
    return unreal.WidgetLibrary.make_brush_from_texture(texture, 0, 0)


# ---------------------------------------------------------------------------

def _render_icon(subsystem, world, lights, name, assembly, rotation, directory, size):
    """The icon's pixels, and, for the mod icon, the title's coverage layer."""
    origin = unreal.Vector(0.0, 0.0, STAGE_Z)
    actors = assembly.spawn(subsystem, origin, rotation)
    if not actors:
        _problem("{0}: nothing to render".format(name))
        return None, None
    band = getattr(assembly, "band", (0.0, 1.0))
    camera = None
    try:
        camera, component, target = _make_camera(subsystem, world, actors, name, size, band)
        mask = _render_mask(world, component, target, size)
        edge = _corner_alpha(mask, size)
        if edge > 0.02:
            _problem("{0}: the corners are not transparent (alpha {1:.2f}) — something other than the assembly is in the capture".format(name, edge))

        # The lights are scaled towards the aim, and halved whenever more of the subject clips
        # than an icon can afford. Clipping wins over the aim.
        factor = 1.0
        _scale_lights(lights, factor)
        colour = None
        for attempt in range(EXPOSURE_TRIES):
            colour = _render_colour(world, component, target, size)
            mean, clipped, count = _subject_stats(colour, mask)
            if mean is None:
                _problem("{0}: no opaque pixels — the alpha route gave nothing".format(name))
                return None, None
            _info("  exposure try {0}".format(attempt + 1),
                  "subject mean {0:.3f}, clipped {1:.1%} of {2} px, lights x{3:.2f}".format(mean, clipped, count, factor))
            if clipped > CLIP_LIMIT:
                factor *= 0.5
            elif abs(mean - EXPOSURE_AIM) <= EXPOSURE_TOLERANCE:
                break
            else:
                factor *= min(max(EXPOSURE_AIM / max(mean, 0.01), 0.5), 2.0)
            _scale_lights(lights, factor)

        pixels = [(r, g, b, a) for (r, g, b, _), a in zip(colour, mask)]

        text = _title_layer(size) if band != (0.0, 1.0) else None
        path = os.path.join(directory, "ACPR_Icon_{0}.png".format(name))
        _write_png(path, _lettered(pixels, text, TITLE_LIGHT) if text else pixels, size)
        _info("rendered", path)
        return pixels, text
    finally:
        _destroy(subsystem, actors + ([camera] if camera else []))


# ---------------------------------------------------------------------------
# The title — a stroke alphabet, drawn as coverage into the frame
# ---------------------------------------------------------------------------

def _arc(cx, cy, rx, ry, start, end, steps=10):
    """Points along an ellipse arc, angles in degrees, counter-clockwise with y up."""
    return [(cx + rx * math.cos(math.radians(start + (end - start) * i / steps)),
             cy + ry * math.sin(math.radians(start + (end - start) * i / steps))) for i in range(steps + 1)]


# Capitals as polylines in a box 1 high (the cap height), y up; each with its advance width.
# Rounded letters are arcs; the strokes are drawn with round caps, which is what joins them.
GLYPHS = {
    "A": (0.78, [[(0.0, 0.0), (0.39, 1.0), (0.78, 0.0)], [(0.14, 0.36), (0.64, 0.36)]]),
    "C": (0.78, [_arc(0.42, 0.5, 0.40, 0.50, 40, 320)]),
    "E": (0.66, [[(0.66, 1.0), (0.0, 1.0), (0.0, 0.0), (0.66, 0.0)], [(0.0, 0.52), (0.56, 0.52)]]),
    "G": (0.80, [_arc(0.42, 0.5, 0.40, 0.50, 40, 360), [(0.46, 0.46), (0.82, 0.46)]]),
    "I": (0.16, [[(0.08, 0.0), (0.08, 1.0)]]),
    "L": (0.62, [[(0.0, 1.0), (0.0, 0.0), (0.62, 0.0)]]),
    "N": (0.78, [[(0.0, 0.0), (0.0, 1.0), (0.78, 0.0), (0.78, 1.0)]]),
    "O": (0.84, [_arc(0.42, 0.5, 0.42, 0.50, 0, 360, 24)]),
    "P": (0.70, [[(0.0, 0.0), (0.0, 1.0), (0.44, 1.0)] + _arc(0.44, 0.76, 0.26, 0.24, 90, -90) + [(0.0, 0.52)]]),
    "R": (0.74, [[(0.0, 0.0), (0.0, 1.0), (0.44, 1.0)] + _arc(0.44, 0.76, 0.26, 0.24, 90, -90) + [(0.0, 0.52)],
                 [(0.40, 0.52), (0.74, 0.0)]]),
    "S": (0.70, [_arc(0.35, 0.74, 0.30, 0.26, 20, 180, 8) + _arc(0.35, 0.26, 0.30, 0.26, 90, -160, 10)]),
    "T": (0.70, [[(0.0, 1.0), (0.70, 1.0)], [(0.35, 1.0), (0.35, 0.0)]]),
    "U": (0.78, [[(0.0, 1.0), (0.0, 0.36)] + _arc(0.39, 0.36, 0.39, 0.36, 180, 360, 10) + [(0.78, 1.0)]]),
    "W": (1.00, [[(0.0, 1.0), (0.24, 0.0), (0.50, 0.70), (0.76, 0.0), (1.0, 1.0)]]),
    "-": (0.50, [[(0.10, 0.50), (0.40, 0.50)]]),
    " ": (0.40, []),
}
GLYPH_SPACING = 0.22   # of the cap height, between letters


def _line_width(text, cap):
    return sum(GLYPHS[c][0] * cap for c in text) + GLYPH_SPACING * cap * (len(text) - 1)


def _stroke(layer, size, x0, y0, x1, y1, radius):
    """Coverage 1 within `radius` of the segment (round caps), pixel-binary: the frame is
    supersampled, so the box filter down to the icon is the anti-aliasing."""
    lo_x, hi_x = int(max(min(x0, x1) - radius, 0)), int(min(max(x0, x1) + radius, size - 1))
    lo_y, hi_y = int(max(min(y0, y1) - radius, 0)), int(min(max(y0, y1) + radius, size - 1))
    dx, dy = x1 - x0, y1 - y0
    length2 = dx * dx + dy * dy
    r2 = radius * radius
    for py in range(lo_y, hi_y + 1):
        row = py * size
        cy = py + 0.5
        for px in range(lo_x, hi_x + 1):
            cx = px + 0.5
            t = 0.0 if length2 == 0.0 else max(0.0, min(1.0, ((cx - x0) * dx + (cy - y0) * dy) / length2))
            ex, ey = cx - (x0 + dx * t), cy - (y0 + dy * t)
            if ex * ex + ey * ey <= r2:
                layer[row + px] = 1.0


def _title_layer(size):
    """The title's coverage over a `size` x `size` frame, and the rule's, drawn to TITLE_LINES
    and TITLE_RULE."""
    layer = [0.0] * (size * size)
    for text, cap_fraction, centre_fraction in TITLE_LINES:
        cap = cap_fraction * size
        radius = TITLE_STROKE * cap * 0.5
        x = (size - _line_width(text, cap)) * 0.5
        baseline = centre_fraction * size + cap * 0.5          # y down: the baseline is below the centre
        for c in text:
            advance, strokes = GLYPHS[c]
            for points in strokes:
                for (ax, ay), (bx, by) in zip(points, points[1:]):
                    _stroke(layer, size, x + ax * cap, baseline - ay * cap, x + bx * cap, baseline - by * cap, radius)
            x += (advance + GLYPH_SPACING) * cap
    rule_y, rule_thickness = TITLE_RULE
    text, cap_fraction, _ = TITLE_LINES[-1]
    width = _line_width(text, cap_fraction * size)
    _stroke(layer, size, (size - width) * 0.5, rule_y * size, (size + width) * 0.5, rule_y * size, rule_thickness * size * 0.5)
    layer_rule = (rule_y - rule_thickness) * size, (rule_y + rule_thickness) * size
    return layer, layer_rule


def _lettered(pixels, text, colour):
    """The title laid over the pixels: its coverage in `colour`, the rule's rows in the accent's
    orange; it overlaps nothing, so this is a lerp on colour and a union on alpha."""
    layer, (rule_top, rule_bottom) = text
    size = int(math.sqrt(len(pixels)))
    out = []
    for index, ((r, g, b, a), t) in enumerate(zip(pixels, layer)):
        y = index // size
        c = RULE_COLOUR if rule_top <= y <= rule_bottom else colour
        out.append((r + (c[0] - r) * t, g + (c[1] - g) * t, b + (c[2] - b) * t, max(a, t)))
    return out


def main():
    _log("acpr_icons.py")
    world = _blank_stage()
    subsystem = _actor_subsystem()
    directory = _output_dir()

    lights = _add_lighting(subsystem)
    materials = {"hologram": _unlit_material(HOLOGRAM_MATERIAL_PATH, HOLOGRAM_COLOUR, HOLOGRAM_OPACITY),
                 "mark": _unlit_material(MARK_MATERIAL_PATH, MARK_COLOUR, 1.0)}
    textures = {}
    try:
        for name, assembly, rotation in build_assemblies(materials):
            size = MOD_RENDER_SIZE if name == "Mod" else RENDER_SIZE
            pixels, text = _render_icon(subsystem, world, lights, name, assembly, rotation, directory, size)
            if pixels is None:
                continue
            lit = _lettered(pixels, text, TITLE_LIGHT) if text else pixels
            if name != "Mod":
                big = os.path.join(directory, "T_ACPR_Icon_{0}.png".format(name))
                _write_png(big, _resample(lit, size, BIG_SIZE), BIG_SIZE)
                textures[(name, "big")] = _import_texture(big, "T_ACPR_Icon_" + name)
                small = os.path.join(directory, "T_ACPR_IconSmall_{0}.png".format(name))
                _write_png(small, _resample(pixels, size, SMALL_SIZE), SMALL_SIZE)
                textures[(name, "small")] = _import_texture(small, "T_ACPR_IconSmall_" + name)
            else:
                resources = os.path.join(_plugin_dir(), "Resources")
                os.makedirs(resources, exist_ok=True)
                _write_png(os.path.join(resources, "Icon128.png"),
                           _resample(lit, size, PLUGIN_ICON_SIZE), PLUGIN_ICON_SIZE)
                # The mod page's version: the parts over the background, the title dark on it.
                page = _on_background(_resample(pixels, size, MOD_PAGE_SIZE), MOD_PAGE_SIZE)
                if text is not None:
                    layer, (rule_top, rule_bottom) = text
                    ratio = float(size) / MOD_PAGE_SIZE
                    small = ([t for _, _, _, t in _resample([(0.0, 0.0, 0.0, t) for t in layer], size, MOD_PAGE_SIZE)],
                             (rule_top / ratio, rule_bottom / ratio))
                    page = _lettered(page, small, TITLE_DARK)
                _write_png(os.path.join(resources, "ModIcon512.png"), page, MOD_PAGE_SIZE)
                _info("plugin icon", os.path.join(resources, "Icon128.png"))
                _info("mod page icon", os.path.join(resources, "ModIcon512.png"))
    finally:
        # The stage map is thrown away with everything in it; the editor is left on a blank map.
        unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
        for material in materials.values():
            _discard(material)

    for name in ("PowerRail", "PowerRailJunction", "PowerRailOutlet", "PowerRailCap"):
        big = textures.get((name, "big"))
        small = textures.get((name, "small"))
        if big and small:
            _log("Desc_" + name)
            _assign(BP_DIR + "/Desc_" + name, (("m_persistent_big_icon", big), ("m_small_icon", small)))

    rail = textures.get(("PowerRail", "big"))
    if rail:
        _log("Schematic_PowerRails")
        _assign(SCHEMATIC_PATH, (("m_schematic_icon", _brush(rail)), ("m_small_schematic_icon", rail)))

    _log("")
    _log("{0} problem(s)".format(len(_problems)))
    for text in _problems:
        _log("  " + text)
    log_path = os.path.join(_saved_dir("ACPR"), LOG_NAME)
    with open(log_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(_log_lines) + "\n")
    unreal.log("[ACPR-ICONS] log written to " + log_path)


try:
    main()
except Exception:
    unreal.log_error("[ACPR-ICONS] " + traceback.format_exc())
