"""
Auto-Connecting Power Rails — measure the power sockets of the game's machines (§16.5's link ends).

A packaged game cannot read mesh vertices, so the fit of the link-end tube to each machine's socket
knob is measured here, in the editor, and written as a table the mod loads at runtime:

    Config/ACPR_Sockets.json        one entry per buildable class and power connection:
                                    the knob's axis offset from the connection point (in the
                                    connection's frame), its radius and the extent of its straight
                                    wall along the connection axis (bottom, top). Classes are keyed
                                    by their full path, which another mod's same-named class does
                                    not share; the mod looks a machine up by its class and then its
                                    parent classes, so a child of a vanilla machine inherits its fit
    Scripts/Temp/acpr_sockets_log.txt
                                    how every entry was found: the components around each
                                    connection, which mesh supplied the wall, what was rejected

How a socket is measured. The blueprint's components are gathered through the SubobjectDataSubsystem
and every scene component's transform is composed up its attachment chain, so a mesh or a connection
that is attached to something other than the root is still placed where the game places it. Every
static mesh the machine renders — StaticMeshComponents, the lightweight instance list
(mInstanceDataCDO) and the meshes of child actors; skeletal meshes are listed but not read, since
their streamed render data can be absent and the copy then asserts — is read through GeometryScript,
positions and normals, and expressed in the connection's frame. The knob is a low-poly cylinder
along one axis of that frame (upright on most machines, sideways on some) or, for a separate plug
mesh, along the direction from that mesh's origin to the connection point (tilted on the Pipeline
Pump), so each frame axis and each such direction is tried. Along an axis the vertices are binned by
distance from it; every bin that forms a ring is circle-fitted (Kåsa, the radius taken as the mode
of the distances from that centre so two close walls do not average) and kept when enough vertices
lie on the wall with no wide angular gap seen from its centre (a sector count would mistake a
16-gon's sides for gaps); a ring is scored by the vertices exactly on its circle, as a cylinder's
edge loops are, and the axis with the best ring is the knob's axis. A wall whose normals point
toward the axis is concave and never the knob; among the convex walls the one with the most exact
vertices is the knob's core (every vanilla socket is an insulator stack: a core with discs on it),
the outermost within twice its radius sets the tube's radius (the discs), and the core's dense run
of edge loops is the tube's extent — the base cylinder below shares the discs' radius, so the
discs' own runs reach too far. The knob is along the connection's up unless another axis's core
beats it clearly. The machine's body lies beyond the knob's bottom, so the end with more vertices
just beyond it is the bottom. The entry is written in the connection's frame: axis, offset from
the connection point to the knob's axis, radius (the discs) and core (the knob's own cylinder),
bottom and top along the axis.

Run inside the editor (Output Log, "Cmd" -> "Python"):
    exec(open(r"<plugin>/Scripts/acpr_sockets.py").read())
It takes a few minutes: every machine mesh is copied once.
"""

import json
import math
import os
import random
import traceback

import unreal

MOD = "AutoConnectingPowerRails"
SCAN_ROOTS = ("/Game/FactoryGame/Buildable/Factory",)
OUTPUT = "Config/ACPR_Sockets.json"
MANUAL = "Config/ACPR_Sockets_Manual.json"   # hand-made corrections, same shape; fields named there replace measured ones
ASSET_FOLDER = "/" + MOD + "/Data"
ASSET_NAME = "DA_ACPR_SocketTable"   # the JSON's text as a UACPRSocketTableAsset: what the packaged mod loads
LOG_NAME = "acpr_sockets_log.txt"

# The default fit (the Constructor's knob) for a machine the table does not know.
DEFAULT_FIT = {"axis": [0.0, 0.0, 1.0], "offset": [0.0, 0.0, 0.0], "radius": 29.5, "core": 17.8, "bottom": -28.0, "top": 26.0}

NEAR_AXIS = 80.0        # vertices further than this from the connection point (per axis) are not the knob
BIN = 2.0               # radial bin width
ROW = 5.0               # height of a row when counting the wall's edge loops
MIN_RING_VERTICES = 24  # a ring needs this many vertices ...
MAX_RING_GAP = 185.0    # ... with no angular gap wider than this (degrees) seen from the axis: a half ring still fits its circle
MIN_RADIUS = 3.0        # the smallest knob the game has is ~5.5
WALL_BAND = 1.0         # a vertex this close to the fitted radius is on the straight wall
TIGHT_BAND = 0.25       # ... but a ring is scored by the vertices exactly on its circle (edge loops are)
STRONG = 0.5            # a ring with this share of the best ring's score is a real wall
MERGE_RADIUS = 3.0      # a convex wall this little outside the core is the core's own cap or band, not a disc
RUN_GAP = 1             # rows that may be empty inside one run of edge loops (loops 10 uu apart are one wall)
PREFER = 1.5            # an axis lower in the order (up; a nearby mesh's direction; across) needs this many times the score
MAX_CORE_GAP = 60.0     # a whole cylinder has no gap wider than this (a 12-gon has 30 deg gaps); only named in the log
HALF_CORE_GAP = 185.0   # the core may be a half cylinder: a knob modelled as one mirrored half (Truck Station)
MIN_CONVEXITY = 0.2     # ... whose normals point outward (a wall's do; an edge loop between faces does not)
DISC_OFFSET = 0.5       # a disc's centre lies within this fraction of its radius of the core's (the Miner's discs are
                        # 13 uu off their core); a ring further off is another part of the machine
