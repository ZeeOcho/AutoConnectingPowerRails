// Auto-Connecting Power Rails — the Junction hologram.
//
// A 1 m cube needs more than AFGBuildableHologram already does, for one specific reason:
//
//   A Junction has to land on the same lattice the Rail's terminals land on, and vanilla's grid is
//   half a cell out of phase with it.
//
// Rail terminals sit with X/Y on multiples of 100 and Z at the foundation's surface plus the Rail's
// 50; vanilla's grid puts a cube's centre on whole metres. Such a Junction cannot couple to
// anything, and nudging cannot fix it either: vanilla's fine nudge is 20 uu, so from a whole-metre
// grid you can reach 20, 40, 60 — never the 50 that the geometry needs.
//
// The two rules, both from Appendix B.2 — "offset the origin 0.5 m from a supporting surface, and
// apply the wall-local grid on the half-cell phase rather than the wall-centre phase":
//
//   1. Surface offset. The centre sits mSurfaceOffset from the surface, measured from the hit point
//      rather than added to wherever vanilla snapped — the same idempotent form the Rail uses, and
//      for the same reason: a wall and a foundation snap to different planes, so one constant added
//      to "wherever vanilla put it" cannot be right for both. On a foundation at 5100 this puts the
//      centre at 5150, which is the Rail's terminal plane. The Z half of the mismatch is gone by
//      construction rather than by a matching constant.
//
//   2. Half-cell phase on the axes the surface does not constrain. A face centre is mHalfExtent from
//      the centre, so for a face to land on a whole-metre Rail terminal the centre must sit half a
//      cell off it. The phase is therefore derived from the cube — fmod( mHalfExtent, pitch ) — and
//      not written down as 50 somewhere. Re-measure the cube against the eventual art and the phase
//      follows; make it a 2 m cube and the phase correctly becomes zero.
//
// The one case this cannot get right alone is closed from the Rail's side (§11 lane lattice). The
// mating axis and the lateral axis want opposite phases, and which axis is which depends on the
// Rail, so no static Junction phase could fix it; this keeps the half-cell phase on both axes
// (B.2's rule), and in the Default build mode a free Rail's start goes to the aimed cell's centre
// and, once the drag picks its axis, to that cell's edge — exactly this Junction's face centre. See
// AACPRRailHologram::DriveLane.
//
// ==========================================================================================
// §7.2 terminal-snapped, and why it adds no coupling code at all
// ==========================================================================================
//
// Free placement landing exactly on a Rail terminal is the only test that §6.1's coupling is driven
// by geometry rather than by the snapping that produced it, and the terminal snap (vanilla's
// attachment pipeline) creates no Coupling. It places the cube so that one face lands on the target
// terminal's exact cell with the exact opposing outward axis, and FACPRCoupling's coincident-pair
// rule does the rest at registration, unchanged and unaware that any snapping happened.
//
// §6.1 lists two routes to a Coupling — an accepted terminal snap, and an accepted coincident pair.
// This makes the first a special case of the second. There is therefore one coupling mechanism in
// this mod, not two that must be kept in agreement, and §7.2 cannot drift from §6.1 because it holds
// no copy of the rule to drift with.
//
// The one honest divergence: §5.1 says the Coupling forms "in the same transaction" as the snap, and
// this forms it on the next tick. Nothing observable differs — a failed construction produces no
// actor and therefore no Coupling either way — but §10.6's aggregate blueprint transaction needs
// the distinction, and that is where it gets paid for rather than here.

#pragma once

