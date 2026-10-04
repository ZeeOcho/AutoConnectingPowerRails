"""
Auto-Connecting Power Rails — the buildables' static meshes, generated with Geometry Script.

Run this INSIDE the Unreal editor:
    Window > Developer Tools > Output Log, dropdown "Cmd" -> "Python", then:
    exec(open(r"<path>/acpr_meshes.py").read())

It writes the static meshes listed in MESHES into /AutoConnectingPowerRails/Meshes/ and
nothing else: it does not assign them to any buildable and does not touch materials.
Re-running deletes and recreates every mesh (set OVERWRITE = False to leave existing
ones alone), which nulls every reference to the old assets — so acpr_materials.py and
acpr_bind_meshes.py must be run AFTER it, every time. It reads no project asset; it
reads only the Geometry Script API, and every enum and function it needs is resolved by
name at run time and reported, so a build that lacks one says so.

Every API name below is from Epic's published Python reference:

    append_box( target_mesh, primitive_options, transform, dimension_x, dimension_y,
                dimension_z, steps_x, steps_y, steps_z, origin, debug )
    apply_mesh_boolean( target_mesh, target_transform, tool_mesh, tool_transform,
                        operation, options, debug )
    create_new_static_mesh_asset_from_mesh( from_dynamic_mesh, asset_path_and_name,
                                            options, debug ) -> ( StaticMesh, outcome )

Why the Rail is two meshes: the Rail body is STRETCHED, not tiled — the mesh is authored
400 uu long along its own +Z and scaled on Z to the Rail's length (scale 0.25 at 100 uu,
10.0 at 4000 uu). Anything baked into that mesh near an end would slide and distort with
length, so the terminal detail lives in a separate collar mesh, placed at each terminal at
run time, which keeps its size at any Rail length. The body carries only what is uniform
along the length: the groove and the chamfers. The collars are created at run time, never
on the CDO, so the Beam hologram's first-match mBeamMesh binding (Appendix B.2) never sees
them.

GEOMETRY. Every number is from §11's table, and every one is a named constant below so it
can be tuned by editing one line.
"""

import math
import os
import traceback

import unreal

# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------

OVERWRITE = True                 # False: leave any mesh that already exists alone.
REPORT_ONLY = False              # True: resolve the API and report, build nothing.

# COLLISION. Render geometry is not collision geometry, and a static mesh created from a dynamic
# mesh gets none unless it is asked for — without it nothing built from these meshes can be
# targeted in game: a Junction you can see and cannot dismantle, a Rail you cannot aim an Outlet
# at.
#
# ALIGNED_BOXES rather than CONVEX_HULLS or MIN_VOLUME_SHAPES: every one of these meshes is a
# single connected box-ish solid whose grooves and indents are decoration, so one axis-aligned
# box per mesh is both the cheapest shape and the one whose result is predictable enough to
# check. The enum is resolved by name at run time and reported, so a build without it says so
# rather than silently falling back. CONVEX_HULLS, reached only as a fallback, needs a
# decomposition budget or it returns one hull — the same answer ALIGNED_BOXES gives.
COLLISION = True
COLLISION_METHOD = "ALIGNED_BOXES"
COLLISION_METHOD_FALLBACKS = ("MIN_VOLUME_SHAPES", "ORIENTED_BOXES", "CONVEX_HULLS")
COLLISION_MAX_HULLS = 8

# Collision is footprint. Collision is the only thing that blocks a placement; render geometry
# blocks nothing. So a shape that exists only in collision is not a smaller violation of the
# whole-metre grid rule than a visible one — it is the violation, with the evidence removed. No
# mesh here carries collision past its own render extent; when a Cap and its host fight over a
# surface, the HOST moves inward (RAIL_TERMINAL_SETBACK, and the collar's collision source in
# _rail_terminal_parts).

# The recesses have to be real for a trace, which a box hull cannot do. The Rail collar's pocket
# (RAIL_TERMINAL_RECESS) and the Junction's face sockets exist so that a Cap is hit first when
# aiming at it — but ALIGNED_BOXES wraps the whole collar in ONE hull that fills the recess back
# in, so a build gun trace against simple collision hits the collar's box and the Cap can never
# be aimed at however it is positioned. No standoff fixes that; the collision shape is the
# problem.
#
# CTF_USE_COMPLEX_AS_SIMPLE makes the render triangles the collision, so every recess, socket
# and groove is there for a trace. These meshes are a few hundred triangles of boxes, they never
# simulate, and the build gun aims with line traces — which is the case complex collision is
# cheap for. The simple hulls are still generated and still used by anything that asks for
# simple.
COLLISION_COMPLEX_AS_SIMPLE = True
COLLISION_TRACE_FLAG = "CTF_USE_COMPLEX_AS_SIMPLE"

MOD = "AutoConnectingPowerRails"
MESH_DIR = "/" + MOD + "/Meshes"

# Unreal units. 100 uu = 1 m.
M = 100.0

# §11: "Rail body | 0.5 m x 0.5 m square profile".
# THE ONE ACCENT NUMBER. The visible width of every accent surface on every ACPR
# buildable, stated here rather than per part so it cannot drift. The 45 degree cut's
# leg is this over root two, because the face a corner cut leaves is the hypotenuse and
# not the depth; see _chamfer for the arithmetic.
#
# It only stays one width in game because every mesh is authored at its final size and
# every C++ fit is 1:1. A mesh that got scaled would need this pre-divided by its own
# factor, and that coupling is exactly what drifts.
ACCENT_WIDTH = 0.02 * M

RAIL_PROFILE = 0.5 * M
# The authored length of the body mesh. mDefaultLength on Build_PowerRail is 400,
# and the stretch (scale 0.25 at 100 uu, 10.00 at 4000 uu) confirms 400 uu is the
# length a scale of 1.0 corresponds to.
RAIL_BODY_LENGTH = 4.0 * M

# A.1's groove, on all four faces so the Rail reads the same whatever its roll. It
# runs the WHOLE length: an authored gap at the ends would be stretched with the body,
# so the gap a player sees comes from the terminal piece at each end instead — see
# RAIL_TERMINAL_LENGTH.
GROOVE_WIDTH = 0.06 * M
GROOVE_DEPTH = 0.02 * M

# The Rail's terminal piece (the collar). Its length IS the visible break in the body's
# groove.
RAIL_TERMINAL_LENGTH = 0.35 * M
# "a little recessed, just so a cap is hit first when aiming at it" — §11 allows
# up to 0.05 m, this takes 0.03.
RAIL_TERMINAL_RECESS = 0.03 * M
RAIL_TERMINAL_RECESS_SIZE = 0.44 * M
# How far behind the terminal plane the collar's solid starts. The build gun aims with a
# swept shape, and a sweep contacts the frontmost surface anywhere in its aim disc — so a
# ring of collar face standing level with the Cap's face would compete with it. A setback
# sinks that ring: the collar starts this far behind the plane and the Cap is this much
# deeper to fill it (CAP_DEPTH_RAIL), which leaves the Cap's face ON the plane — §9 and
# §11's "Cap external spacing | 0 m" — and every collar surface behind it. Nothing crosses
# the metre in either direction. At 0 the spec's own geometry — pocket, Cap flush in it —
# is what the collar is; it becomes 0.01 * M if the ring is measured to win a swept aim.
RAIL_TERMINAL_SETBACK = 0.0
# The terminal's indicator is a segment of the groove, not a separate set of lights.
#
# The LEDs are a close-range readout, used while building a run or while working out why
# one is not powered, and the groove is what carries the same network at a distance. Seen
# as "factory scaffolding" from across a base a Rail should read as a lit line, not as a
# row of lamps. The terminal piece is its own mesh with its own material slot, so it
# carries the SAME groove as the body — same width, same depth, perfectly in line — and
# that segment is the terminal's state light. Up close the hue tells you open / coupled /
# powered / capped. At a distance it is just the line, unbroken through the Rail's ends.
#
# An open terminal is the rare state in a finished network, so a run reads as one
# continuous power-coloured line and an amber segment stands out as "unfinished here".
#
# The segment runs the full length of the terminal piece and into the end recess, so a
# capped end still shows it — dark, which is exactly what §4 says a capped terminal is.
TERMINAL_GROOVE_WIDTH = GROOVE_WIDTH
TERMINAL_GROOVE_DEPTH = GROOVE_DEPTH

# §11: "Junction logical cell | 1 m cube; terminal planes +/-0.5 m from centre".
JUNCTION_SIZE = 1.0 * M
# A 50 cm square indent per face, matching the Rail's profile so the face reads as
# "a beam goes here".
JUNCTION_INDENT = RAIL_PROFILE
# 4 cm produces no readable shading on a 1 m face; 7 cm is still well inside the cell
# and gives the wall something to cast.
JUNCTION_INDENT_DEPTH = 0.07 * M

# THE FACE CUE: FOUR STRIPS. A closed ring around the socket says "this face" and nothing
# else. Four strips radiating from the socket edge say what the face is FOR, because they
# are where a coupled Rail's four grooves come out: the lit line runs along the Rail, into
# the node, and splays onto the Junction's face.
#
# They run the full width of the face — out to where the chamfer begins — and never turn
# the corner onto it. Reaching the rim is what makes them read as the Rail's line
# arriving rather than as four marks near a socket; stopping at the chamfer is what keeps
# them invisible when two Junctions are flush.
#
# The inner end runs 1 cm INTO the socket rim rather than stopping on it: a strip that
# ends exactly on an edge leaves a knife-edge sliver for the boolean to resolve, and a
# coupled Rail (50.4 cm across a 50 cm socket) covers that centimetre anyway.
JUNCTION_STRIP_NEAR = 0.24 * M
# Short of the flat face's edge, because the Cap has to cover the strips AND the seat has
# to leave a rim of face between itself and the chamfer — see JUNCTION_SEAT_MARGIN.
JUNCTION_STRIP_FAR = 0.42 * M
JUNCTION_STRIP_WIDTH = GROOVE_WIDTH
# FLAT, NOT CUT: "calm by default, information only when lit". A groove reads as detail
# whether or not it is powered; a flat strip reads as nothing until it lights. 1 mm rather
# than 0 because a strip has to be its own triangles to carry its own material, and there
# is no way to split a face without cutting it. Against the chamfer's 2 cm that is twenty
# times smaller. It is cut into the SEAT floor, where the face actually is — see
# `seat_face` in make_junction.
JUNCTION_STRIP_DEPTH = 0.001 * M
# The chamfer is cosmetic. Kept small because it softens the silhouette and hides
# z-fighting where two cubes meet. Set to 0.0 to drop it.
JUNCTION_CHAMFER = 0.02 * M

# A.2: "compact ribbed cylindrical body, dark rings, metal rims".
OUTLET_RADIUS = 0.20 * M
OUTLET_HEIGHT = 0.26 * M
OUTLET_RADIAL_STEPS = 24
OUTLET_RIB_COUNT = 3
OUTLET_RIB_DEPTH = 0.015 * M
OUTLET_RIB_HEIGHT = 0.03 * M
# A slim square pad at the Outlet's base. Its interruption of the Rail's power strip is
# what says "power leaves the network here", and it works in both host modes: on a Rail
# end the pad matches the pocket it caps, on a Junction face the socket.
#
# The pad is NARROWED, not chamfered, to the width of the Rail's remaining flat face, so
# it does not overhang into the chamfer. Chamfering the pad would put four accent strips
# along the MOUNTING NORMAL: on an Outlet sitting on top of a horizontal Rail those read
# as vertical bars, across the grain of the Rail's own accent lines rather than along it.
RAIL_FLAT = RAIL_PROFILE - ACCENT_WIDTH * math.sqrt(2.0)
OUTLET_BASE_SIZE = RAIL_FLAT
# The pad's height is the accent width, not a thickness chosen for its own sake: it is a
# strip like all the others, so no second width hides here.
OUTLET_BASE_HEIGHT = ACCENT_WIDTH

# §9: the Cap's outer face is at the terminal plane and the mesh extends INWARD.
#
# TWO CAPS, AND THE REASON IS THE ACCENT WIDTH. One mesh cannot serve both hosts once the
# chamfer carries the accent: the fit scales the whole mesh, so a single 2 cm cut comes out
# 1.6 cm on a 40 cm Rail end and 3.6 cm on a 90 cm Junction face. Neither matches the host
# it is sitting on, which is the one thing a Cap has to do. Authored at the true face width
# instead, each cap's fit is 1:1 and its chamfer is the same 2 cm as everything else.
#
# The numbers are AACPRCap::mRailFaceWidth and mJunctionFaceWidth, and mDepth. They are
# duplicated here because a script cannot read a header; FitZAuthoredMeshToLocalX logs a
# mismatch rather than silently rescaling, so drift is caught on the next run.
#
# A CAP FILLS ITS POCKET, in width as in depth. A Cap narrower than the pocket leaves a
# ring of pocket wall showing all round and is indistinguishable from the bottom of the
# hole it sits in. At the pocket's own width it is the pocket, which is what §9 describes.
CAP_FACE_RAIL = RAIL_TERMINAL_RECESS_SIZE
# THE JUNCTION FACE GETS A SEAT, and the Cap fills that instead of being buried in the cube.
#
# A separate actor cannot be both flush and in front of its host unless it protrudes
# (collision is footprint, so it must not) or the host is cut back. So the host is cut
# back. The junction Cap hides the face rather than dimming it — "a smooth junction face
# indicates capped" — and the four indicator strips run out to JUNCTION_STRIP_FAR from
# the face centre, so the seat covers the whole FLAT face, bounded by the chamfer, and
# the strips are covered right out to the rim. Uncapped, a face is a shallow recessed
# panel with the indicator strips in its floor; capped, it is plated over smooth.
#
# THE SEAT LEAVES A RIM. At the full flat-face width the six seats would reach exactly
# as far as the twelve chamfers and sever the edge strips from the body (the mesh log
# reports it as "Junction shape: components=2"). A margin of face has to survive between
# each seat and the chamfer.
JUNCTION_SEAT_MARGIN = 0.02 * M
JUNCTION_CAP_SEAT_SIZE = ( JUNCTION_SIZE - ACCENT_WIDTH * math.sqrt(2.0)
                           - JUNCTION_SEAT_MARGIN * 2.0 )
JUNCTION_CAP_SEAT_DEPTH = 0.02 * M
CAP_FACE_JUNCTION = JUNCTION_CAP_SEAT_SIZE

# DEPTH IS PER HOST. A Cap fills its host's pocket exactly and stops. A Cap deeper than
# its pocket sits inside solid host — two interpenetrating solids, and with complex-as-
# simple collision two sets of coincident surfaces for a trace to choose between, which is
# a build gun flicking between Rail and Cap on a tiny mouse movement.
#
# Its outer face then lands ON the terminal plane — §9's "the outer face sits AT the
# terminal plane", §11's "Cap external spacing | 0 m" — so nothing extends past the
# whole-metre boundary and no standoff is needed or wanted. The numbers are the pockets
# themselves, plus the collar's setback so the Rail Cap fills whatever the collar's ring
# gives up.
CAP_DEPTH_RAIL = RAIL_TERMINAL_RECESS + RAIL_TERMINAL_SETBACK
CAP_DEPTH_JUNCTION = JUNCTION_CAP_SEAT_DEPTH

# THE RAIL CAP'S RIM, AND ONLY THE RAIL CAP'S. It is about sight lines: looking dead-on at
# a capped JUNCTION face you already see the cube's twelve chamfers, so the circuit colour
# is there without the cap doing anything. Looking dead-on at a capped RAIL end, the
# Rail's four chamfers run away from you and read as nothing at all — so that cap needs
# to carry the accent itself.
#
# The alternative — chamfering and accenting the Rail's END edges too, so neither cap
# needs a rim — is not done because a Rail end chamfer would sit about 2 cm from the
# terminal groove, which IS the terminal-state light, putting the circuit colour in
# contact with the state colour at the one place the state has to be legible; and because
# a Rail end is capped, coupled or seated in a Junction far more often than it is left
# bare, so it would rarely be seen. The Cap's rim is visible exactly when there is a Cap.
CAP_RIM = True
CAP_RIM_INSET = 0.04 * M
CAP_RIM_DEPTH = 0.012 * M