LEVEL_REACH = 2         # the core's run comes within this many rows of the connection's level along the axis
BODY_REACH = 12         # rows beyond an end over which the body is counted when no stem decides
LOOP_TRIALS = 12        # circles seeded through three vertices of one edge loop, per loop
MIN_LOOP = 6            # a ring has a row with at least this many vertices exactly on its circle (an edge loop)
STRONG_DISC = 0.25      # a disc has this share of the strongest disc's vertices
BODY_ROWS = 4           # the body is on the side of the knob with more vertices within this many rows beyond its end
RADIUS_STEP = 0.2       # the radius histogram's resolution when two close walls share a bin
NEAR_MESH = 160.0       # a mesh whose origin is this close to the connection may be the plug itself
PRECISION = 2

_report = []


def _log(message):
    _report.append(message)
    unreal.log("[ACPR-SOCKETS] " + message)


def _note(message):
    _report.append(message)


def _plugin_dir():
    return os.path.join(os.path.abspath(str(unreal.Paths.project_dir())), "Mods", "GameFeatures", MOD)


def _temp_dir(*parts):
    """Scripts/Temp — where every script writes its log and scratch output; git ignores it."""
    directory = os.path.join(_plugin_dir(), "Scripts", "Temp", *parts)
    os.makedirs(directory, exist_ok=True)
    return directory


def _subobject(data):
    """The object behind a subobject handle: a blueprint's template component."""
    return unreal.SubobjectDataBlueprintFunctionLibrary.get_associated_object(data)


# --------------------------------------------------------------------------- components

def _handles(blueprint):
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    return list(subsystem.k2_gather_subobject_data_for_blueprint(blueprint) or [])


def _components(blueprint):
    """
    Every scene component of the blueprint with its actor-space transform, composed up the
    attachment chain the SubobjectDataSubsystem reports (get_parent_handle).
    """
    # A SubobjectDataHandle has no reflected fields, so handles cannot be dictionary keys or
    # compared from Python (they all look equal). The components are keyed by their names, and a
    # parent is found by resolving its handle to its object and taking that object's name.
    library = unreal.SubobjectDataBlueprintFunctionLibrary
    by_name = {}
    for handle in _handles(blueprint):
        data = library.get_data(handle)
        obj = _subobject(data)
        if obj is None or not isinstance(obj, unreal.SceneComponent):
            continue
        parent_name = None
        parent = library.get_parent_handle(data)
        if parent is not None and library.is_handle_valid(parent):
            parent_obj = _subobject(library.get_data(parent))
            if isinstance(parent_obj, unreal.SceneComponent):
                parent_name = parent_obj.get_name()
        by_name[obj.get_name()] = (obj, parent_name)

    transforms = {}

    def actor_transform(name, depth=0):
        if name in transforms:
            return transforms[name]
        obj, parent_name = by_name[name]
        local = obj.get_relative_transform()
        if depth > 32 or parent_name is None or parent_name not in by_name or parent_name == name:
            transforms[name] = local
        else:
            # Parent first in the chain: local then parent (A then B: A * B).
            transforms[name] = local.multiply(actor_transform(parent_name, depth + 1))
        return transforms[name]

    return [(obj, actor_transform(name), parent_name or "-") for name, (obj, parent_name) in by_name.items()]


# --------------------------------------------------------------------------- meshes

def _vertices(copy, asset, lod_type=None):
    """
    All vertices of a mesh asset (render LOD 0 unless lod_type says otherwise) in the asset's own
    frame, as (position, normal) pairs; the normal is None when the mesh carries no valid normals.
    """
    dynamic = unreal.DynamicMesh()
    options = unreal.GeometryScriptCopyMeshFromAssetOptions()
    lod = unreal.GeometryScriptMeshReadLOD(lod_type=lod_type or unreal.GeometryScriptLODType.RENDER_DATA, lod_index=0)
    result = copy(asset, dynamic, options, lod)
    outcome = result[-1] if isinstance(result, tuple) else result
    if str(outcome).endswith("FAILURE"):
        return None

    def vector_list(got):
        for item in (got if isinstance(got, tuple) else (got,)):
            if isinstance(item, unreal.GeometryScriptVectorList):
                return list(unreal.GeometryScript_List.convert_vector_list_to_array(item))
        return None

    # Both lists are indexed by vertex id (skip_gaps False), so they line up.
    positions = vector_list(unreal.GeometryScript_MeshQueries.get_all_vertex_positions(dynamic, False))
    if positions is None:
        return None
    got = unreal.GeometryScript_Normals.get_mesh_per_vertex_normals(dynamic, True)
    valid = isinstance(got, tuple) and any(item is True for item in got[2:3])
    normals = vector_list(got) if valid else None
    if normals is None or len(normals) != len(positions):
        return [(v, None) for v in positions]
    return list(zip(positions, normals))


_mesh_cache = {}


def _mesh_vertices(mesh):
    if mesh is None:
        return None
    path = mesh.get_path_name()
    if path not in _mesh_cache:
        # The copy runs inside the engine; if it asserts, the process dies with it, so the log
        # names the mesh first.
        _note("    copying %s" % path)
        _write_log(quiet=True)
        try:
            if isinstance(mesh, unreal.StaticMesh) and mesh.get_num_lods() == 0:
                # No render LODs at all: copying it, or asking for LOD 0's counts, asserts in the engine.
                _note("    mesh %s: no render LODs" % path)
                _mesh_cache[path] = None
            elif isinstance(mesh, unreal.StaticMesh):
                _mesh_cache[path] = _vertices(unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh, mesh)
            elif isinstance(mesh, unreal.SkeletalMesh):
                # The Miner Mk.1 and the Truck Station keep their socket in a skeletal mesh. Its LOD
                # render data streams and the copy asserts when LOD 0 is not resident, so the editor's
                # source model is read instead, which is never streamed.
                _mesh_cache[path] = _vertices(unreal.GeometryScript_AssetUtils.copy_mesh_from_skeletal_mesh, mesh,
                                              unreal.GeometryScriptLODType.SOURCE_MODEL)
            else:
                _mesh_cache[path] = None
        except Exception as error:
            _note("    mesh %s: %s" % (path, str(error).splitlines()[0]))
            _mesh_cache[path] = None
    return _mesh_cache[path]


