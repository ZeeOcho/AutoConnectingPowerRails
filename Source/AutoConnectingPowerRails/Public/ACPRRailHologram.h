// Auto-Connecting Power Rails — the Power Rail hologram.
//
// §5.1 says the Rail "inherits the Painted Beam's two-stage Build Gun workflow and placement
// behaviour", and AFGBeamHologram implements all of it — TrySnapToActor, SetHologramLocationAndRotation,
// DoMultiStepPlacement, the diagonal and freeform build modes, CanIntersectWithDesigner, and the mesh
// stretch. Every one of those works unmodified, provided the buildable adds no mesh component of its
// own. The build-gun cost readout never calls GetBaseCostMultiplier / GetBaseCost / GetCost; it is
// mUsesDistanceForZooping on the descriptor.
//
// What this class adds is the §11 placement offsets, the nudge distance, terminal snapping (§5.1's
// "A Rail may begin at an open terminal") and the Appendix C.2 guideline hooks.
//
// On the nudge: AFGBeamHologram overrides none of the nudge interface, so a beam's nudge is resolved
// by AFGBuildableHologram. mDefaultNudgeDistance is a dead field on that path: it arrives on the
// instance correctly and AFGHologram::GetNudgeDistance() never reads it, returning a hardcoded 100 (or
// 20 with the fine modifier). Overriding that virtual is the whole mechanism. mGridSnapSize is not
// involved — it reads 100 while the offset walks 25 / 50 / 75 / 100 — and it also governs placement
// snapping, so it is left alone.
//
// There is no Tick override. A bridge's re-evaluation is the manager's, run from vanilla's own update
// with a gate on the body's real transform (FACPRBlueprintTerminalManager::EvaluateAll). What runs per
// frame is vanilla's own placement cascade, and the overrides that must run inside it.

#pragma once

#include "CoreMinimal.h"
#include "ACPRLog.h"
#include "ACPRHologramCommon.h"
#include "ACPRSpec.h"
#include "Hologram/FGBeamHologram.h"
#include "ACPRRailHologram.generated.h"

/**
 * §10.6 — everything the blueprint manager tells a generated bridge Rail, in one value.
 *
 * Plain C++, not a USTRUCT: nothing here is reflected, saved or replicated, and keeping it out of UHT
 * keeps the hologram's reflected shape unchanged — no editor restart for it.
 */
struct FACPRBridgeSpec
{
	/** The BP terminal's snap point: terminal A of the generated Rail sits here. */
	FVector Start = FVector::ZeroVector;

	/** The BP terminal's outward axis — the Rail's local +X, invariant 9's inherited direction. */
	FVector Outward = FVector::ForwardVector;

	/** The BP terminal frame's Z. §5.2: the BP-side roll (or a Junction face's zero roll) carries across. */
	FVector Up = FVector::UpVector;

	/** Exact constructed length, uu. Terminal B lands at Start + Outward * Length. */
	double Length = 0.0;

	/** The latched (or proposed) OB terminal. Weak: the world owns it. */
	TWeakObjectPtr< class UACPRTerminalComponent > FarTerminal;

	/** The placement's build space (§10.1). Null is the world. */
	class AFGBuildableBlueprintDesigner* Space = nullptr;

	/** §10.4's geometry verdict: true when the latched pair has stopped holding (the manager keeps the reason). */
	bool Faulted = false;

