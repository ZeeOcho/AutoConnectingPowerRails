# Auto-Connecting Power Rails — Design Specification

| Field | Value |
| --- | --- |
| Mod title | **Auto-Connecting Power Rails** (short: **Power Rails**) |
| Mod reference / plugin / source name | **AutoConnectingPowerRails** |
| Status | Implemented and tested in single-player. The construct message and hologram state are serialized for multiplayer (§14) but untested with a second player. |

§1–§15 are the rules. Appendices hold art direction, implementation notes and engine
verification — different readers, different change rates.

Every rule appears once, in the section that owns it. Nothing is stated that a careful
reader could derive. §15 is an index, not a source of rules.

**Three terms, never interchangeable:**

- **Coupling** — the saved conductive relationship between two Rail-system terminals.
- **Power Connection** — the vanilla connector the Outlet exposes, into which Power Lines plug.
- **Auto-connect** — the vanilla blueprint feature, by its own name.

---

## 1. Purpose

Vanilla automatically connects compatible conveyor and pipeline endpoints, including across
blueprint boundaries. Power has no equivalent selective backbone.

This mod adds a beam-like conductive **Power Rail** system. Rails couple only through explicit
compatible terminals, giving modular factories an auto-connecting power backbone without
indiscriminate proximity wiring.

### 1.1 The four buildables

- **Power Rail** — derives from the vanilla Painted Beam. Its two ends are logical
  **terminals**.
- **Power Rail Junction** — derives from the Beam Connector. Its six faces are terminals.
- **Power Rail Outlet** — derives from the Wall Outlet. Mounts on any open terminal or on a
  Rail body, and is the sole interface between Rail topology and ordinary power wiring.
- **Insulated Terminal Cap** — no vanilla equivalent. Closes an open terminal off.

Coupled Rails, Junctions and Outlets participate in the vanilla power circuit.

### 1.2 Governing principles

1. **If it snaps, it couples — and coupling has no other cause.** The player never targets a
   Coupling directly; invariant 15 states the mechanism. Its corollary: **if it is geometrically
   in a snapped place, it couples** — a Rail's far end that reaches a terminal its aim never
   touched, a free-hand cube whose centre lands on a Rail's axis (§7.3), two terminals that
   coincide however they got there (§6.1).
2. **Direction is inherited, never chosen.** A Rail leaves a terminal only along that terminal's
   outward axis, so a coupling is always a straight continuation, never a bend.

Where this document is silent, optimise for world/blueprint parity, vanilla-like construction,
readable topology, and deterministic behaviour over convenience.

---

## 2. Scope

### 2.1 Ships

- The four buildables.
- Terminal snapping and coincident coupling during world construction.
- Three Junction placement modes: standalone, terminal-snapped, inline.
- Blueprint boundary auto-connect.
- Vanilla power-grid participation, including cycles.
- Hoverpack power along Rail and Junction length.
- Full Customizer colour and pattern support (§5.5).
- Terminal-state and powered cues on every buildable (§12).

### 2.2 Deferred

Recorded so they are not rediscovered as gaps. No design is attached to any.

- **Machine Auto-wire.** An opt-in mode where a newly placed, unconnected power-consuming
  machine runs a Power Line to the nearest eligible Outlet. Needs its own design for candidate
  preview and cycling, eligibility, Cable cost, failure and cancellation, multiplayer
  validation, and blueprint interaction; disabled while editing or placing blueprints. The
  workaround is to wire machines to an Outlet inside a blueprint and let the backbone
  auto-connect at its boundary. Keep Outlet discovery compatible; add no hook now.
- **Power Line-to-Outlet substitution.** Completing a Power Line onto a valid Rail target would
  atomically create the appropriate Outlet plus the line. Deferred as pure convenience requiring
  deep hologram surgery on fragile seams. If revived: do not replace the global Power Line
  hologram class, do not detour `AFGHologram::SpawnChildren`; the Power Line-specific
  `BeginPlay`, target and construction functions are the supported seams.
- Graceful *visual* degradation on mod removal: the redirected network (§14) is powered but its
  Wall Outlets float where Rails and Junctions were.
- Outlet Mk.2/Mk.3 tiers (§13). If added, they become the hold-to-replace swap group.
- **Clearance data on the Junction.** §7.4's crossing rule runs on the mod's own registry sweep
  rather than on vanilla's clearance overlap event, because vanilla's soft clearance on a
  hologram without clearance data reports no overlap at all (B.2). Giving the Junction clearance
  data would hand that question to vanilla; it is not needed for correctness and is left for a
  later pass.

---

## 3. Invariants

Not to be weakened for implementation convenience.

1. Every accepted terminal snap is conductive.
2. Proximity may locate a candidate; proximity alone never couples.
3. Bare Rail bodies are not coupling targets. Only a Junction or Outlet hologram may target one.
4. Every terminal accepts exactly one occupant: a compatible terminal or private attachment
   interface, or a Cap.
5. A capped terminal is not a geometric snap target.
6. Couplings are stored explicitly. Loading restores them; it never infers them from geometry.
7. Rail topology and Power Line capacity are separate. Internal Couplings do not count toward
   the Outlet's four Power Lines.
8. Ambiguity produces no coupling.
9. Direction is inherited. It enters the system in exactly three places: a free-form standalone
   Rail, a freely rotated standalone Junction, and a Junction's roll about its mating axis.
   Everywhere else it propagates.
10. Generated auto-connect geometry uses the same validator as manual construction — no
    stricter, no looser.
11. Terminals occlude a blueprint search ray; bodies do not. Occlusion and construction
    validation stay separate concerns.
12. Roll is cosmetic. Compatibility and occlusion use position and outward axis only.
13. Topology operations are atomic and server-authoritative.
14. Cycles are valid. No implementation may assume a tree.
15. A Coupling is created only by construction and removed only by dismantling. It is never
    independently selectable or player-targetable.

---

## 4. Terminals and topology

Every terminal is **open**, **coupled** or **capped**, never two at once. Only an open terminal
accepts anything.

A terminal-mounted Outlet owns a private one-sided mounting interface; attaching it creates a
Coupling to that interface, keeping the host terminal inside the three-state model. That
interface is not a continuation terminal, not a blueprint candidate, and cannot accept a Rail.

Electrically:

- **A terminal is geometry and state, not a connector.** Each Rail and each Junction carries
  exactly one Power Connection — at the Rail's midpoint, at the Junction's cube centre — and a
  Coupling is one hidden edge between the two hosts' connections. A terminal has a position, an
  outward axis and a state; the circuit never sees it.
- Each Rail is continuous between its two terminals.
- All six Junction terminals are common.
- Every Outlet is bonded to its host network through the same hidden edge; its socket is the one
  Power Connection external Power Lines reach.
- Crossing, touching, overlapping or visually coincident *bodies* remain separate unless
  terminal topology joins them, so distinct networks may share structural space without becoming
  one circuit.

---

## 5. Power Rail

### 5.1 Construction

- Rails inherit the Painted Beam's two-stage Build Gun workflow and placement behaviour,
  including **soft construction clearance** — a Rail passes through walls, foundations and
  buildings as a structural beam does.
- Length 1–40 m, freely variable.
- A Rail may begin at an open terminal, end at one, join two in one construction, or stand
  alone. Snapping creates the Coupling in the same transaction.
- A snapped Rail continues along the terminal's outward axis, with only roll and length free.
  A bend requires a Junction.
- **A Rail may not pass through a terminal on its axis — any Rail, in its length step.** The
  first terminal encountered along the axis is the only one considered: if it is occupied or
  non-opposing it blocks, and the search never falls through to a farther one, so a Rail cannot
  pass through a capped or coupled face to reach an open one behind it, through its own start
  Rail, or through a Junction's two faces with no Coupling at either. This is §10.4 step 11's
  rule stated for manual construction, which is the point: the same geometry must refuse by hand
  and in a blueprint, or invariant 10's "no stricter, no looser" is untrue at the one place a
  player can see both. It holds for a snapped Rail, a Rail on §11's lane lattice and a free-hand
  Rail alike, measured against the length the Rail will actually be built at.
- The build gun's length readout is the vanilla beam's own and agrees with the built length on a
  far-end snap (B.2).
- Reversing construction direction changes nothing.

### 5.2 Roll

- Square profile with a restrained longitudinal groove on **all four faces**, so the Rail
  presents the same thing at any roll.
- A manual Rail takes the player's beam-derived roll.
- A generated blueprint Rail inherits the BP-side Rail-local roll, or zero roll in the
  Junction's local frame where the BP-side source is a Junction.
- **No collar.** A four-fold groove has no discontinuity to hide: two coupled Rails at any
  relative roll present the same four grooves at the joint, and a 90° roll is the identity.
  §12's terminal-state cue marks the joint instead — and it marks the *terminal* rather than
  the Coupling, because a terminal is a place with a surface while a Coupling is an edge
  without one.

### 5.3 Electrical behaviour

- A Rail carries connectivity only — it does not produce, consume, store, switch, prioritize or
  limit power, so no throughput, voltage or load is simulated.
- Removing one edge of a still-connected loop does not depower the rest; removing the only
  bridge splits the grids immediately.
- Rails behave as ordinary conductors in the power UI, adding no widget of their own. The
  Outlet shows vanilla's connections readout (used / total Power Lines) when looked at, as a
  power pole does.

### 5.4 Hoverpack

A powered Rail or Junction provides Hoverpack power through its one ordinary power connection
component (§4), which vanilla's unmodified Hoverpack search finds. The mod adds no search, no
hook and no geometry test of its own.

The Hoverpack's search radius is **6200 uu (62 m)**, read from `Equip_HoverPack`. A Rail's
connection sits at its midpoint, a Junction's at its cube centre, which it needs regardless as the
common point of its six terminals. The worst case on a chain of maximum-length Rails is a point
20 m from the nearest node, giving guaranteed perpendicular coverage of `√(62² − 20²) = 58.7 m`:
**94.7% of full reach**, for any mix of lengths, with no global coordination.