def _blueprint_of_class(cls):
    """The Blueprint asset of a generated class: /Game/X/Build_Y.Build_Y_C -> /Game/X/Build_Y."""
    if cls is None:
        return None
    path = cls.get_path_name()
    if not path.endswith("_C"):
        return None
    return unreal.EditorAssetLibrary.load_asset(path.split(".")[0])


def _meshes(blueprint, cdo, depth=0):
    """
    Every mesh the machine renders, as (label, mesh asset, actor-space transform). Child actor
    components contribute the meshes of their child class, placed by the component.
    """
    out = []
    components = _components(blueprint)
    for obj, transform, parent_name in components:
        name = obj.get_name()
        if isinstance(obj, unreal.StaticMeshComponent):
            out.append((name, obj.get_editor_property("static_mesh"), transform))
        elif isinstance(obj, unreal.SkeletalMeshComponent):
            out.append((name, obj.get_editor_property("skeletal_mesh_asset"), transform))
        elif isinstance(obj, unreal.ChildActorComponent) and depth < 2:
            child_class = obj.get_editor_property("child_actor_class")
            child_blueprint = _blueprint_of_class(child_class)
            if child_blueprint is not None:
                child_cdo = unreal.get_default_object(child_class)
                for label, mesh, local in _meshes(child_blueprint, child_cdo, depth + 1):
                    out.append((name + "/" + label, mesh, local.multiply(transform)))
            else:
                _note("    child actor %s: class %s has no blueprint to read" % (name, str(child_class)))
    instance_data = cdo.get_editor_property("m_instance_data_cdo") if cdo is not None else None
    if instance_data is not None:
        for index, instance in enumerate(instance_data.get_editor_property("instances") or []):
            out.append(("instance[%d]" % index, instance.get_editor_property("static_mesh"),
                        instance.get_editor_property("relative_transform")))
    return out, components


# --------------------------------------------------------------------------- the fit

def _kasa(points):
    """Least-squares circle through 2-D points: x² + y² + a·x + b·y + c = 0. Returns (cx, cy, r)."""
    sxx = sxy = syy = sx = sy = sxz = syz = sz = 0.0
    n = float(len(points))
    for x, y in points:
        z = x * x + y * y
        sxx += x * x; sxy += x * y; syy += y * y; sx += x; sy += y
        sxz += x * z; syz += y * z; sz += z
    # Normal equations for (a, b, c), solved by Cramer's rule.
    m = [[sxx, sxy, sx], [sxy, syy, sy], [sx, sy, n]]
    v = [-sxz, -syz, -sz]

    def det(a):
        return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))

    d = det(m)
    if abs(d) < 1e-9:
        return None
    sol = []
    for i in range(3):
        mi = [row[:] for row in m]
        for r in range(3):
            mi[r][i] = v[r]
        sol.append(det(mi) / d)
    a, b, c = sol
    cx, cy = -a / 2.0, -b / 2.0
    rr = cx * cx + cy * cy - c
    if rr <= 0:
        return None
    return cx, cy, math.sqrt(rr)


def _largest_gap(points):
    """The widest angular gap (degrees) between neighbouring points seen from the origin."""
    angles = sorted(math.degrees(math.atan2(y, x)) for x, y in points)
    if len(angles) < 2:
        return 360.0
    gaps = [b - a for a, b in zip(angles, angles[1:])]
    gaps.append(angles[0] + 360.0 - angles[-1])
    return max(gaps)


def _rows(values):
    """Vertex counts per ROW of distance along the axis, keyed by the row's lower edge."""
    rows = {}
    for z in values:
        k = int(math.floor(z / ROW)) * ROW
        rows[k] = rows.get(k, 0) + 1
    return rows


def _dense_run(rows):
    """
    The run of occupied rows with the most vertices, where up to RUN_GAP empty rows do not break
    a run (edge loops 10 uu apart, discs 15 uu apart, still read as one wall). Returns (lo, hi,
    count) in row keys.
    """
    keys = sorted(rows)
    runs, current = [], []
    for k in keys:
        if current and k - current[-1] > (RUN_GAP + 1) * ROW:
            runs.append(current)
            current = []
        current.append(k)
    if current:
        runs.append(current)
    best = max(runs, key=lambda run: sum(rows[k] for k in run))
    return best[0], best[-1], sum(rows[k] for k in best)