# --- material slots ------------------------------------------------------------
#
# The groove, the terminal segment, the Junction strips and the Outlet's ribs glow, and the
# body around them does not. These meshes have no mask texture that could tell one from
# the other, so the split is a MATERIAL SLOT: everything structural is material ID 0,
# every cue surface is a cue ID, and create_new_static_mesh_asset_from_mesh turns those
# into slots.
#
# Which triangles are cue is decided analytically, by centroid, against the same volumes
# the cutting tools are built from — see "Cue volumes" below. The pass also REPORTS what
# every mesh ended up with (_slot_census) instead of assuming.
#
# Verified names (Epic's Python API reference, unreal.GeometryScript_Materials):
#     enable_material_i_ds( target_mesh, debug )
#     clear_material_i_ds( target_mesh, clear_value, debug )
#     get_max_material_id( target_mesh )
MATERIAL_ID_BODY = 0
MATERIAL_ID_CUE = 1
# THE ACCENT — the only paintable surface on an ACPR buildable.
#
# Seven, because the Junction's per-face cue occupies 1..6 and one number that works for
# every mesh beats a per-mesh rule nobody will remember. The IDs are compacted per mesh at
# save time, so the simple meshes do not carry the empty IDs in between.
MATERIAL_ID_ACCENT = MATERIAL_ID_CUE + 6
# THE SOCKET FLOORS go where the actual physical power connection would be made. The floor
# of the Rail collar's Cap pocket and the floor of each Junction beam socket carry the
# NETWORK's power (dim white / white), not the terminal's state — so a Junction with power
# on any terminal lights all six floors. Visible exactly when the terminal is open, which
# is the rare case; a coupled Rail or a Cap covers it.
MATERIAL_ID_POWER = MATERIAL_ID_ACCENT + 1
# HALF THE SIDE, or the floor looks like a massive glowing power beam inside with only slim
# insulation. The indicator is a square in the middle of the floor, sunk a millimetre so it
# is its own triangles; the rest of the floor is body.
POWER_FLOOR_FRACTION = 0.5
POWER_FLOOR_DEPTH = 0.001 * M
# PER FACE ON THE JUNCTION. A coupled Rail's end sits ON the terminal plane, 2 cm proud of
# the seat floor the socket opens in, so a lit socket floor would show through that 2 cm
# slot all round the Rail. The floor of an OCCUPIED terminal is therefore dark, which
# needs the six floors to be six slots: MATERIAL_ID_POWER + face, in _face_slot order. The
# collar's one floor is one terminal already.
def _power_slot(axis, sign):
    return MATERIAL_ID_POWER + axis * 2 + (0 if sign > 0 else 1)

# A material instance is one set of values, so one cue slot can only ever show one state.
# The Junction has six faces that must each answer for themselves — a Rail coupled on +X
# while -X is bare — so its cue is split six ways, slot 1 + face. Face order is
# ( axis, sign ): +X, -X, +Y, -Y, +Z, -Z. Everything else on the mod has no per-face state
# and stays in slot 1.
JUNCTION_FACE_NAMES = ("+X", "-X", "+Y", "-Y", "+Z", "-Z")


def _face_slot(axis, sign):
    """( axis, sign ) -> the material ID that face's cue lives in."""
    return MATERIAL_ID_CUE + axis * 2 + (0 if sign > 0 else 1)
# How far outside a cue volume a triangle's CENTROID may lie and still count as one of
# its walls. A groove's floor and side walls sit exactly ON the volume's surface, which
# is the ambiguous case for an inside/outside test, so they need a little shell to be
# caught reliably. One millimetre is far below the 6 cm width of the narrowest cue.
CUE_PAD = 0.1
# Only used by the fallback path (see _mark_cue_by_volume).
CUE_SHELL = 0.5
# If the analytic test claims more than this share of a mesh, something is wrong with
# the volumes rather than with the mesh, and the log says so instead of shipping it
# quietly.
#
# IT IS A SHARE OF SURFACE AREA, NOT OF TRIANGLE COUNT. Triangle count is not area: a
# boolean leaves the big flat faces as a handful of enormous slivers and the thin grooves
# as hundreds of small triangles, so a correct Junction is over half cue BY COUNT and a
# few per cent BY AREA. Counting triangles measures how finely a surface happens to be
# tessellated; only area measures how much of the object glows.
CUE_SANITY_AREA_FRACTION = 0.25

# Whether the build may TURN a mesh it believes is inside out. It may not, by default: an
# automatic correction driven by a misread measurement turns correct meshes into
# see-through ones while the log reports success. The numbers are printed every build;
# acting on them is a decision, not a reflex — see _orientation for the sign convention.
FLIP_INSIDE_OUT = False

# --- UVs -----------------------------------------------------------------------
#
# MM_FactoryBaked samples Albedo, Normal, ReflectionMap and AOMasks, and a mesh whose cut
# faces carry no usable UVs renders those as a smeared checkerboard.
#
# A BOX PROJECTION is the right kind for these shapes. Everything here is axis-aligned
# boxes and one cylinder, so projecting from a box gives every face a flat, correctly
# oriented, correctly scaled piece of texture with no stretching — and, unlike an
# automatic unwrap, it is deterministic and produces the same result every run. The
# projection box is a FIXED size of UV_TILE uu, so texel density is the same on every
# mesh regardless of its size; fitting the box to each mesh's bounds instead would give a
# 1 m cube and a 4 m rail texel densities four times apart.
#
# UV_TILE is the world size of one texture repeat, measured off the family we wear: the
# structural buildables on MM_Factory_Array (hyper tube poles, walls, railings, the power
# tower platform) cluster at 4013..4134 uu per repeat, spread 3.0%. Our Rails, Junctions,
# Outlets and Caps are structural parts, so they belong in that cluster. At 4096 a 4 m
# Rail spans 400/4096 = 0.098 of the texture and a 1 m Junction 0.024, which is the same
# order as the vanilla structural meshes (SM_WallConcave_8x8_02 occupies u 0.047..0.242).
#
# Two sets, because vanilla's master material reads a second channel for its AO masks.
#
#     set_num_uv_sets( target_mesh, num_uv_sets, debug )
#     set_mesh_u_vs_from_box_projection( target_mesh, uv_set_index, box_transform,
#                                        selection, min_island_tri_count, debug )
#
# — Epic's Python reference, and note the u_vs spelling the bindings produce.
UV_TILE = 4096.0
UV_SETS = 2

# THE ATLAS, measured off TextureSheet_end.png. Edge detection on that frame puts the cell
# boundaries at
#     u: 0.000  0.333  0.667  0.833  1.000
#     v: 0.000  0.167  0.333  0.667  1.000
# and the cells sample as distinct materials — mean RGB over each cell's middle:
#
#     u 0.00-0.33  v 0.00-0.33    39  37  34   dark painted metal      <- ours
#     u 0.33-0.67  v 0.00-0.33    75  68  60   mid brown metal
#     u 0.67-0.83  v 0.00-0.33   255 251 223   very bright, looks emissive
#     u 0.00-0.33  v 0.33-0.67   215 214 212   near-white plastic
#     u 0.33-0.67  v 0.33-0.67   162 160 157   light grey concrete
#     u 0.67-1.00  v 0.33-0.67    69  62  53   dark brown metal
#     u 0.00-0.67  v 0.67-1.00    19  18  16   near-black
#     u 0.67-1.00  v 0.67-1.00   174 173 172   light galvanised metal
#
# Vanilla picks the same cell for the same job: SM_WallConcave_8x8_02, a large flat
# structural surface, measures u 0.047..0.242, v 0.019..0.317 — wholly inside the dark
# painted metal cell. Appendix A.1's "weighty matte steel" is that cell.
#
# The atlas cells carry which paint channel they read, not just a substrate: the
# near-white plastic cell is painted from the swatch's SECONDARY colour. The accent does
# not depend on the cell at all — its own master (acpr_materials.py, M_ACPR_Accent) reads
# the primary colour straight from custom data — so every triangle stays in this one cell.
UV_CELL_NAME = "dark painted metal (u 0..0.333, v 0..0.333)"
UV_CELL = (0.0, 0.333, 0.0, 0.333)     # None disables placement entirely
UV_CELL_MARGIN = 0.004                 # keep off the seam; bilinear filtering bleeds

# Epic's default is 2: it "filters small UV islands, excluding those below the triangle
# threshold from processing", so every one-triangle island a boolean leaves behind would
# keep its unset (0,0) UVs and sample the corner of vanilla's atlas instead of the cell.
# 1 excludes nothing.
UV_MIN_ISLAND_TRIS = 1

# How far the collar's cross-section stands proud of the body's, per side. Zero: the body
# is fitted to span only the gap BETWEEN the two collars, so the three pieces abut instead
# of overlapping and there is no coplanar pair to avoid. Abutting boxes are fine — the
# shared faces point away from each other and each is hidden by the other. Any oversize
# would step the chamfer and the groove sideways at the joint, which reads as a seam.
RAIL_TERMINAL_OVERSIZE = 0.0

MESHES = ("SM_ACPR_RailBody", "SM_ACPR_RailTerminal",
          # The collar WITH the 2 cm bridge slice grown on, as one mesh. The C++ swaps a collar
          # to this while its terminal is coupled to a Junction, instead of showing a separate
          # bridge component — two collar components per Rail, not four.
          "SM_ACPR_RailTerminalBridged", "SM_ACPR_Junction",
          "SM_ACPR_Outlet",
          # The Outlet's pad is its own mesh, one per host. A Rail body wants the flat's 47.172,
          # a Rail end its 44 pocket, a Junction its 50 socket, and the same pad cannot be all
          # three. The C++ picks one the way ApplyCapMesh picks a Cap.
          "SM_ACPR_OutletBaseBody", "SM_ACPR_OutletBaseRail", "SM_ACPR_OutletBaseJunction",
          # One Cap per host face width — see CAP_FACE_RAIL.
          "SM_ACPR_CapRail", "SM_ACPR_CapJunction")

LOG_NAME = "ACPR_mesh_log.txt"

_built = []
_failed = []
_notes = []
_material_ids = {}
_lines = []


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _log(m):
    _lines.append(m)
    unreal.log("[ACPR] " + m)


def _info(label, detail=""):
    _log("{0}: {1}".format(label, detail))


def _note(text):
    if text not in _notes:
        _notes.append(text)
        _log("NOTE  " + text)


def _write_log():
    """The mesh pass logs to a FILE as well as the Output Log — <project>/Saved/ACPR/ —
    so the build-stage triangle trace can be read outside the editor."""
    try:
        directory = os.path.join(str(unreal.Paths.project_saved_dir()), "ACPR")
    except Exception:
        directory = os.path.join(os.path.expanduser("~"), "ACPR")
    directory = os.path.abspath(directory)
    try:
        if not os.path.isdir(directory):
            os.makedirs(directory)
        with open(os.path.join(directory, LOG_NAME), "w") as handle:
            handle.write("Auto-Connecting Power Rails — mesh build log\n")
            handle.write("=" * 60 + "\n\n")
            for line in _lines:
                handle.write(line + "\n")
        unreal.log("[ACPR] log written to " + os.path.join(directory, LOG_NAME))
        return True
    except Exception as e:
        unreal.log("[ACPR] could not write the mesh log: {0}".format(e))
        return False


def _enum(type_name, *candidates):
    """Enum member names are the single most guessable thing here, so none of them
    is guessed: the type is looked up, the candidates are tried in order, and the
    one that resolves is logged. Returns None rather than raising."""
    enum_type = getattr(unreal, type_name, None)
    if enum_type is None:
        _info("enum " + type_name, "ABSENT")
        return None
    for name in candidates:
        value = getattr(enum_type, name, None)
        if value is not None:
            _info("enum " + type_name, "using ." + name)
            return value
    available = [n for n in dir(enum_type) if n.isupper()]
    _info("enum " + type_name, "none of {0} exist; available: {1}".format(
        list(candidates), ", ".join(available[:20])))
    return None


def _xf(x=0.0, y=0.0, z=0.0):
    return unreal.Transform(unreal.Vector(x, y, z), unreal.Rotator(0.0, 0.0, 0.0),
                            unreal.Vector(1.0, 1.0, 1.0))


def _xf_yaw(x, y, z, yaw):
    """_xf with a yaw, for the 45 degree corner cutters. unreal.Rotator takes
    ( roll, pitch, yaw ) in that order in the Python bindings — the argument order is
    NOT the Pitch/Yaw/Roll of the C++ FRotator constructor, which is the kind of thing
    that silently rotates a cutter about the wrong axis, so it is named here."""
    return unreal.Transform(unreal.Vector(x, y, z),
                            unreal.Rotator(0.0, 0.0, yaw),
                            unreal.Vector(1.0, 1.0, 1.0))


def _new_mesh():
    """unreal.new_object( unreal.DynamicMesh ) is the constructor that works in this
    editor; both are tried so a different build does not take the whole pass down."""
    errors = []
    for label, make in (("unreal.new_object(DynamicMesh)",
                         lambda: unreal.new_object(unreal.DynamicMesh)),
                        ("unreal.DynamicMesh()", lambda: unreal.DynamicMesh())):
        try:
            mesh = make()
            if mesh is not None:
                return mesh
        except Exception as e:
            errors.append("{0}: {1}".format(label, e))
    raise RuntimeError("no DynamicMesh could be created — " + "; ".join(errors))


def _box(mesh, size_x, size_y, size_z, at=None, centred=True):
    """append_box with the origin mode resolved rather than assumed. BASE puts the
    box's base at the transform; CENTER centres it. Everything here is easier to
    reason about centred, so that is preferred and BASE is compensated for."""
    options = unreal.GeometryScriptPrimitiveOptions()
    transform = at or _xf()

    origin = None
    if centred:
        origin = _ORIGIN_CENTER
        if origin is None:
            # No CENTER mode: shift the transform down by half the height instead,
            # which is what CENTER would have done.
            location = transform.translation if hasattr(transform, "translation") else None
            if location is not None:
                transform = _xf(location.x, location.y, location.z - size_z * 0.5)
            origin = _ORIGIN_BASE

    kwargs = {"dimension_x": size_x, "dimension_y": size_y, "dimension_z": size_z}
    if origin is not None:
        kwargs["origin"] = origin

    return unreal.GeometryScript_Primitives.append_box(mesh, options, transform, **kwargs)


def _subtract(target, tool):
    """Boolean subtract, degrading to 'leave the target alone' with a logged note
    rather than failing the whole mesh — a plain box to look at beats nothing."""
    if _BOOLEAN_SUBTRACT is None:
        _note("No boolean SUBTRACT operation could be resolved, so the grooves, "
              "recesses and pads are missing from these meshes. The shapes are "
              "otherwise correct.")
        return target
    try:
        options = unreal.GeometryScriptMeshBooleanOptions()
        result = unreal.GeometryScript_MeshBooleans.apply_mesh_boolean(
            target, _xf(), tool, _xf(), _BOOLEAN_SUBTRACT, options)
        # The call edits the target in place and hands it back. A build that hands
        # back nothing would otherwise silently replace the mesh with None and take
        # every later step down with it, so the target is kept either way.
        return result if result is not None else target
    except Exception as e:
        _note("apply_mesh_boolean failed ({0}), so cut details are missing from "
              "these meshes.".format(e))
        return target