> **Why one node, not several.** An earlier design placed `⌈L / S⌉` connections per Rail with a
> tunable spacing *S*. One connection per host is what §4's electrical model wants — a Coupling
> is an edge between two hosts' connections — and the coverage arithmetic above shows the extra
> nodes bought at most 2.5 m of reach while doubling the Hoverpack handoffs a player flying a
> backbone experiences. Vanilla's own railway is one connection per arbitrarily long track piece.
>
> **Why not the Railway.** A railway track piece holds one *curved* spline whose length is bounded
> only by an editor default, and exactly one power connection, exposed as `GetThirdRail()`; vanilla
> measures Hoverpack range along the track spline for that reason. A Rail is straight, capped at
> 40 m, so the ordinary point model — what every pole, building and station uses — is dense
> enough. The spline path is closed in any case: `AFGHoverPack` names the concrete class
> `AFGBuildableRailroadTrack` in both its connection call and its replicated state.

### 5.5 Customization

- Steel is the structural and recipe material for Rail and Junction.
- Colour swatches, paint finishes and surface patterns are available on all four buildables
  under ordinary vanilla unlocks. All three are free (§13), and all survive blueprint save and
  placement, generated Rail construction, and Junction insertion.
- **A fresh part wears the Foundation group's default swatch**, so an unpainted part is dark
  infrastructure and the accent appears when someone paints it. **A hand-built Rail or Junction
  takes its first coupled partner's customization when it couples** — not when it snaps, so
  aiming away after a snap leaves nothing behind — and a Cap or Outlet takes its host's. Loaded
  and blueprint-placed parts are untouched.
- **Material-replacement recipes are not offered**, though vanilla structures support them:
  Concrete is the visual signal for the insulating Cap, so a Concrete Rail would read as exactly
  the thing that means "no coupling here".

### Verify

- A Rail passes through a wall as a beam does.
- A loop stays powered after one redundant edge is removed.
- Hoverpack coverage holds at the worst point in a chain — a boundary between two maximum-length
  Rails — and every Rail length in between.
- Reversing construction direction leaves canonical coordinates unchanged.
- A free-hand Rail cannot be built back through its own start Rail or through a Junction.

---

## 6. Couplings

### 6.1 Creation

A Coupling is saved state, not a purchased buildable. It arises only from a construction event:

1. an accepted terminal snap during Rail, Junction or terminal-mounted Outlet construction; or
2. an accepted coincident pair — two exactly coincident, opposing, open terminals — arising from
   any construction, including blueprint placement in *every* mode.

A Coupling has no visual of its own. What the player sees is both terminals' state cue changing
together (§12) — the same information, at no geometric cost and with nothing to keep
rotationally neutral.

Coincidence couples however achieved, including free-hand placement or nudging that lands
exactly. Players build on the 1 m grid routinely, so this is common rather than exotic, and
coupling is what someone who aligned two terminals almost always wants.

**When it forms.** A Coupling forms the instant its second host registers its terminals — at
the end of that host's own `BeginPlay`, for a built, loaded or blueprint-placed host alike — and
at no other time on its own. There is no deferred tick, no later pass: a blueprint's generated
Rails are coupled inside the construction that makes them, and a split Rail's children couple
correctly by construction.

### 6.2 Consequences

- **Directly-coupled Junctions are supported.** Aim a Junction hologram at another's open face
  (§7), or place two cubes 1 m apart centre-to-centre so their faces coincide. A row or block of
  them is a compact multi-lane manifold carrying connectivity only.
- **To build adjacent but *uncoupled*, cap the terminal first.**
- Two opposing Caps may sit face-to-face. Removing either or both never couples them; the
  exposed pair is joined only by dismantling and rebuilding one participant.

### 6.3 Removal

- Dismantling either host removes the Coupling and reopens the surviving terminal — before the
  dismantle effect plays, so a neighbour never reads Coupled to a part that is going.
- Hold-to-replace preserves Couplings when the replacement's terminals land at identical
  positions and axes, and is rejected otherwise, so a cosmetic change never silently severs a
  network. Forward cover only, since vanilla beam variants share no swap group (Appendix B.2).
- Junction insertion may transfer a Coupling only within its all-or-nothing replacement
  transaction.
- Failed-construction rollback and conservative save repair may remove an invalid edge before it
  is player-visible — integrity operations, not dismantle actions.
- Removing a Cap, loading a save, or moving unrelated geometry never initiates a coupling search.

### Verify

- Coincident opposing open terminals couple however the coincidence arose, in every placement
  mode.
- Two capped opposing terminals do not couple, and removing both Caps does not couple them.
- Adjacent coupled Junctions form one electrically common node.
- A save of 19 000 buildables reloads with every Coupling restored and zero repairs.

---

## 7. Power Rail Junction

A restrained 1 m cubic node with three opposing terminal pairs, all six electrically common and
independently open, coupled or capped. Unused terminals need no Caps, though Caps may mark
deliberate boundaries. The Beam Connector is the placement and visual basis, not a
class-inheritance requirement.

### 7.1 Standalone

- Placeable before any Rail exists, all terminals open.
- Flush against a wall, floor or ceiling it keeps the surface-facing terminal open and usable,
  since under soft clearance a Rail leaving that face routes through the surface.
- Surface support, rotation and base placement match the Beam Connector where six-way topology
  permits. Vanilla's floor-angle rule applies to a standalone cube on a floor; on a surface too
  steep for that rule — a wall, a ceiling — only the angle test is relaxed, and every other
  vanilla placement rule still runs.
- Free positioning by default; **Snap to Guidelines** exposes 1 m wall-, surface- and
  Rail-aligned offsets.
- Can be locked and nudged.

### 7.2 Terminal-snapped

- Snaps to any open terminal; one face mates and the Coupling forms in the same transaction.
- The cube extends outward from the terminal plane, consuming no Rail length. The other five
  terminals begin open.
- Roll about the mating axis is 45° by scroll, 5° with the fine modifier. Since all faces are
  functionally identical, roll only determines where branches point in world space — and a 90° roll
  of the cube is the *identity*, mapping the four side faces onto each other, which is why it is not
  offered as the coarse step. 5° is an exact divisor of 45, so fine adjustment always returns to the
  coarse positions.
- The snap point is authoritative — no nudge. A snapped cube stands on no floor: the placement is
  the host's, and vanilla's floor rule does not apply to it.
- Under soft clearance the cube may overlap surrounding geometry, which is visual rather than
  blocking.

### 7.3 Inline insertion

- Snaps to one specifically targeted Rail body; the previewed host is authoritative and nearby
  Rails are irrelevant.
- **One geometric extension**, in the spirit of principle 1's corollary: a cube placed standalone
  whose centre lands on a Rail's axis, inside that Rail's insertable span, *is* an insertion of
  that Rail, however the aim got there. A Rail leaving a Junction face runs at exactly the height
  the lattice gives a standalone cube's centre, so an aim that slides off the Rail onto the
  surface beside it lands the cube in the cell the insertion had; that cell is one §7.4 refuses
  outright, so no valid standalone placement is converted. Exactly one Rail may qualify; two is
  ambiguity (invariant 8). A Rail merely *near* the cell is never a candidate.
- One opposing pair aligns with and is consumed by the host; the other four remain open as
  branches, rollable about the host with the same 45°/5° control.
- The hologram slides freely, or snaps in 1 m Rail-local increments on Guidelines — a coordinate
  stable under Rail reversal, save/load, blueprint rotation and later splitting. No nudge. An
  inserted cube stands on no floor (§7.2).
- Construction atomically replaces the target with two child Rails coupled to the aligned
  terminals — the split first, so a Junction is never built inside an intact Rail.
- Aiming within 1.5 m of a Rail end switches the hologram to terminal-snapped mode, removing the
  dead zone a 1 m cell plus a 1 m minimum child would otherwise leave; the shortest insertable
  host is therefore 3 m. The modes differ: insertion consumes 1 m of Rail, terminal-snapping
  extends beyond the end.
- **Where that end terminal is coupled or capped**, neither mode is available, and the
  disqualifier must name the occupied terminal rather than reporting proximity to the end.

### 7.4 Insertion validity

Insertion preserves:

- the original's external Couplings and Customizer state, on both children;
- body-mounted Outlets, remapped to the correct child without moving them;
- terminal Caps and Outlets at the original endpoints;
- effective path continuity through the 1 m cell.

It is rejected where it would overlap an attachment, leave an invalid one, sit too close to
another Junction or Outlet, or fail to leave two valid children. It completes atomically on the
server.

**The crossing rule, for every placement mode.** A Junction is rejected — standalone,
terminal-snapped or inserted — when **an unrelated Rail body or terminal passes through its
cell**: an explicit rule, not a clearance consequence, since soft clearance would permit the
overlap and a Junction visually swallowing a Rail it is not coupled to would imply a coupling
that does not exist. "Unrelated" excludes the one host the placement is attached to: the Rail
being split, whose body is in the cell by design, or the host being mated to, whose terminal
lies on the cube's face. A Rail crossing a cell that is attached to a *different* host is never
split or absorbed; to turn an existing crossing into a Junction, rebuild the second axis onto its
terminals. A standalone cell exactly on a Rail's axis is not a crossing but an insertion (§7.3).

### 7.5 Cost ledger and dismantling

- Insertion consumes only the Junction recipe, whose own cost and refund stay separate from the
  Rail ledger.
- The original's paid ledger is never recomputed from child lengths. Each material divides in
  proportion to effective length, integer remainders following a stable endpoint order from
  serialized endpoint identity, and repeated splits subdivide the stored child ledger rather than
  a fresh recipe.
- Aggregate refund across every dismantle order therefore equals the original's exact paid
  ledger. An individual child may refund a different amount than a new Rail of the same length;
  the preview shows the stored figure.
- Both children and the Junction inherit the original's Blueprint Dismantle group, and remapped
  Caps and Outlets keep the appropriate relationship. Branches built later are independent, so
  dismantling the inherited group leaves them open-ended with no stale relationship.
- Dismantling the Junction removes its Couplings and dependent attachments, refunds it, and
  leaves the two through children separated by a 1 m gap repairable with a minimum-length Rail.

### Verify

- Aiming within 1.5 m of an *open* end switches mode; a coupled or capped end is invalid with a
  disqualifier naming the terminal.