def _fit_axis(points):
    """
    points: (x, y, a, nx, ny) — across, across, along the candidate axis, and the vertex normal's
    across components (None, None without normals). Finds the knob among the rings (radial bins)
    seen along this axis and returns (fit, score, text) or (None, 0, text):

    * every bin that holds a ring is circle-fitted (Kåsa), the radius then taken as the mode of the
      vertices' distances from that centre so two close walls in one bin do not average, and the
      fit refined on the vertices on that wall;
    * a ring is scored by the vertices exactly on its circle within its densest run of edge loops;
      rings with at least STRONG of the best score are real walls; the axis's score is its core's;
    * a wall whose normals point toward the axis is concave — a recess, a hole — and never the
      knob; among the convex walls the best-scoring is the knob's core and the outermost within
      twice its radius is the tube's radius (the discs of an insulator stack); the tube spans the
      core's run; the end with more vertices just beyond it is the bottom, where the body is.
    """
    # Seeds for the ring search: every radial bin (a ring on the axis lands in one), and circles
    # through three vertices of one edge loop (a ring off the axis, like the Miner's discs, is
    # spread over many bins and lands in none).
    bins = {}
    for x, y, a, nx, ny in points:
        r = math.hypot(x, y)
        if MIN_RADIUS <= r <= NEAR_AXIS:
            bins.setdefault(int(r / BIN), []).append((x, y, a, nx, ny))
    seeds = []
    for b in sorted(bins):
        wall = bins[b] + bins.get(b - 1, []) + bins.get(b + 1, [])
        if len(wall) >= MIN_RING_VERTICES:
            seeds.append(_kasa([(x, y) for x, y, _, _, _ in wall]))
    seeds += _loop_seeds(points)
    candidates, rejected = [], []
    for circle in seeds:
        if circle is None:
            continue
        cx, cy, radius = circle
        # The bin holds only an arc of an off-centre ring, so the first fit is biased: the wall is
        # re-gathered from every vertex within WALL_BAND of the circle, its radius taken as the
        # most populated RADIUS_STEP of distances from the centre (two close walls do not average),
        # the fit refined on that wall alone, and the three steps repeated until the circle holds
        # still — an arc's fit walks onto its full ring in a few rounds.
        settled = False
        for _ in range(8):
            near_wall = [(x, y) for x, y, _, _, _ in points if abs(math.hypot(x - cx, y - cy) - radius) <= WALL_BAND]
            if len(near_wall) < MIN_RING_VERTICES // 2:
                break
            histogram = {}
            for x, y in near_wall:
                k = int(math.floor(math.hypot(x - cx, y - cy) / RADIUS_STEP))
                histogram[k] = histogram.get(k, 0) + 1
            mode = max(histogram, key=histogram.get)
            mode_radius = (mode + 0.5) * RADIUS_STEP
            on_mode = [(x, y) for x, y in near_wall if abs(math.hypot(x - cx, y - cy) - mode_radius) <= RADIUS_STEP]
            refined = _kasa(on_mode) if len(on_mode) >= MIN_RING_VERTICES // 2 else None
            if refined is None:
                break
            moved = math.hypot(refined[0] - cx, refined[1] - cy) + abs(refined[2] - radius)
            cx, cy, radius = refined
            if moved < TIGHT_BAND / 2:
                settled = True
                break
        if not settled:
            continue
        on_wall = [(x, y, a, nx, ny) for x, y, a, nx, ny in points if abs(math.hypot(x - cx, y - cy) - radius) <= WALL_BAND]
        gap = _largest_gap([(x - cx, y - cy) for x, y, _, _, _ in on_wall])
        if len(on_wall) < MIN_RING_VERTICES or gap > MAX_RING_GAP:
            rejected.append("r %.1f: %d vertices on the wall, widest gap %.0f deg" % (radius, len(on_wall), gap))
            continue
        # The wall's extent and score come from the vertices exactly on its circle: a band a little
        # outside is within WALL_BAND but not on this wall.
        on_circle = [(x, y, a, nx, ny) for x, y, a, nx, ny in on_wall if abs(math.hypot(x - cx, y - cy) - radius) <= TIGHT_BAND]
        along = [a for _, _, a, _, _ in on_circle]
        if len(along) < MIN_RING_VERTICES // 2:
            rejected.append("r %.1f: only %d vertices exactly on the circle" % (radius, len(along)))
            continue
        rows = _rows(along)
        # A ring is made of edge loops: some row holds a good part of a loop. A circle that merely
        # threads through scattered vertices of other rings has one or two per row.
        if max(rows.values()) < MIN_LOOP:
            rejected.append("r %.1f: no edge loop (at most %d vertices in a row)" % (radius, max(rows.values())))
            continue
        lo, hi, _ = _dense_run(rows)
        run = [a for a in along if lo <= a < hi + ROW]
        below = sum(1 for a in along if a < lo)
        above = sum(1 for a in along if a >= hi + ROW)
        exact = [v for v in on_circle if lo <= v[2] < hi + ROW]
        score = len(exact)
        if score < MIN_RING_VERTICES // 2:
            rejected.append("r %.1f: only %d vertices exactly on the circle" % (radius, score))
            continue
        # Convexity: the mean radial component of the normals on the wall; without normals, convex.
        radial = [((x - cx) * nx + (y - cy) * ny) / max(math.hypot(x - cx, y - cy), 0.01)
                  for x, y, _, nx, ny in exact if nx is not None]
        convexity = sum(radial) / len(radial) if radial else 1.0
        candidate = {"score": score, "cx": cx, "cy": cy, "radius": radius, "lo": min(run), "hi": max(run),
                     "below": below, "above": above, "n": len(on_wall), "gap": gap, "rows": rows,
                     "convexity": convexity}
        # The same wall reached from a neighbouring bin: keep whichever fit has more exact vertices.
        same = [c for c in candidates if abs(c["radius"] - radius) < WALL_BAND / 2 and math.hypot(c["cx"] - cx, c["cy"] - cy) < WALL_BAND]
        if same:
            if score > same[0]["score"]:
                candidates[candidates.index(same[0])] = candidate
            continue
        candidates.append(candidate)
    if not candidates:
        return None, 0, "no ring: " + ("; ".join(rejected) if rejected else "no vertices near the axis")
    best_score = max(c["score"] for c in candidates)
    strong = sorted((c for c in candidates if c["score"] >= STRONG * best_score), key=lambda c: c["radius"])
    convex = [c for c in strong if c["convexity"] >= 0.0]
    if not convex:
        return None, 0, "every wall is concave: " + ", ".join("r %.1f (%+.2f)" % (c["radius"], c["convexity"]) for c in strong)
    # The core: the innermost convex wall that is a whole cylinder (no wide gap) and strong — the
    # knob's own cylinder, between and above the discs; its dense run of edge loops is the knob's
    # extent (the discs sit on it, and the base cylinder below shares the discs' radius, so the
    # discs' own runs reach too far).
    # The knob stands at the connection's level along the axis — a cable attaches there — though
    # not always on its axis (the Truck Station's connection sits beside its knob); a ring whose
    # run lies wholly above or below is another part of the machine, however clean.
    near = [c for c in convex if c["convexity"] >= MIN_CONVEXITY and c["gap"] <= HALF_CORE_GAP
            and c["lo"] - LEVEL_REACH * ROW <= 0.0 <= c["hi"] + LEVEL_REACH * ROW]
    if not near:
        return None, 0, "no convex wall at the connection's level: " + ", ".join(
            "r %.1f (gap %.0f, %+.2f, %.0f..%.0f at (%.1f, %.1f))" % (c["radius"], c["gap"], c["convexity"], c["lo"], c["hi"], c["cx"], c["cy"]) for c in convex)
    core = near[0]
    core_kind = "whole" if core["gap"] <= MAX_CORE_GAP else "half"
    cx, cy = core["cx"], core["cy"]
    # The discs: every convex ring outside the core within twice its radius and centred near it,
    # strong or not — a disc's edge loops are often coarser than the core's, and the tube must
    # still clear them; a ring centred further off is another part of the machine.
    stack = sorted((c for c in candidates if c["convexity"] >= 0.0 and core["radius"] <= c["radius"] <= 2.0 * core["radius"]
                    and math.hypot(c["cx"] - cx, c["cy"] - cy) <= DISC_OFFSET * c["radius"]), key=lambda c: c["radius"])
    # A faint ring just outside the discs (a circle threading a few of their vertices) does not
    # set the radius: a disc has a fair share of the strongest disc's vertices.
    strongest = max(c["score"] for c in stack)
    stack = [c for c in stack if c["score"] >= STRONG_DISC * strongest]
    # The tube wraps the discs: its radius and centre are the strongest disc's — the one that is
    # seen, which need not be concentric with the core (the Miner's is not) — and not a fainter
    # ring a little further out, which is a circle threading a few vertices of a housing.
    discs = [c for c in stack if c["radius"] - core["radius"] > MERGE_RADIUS]
    outer = max(discs, key=lambda c: c["score"]) if discs else core
    centre_on = outer
    lo, hi = core["lo"], core["hi"]
    # Which end is the top. A sparse continuation of the core beyond one end only is its stem,
    # modelled on into the body: that end is the bottom. Failing that, the body is the side with
    # more vertices within BODY_REACH rows beyond the end, inside 2.5 radii of the axis.
    limit = 2.5 * outer["radius"]
    below = sum(1 for x, y, a, _, _ in points if lo - BODY_REACH * ROW <= a < lo and math.hypot(x - cx, y - cy) <= limit)
    above = sum(1 for x, y, a, _, _ in points if hi < a <= hi + BODY_REACH * ROW and math.hypot(x - cx, y - cy) <= limit)
    if (core["below"] > 0) != (core["above"] > 0):
        flipped = core["above"] > 0
        decided = "stem"
    else:
        flipped = above > below
        decided = "body"
    if flipped:
        lo, hi = -hi, -lo
    fit = {"sign": -1.0 if flipped else 1.0, "offset": (centre_on["cx"], centre_on["cy"]), "radius": outer["radius"],
           "core": core["radius"], "bottom": lo, "top": hi}
    text = "%s core r %.1f (%d exact, widest gap %.0f deg, convexity %+.2f) centred (%.1f, %.1f), run %.1f..%.1f, stem below %d / above %d, body beyond bottom %d / top %d, %s decides%s" % (
        core_kind, core["radius"], core["score"], core["gap"], core["convexity"], cx, cy, core["lo"], core["hi"],
        core["below"], core["above"], below, above, decided, " -> knob points along -axis" if flipped else "")
    if len(stack) > 1:
        text += "; stack to r %.1f: " % outer["radius"] + ", ".join("r %.1f (%d, %.1f..%.1f)" % (c["radius"], c["score"], c["lo"], c["hi"]) for c in stack[1:])
    text += "; centres: " + ", ".join("r %.1f (%.1f, %.1f)" % (c["radius"], c["cx"], c["cy"]) for c in sorted(candidates, key=lambda c: c["radius"]) if c["convexity"] >= 0.0)
    text += "; tube r %.1f %.1f..%.1f centred on r %.1f (%.1f, %.1f)" % (fit["radius"], fit["bottom"], fit["top"], centre_on["radius"], centre_on["cx"], centre_on["cy"])
    text += "; rows " + " ".join("%d:%d" % (k, core["rows"][k]) for k in sorted(core["rows"]))
    others = [c for c in sorted(candidates, key=lambda c: -c["score"]) if c not in stack]
    if others:
        text += "; other rings: " + ", ".join("r %.1f (%d, %+.2f%s)" % (c["radius"], c["score"], c["convexity"], ", strong" if c["score"] >= STRONG * best_score else "") for c in others[:5])
    if rejected:
        text += "; rejected: " + "; ".join(rejected[:6])
    # The axis's score is its core's: the one wall that must be a real cylinder.
    return fit, core["score"], text