#include "CoreMinimal.h"
#include "ACPRLog.h"
#include "ACPRHologramCommon.h"
#include "ACPRSpec.h"
#include "Hologram/FGBuildableHologram.h"
#include "ACPRJunctionHologram.generated.h"

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API AACPRJunctionHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	AACPRJunctionHologram();

	virtual void BeginPlay() override;

	/** Takes §12's highlight off the aimed host. See AACPRRailHologram::Destroyed. */
	virtual void Destroyed() override;

	/** The roll is a change vanilla should know about, and the reset clears it. */
	virtual bool IsChanged() const override;
	virtual bool HandleReset() override;

	/**
	 * §7.2's terminal snap is vanilla's attachment-point pipeline. This is the one rule of ours in
	 * it — §10.1, a host in another build space is not offered — and the only override; selection
	 * and the mating transform are vanilla's (equal to PlaceOnTerminal's).
	 */
	virtual void FilterAttachmentPoints( TArray< const FFGAttachmentPoint* >& Points, class AFGBuildable* pBuildable, const FHitResult& HitResult ) const override;

	/**
	 * The placement hook. For a single-step buildable hologram this is what the build gun calls every
	 * frame, so it is where the two rules above are applied.
	 *
	 * PostHologramPlacement is overridden as well and applies the same correction. That is not
	 * belt-and-braces for its own sake: on the Rail, SetHologramLocationAndRotation never fires
	 * during step 1 and PostHologramPlacement is the only hook that runs. A Junction is a different
	 * hologram class on a different path, and rather than assume which of the two fires here, both
	 * do — the correction is idempotent, so the cost of being wrong about it is one redundant call,
	 * and the log names the hook that actually moved something.
	 */
	virtual void SetHologramLocationAndRotation( const FHitResult& hitResult ) override;
	virtual void PostHologramPlacement( const FHitResult& hitResult, bool callForChildren ) override;

	/** Clears the snap record at the top of each frame's placement pass. See mSnappedThisFrame. */
	virtual void PreHologramPlacement( const FHitResult& hitResult, bool callForChildren ) override;

	/**
	 * Not a behaviour change — a record of one, and the guard that keeps the rules above from
	 * overwriting it.
	 *
	 * FGHologram.h:175: "If returning true, we assume location and snapping is applied, and no further
	 * location and rotation will be updated this frame by the build gun." That suppresses
	 * SetHologramLocationAndRotation — and nothing else. PostHologramPlacement still runs, so a
	 * correction applied from there would drag a successfully snapped Junction back onto our lattice,
	 * up to half a metre sideways, every frame.
	 *
	 * mSnappedBuilding is NOT the guard for this. FGBuildableHologram.h documents it as also being set
	 * for ordinary foundation grid alignment, so guarding on it would switch the rules off exactly
	 * where they are needed most. The return value of the snap attempt is the only thing that means
	 * "vanilla has already decided where this goes".
	 */
	virtual bool TrySnapToActor( const FHitResult& hitResult ) override;

	/**
	 * §7.2: "The snap point is authoritative — no nudge."
	 *
	 * The two placement modes want opposite things from the nudge, and this is the switch. A
	 * standalone Junction needs it (that is what makes the half-cell phase reachable); a snapped one
	 * must not have it, because the snap point is derived from a terminal that is already exact and
	 * any offset from it would break the coincidence that forms the Coupling.
	 */
	virtual bool CanNudgeHologram() const override;

	/**
	 * Rotation is ours, not vanilla's — and every reason is a measurement, not a preference.
	 *
	 *   The step. Vanilla's scroll accumulator advances 10° per notch (the log walks
	 *   roll = 0, -10, -20 ... -90) while GetRotationStep separately reports 90, so those are two
	 *   different numbers from two different places: GetRotationStep does not drive this one, and no
	 *   threshold on it can read the mode.
	 *
	 *   The axis. Standalone on a wall, vanilla rotates about world Z — the cube's top and bottom
	 *   faces stay put while the four in the wall plane sweep out of it. That is the one axis of the
	 *   three that is no use on a wall.
	 *
	 *   The granularity. 10° on a cube puts all six faces off every axis at once. The useful set is
	 *   multiples of 45.
	 *
	 * So this keeps its own accumulator and does not call Super — vanilla's would keep counting in 10°
	 * underneath ours and resurface the moment anything else read it.
	 *
	 * A 90° roll of an axis-aligned cube is the identity — it maps the four side faces onto each
	 * other — so a 90° coarse step would be a no-op. mRollStepDegrees defaults to 45, and the fine
	 * modifier is where the finer step goes.
	 */
	virtual void ScrollRotate( int32 delta, int32 step ) override;

	/**
	 * §7.2's roll: 45° per notch, about the mating axis when snapped and about the surface normal
	 * when standalone. A whole fraction of 45 is the other sensible family; anything else takes all
	 * six faces off every axis at once.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mRollStepDegrees = 45.0f;

	/**
	 * §7.2's fine roll, held on AFGHologram::mSnapToGuideLines (FGHologram.h:698) — the same flag
	 * vanilla's Ctrl sets, handed to the hologram by the build gun rather than encoded in any value.
	 *
	 * 5 is an exact divisor of 45, so nine fine notches make one coarse notch and fine adjustment
	 * always returns to the coarse positions. Any other whole divisor — 22.5, 15, 9 — shares that
	 * property; 5 is the finest of them, and fine mode is for settling onto a target rather than
	 * travelling to one.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mRollStepFineDegrees = 5.0f;

	/**
	 * How closely a terminal's outward axis must agree with the surface the player hit for that
	 * terminal to be treated as the one being aimed at. 0.5 is 60°, which separates "this face" from
	 * "a neighbouring face" without being fussy about where on the face the crosshair sits.
	 *
	 * Zero or less disables the rule and falls back to distance alone, which produces runs of
	 * REFUSED (ambiguous) lines whenever the aim crosses a Junction's edges.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mFaceAlignmentMinimum = 0.5f;

	/**
	 * §7.3: "Aiming within 1.5 m of a Rail end switches the hologram to terminal-snapped mode."
	 *
	 * That sentence is about the insertion mode, but the number is the same question — how near an
	 * end counts as aiming at it — so the terminal snap takes it from there rather than inventing one.
	 *
	 * It also has to be at least the Junction's own half diagonal, or aiming at one face of an
	 * existing Junction would fail to reach that face's own terminal. 150 uu clears 87 comfortably.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mTerminalSnapRange = ACPRSpec::TerminalSnapRange;

	/**
	 * Invariant 8: "Ambiguity produces no coupling." Two candidate terminals within this of each
	 * other are a tie, and a tie refuses rather than picking one.
	 *
	 * Not an epsilon on a geometric test — B.3 forbids those in the resolver, and this is not the
	 * resolver. It is a tolerance on "which of these did the player mean", where the honest answer
	 * to a tie is that nobody knows.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mAmbiguityTolerance = 5.0f;

	/**
	 * §7.4: "rejected where it would ... sit too close to another Junction or Outlet." The distance
	 * is measured centre to centre from this cell's face, so the default leaves a clear metre — one
	 * lane pitch (§11) — between two inserted cells.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mNeighbourClearance = ACPRSpec::NeighbourClearance;

	/**
	 * §11: "Rail body | 0.5 m x 0.5 m square profile", so a Rail's body reaches a quarter metre off
	 * its centreline. Used only by §7.4's crossing test, which errs toward refusing.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mRailHalfWidth = ACPRSpec::RailHalfWidth;

	/**
	 * How far §7.4's crossing sweep looks, and it is a whole Rail length plus a cell rather than a
	 * neighbourhood radius. A 40 m Rail crossing this cell can have BOTH its terminals far away, and
	 * the registry holds terminals — but one of them is always within one Rail length of any point
	 * the body reaches, so this is the smallest radius that cannot miss one.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mCrossingSearchRadius = ACPRSpec::CrossingSearchRadius;

	/**
	 * §12's red feedback for an insertion §7.4 refuses. UFGCDInvalidAimLocation because it is the
	 * class the Cap, the Outlet and this hologram all already load; see mOccupiedTerminalDisqualifier.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	TSubclassOf< class UFGConstructDisqualifier > mInsertionDisqualifier;

	/**
	 * §7.3's named disqualifier for an occupied end. See CheckValidPlacement.
	 *
	 * Set in the constructor to a real class for the reason AACPRCapHologram's equivalent records:
	 * a null here would make the refusal a no-op, and a rule that switches itself off when its
	 * message is missing is not a rule.
	 *
	 * It defaults to UFGCDInvalidAimLocation, which is not the best wording, on purpose.
	 * UFGCDInvalidPlacement says what this failure actually is — the player is not being asked to
	 * snap to anything, the thing they aimed at is taken — but it is not a symbol this module imports
	 * anywhere else, and an unresolved vanilla import takes the game down at load with
	 * GetLastError=127. So the C++ default is the class the Cap and the Outlet already load, and
	 * pointing this field at UFGCDInvalidPlacement in the editor gets the better message with no DLL
	 * import and no rebuild, which is what the field is for.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	TSubclassOf< class UFGConstructDisqualifier > mOccupiedTerminalDisqualifier;

	/**
	 * A Junction is not a floor-mounted building, so it does not ask for a floor.
	 *
	 * CheckValidFloor rejects a surface steeper than mMaxPlacementFloorAngle, which is the correct
	 * rule for a constructor or a smelter and the wrong rule for something §7.1 describes as freely
	 * positioned. A wall normal has Z = 0; no default angle passes that, so aiming at a wall would
	 * give "invalid target" and no `place(...)` line at all.
	 *
	 * Overridden rather than fixed by raising mMaxPlacementFloorAngle to 90, because the angle is a
	 * threshold on a rule that should not apply at all — and 90 leaves cos(90°) ≈ -4.4e-8 as the
	 * bound, which is a float-precision coin toss on exactly the surface we care about.
	 *
	 * It relaxes the angle rule only. Super is still called whenever the surface would have passed
	 * the angle test, because the base refuses a floor that is "too steep, ANOTHER BUILDING etc" and
	 * signals that by adding a construct disqualifier rather than by returning anything — so skipping
	 * it outright would silently drop whichever disqualifier vanilla raised, not merely the angle
	 * one. Floors keep every vanilla rule; walls lose exactly one.
	 *
	 * It logs the geometry it declined to judge, including what vanilla's threshold would have said,
	 * so if a wall still fails the log names the other gate.
	 */
	virtual void CheckValidFloor() override;

	/**
	 * The gate that refuses walls. Measured, with super= the base's verdict:
	 *
	 *   Build_Foundation_8x1_01   super=1   placement continues
	 *   Build_PowerRail_C         super=1   placement continues
	 *   Build_Wall_8x4_01 (side)  super=0   nothing follows; the player sees "invalid target"
	 *   Build_Wall_8x4_01 (top)   super=0   normal Z = 1, so this is not about steepness
	 *   Build_PowerRailJunction   super=0   vanilla refuses our own Junction too
	 *
	 * That settles two things a comment could not. First the polarity: the base's @return says "true
	 * if the hit result is INVALID" and the measurements say the opposite, so the name is right and
	 * the comment is stale. Second the mechanism: the wall's top face is refused as firmly as its
	 * side, so whatever the base's rule is, it is about which buildings may be aimed at and not about
	 * the surface angle. CheckValidFloor is a real second gate, but it is not this symptom.
	 *
	 * The override reverses the refusal in exactly one case — a blocking hit on an AFGBuildable —
	 * because §7.1 makes a Junction freely positioned and that rule is the one thing standing between
	 * it and a wall. Aiming at nothing still produces no blocking hit and stays invalid, anything the
	 * base rejects for other reasons stays rejected, and every later gate (CheckValidPlacement,
	 * clearance, CheckValidFloor) still runs on the frames this lets through.
	 */
	virtual bool IsValidHitResult( const FHitResult& hitResult ) const override;

	/**
	 * §7.3's last two lines.
	 *
	 * §7.3: "Where that end terminal is coupled or capped, neither mode is available, and the
	 * disqualifier must name the occupied terminal rather than reporting proximity to the end."
	 * §15 says the same as a row: "Junction | Rail body within 1.5 m of a coupled or capped end |
	 * Invalid; disqualifier names the terminal."
	 *
	 * Without this rule the hologram would simply fall back to standalone placement — the cube
	 * "slipping off the face" by mSurfaceOffset — and a capped Rail end would look exactly like an
	 * uncapped one because nothing said otherwise.
	 *
	 * Proximity, not face alignment, because §7.3 writes the rule as "within 1.5 m of an end". A
	 * player aiming at a Rail's SIDE a hand's width from a capped end is in the same situation as
	 * one aiming at the end cap, and getting a standalone placement there would be the same silence
	 * in a different pose.
	 */
	virtual void CheckValidPlacement() override;

	/**
	 * §7.3's inline insertion, and the one place in this mod that builds more than one actor.
	 *
	 * "Construction atomically replaces the target with two child Rails coupled to the aligned
	 * terminals." AFGHologram::Construct is the seam that allows it — FGHologram.h:331 takes
	 * `TArray< AActor* >& out_children` and returns the main actor — and AACPRRail::Split does the
	 * replacement, modelled on AFGBuildableConveyorBelt::Split, which is vanilla's own one-into-three
	 * case (a belt receiving a splitter).
	 *
	 * Not GetUpgradedActor. That seam replaces one actor with one actor; see AACPRRail::Split's
	 * comment for the evidence, which is the interface's own wording plus every vanilla implementor.
	 * This class deliberately does not override it, so a Junction is never an "upgrade" of anything.
	 *
	 * Coupling is still §6.1's. Split places the children so their inner terminals land exactly on
	 * the Junction's two consumed faces, and the coincident-pair rule joins all four a tick later:
	 * place exactly, and let the one mechanism do the rest.
	 */
	virtual AActor* Construct( TArray< AActor* >& out_children,
	                           FNetConstructionID constructionID ) override;

	/**
	 * §7.4's crossing rule on vanilla's own overlap event. An overlap of the cell with a Rail, Junction
	 * or Outlet that is not the host being split or mated to is upgraded from whatever vanilla decided
	 * (soft, most likely) to mInsertionDisqualifier. FGHologram.h:552. The registry sweep in
	 * FindInsertionRejection remains only for a hologram without clearance data.
	 */
	virtual TSubclassOf< class UFGConstructDisqualifier > GetConstructDisqualifierFromClearanceOverlap(
		const EClearanceOverlapResult& overlapResult, AActor* otherActor ) const override;

	/**
	 * The same lever the Rail needs, for the same reason: AFGHologram::GetNudgeDistance() returns a
	 * hardcoded 100, or 20 with the fine modifier, and reads mDefaultNudgeDistance never.
	 *
	 * The Junction needs it more than the Rail does. 20 uu cannot reach a 50 uu correction from a
	 * whole-metre start at all; 25 reaches it in two presses and reaches every position a nudged Rail
	 * can put a terminal at. Matching the Rail's pair is the point — a Junction must be able to meet
	 * a Rail wherever the Rail can be put.
	 */
	virtual float GetNudgeDistance() const override;

	/** One press, one lane. Vanilla's coarse value, kept deliberately. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mNudgeDistanceCoarse = ACPRSpec::NudgeCoarse;

	/** Two presses to the half cell, and fine enough to meet a Rail nudged to any quarter metre. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mNudgeDistanceFine = ACPRSpec::NudgeFine;

	/**
	 * B.2's "0.5 m from a supporting surface". The same value and the same measured-not-added form as
	 * AACPRRailHologram::mSurfaceOffset, which is what makes a Junction centre and a Rail centre sit
	 * on the same plane above the same foundation without either knowing about the other.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mSurfaceOffset = ACPRSpec::HalfCell;

	/**
	 * The lattice the Rail's endpoints live on. 1 m, from §11's lane pitch.
	 *
	 * The PHASE is not here on purpose — it is fmod( the cube's half extent, this ), so the cube and
	 * the grid cannot drift apart. Zero or less disables the grid rule entirely and leaves only the
	 * surface offset, which is the escape hatch if this turns out to fight something.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mGridPitch = ACPRSpec::CellSize;

private:
	/** One Verbose line per adopted vanilla snap target. */
	void LogSnapAdopted( const class UACPRTerminalComponent* terminal );

	/**
	 * The placement half of §7.2, split out so the locked path can re-apply it without re-choosing a
	 * target. Locking freezes WHICH terminal, not the transform — scrolling a locked snapped Junction
	 * still rolls it about the mating axis, which is what §7.2 asks for.
	 */
	bool PlaceOnTerminal( class UACPRTerminalComponent* target );

	/** The build class's mHalfExtent, or the default if the build class is wrong. */
	float GetHalfExtent() const;

	/**
	 * Writes the accumulated roll onto the hologram, absolutely, from whichever base the current mode
	 * defines. Called from every hook that could have overwritten it.
	 *
	 * The reason it is called more than once: TrySnapToActor sets the rotation and vanilla can set it
	 * back afterwards, because FGHologram.h:172's "no further location and rotation will be updated
	 * this frame" covers SetHologramLocationAndRotation only. Writing last is the remedy, and being
	 * absolute is what makes writing repeatedly safe.
	 */
	void ApplyRoll();

	/** Remembers what ApplyRoll last wrote. */
	void RecordAppliedRotation();

	/** Surface offset + half-cell phase, idempotent, skipped while locked. See the .cpp. */
	void ApplyPlacementOffsets( const FHitResult& hitResult, const TCHAR* hook );

	/** fmod( AACPRJunction::mHalfExtent, mGridPitch ) — the offset a face centre needs from a cell. */
	float GetGridPhase() const;

	/**
	 * Captures the buildable's own mesh component, once, at BeginPlay.
	 *
	 * Locking the hologram spawns the nudge gizmo, which is built from ordinary static meshes — a
	 * cylinder per arrow shaft, a cone per head. A per-frame search for "plain static mesh
	 * components" would find those too and scale each of them up to a 1 m cube: a giant pipe segment
	 * and two cones inside the preview, flickering as the gizmo rebuilds. The set that belongs to the
	 * buildable is fixed at construction, so it is captured then, not re-derived.
	 */
	void BindPreviewMesh();

	/**
	 * Sizes the preview cube through AACPRJunction::FitMeshComponentToCube — the same function the
	 * built actor uses, so the thing you aim and the thing you get cannot be different sizes. Only
	 * ever touches the component BindPreviewMesh captured.
	 */
	void FitPreviewMesh();

	/**
	 * §12. Shows every eligible terminal on the aimed host, with this frame's snap target marked.
	 *
	 * Driven from PostHologramPlacement rather than SetHologramLocationAndRotation, because the
	 * Junction is the one hologram where that function does not run every frame: FGHologram.h:172
	 * says a TrySnapToActor returning true suppresses it, which is precisely the terminal-snapped
	 * frame the markers matter most on. Post runs either way.
	 */
	void UpdateTerminalHighlight( const FHitResult& hitResult );

	/**
	 * §7.3: "Snaps to one specifically targeted Rail body; the previewed host is authoritative and
	 * nearby Rails are irrelevant."
	 *
	 * Returns false outside the insertable span, rather than clamping into it. The span's near bound
	 * is one half-extent plus one minimum child — 1.5 m exactly, which is also §7.3's "aiming within
	 * 1.5 m of a Rail end switches the hologram to terminal-snapped mode". So the two modes meet with
	 * no gap and no overlap, and the case §7.3 calls invalid — an end that is coupled or capped, where
	 * "neither mode is available" — falls through both and is refused by CheckValidPlacement. Clamping
	 * instead would slide the cube 1.5 m up the Rail from wherever the player aimed, a silent
	 * fallback.
	 */
	bool TryInsertIntoRail( const FHitResult& hitResult );

	/** Places the cube on the host's axis at mInsertCentre, absolutely, reading mRollDegrees. */
	bool PlaceOnRail();

	/** §7.3's insertable span of `host`: [half + minimum child, length − half − minimum child]. False when empty. */
	bool InsertableSpan( const class AACPRRail& host, double& out_low, double& out_high ) const;

	/**
	 * A standalone cell centred on a Rail's axis, within that Rail's insertable span, is an insertion
	 * of that Rail — the aim slid off the Rail onto the surface beside it, and §7.1's lattice put the
	 * cube back in the cell the insertion had. Converts only frames §7.4 refuses anyway. Exactly one
	 * Rail may qualify; two is ambiguity and the frame stays a §7.4 refusal.
	 */
	bool AdoptCellInsertion();

	/** §7.4's insertion rejections (children, attachments, neighbours), then the crossing rule. Reason, or null. */
	const TCHAR* FindInsertionRejection() const;

	/**
	 * §7.4's crossing rule for any placement frame: an unrelated Rail body or terminal in the cell.
	 * `related` is the host this frame is attached to — being split, or mated to — whose body or
	 * terminal meets the cell by design; null on a standalone frame. The registry sweep; vanilla's
	 * overlap event answers it instead on a hologram with clearance data.
	 */
	const TCHAR* FindCrossingRejection( const AActor* related ) const;

	/** One line per (mode, reason, cell) for a §7.4 crossing refusal. */
	void LogCrossing( const TCHAR* mode, const TCHAR* reason ) const;

	/** One line per distinct (verdict, host, 50 uu bucket). */
	void LogInsertion( const TCHAR* verdict, const TCHAR* detail ) const;

	/** §7.3's refusal, named. One line per distinct (host, terminal) pair. */
	void LogOccupiedEnd( const AActor* hitActor,
	                     const class UACPRTerminalComponent* terminal,
	                     double distance ) const;

	void LogPlacement( const FHitResult& hitResult, const TCHAR* hook );

	mutable FACPRLogGate mPlacementLog{ 16 };

	/** False when the surface was not axis-aligned and only the offset was applied. In the log. */
	bool mLastGridApplied = false;

	/**
	 * What TrySnapToActor returned this frame.
	 *
	 * Cleared in PreHologramPlacement. FGHologram.h:188/193 documents Pre as running "before all the
	 * placement logic" and Post as running after, so the order is Pre -> TrySnapToActor -> Post:
	 * clearing in Pre happens before the write, not after it. Leaving it uncleared would be unsafe —
	 * on any frame where Post runs and TrySnapToActor does not, a true left over from an earlier
	 * frame would switch both placement rules off silently, with no line in the log saying why the
	 * Junction stopped moving.
	 */
	bool mSnappedThisFrame = false;

	/** One "vanilla snapped this" note per hologram. A local static would make it one per process. */
	bool mSnapReported = false;

	/**
	 * The last aim IsValidHitResult saw — the one hook that still runs when a surface is rejected, and
	 * the one vanilla calls every frame with the raw hit BEFORE the placement pass, so by the time
	 * CheckValidPlacement asks this is this frame's aim (§7.3's 1.5 m is measured from its impact
	 * point) and CheckValidFloor, which takes no arguments, has a normal to judge. Diagnostic and
	 * placement state both; mutable because IsValidHitResult is const (ACPRHologramCommon.h).
	 */
	mutable FACPRAimRecord mAim;

	/** What the occupied-end scan found this frame, for the disqualifier and the log. */
	/**
	 * §7.3's insertion target and where on it the cube sits, in Rail-local uu from terminal A.
	 *
	 * Rail-local on purpose, and §7.3 says why: it is "a coordinate stable under Rail reversal,
	 * save/load, blueprint rotation and later splitting". A world position would be none of those.
	 *
	 * Both are CustomSerialization — part of vanilla's construct message (FGHologram.h:342), so a
	 * server building from a client's hologram splits the same Rail at the same place. A TObjectPtr
	 * rather than a weak pointer because that is what the message serializer carries; Pre clears it every
	 * frame, so it cannot outlive its Rail by more than one placement pass. Untested in multiplayer.
	 */
	UPROPERTY( CustomSerialization )
	TObjectPtr< class AACPRRail > mInsertHost = nullptr;

	UPROPERTY( CustomSerialization )
	double mInsertCentre = 0.0;

	mutable FACPRLogGate mInsertLog{ 24 };
	mutable FACPRLogGate mCrossingLog{ 24 };

	mutable TWeakObjectPtr< class UACPRTerminalComponent > mOccupiedEnd;
	mutable FACPRLogGate mOccupiedLog{ 16 };
	mutable FACPRLogGate mValidityLog{ 24 };

	mutable FACPRLogGate mFloorLog{ 12 };

	/** The preview fit is re-applied every placement frame; the line about it is printed once. */
	bool mPreviewFitLogged = false;

	/** BindPreviewMesh is re-callable per frame, so its failure warning is printed once. */
	bool mPreviewBindWarned = false;

	/**
	 * The terminal this hologram is currently mated to, or null in standalone mode.
	 *
	 * Per-frame, cleared in PreHologramPlacement alongside mSnappedThisFrame. It is the mode switch
	 * for the nudge and for the placement rules, and it is deliberately NOT carried into
	 * construction: the Coupling is formed from geometry by FACPRCoupling, so nothing downstream
	 * needs to be told which terminal was aimed at. Set from vanilla's attachment snap (TrySnapToActor).
	 */
	TWeakObjectPtr< class UACPRTerminalComponent > mSnappedTerminal;

	mutable FACPRLogGate mSnapLog{ 24 };

	/** §7.2's roll, in degrees, accumulated by ScrollRotate and applied by ApplyRoll. */
	double mRollDegrees = 0.0;

	/**
	 * Vanilla's surface-derived orientation and the surface it came from, captured each frame in
	 * SetHologramLocationAndRotation BEFORE our roll goes on.
	 *
	 * Storing the base is what lets ApplyRoll be absolute. Composing against the CURRENT rotation
	 * instead would add the roll again on every hook that calls it, and the cube would spin up on its
	 * own without anybody touching the scroll wheel.
	 */
	FQuat mBaseRotation = FQuat::Identity;
	FVector mBaseNormal = FVector::ZeroVector;

	int32 mRollLogBudget = 24;

	/** What ApplyRoll last wrote. */
	FQuat mLastAppliedRotation = FQuat::Identity;

	/**
	 * The buildable's cube, captured at BeginPlay. Weak because the hologram owns its lifetime and
	 * may drop it; the fit simply stops rather than touching whatever took its place.
	 */
	TWeakObjectPtr< class UStaticMeshComponent > mPreviewMesh;

	/** The host this hologram last wrote §12's highlight onto, so it can be cleared. */
	TWeakObjectPtr< AActor > mHighlightHost;

	mutable float mLastLoggedNudgeDistance = -1.0f;

	// ---------------------------------------------------------------------------------------
	// §10.1 — the build space this placement is in. See AACPRRailHologram's identical block.
	// ---------------------------------------------------------------------------------------

	/** AFGHologram::mBlueprintDesigner (FGHologram.h:776), read as our own protected field. */
	class AFGBuildableBlueprintDesigner* GetPlacementDesigner() const;

	FString mLastSpaceLog;
	TWeakObjectPtr< class AFGBuildableBlueprintDesigner > mLastPlacementSpace;
	bool mPlacementSpaceSeen = false;
};