- Insertion over an unrelated crossing Rail is rejected and modifies neither Rail; a standalone
  cube on an unrelated Rail's body is rejected too.
- Sliding the aim off a Rail onto the foundation beside it keeps the insertion; building there
  splits the Rail.
- Aggregate refunds across every dismantle order equal the original ledger, with uneven integer
  allocation visible in the preview.
- Later branches survive inherited-group dismantling as independent open-ended Rails.
- Only standalone Junctions can be nudged.

---

## 8. Power Rail Outlet

One tier, equivalent to Wall Outlet Mk.1: one Power Connection, four Power Lines
(`mMaxNumConnectionLinks = 4`), vanilla connector feedback. Its Coupling does not count toward
the four; Rail topology cannot use the Power Connection.

### 8.1 Body-mounted

- Targets one Rail body — the four longitudinal faces only, never a Junction body.
- Bonds without a visible Power Line, changes no terminal state, does not shorten the host.
- Orientation from the target face; ordinary rotation applies.
- Slides freely, snaps to the 1 m grid on Guidelines, cannot be nudged.
- Multiple Outlets per Rail wherever attachment checks pass.
- Aiming at an end face resolves to terminal-mounted or invalid, never silently to a body
  attachment.

### 8.2 Terminal-mounted

- Occupies one open terminal via a private one-sided interface.
- An end attachment, not a pass-through: no Rail continues, and it is neither a snap target nor
  an auto-connect candidate.
- Mounting normal from the terminal axis, rotation about it; no slide, no nudge.
- Dismantling reopens the terminal with no retroactive coupling.

### 8.3 Persistence

Host mode, transform, Power Connection state and Power Lines are saved and replicated; blueprint
construction and Junction insertion remap both modes. Dismantling a host also dismantles
dependent Outlets with full refunds, previewed. An Outlet never becomes a vanilla Wall Outlet.

### Verify

- A terminal-mounted Outlet prevents Rail continuation, and dismantling it reopens the terminal
  without retroactive coupling.
- Aiming at a Rail end face never produces a body attachment.
- Looking at an Outlet shows its used / total Power Lines.

---

## 9. Insulated Terminal Cap

- A Cap snaps only to an open terminal and caps it. It is not itself a snap target.
- A capped terminal is excluded from manual snapping and blueprint candidacy but still occludes
  a blueprint ray.
- Dismantling the Cap reopens the terminal, initiating no search. Dismantling the owning Rail or
  Junction dismantles the Cap with a full refund and visible dependency preview.
- The outer face lies at the terminal snap plane, with a microscopic inward inset permitted
  against z-fighting, and the mesh extends inward into the owning actor and never beyond the
  plane. A Cap therefore changes no terminal position, Rail length, Junction cell, blueprint
  interface transform or stacking geometry. **This holds for every representation** — visible
  mesh, simple collision hull, clearance: nothing of a Cap, seen or unseen, stands proud of the
  plane, and where a Cap has nowhere left to sink, the host's seat moves inward.
- Two opposing Caps sit back-to-back at one plane without overlap, and a dark front-visible edge
  should remain selectable there.
- A Cap may nonetheless become unreachable when another buildable's face lands flush against it —
  the case the cap-then-place pattern in §6.2 creates. This is accepted: the player dismantles
  what is in front of it, as with any enclosed vanilla building.

### Verify

- Back-to-back Caps fit at a shared plane with zero added spacing, and either stays selectable
  from an accessible side.
- A capped terminal still occludes a ray.

---

## 10. Blueprint auto-connect

### 10.1 Inside the Designer

- Rails, Junctions, Outlets, Caps and their Couplings can be constructed and saved, their
  topology serialized and remapped rather than rediscovered from geometry.
- Terminal construction inside one Designer may couple only actors of that same Designer;
  Designer-to-world and cross-Designer Couplings are rejected — and a host in another build space
  is never offered as a snap target in the first place.
- An uncoupled terminal remains a potential boundary interface unless capped or occupied by a
  terminal-mounted Outlet.
- **Both boundary patterns are supported.** A terminal flush with the boundary plane couples
  directly to an adjacent module at no cost; a terminal set back is bridged by a generated Rail —
  trading material cost against module compactness.
- A blueprint's cost comes from its contents' recipes, as vanilla does, so a blueprint containing
  Rails split by Junction insertion is costed from the recipe rather than stored child ledgers and
  always shows one fixed price. Rounding is per child, so the re-cost can only exceed the original
  paid amount, leaving no duplication exploit. §7.5's no-recompute rule stays scoped to live
  splits.

### 10.2 Modes and the two-stage workflow

Four vanilla build modes apply: **Default**, **Default (Auto-Connect)**, **Blueprint**,
**Blueprint (Auto-Connect)**. The "Blueprint" pair describes what the *hologram snaps to* — they
align to existing blueprints — not whether a blueprint is being placed.

- Rail generation runs only in the two Auto-Connect modes, and only while vanilla drives it:
  vanilla's own update call in an auto-connect mode turns the resolver on, its reset call on
  leaving one turns it off. No mode names, no timer.
- Coincident coupling is not mode-gated, because coincidence is itself a snap.

The workflow, confirmed against vanilla:

1. Aiming previews eligible auto-connects continuously.
2. **The first click latches** the exact BP- and OB-terminal identities.
3. The player may then move or nudge. The hologram follows and each latched auto-connect adapts
   its *geometry* in real time but never retargets, showing red where ordinary building rules
   would prevent it, with the error surfaced at that moment.
4. **The second click builds**, but only if every latched auto-connect is valid. A single red one
   blocks the whole placement, and the click simply does nothing since the message is already on
   screen.

Nudge mode entered *before* the first click updates discovery, so pairings can change; entered
*after*, targets stay latched and only geometry adapts. Nudging that changes a generated Rail's
length recalculates its cost live, and running out of materials mid-nudge invalidates the
placement like any other missing-material case.

### 10.3 Candidate snapshot

A **BP candidate** is an open terminal in the hologram being placed; an **OB candidate** is a
terminal already in the world.

- Capped, internally coupled and Outlet-occupied terminals are excluded as sources.
- All world terminals are possible OB candidates or blockers regardless of state.
- Terminals still belonging to any Designer are not world targets.
- Terminals inside the hologram occlude other BP rays but can never be targets.
- The resolver reads one immutable snapshot, taken at the current hologram transform, and does
  not mutate it while iterating. An evaluation runs only when that transform has changed, and the
  blueprint's own terminals — rigid in its body — are indexed once.

### 10.4 The ray test

**Positions** snap to a 1 mm lattice on registration and all matching is exact equality; no
blueprint-only radius or looser rule exists.

**Axes are derived, not separately quantized.** A Rail's outward axis is defined by its two
lattice-aligned terminals, making collinearity an exact rational test. Quantizing angles
independently would admit 0.5° of error, displacing the far terminal of a 40 m Rail by ~0.35 m —
a third of a lane — so logical axis and rendered geometry would disagree. Junctions inherit their
mating axis exactly and multiples of 45° are exact; a freely rotated standalone Junction and one
rolled on the 5° fine step both need quantization, using vanilla's steps.

**Terminal occlusion.** A terminal occludes a **disc centred on its snap point of diameter `face
width − 0.1 m`** — 0.9 m on a Junction face, 0.4 m on a Rail end — governing occlusion only,
while matching still uses the exact snap point. A disc rather than a square keeps occlusion
rotation-invariant, satisfying invariant 12. The 0.1 m margin guarantees a dead band between
adjacent lanes (0.1 m Junction-to-Junction, 0.6 m Rail-to-Rail, 0.35 m mixed), so neighbours can
never both be hit by one ray. Manual snapping uses the ordinary hologram hit test.

Each BP terminal has snap position *p* and outward unit direction *d*, giving the ray *p* + *td*,
*t* ≥ 0. For each source:

1. Search up to the maximum Rail length, 40 m.
2. Exclude the source terminal.
3. Find the smallest *t* at which the ray enters any terminal's occlusion disc.
4. Collect every distinct terminal occluding at that distance.
5. Bodies — Rail, Junction, Outlet, Cap — never occlude; only terminals do. A ray may therefore
   pierce a Junction cube through a corner outside the 0.9 m disc, which requires misalignment of
   roughly 0.48 m on two axes at once and is accepted.
6. No terminal hit: no proposal.
7. More than one terminal at that distance: ambiguous, no proposal.
8. Sole first hit belonging to the new blueprint, capped, coupled, Outlet-occupied, or with a
   non-opposing axis: blocks, no proposal.
9. Sole first hit an open OB terminal with matching quantized position and opposing axis: the
   source's only possible target.
10. A separated pair must pass the ordinary Rail hologram validation — the same validator the
    manual tool uses. Under soft clearance that means length and axis only; a generated Rail
    routes through walls and machines exactly as a hand-built one does, and non-open terminals
    are the only real blocker, handled at step 8.
11. Failure never falls through to a farther terminal.

Each BP candidate therefore has zero or one valid target by construction, and no reverse ray test
is needed. **A ray costs its length, not the world:** every "what is near this ray or point"
question is answered from a coarse spatial index whose cost is bounded by the index, never by a
scan of every terminal.

> **Declared divergence.** Vanilla separates *discovery* range — roughly 16 m — from *validation*
> range, whether the resulting belt is buildable. For straight Rails these collapse into one
> number, set here to the full 40 m Rail length, well beyond vanilla's discovery reach. A
> generated Rail can therefore be long and expensive, so its hologram and exact cost must be
> prominent before confirmation.

### 10.5 Conflicts

All valid proposals are produced from the immutable snapshot before any is accepted. Geometry
already prevents two BP terminals reaching one target: §10.4 gives each source zero or one target,
and step 9's opposing-axis requirement means two sources would have to approach one target along
its single outward axis without occluding each other.

Where it nonetheless happens — a malformed blueprint, exact duplicates, numerical inconsistency,
an implementation defect — **the lowest-scoring proposal wins and the others are dropped for that
frame**, matching the host machinery (Appendix B.3). The resolver logs every such collision by
source, target and score, so the defect this rule exists to catch still surfaces. It is a
diagnostic, not a gameplay rule, and no player-reachable geometry can reach it.