def _loop_seeds(points):
    """
    Circles through three vertices of one edge loop (vertices in one ROW along the axis), as seeds
    for rings the radial bins miss. Deterministic: the same mesh gives the same table.
    """
    rows = {}
    for x, y, a, _, _ in points:
        rows.setdefault(int(math.floor(a / ROW)), []).append((x, y))
    picker = random.Random(0)
    seeds = []
    for row in sorted(rows):
        loop = rows[row]
        if len(loop) < 8:
            continue
        for _ in range(LOOP_TRIALS):
            (x1, y1), (x2, y2), (x3, y3) = picker.sample(loop, 3)
            d = 2.0 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
            if abs(d) < 1e-6:
                continue
            cx = ((x1 * x1 + y1 * y1) * (y2 - y3) + (x2 * x2 + y2 * y2) * (y3 - y1) + (x3 * x3 + y3 * y3) * (y1 - y2)) / d
            cy = ((x1 * x1 + y1 * y1) * (x3 - x2) + (x2 * x2 + y2 * y2) * (x1 - x3) + (x3 * x3 + y3 * y3) * (x2 - x1)) / d
            radius = math.hypot(x1 - cx, y1 - cy)
            if MIN_RADIUS <= radius <= NEAR_AXIS and math.hypot(cx, cy) <= NEAR_AXIS:
                seeds.append((cx, cy, radius))
    return seeds