def _collision_method():
    """The GeometryScriptCollisionGenerationMethod value, matched by name.

    Returns (value, label). value is None when the enum or the name is missing, and the label
    says which — a missing enum and a missing member are different problems."""
    enum = getattr(unreal, "GeometryScriptCollisionGenerationMethod", None)
    if enum is None:
        return None, "unreal.GeometryScriptCollisionGenerationMethod is not in this build"
    wanted = COLLISION_METHOD
    for candidate in (wanted,) + COLLISION_METHOD_FALLBACKS:
        value = getattr(enum, candidate, None)
        if value is not None:
            note = candidate if candidate == wanted else (
                "{0} is absent, fell back to {1}".format(wanted, candidate))
            return value, note
    available = [n for n in dir(enum) if n.isupper()]
    return None, "none of {0} exist; the enum offers {1}".format(
        (COLLISION_METHOD,) + COLLISION_METHOD_FALLBACKS, ", ".join(available))


def _simple_collision_count(asset):
    """How many simple collision shapes the asset ended up with, or None if unreadable.

    Two APIs, because get_simple_collision_count moved from EditorStaticMeshLibrary to
    StaticMeshEditorSubsystem in UE5. Whichever answers is reported."""
    subsystem_class = getattr(unreal, "StaticMeshEditorSubsystem", None)
    getter = getattr(unreal, "get_editor_subsystem", None)
    if subsystem_class is not None and getter is not None:
        try:
            subsystem = getter(subsystem_class)
            fn = getattr(subsystem, "get_simple_collision_count", None)
            if fn is not None:
                return int(fn(asset)), "StaticMeshEditorSubsystem"
        except Exception as e:
            _info("  simple collision count", "StaticMeshEditorSubsystem: {0}".format(
                str(e).splitlines()[0]))
    library = getattr(unreal, "EditorStaticMeshLibrary", None)
    fn = getattr(library, "get_simple_collision_count", None) if library else None
    if fn is not None:
        try:
            return int(fn(asset)), "EditorStaticMeshLibrary"
        except Exception as e:
            _info("  simple collision count", "EditorStaticMeshLibrary: {0}".format(
                str(e).splitlines()[0]))
    return None, "no counting API in this build"


def _complex_as_simple(asset, name):
    """Make the render triangles the collision, and READ THE FLAG BACK.

    unreal.BodySetup.collision_trace_flag is a CollisionTraceFlag, whose members are
    CTF_USE_DEFAULT, CTF_USE_SIMPLE_AND_COMPLEX, CTF_USE_SIMPLE_AS_COMPLEX and
    CTF_USE_COMPLEX_AS_SIMPLE — from Epic's Python API reference, not from memory."""
    if not COLLISION_COMPLEX_AS_SIMPLE:
        return
    enum = getattr(unreal, "CollisionTraceFlag", None)
    wanted = getattr(enum, COLLISION_TRACE_FLAG, None) if enum else None
    if wanted is None:
        _note("unreal.CollisionTraceFlag." + COLLISION_TRACE_FLAG + " is not in this build, "
              "so " + name + " keeps box collision and anything sunk into it — a Cap in a "
              "terminal recess, a Rail in a Junction socket — cannot be aimed at.")
        return

    body = None
    try:
        body = asset.get_editor_property("body_setup")
    except Exception as e:
        _info("  " + name + " body_setup", "unreadable: {0}".format(str(e).splitlines()[0]))
    if body is None:
        _note(name + ": no body_setup, so the collision trace flag could not be set.")
        return

    try:
        body.set_editor_property("collision_trace_flag", wanted)
    except Exception as e:
        _note(name + ": collision_trace_flag could not be set: {0}".format(
            str(e).splitlines()[0]))
        return

    try:
        back = body.get_editor_property("collision_trace_flag")
    except Exception:
        back = None
    if back == wanted:
        _info("  " + name + " trace flag", COLLISION_TRACE_FLAG)
    else:
        _note(name + ": collision_trace_flag read back as {0}, not {1}.".format(
            back, COLLISION_TRACE_FLAG))


def _collision(mesh, path, name, collision_mesh=None):
    """Generate simple collision on the just-created static mesh, and READ IT BACK.

    Signature, verbatim from the Python API reference:

        set_static_mesh_collision_from_mesh(from_dynamic_mesh, to_static_mesh_asset, options,
            static_mesh_collision_options=[True], debug=None) -> DynamicMesh
    """
    if not COLLISION:
        _info("  " + name + " collision", "COLLISION is False — none generated")
        return
    library = getattr(unreal, "GeometryScript_Collision", None)
    fn = getattr(library, "set_static_mesh_collision_from_mesh", None) if library else None
    if fn is None:
        _note("unreal.GeometryScript_Collision.set_static_mesh_collision_from_mesh is not in "
              "this build, so " + name + " has NO collision and nothing built from it can be "
              "targeted in game.")
        return

    method, method_note = _collision_method()
    if method is None:
        _note(name + " collision: " + method_note)
        return

    try:
        asset = unreal.EditorAssetLibrary.load_asset(path)
    except Exception as e:
        _info("  " + name + " collision", "asset not loadable: {0}".format(e))
        return
    if asset is None:
        _info("  " + name + " collision", "asset not loadable")
        return

    options = unreal.GeometryScriptCollisionFromMeshOptions()
    try:
        options.set_editor_property("method", method)
        if "CONVEX" in method_note:
            # Without a budget the decomposition is not attempted and one hull comes back,
            # which is the same answer ALIGNED_BOXES already gives.
            options.set_editor_property("max_convex_hulls_per_mesh", COLLISION_MAX_HULLS)
    except Exception as e:
        _info("  " + name + " collision", "method could not be set: {0}".format(
            str(e).splitlines()[0]))
        return

    try:
        fn(collision_mesh if collision_mesh is not None else mesh, asset, options)
    except Exception as e:
        _note(name + " collision FAILED: {0}".format(e))
        return

    _complex_as_simple(asset, name)

    count, how = _simple_collision_count(asset)
    if count is None:
        _info("  " + name + " collision", "generated via {0}; shape count unreadable ({1})".format(
            method_note, how))
    elif count > 0:
        _info("  " + name + " collision", "{0} simple shape(s) via {1} [{2}]".format(
            count, method_note, how))
    else:
        _note(name + " collision: the call succeeded but the asset reports 0 simple shapes "
                     "[{0}] — it will still be untargetable in game.".format(how))



# --- per-mesh slot tables -----------------------------------------------------
#
# The material IDs above are one rule for every mesh, which would leave every simple mesh with
# empty slots in between (2..6 on a collar) — and every hologram, blueprint duplicate and build
# effect walks every slot. So the IDs are COMPACTED at save time, per mesh, to the ones that hold
# triangles, and each surviving slot is NAMED for what it is. The names are the contract:
# acpr_materials.py fills slots by name and the C++ finds them by name (ACPRSlot in
# ACPRTerminalHost.h), so no number is restated anywhere.
#
#     Body            material ID 0
#     Cue             ID 1 on every mesh but the Junction
#     Cue_+X .. Cue_-Z   IDs 1..6 on the Junction, in _face_slot order
#     Accent          ID 7
#     Power           ID 8 on the collar
#     Power_+X .. Power_-Z   IDs 8..13 on the Junction, in _power_slot order
#
# Verified names (Epic's Python API reference, unreal.GeometryScript_Materials, 5.3):
#     remap_material_i_ds( target_mesh, from_material_id, to_material_id, debug=None ) -> DynamicMesh
#     get_triangle_material_id( target_mesh, triangle_id ) -> ( int32, is_valid_triangle=bool )
# compact_material_i_ds exists too, but it wants the asset's material list and reorders on its
# own terms; a remap in ascending order is explicit and every step is logged.
def _slot_name_for_id(material_id, name):
    """The slot name a material ID carries on mesh `name` (the asset's short name)."""
    junction = (name == "SM_ACPR_Junction")
    if material_id == MATERIAL_ID_BODY:
        return "Body"
    if MATERIAL_ID_CUE <= material_id < MATERIAL_ID_CUE + 6:
        if junction:
            return "Cue_" + JUNCTION_FACE_NAMES[material_id - MATERIAL_ID_CUE]
        return "Cue" if material_id == MATERIAL_ID_CUE else "Cue_unused_{0}".format(material_id)
    if material_id == MATERIAL_ID_ACCENT:
        return "Accent"
    if MATERIAL_ID_POWER <= material_id < MATERIAL_ID_POWER + 6:
        if junction:
            return "Power_" + JUNCTION_FACE_NAMES[material_id - MATERIAL_ID_POWER]
        return "Power" if material_id == MATERIAL_ID_POWER else "Power_unused_{0}".format(material_id)
    return "Id_{0}".format(material_id)


def _material_id_tally(mesh):
    """{ material ID: triangle count } — one walk, the census's own calls. None if unreadable."""
    queries = getattr(unreal, "GeometryScript_MeshQueries", None)
    materials = getattr(unreal, "GeometryScript_Materials", None)
    count_fn = getattr(queries, "get_num_triangle_i_ds", None) if queries else None
    id_fn = getattr(materials, "get_triangle_material_id", None) if materials else None
    if count_fn is None or id_fn is None:
        return None
    tally = {}
    try:
        for triangle in range(int(count_fn(mesh))):
            value = id_fn(mesh, triangle)
            if isinstance(value, (tuple, list)):
                value = value[0]
            tally[int(value)] = tally.get(int(value), 0) + 1
    except Exception as e:
        _note("material ID tally failed: {0}".format(e))
        return None
    return tally


def _compact_slots(mesh, name):
    """Remap the mesh's material IDs to 0..N-1 over the IDs that hold triangles, ascending, and
    return the slot names in that order — or None (mesh left as it is) if anything is missing."""
    library = getattr(unreal, "GeometryScript_Materials", None)
    remap = getattr(library, "remap_material_i_ds", None) if library else None
    tally = _material_id_tally(mesh)
    if remap is None or tally is None:
        _note(name + ": slots not compacted (remap_material_i_ds or the tally is unavailable); "
                     "the mesh keeps its sparse slots and the C++ will warn about missing names.")
        return None

    used = sorted(tally)
    names = []
    for new_id, old_id in enumerate(used):
        names.append(_slot_name_for_id(old_id, name))
        if old_id == new_id:
            continue
        # Ascending: every ID below old_id has already landed on a value < new_id, so new_id is
        # free — the remap can never merge two slots.
        try:
            mesh = remap(mesh, old_id, new_id)
        except Exception as e:
            _note("{0}: remap_material_i_ds({1} -> {2}) failed: {3}".format(name, old_id, new_id, e))
            return None
    _info("  " + name + " slots compacted",
          ", ".join("{0}={1} ({2} tris)".format(i, n, tally[old]) for i, (n, old) in enumerate(zip(names, used))))
    return names


def _name_slots(path, name, names):
    """Write the slot names onto the new asset; materials stay empty for acpr_materials.py."""
    if not names:
        return
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        _note(name + ": asset not loadable after creation; slot names not written")
        return
    try:
        slots = list(asset.get_editor_property("static_materials") or [])
    except Exception as e:
        _note("{0}: static_materials unreadable: {1}".format(name, e))
        return
    if len(slots) != len(names):
        _note("{0}: the asset has {1} slot(s) but {2} were compacted — names not written, check the "
              "remap log above".format(name, len(slots), len(names)))
        return
    wanted = []
    for slot, slot_name in zip(slots, names):
        try:
            material = slot.get_editor_property("material_interface")
        except Exception:
            material = None
        wanted.append(unreal.StaticMaterial(material_interface=material, material_slot_name=slot_name))
    try:
        asset.set_editor_property("static_materials", wanted)
    except Exception as e:
        _note("{0}: could not name the slots: {1}".format(name, e))


def _save(mesh, name, collision_mesh=None):
    path = MESH_DIR + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        if not OVERWRITE:
            _info(name, "exists, left alone (OVERWRITE is False)")
            return False
        try:
            unreal.EditorAssetLibrary.delete_asset(path)
        except Exception as e:
            _info(name, "could not delete the existing asset: {0}".format(e))
            _failed.append(name)
            return False

    # The slot table is compacted and named here, at the one place every mesh passes.
    names = _compact_slots(mesh, name)

    try:
        options = unreal.GeometryScriptCreateNewStaticMeshAssetOptions()
        result = unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(
            mesh, path, options)
        _info(name, "created -> {0}".format(result))
        _name_slots(path, name, names)
        # Collision BEFORE the save, so the shapes are in the package that gets written rather
        # than in a dirty asset nobody flushes.
        _collision(mesh, path, name, collision_mesh)
        unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
        _built.append(name)
        return True
    except Exception as e:
        _info(name, "FAILED: {0}".format(e))
        _failed.append(name)
        return False


# ---------------------------------------------------------------------------
# Shared shaping helpers
# ---------------------------------------------------------------------------

def _cylinder(mesh, radius, height, at=None, steps=24, capped=True):
    options = unreal.GeometryScriptPrimitiveOptions()
    kwargs = {"radius": radius, "height": height, "radial_steps": steps, "capped": capped}
    if _ORIGIN_BASE is not None:
        kwargs["origin"] = _ORIGIN_BASE
    return unreal.GeometryScript_Primitives.append_cylinder(
        mesh, options, at or _xf(), **kwargs)


def _frame_tool(thickness, outer, inner, axis, offset):
    """A square frame: a slab of `thickness` along `axis`, `outer` across, with an
    `inner` square removed. Subtracting it from the cube cuts a groove that wraps
    the four faces adjacent to `axis` — which is how each Junction face's indicator
    ends up on its NEIGHBOURS, and so stays visible when two Junctions touch."""
    dims_outer = [outer, outer, outer]
    dims_inner = [inner, inner, inner]
    dims_outer[axis] = thickness
    dims_inner[axis] = thickness * 2.0
    where = [0.0, 0.0, 0.0]
    where[axis] = offset

    frame = _new_mesh()
    _box(frame, dims_outer[0], dims_outer[1], dims_outer[2], at=_xf(*where))
    hole = _new_mesh()
    _box(hole, dims_inner[0], dims_inner[1], dims_inner[2], at=_xf(*where))
    return _subtract(frame, hole)