**Generated Rails are not cross-checked against each other geometrically.** Two bridges may
cross, which soft clearance makes harmless, and a geometric pass would reintroduce the
order-dependence the immutable snapshot exists to avoid.

### 10.6 Results and construction

- An accepted coincident pair creates a free Coupling; a separated pair creates one straight Rail
  coupled at both ends, costed from exact length under ordinary missing-material behaviour.
- Every accepted pair shows the vanilla blue two-link icon, drawn by the blueprint hologram exactly as
  it is for vanilla's own auto-connects; every separated pair additionally shows the full generated Rail
  hologram, costed from its exact length, before confirmation. Like vanilla, there is no separate length
  label.
- **A generated Rail's hologram is an active child only once latched.** An unlatched proposal is an
  offer: shown and validated, never priced or constructed. Its validity is re-established when its
  inputs change — the blueprint moved, a click — not every frame; between evaluations the last
  verdict stands.
- The generated Rail inherits BP-side customization and roll, falling back to the saved
  customization of the BP terminal's owning buildable.
- It joins the placed blueprint's Blueprint Dismantle group; the pre-existing OB actor never
  changes ownership, and dismantling a generated Rail reopens both terminals and regenerates
  nothing.
- The base hologram, every generated Rail and Coupling, all costs and all occupancy checks form
  one aggregate transaction: **every latched pair is re-checked before any Rail is built, none is
  built if any is refused**, each built Rail is verified coupled at both ends and destroyed if it
  is not, and child proposals are not applied while the resolver is still evaluating. A read-only
  audit on the next frame reports; it never writes.
- The server revalidates the exact latched identities and geometry, failing the whole construction
  with normal feedback if any is invalid. It never substitutes an unpreviewed target nor drops one
  promised auto-connect while building the rest.
- A placement with no accepted proposals stays valid when the underlying ordinary placement is.
- Reversing which module was placed first yields the same electrical topology, though BP-side
  cosmetic ownership may differ.
- Save loading restores Couplings, generated Rails, parent attachments, dismantle ownership and
  power graph relationships, performing no broad scan. Migration, if ever required, is explicit,
  versioned and narrowly scoped.

### Verify

- One BP source has zero or one valid target.
- A nearer mismatched, capped, coupled, Outlet-occupied or same-blueprint terminal blocks every
  farther one; a body does not.
- Two terminals occluding at the same distance yield no proposal.
- Four stacked lanes auto-connect without cross-lane pairing, and quantized transforms survive
  blueprint rotation.
- Nudge before the first click re-discovers; after it, it does not.
- One invalid latched auto-connect blocks the whole placement.
- Length-changing nudges update cost live.
- A blueprinted split Rail shows one fixed recipe-derived price.
- Ordinary Default and Blueprint generate no Rails but still couple coincident terminals.
- Both Designer boundary patterns work.
- The server builds exactly the previewed set, or nothing.
- A 100-bridge placement builds 100 Rails coupled at both ends, and aiming such a blueprint over
  a 1500-buildable factory stays playable.

---

## 11. Geometry

The 0.5 m Rail profile is a visual identity choice, not a density one: a 1 m profile would be
indistinguishable from structural Painted Beams at a glance, while a much smaller one would weaken
selection, terminal readability and the Wall Outlet relationship. The 1 m lane pitch is set by the
Junction cube, not the Rail.

| Element | Baseline geometry |
| --- | --- |
| Rail body | 0.5 m × 0.5 m square profile |
| Rail length | 1 m minimum, 40 m maximum, freely variable |
| Rail groove | 0.06 m wide × 0.02 m deep, all four longitudinal faces, full length |
| Rail terminal segment | The last 0.35 m at each end carries the same groove; that segment is the state cue |
| Rail joint bridge | While coupled to a Junction, a 0.02 m slice of the Rail's profile fills the Junction's seat between the Rail's end and the socket floor, its groove rising on a 45° ramp to meet the face strip. No collision; the one thing drawn past a terminal plane, and allowed because the space is the coupled partner's |
| Canonical stack pitch | 1 m centre-to-centre |
| Four wall-lane centrelines | 0.5 m, 1.5 m, 2.5 m, 3.5 m on a 4 m wall |
| Floor and wall lane cells | The world 1 m grid on the half-cell phase, both axes across the surface — the Junction's cells |
| Wall-aligned Rail centre | 0.5 m from the wall, leaving a 0.25 m gap behind |
| Rail terminal face | Flush or recessed ≤ 0.05 m; no outward appendage |
| Terminal occlusion disc | Face width − 0.1 m: 0.4 m on a Rail end, 0.9 m on a Junction face. A logical quantity, not drawn |
| Visible terminal recess | Rail end: flush or ≤ 0.05 m. Junction face: a 0.5 m square socket 0.07 m deep — the Rail's own profile, not the occlusion disc. Snap planes unmoved either way |
| Junction logical cell | 1 m cube; terminal planes ±0.5 m from centre on all axes |
| Junction chamfer | 0.02 m on all twelve edges |
| Junction face cue | Four 0.06 m strips per face, from the socket edge to the chamfer — the full width of the face, never onto the rim |
| Junction socket floor | The centre half of each socket floor, sunk a millimetre, lit with the network's power while the terminal is bare |
| Junction joint cue | A 0.02 m band on each of the four *adjacent* faces, 0.03–0.05 m from the face plane |
| Cap seat | The same octagon as the chamfered Cap it seats, on Rail end and Junction face alike, so no corner triangle is left empty |
| Outlet mounting pad | A slim square pad the Rail's own 0.5 m across, ≤ 0.05 m deep, flush with its seat; one pad per seat kind, part of the Outlet |
| Outlet transverse envelope | Visual target ≤ 0.9 m (Appendix C.1) |
| Cap external spacing | 0 m; outer face at or microscopically inside the terminal plane, in every representation (§9) |

Four independent Rail runs including aligned Junctions must sit without geometric overlap on one
4 m wall. The lane centre sits 0.5 m out so a 1 m Junction cube is flush rather than sunk into the
wall, and the vertical centrelines place four 1 m cells exactly across it.

**Rail lanes.** In the Default build mode, on an axis-aligned floor, wall or ceiling, a free Rail uses
the Junction's cells, so a Rail and a Junction placed on the same surface share lanes. Before the first
click the Rail's start sits at the centre of the aimed cell, 0.5 m off the surface. The drag chooses the
axis as it does for a vanilla beam, and the start moves to that cell's edge on the drag side — the face a
Junction in that cell would offer — so the Rail runs down the lane centreline in whole metres and both
terminals land where Junction faces would. A Rail dragged away from (or into) the surface starts mid-cell
on the surface. Diagonal and Free-form modes, tilted or rotated surfaces and locked holograms keep the
Painted Beam's behaviour, and a terminal snap at either end wins over the lattice.

**Clearance is soft** on all four buildables, matching vanilla beams, supports and connectors.
Rails route through walls and machines as beams do, and adjacent lanes never reject one another.
The 1 m lane system is a layout convention supported by guidelines, not a clearance constraint —
which is why generated-bridge validity (§10.4 step 10) and the crossing-Rail rejection (§7.4) are
stated as explicit rules rather than left to fall out of clearance.

Four spatial concepts stay separate in implementation:

- **Visible mesh** — what is rendered.
- **Physical/query collision** — the Build Gun hit surface, for which `Query and Physics` blocking
  is appropriate.
- **Construction clearance** — placement-overlap data. Soft, as above.
- **Attachment Points and terminals** — every terminal is also a vanilla attachment point of one
  Power-Rail-specific type, which is how a hologram snaps to it (B.2); they are not ordinary
  structural Beam Attachment Points.

Selection and dismantle collision may be more forgiving than the mesh but must not make an
adjacent lane unusable. **Hoverpack discovery is vanilla's, not the mod's:** the mod contributes
only the connection component §5.4 places, and implements no search, spatial index or geometry
test for it.

### Verify

- Four wall-aligned Rails fit at the four centrelines, with four flush Junctions at one
  longitudinal position.
- Guidelines produce stable 1 m coordinates surviving Rail reversal, save/load, blueprint rotation
  and splitting.
- Adjacent-lane selection collision does not block construction or dismantling.

---

## 12. Interaction and feedback

- **The exact selected target is highlighted during construction, and nothing else.** A
  hologram lights the one terminal its click will couple to — the far end's target by geometry
  as much as the aimed one — on that terminal's own cue surfaces, at a strength that outshines a
  host already in the state the click produces. Capped, coupled and Outlet-occupied terminals
  never display as valid. There is no "could take" highlight: lighting every eligible terminal
  read as a promise of coupling and was retired.
- Inline Junction and Outlet holograms preview the exact host and final orientation.
- Invalid or ambiguous cases use vanilla-style red feedback with an appropriate disqualifier.
- Free body position is the default, with **Snap to Guidelines** exposing the 1 m Rail-local or
  wall-lane system.
- **Modifier convention.** Following vanilla, the fine modifier (Ctrl by default) modifies
  whichever input is in use: with scroll it gives fine rotation, 5° instead of 45° on a Junction;
  while aiming it exposes guidelines and the 1 m grid.
- Colours, finishes and patterns never influence compatibility, occlusion, candidate search or
  lane identity, and coupling visuals must stay legible after any supported Customizer choice.

**Coupling feedback.** The mod's central promise is that a snap means a coupling, so that moment
is marked:

- Creating a Coupling plays the vanilla electrical-connection cue — spark VFX plus the electrical
  sound.
- It fires regardless of whether the network carries power, matching vanilla's behaviour between
  two unpowered poles.
- For a placement creating many Couplings at once, the sound fires once per construction
  transaction to avoid a pile-up, while the spark plays at each coupling point. A transaction is
  one frame (§6.1), so the batch needs no timer.
- A Coupling restored rather than made plays nothing: a save load, and the internal Couplings a placed
  blueprint carries with it. A blueprint's boundary Couplings and its generated Rails' ends are made by
  that placement and do cue.

**Terminal state.** Every terminal carries a light that is a readout of §4's three states, so
a player can see where a Rail can go without aiming at anything.