def _basis(axis):
    """An orthonormal (u, v) across a unit axis, so a point reads as (p·u, p·v, p·axis)."""
    ax, ay, az = axis
    hx, hy, hz = (1.0, 0.0, 0.0) if abs(az) > 0.9 else (0.0, 0.0, 1.0)
    ux, uy, uz = ay * hz - az * hy, az * hx - ax * hz, ax * hy - ay * hx
    n = math.sqrt(ux * ux + uy * uy + uz * uz)
    ux, uy, uz = ux / n, uy / n, uz / n
    vx, vy, vz = ay * uz - az * uy, az * ux - ax * uz, ax * uy - ay * ux
    return (ux, uy, uz), (vx, vy, vz)


def _fit(local, extra_axes=()):
    """
    local: vertices in the connection's frame. Tries the knob axis along each axis of the frame
    and along every direction in extra_axes (from a nearby mesh's origin to the connection point:
    a separate plug mesh points its knob that way, which need not be a frame axis), and keeps the
    axis whose ring scores highest. Returns (fit dict, explanation) or (None, explanation); the
    fit is in the connection's frame: axis (unit), offset (from the connection point to the
    knob's axis), radius, bottom and top along the axis.
    """
    if not local:
        return None, "no vertices within %.0f of the connection" % NEAR_AXIS
    axes = [("Z", (0.0, 0.0, 1.0)), ("X", (1.0, 0.0, 0.0)), ("Y", (0.0, 1.0, 0.0))] + list(extra_axes)
    # The order is the prior: the connection's up first, then the direction of a mesh placed at
    # the connection (a separate plug), then across; a later axis takes over only with PREFER
    # times the score of what it replaces, and a core must be a real ring.
    axes = axes[:1] + list(extra_axes) + axes[1:3]
    best, lines = None, []
    for name, axis in axes:
        u, v = _basis(axis)
        def read(p, n, u=u, v=v, a=axis):
            x, y, z = p.x, p.y, p.z
            nx = ny = None
            if n is not None:
                nx, ny = n.x * u[0] + n.y * u[1] + n.z * u[2], n.x * v[0] + n.y * v[1] + n.z * v[2]
            return (x * u[0] + y * u[1] + z * u[2], x * v[0] + y * v[1] + z * v[2], x * a[0] + y * a[1] + z * a[2], nx, ny)
        fit, score, text = _fit_axis([read(p, n) for p, n in local])
        lines.append("axis %s: %s" % (name, text))
        if fit and score >= MIN_RING_VERTICES and (best is None or score >= PREFER * best[0]):
            best = (score, name, axis, u, v, fit)
    if best is None:
        return None, "no knob on any axis\n            " + "\n            ".join(lines)
    score, name, axis, u, v, fit = best
    sign = fit["sign"]
    cx, cy = fit["offset"]
    offset = tuple(cx * u[i] + cy * v[i] for i in range(3))
    out = {"axis": [round(a * sign, 4) + 0.0 for a in axis],
           "offset": [round(o, PRECISION) for o in offset],
           "radius": round(fit["radius"], PRECISION), "core": round(fit["core"], PRECISION),
           "bottom": round(fit["bottom"], PRECISION), "top": round(fit["top"], PRECISION)}
    why = "knob along %s%s (%.2f, %.2f, %.2f): radius %.1f (core %.1f), offset (%.1f, %.1f, %.1f), tube %.1f..%.1f\n            " % (
        "-" if sign < 0 else "+", name, out["axis"][0], out["axis"][1], out["axis"][2],
        out["radius"], out["core"], offset[0], offset[1], offset[2], out["bottom"], out["top"])
    return out, why + "\n            ".join(lines)


# --------------------------------------------------------------------------- the scan