def _chamfer(mesh, side, low, high, width=None, material_id=None, axis=2):
    """Cut the four long corners of a square profile at 45 degrees and return
    ( mesh, volumes ) so the new faces can be marked as the accent.

    A CUT, NOT A BEVEL, AND THE REASON IS THE JUNCTION. apply_mesh_polygroup_bevel
    bevels every polygroup BOUNDARY, and after the booleans every groove, indent and
    socket recess is its own polygroup — so a bevel would put the accent on every
    internal edge in the mesh, not on the four outer ones. Four rotated box cutters are
    exact, they are the same boolean machinery the grooves already use, and the faces
    they create can be found analytically exactly like a groove's walls.

    THE ARITHMETIC, WRITTEN OUT, BECAUSE THE VISIBLE WIDTH IS NOT THE CUT DEPTH. The cut
    takes `leg` off each of the two faces meeting at a corner; the face it leaves is the
    hypotenuse, so it is leg * sqrt(2) wide. `width` is the VISIBLE one — what "all the
    accent strips are the same width" is measured on — so the leg is width / sqrt(2).

        corner at ( side/2, side/2 ), cut line from ( side/2 - leg, side/2 )
        to ( side/2, side/2 - leg ), which is the plane  x + y = side - leg

    The cutter is a square box turned 45 degrees ABOUT `axis`, so its near face lies on
    that plane. A square is symmetric under 90 degree turns, so the same 45 serves all
    four edges and only the centre's signs change.

    `axis` is 0, 1 or 2, and the two perpendicular coordinates follow it cyclically —
    X -> ( Y, Z ), Y -> ( Z, X ), Z -> ( X, Y ). Four calls at axis 2 chamfer a Rail; all
    three axes chamfer a cube's twelve edges. The corners where chamfers meet are cut more
    than once, which is what a chamfered cube looks like, and the last cut's facet lands on
    that cut's plane so it is marked with the rest.

    `low` / `high` are the solid's own extent ALONG `axis`. The cutter overshoots both,
    the VOLUME does not — the same split the grooves use, so the end caps' triangles
    stay body instead of being claimed as accent.
    """
    width = ACCENT_WIDTH if width is None else width
    material_id = MATERIAL_ID_ACCENT if material_id is None else material_id
    if width <= 0.0 or side <= 0.0:
        return mesh, []

    root2 = math.sqrt(2.0)
    leg = width / root2
    plane_d = side - leg

    # Generous: the cutter only has to cover the edge it is removing.
    box = side
    # Centre distance along the diagonal, so the cutter's near face lands on plane_d.
    diagonal = plane_d / root2 + box * 0.5
    offset = diagonal / root2
    length = ( high - low ) * 1.4 + side
    centre = ( low + high ) * 0.5

    # Cyclic: axis 2 gives ( X, Y ), which is the Rail's frame.
    u = ( axis + 1 ) % 3
    v = ( axis + 2 ) % 3

    volumes = []
    for su, sv in ( ( 1.0, 1.0 ), ( -1.0, 1.0 ), ( 1.0, -1.0 ), ( -1.0, -1.0 ) ):
        dims = [ box, box, box ]
        dims[ axis ] = length
        pos = [ 0.0, 0.0, 0.0 ]
        pos[ axis ] = centre
        pos[ u ] = su * offset
        pos[ v ] = sv * offset
        # unreal.Rotator is ( roll, pitch, yaw ) — roll turns about X, pitch about Y, yaw
        # about Z — so the component to set is the axis index itself.
        turn = [ 0.0, 0.0, 0.0 ]
        turn[ axis ] = 45.0

        tool = _new_mesh()
        _box(tool, dims[ 0 ], dims[ 1 ], dims[ 2 ],
             at=unreal.Transform(unreal.Vector(pos[ 0 ], pos[ 1 ], pos[ 2 ]),
                                 unreal.Rotator(turn[ 0 ], turn[ 1 ], turn[ 2 ]),
                                 unreal.Vector(1.0, 1.0, 1.0)))
        tool = _tag(tool, material_id)
        volumes.append(_vol_plane(axis, su, sv, plane_d, low, high, tool=tool,
                                  material_id=material_id))
        mesh = _subtract(mesh, tool)

    return mesh, volumes


def _vol_plane(axis, sign_u, sign_v, plane_d, low, high, tool=None, pad=None,
               material_id=None):
    """The analytic twin of one of _chamfer's cutters: the 45 degree face itself.

    A triangle lying IN the cut plane has its centroid exactly on it, so a small pad is
    enough and nothing else in these meshes comes near — the Rail's grooves run down the
    middle of each face, a quarter of the profile from any corner, and the Junction's
    strips stop at JUNCTION_STRIP_FAR, which is inside where the chamfer begins. The
    quadrant test keeps the four edges from claiming each other's triangles, and the range
    along `axis` keeps the end caps out."""
    pad = CUE_PAD if pad is None else pad
    u = ( axis + 1 ) % 3
    v = ( axis + 2 ) % 3

    def test(x, y, z):
        point = ( x, y, z )
        if point[ axis ] < low - pad or point[ axis ] > high + pad:
            return False
        if sign_u * point[ u ] < 0.0 or sign_v * point[ v ] < 0.0:
            return False
        return abs(sign_u * point[ u ] + sign_v * point[ v ] - plane_d) <= pad

    return _Volume(test, "chamfer axis {0} ({1:+.0f},{2:+.0f}) at {3:.1f}".format(
        axis, sign_u, sign_v, plane_d), tool, material_id)


def _tag(mesh, material_id):
    """Paint every triangle currently in `mesh` with one material ID.

    Used on a finished solid (ID 0) and on each cutting tool (ID 1) before it is
    subtracted, so the walls of every groove, pad and ring land in slot 1. Degrades
    to a no-op with a note: a mesh with one slot is still a correct mesh."""
    library = getattr(unreal, "GeometryScript_Materials", None)
    if library is None:
        _note("unreal.GeometryScript_Materials is not available in this build, "
                   "so the meshes have a single material slot and the emissive cues "
                   "will need a different split (a second mesh, or UVs and a mask).")
        return mesh

    enable = getattr(library, "enable_material_i_ds", None)
    if enable is not None:
        try:
            result = enable(mesh)
            if result is not None:
                mesh = result
        except Exception as e:
            _info("enable_material_i_ds", "failed: {0}".format(e))

    clear = getattr(library, "clear_material_i_ds", None)
    if clear is None:
        _note("GeometryScript_Materials has no clear_material_i_ds; material "
                   "slots not assigned.")
        return mesh
    try:
        result = clear(mesh, material_id)
        if result is not None:
            mesh = result
    except Exception as e:
        _info("clear_material_i_ds({0})".format(material_id), "failed: {0}".format(e))
    return mesh


def _tris(mesh):
    """How many triangles the mesh has right now, or -1.

    `GeometryScript_MeshQueries.get_num_triangle_i_ds( target_mesh ) -> int32`, from
    Epic's Python reference. (get_all_triangle_material_i_ds returns a THREE-tuple
    ( target_mesh, index_list, has_ids ), so its length is not a count.)"""
    library = getattr(unreal, "GeometryScript_MeshQueries", None)
    if library is None:
        return -1
    for name in ("get_num_triangle_i_ds", "get_vertex_count"):
        fn = getattr(library, name, None)
        if fn is None:
            continue
        try:
            return int(fn(mesh))
        except Exception:
            continue
    return -1


def _stage(mesh, label, previous):
    """Log a build stage and the triangles it added. Returns the new count."""
    count = _tris(mesh)
    if count < 0:
        return count
    if previous < 0:
        _info("  " + label, "{0} triangles".format(count))
    else:
        delta = count - previous
        note = "" if delta else "   <-- this step changed nothing"
        _info("  " + label, "{0} triangles ({1:+d}){2}".format(count, delta, note))
    return count


# ---------------------------------------------------------------------------
# Cue volumes: which surfaces glow, decided by arithmetic
# ---------------------------------------------------------------------------
#
# Neither the boolean nor a containment test built on the boolean's tool can say which
# surfaces belong to a cut. A cutting tool's material ID does carry onto the walls it
# cuts — and onto a great deal more besides, because cutting a groove across a face
# RETRIANGULATES THE WHOLE FACE and every new triangle takes the tool's ID. And the
# retriangulation does not produce small triangles around the cut — it produces SLIVERS
# that span the entire face, from a groove rim right across to the far corner. Two of such
# a sliver's three vertices sit on the groove rim, i.e. inside the tool, so a
# two-vertices-inside test selects a triangle that covers half the Rail.
#
# A triangle's VERTICES say nothing about where the triangle is. Its centroid does. So
# nothing is asked of the boolean or of the selection library: every cue feature is also
# declared as an analytic volume — a box, a square frame, a cylindrical band, all of which
# these shapes are made of — and a triangle is a cue triangle exactly when its centroid
# falls inside one. A sliver spanning a face has its centroid out on the face and is body;
# a groove wall's centroid is in the groove and is cue. No heuristic, no tolerance beyond
# a 1 mm shell for surfaces that lie exactly on a volume's boundary, and the same answer
# every run.
#
# The volumes are built from the same constants as the cutting tools, in the same
# helper call, so the two cannot drift apart.


class _Volume(object):
    """One cue feature, described as arithmetic instead of as geometry.

    `tool` is the DynamicMesh that cut it, kept only so the fallback path below has
    something to hand to select_mesh_elements_inside_mesh if the analytic route
    cannot run at all in this build."""

    def __init__(self, test, label, tool=None, material_id=None, normal_test=None):
        self.test = test
        self.label = label
        self.tool = tool
        # Optional second test on the triangle's (unnormalised) winding normal — for a surface
        # that a centroid box cannot single out, such as a chamfer whose neighbours share its
        # centroid band. See _vol_cone.
        self.normal_test = normal_test
        # Which slot this cue lands in. One for every cue on the Rail and the Outlet;
        # ONE PER FACE on the Junction, because a material instance is a single set of
        # values and cannot say "this face is coupled and that one is not".
        self.material_id = MATERIAL_ID_CUE if material_id is None else material_id

    def contains(self, x, y, z, normal=None):
        if not self.test(x, y, z):
            return False
        if self.normal_test is not None and normal is not None:
            return self.normal_test(normal)
        return True


def _vol_box(cx, cy, cz, sx, sy, sz, tool=None, pad=None, material_id=None):
    pad = CUE_PAD if pad is None else pad
    hx, hy, hz = sx * 0.5 + pad, sy * 0.5 + pad, sz * 0.5 + pad

    def test(x, y, z):
        return abs(x - cx) <= hx and abs(y - cy) <= hy and abs(z - cz) <= hz

    return _Volume(test, "box {0:.0f}x{1:.0f}x{2:.0f}".format(sx, sy, sz), tool, material_id)


def _vol_frame(axis, offset, thickness, outer, inner, tool=None, pad=None,
               material_id=None):
    """The analytic twin of _frame_tool: a slab `thickness` deep along `axis`, a
    square `outer` across, with a square `inner` hole. The hole is SHRUNK by the pad
    and the outside GROWN by it, so a wall lying exactly on either boundary counts."""
    pad = CUE_PAD if pad is None else pad
    half_thick = thickness * 0.5 + pad
    outer_half = outer * 0.5 + pad
    inner_half = inner * 0.5 - pad

    def test(x, y, z):
        point = (x, y, z)
        if abs(point[axis] - offset) > half_thick:
            return False
        cross = max(abs(point[i]) for i in range(3) if i != axis)
        return inner_half <= cross <= outer_half

    return _Volume(test, "frame axis {0}".format(axis), tool, material_id)


def _vol_band(z_low, z_high, r_inner, r_outer, tool=None, pad=None, material_id=None):
    """The Outlet's ribs: a cylindrical band about the Z axis."""
    pad = CUE_PAD if pad is None else pad

    def test(x, y, z):
        if z < z_low - pad or z > z_high + pad:
            return False
        radius = math.sqrt(x * x + y * y)
        return (r_inner - pad) <= radius <= (r_outer + pad)

    return _Volume(test, "band r {0:.0f}..{1:.0f}".format(r_inner, r_outer), tool, material_id)


def _vol_cone(z_low, z_high, r_low, r_high, tool=None, pad=None, material_id=None):
    """The Outlet's cap chamfer: the conical surface running from radius r_low at z_low to
    r_high at z_high.

    Selected by slope, not by position alone. A centroid band round the cone is not enough
    here, because the surfaces the chamfer meets share that band: the top cap's rim triangles
    sit at z_high on the cone's radius there, and the cylinder wall's at z_low. Excluding the
    ends loses chamfer triangles instead — the boolean does not cut the chamfer into tidy
    quads, it fans slivers from the top rim, and their centroids sit within a millimetre of
    z_high.

    What no neighbour shares is the SLOPE: the chamfer's normal is 45 degrees to Z, the wall's
    is 0, the cap's and the rib grooves' walls are 90. So the position test is loose — the z
    band padded, the radius within 1 cm of the cone — and the normal test is what decides: the
    winding normal's |z| component must be sin(45) within 0.15, i.e. between about 37 and 53
    degrees. |z| rather than z because the winding is only made consistent later, in _finish."""
    tolerance = 1.0 if pad is None else pad
    slope = (r_high - r_low) / (z_high - z_low) if z_high > z_low else 0.0
    wanted = math.sin(math.radians(45.0))

    def test(x, y, z):
        if z < z_low - 0.6 or z > z_high + 0.6:
            return False
        radius = math.sqrt(x * x + y * y)
        expected = r_low + (z - z_low) * slope
        return abs(radius - expected) <= tolerance

    def normal_test(n):
        length = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
        if length <= 1e-9:
            return False
        return abs(abs(n[2]) / length - wanted) <= 0.15

    return _Volume(test, "cone r {0:.0f}..{1:.0f}".format(r_low, r_high), tool, material_id,
                   normal_test=normal_test)


def _triangle_count(mesh):
    queries = getattr(unreal, "GeometryScript_MeshQueries", None)
    fn = getattr(queries, "get_num_triangle_i_ds", None) if queries else None
    try:
        return int(fn(mesh)) if fn else -1
    except Exception:
        return -1


def _vectors(result):
    """Pull every Vector-shaped thing out of whatever tuple a call handed back.

    get_triangle_positions has three out-parameters and a bool, and the order of a tuple
    of out-parameters is not something to assume (get_max_material_id reads id-then-flag;
    get_all_triangle_material_i_ds is a three-tuple). Picking the vectors out by shape
    cannot be got the wrong way round."""
    if result is None:
        return []
    items = list(result) if isinstance(result, (tuple, list)) else [result]
    out = []
    for item in items:
        if hasattr(item, "x") and hasattr(item, "y") and hasattr(item, "z"):
            try:
                out.append((float(item.x), float(item.y), float(item.z)))
            except Exception:
                continue
    return out


def _centroids(mesh, total):
    """Per triangle: ( centroid_x, centroid_y, centroid_z, area, cross_product ), with
    None where a triangle ID is a gap rather than a triangle. Returns None only if NOT
    ONE could be read, which is the case that means the call itself is unusable; a
    handful of dead IDs in a mesh that has been booleaned a few dozen times is normal
    and must not throw the whole pass onto the fallback.

    The area comes along free — the cross product is already being formed — and it is
    what the cue's sanity check needs (see CUE_SANITY_AREA_FRACTION). The unnormalised
    cross product is the winding-order normal, which is what _report_shape tests."""
    queries = getattr(unreal, "GeometryScript_MeshQueries", None)
    fn = getattr(queries, "get_triangle_positions", None) if queries else None
    if fn is None:
        return None
    points = []
    skipped = 0
    first_reason = ""
    for triangle in range(total):
        corners = []
        try:
            corners = _vectors(fn(mesh, triangle))
        except Exception as e:
            if not first_reason:
                first_reason = "{0} at triangle {1}".format(e, triangle)
        if len(corners) != 3:
            if not first_reason:
                first_reason = "{0} vectors at triangle {1}, expected 3".format(
                    len(corners), triangle)
            skipped += 1
            points.append(None)
            continue
        a, b, c = corners
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        area = 0.5 * math.sqrt(nx * nx + ny * ny + nz * nz)
        points.append(((a[0] + b[0] + c[0]) / 3.0,
                       (a[1] + b[1] + c[1]) / 3.0,
                       (a[2] + b[2] + c[2]) / 3.0,
                       area, (nx, ny, nz)))
    if skipped:
        _info("get_triangle_positions",
              "{0} of {1} triangle IDs gave no position ({2})".format(
                  skipped, total, first_reason))
    if skipped >= total:
        return None
    return points


def _selection_type_triangles():
    enum = getattr(unreal, "GeometryScriptMeshSelectionType", None)
    if enum is None:
        return None
    for name in ("TRIANGLES", "TRIANGLE", "FACES"):
        value = getattr(enum, name, None)
        if value is not None:
            return value
    return None


def _selection_from_indices(mesh, indices):
    """Turn a list of triangle indices into a GeometryScriptMeshSelection."""
    selector = getattr(unreal, "GeometryScript_MeshSelection", None)
    if selector is None:
        return None
    kind = _selection_type_triangles()
    selection_class = getattr(unreal, "GeometryScriptMeshSelection", None)
    for name in ("convert_index_array_to_mesh_selection",
                 "convert_index_list_to_mesh_selection"):
        fn = getattr(selector, name, None)
        if fn is None:
            continue
        try:
            result = fn(mesh, indices, kind) if kind is not None else fn(mesh, indices)
        except Exception as e:
            _info(name, "failed: {0}".format(e))
            continue
        items = list(result) if isinstance(result, (tuple, list)) else [result]
        if selection_class is not None:
            for item in items:
                if isinstance(item, selection_class):
                    return item
        mesh_class = getattr(unreal, "DynamicMesh", None)
        for item in items:
            if item is None:
                continue
            if mesh_class is not None and isinstance(item, mesh_class):
                continue
            return item
    return None