| State | Reads as |
| --- | --- |
| Open | Amber, low (a few percent of the lamp strength) |
| Coupled, circuit dead | The power colour, dim |
| Coupled, circuit live | The power colour |
| Capped | Off |

The power colour is a matte white, and it is the same colour the Rail's groove carries — so
terminal and conductor are one signal on two surfaces rather than two schemes. **Hue carries
state, brightness carries power**, which also keeps the amber/green pair, the common
deuteranopia collision, out of the design entirely. **A body is not a terminal:** the Rail's
groove and the Junction's strips show power only — lit when the network is live, dark when it is
not — and never amber, so the only amber on a network is at an open terminal.

Where that light lives follows one rule: **the state cue is close-range and the groove is what
carries the network at a distance.** A Rail should read as a lit line from across a base, not
as a row of lamps.

- **Rail** — the last 0.35 m of groove at each end, on all four faces: same width and depth as
  the body's, so the lit line is unbroken and only its hue changes. A Cap covers the end face
  and not the four sides, so a capped end still shows the segment, dark.
- **Junction face** — four strips running the full width of the face, from the socket edge to
  the chamfer, continuing the four grooves of a Rail coupled there. They stop at the chamfer
  and never turn onto it, so a face buried by another Junction shows nothing. The socket floor
  carries the network's power while the terminal is bare, and goes dark the moment it is
  occupied.
- **Junction joint** — a band carried on the four faces *adjacent* to each face, lit only where
  that terminal is coupled to another Junction's terminal. It exists for the one configuration
  that buries a face entirely, and §6.2 makes that configuration reachable on purpose: two
  flush Junctions are either coupled or capped, so a dark joint is unambiguous.
- **Outlet** — its rib grooves; the accent is the chamfered top ring of its cylinder.
- **Cap** — none. A capped terminal is off by definition.

**The line is continuous along conductors and interrupted only where something attaches to
it.** A Junction replaces the break with the four radiating strips, because it distributes; an
Outlet replaces it with its own body and pad, because it taps. Neither reads as damage to the
line; both say what is happening to the power there. A cue that crosses from one part to another
changes level on one lit face — the Rail's bridge ramps up to the Junction's strip — and never
opens into the other's interior or turns a corner it can be seen edge-on.

**Indicators are events, not polls.** A terminal's light changes when its state changes — a
Coupling forms or is released, a Cap lands or is removed, the circuit gains or loses power, a
hologram selects or releases it — and at no other time. Nothing on a host ticks or polls for it.

The state colours are authored as fixed material instances, one per state, rather than read from
the Customizer, so "can I connect here" stays a learnable rule on every network. The accent takes
the swatch's colour — colour-coding where colour-coding is actually used — with the small state
lights readable regardless of paint.

### Verify

- A Coupling produces spark and sound on powered and unpowered networks alike, once per
  transaction with a spark at each point.
- A single built Rail with no partner shows two amber ends and a dark body; the same Rail
  coupled to a powered network shows a lit body and white ends.
- While aiming, exactly one terminal is highlighted, and it is the one the click couples to.

---

## 13. Progression and costs

**Unlock.** One **Power Rails** package in the AWESOME Shop unlocks all four buildables, visible
after **Basic Steel Production**, at a baseline **4 FICSIT Coupons**. It is self-contained: it
neither requires nor grants the vanilla Structural Beam or Wall Outlet packages, grants no cosmetic
entitlement, and contains only the single Outlet tier.

| Buildable | Baseline recipe | Derivation |
| --- | --- | --- |
| **Power Rail**, length *L* | ⌈*L* / 10 m⌉ × (1 Steel Beam + 1 Cable) | One divisor for both terms, so a single multiplier expresses the whole recipe (Appendix B.2). A 40 m Rail costs 4 Steel Beams + 4 Cable. |
| **Junction** | 1 Steel Beam + 1 Cable | Steel continuity plus one conductive-node Cable. A node joins runs rather than extending one, so it costs no more than the 10 m of Rail it sits between — the six-way join is basic factory plumbing, not a tier of its own. |
| **Outlet** | 4 Wire + 1 Iron Rod | Identical to vanilla Wall Outlet Mk.1 |
| **Cap** | 1 Concrete | Early-game non-conductive cue without an Oil Processing prerequisite |

Both Outlet host modes share the recipe. Rail and Junction deliberately share Steel Beam as their
structural language; Concrete is reserved for the visibly insulating Cap, which contains no Cable
because its role is explicitly non-conductive.

The Rail's cost across its range:

| Length | Multiplier | Steel Beams | Cable |
| --- | --- | --- | --- |
| 1–10 m | 1 | 1 | 1 |
| 11–20 m | 2 | 2 | 2 |
| 21–30 m | 3 | 3 | 3 |
| 31–40 m | 4 | 4 | 4 |

**Rounding and refunds.**

- The Rail's single multiplier rounds up, and generated blueprint Rails use exact constructed
  length in the same formula. The length is taken on the registry's 1 uu lattice first, so a
  far-end snap at an exact multiple costs the multiple, not one step more.
- Coincident Couplings are free.
- Junction insertion uses the stored-ledger rules in §7.5; blueprint capture re-costs from the
  recipe (§10.1).
- Dismantle previews always show actual stored refunds.

**Cosmetics are free and carry no ledger.** Vanilla made colour application free in Update 0.5,
and in-game testing finds no cost for surface patterns either, on application,
dismantling or clearing. There is therefore no cosmetic deposit and nothing for Junction insertion
or blueprint placement to charge or divide. Should a future update make patterns cost-bearing, a
per-building deposit would need reintroducing, since splitting one patterned Rail would otherwise
back two with one payment.

**Outlet tiers.** The initial Outlet is not labelled Mk.1 in player-facing text. Adding Mk.2/Mk.3
would bypass the Caterium MAM progression of the corresponding vanilla connector tiers, while
conditionally injecting Rail variants into other schematics would add compatibility complexity.
Players needing more than four Power Lines add another Outlet to the same Rail.

**Balance status.** Recipe structure, one-package unlock, one Outlet tier, cosmetic scope and
refund model are settled. The 4-coupon price and exact numeric recipes are prototype baselines
subject to playtesting; tune numbers before adding tiers or fragmenting the unlock. Four notes for
that tuning:

- **The shared divisor is structural, not a balance choice.** The vanilla beam cost path applies
  one integer multiplier to the whole recipe, so ingredients scaling at different rates cannot be
  expressed through it (Appendix B.2). The 10 m figure is tunable; the fact that both terms share
  it is not, unless the cost calculation is replaced outright.
- **Steel is now roughly volume-fair.** 4 Steel Beams per 40 m is about a quarter of what a vanilla
  beam of the same length costs, which matches the Rail's quarter-volume 0.5 m profile. The
  earlier ⌈*L* / 4 m⌉ rate charged the full beam rate for a quarter of the material.
- **Cable runs about twice a Power Line's rate** — 4 per 40 m against a Power Line's 2 for the same
  span — which is the intended premium for a conductor that is also structure and is Hoverpack-
  reachable without an Outlet.
- **Granularity is four steps and the floor is one of each.** A 1 m Rail costs what a 10 m Rail
  costs, and §7.3's insertion children can be that short. Halving the divisor doubles the steps and
  the total cost together; they are not separable.

---

## 14. Save, multiplayer and performance

- Topology changes are authoritative on the server; clients preview and the server atomically
  revalidates, so two concurrent operations cannot both occupy one terminal.
- Couplings, terminal states, Cap and Outlet dependencies, host modes, Junction topology, child
  cost ledgers, dismantle ownership, Customizer state and external Power Lines are saved and
  replicated, under a mod-owned schema version distinct from FactoryGame's.
- **What a hologram decided travels with the construct message.** A Rail hologram's far snap and
  requested length, lane length and bridge flag; a Junction hologram's insertion host and centre;
  a Cap's or Outlet's host, terminal index and mode — all are serialized with the hologram, and a
  blueprint's latched terminals, targets and lengths with its construct message, so the server
  builds from what the client previewed rather than from its own re-aim. Serialized and
  replicated; **untested with a second player**, which is why the first release is single-player.
- Runtime behaviour is event-driven: register and unregister terminals and attachments on
  construction, load, split, remap and dismantle; indicators change on the events that change
  them (§12). **No host ticks, no host polls, and no hologram ticks for the mod's sake.**
- Candidate lookup uses a bounded spatial endpoint registry with a coarse index; every spatial
  question — a blueprint ray, a Junction's crossing sweep, a far-end search — costs the index it
  touches, never the world.
- After load and blueprint deserialization, one deterministic world-level pass reconciles
  reciprocal terminal references and their hidden power edge, conservatively discarding corrupt or
  one-sided state but never creating a Coupling from geometry. Each host restores its half of a
  Coupling from the geometry it saved, so whichever host begins play second completes the pair
  and no ordering is assumed.
- **No per-tick world-wide scan and no periodic proximity reconnection pass.** These structural
  guarantees are the performance criterion; throughput figures are a playtest observation, not a
  pre-implementation target. Measured: a 1575-buildable factory with a 100-bridge blueprint held
  over it stays playable, and a 19 000-buildable save reloads with zero repairs.
- Diagnostics cost nothing when off: timing and cost meters run only under a console variable,
  and investigation lines whose question has been answered are deleted, not gated.
- Dismantling any element immediately removes graph edges. The electrical representation supports
  cycles, multiple Outlets on one component, and grid merges and splits.
- **Mod removal keeps the network powered.** A save references a power connection by actor and
  component name — the circuit's member list, every Power Line's endpoint, every Coupling's hidden
  edge — and vanilla restores all three. So every host's one power connection component is named
  as the Wall Outlet's (`PowerConnection`, `mMaxNumConnectionLinks` 4 on both), and the README
  carries the Core Redirects that turn a Rail, a Junction and an Outlet into a Wall Outlet and the
  mod's connection class into vanilla's. With them, the Wall Outlets stand where the hosts' origins
  were, every Coupling is a hidden connection between two of them, every Outlet keeps its Power
  Lines, and Caps are dropped: the factory stays powered and the player re-runs cables only to make
  them visible. Development saves are disposable until first public release, after which
  migration is explicit and versioned.