def _measure(blueprint, cls):
    cdo = unreal.get_default_object(cls)
    meshes, components = _meshes(blueprint, cdo)
    connections = [(obj, transform) for obj, transform, _ in components
                   if isinstance(obj, unreal.FGPowerConnectionComponent)]
    if not connections:
        return None
    _note("%s" % cls.get_name())
    for obj, transform, parent in components:
        loc = transform.translation
        rot = transform.rotation.rotator()
        _note("    component %-40s %-34s parent=%-24s at (%.1f, %.1f, %.1f) rot (%.0f, %.0f, %.0f)" % (
            obj.get_name(), obj.get_class().get_name(), parent, loc.x, loc.y, loc.z, rot.roll, rot.pitch, rot.yaw))
    loaded = []
    for label, mesh, transform in meshes:
        vertices = _mesh_vertices(mesh)
        _note("    mesh %-28s %s: %s" % (label, mesh.get_name() if mesh else "-",
                                        "no vertices read" if vertices is None else "%d vertices%s" % (
                                            len(vertices), "" if vertices and vertices[0][1] is not None else ", no normals")))
        if vertices and isinstance(mesh, unreal.SkeletalMesh):
            # Where the skeletal source model sits, in actor space: the source model is read in
            # the mesh's own frame and may not be where the posed mesh is.
            corners = [transform.transform_location(v) for v, _ in vertices[::max(1, len(vertices) // 500)]]
            _note("        skeletal bounds (actor space, sampled): x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f" % (
                min(c.x for c in corners), max(c.x for c in corners), min(c.y for c in corners), max(c.y for c in corners),
                min(c.z for c in corners), max(c.z for c in corners)))
        if vertices:
            loaded.append((label, vertices, transform, mesh))
    fits = {}
    for connection, transform in connections:
        name = connection.get_name()
        point = transform.translation
        axis = transform.transform_direction(unreal.Vector(0, 0, 1))
        _note("    connection %s at (%.1f, %.1f, %.1f) axis (%.2f, %.2f, %.2f)" % (
            name, point.x, point.y, point.z, axis.x, axis.y, axis.z))
        local = []
        per_mesh = []
        extra_axes = []
        for label, vertices, mesh_transform, _ in loaded:
            # A mesh placed near the connection may be the plug itself, pointing from its origin
            # (its mounting base) to the connection point (its tip): one more axis to try.
            o = transform.inverse_transform_location(mesh_transform.translation)
            distance = math.sqrt(o.x * o.x + o.y * o.y + o.z * o.z)
            if 1.0 <= distance <= NEAR_MESH:
                extra_axes.append(("from %s" % label, (-o.x / distance, -o.y / distance, -o.z / distance)))
            count = 0
            for v, n in vertices:
                world = mesh_transform.transform_location(v)
                p = transform.inverse_transform_location(world)
                if abs(p.x) <= NEAR_AXIS and abs(p.y) <= NEAR_AXIS and abs(p.z) <= NEAR_AXIS:
                    normal = transform.inverse_transform_direction(mesh_transform.transform_direction(n)) if n is not None else None
                    local.append((p, normal))
                    count += 1
            per_mesh.append((count, label))
        per_mesh.sort(reverse=True)
        _note("        vertices near the connection per mesh: " + ", ".join("%s=%d" % (l, c) for c, l in per_mesh[:6]))
        fit, why = _fit(local, extra_axes)
        _note("        " + ("FIT " if fit else "NO FIT ") + why)
        if fit:
            name = _runtime_name(name)
            # The mesh that supplied the knob: the one with most vertices near the connection.
            fit["mesh"] = next((m.get_path_name() for (count, label) in per_mesh[:1]
                                for l, _, _, m in loaded if l == label), None)
            fits[name] = fit
    return fits


def _runtime_name(name):
    """The runtime component is the blueprint's template without the "_GEN_VARIABLE" suffix."""
    return name[:-len("_GEN_VARIABLE")] if name.endswith("_GEN_VARIABLE") else name


def game_changelist():
    """"5.6.1-<changelist>+++FactoryGame+..." — the changelist is the game version SML checks against."""
    engine = str(unreal.SystemLibrary.get_engine_version())
    return engine.split("-", 1)[1].split("+", 1)[0] if "-" in engine else engine


def machines():
    """Every (class, blueprint) under SCAN_ROOTS that is a factory buildable."""
    paths = []
    for root in SCAN_ROOTS:
        paths += unreal.EditorAssetLibrary.list_assets(root, recursive=True, include_folder=False)
    paths = sorted(set(p.split(".")[0] for p in paths if p.split("/")[-1].startswith("Build_")))
    out = []
    for path in paths:
        cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
        if cls is not None and unreal.MathLibrary.class_is_child_of(cls, unreal.FGBuildableFactory):
            out.append((cls, unreal.EditorAssetLibrary.load_asset(path)))
    return out


def signature(blueprint, cls):
    """
    What a fit depends on, read without touching geometry: per power connection, its actor-space
    location and the machine's static meshes with their LOD 0 vertex and triangle counts. Equal
    signatures on two game versions mean the socket has not moved and no mesh it could be on has
    been re-exported — the cheap check acpr_validate.py makes when the game version changed; the
    scan itself is the full one.
    """
    cdo = unreal.get_default_object(cls)
    meshes, components = _meshes(blueprint, cdo)
    mesh_stats = set()
    for _, m, _ in meshes:
        if isinstance(m, unreal.StaticMesh):
            _note("    signature: reading LOD 0 counts of %s (%d LODs)" % (m.get_path_name(), m.get_num_lods()))
            _write_log(quiet=True)
            if m.get_num_lods() > 0:
                mesh_stats.add((m.get_path_name(), m.get_num_vertices(0), m.get_num_triangles(0)))
    mesh_stats = sorted(mesh_stats)
    out = {}
    for obj, transform, _ in components:
        if isinstance(obj, unreal.FGPowerConnectionComponent):
            loc = transform.translation
            out[_runtime_name(obj.get_name())] = {"connection": [round(loc.x, 1), round(loc.y, 1), round(loc.z, 1)],
                                                  "meshes": [list(m) for m in mesh_stats]}
    return out


def signatures():
    """{class path: signature} for every machine; what acpr_validate.py compares the table against."""
    return {cls.get_path_name(): signature(blueprint, cls) for cls, blueprint in machines()}


def _previous(output):
    try:
        with open(output, "r", encoding="utf-8") as handle:
            return json.load(handle).get("sockets", {})
    except (OSError, ValueError):
        return {}


def _diff(before, after):
    """What changed against the previous table, fit by fit, for the log."""
    keys = ("axis", "offset", "radius", "core", "bottom", "top")
    lines = []
    for cls in sorted(set(before) | set(after)):
        for name in sorted(set(before.get(cls, {})) | set(after.get(cls, {}))):
            old, new = before.get(cls, {}).get(name), after.get(cls, {}).get(name)
            label = "%s / %s" % (cls.split(".")[-1], name)
            if old is None:
                lines.append("    new       %s: r=%.1f %.1f..%.1f" % (label, new["radius"], new["bottom"], new["top"]))
            elif new is None:
                lines.append("    gone      %s" % label)
            elif any(old.get(k) != new.get(k) for k in keys):
                lines.append("    changed   %s: r=%.1f %.1f..%.1f axis %s offset %s -> r=%.1f %.1f..%.1f axis %s offset %s" % (
                    label, old["radius"], old["bottom"], old["top"], old.get("axis"), old.get("offset"),
                    new["radius"], new["bottom"], new["top"], new.get("axis"), new.get("offset")))
    return lines


def scan():
    _report.clear()
    changelist = game_changelist()
    table = {"game_changelist": changelist, "default": DEFAULT_FIT, "sockets": {}}
    if not hasattr(unreal, "ACPRSocketTableAsset"):
        _log("ACPRSocketTableAsset is not in this editor — the C++ has not been built since it was added, or the editor not restarted; the table asset cannot be written")
    found = machines()
    _log("%d factory buildables under %s" % (len(found), ", ".join(SCAN_ROOTS)))
    for cls, blueprint in found:
        # The log is rewritten after every machine: a crash inside the engine (a mesh copy, say)
        # takes the process with it, and the log then names the machine it died on.
        _write_log(quiet=True)
        try:
            fits = _measure(blueprint, cls)
            if fits:
                for name, sig in signature(blueprint, cls).items():
                    if name in fits:
                        fits[name]["signature"] = sig
                table["sockets"][cls.get_path_name()] = fits
        except Exception:
            _note("%s: ERROR\n%s" % (cls.get_name(), traceback.format_exc()))
    output = os.path.join(_plugin_dir(), OUTPUT)
    previous = _previous(output)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    text = json.dumps(table, indent=2, sort_keys=True) + "\n"
    with open(output, "w", encoding="utf-8") as handle:
        handle.write(text)
    _log("%d classes fitted on game changelist %s -> %s" % (len(table["sockets"]), changelist, output))
    _write_asset(json.dumps(_with_manual(table), indent=2, sort_keys=True) + "\n")
    if previous:
        changes = _diff(previous, table["sockets"])
        _log("against the previous table: %d fit(s) changed" % len(changes))
        for line in changes:
            _log(line)


def _with_manual(table):
    """
    The shipped table: the measured one with Config/ACPR_Sockets_Manual.json laid over it — a
    hand-made file of the same shape; a field it names replaces the measured one for that class
    and connection, a field it omits stays measured, and an entry it no longer has drops back to
    the measurement. Every field applied is in the log.
    """
    path = os.path.join(_plugin_dir(), MANUAL)
    if not os.path.isfile(path):
        return table
    try:
        with open(path, "r", encoding="utf-8") as handle:
            manual = json.load(handle)
    except ValueError as error:
        _log("MANUAL corrections %s are not valid JSON (%s) — not applied" % (path, error))
        return table
    merged = json.loads(json.dumps(table))
    applied = 0
    for cls, fits in (manual.get("sockets") or {}).items():
        for name, fields in (fits or {}).items():
            entry = merged["sockets"].setdefault(cls, {}).setdefault(name, dict(DEFAULT_FIT))
            for key, value in (fields or {}).items():
                if key in ("axis", "offset", "radius", "core", "bottom", "top"):
                    _log("manual %s / %s: %s = %s (measured %s)" % (cls.split(".")[-1], name, key, value, entry.get(key)))
                    entry[key] = value
                    applied += 1
    if "default" in manual:
        merged["default"].update(manual["default"])
        _log("manual default: %s" % manual["default"])
    _log("%d manual field(s) applied from %s" % (applied, path))
    return merged


def _write_asset(text):
    """The JSON's text into the data asset the packaged mod loads (a loose file is not packaged)."""
    if not hasattr(unreal, "ACPRSocketTableAsset"):
        _log("ACPRSocketTableAsset is not in this editor: build the C++ and restart the editor, then run this again — the JSON is written, the asset is not")
        return
    path = ASSET_FOLDER + "/" + ASSET_NAME
    asset = unreal.EditorAssetLibrary.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
    if asset is None:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", unreal.ACPRSocketTableAsset)
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(ASSET_NAME, ASSET_FOLDER, None, factory)
        if asset is None:
            _log("FAILED to create " + path)
            return
    asset.set_editor_property("json", text)
    saved = unreal.EditorAssetLibrary.save_loaded_asset(asset)
    _log("%s -> %s (%d characters)" % ("saved" if saved else "NOT SAVED", path, len(text)))


def _write_log(quiet=False):
    path = os.path.join(_temp_dir(), LOG_NAME)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(_report) + "\n")
    if not quiet:
        _log("log written to " + path)


# acpr_validate.py loads this file as a library (ACPR_SOCKETS_LIBRARY set) for signatures().
if not globals().get("ACPR_SOCKETS_LIBRARY"):
    try:
        scan()
    except Exception:
        _log("the scan stopped early:")
        for _line in traceback.format_exc().splitlines():
            _log("    " + _line)
    finally:
        _write_log()