def _mark_cue(mesh, volumes, name):
    """Put the cue material IDs on exactly the cue surfaces, by testing triangle
    centroids against the analytic volumes. Falls back to a containment test, at
    min_num_triangle_points = 3, only if the centroids cannot be read at all."""
    volumes = [v for v in volumes if v is not None]
    if not volumes:
        return mesh

    total = _tris(mesh)
    if total <= 0:
        _info("  " + name + " cue", "no triangle count; cue surfaces not marked")
        return mesh

    points = _centroids(mesh, total)
    if points is None:
        _note("get_triangle_positions is unavailable or did not return three vectors, "
              "so the cue surfaces fall back to a containment test. Check the per-slot "
              "census below: if the cue is a large share of any mesh, that test has "
              "over-selected and the fix is a different one.")
        return _mark_cue_by_volume(mesh, volumes, name)

    by_slot = {}
    indices = []
    per_volume = [0] * len(volumes)
    cue_area = 0.0
    all_area = 0.0
    for triangle, point in enumerate(points):
        if point is None:
            continue
        x, y, z, area = point[0], point[1], point[2], point[3]
        normal = point[4] if len(point) > 4 else None
        all_area += area
        for slot, volume in enumerate(volumes):
            if volume.contains(x, y, z, normal):
                indices.append(triangle)
                by_slot.setdefault(volume.material_id, []).append(triangle)
                per_volume[slot] += 1
                cue_area += area
                break

    empty = [volumes[i].label for i, hits in enumerate(per_volume) if hits == 0]
    share = float(len(indices)) / float(total)
    area_share = (cue_area / all_area) if all_area > 0.0 else 0.0
    _info("  " + name + " cue", "{0} of {1} triangles ({2}% by count, {3}% by AREA) "
          "in {4} volume(s), by centroid".format(
              len(indices), total, int(round(100.0 * share)),
              int(round(100.0 * area_share)), len(volumes)))
    if empty:
        _note("{0}: {1} cue volume(s) caught no triangles ({2}). Either the cut did not "
              "happen or the volume does not describe it — both are bugs, and the mesh "
              "will be missing that cue.".format(name, len(empty), ", ".join(empty[:4])))
    if area_share > CUE_SANITY_AREA_FRACTION:
        _note("{0}: the cue covers {1}% of the SURFACE, which is far more than thin "
              "grooves can account for. The volumes are wrong, not the mesh.".format(
                  name, int(round(100.0 * area_share))))
    if not indices:
        return mesh

    materials = getattr(unreal, "GeometryScript_Materials", None)
    assign = getattr(materials, "set_material_id_for_mesh_selection", None) if materials else None
    if assign is None:
        _note("set_material_id_for_mesh_selection is missing, so every mesh has one slot.")
        return mesh

    # One selection per material ID. On the Rail that is a single pass; on the Junction
    # it is six, one per face.
    for material_id in sorted(by_slot):
        selection = _selection_from_indices(mesh, by_slot[material_id])
        if selection is None:
            _note("No triangle-index selection could be built (neither "
                  "convert_index_array_to_mesh_selection nor "
                  "convert_index_list_to_mesh_selection worked), so the cue surfaces "
                  "fall back to a containment test.")
            return _mark_cue_by_volume(mesh, volumes, name)
        try:
            result = assign(mesh, selection, material_id)
            if result is not None:
                mesh = result
        except Exception as e:
            _info(name + " cue assignment", "slot {0} failed: {1}".format(material_id, e))
    if len(by_slot) > 1:
        _info("  " + name + " cue slots",
              ", ".join("{0}={1} tris".format(k, len(v)) for k, v in sorted(by_slot.items())))
    return mesh


def _mark_cue_by_volume(mesh, volumes, name):
    """The containment route, a fallback only, at min_num_triangle_points = 3 so that a
    sliver with two vertices on a groove rim is not swept up."""
    selector = getattr(unreal, "GeometryScript_MeshSelection", None)
    materials = getattr(unreal, "GeometryScript_Materials", None)
    select = getattr(selector, "select_mesh_elements_inside_mesh", None) if selector else None
    assign = getattr(materials, "set_material_id_for_mesh_selection", None) if materials else None
    if select is None or assign is None:
        _note("Neither the analytic route nor the containment route is available, so "
              "every mesh has a single material slot and no cue.")
        return mesh

    marked = 0
    for volume in volumes:
        if volume.tool is None:
            continue
        try:
            selection = select(mesh, volume.tool, _xf(), shell_distance=CUE_SHELL,
                               min_num_triangle_points=3)
        except Exception as e:
            _info(name + " cue selection", "failed: {0}".format(e))
            break
        if isinstance(selection, (tuple, list)) and len(selection) == 2:
            selection = selection[1]
        try:
            result = assign(mesh, selection, MATERIAL_ID_CUE)
            if result is not None:
                mesh = result
            marked += 1
        except Exception as e:
            _info(name + " cue assignment", "failed: {0}".format(e))
            break
    _info("  " + name + " cue volumes", "{0} of {1} marked (fallback)".format(
        marked, len(volumes)))
    return mesh


def _orientation(mesh, total):
    """What fraction of this mesh's surface faces OUTWARD, by area.

    For each triangle, ( b - a ) x ( c - a ) is the normal its winding order implies,
    dotted against the vector from the mesh's centre to the triangle's centre.

    The sign of that dot product is NEGATIVE for an outward face. Unreal is left-handed
    and winds its front faces the opposite way to the right-handed cross product used
    here, so a correctly built solid measures as "inward" under the naive test: a plain
    append_box primitive with no boolean anywhere near it measures 0% outward by the
    naive sign, and a Geometry Script primitive is not inside out.

    The pass does not CHANGE anything on the strength of this number. It reports, and
    FLIP_INSIDE_OUT stays False unless a human has looked at the evidence.

    Returns ( outward_area_fraction, samples ) or ( None, 0 )."""
    points = _centroids(mesh, total)
    if points is None:
        return None, 0
    live = [p for p in points if p is not None]
    if not live:
        return None, 0
    cx = sum(p[0] for p in live) / len(live)
    cy = sum(p[1] for p in live) / len(live)
    cz = sum(p[2] for p in live) / len(live)
    outward = 0.0
    total_area = 0.0
    for x, y, z, area, normal in live:
        if area <= 0.0:
            continue
        dot = normal[0] * (x - cx) + normal[1] * (y - cy) + normal[2] * (z - cz)
        total_area += area
        if dot < 0.0:
            outward += area
    if total_area <= 0.0:
        return None, 0
    return outward / total_area, len(live)


def _report_shape(mesh, name):
    """Is this mesh closed, in one piece, and pointing the right way out?

    A mesh with reversed winding renders as a different material from its neighbour under
    the same instance and the same light, and is see-through from outside, so its lit
    faces take the colour of the background — which reads exactly like a material problem
    and is not one. Measured on every mesh, every build."""
    queries = getattr(unreal, "GeometryScript_MeshQueries", None)
    if queries is None:
        return
    bits = []
    for label, fname in (("closed", "get_is_closed_mesh"),
                         ("open border edges", "get_num_open_border_edges"),
                         ("components", "get_num_connected_components"),
                         ("has triangle normals", "get_has_triangle_normals"),
                         ("triangle ID gaps", "get_has_triangle_id_gaps")):
        fn = getattr(queries, fname, None)
        if fn is None:
            continue
        try:
            value = fn(mesh)
        except Exception as e:
            bits.append("{0}=?({1})".format(label, e))
            continue
        if isinstance(value, (tuple, list)) and value:
            value = value[0]
        bits.append("{0}={1}".format(label, value))
    # get_mesh_volume_area returns ( AREA, VOLUME ), the opposite order to its name; the
    # arithmetic settles it: the Rail body's 980800 is 400x50x50 less four 6x2x400
    # grooves, and that is the second number.
    #
    # The VOLUME is also the authoritative winding test, and a better one than the
    # centroid measure in _orientation: a closed mesh wound inside out encloses a
    # NEGATIVE volume.
    fn = getattr(queries, "get_mesh_volume_area", None)
    if fn is not None:
        try:
            result = fn(mesh)
            numbers = [v for v in (result if isinstance(result, (tuple, list)) else [result])
                       if isinstance(v, float)]
            if len(numbers) >= 2:
                bits.append("area={0:.0f}; volume={1:.0f}{2}".format(
                    numbers[0], numbers[1],
                    "  <-- NEGATIVE: this mesh IS inside out" if numbers[1] < 0 else ""))
            elif numbers:
                bits.append("volume/area=" + "/".join("{0:.0f}".format(v) for v in numbers))
        except Exception:
            pass
    if bits:
        _info("  " + name + " shape", "; ".join(b for b in bits if b))