### Verify

- Host, client, dedicated-server and late-join produce identical topology, ledgers, groups and
  circuit edges, with concurrent terminal contention resolving atomically. *(Open: no second-player
  test has been run.)*
- Loading discovers no couplings that were not saved.
- No per-tick or periodic scan exists at any network size.
- Editor Data Validation reports all four Blueprint leaves valid before cooking: required mesh
  present, functional components native, six Junction face terminals, one Outlet Power Connection
  at `mMaxNumConnectionLinks = 4`, all four non-lightweight.
- A save with a Rail backbone, a Junction, two Outlets, a generator and a machine stays powered
  after the mod is removed with the README's redirects in place.

---

## 15. Targeting index

**Non-normative.** A reader aid summarizing §5–§10. Where it disagrees with them, they win.

| Active hologram | Exact target | Result |
| --- | --- | --- |
| Power Rail | Open compatible separated terminal | New Rail plus Coupling |
| Power Rail | Coupled or capped terminal | No snap; occupied |
| Power Rail | An open terminal behind an occupied one on the same axis | No snap; the nearer terminal blocks (§5.1) |
| Power Rail | Its own start Rail, or a Junction's two faces, in the length step | Invalid; the span rule (§5.1) |
| Power Rail | Cap or bare Rail body | No snap, no coupling |
| Cap | Open terminal | Terminal becomes capped |
| Cap | Rail body or Junction body | Invalid |
| Junction | Open terminal | Terminal-snapped Junction plus Coupling |
| Junction | Rail body ≥ 1.5 m from either end | Atomic inline insertion |
| Junction | A surface beside a Rail, with the cube's cell on that Rail's axis inside its span | Atomic inline insertion (§7.3) |
| Junction | Rail body within 1.5 m of an **open** end | Switches to terminal-snapped mode |
| Junction | Rail body within 1.5 m of a **coupled or capped** end | Invalid; disqualifier names the terminal |
| Junction | Unrelated Rail or terminal in the cell, in any mode | Invalid; not absorbed (§7.4) |
| Outlet | Valid targeted Rail body | Body-mounted Outlet |
| Outlet | Open terminal | Terminal-mounted Outlet plus Coupling |
| Outlet | Coupled/capped terminal, or Junction body | Invalid |
| Started Power Line | Existing Outlet Power Connection | Ordinary Power Line |
| Any construction | Resulting coincident opposing open terminals | Free Coupling |
| Blueprint BP terminal | Accepted separated OB terminal | Costed generated Rail, coupled both ends |

---

# Appendix A — Art direction

### A.1 Rail

- One Beam-resized visual mesh for the whole variable length. Its terminals are invisible
  endpoints at the mesh's end planes, not separate objects; each end carries a terminal segment
  (§11) that is the state cue.
- An open-face pocket is part of that mesh — the Cap's octagon, so a Cap sits flush in it (§11).
  Caps, terminal-mounted Outlets and any cue are attachment or state visuals rather than the
  terminal itself.
- Reads as weighty matte steel: low specularity, broad practical surfaces, restrained edge wear.
  Steel dominates; the painted accent is a restrained line only.
- One narrow longitudinal groove on each of the four faces — 0.06 m wide, 0.02 m deep — gives a
  subordinate conductor cue without splitting the Rail into twin channels, and reads the same
  at any roll.
- **The groove is dimly emissive when powered.** 0.6's "no bright full-length emissive strip"
  stands as written; what is added is a floor, because "dim" is an adjective and adjectives
  drift. Unpowered is unlit. Powered sits at the bottom of the master material's emissive range
  — enough to read as lit across a room, not enough to light anything near it — and the figure
  is tuned by looking and then written into the material script. No animated electricity, no
  neon, no pulsing on the powered state.
- End faces stay flush, so coupled collinear Rails read as one continuous element at normal
  distance. The four-fold groove makes that true at any relative roll, which is what retires
  the collar rather than shrinking it.

### A.2 Junction, Outlet, Cap

- **Junction** — one compact 1 m cubic housing in Beam Connector visual language, no modelled
  terminal arms, twelve edges chamfered 0.02 m. Each face carries a 0.5 m square socket 0.07 m
  deep — the Rail's own profile, so the face reads as *a beam goes here* — and four strips
  running from the socket edge out to the chamfer, which continue the four grooves of a Rail
  coupled there. Reaching the full width is what makes them read as the Rail's line arriving
  rather than as marks around a socket; stopping at the chamfer is what keeps a buried face
  dark. All markings are recessed and move no snap plane. The occlusion disc is deliberately
  **not** drawn: it is a logical quantity (§10.4), and at 0.9 m on a 1 m face it left a 0.05 m
  frame that read as nothing.
- **Outlet** — recognizable Wall Outlet silhouette: compact ribbed cylindrical body, dark rings,
  metal rims, the accent on the chamfered top ring, standing on a slim square pad the Rail's own
  0.5 m across. The pad is part of the Outlet — its own component, one per seat kind, flush with
  the seat — rather than a separate clamp, and carries no accent: on a Rail body it interrupts
  the groove, which is what says *power leaves the network here*; on a Rail end it exactly
  matches the profile it caps. Secondary to the Rail in the mod icon, clearly recognizable
  in-world.
- **Cap** — dark ceramic/concrete/composite face, chamfered to an octagon at the accent width,
  with a restrained host-coloured collar, its dark edge visible and targetable from the front.

An open terminal, a coupled one, a terminal-mounted Outlet and a Cap must be distinguishable at
ordinary build distance — by §12's palette close up, and by silhouette beyond it: an Outlet is
a drum standing off the surface, a Cap is a flat dark plate, and an open terminal is the only
one of the four that is amber.

If testing disproves this baseline, the fallbacks are a twin-channel profile (stronger electrical
silhouette, easily mistaken for miniature railway track) or an encased busbar (immediate electrical
reading, repeating bands stretch poorly across variable lengths). It has not; they are kept until
the icons are done.

### A.3 Build menu and icons

English-only, no tutorial, so discoverability rests on the Build Menu, names, descriptions, icons,
terminal highlighting and hologram feedback. All four appear in the **Power** category and fold into
the vanilla **Power** dismantle filter, searchable under *power*, *rail*, *beam*, *outlet*,
*junction*, *cap*.

| Buildable | Baseline description | Icon direction |
| --- | --- | --- |
| **Power Rail** | A steel power backbone that connects through compatible Rail terminals. Attach a Power Rail Outlet for ordinary Power Lines. | Diagonal matte-steel Rail with restrained groove and a small Outlet cue |
| **Power Rail Junction** | A six-way Power Rail node. Place it standalone, snap it to a terminal, or insert it into one Rail; every unoccupied face accepts a Rail, Outlet or Cap. | Compact cube in Beam Connector visual language |
| **Power Rail Outlet** | Mounts on a Power Rail body or open terminal and provides one connection point for up to four Power Lines. | Wall Outlet silhouette on its pad |
| **Insulated Terminal Cap** | Blocks a Rail terminal from snapping and blueprint auto-connection. | Dark covered terminal face with a restrained insulation cue |

The icons are rendered from the final meshes by `Scripts/acpr_icons.py`, lit by the editor's
thumbnail cubemap, with the parts powered: each buildable alone on a transparent background (512 px
and 64 px, the Cap as a plate seen three-quarters on), and the mod icon — the four buildables above
a run of two Rails bridged by a Rail hologram with the link mark at each coupling, an Outlet with a
cable on the front Rail, and the mod's name over an orange rule. The buildable icons are
transparent; the mod icon exists as the transparent plugin icon with white lettering and the
ficsit.app image with a light grey background and dark lettering. The AWESOME Shop entry shows the
Rail's icon, as vanilla's entries show one buildable.

---

# Appendix B — Implementation notes

Non-normative, but B.1 and B.2's component rules are load-bearing.

### B.1 Game Feature plugin structure

SML 3.12 migrated mods to Unreal **Game Feature plugins**. The plugin lives at
`<Project>/Mods/GameFeatures/AutoConnectingPowerRails/`, not `<Project>/Mods/`, and an
`FGGameFeatureData` asset must exist in the mod's content root named exactly the mod reference —
without it the feature never activates, so the module never loads, with no error explaining why.

**Scaffold with Alpakit's Create Mod wizard (C++ & Blueprint template) rather than by hand.**
Verified against its output, it generates everything structural correctly, including
several things not derivable from the documentation:

| Generated | Note |
| --- | --- |
| `Content/<ModReference>.uasset` | The `FGGameFeatureData` asset, class `/Script/FactoryGame.FGGameFeatureData`. The migration guide's instruction to create this by hand addresses *upgrading a 3.11 mod*, which never had one. |
| `Source/<Mod>/Public\|Private/<Mod>.h/.cpp` | The module class `F<Mod>Module` with `StartupModule()`, `ShutdownModule()` and `IMPLEMENT_MODULE`. **Note the filename has no `Module` suffix**, unlike the manual-setup docs. |
| `Source/<Mod>/<Mod>.Build.cs` | Sets `CppStandard = Cpp20` and a curated list of FactoryGame transitive dependencies before adding `FactoryGame` and `SML`. Substantially more than a hand-written minimum; treat as authoritative. |
| `Config/AccessTransformers.ini` | See below. |
| `Config/PluginSettings.ini` | `+AdditionalNonUSFDirectories=Resources`. |
| `.uplugin` | `FileVersion: 3`, `LoadingPhase: "Default"`, `BuiltInInitialFeatureState: "Active"`, `GameVersion` as a game changelist, SML pinned `^3.12.0`. |

**`Config/AccessTransformers.ini` is SML's mechanism for reaching private and protected
FactoryGame members**, shipped empty. Its entries take the form
`Friend=(Class="AFGSomeClass", FriendClass="FOurClass")` — C++ friendship injection rather than
raising visibility, so one entry naming our module class reaches everything protected and private
on that class. The mod's entries: the module class onto `AFGBlueprintHologram` (registering the
manager, B.3); the manager onto `AFGBlueprintHologram` (its build space, disqualifiers and the
duplicate-connection map, read and filed); the Rail onto `AFGBuildableBlueprintDesigner` (a split
inside the Designer); the mod's power connection onto `UFGCircuitConnectionComponent` (emptying an
archetype-copied preview); the Rail hologram onto `AFGBeamHologram` (reading the private length
the beam will build). Two private fields with no setter — an attachment point component's type and
usage — are written by reflection instead, since a Friend entry for a two-field write is heavier
than the write.

