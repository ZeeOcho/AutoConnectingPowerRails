// Auto-Connecting Power Rails — the Power Rail Outlet hologram.
//
// §8 gives the Outlet two host modes and §15's targeting index gives the rule for choosing between
// them, which is the whole of this class:
//
//   Outlet | Valid targeted Rail body            -> Body-mounted Outlet
//   Outlet | Open terminal                       -> Terminal-mounted Outlet plus Coupling
//   Outlet | Coupled/capped terminal, or Junction body -> Invalid
//
// The order matters, and §8.1 says so outright: "Aiming at an end face resolves to terminal-mounted
// or invalid, never silently to a body attachment." A Rail's end face is geometrically part of its
// body, so a naive body-mount test would happily clamp an Outlet to the cap of a Rail whose terminal
// is already coupled — putting a Power Connection somewhere §8.2 says an end attachment belongs, at
// a position that moves if the Rail is ever split. So the terminal question is asked first, and a
// hit whose normal lies along the Rail's own axis is refused outright rather than falling through.
//
// Two vanilla-base overrides carry the rest: IsValidHitResult, because vanilla refuses hits on our
// own buildables and would hide the hologram before any of the above runs; CheckValidFloor, because
// an Outlet mounts on whichever face is aimed at, including vertical ones and the underside of a
// Rail.

#pragma once

#include "CoreMinimal.h"
#include "ACPRLog.h"
#include "ACPRHologramCommon.h"
#include "ACPRSpec.h"
#include "ACPROutlet.h"
#include "Hologram/FGBuildableHologram.h"
#include "ACPROutletHologram.generated.h"

class UACPRTerminalComponent;

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API AACPROutletHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	AACPROutletHologram();

	virtual void BeginPlay() override;

	/** Takes §12's highlight off the aimed host. See AACPRRailHologram::Destroyed. */
	virtual void Destroyed() override;

	/** Vanilla refuses hits on our own buildables — see the header. */
	virtual bool IsValidHitResult( const FHitResult& hitResult ) const override;

	/**
	 * §15's three rows, in §8.1's required order: terminal first, body second, invalid otherwise.
	 *
	 * Returning true means "location and snapping are applied" (FGHologram.h:175), which is right for
	 * both accepted modes — each computes an exact transform — and returning Super's answer for the
	 * refusals keeps the hologram visible while the player sweeps toward a valid target.
	 */
	virtual bool TrySnapToActor( const FHitResult& hitResult ) override;

	/** §8.1 and §8.2 both: "cannot be nudged". The host face decides the position, not the player. */
	virtual bool CanNudgeHologram() const override;

	/**
	 * The one per-frame hook — see AACPRCapHologram's equivalent for the full argument.
	 *
	 * The short version: FGHologram.h:178-179 says SetHologramLocationAndRotation runs only on a
	 * frame that "did not snap", so it is absent on exactly the frames this hologram accepts a
	 * target. Post runs either way.
	 */
	virtual void PostHologramPlacement( const FHitResult& hitResult, bool callForChildren ) override;

protected:
	virtual void CheckValidFloor() override;
	virtual void CheckValidPlacement() override;
	virtual void ConfigureActor( class AFGBuildable* inBuildable ) const override;