	/** §10.6: "inherits BP-side customization". Copied onto the hologram so ConfigureActor carries it. */
	const struct FFactoryCustomizationData* Customization = nullptr;

};

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API AACPRRailHologram : public AFGBeamHologram
{
	GENERATED_BODY()

public:
	AACPRRailHologram();

	virtual void BeginPlay() override;

	/**
	 * The hologram's end, whatever ended it — a cancelled build, a swapped recipe, the build gun
	 * unequipped. §12's highlight lives on the aimed host's materials, not on this actor, so it has to be
	 * taken down here or it outlives the aim that put it there. AFGHologram::Destroyed, FGHologram.h:134.
	 */
	virtual void Destroyed() override;

	/**
	 * The preview's LENGTH is written here, not on the buildable, because the two are fed differently:
	 * the built Rail fits its body in BeginPlay, the hologram's copy of that component is vanilla's to
	 * position and rotate every frame (nothing writes its scale), so the length scale is ours to write
	 * after Super:: in each hook that places.
	 */
	virtual void SetHologramLocationAndRotation( const FHitResult& hitResult ) override;

	/**
	 * Vanilla's own "the transform changed after the initial move" event (FGHologram.h:530). For a
	 * bridge it is the one place a move by something other than ApplyBridgeSpec would be visible. A
	 * bridge's place is written once, from the spec; if this event finds it off that frame, it says so
	 * at Warning, budgeted, and does NOT put it back — a second mover is a bug to see, not a symptom to
	 * correct.
	 */
	virtual void OnHologramTransformUpdated() override;

	/**
	 * The roll is a change vanilla should know about. IsChanged() (FGHologram.h:301) is what the build
	 * gun asks before it offers a reset; HandleReset() (:304) is the reset. A Rail rolled off zero
	 * reports changed and resets to zero; otherwise both defer to Super.
	 */
	virtual bool IsChanged() const override;
	virtual bool HandleReset() override;

	/**
	 * A Junction target is snapped by vanilla's attachment-point pipeline. This is the one rule of ours
	 * in it — §10.1, a host in another build space is not offered — and the only override; selection
	 * and the mating transform are vanilla's (measured equal to §6.1's).
	 */
	virtual void FilterAttachmentPoints( TArray< const FFGAttachmentPoint* >& Points, class AFGBuildable* pBuildable, const FHitResult& HitResult ) const override;

	/**
	 * A bridge does not take the nudge. Vanilla applies a locked hologram's nudge offset to the root AND
	 * to every child hologram through this virtual, every frame — and a bridge's base location for that
	 * arithmetic is nothing, so each frame it would be sent ~3 km to the world origin. A bridge's place
	 * is the manager's to set (ApplyBridgeTransform, from ApplyBridgeSpec), and the nudge reaches it
	 * through the parent's evaluation.
	 */
	virtual void SetHologramNudgeLocation() override;
	virtual bool DoMultiStepPlacement( bool isInputFromARelease ) override;

	/**
	 * Step 1 of placement never calls SetHologramLocationAndRotation. These two are where step 1
	 * positions the hologram, so the profile is applied there and the preview is Rail-width before the
	 * first click rather than after it.
	 */
	virtual void PreHologramPlacement( const FHitResult& hitResult, bool callForChildren ) override;

	virtual void PostHologramPlacement( const FHitResult& hitResult, bool callForChildren ) override;

	/**
	 * §5.1: "A Rail may begin at an open terminal … Snapping creates the Coupling in the same
	 * transaction."
	 *
	 * This is step 1 only. Once the anchor is latched the aim means LENGTH, not position, and a snap
	 * attempt there would drag the already-placed near end around.
	 *
	 * Like the Junction's, it creates no Coupling. It puts the near terminal on the target's exact
	 * cell with the exact opposing axis, and FACPRCoupling's coincident-pair rule does the rest a
	 * tick later — one mechanism for both buildables, and neither holds a copy of the rule.
	 */
	virtual bool TrySnapToActor( const FHitResult& hitResult ) override;

	/**
	 * AFGBuildableHologram::IsValidHitResult refuses some buildables outright — a Junction among them —
	 * and the hologram is then hidden and placement skipped, so TrySnapToActor is never even called: a
	 * gate upstream of snapping. Vanilla has a list for exactly this — AFGHologram::mValidHitClasses
	 * (FGHologram.h:663, "these classes will be considered a valid hit"), filled by AddValidHitClass
	 * (:612, "use this to add a valid hit class for this hologram in blueprints begin play") and read by
	 * IsValidHitActor (:619). BeginPlay adds the two host classes a Rail can snap to. This override is
	 * the bridge between that list and AFGBeamHologram's own IsValidHitResult (FGBeamHologram.h:33),
	 * whose body cannot be read here: it accepts a blocking hit whose actor IsValidHitActor.
	 */
	virtual bool IsValidHitResult( const FHitResult& hitResult ) const override;

	/**
	 * §5.1's no-fall-through rule, enforced in validation rather than in the far-end search.
	 *
	 * The search cannot enforce it: a capped face is not open, so it never enters the candidate set,
	 * and a search that declines simply returns false — the Rail is then placed freely, straight
	 * through the host. Validation runs every frame whether anything snapped or not, and there the
	 * rule is stated as geometry rather than as a snapping rule: **no terminal may lie strictly inside
	 * the Rail's own span.** A terminal exactly AT the far end is the snap and is fine; one behind the
	 * near end is not on the Rail; one in between is a face the Rail passes through, and §5.1 says the
	 * first one encountered blocks.
	 *
	 * That single sentence needs no test for WHY the terminal is unusable. Capped, coupled,
	 * non-opposing, or perfectly open and simply not the end the player is aiming at — a Rail that runs
	 * through a face implies a coupling that does not exist, which is the same argument §7.4 makes in
	 * the other direction for Junction insertion.
	 */
	virtual void CheckValidPlacement() override;

	/** Vanilla's hide toggle is what SetDisabled costs (5.3 ms per bridge shown); timed on a bridge. */
	virtual void SetActorHiddenInGame( bool newHidden ) override;

	/**
	 * §5.1 gives a snapped Rail "only roll and length free", and the roll is ours as well as the length.
	 *
	 * Vanilla's roll is a 10° accumulator, and no threshold on GetRotationStep or on this function's own
	 * `step` argument can recover the fine modifier from it; `mSnapToGuideLines` is the flag, read
	 * directly. Super is deliberately not called, for the same reason as the Junction's: vanilla's
	 * accumulator would keep counting in 10° underneath ours and resurface the moment anything else
	 * read it.
	 */
	virtual void ScrollRotate( int32 delta, int32 step ) override;

	/**
	 * §7.2's 45°, shared. A Rail's cross-section is as symmetric as the Junction's cube, so the same
	 * argument applies: 90° is close to a no-op and 10° lands the profile off every axis at once.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mRollStepDegrees = 45.0f;

	/** The fine step, held on `mSnapToGuideLines`. Same value as the Junction's on purpose. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mRollStepFineDegrees = 5.0f;

	/**
	 * §7.3's 1.5 m, reused. How near an end counts as aiming at it is the same question whichever
	 * hologram asks it, and the Junction has already tested this value.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mTerminalSnapRange = ACPRSpec::TerminalSnapRange;

	/** Invariant 8. Two candidates within this of each other refuse rather than picking one. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mAmbiguityTolerance = 5.0f;

	/** How closely a terminal's outward must agree with the hit normal to count as the aimed face. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mFaceAlignmentMinimum = 0.5f;

	/**
	 * The only lever on the nudge step. GetNudgeDistance() is declared virtual on AFGHologram and
	 * nothing in the game overrides it, so whatever reads the nudge distance through the virtual reads
	 * ours. Measured on the live hologram: mDefaultNudgeDistance = 25 arrives correctly and
	 * AFGHologram::GetNudgeDistance() never returns it — it reports a hardcoded 100, or 20 with the
	 * fine modifier held, so no value of that property can change the step. mGridSnapSize is not
	 * involved either: it reads 100 throughout while the nudge offset walks 25 / 50 / 75 / 100.
	 *
	 * The mode is read off the modifier flag itself, `mSnapToGuideLines` — the same field ScrollRotate
	 * reads — not inferred from vanilla's returned value against a threshold.
	 */
	virtual float GetNudgeDistance() const override;

	/**
	 * The coarse step — no modifier held. Left at vanilla's 100 so one press is one lane, which keeps
	 * a nudged Rail on the lane lattice it started on.
	 *
	 * Editable so it can be tuned without a rebuild. Zero or less means "leave vanilla alone".
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mNudgeDistanceCoarse = ACPRSpec::NudgeCoarse;

	/**
	 * The fine step — modifier held. §11's lane pitch is 1 m and the Rail is 0.5 m wide, so a
	 * quarter metre reaches every lane centre and every half-lane between them.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mNudgeDistanceFine = ACPRSpec::NudgeFine;

	/**
	 * §11: "Wall-aligned Rail centre | 0.5 m from the wall, leaving a 0.25 m gap behind."
	 *
	 * Applied in PostHologramPlacement, the only hook that runs during step 1 AFTER vanilla has
	 * positioned and snapped; SetHologramLocationAndRotation never fires in step 1 at all.
	 *
	 * Measured against the hit point rather than added to the snapped location: vanilla snaps a
	 * wall-mounted beam to a plane 25 uu INSIDE the wall's face but a foundation-mounted one to the
	 * surface itself, so an added constant would mean two different things. The distance from the
	 * surface is SET to this value, whatever plane vanilla chose. Wall and floor come out the same, and
	 * the formula is idempotent for free — re-running it on an already-offset location computes a
	 * delta of zero.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mSurfaceOffset = ACPRSpec::HalfCell;

	/**
	 * §11's lane phase: "Four wall-lane centrelines | 0.5 m, 1.5 m, 2.5 m, 3.5 m on a 4 m wall."
	 *
	 * Vanilla snaps to whole metres measured from the wall's own base, giving 0/1/2/3/4 — five
	 * positions, of which the first and last overhang by half the Rail. Appendix B.2 records the rule
	 * for the Junction: "apply the wall-local grid on the half-cell phase rather than the wall-centre
	 * phase." This is the same half cell, applied to the Rail.
	 *
	 * In the Diagonal and FreeForm build modes it is applied along world Z, on surfaces whose normal
	 * is horizontal — walls. The axis cannot be derived from the Rail: in step 1 the hologram's forward
	 * IS the surface normal, so a cross product with it is the zero vector.
	 *
	 * In the Default build mode on an axis-aligned floor or wall, this same half-cell phase is applied
	 * on BOTH world axes across the surface — the §11 lane lattice below, which puts the Rail on the
	 * grid the Junction already uses.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mLaneOffset = ACPRSpec::HalfCell;

	// ---------------------------------------------------------------------------------------
	// §10.6 — this hologram as a generated blueprint bridge
	//
	// A bridge is an ordinary Rail hologram spawned as a CHILD of the blueprint hologram
	// (AFGHologram::SpawnChildHologramFromRecipe, FGHologram.h:98 — the route Vertical Conveyor
	// Auto-Connect ships). It is never aimed: the blueprint manager hands it its whole geometry in an
	// FACPRBridgeSpec, and every placement hook below stands down to that. What it keeps is exactly
	// what invariant 10 asks for — the same CheckValidPlacement a hand-built Rail runs, the same
	// ConfigureActor length correction, the same recipe and the same cost path.
	//
	// Nothing here is a UPROPERTY, so the reflected shape of the class is unchanged.
	// ---------------------------------------------------------------------------------------

	/** From SpawnChildHologramFromRecipe's pre-spawn hook, so BeginPlay already knows. */
	void MarkAsBridge();

	bool IsBridge() const { return mIsBridge; }

	/** The manager's first click (§10.2 step 2). Only a latched bridge may block the whole placement. */
	void SetBridgeLatched( bool latched ) { mBridgeLatched = latched; }

	/** Writes the whole geometry, the fault, the space and the customization; re-applies the preview. */
	void ApplyBridgeSpec( const FACPRBridgeSpec& spec );

	/** "#3 t0" and the like — only for the log. Once, at spawn, not a string per evaluation. */
	void SetBridgeLabel( const FString& label ) { mBridgeLabel = label; }

	/**
	 * Runs this hologram's own validation NOW rather than waiting for the parent's pass: clears
	 * mConstructDisqualifiers (FGHologram.h:772, protected, so no import) and calls CheckValidPlacement.
	 * True when nothing disqualified it. The names go to out_disqualifiers for the log.
	 */
	bool ValidateBridgeNow( FString& out_disqualifiers );

	double GetBridgeLength() const { return mIsBridge ? static_cast< double >( mRequestedFarLength ) : 0.0; }

	/** §13 applied to the exact length: max( 1, ceil( L / mLengthPerCost ) ). */
	int32 GetBridgeCostMultiplier() const;

	/** The build class CDO, for the manager's §10.4 length limits. */
	const class AACPRRail* GetRailBuildCDO() const { return GetRailCDO(); }

	/** GetCost( false ) as one readable string, for the log. */
	FString DescribeOwnCost() const;

	/**
	 * §10.2 "nudging that changes a generated Rail's length recalculates its cost live". AFGBeamHologram's
	 * multiplier comes from its private mCurrentLength, which a bridge never drives (it is never aimed),
	 * so for a bridge the multiplier is computed from the exact length instead. A virtual of our base
	 * (FGBeamHologram.h:40) — overriding it imports nothing.
	 *
	 * Hand-placed Rails whose length is OURS — a far-end snap or a §11 lane Rail (GetOwnLength) — use the
	 * same exact-length formula, because that is the length ConfigureActor builds. Every other Rail goes to
	 * Super, whose length is the one the build will have.
	 */
	virtual int32 GetBaseCostMultiplier() const override;

	/**
	 * §10.2 "showing red where ordinary building rules would prevent it". The blueprint hologram may push
	 * its own material state onto its children (GetHologramsToShareMaterialStateWith is overridden there,
	 * FGBlueprintHologram.h:43); a bridge with a disqualifier of its own stays red whatever it is handed.
	 */
	virtual void SetPlacementMaterialState( EHologramMaterialState materialState ) override;

protected:
	virtual void ConfigureActor( class AFGBuildable* inBuildable ) const override;
	virtual void ConfigureComponents( class AFGBuildable* inBuildable ) const override;

	/**
	 * A bridge has no aim and therefore no floor. AFGBuildableHologram's floor test (FGBuildableHologram.h:292,
	 * protected virtual) reads the last hit, which a child never gets — Vertical Conveyor Auto-Connect hit
	 * exactly this as a spurious InvalidFloor on its synthetic lifts. Hand-placed Rails go to Super.
	 */
	virtual void CheckValidFloor() override;

private:
	/**
	 * Step 1: places the near end on an open terminal and records the axis the drag is confined to.
	 *
	 * Terminal A sits at the actor origin with yaw 180, so its outward is the actor's local -X
	 * (ACPRRail::CreateTerminals). Opposing the target therefore means local +X = the target's
	 * outward, and the Rail extends from the snap point along it — which is exactly §5.1's "a snapped
	 * Rail continues along the terminal's outward axis", falling out of the geometry rather than
	 * being enforced separately.
	 */
	bool TrySnapAnchorToTerminal( const FHitResult& hitResult );

	/**
	 * Which terminal is being aimed at — the one question both ends ask, asked in one place.
	 *
	 * Invariant 8's ambiguity refusal, the impact-normal face filter and the two-pass tie count are
	 * one set of rules; the far end needs all three and not one of them differently, so a second copy
	 * could only ever drift from the first. Returns null when nothing qualifies OR when the result is
	 * ambiguous; `out_tied > 1` distinguishes the two for the log.
	 */
	class UACPRTerminalComponent* FindAimedTerminal( const FHitResult& hitResult, double& out_distance,
	                                                 int32& out_open, int32& out_candidates,
	                                                 int32& out_tied ) const;

	/**
	 * §5.1's second end: "the far end may likewise snap to an open terminal lying along the Rail's
	 * axis within its maximum length."
	 *
	 * The same trick as the near end, and for the same reason. The far end IS the length, and length
	 * is what `AFGBeamHologram` derives from the hit point — so snapping it means rewriting the aim to
	 * the target terminal's location and letting vanilla compute from there. The preview stretches to
	 * the real length rather than to a corrected one, and `ConfigureActor`'s length push carries it to
	 * the buildable.
	 *
	 * It works with a free near end too, and that falls out rather than being added: with the anchor
	 * snapped the target must additionally lie on the anchor's ray, and without one the aim itself
	 * chooses the axis. Two rules, not two code paths.
	 *
	 * Returns true when it rewrote the hit. Creates no Coupling.
	 */
	bool TryFarEndSnap( FHitResult& hitResult );

	/** Drops the far-end state. Separate from ClearAnchor: the two ends are independent. */
	void ClearFarSnap();

	/**
	 * Every far-end verdict, accepted or refused, with the number that decided it.
	 *
	 * The refusals are the useful half. "The second end will not snap" has four distinct causes —
	 * nothing aimed at, too short or too long, the wrong way round, off the anchor's axis — and they
	 * are indistinguishable from the outside. Each one prints its own `residual`, so the log answers
	 * which rule declined rather than only that something did.
	 */
	void LogFarSnap( const AActor* host, const class UACPRTerminalComponent* terminal,
	                 const TCHAR* verdict, double length, double residual, int32 open, int32 survivors,
	                 int32 tied, int32 rejRange, int32 rejOpposing, int32 rejAxis );

	/**
	 * Step 2: rewrites the aim onto the anchor's outward ray before vanilla ever sees it.
	 *
	 * This is the whole trick, and it writes nothing of vanilla's. AFGBeamHologram computes direction
	 * and length from the hit result into mCurrentLength (Appendix B.2). SetHologramLocationAndRotation
	 * takes the hit by const reference — so a COPY with the aim point projected onto the ray, handed to
	 * Super, makes vanilla compute a length along an axis we chose, using its own code, with its own
	 * preview stretch and its own ConfigureActor length push. Invariant 9's "direction is inherited"
	 * becomes a property of the input rather than a correction applied afterwards.
	 *
	 * The clamp is the Rail's own limits, read off the build class (GetSize, GetMaxLength) rather
	 * than written down here — §11's 1 m and 40 m are the beam's numbers and must not be duplicated.
	 */
	void ConstrainAimToAxis( FHitResult& hitResult ) const;

	/** Drops all four anchor fields together. The only clearer, so they cannot go out of step. */
	void ClearAnchor();

	/** The build class CDO, for the length limits. Null if the parent class is wrong. */
	const class AACPRRail* GetRailCDO() const;

	void LogAnchorSnap( const AActor* hitActor, const class UACPRTerminalComponent* terminal,
	                    double distance, int32 openInRange, int32 candidates, int32 tied );

	/**
	 * What the drag asked for, what the constraint allowed, and what vanilla did with it. The last
	 * part is the one that matters: every other line is printed by the code doing the handing-over,
	 * so none of them could report that the hand-over was ignored.
	 */
	void LogConstrainedAim( const FHitResult& raw, const FHitResult& constrained,
	                        const FTransform& beforeSuper, const FTransform& afterSuper,
	                        const FRotator& pinned ) const;

	/**
	 * Writes rotation = roll ∘ base, absolutely, from whichever hook ran last.
	 *
	 * Absolute rather than accumulated: the roll is composed onto a stored roll-FREE base every time,
	 * so calling this from two hooks in one frame costs a quaternion multiply and cannot compound.
	 */
	void ApplyRoll();

	/**
	 * Records the rotation that roll composes onto, and the frame it was recorded on.
	 *
	 * The frame stamp is load-bearing, not bookkeeping. Vanilla rewrites the rotation from scratch on
	 * every step-2 frame, so capturing `GetActorQuat()` after Super:: gives a roll-free base — but on
	 * a frame where `TrySnapToActor` returned true, `SetHologramLocationAndRotation` never ran and
	 * the only rotation available is the one WE last wrote, roll included. Capturing that would fold
	 * the roll into the base and double it on the next frame, and again on the next. The stamp is how
	 * PostHologramPlacement tells the two frames apart.
	 */
	void CaptureRollBase( const FQuat& base );

	/**
	 * Remembers what the pin last wrote. PostHologramPlacement's free-hand branch compares the actor's
	 * rotation against it to tell "vanilla wrote a fresh, roll-free base this frame" from "this is still
	 * our own rolled write" — the guard that keeps the roll from folding into its own base.
	 */
	void RecordAppliedRotation();

	/**
	 * Writes the preview body's length scale. The component is bound once (BeginPlay, by the
	 * buildable's mesh asset) and the scale is written only when the length changed, so a bridge's
	 * placement hooks do not rescan its mesh components.
	 */
	void ApplyLengthToPreview();

	/** The preview body: the plain mesh component carrying mPreviewMeshAsset. Re-bound if the pointer rots. */
	class UStaticMeshComponent* GetPreviewMesh() const;

	/** §5.1's block, logged once per distinct (blocker, verdict). */
	void LogBlocked( const class UACPRTerminalComponent* blocker, double along, double length,
	                 int32 candidates, const TCHAR* verdict );

	/**
	 * §12. Shows every eligible terminal on the aimed host, with this frame's target marked.
	 *
	 * Driven from PostHologramPlacement, which is the only hook that runs in BOTH steps and on every
	 * frame — SetHologramLocationAndRotation is suppressed whenever TrySnapToActor returns true, and
	 * that is precisely the snapped frame the markers matter most on.
	 *
	 * Which target is "selected" depends on the step, and that is the feature rather than a detail.
	 * Before the first click the player is choosing a face to stand the Rail ON, so the anchor's pick
	 * is the selection; after it they are choosing a face to REACH, so the far-end pick is. Marking
	 * the wrong one would be worse than marking none.
	 */
	void UpdateTerminalHighlight( const FHitResult& hitResult );

	/** Moves the positioned hologram off the surface. Idempotent by measurement — see the .cpp. */
	void ApplyPlacementOffsets( const FHitResult& hitResult );

	/** Which hook fired, in which step, against which surface. */
	void LogPlacement( const FHitResult& hitResult, const TCHAR* step );

	/**
	 * The beam preview's mesh ASSET, recorded at BeginPlay while the hologram's component set is
	 * still only the buildable's, and the component carrying it.
	 *
	 * The length scale is written to the component carrying this asset and to nothing else. Locking the
	 * hologram spawns the nudge gizmo out of ordinary static meshes, and a search for "plain static
	 * mesh components" picks those up too — a scale written into them shows as a pipe segment and two
	 * cones inside the preview. The asset is what identifies the body; the component pointer is the
	 * cache, re-derived from the asset if it ever goes invalid.
	 */
	TWeakObjectPtr< class UStaticMesh > mPreviewMeshAsset;
	mutable TWeakObjectPtr< class UStaticMeshComponent > mPreviewMesh;

	/** The last length scale ApplyLengthToPreview wrote; the write is skipped while it stands. */
	double mPreviewScaleZ = -1.0;

	/**
	 * Adopts vanilla's attachment snap (a Junction target, step 1) as the anchor — the same four
	 * fields TrySnapAnchorToTerminal writes, from the terminal behind mSnappedAttachmentPoint.
	 */
	void AdoptVanillaSnapAsAnchor();

	/** Placement updates run every frame; these keep the log to one line per real change. */
	mutable FACPRLogGate mPlacementLog{ 16 };

	/** Set once the first click latches the anchor; the offset must not be re-applied after that. */
	bool mAnchorLatched = false;

	/**
	 * The terminal the near end is snapped to, and the ray the drag is confined to.
	 *
	 * Set in step 1 and DELIBERATELY NOT CLEARED when the anchor latches — the axis it carries is
	 * what step 2 constrains against, and §5.1 gives the drag only length and roll once a Rail has
	 * begun at a terminal. It is cleared per frame only while step 1 is still running.
	 */
	/**
	 * The anchor state is self-contained, and the weak pointer is not part of it.
	 *
	 * The constraint needs a point, an axis and an up vector — nothing else about the target. Gating
	 * it on mAnchorTerminal.IsValid() would mean that if the target's owner were dismantled mid-drag
	 * the weak pointer would go stale, the raw hit would reach vanilla, and the near end would jump
	 * off the terminal with nothing in the log to say why. Caching the geometry and gating on the
	 * AXIS instead makes the constraint depend only on values that cannot rot.
	 *
	 * All four are written and cleared together. The pointer is kept for the log and for whatever
	 * §6.1 wants later; mAnchorOutward being non-zero is what "we are snapped" means.
	 */
	TWeakObjectPtr< class UACPRTerminalComponent > mAnchorTerminal;
	FVector mAnchorPoint = FVector::ZeroVector;
	FVector mAnchorOutward = FVector::ZeroVector;
	FVector mAnchorUp = FVector::ZeroVector;

	/** True once the near end is on a terminal. One name for the condition, checked everywhere. */
	FORCEINLINE bool IsAnchorSnapped() const { return !mAnchorOutward.IsNearlyZero(); }

	/**
	 * The far end's target, and the point the aim was rewritten to.
	 *
	 * Re-evaluated every step-2 frame rather than latched, because the far end is what the player is
	 * still dragging: aiming away from a terminal must release it as readily as aiming at it takes
	 * hold. The near end latches on a click; this one does not latch at all until construction.
	 */
	TWeakObjectPtr< class UACPRTerminalComponent > mFarTerminal;
	FVector mFarPoint = FVector::ZeroVector;

	/**
	 * CustomSerialization is vanilla's specifier for "part of the construct message" (FGHologram.h:342;
	 * AFGBeamHologram marks mCurrentLength with it, FGBeamHologram.h:80). In multiplayer a client's
	 * hologram is serialized to the server, which builds from ITS copy — so every field ConfigureActor
	 * reads must travel: the far snap, the requested length, the lane length and the bridge flag. The
	 * transform, the customization and mBlueprintDesigner are vanilla's and already travel. Untested in
	 * a multiplayer session; this is what should be needed.
	 */
	UPROPERTY( CustomSerialization )
	bool mFarSnapped = false;

	/**
	 * The length the far-end snap asked for. ConfigureActor builds it exactly and prints it beside the
	 * length vanilla derived: AFGBeamHologram quantises its length to whole multiples of mSize, so a far
	 * end snapped to a terminal 1050 uu away would otherwise land 50 uu off its cell and no Coupling
	 * would form. A mismatch between the two names itself rather than being inferred from an absent
	 * Coupling.
	 */
	UPROPERTY( CustomSerialization )
	float mRequestedFarLength = -1.0f;

	/**
	 * How far off the anchor's ray a far-end candidate may sit and still count as "along the axis".
	 *
	 * Same 5 uu as the ambiguity tolerance, and for a related reason: terminal cells are exact
	 * integers, so a target genuinely on the axis is off by float noise or by whole metres, never by
	 * three. This is a noise budget, not a search radius.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mAxisTolerance = 5.0f;

	/**
	 * §12's red feedback for a Rail blocked by a terminal in its path. See CheckValidPlacement.
	 *
	 * UFGCDInvalidAimLocation, which the Cap, the Outlet and the Junction all use — a disqualifier class
	 * is a symbol the DLL must import like any other, and a better-worded one can be selected here in
	 * the editor at no DLL cost.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	TSubclassOf< class UFGConstructDisqualifier > mBlockedDisqualifier;

	/**
	 * How far past the far end a terminal may sit and still count as "at the end" rather than
	 * "inside the span".
	 *
	 * It is not an epsilon on a geometric test in B.3's forbidden sense — that rule is about the
	 * resolver, where positions are exact integers. This is about the LENGTH, which vanilla quantises
	 * up to a whole multiple of mSize: a Rail aimed at 675 is built at 700, so a terminal at 700 has to
	 * read as the end and not as an obstruction one micron inside it.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	float mSpanEndTolerance = 5.0f;

	/** The host this hologram last wrote §12's highlight onto, so it can be cleared. */
	TWeakObjectPtr< AActor > mHighlightHost;

	/** The anchor's host, kept lit through step 2 by FACPRTerminalHighlight::Pin. */
	TWeakObjectPtr< AActor > mAnchorHighlightHost;

	/** The host of the terminal the far end will couple to by geometry (FindFarEndCoincidence), lit Selected. */
	TWeakObjectPtr< AActor > mFarHighlightHost;

	/** The open, opposing terminal in the cell the Rail's far end will be built on, if any. */
	const class UACPRTerminalComponent* FindFarEndCoincidence() const;

	/**
	 * The length ConstrainAimToAxis last produced, which is the number vanilla derives mCurrentLength
	 * from. CheckValidPlacement's only view of how long this Rail is about to be. See §5.1's block.
	 */
	mutable double mAimAlong = 0.0;

	/** The blocking scan's own log state. */
	mutable FACPRLogGate mBlockLog{ 24 };

	/** §7.2's roll, in degrees, accumulated by ScrollRotate and applied by ApplyRoll. */
	double mRollDegrees = 0.0;

	/**
	 * The rotation roll composes onto, and the frame it was captured on. See CaptureRollBase for why
	 * the frame matters.
	 */
	FQuat mRollFreeRotation = FQuat::Identity;
	uint64 mRollBaseFrame = 0;

	int32 mRollLogBudget = 16;
	mutable FACPRLogGate mFarSnapLog{ 40 };

	mutable FACPRLogGate mSnapLog{ 24 };

	mutable FACPRLogGate mAimLog{ 64 };

	mutable FACPRLogGate mValidityLog{ 24 };

	/** What the pin last wrote. See RecordAppliedRotation. */
	FQuat mLastAppliedRotation = FQuat::Identity;

	/** Last computed lane axis, kept only so the log can show what the rule actually used. */
	FVector mLastLaneAxis = FVector::ZeroVector;

	/** Last distance GetNudgeDistance returned, so each MODE logs once rather than only the first. */
	mutable float mLastLoggedNudgeDistance = -1.0f;

	// ---------------------------------------------------------------------------------------
	// §10.1 — the build space this placement is in
	// ---------------------------------------------------------------------------------------

	/**
	 * AFGHologram::mBlueprintDesigner (FGHologram.h:776, protected) — vanilla's own record of whether
	 * this hologram "is currently residing inside a blueprint designer" (FGHologram.h:446). Read as a
	 * field rather than through AFGHologram::GetBlueprintDesigner (:448), which is a plain export.
	 */
	class AFGBuildableBlueprintDesigner* GetPlacementDesigner() const;

	/** FACPRSpace::IsForeignTarget's throttles — one per end, or two refused ends would alternate. */
	FString mLastSpaceLog;
	FString mLastSpaceLogFar;

	/** FACPRSpace::WatchPlacementSpace's state: logs each change of placement space once. */
	TWeakObjectPtr< class AFGBuildableBlueprintDesigner > mLastPlacementSpace;
	bool mPlacementSpaceSeen = false;

	// ---------------------------------------------------------------------------------------
	// §10.6 bridge state. See ApplyBridgeSpec.
	// ---------------------------------------------------------------------------------------

	UPROPERTY( CustomSerialization )
	bool mIsBridge = false;

	/** The placement's space, handed over by the manager. GetPlacementDesigner answers this for a bridge. */
	TWeakObjectPtr< class AFGBuildableBlueprintDesigner > mBridgeSpace;

	/** §10.4's geometry verdict from the manager; true adds mBlockedDisqualifier. */
	bool mBridgeFaulted = false;

	FString mBridgeLabel;

	/**
	 * True only while ValidateBridgeNow runs. The parent-level block (a latched invalid bridge
	 * disqualifies the whole blueprint) is added only from VANILLA's pass over its children, which runs
	 * inside the parent's own validation, after the parent's reset — never from ours, which runs outside
	 * it and could leave a stale entry on the parent.
	 */
	bool mInManagerValidation = false;

	/** Vanilla's disqualifiers from the last manager validation, replayed on the frames between. */
	TArray< TSubclassOf< class UFGConstructDisqualifier > > mCachedBridgeDisqualifiers;

	/** DescribeDisqualifiers() of that list, rebuilt only when the list changes. */
	FString mCachedDisqualifierText;

	/** Set by the manager on the first click. Only a latched bridge may block its parent (§10.2 step 4). */
	bool mBridgeLatched = false;

	FString mLastBridgeValidation;
	int32 mBridgeLogBudget = 40;
	int32 mBridgeMoveLogBudget = 6;

	/** Puts the actor on the spec's frame. Called from ApplyBridgeSpec only; see OnHologramTransformUpdated. */
	void ApplyBridgeTransform();

	/** mConstructDisqualifiers as "A,B" or "<none>". */
	FString DescribeDisqualifiers() const;

	/**
	 * The hand-placed Rail's whole validation — §10.1's space refusal and §5.1's blocked span.
	 * CheckValidPlacement calls this and then adds the bridge's two extras, so a bridge runs the same
	 * validator as a manual Rail (invariant 10) and the early returns in here cannot skip them.
	 */
	void CheckRailPlacement();

	// ---------------------------------------------------------------------------------------
	// §11 lane lattice — the Default build mode on axis-aligned floors and walls
	//
	// The problem, in one line: a Junction's centre sits mid-cell on the world 1 m grid, so its face
	// centres are "whole metre along the face normal, half metre across it" — and a Rail's endpoints
	// need exactly that mixed phase too, but which coordinate is which depends on the Rail's direction,
	// which does not exist until the drag after the first click.
	//
	// The rule: before the click the start preview sits at the aimed cell's centre C — where a Junction
	// would stand — mSurfaceOffset off the surface. After the click the drag picks the axis, and the
	// start moves to the edge of that cell on the drag side, C + axis·50: exactly the face a Junction in
	// that cell would offer. Lengths are whole metres, so the far end lands on a face centre too. A drag
	// away from (or into) the surface starts mid-cell ON the surface instead.
	//
	// Who picks the axis: vanilla. Each lane frame hands AFGBeamHologram the raw aim once, from C, and
	// reads the direction its Default mode chose; that direction is taken as the world axis it lies on (a
	// direction more than ~2.6° off every axis is left to vanilla for that frame, and logged).
	// So the "which way does my drag go" feel is vanilla's own, and only the start and the length are
	// ours. A second pass then hands vanilla an aim on our axis so its own length, readout and cost agree.
	//
	// Diagonal and FreeForm never enter this; a locked hologram freezes it (nudges are the player's); a
	// terminal snap at either end wins over it; and like a snapped Rail it may not pass through a terminal on
	// its own axis (§5.1's span rule, CheckRailPlacement). Plain C++ — the reflected shape is unchanged.
	// ---------------------------------------------------------------------------------------

	/** Not Diagonal and not FreeForm (AFGHologram::IsCurrentBuildMode, FGHologram.h:266, UFUNCTION). */
	bool IsLaneBuildMode() const;

	/** Step 2 of a lane Rail with an axis chosen. The one test every hook uses. */
	FORCEINLINE bool IsLaneDriving() const { return mLaneMode && mAnchorLatched && !mLaneAxis.IsNearlyZero(); }

	/**
	 * The length this Rail will be built at when the length is ours rather than vanilla's: the far-end
	 * snap's exact length, else the lane length. -1 when vanilla's length stands.
	 */
	double GetOwnLength() const;

	/** Drops all lane state except the step-1 candidate, which PostHologramPlacement recomputes per frame. */
	void ClearLane();

	/**
	 * The step-2 lane frame. True when it placed the Rail; false hands the frame back to the ordinary
	 * free-hand path (no lane mode, a snapped anchor, or the build mode changed after the click).
	 */
	bool DriveLane( const FHitResult& hitResult );

	/** Locked in step 2: axis and length freeze, a live far snap becomes a plain length, location is vanilla's. */
	void FreezeLaneForLock();

	/** C + axis·50 across the surface, C − normal·mSurfaceOffset along it. */
	FVector LaneStartFor( const FVector& axis ) const;

	/** The pinned frame: +X along the axis, +Z world up unless the axis is vertical. */
	FQuat LaneRotationFor( const FVector& axis ) const;

	/** How far along the axis the aim is, from `start`; out_source names the method for the log. */
	double LaneAimAlong( const FHitResult& hitResult, const FVector& start, const FVector& axis,
	                     const TCHAR*& out_source, double& out_other ) const;

	/** The beam preview's current length, read off the mesh scale vanilla writes. -1 when not found. */
	double ReadPreviewLength() const;


	void LogLaneEvent( const FString& what );

	/** Step 1: this frame's aimed cell qualified, and its centre and exact axis normal. */
	bool mLaneCandidate = false;
	FVector mLaneCandidateCentre = FVector::ZeroVector;
	FVector mLaneCandidateNormal = FVector::ZeroVector;

	/** Set by the first click from the candidate; cleared by the next placement's Pre. */
	bool mLaneMode = false;
	FVector mLaneCentre = FVector::ZeroVector;
	FVector mLaneNormal = FVector::ZeroVector;

	/** This frame's lane axis (a world axis direction), start, and whole-metre (or far-snapped) length. */
	FVector mLaneAxis = FVector::ZeroVector;
	FVector mLaneStart = FVector::ZeroVector;

	UPROPERTY( CustomSerialization )
	double mLaneLength = -1.0;

	/** True while a lock holds the lane. */
	bool mLaneFrozen = false;

	/** One line per run of frames where vanilla's direction was not a world axis. */
	bool mLaneOffAxisLogged = false;

	mutable FACPRLogGate mLaneLog{ 60 };
	mutable FACPRLogGate mLaneCellLog{ 20 };
	int32 mLaneEventLogBudget = 30;
};