Scan rules in the Game Feature Data asset are needed only for `FGUserSetting`, `FGMessage`,
`FGIconLibrary` and `FGChildInputMappingContext` — none used today, though `FGUserSetting` becomes
necessary if the mod gains settings.

Content registration with SML uses a **GameWorldModule** (`UGameWorldModule` Blueprint), which is a
different mechanism from the C++ `StartupModule()` above despite the similar name.

**Two binaries, not one.** A Visual Studio `Development_Editor` build produces
`UnrealEditor-<Mod>.dll`, which only the editor loads. The game loads
`FactoryGame-Win64-Shipping.dll`, built by Alpakit. Rebuilding in Visual Studio therefore does not
change what the game runs, and a stale game binary is invisible unless the module logs something
that identifies its own build — which is why every class's first runtime line carries a
compile-time stamp from `__DATE__ " " __TIME__`, expanded per source file, so the log says which
files were in the last build.

Alpakit's UAT invocation passes **`-nocompileeditor`** and names only `FactoryGameSteam Win64
Shipping`, `FactoryServer Win64 Shipping` and `FactoryServer Linux Shipping`, so it never builds the
editor DLL. The two tools are complementary rather than alternatives, and the working loop is:
Visual Studio `Development Editor` build → restart the editor **only when the reflected shape
changed** (a new, renamed or removed `UPROPERTY` / `UFUNCTION` / `UCLASS`, a changed property type
or parent class, or a changed CDO component set) → run the editor script → **Save All** → Alpakit →
test.

**A stale build can come from timestamps rather than from tooling.** UBT compares source mtimes
against object files, so any transfer that lands sources older than the last build — a zip extracted
from a machine in another timezone, since zip entries store bare local wall-clock times with no
offset — turns every later build into a correct no-op that still reports success. The tell is a build log with **zero Compile actions** for the shipping target;
the fix is to stamp delivered files ahead of the receiving clock; the guard is the compile-time
build stamp above.

**A clean checkout builds and binds with no manual step.** Every asset reference the code needs is
a soft path default in C++; the editor scripts that generate meshes and materials are re-runnable
and read back what they wrote; `docs/BUILDING.md` gives the order, and its register of manual
steps is empty.

### B.2 Classes and construction

- Use the direction native FactoryGame base → native mod implementation → thin mod Blueprint asset
  leaf. A native class cannot inherit an editor-authored vanilla Blueprint such as
  `Build_Beam_Painted_C`.
- Derive the Rail buildable and hologram from `AFGBuildableBeam` and `AFGBeamHologram` rather than
  duplicating placement constants, supplying the missing Painted Beam asset defaults explicitly
  since sibling Blueprint defaults are not inherited. **This includes the hologram's own defaults:**
  `mBuildModeDiagonal` and `mBuildModeFreeForm` live on the vanilla beam's hologram *Blueprint*, and
  a hologram class with neither cannot choose between its vertical and freeform placement paths.
- **The beam axis is the actor's local X**, while the vanilla beam mesh is authored along +Z from a
  pivot at one end. Any mesh component bound by hand needs the rotation that maps one onto the
  other; the engine convention (an actor's forward is +X) is the reliable guide, not the mesh.
- Keep the Rail non-lightweight: its real actor and components own saved, replicated terminal
  identity and topology. `mCanContainLightweightInstances` and
  `mManagedByLightweightBuildableSubsystem` are independent conditions — clearing only the second
  would already leave a real actor — and the Rail clears both, like the other three buildables; the
  lightweight instance-data hook answers nothing, on every path.
- **The Rail renders through its own `UStaticMeshComponent`, as the other three always have.**
  1.0's rule that it must add no mesh component rested on the belief that a non-lightweight beam
  still rendered through instance data; measured, it never had a renderer at all. The component
  is named, the hologram binds it by that name — never by first match, since every mesh component
  earns an empty zoop container in the hologram that can land ahead of it — and the hologram
  orients it once at `BeginPlay` while vanilla's own code stretches it per frame. The general
  rule survives in its corrected form: **a vanilla buildable's component set is part of its
  hologram's contract — know what the hologram will do with each component before adding one.**
- **A hologram's preview is duplicated from the buildable CDO's component templates**
  (`AFGHologram::SetupComponent`), so a mesh assigned at `BeginPlay` reaches instances but never the
  preview. Anything the hologram must draw has to be on the template, which means `PostLoad` or the
  constructor, not `BeginPlay`.
- **Every terminal is a vanilla attachment point of one mod-owned type.** The hosts carry an
  `UFGAttachmentPointComponent` per terminal (type and usage set by reflection on the CDO), offer
  their terminals through `GetAttachmentPoints`, and the type's `CanAttach` answers §4 — open, and
  not the private interface. The Junction hologram and the Rail hologram aimed at a Junction let
  vanilla's own pipeline filter, select and place: its mating transform was measured identical to
  §6.1's geometry on every face, so the holograms override nothing of it but one filter (a host in
  another build space is never offered, §10.1). **One exception, measured:** `AFGBeamHologram`
  takes a private path for a *beam* target that never reaches the generic code, so a Rail hologram
  aimed at a Rail end runs the mod's own picker for that one target class — the same points, the
  same openness test, a second entry into one rule set.
- **The Junction has no clearance data** (§2.2). Vanilla's per-actor overlap virtual is overridden
  and dormant; §7.4's crossing rule runs on the registry sweep for every placement frame.
- **The length readout is vanilla's.** On a far-end snap the hologram hands vanilla an aim half a
  cell short of the terminal, because vanilla's length is the next whole cell above the distance
  from its own start, and on the attachment route that start is the anchor exactly; the Rail is
  then built at the snap's exact length and the readout agrees with it. The span rule reads the
  length vanilla will build (its private `mCurrentLength`, via the Friend entry) rather than
  estimating it.
- **The build-gun length readout is a descriptor flag, not a cost.**
  `UFGBuildingDescriptor::mUsesDistanceForZooping` decides whether the zoop readout renders as a
  distance or as an instance count — `FGBuildGunBuild.h` states it outright: *"Beams use meters,
  cosmetic buildables use number of instances."* The hologram's cost API is not involved at any
  point: `GetBaseCostMultiplier`, `GetBaseCost` and `GetCost` are never called by the readout.
- **Keep the Rail recipe on a single length divisor.** The vanilla beam cost path multiplies the
  whole recipe by one integer derived from `mLengthPerCost`, so a recipe whose ingredients scale at
  different rates cannot be expressed through it — which is why §13 shares one divisor between
  Steel Beam and Cable. In the asset, §13's ⌈*L* / 10 m⌉ is **`mLengthPerCost = 1000`**, a
  deliberate divergence from vanilla's own 400 uu (4 m), which every vanilla beam uses and which
  would cost ×10 at 40 m. The divisor diverges; the mechanism does not.
- Vanilla beam variants share no hold-to-replace swap group, so the Rail cannot currently be
  swapped into a structural beam. Should that change, keeping it out of that group becomes a hard
  requirement: a silent swap would destroy a Coupling and defeat invariant 1.
- Keep the Junction actor origin at the exact centre of its 1 m cube. Validate mesh bounds and pivot,
  offset the origin 0.5 m from a supporting surface, and snap the two axes across the surface to the
  world 1 m grid on the half-cell phase. Free Rails in the Default mode use the same cells (§11), which
  is what keeps a Junction's face centres and a Rail's terminals on one lattice.
- **A cue state is a material instance, not a custom-data float.** Custom primitive data is
  vanilla's channel — index 6 is written by its power hook whatever the mod puts there, and
  rewriting the block clobbers the swatch preview — so each §12 state is a fixed material instance
  swapped into the slot that changed. Slots are addressed **by name**: the mesh script names the
  slots it makes, the material script fills them by name, the C++ asks the mesh for a name's index,
  and a name a mesh does not carry is a Warning naming the mesh. The accent reads the swatch's
  colour from custom data and nothing else — finishes off, roughness pinned — so a colour-coding
  line is the same colour from every angle. The body reads no custom data at all.
- **The Rail's terminal segment is one component with two meshes:** the plain segment, swapped for
  the bridged one on the coupling event when the partner is a Junction (§11's joint bridge), and
  back on release. Not two components shown and hidden.
- **§12's cue uses two vanilla assets, found in the cooked game:**
  `/Game/WwiseAudio/Events/Buildable/_Shared/Play_F_PowerConnection` for the sound and
  `/Game/FactoryGame/VFX/Misc/BuildEffect/P_PowerlineSparks_01` for the spark. Both are played through
  reflection rather than a module dependency, because event posting moved onto `UAkAudioEvent` in the
  Wwise integration this game ships (`UAkGameplayStatics::PostEventAtLocation` does not exist in it).
  The batch flushes on the next tick — one construction is one frame (§6.1).
- **The Outlet's connections readout** is vanilla's pole widget (`UFGPoleConnectionsWidget`), its
  class read from the Power Pole Mk.1 CDO by reflection, shown and hidden by the buildable's own
  looked-at virtuals.
- **`ConfigureComponents` runs only for hand placement.** It remains the right place for
  player-directed snapping data, and `ConfigureActor` must not touch components. But
  `AFGBlueprintHologram` never instantiates a member buildable's hologram class, so a
  blueprint-placed buildable reaches `BeginPlay` with none of the three configure functions having
  run, and save loading skips them too. Anything a Rail needs in order to *function* therefore
  belongs in the C++ constructor, or in explicit linking at construction finalization (B.3) —
  never in `ConfigureComponents`. What a hologram decided (§14) travels as serialized hologram state.

### B.3 Topology and resolution

- **One terminal component for geometry and state; one power connection per host for the
  circuit.** The terminal is a scene component carrying position, outward axis, occlusion radius,
  state, occupant identity, saved Coupling, candidate registration and diagnostics. The host's one
  `UFGPowerConnectionComponent` is the circuit's node, and a Coupling is an `AddHiddenConnection`
  between two hosts' connections — which is what §4's electrical model and invariant 7's
  wire-budget separation require, since hidden connections do not consume
  `mMaxNumConnectionLinks`.
- **Couplings resolve at registration** (§6.1): at the end of the host's own `BeginPlay`, after its
  terminals are registered. A loaded host restores its half from the geometry it saved rather than
  from the partner's live components, so the pair completes whichever side begins play second.
  `Split` releases the original's Couplings first and lets the children couple by construction.
- **Implement §10 as a specialisation of vanilla's blueprint machinery**, not as bespoke code:
  `FGBlueprintOpenConnectionManager< power connection, Rail hologram >`, registered onto
  `AFGBlueprintHologram` through the protected `RegisterOpenConnectionManager< ManagerClass >()`.
  The template owns only the lifecycle and imposes no distance constant of its own.
  `GenerateOpenConnections` is pure virtual and carries §10.3's candidate rules;
  `GeneratePotentialConnections` and its scoring carry §10.4's ray test;
  `CanDirectlyConnectOpenState` and `ConnectStateDirectly` carry §6.1's coincident pair. In
  exchange it supplies overlap-fed nearby-actor registration, §10.2's latch, `Construct`,
  `SerializeConstructMessage` / `PostConstructMessageDeserialization` for §10.6's server
  revalidation, and `HandleBuildableConnectionRemapping` for the explicit linking B.2 requires.
  §10.5's conflict rule follows the template's rather than overriding it. The manager's own
  evaluation is gated on the body transform it reads, runs only while vanilla drives it (§10.2),
  and validates a bridge when its inputs change (§10.6); bridges refuse vanilla's per-child
  placement calls and are placed by the manager alone.
- Quantize terminal **positions** on registration and compare quantized values for equality.
  Derive a Rail's outward axis from its two quantized endpoints rather than storing a separately
  quantized angle. Introduce no runtime epsilon comparisons in the resolver. The 1 uu quantum is
  the one tolerance the mod has, and every geometric coincidence test uses it.
- Model terminal-mounted Outlet attachment as a Coupling to a private one-sided interface, so the
  three terminal states stay authoritative.
- Represent a Coupling only as reciprocal saved terminal state and a hidden vanilla power edge.
  With no player-facing coupling interaction, no transient acknowledgement actor is required.
- Use one ordinary Rail validator for manual and generated Rails alike.
- Evaluate blueprint proposals from an immutable terminal snapshot, then resolve conflicts, latch
  identities, validate aggregate child holograms, and construct atomically — preflight every pair,
  build none if any is refused, verify each built Rail coupled at both ends — keeping search
  occlusion separate from collision validation.
- Junction insertion should be one authoritative replacement transaction with explicit actor
  remapping and cost-ledger transfer, its stable child-ledger orientation derived from serialized
  endpoint identity rather than actor iteration order.
- **The design gate.** No arbitrary number, delay, timer, retry or offset is added to make a
  problem go away unless it is documented that vanilla's headers offer no event, hook or virtual
  for it, and where the information would otherwise have to come from. Four questions,
  asked before shipping and again in review: is this an arbitrary value? how does vanilla do it?
  is there an event, and if it does not reach us, where would it? is this the right place, or a
  place that works? A finding about a vanilla base class applies to every subclass the mod owns,
  and the entry that records it lists them.
- Diagnostics record source terminal, first-hit distance, every terminal occluding at that
  distance, rejection reason, latched target, blueprint actor mapping, ledger transfer and final
  result. All runtime logging goes through one header: one category, one `[ACPR-…]` tag per
  subsystem, one budgeted gate type for per-frame lines, so a line is one call and a rename is
  one edit. Investigation lines whose question has been answered are deleted; what reads a live
  game stays behind `acpr.Trace`, and costs nothing when it is off. A tripwire that would name a
  real fault — a Warning on an invariant — is neither and stays on.
- Author all player-facing strings as localizable from the start.

### B.4 References

- Modding docs: [Buildable Holograms](https://docs.ficsit.app/satisfactory-modding/latest/Development/Satisfactory/BuildableHolograms.html)
 · [Upgrading SML 3.11 → 3.12](https://docs.ficsit.app/satisfactory-modding/latest/Development/UpdatingFromSml311.html)
 · [Reusing Game Files](https://docs.ficsit.app/satisfactory-modding/latest/Development/ReuseGameFiles.html)
- Wiki: [Beams](https://satisfactory.wiki.gg/wiki/Beams) · [Build Gun](https://satisfactory.wiki.gg/wiki/Build_Gun)
 · [Customizer](https://satisfactory.wiki.gg/wiki/Customizer) · [Power Line](https://satisfactory.wiki.gg/wiki/Power_Line)
 · [Wall Outlets](https://satisfactory.wiki.gg/wiki/Wall_Outlets) · [Hoverpack](https://satisfactory.wiki.gg/wiki/Hoverpack)
 · [Blueprint](https://satisfactory.wiki.gg/wiki/Blueprint)
- Precedents: [VerticalConveyorAutoConnect notes](https://github.com/ZeeOcho/VerticalConveyorAutoConnect/blob/main/DEVELOPMENT.md)
 · [Cable Choices Plus — rail power-box snapping](https://github.com/DavidHGillen/Satisfactory_AB-CableChoices/blob/master/PluginFolder/Source/AB_CableMod/Private/ABTrackPowersnapHologram.cpp)
---

# Appendix C — Engine verification

These check whether the engine permits the design, as distinct from the per-section **Verify**
items that check whether the mod implements it. A failed check is an implementation defect or an
asset correction, not permission to drop a specified behaviour silently.

**Resolved by in-game testing:**

- The two-phase latch model and its nudge semantics.
- That one invalid latched auto-connect blocks the whole placement.
- The four build-mode names.
- That vanilla beams share no hold-to-replace swap group.
- That a beam builds flush with the Designer boundary.
- That surface patterns are free.

### C.1 Asset scale and collision

Verified in game on the generated meshes:

- Rail body 0.5 m square; Junction housing one 1 m cube within its stacking cell.
- Junction face socket 0.5 m × 0.07 m and its four face strips, moving no snap plane.
- Outlet mounting pad within the Rail's 0.5 m profile in both host modes.
- Terminal-state cues legible at build distance and subordinate at "factory scaffolding"
  distance, on all four buildables and against every supported swatch.
- Selection collision not blocking adjacent lanes.
- A Cap sandwiched between two solid faces behaving acceptably when unreachable.

Any change to the 1 m lane grid or the wall-lane offset requires a design revision, because it
affects blueprint compatibility.

### C.2 Engine integration

**Resolved by probe testing in the editor and the game:**

- **Hoverpack acquisition — the former BLOCKING item.** A hidden power connection is acquired by
  the Hoverpack exactly as a visible one is, and refuses Power Lines at the same time. §5.4's
  Outlet-only fallback is not needed, and §2.1 stands.
- A Coupling does not consume the Outlet's wire budget, and no cap on hidden connections was
  reached — invariant 7 holds in the engine, not only on paper.
- Couplings survive save and reload, including the internal edges between one buildable's own
  connection components, and `AddHiddenConnection` is idempotent.
- `ConfigureComponents` does not run on the blueprint placement path (B.2).
- Vanilla's blueprint open-connection machinery accepts new connection types and received one as
  recently as 1.2's vehicle paths (B.3).

**Resolved by in-game testing of the Rail's construction path:**

- **Lightweight behaviour is only `AFGBuildableFactoryBuildingLightweight`'s constructor defaults.**
  A subclass that clears `mCanContainLightweightInstances` and
  `mManagedByLightweightBuildableSubsystem` stays a real actor, and the engine never asks it for
  instance data. §14's non-lightweight requirement and `AFGBuildableBeam` are compatible.
- **The vanilla length chain needs nothing from us.** `AFGBeamHologram::ConfigureActor` pushes its
  private `mCurrentLength` through the public `SetLength`, confirmed at six different player-dragged
  lengths. `mLength` is `SaveGame, Replicated`.
- **The beam axis is the actor's local X** (B.2).
- **The build-menu readout is `mUsesDistanceForZooping` on the building descriptor** (B.2), not a
  cost path.

**Resolved by measurement during the artwork and the review:**

- **The Rail had no renderer** under the instance-data model; it renders through its own named
  `UStaticMeshComponent`, bound by name in the hologram (B.2). Generated meshes carry collision
  (aligned boxes), named material slots and a state material per slot.
- **Custom primitive data is vanilla's channel**, not the mod's: index 6 is written by vanilla's
  power hook regardless, so cue states are material instances (B.2).
- **Vanilla's attachment-point pipeline places the mod's holograms exactly where §6.1's geometry
  would** — 0.0 uu, 0.0° over 842 frames on every Junction face — and `AFGBeamHologram` takes a
  private path for a beam target that the generic pipeline never sees (B.2).
- **Vanilla's floor rule under an inserted cube reads the Rail's body** and refuses it as too
  steep; an attached cube stands on no floor (§7.2, §7.3).
- **`SetupBuildableComponent` returns null for a connection-component template**; the blueprint
  hologram's link-icon stand-in is made with `SetupComponent`.
- **A hologram virtual called at `BeginPlay` runs every other mod's hook on it**: `GetNudgeDistance`
  from a bridge child with no player crashed under another mod's hook. Vanilla's answer is recorded,
  not re-asked.

**No engine-integration item remains open** for single-player. Multiplayer (host, client, dedicated
server, late join) is serialized but unmeasured.

### C.3 Toolchain

Satisfactory 1.2; SML 3.12.x and the SML Starter Project; the Satisfactory-compatible Unreal Engine
5.6.1-CSS toolchain; Visual Studio 2022; Alpakit Dev for iteration and Alpakit Release for
distribution.

The meshes, their materials and the icons are generated by re-runnable editor scripts (Geometry
Script for the meshes), read back after every write; `docs/BUILDING.md` gives the order.

---