public:
	/** §7.3's 1.5 m, shared across every hologram that asks "which terminal is being aimed at". */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mTerminalSnapRange = ACPRSpec::TerminalSnapRange;

	/** Invariant 8. Two candidates within this of each other refuse rather than picking one. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mAmbiguityTolerance = 5.0f;

	/** The face filter — how closely a terminal's outward must agree with the hit normal. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mFaceAlignmentMinimum = 0.5f;

	/**
	 * How nearly a hit normal must lie along the Rail's own axis to count as an END face.
	 *
	 * §8.1's "aiming at an end face resolves to terminal-mounted or invalid, never silently to a body
	 * attachment" needs a test, and this is it. Deliberately generous: a normal 80% aligned with the
	 * axis is already unambiguously an end cap, and being generous errs toward refusing a body mount
	 * near an end — which is the safe direction, because §8.2's end attachment is the one the spec
	 * wants there.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mEndFaceAlignment = 0.8f;

	/**
	 * §11: the Rail is 0.5 m square, so its longitudinal faces sit a quarter metre off the centreline.
	 *
	 * Read from the buildable would be better and is not available: AFGBuildableBeam::GetSize() is 100 —
	 * it drives neither the mesh nor snapping. So this is §11's number, as an editable field with the
	 * reasoning attached, which is the rule for an unsourced value.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mRailHalfWidth = ACPRSpec::RailHalfWidth;

	/** §8.1's Guidelines step: "snaps to the 1 m grid on Guidelines", in Rail-local coordinates. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mBodyGuidelineStep = ACPRSpec::CellSize;

	/**
	 * §12's red feedback, defaulted to UFGCDInvalidAimLocation in the constructor — see
	 * AACPRCapHologram's equivalent for the choice of class.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	TSubclassOf< class UFGConstructDisqualifier > mInvalidTargetDisqualifier;

private:
	/** §8.2. Returns false when nothing open is being aimed at, so the body test can run. */
	bool TryTerminalMount( const FHitResult& hitResult );

	/** §8.1. Only ever called for an AACPRRail, and only for a non-end face. */
	bool TryBodyMount( const FHitResult& hitResult );

	void ClearTarget();
	void LogMount( const TCHAR* verdict, const TCHAR* detail );

	/** B.2's consequence — the preview is duplicated from the CDO's templates and never runs BeginPlay. */
	void BindPreviewMesh();
	void FitPreviewMesh();

	/** §12. Shows every eligible terminal on the aimed host, with this frame's target marked. */
	void UpdateTerminalHighlight( const FHitResult& hitResult );

	TWeakObjectPtr< class UStaticMeshComponent > mPreviewMesh;
	/** The pad's copy — found by name, fitted alongside the cylinder. */
	TWeakObjectPtr< class UStaticMeshComponent > mPreviewBase;
	bool mPreviewBindWarned = false;
	bool mPreviewFitLogged = false;
	bool mPreviewFitted = false;
	/** The seat the preview was last fitted for (AACPROutlet::ESeat), -1 before the first fit. */
	int32 mPreviewSeat = -1;

	/** The host this hologram last wrote §12's highlight onto, so it can be cleared. */
	TWeakObjectPtr< AActor > mHighlightHost;

	/** What ConfigureActor writes onto the built Outlet. */
	UPROPERTY( CustomSerialization )
	EACPROutletHostMode mMode = EACPROutletHostMode::Terminal;
	/**
	 * What ConfigureActor writes is part of the construct message (CustomSerialization,
	 * FGHologram.h:342), so a server building from a client's hologram writes the same host. A TObjectPtr
	 * because that is what the message serializer carries; Pre/TrySnapToActor rewrite it every frame.
	 * Untested in multiplayer.
	 */
	UPROPERTY( CustomSerialization )
	TObjectPtr< AActor > mTargetHost = nullptr;

	/**
	 * The terminal this frame's §8.2 mount landed on. Not used for construction — ConfigureActor
	 * writes (host, index), which is the save form — but §12's highlight has to name the EXACT
	 * selected target, and an index is not enough to compare a marker against.
	 */
	TWeakObjectPtr< UACPRTerminalComponent > mTargetTerminal;

	UPROPERTY( CustomSerialization )
	uint8 mTargetIndex = 0;
	UPROPERTY( CustomSerialization )
	bool mHasTarget = false;

	/** Rail-local distance along the host, for the log and for the Guidelines snap. */
	double mBodyAlong = 0.0;

	mutable FACPRLogGate mValidityLog{ 24 };
	/** The last aim IsValidHitResult saw; CheckValidFloor reads its normal (ACPRHologramCommon.h). */
	mutable FACPRAimRecord mAim;

	mutable FACPRLogGate mMountLog{ 32 };

	mutable FACPRLogGate mFloorLog{ 12 };
	// ---------------------------------------------------------------------------------------
	// §10.1 — the build space this placement is in. See AACPRRailHologram's identical block.
	// ---------------------------------------------------------------------------------------

	/** AFGHologram::mBlueprintDesigner (FGHologram.h:776), read as our own protected field. */
	class AFGBuildableBlueprintDesigner* GetPlacementDesigner() const;

	FString mLastSpaceLog;
	TWeakObjectPtr< class AFGBuildableBlueprintDesigner > mLastPlacementSpace;
	bool mPlacementSpaceSeen = false;
};