def _uv_span_for_set(mesh, uv_set, total_triangles=None):
    """( min_u, max_u, min_v, max_v ) for one UV set, or None.

    The numeric half of _uv_span, which formats. Shares its two rules: skip gap triangle
    IDs, and believe have_valid_u_vs rather than the vectors beside it — a gap's three
    zero vectors read as data would make every booleaned mesh report a fake 0..0.5 span."""
    queries = getattr(unreal, "GeometryScript_MeshQueries", None)
    fn = getattr(queries, "get_triangle_u_vs", None) if queries else None
    if fn is None:
        return None
    total = total_triangles if total_triangles else _tris(mesh)
    if total <= 0:
        return None
    valid_fn = getattr(queries, "is_valid_triangle_id", None)
    step = max(1, total // 200)
    lo_u = hi_u = lo_v = hi_v = None
    for triangle in range(0, total, step):
        if valid_fn is not None:
            try:
                if not valid_fn(mesh, triangle):
                    continue
            except Exception:
                pass
        try:
            result = fn(mesh, uv_set, triangle)
        except Exception:
            return None
        items = list(result) if isinstance(result, (tuple, list)) else [result]
        flags = [i for i in items if isinstance(i, bool)]
        if flags and not flags[-1]:
            continue
        for item in items:
            u = getattr(item, "x", None)
            v = getattr(item, "y", None)
            if u is None or v is None or getattr(item, "z", None) is not None:
                continue
            u, v = float(u), float(v)
            lo_u = u if lo_u is None else min(lo_u, u)
            hi_u = u if hi_u is None else max(hi_u, u)
            lo_v = v if lo_v is None else min(lo_v, v)
            hi_v = v if hi_v is None else max(hi_v, v)
    if lo_u is None:
        return None
    return (lo_u, hi_u, lo_v, hi_v)


def _uv_span(mesh, total_triangles=None):
    """How far the UVs actually spread, per set — the read-back that says whether the
    projection and the cell placement did what they were asked.

        GeometryScript_MeshQueries.get_triangle_u_vs( mesh, uv_set_index, triangle_id )

    Sampled rather than exhaustive: a few hundred triangles pin the span down and the
    call is not free."""
    queries = getattr(unreal, "GeometryScript_MeshQueries", None)
    fn = getattr(queries, "get_triangle_u_vs", None) if queries else None
    if fn is None:
        return ""
    total = total_triangles if total_triangles else _tris(mesh)
    if total <= 0:
        return ""
    valid_fn = getattr(queries, "is_valid_triangle_id", None)
    step = max(1, total // 200)
    parts = []
    for uv_set in range(UV_SETS):
        lo_u = lo_v = None
        hi_u = hi_v = None
        skipped = 0
        counted = 0
        for triangle in range(0, total, step):
            # A GAP IS NOT A TRIANGLE. get_num_triangle_i_ds counts IDs, not triangles,
            # and a booleaned mesh has gaps in that range. Asked about a gap,
            # get_triangle_u_vs hands back three zero vectors and a FALSE in
            # have_valid_u_vs; reading the vectors and ignoring the flag would report
            # a fake (0,0)..(0.5,0.5) span on every booleaned mesh.
            #
            #     get_triangle_u_vs( mesh, set, id ) -> ( uv1, uv2, uv3, have_valid_u_vs )
            if valid_fn is not None:
                try:
                    if not valid_fn(mesh, triangle):
                        skipped += 1
                        continue
                except Exception:
                    pass
            try:
                result = fn(mesh, uv_set, triangle)
            except Exception:
                return ""
            items = list(result) if isinstance(result, (tuple, list)) else [result]
            flags = [i for i in items if isinstance(i, bool)]
            if flags and not flags[-1]:
                skipped += 1
                continue
            for item in items:
                u = getattr(item, "x", None)
                v = getattr(item, "y", None)
                if u is None or v is None or getattr(item, "z", None) is not None:
                    continue
                u, v = float(u), float(v)
                lo_u = u if lo_u is None else min(lo_u, u)
                hi_u = u if hi_u is None else max(hi_u, u)
                lo_v = v if lo_v is None else min(lo_v, v)
                hi_v = v if hi_v is None else max(hi_v, v)
            counted += 1
        if lo_u is None:
            parts.append("uv{0}=? ({1} sampled IDs were gaps)".format(uv_set, skipped))
        else:
            parts.append("uv{0} span {1:.4f}x{2:.4f} at ({3:.3f},{4:.3f}) "
                         "[{5} real, {6} gaps]".format(
                             uv_set, hi_u - lo_u, hi_v - lo_v,
                             (lo_u + hi_u) * 0.5, (lo_v + hi_v) * 0.5, counted, skipped))
    return "; ".join(parts)


def _fix_orientation(mesh, name):
    """Measure which way the surface faces. Report it. Change nothing unless a human
    has set FLIP_INSIDE_OUT after reading the numbers."""
    total = _tris(mesh)
    if total <= 0:
        return mesh
    fraction, samples = _orientation(mesh, total)
    if fraction is None:
        _info("  " + name + " facing", "could not be measured")
        return mesh
    _info("  " + name + " facing",
          "{0}% of the surface area points outward ({1} triangles)".format(
              int(round(100.0 * fraction)), samples))
    if fraction >= 0.5:
        return mesh
    if not FLIP_INSIDE_OUT:
        _note("{0} measures only {1}% outward. NOTHING HAS BEEN CHANGED — the signed "
              "volume on the shape line above is the test that decides this, and "
              "FLIP_INSIDE_OUT is False. Turn it on only if that volume is negative."
              .format(name, int(round(100.0 * fraction))))
        return mesh

    _note("{0} measures {1}% outward and FLIP_INSIDE_OUT is set, so it is being "
          "flipped.".format(name, int(round(100.0 * fraction))))
    library = getattr(unreal, "GeometryScript_Normals", None)
    flipped = False
    for fname in ("flip_normals", "flip_mesh_normals"):
        fn = getattr(library, fname, None) if library else None
        if fn is None:
            continue
        try:
            result = fn(mesh)
            if result is not None:
                mesh = result
            flipped = True
            _info("  " + name + " facing", "flipped with " + fname)
            break
        except Exception as e:
            _info("  " + name + " " + fname, "failed: {0}".format(e))
    if not flipped:
        _note("Nothing in unreal.GeometryScript_Normals would flip {0}, so it stays "
              "inside out.".format(name))
        return mesh

    fraction, _ = _orientation(mesh, _tris(mesh))
    if fraction is None:
        _info("  " + name + " facing", "could not be re-measured after the flip")
    else:
        _info("  " + name + " facing",
              "now {0}% outward".format(int(round(100.0 * fraction))))
        if fraction < 0.5:
            _note("{0} is STILL inside out after the flip, so flip_normals is not the "
                  "right lever for it.".format(name))
    return mesh


def _slot_census(mesh, name):
    """How many triangles are in EACH material ID, not just what the highest one is.

    "Max material ID 1" says the mesh has two slots; it says nothing about which surfaces
    are in which, and a mesh whose body is in the cue slot and whose groove is in the
    body slot reports exactly the same thing as a correct one.

        GeometryScript_MeshQueries.get_num_triangle_i_ds( target_mesh )
        GeometryScript_Materials.get_triangle_material_id( target_mesh, triangle_id )

    One call per triangle is not elegant, but a few thousand calls once per build is
    cheap."""
    queries = getattr(unreal, "GeometryScript_MeshQueries", None)
    materials = getattr(unreal, "GeometryScript_Materials", None)
    count_fn = getattr(queries, "get_num_triangle_i_ds", None) if queries else None
    id_fn = getattr(materials, "get_triangle_material_id", None) if materials else None
    if count_fn is None or id_fn is None:
        return "per-slot counts unavailable"
    try:
        total = int(count_fn(mesh))
    except Exception as e:
        return "per-slot counts unavailable ({0})".format(e)

    tally = {}
    try:
        for triangle in range(total):
            value = id_fn(mesh, triangle)
            if isinstance(value, (tuple, list)):
                value = value[0]
            tally[int(value)] = tally.get(int(value), 0) + 1
    except Exception as e:
        return "per-slot counts failed after {0} of {1} ({2})".format(
            sum(tally.values()), total, e)

    parts = []
    for key in sorted(tally):
        if key == MATERIAL_ID_BODY:
            label = "body"
        elif name == "Junction" and MATERIAL_ID_CUE <= key < MATERIAL_ID_CUE + 6:
            label = "cue " + JUNCTION_FACE_NAMES[key - MATERIAL_ID_CUE]
        elif key == MATERIAL_ID_CUE:
            label = "cue"
        else:
            label = "id {0}".format(key)
        parts.append("{0}={1} tris ({2}%)".format(
            label, tally[key], int(round(100.0 * tally[key] / max(total, 1)))))
    return ", ".join(parts)


def _material_id_summary(mesh):
    """The highest material ID the mesh ACTUALLY ended up with. Returns a short string."""
    library = getattr(unreal, "GeometryScript_Materials", None)
    fn = getattr(library, "get_max_material_id", None) if library else None
    if fn is None:
        return "unknown (no get_max_material_id)"
    try:
        result = fn(mesh)
    except Exception as e:
        return "unknown ({0})".format(e)
    # GetMaxMaterialID RETURNS the id and has bHasMaterialIDs as an out-parameter, so
    # Python hands back ( max_id, has_ids ) — in that order.
    has_ids, max_id = True, result
    try:
        if isinstance(result, (tuple, list)) and len(result) == 2:
            max_id, has_ids = result[0], result[1]
    except Exception:
        pass
    if not has_ids:
        return "no material IDs"
    return "max material ID {0} -> {1} slot(s)".format(max_id, int(max_id) + 1)


def _uvs(mesh, name):
    """Box-project UVs onto every set, before normals and tangents.

    An empty GeometryScriptMeshSelection means the whole mesh, which is the convention
    every selection-taking Geometry Script function uses."""
    library = getattr(unreal, "GeometryScript_UVs", None)
    if library is None:
        _note("unreal.GeometryScript_UVs is not available, so the meshes have no usable "
              "UVs and any textured material will render as a smear.")
        return mesh

    fn = getattr(library, "set_mesh_u_vs_from_box_projection", None)
    if fn is None:
        _note("GeometryScript_UVs has no set_mesh_u_vs_from_box_projection; UVs not built.")
        return mesh

    setter = getattr(library, "set_num_uv_sets", None)
    if setter is not None:
        try:
            result = setter(mesh, UV_SETS)
            if result is not None:
                mesh = result
        except Exception as e:
            _info(name + " uv sets", "set_num_uv_sets failed: {0}".format(e))

    try:
        selection = unreal.GeometryScriptMeshSelection()
    except Exception:
        selection = None

    box = _projection_box(mesh, name)
    for index in range(UV_SETS):
        try:
            # min_island_tri_count: Epic's default of 2 "filters small UV islands,
            # excluding those below the triangle threshold from processing" — so every
            # ONE-TRIANGLE island would be skipped and keep whatever UVs it had, which
            # after a boolean is (0,0). A plain box has no such islands; a mesh cut
            # thirty times is full of them, and they would sample a different corner of
            # vanilla's atlas from the rest of the object. UV_MIN_ISLAND_TRIS = 1
            # excludes nothing.
            result = fn(mesh, index, box, selection,
                        min_island_tri_count=UV_MIN_ISLAND_TRIS)
            if result is not None:
                mesh = result
        except Exception as e:
            _info("{0} uv set {1}".format(name, index), "failed: {0}".format(e))
            return mesh

    if UV_CELL is not None:
        mesh = _place_in_cell(mesh, name, selection)
    return mesh


def _place_in_cell(mesh, name, selection):
    """Slide the projected UVs bodily into one atlas cell (UV_CELL).

    TX2D_FactoryBase_BC is an atlas — see the cell table at UV_CELL. A box projection at
    UV_TILE gives the right SIZE — a 4 m Rail spans 0.098, a 1 m Junction 0.024 — but the
    box is centred on the world origin, so the rectangle lands on u,v = 0, which is the
    corner where four cells meet: the worst placement available, and the mesh renders as a
    patchwork.

    So: project, MEASURE where the rectangle landed, then translate it bodily into the
    chosen cell. Measuring rather than computing the offset means this does not depend on
    knowing whether the projection's UV origin is the box centre or its corner, and the
    log reports the result either way."""
    library = getattr(unreal, "GeometryScript_UVs", None)
    move = getattr(library, "translate_mesh_u_vs", None) if library else None
    if move is None:
        _note("GeometryScript_UVs has no translate_mesh_u_vs, so the UVs stay where the "
              "projection put them and will straddle atlas cells.")
        return mesh

    lo_u, hi_u, lo_v, hi_v = UV_CELL
    for index in range(UV_SETS):
        span = _uv_span_for_set(mesh, index)
        if span is None:
            _info("  " + name + " uv cell", "set {0}: span unreadable, left alone".format(index))
            continue
        min_u, max_u, min_v, max_v = span
        width, height = max_u - min_u, max_v - min_v
        room_u = (hi_u - lo_u) - 2 * UV_CELL_MARGIN
        room_v = (hi_v - lo_v) - 2 * UV_CELL_MARGIN
        if width > room_u or height > room_v:
            _note("{0}: its UV rectangle is {1:.4f} x {2:.4f} and the atlas cell only has "
                  "{3:.4f} x {4:.4f} — it will cross a cell boundary. Raise UV_TILE or "
                  "choose a taller cell.".format(name, width, height, room_u, room_v))
        # Centre it in the cell, which leaves the most room on every side.
        target_u = lo_u + ((hi_u - lo_u) - width) * 0.5
        target_v = lo_v + ((hi_v - lo_v) - height) * 0.5
        delta_u, delta_v = target_u - min_u, target_v - min_v
        try:
            result = move(mesh, index, unreal.Vector2D(delta_u, delta_v), selection)
            if result is not None:
                mesh = result
        except Exception as e:
            _info("  " + name + " uv cell", "translate failed: {0}".format(e))
            continue
        after = _uv_span_for_set(mesh, index)
        if after is None:
            _info("  " + name + " uv cell",
                  "set {0}: moved by ({1:+.4f}, {2:+.4f}); result unreadable".format(
                      index, delta_u, delta_v))
            continue
        a_min_u, a_max_u, a_min_v, a_max_v = after
        inside = (a_min_u >= lo_u - 1e-4 and a_max_u <= hi_u + 1e-4 and
                  a_min_v >= lo_v - 1e-4 and a_max_v <= hi_v + 1e-4)
        _info("  " + name + " uv cell",
              "set {0}: {1:.4f}..{2:.4f} x {3:.4f}..{4:.4f} -> {5:.4f}..{6:.4f} x "
              "{7:.4f}..{8:.4f}  {9}".format(
                  index, min_u, max_u, min_v, max_v,
                  a_min_u, a_max_u, a_min_v, a_max_v,
                  "INSIDE " + UV_CELL_NAME if inside else
                  "STILL CROSSES the cell boundary"))
    return mesh


def _projection_box(mesh, name):
    """The transform the box projection projects from: a fixed box of UV_TILE uu at the
    origin, so texel density is the same on every mesh regardless of its size."""
    return unreal.Transform(unreal.Vector(0.0, 0.0, 0.0),
                            unreal.Rotator(0.0, 0.0, 0.0),
                            unreal.Vector(UV_TILE, UV_TILE, UV_TILE))


def _finish(mesh, name):
    """UVs, hard-edged normals and real tangents. Without this the editor reports
    'has some nearly zero tangents ... bi-normals', because the boolean cuts leave new
    faces with no tangent data; per-face normals are also what a boxy, chamfered shape
    wants. set_per_face_normals takes no options struct, which is why it is preferred
    over recompute_normals here."""
    _report_shape(mesh, name)
    # Before the normals are baked, not after: set_per_face_normals writes down
    # whatever the winding says, so an inside-out mesh has to be turned first or the
    # wrong answer is made permanent.
    mesh = _fix_orientation(mesh, name)
    mesh = _uvs(mesh, name)
    span = _uv_span(mesh)
    if span:
        _info("  " + name + " uvs", span)

    library = getattr(unreal, "GeometryScript_Normals", None)
    if library is None:
        _note("unreal.GeometryScript_Normals is not available, so normals and "
              "tangents are whatever the booleans left behind — expect odd "
              "highlights on cut faces.")
        return mesh

    fn = getattr(library, "set_per_face_normals", None)
    if fn is not None:
        try:
            mesh = fn(mesh)
        except Exception as e:
            _info(name + " normals", "set_per_face_normals failed: {0}".format(e))

    fn = getattr(library, "compute_tangents", None)
    if fn is not None:
        try:
            mesh = fn(mesh, unreal.GeometryScriptTangentsOptions())
        except Exception as e:
            _info(name + " tangents", "compute_tangents failed: {0}".format(e))

    summary = "{0}; {1}".format(_material_id_summary(mesh), _slot_census(mesh, name))
    _material_ids[name] = summary
    _info(name + " slots", summary)
    return mesh


# ---------------------------------------------------------------------------
# The meshes
# ---------------------------------------------------------------------------

def make_rail_body():
    """0.5 x 0.5 m profile, authored 4 m along +Z with its base at the origin,
    because that is the frame the vanilla beam stretch scales (Appendix B.2).

    The groove is on all four faces and runs the WHOLE length. It stops short of the
    Rail's ends so it does not run into the terminal indicators — but not by being
    authored that way. This mesh is stretched, so a 50 cm gap authored here would be
    50 cm on a 4 m Rail and 5 m on a 40 m one. The break is made instead by the
    terminal piece, which is solid, the same cross-section, and occupies the last
    35 cm at each end."""
    mesh = _new_mesh()
    _box(mesh, RAIL_PROFILE, RAIL_PROFILE, RAIL_BODY_LENGTH,
         at=_xf(0.0, 0.0, RAIL_BODY_LENGTH * 0.5))
    mesh = _tag(mesh, MATERIAL_ID_BODY)

    cue = []
    half = RAIL_PROFILE * 0.5
    for x, y in ((half, 0.0), (-half, 0.0), (0.0, half), (0.0, -half)):
        size = (GROOVE_DEPTH * 2.0 if x else GROOVE_WIDTH,
                GROOVE_WIDTH if x else GROOVE_DEPTH * 2.0,
                RAIL_BODY_LENGTH * 1.2)
        centre = (x, y, RAIL_BODY_LENGTH * 0.5)
        tool = _new_mesh()
        _box(tool, size[0], size[1], size[2], at=_xf(*centre))
        # The groove IS the powered cue (A.1). The same numbers describe the cut and
        # the volume that decides which triangles glow, so the two cannot disagree —
        # except along Z, where the CUTTER deliberately overshoots both ends and the
        # VOLUME must not, or it would claim the end caps' triangles as well.
        cue.append(_vol_box(centre[0], centre[1], RAIL_BODY_LENGTH * 0.5,
                            size[0], size[1], RAIL_BODY_LENGTH, tool=tool))
        mesh = _subtract(mesh, tool)

    mesh = _tag(mesh, MATERIAL_ID_BODY)

    # THE ACCENT, and the reason it is safe on the one mesh that gets stretched. The four
    # cuts run ALONG the length, so their width lives entirely in the cross-section and a
    # 900 cm Rail wears the same 2 cm strip as a 400 cm one. A chamfer on the END faces
    # would not survive; there is none, and the terminal collars cover the ends anyway.
    mesh, accent = _chamfer(mesh, RAIL_PROFILE, 0.0, RAIL_BODY_LENGTH)

    mesh = _tag(mesh, MATERIAL_ID_BODY)
    mesh = _mark_cue(mesh, cue + accent, "RailBody")
    mesh = _finish(mesh, "RailBody")
    return _save(mesh, "SM_ACPR_RailBody")


def _rail_terminal_parts():
    """The Rail's terminal piece, authored along +Z with z = 0 AT THE RAIL'S END, so
    it is placed by putting its origin on the terminal plane. Separate from the body
    because the body stretches and these details must not.

    A slightly recessed end face so a Cap is hit first when aiming at it, no other
    detailing, and the body's groove continued across it on all four faces. That segment
    IS the terminal-state light: same width and depth as the body's groove, so the lit
    line runs unbroken through the Rail's end, and the hue over these last 35 cm is what
    says open, coupled, powered or capped.

    RAIL_TERMINAL_SETBACK, when non-zero, sinks the ring of face around the pocket, so
    the collar's solid starts at z0 rather than at the plane and the Cap's face is alone
    up there."""
    profile = RAIL_PROFILE + RAIL_TERMINAL_OVERSIZE * 2.0

    # The solid runs z0 .. z1, both measured INWARD from the terminal plane at z = 0.
    # A setback and an oversize are the same dial turned opposite ways, so an oversize
    # eats the setback here rather than silently putting the collar out past the metre.
    z0 = RAIL_TERMINAL_SETBACK - RAIL_TERMINAL_OVERSIZE
    z1 = RAIL_TERMINAL_LENGTH
    length = z1 - z0

    mesh = _new_mesh()
    _box(mesh, profile, profile, length, at=_xf(0.0, 0.0, ( z0 + z1 ) * 0.5))
    mesh = _tag(mesh, MATERIAL_ID_BODY)

    # The pocket, cut from the ring inward. Its floor is at z0 + RECESS, and the Cap is
    # CAP_DEPTH_RAIL = RECESS + SETBACK deep, so it fills the pocket AND the setback and its
    # outer face lands exactly on the plane. §11 allows up to 0.05 m of recess, and the
    # terminal PLANE does not move.
    tool = _new_mesh()
    _box(tool, RAIL_TERMINAL_RECESS_SIZE, RAIL_TERMINAL_RECESS_SIZE,
         RAIL_TERMINAL_RECESS * 2.0, at=_xf(0.0, 0.0, z0))
    # The pocket's corners match the Cap's: the Rail Cap is chamfered at ACCENT_WIDTH like
    # the Junction Cap, so the cutter gets the same four edge cuts.
    if ACCENT_WIDTH > 0.0:
        tool, _unused = _chamfer(tool, RAIL_TERMINAL_RECESS_SIZE,
                                 z0 - RAIL_TERMINAL_RECESS * 2.0, z0 + RAIL_TERMINAL_RECESS * 2.0,
                                 material_id=MATERIAL_ID_BODY)
    tool = _tag(tool, MATERIAL_ID_BODY)
    mesh = _subtract(mesh, tool)

    # The pocket FLOOR's centre square is the power indicator (MATERIAL_ID_POWER): a
    # sub-pocket POWER_FLOOR_DEPTH deep and POWER_FLOOR_FRACTION of the floor across, whose
    # own floor a zero-thick volume catches by centroid — and none of its walls.
    floor_z = z0 + RAIL_TERMINAL_RECESS
    ind = RAIL_TERMINAL_RECESS_SIZE * POWER_FLOOR_FRACTION
    tool = _new_mesh()
    _box(tool, ind, ind, POWER_FLOOR_DEPTH * 2.0, at=_xf(0.0, 0.0, floor_z))
    tool = _tag(tool, MATERIAL_ID_BODY)
    mesh = _subtract(mesh, tool)
    power = [_vol_box(0.0, 0.0, floor_z + POWER_FLOOR_DEPTH, ind, ind, 0.0,
                      material_id=MATERIAL_ID_POWER)]

    # The state light: the body's groove, continued across the terminal piece on all
    # four faces. Same width and depth, so the line is unbroken.
    cue = []
    half = profile * 0.5
    for x, y in ((half, 0.0), (-half, 0.0), (0.0, half), (0.0, -half)):
        size = (TERMINAL_GROOVE_DEPTH * 2.0 if x else TERMINAL_GROOVE_WIDTH,
                TERMINAL_GROOVE_WIDTH if x else TERMINAL_GROOVE_DEPTH * 2.0,
                length * 1.4)
        centre = (x, y, ( z0 + z1 ) * 0.5)
        tool = _new_mesh()
        _box(tool, size[0], size[1], size[2], at=_xf(*centre))
        # Same as the body: the cutter overshoots in Z, the volume stops at the
        # terminal piece's own extent, which runs z0 .. z1.
        solid_low = z0
        solid_high = z1
        cue.append(_vol_box(centre[0], centre[1], (solid_low + solid_high) * 0.5,
                            size[0], size[1], solid_high - solid_low, tool=tool))
        mesh = _subtract(mesh, tool)

    # The same cut as the body, so the accent line runs unbroken through the joint — the
    # groove's argument, applied to the strip beside it. The collar is never stretched, so
    # this 2 cm is 2 cm at any Rail length — the body's is too, for the different reason
    # that its cuts run along the stretch axis.
    mesh, accent = _chamfer(mesh, profile, z0, z1)

    mesh = _tag(mesh, MATERIAL_ID_BODY)

    # The collision source, stopping at the pocket floor — inward only.
    #
    # ALIGNED_BOXES gives this mesh exactly one box, and one box cannot have a hole in it:
    # fitted to the render mesh it fills the pocket straight back in, so anything that queries
    # SIMPLE shapes meets the collar where the Cap is. Generating it from a shorter box instead
    # puts the collar's simple face at the pocket floor, behind the Cap under every query type
    # rather than only under line traces.
    #
    # What it costs: the pocket's depth of an UNCAPPED Rail end is not there for a sweep. That is a
    # 1.59 cm ring of rim, it is inside a Junction socket whenever the end is connected, and line
    # traces still use the real triangles (COLLISION_TRACE_FLAG), so aiming is unaffected. Every
    # millimetre of this moves collision INWARD, which is the direction that can never cost a
    # placement — collision is footprint, see COLLISION_METHOD.
    floor = z0 + RAIL_TERMINAL_RECESS
    collision = _new_mesh()
    _box(collision, profile, profile, z1 - floor, at=_xf(0.0, 0.0, ( floor + z1 ) * 0.5))

    return mesh, cue + accent + power, collision


def make_rail_terminal():
    """The plain collar. See _rail_terminal_parts for the shape."""
    mesh, volumes, collision = _rail_terminal_parts()
    mesh = _mark_cue(mesh, volumes, "RailTerminal")
    mesh = _finish(mesh, "RailTerminal")
    return _save(mesh, "SM_ACPR_RailTerminal", collision)


def make_rail_terminal_bridged():
    """The collar and the bridge slice as ONE mesh.

    A separate slice component would mean four collar-sized components per Rail, two of them
    hidden almost always, each copied onto every hologram and every blueprint duplicate. So
    the Rail swaps its collar's mesh to this one while the terminal is coupled to a Junction
    (AACPRRail::UpdateCollarMesh), and the component count is two.

    Geometry: exactly the collar (_rail_terminal_parts) plus exactly the slice
    (_rail_bridge_parts), appended in place — both are authored on the terminal plane at z = 0,
    the collar inward (+Z), the slice outward (-Z), so no transform. No boolean: the two never
    overlap (the collar starts at its setback, the slice ends at the plane), and the cue
    volumes of both are marked on the combined mesh so the groove reads as one lit line.
    Collision is the collar's, unchanged — the slice has none, because collision is
    footprint and the slice lies in the partner's cell."""
    mesh, volumes, collision = _rail_terminal_parts()
    slice_mesh, slice_volumes = _rail_bridge_parts()
    try:
        unreal.GeometryScript_MeshEdits.append_mesh(mesh, slice_mesh, _xf())
    except Exception as e:
        _note("RailTerminalBridged: append_mesh failed ({0}); the bridged collar is the plain "
              "collar and the seat gap at Junctions stays open.".format(e))
    mesh = _mark_cue(mesh, volumes + slice_volumes, "RailTerminalBridged")
    mesh = _finish(mesh, "RailTerminalBridged")
    return _save(mesh, "SM_ACPR_RailTerminalBridged", collision)


def make_junction():
    """§11's 1 m cube, chamfered on its twelve edges. Each face gets a shallow Cap seat,
    a 50 cm square beam socket in the seat's floor — the Rail's own profile, so the face
    reads as 'a beam goes here' — a power indicator in the socket's floor, and four
    indicator strips that continue a coupled Rail's grooves. Each face's cue is its own
    material slot, so the face answers for itself.

    The occlusion disc is a logical quantity; it does not have to be drawn."""
    _log("=== Junction build stages ===")
    mesh = _new_mesh()
    _box(mesh, JUNCTION_SIZE, JUNCTION_SIZE, JUNCTION_SIZE)
    count = _stage(mesh, "cube", -1)
    half = JUNCTION_SIZE * 0.5

    # The twelve edges, as explicit corner cuts. apply_mesh_polygroup_bevel bevels every
    # polygroup BOUNDARY, and this mesh is about to acquire six seats, six sockets and four
    # strips per face — every one of them its own polygroup. Bevelling after all that
    # would put the circuit colour on every internal edge in the Junction; bevelling
    # before it produces a chamfer that nothing can then find analytically. Three axes of
    # corner cuts give exactly twelve edges and hand back the volumes that mark them.
    #
    # FIRST, before the sockets and strips, because the cutters are full-length boxes and
    # would otherwise reach into the recesses they pass. The faces they leave survive
    # every later boolean; the volumes find them at the end regardless of order.
    accent = []
    if ACCENT_WIDTH > 0.0:
        for axis in range(3):
            mesh, cuts = _chamfer(mesh, JUNCTION_SIZE, -half, half, axis=axis)
            accent.extend(cuts)
        count = _stage(mesh, "twelve chamfered edges", count)

    # After the cuts, not before: they make new triangles of their own.
    mesh = _tag(mesh, MATERIAL_ID_BODY)

    cue = []

    # THE SIX CAP SEATS, CUT FIRST so everything below lands in their floors — the strips and
    # the beam socket measure from the original face and end up 2 cm further in. A capped face
    # is then plated over smooth ("a smooth junction face indicates capped"); an uncapped one is
    # a shallow recessed panel with its indicators in the bottom.
    if JUNCTION_CAP_SEAT_DEPTH > 0.0:
        for axis in range(3):
            for sign in (1.0, -1.0):
                where = [0.0, 0.0, 0.0]
                where[axis] = sign * half
                dims = [JUNCTION_CAP_SEAT_SIZE] * 3
                dims[axis] = JUNCTION_CAP_SEAT_DEPTH * 2.0
                tool = _new_mesh()
                _box(tool, dims[0], dims[1], dims[2], at=_xf(*where))

                # THE SEAT'S CORNERS MATCH THE CAP'S. The Cap is chamfered at ACCENT_WIDTH so
                # its outline is an octagon; a square seat would leave four empty triangles at
                # the corners. Same as the beam socket below: chamfer the CUTTER's four edges
                # along the face normal at the same width, so the seat is the same octagon and
                # the Cap fills it edge to edge.
                if ACCENT_WIDTH > 0.0:
                    tool, _unused = _chamfer(tool, JUNCTION_CAP_SEAT_SIZE,
                                             where[axis] - dims[axis],
                                             where[axis] + dims[axis],
                                             material_id=MATERIAL_ID_BODY, axis=axis)

                tool = _tag(tool, MATERIAL_ID_BODY)
                mesh = _subtract(mesh, tool)
        count = _stage(mesh, "six cap seats", count)

    # Six face indents. Shape, not cue — a beam socket, painted like the body.
    for axis in range(3):
        for sign in (1.0, -1.0):
            where = [0.0, 0.0, 0.0]
            where[axis] = sign * half
            dims = [JUNCTION_INDENT] * 3
            dims[axis] = JUNCTION_INDENT_DEPTH * 2.0
            tool = _new_mesh()
            _box(tool, dims[0], dims[1], dims[2], at=_xf(*where))

            # THE SOCKET IS CHAMFERED TO MATCH THE RAIL IT ACCEPTS, by chamfering the CUTTER
            # rather than the Junction. A chamfered Rail's cross-section is an octagon
            # inscribed in the 50 square; a square socket leaves four empty corners around it.
            # Cutting the tool's corners off leaves material in the socket's corners — exactly
            # where the Rail has none — so the two silhouettes agree.
            #
            # The socket walls stay BODY: they are covered the moment a Rail is in them, and an
            # accent nobody can see is an accent that only shows up when something is missing.
            if ACCENT_WIDTH > 0.0:
                tool, _unused = _chamfer(tool, JUNCTION_INDENT,
                                         where[axis] - dims[axis],
                                         where[axis] + dims[axis],
                                         material_id=MATERIAL_ID_BODY, axis=axis)

            tool = _tag(tool, MATERIAL_ID_BODY)
            mesh = _subtract(mesh, tool)
    count = _stage(mesh, "six 50 cm face sockets", count)

    # Each socket's FLOOR is a power indicator (MATERIAL_ID_POWER + face, see _power_slot). The
    # socket cutter straddles the ORIGINAL face (sign * half) by INDENT_DEPTH either way, so its
    # floor is half - INDENT_DEPTH — 43 — measured from the cube's face, not from the seat's; a
    # zero-thick volume there catches the floor and no socket wall.
    socket_floor = half - JUNCTION_INDENT_DEPTH
    ind = JUNCTION_INDENT * POWER_FLOOR_FRACTION
    for axis in range(3):
        for sign in (1.0, -1.0):
            where = [0.0, 0.0, 0.0]
            where[axis] = sign * socket_floor
            dims = [ind] * 3
            dims[axis] = POWER_FLOOR_DEPTH * 2.0
            tool = _new_mesh()
            _box(tool, dims[0], dims[1], dims[2], at=_xf(*where))
            tool = _tag(tool, MATERIAL_ID_BODY)
            mesh = _subtract(mesh, tool)
            where[axis] = sign * (socket_floor - POWER_FLOOR_DEPTH)
            dims[axis] = 0.0
            cue.append(_vol_box(where[0], where[1], where[2], dims[0], dims[1], dims[2],
                                material_id=_power_slot(axis, sign)))
    count = _stage(mesh, "six socket-floor indicators", count)

    # The face cue: four strips per face continuing a coupled Rail's grooves.
    #
    # THE STRIPS ARE CUT INTO THE SEAT FLOOR, NOT THE ORIGINAL FACE. The seats above have
    # already cut the face 2 cm inward. A cutter deep enough to reach does not care: the socket
    # straddles 14 cm and gets there. A strip cutter straddling 0.2 cm at the ORIGINAL face plane
    # would sit entirely in the air the seat left behind, touch nothing, and the boolean would
    # report success.
    #
    # `seat_face` is where the face actually is once the seats are cut — the seat floor, or the
    # original face when there is no seat.
    seat_face = half - ( JUNCTION_CAP_SEAT_DEPTH if JUNCTION_CAP_SEAT_DEPTH > 0.0 else 0.0 )
    for axis in range(3):
        for sign in (1.0, -1.0):
            for radial in range(3):
                if radial == axis:
                    continue
                third = 3 - axis - radial
                for rsign in (1.0, -1.0):
                    dims = [0.0, 0.0, 0.0]
                    where = [0.0, 0.0, 0.0]
                    dims[axis] = JUNCTION_STRIP_DEPTH * 2.0
                    where[axis] = sign * seat_face
                    dims[radial] = JUNCTION_STRIP_FAR - JUNCTION_STRIP_NEAR
                    where[radial] = rsign * (JUNCTION_STRIP_NEAR
                                             + JUNCTION_STRIP_FAR) * 0.5
                    dims[third] = JUNCTION_STRIP_WIDTH
                    tool = _new_mesh()
                    _box(tool, dims[0], dims[1], dims[2], at=_xf(*where))
                    cue.append(_vol_box(where[0], where[1], where[2],
                                        dims[0], dims[1], dims[2], tool=tool,
                                        material_id=_face_slot(axis, sign)))
                    mesh = _subtract(mesh, tool)
    _stage(mesh, "twenty-four face strips", count)

    mesh = _tag(mesh, MATERIAL_ID_BODY)
    mesh = _mark_cue(mesh, cue + accent, "Junction")
    mesh = _finish(mesh, "Junction")
    return _save(mesh, "SM_ACPR_Junction")


def make_outlet():
    """A.2's "compact ribbed cylindrical body": a capped cylinder with three ring
    grooves cut into it, authored along +Z with its base on the mounting plane."""
    mesh = _new_mesh()
    _cylinder(mesh, OUTLET_RADIUS, OUTLET_HEIGHT, steps=OUTLET_RADIAL_STEPS)
    # The mounting pad is a separate mesh (_make_outlet_base). The cylinder's base sits on
    # the pad's top, which is the mounting plane at z = 0.
    mesh = _tag(mesh, MATERIAL_ID_BODY)

    cue = []
    # Ribs: a slightly oversized disc with a smaller one removed leaves a ring, and
    # subtracting that from the body cuts a groove around it. These rings are the
    # Outlet's powered cue, so they go in the cue slot.
    for i in range(OUTLET_RIB_COUNT):
        z = OUTLET_HEIGHT * (i + 1.0) / (OUTLET_RIB_COUNT + 1.0) - OUTLET_RIB_HEIGHT * 0.5
        outer = _new_mesh()
        _cylinder(outer, OUTLET_RADIUS + 0.02 * M, OUTLET_RIB_HEIGHT, at=_xf(0.0, 0.0, z),
                  steps=OUTLET_RADIAL_STEPS)
        inner = _new_mesh()
        _cylinder(inner, OUTLET_RADIUS - OUTLET_RIB_DEPTH, OUTLET_RIB_HEIGHT * 3.0,
                  at=_xf(0.0, 0.0, z - OUTLET_RIB_HEIGHT), steps=OUTLET_RADIAL_STEPS)
        rib = _subtract(outer, inner)
        # The rib groove is a band about the Z axis: between the two cylinders'
        # radii, over the outer cylinder's height. The base-origin cylinder puts
        # z at its bottom, so the band runs z .. z + OUTLET_RIB_HEIGHT.
        cue.append(_vol_band(z, z + OUTLET_RIB_HEIGHT,
                             OUTLET_RADIUS - OUTLET_RIB_DEPTH,
                             OUTLET_RADIUS + 0.02 * M, tool=rib))
        mesh = _subtract(mesh, rib)

    # THE OUTLET'S ACCENT IS ITS CAP RING: a 45-degree conical chamfer ACCENT_WIDTH wide on
    # the cylinder's top edge.
    #
    # The frustum runs R + m at z0 - m, narrowing to R - w - m at the top + m, so the
    # 45-degree line passes through (R, z0) and (R - w, top) and the tool — the fat disc minus
    # that frustum — is exactly the corner outside the rim. (The other way round it would
    # subtract the wedge UNDER the rim and leave a full-radius lip with an undercut.) 24 radial
    # steps on both, so the cone's facets meet the cylinder's edge to edge. append_cone is
    # Epic's (GeometryScript_Primitives.append_cone, base_radius / top_radius / height, base at
    # the origin); if this build lacks it the Outlet keeps a square top and the log says so,
    # rather than a silent skip.
    accent = []
    if ACCENT_WIDTH > 0.0:
        cone_fn = getattr(unreal.GeometryScript_Primitives, "append_cone", None)
        if cone_fn is None:
            _note("Outlet: GeometryScript_Primitives.append_cone is not in this build, so the "
                  "cap ring is not chamfered and the Outlet has NO accent.")
        else:
            w = ACCENT_WIDTH
            m = 0.01 * M
            z0 = OUTLET_HEIGHT - w
            disc = _new_mesh()
            _cylinder(disc, OUTLET_RADIUS + m, w + m, at=_xf(0.0, 0.0, z0),
                      steps=OUTLET_RADIAL_STEPS)
            frustum = _new_mesh()
            kwargs = {"base_radius": OUTLET_RADIUS + m, "top_radius": OUTLET_RADIUS - w - m,
                      "height": w + 2.0 * m, "radial_steps": OUTLET_RADIAL_STEPS, "capped": True}
            if _ORIGIN_BASE is not None:
                kwargs["origin"] = _ORIGIN_BASE
            cone_fn(frustum, unreal.GeometryScriptPrimitiveOptions(), _xf(0.0, 0.0, z0 - m),
                    **kwargs)
            tool = _subtract(disc, frustum)
            before = _triangle_count(mesh)
            mesh = _subtract(mesh, tool)
            _info("  Outlet cap chamfer", "{0} -> {1} triangles".format(before, _triangle_count(mesh)))
            # The chamfer surface: radius R at z0 falling to R - w at the top, picked out by its
            # 45-degree slope (see _vol_cone). The boolean fans it into more than the 48 triangles
            # two-per-step would give, so the census counts slivers too; what matters is that
            # nothing flat (top cap) or vertical (wall) is in it.
            accent.append(_vol_cone(z0, OUTLET_HEIGHT, OUTLET_RADIUS, OUTLET_RADIUS - w,
                                    material_id=MATERIAL_ID_ACCENT))

    mesh = _tag(mesh, MATERIAL_ID_BODY)
    mesh = _mark_cue(mesh, cue + accent, "Outlet")
    mesh = _finish(mesh, "Outlet")
    return _save(mesh, "SM_ACPR_Outlet")


def _make_outlet_base(size, name, asset):
    """The Outlet's pad, authored with its TOP at z = 0 and its thickness reaching -Z, so
    an origin on the mounting plane puts the top flush with that plane and a negative
    standoff sinks it. Chamfered on its four vertical edges at ACCENT_WIDTH so it matches
    the chamfered pocket, socket or Rail flat it sits in."""
    mesh = _new_mesh()
    _box(mesh, size, size, OUTLET_BASE_HEIGHT, at=_xf(0.0, 0.0, -OUTLET_BASE_HEIGHT * 0.5))
    mesh = _tag(mesh, MATERIAL_ID_BODY)
    # NO ACCENT ON THE PAD: an accented rim shines through ever so slightly in a Junction
    # seat and breaks the power indicator strip on a Rail body. The pad is body-coloured on
    # every face; the Outlet's accent is the chamfered ring at the top of its cylinder
    # (make_outlet). The chamfer stays, for the fit against the chamfered seats, and is
    # body too.
    if ACCENT_WIDTH > 0.0:
        mesh, _unused = _chamfer(mesh, size, -OUTLET_BASE_HEIGHT, 0.0,
                                 material_id=MATERIAL_ID_BODY)
    mesh = _tag(mesh, MATERIAL_ID_BODY)
    mesh = _mark_cue(mesh, [], name)
    mesh = _finish(mesh, name)
    return _save(mesh, asset)


def make_outlet_base_body():
    return _make_outlet_base(RAIL_FLAT, "OutletBaseBody", "SM_ACPR_OutletBaseBody")


def make_outlet_base_rail():
    return _make_outlet_base(RAIL_TERMINAL_RECESS_SIZE, "OutletBaseRail", "SM_ACPR_OutletBaseRail")


def make_outlet_base_junction():
    return _make_outlet_base(JUNCTION_INDENT, "OutletBaseJunction", "SM_ACPR_OutletBaseJunction")


def _rail_bridge_parts():
    """The 2 cm slice that closes the seat gap at a Junction.

    A coupled Rail's end sits on the terminal plane; the Junction's socket opens in its cap
    seat, 2 cm behind that plane. Without the slice, four power strips that are meant to
    run continuously from the Rail into the Junction would stop for 2 cm at every joint.
    This is the collar's profile, grooves and chamfers, JUNCTION_CAP_SEAT_DEPTH long,
    authored from z = 0 to z = -depth so that, placed like the collar, it reaches OUTWARD
    past the terminal plane into the Junction's seat. It has no pocket and no collision,
    and the C++ shows it only while that terminal is coupled to a Junction — the seat it
    fills is then already inside the partner's cell, which keeps collision-is-footprint
    intact."""
    profile = RAIL_PROFILE
    depth = JUNCTION_CAP_SEAT_DEPTH
    z0, z1 = -depth, 0.0
    mesh = _new_mesh()
    _box(mesh, profile, profile, depth, at=_xf(0.0, 0.0, (z0 + z1) * 0.5))
    mesh = _tag(mesh, MATERIAL_ID_BODY)

    cue = []
    half = profile * 0.5
    # THE GROOVE RAMPS OUT ONTO THE SEAT FLOOR.
    #
    # The seat floor at the slice's end IS the Junction's 50 cm socket opening, and the
    # Junction's strip only begins at r = 25. A groove running the full 2 cm at full depth
    # would open its floor at r = 23 into the socket void — a dark 2 x 2 cm hole in the line.
    # An end wall closing it would leave a 2 mm sliver of profile face at r = 25, lit because
    # it lies inside the cue volume — a bright hairline at the foot of the bridge.
    #
    # So the groove has no end wall and no floor at the seat end: its floor RISES at 45
    # degrees over the bridge's length, from full depth at the terminal plane (continuous with
    # the collar's groove) to the profile face exactly at the seat floor, where the Junction's
    # strip begins. One lit surface — collar groove floor, ramp, strip — with no perpendicular
    # sliver anywhere for a slit to come from. The cutter is a box turned 45 degrees whose
    # lower face lies on the ramp plane; at 45 degrees the two in-plane axes are the same set
    # whichever sign the rotation takes, which is why this does not have to know
    # unreal.Rotator's sign convention. It does rely on the ramp being 45 degrees, i.e. on the
    # groove depth equalling the seat depth (both 2 cm), and says so if that ever changes.
    if abs(TERMINAL_GROOVE_DEPTH - depth) > 1e-6:
        _note("RailBridge: TERMINAL_GROOVE_DEPTH ({0:.1f}) != JUNCTION_CAP_SEAT_DEPTH ({1:.1f}), "
              "so the 45-degree ramp cutter does not describe the groove end; the strips "
              "will not meet the Junction's cleanly.".format(TERMINAL_GROOVE_DEPTH, depth))
    ramp_len = depth * 4.0
    for ox, oy in ((1.0, 0.0), (-1.0, 0.0), (0.0, 1.0), (0.0, -1.0)):
        # A point on the ramp plane, halfway along it, and the plane's outward-and-up normal.
        r_mid = half - TERMINAL_GROOVE_DEPTH * 0.5
        z_mid = (z0 + z1) * 0.5
        px, py = ox * r_mid, oy * r_mid
        nx, ny, nz = ox / math.sqrt(2.0), oy / math.sqrt(2.0), 1.0 / math.sqrt(2.0)
        cx, cy, cz = (px + nx * ramp_len * 0.5, py + ny * ramp_len * 0.5,
                      z_mid + nz * ramp_len * 0.5)
        # unreal.Rotator is ( roll, pitch, yaw ) here — see _xf_yaw. A groove on a +/-X face
        # ramps in the XZ plane, which is a rotation about Y (pitch); on a +/-Y face, about X
        # (roll). The transverse size stays the groove width because that axis is the one
        # rotated about.
        if ox:
            rotator = unreal.Rotator(0.0, 45.0, 0.0)
            size = (ramp_len, TERMINAL_GROOVE_WIDTH, ramp_len)
        else:
            rotator = unreal.Rotator(45.0, 0.0, 0.0)
            size = (TERMINAL_GROOVE_WIDTH, ramp_len, ramp_len)
        tool = _new_mesh()
        _box(tool, size[0], size[1], size[2],
             at=unreal.Transform(unreal.Vector(cx, cy, cz), rotator, unreal.Vector(1.0, 1.0, 1.0)))
        # The cue volume is the groove's own slab — floor, walls and ramp all lie inside it.
        vx, vy = ox * half, oy * half
        vol_size = (TERMINAL_GROOVE_DEPTH * 2.0 if ox else TERMINAL_GROOVE_WIDTH,
                    TERMINAL_GROOVE_WIDTH if ox else TERMINAL_GROOVE_DEPTH * 2.0)
        cue.append(_vol_box(vx, vy, z_mid, vol_size[0], vol_size[1], z1 - z0, tool=tool))
        before = _triangle_count(mesh)
        mesh = _subtract(mesh, tool)
        _info("  RailBridge ramp ({0:+.0f},{1:+.0f})".format(ox, oy),
              "{0} -> {1} triangles".format(before, _triangle_count(mesh)))

    mesh, accent = _chamfer(mesh, profile, z0, z1)
    mesh = _tag(mesh, MATERIAL_ID_BODY)
    return mesh, cue + accent


def _make_cap(face, depth, rim, name, asset):
    """§9: the outer face sits AT the terminal plane and the mesh extends inward, so
    a Cap changes no terminal position and two can sit back to back.

    Authored at its host's true face width, so its chamfer is the same 2 cm as the host
    it sits against — see CAP_FACE_RAIL / CAP_FACE_JUNCTION for why one mesh could not."""
    mesh = _new_mesh()
    _box(mesh, face, face, depth, at=_xf(0.0, 0.0, -depth * 0.5))
    mesh = _tag(mesh, MATERIAL_ID_BODY)

    # The chamfer, so the Cap's corners stop poking out past the chamfered host.
    mesh, accent = _chamfer(mesh, face, -depth, 0.0)

    if rim and CAP_RIM and CAP_RIM_DEPTH > 0.0:
        # A square groove in the outer face, the same width as every other accent strip.
        # _frame_tool straddles the face at offset 0 with twice the depth, so it cuts
        # exactly CAP_RIM_DEPTH in — the same trick the Junction's rings use.
        outer = face - CAP_RIM_INSET * 2.0
        inner = outer - ACCENT_WIDTH * 2.0
        if inner > 0.0:
            tool = _frame_tool(CAP_RIM_DEPTH * 2.0, outer, inner, 2, 0.0)
            tool = _tag(tool, MATERIAL_ID_ACCENT)
            accent.append(_vol_frame(2, 0.0, CAP_RIM_DEPTH * 2.0, outer, inner,
                                     tool=tool, material_id=MATERIAL_ID_ACCENT))
            mesh = _subtract(mesh, tool)
        else:
            _note(name + ": CAP_RIM_INSET and ACCENT_WIDTH leave no room for a rim on a "
                         "{0:.0f} face; it is skipped.".format(face))

    # A capped terminal is OFF by definition, so the Cap has no CUE surface. It does have an
    # accent — the circuit colour is not a state.
    mesh = _tag(mesh, MATERIAL_ID_BODY)
    mesh = _mark_cue(mesh, accent, name)
    mesh = _finish(mesh, name)

    # A Cap's collision is its render mesh and nothing more: a shape that exists only in
    # collision still occupies the grid, because collision is the only thing a placement test
    # can see. The reach the Cap needs is taken out of its HOST instead (RAIL_TERMINAL_SETBACK).
    return _save(mesh, asset)


def make_cap_rail():
    return _make_cap(CAP_FACE_RAIL, CAP_DEPTH_RAIL, True, "CapRail", "SM_ACPR_CapRail")


def make_cap_junction():
    # No rim: a capped Junction face is read against the cube's own twelve chamfers.
    return _make_cap(CAP_FACE_JUNCTION, CAP_DEPTH_JUNCTION, False,
                     "CapJunction", "SM_ACPR_CapJunction")


# ---------------------------------------------------------------------------

def main():
    _log("meshes | OVERWRITE = {0} | REPORT_ONLY = {1}".format(OVERWRITE, REPORT_ONLY))

    global _ORIGIN_CENTER, _ORIGIN_BASE, _BOOLEAN_SUBTRACT, _BOOLEAN_UNION
    _ORIGIN_CENTER = _enum("GeometryScriptPrimitiveOriginMode", "CENTER", "CENTERED", "CENTRE")
    _ORIGIN_BASE = _enum("GeometryScriptPrimitiveOriginMode", "BASE", "BOTTOM")
    _BOOLEAN_SUBTRACT = _enum("GeometryScriptBooleanOperation", "SUBTRACT", "DIFFERENCE", "MINUS")
    _BOOLEAN_UNION = _enum("GeometryScriptBooleanOperation", "UNION", "ADD")

    if REPORT_ONLY:
        _log("REPORT_ONLY is set — no meshes built.")
        return

    _log("=== building ===")
    for label, fn in (("Rail body", make_rail_body),
                      ("Rail terminal", make_rail_terminal),
                      ("Rail terminal (bridged)", make_rail_terminal_bridged),
                      ("Junction", make_junction),
                      ("Outlet", make_outlet),
                      ("Outlet base (body)", make_outlet_base_body),
                      ("Outlet base (rail end)", make_outlet_base_rail),
                      ("Outlet base (junction)", make_outlet_base_junction),
                      ("Cap (Rail)", make_cap_rail),
                      ("Cap (Junction)", make_cap_junction)):
        try:
            fn()
        except Exception as e:
            _info(label + " — raised", "{0}: {1}".format(type(e).__name__, e))
            for line in traceback.format_exc().splitlines():
                _log("    " + line)
            _failed.append(label)

    _log("=== summary ===")
    _log("{0} built, {1} failed".format(len(_built), len(_failed)))

    # The step this script makes necessary and cannot do itself.
    #
    # _save DELETES the existing asset and creates a new one, so every reference to the old
    # object is nulled: Build_PowerRail's RailMesh component, the Junction's, the Outlet's, and
    # the material slots. The buildables survive it — AACPRRail and AACPRCap fall back to their
    # C++ soft paths — but the HOLOGRAMS do not, because a hologram copies the buildable CDO's
    # component and never runs BeginPlay. The symptom is a preview that has vanished while the
    # built part looks fine, and it reads exactly like a hologram bug. So it is printed here,
    # every run, in the file that causes it.
    if _built:
        _log("")
        _log("  NEXT, AND NOT OPTIONAL: run acpr_materials.py, then acpr_bind_meshes.py.")
        _log("  This pass deleted and recreated every mesh above, so every reference to them")
        _log("  is now null — material slots and the buildables' mesh components alike.")
        _log("  Save when the editor offers to save Build_PowerRail and friends on exit.")
    for name in _built:
        _log("  built    " + MESH_DIR + "/" + name)
    for name in _failed:
        _log("  FAILED   " + name)

    # Per mesh: the highest material ID and the per-slot census, so a mesh whose cue
    # surfaces landed in the wrong slot is visible here rather than in game.
    if _material_ids:
        _log("  --- material slots ---")
        for name in sorted(_material_ids):
            _log("  {0:<14} {1}".format(name, _material_ids[name]))

    for note in _notes:
        _log("  NOTE  " + note)


_ORIGIN_CENTER = None
_ORIGIN_BASE = None
_BOOLEAN_SUBTRACT = None
_BOOLEAN_UNION = None

try:
    main()
    _write_log()
except Exception:
    _log("the mesh pass stopped early:")
    for _line in traceback.format_exc().splitlines():
        _log("    " + _line)
    _write_log()
