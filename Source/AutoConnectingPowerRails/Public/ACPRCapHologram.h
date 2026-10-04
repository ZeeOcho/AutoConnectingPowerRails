// Auto-Connecting Power Rails — the Insulated Terminal Cap hologram.
//
// §9: "A Cap snaps only to an open terminal and caps it." That is the entire targeting rule, and it
// makes this the simplest hologram in the mod: there is no free placement, no length, no lattice and
// no nudge. Either the crosshair is on an open terminal or the Cap does not place.
//
// Two validity overrides, because a fact about a VANILLA base class applies to every subclass we
// own:
//
//   IsValidHitResult — vanilla refuses a hit on some buildables outright, which hides the hologram
//   before TrySnapToActor is ever called. A Cap that could not be aimed at a Junction would be the
//   same bug with a different buildable's name on it.
//
//   CheckValidFloor — vanilla's floor-angle rule refuses anything steep, and a Cap goes on
//   whichever face a terminal points out of, including straight down off a Junction's bottom.
//
// What it does not need, so the absence is deliberate rather than forgotten: no
// ScrollRotate (a Cap on a terminal has no meaningful roll — §12 says roll is cosmetic and a Cap's
// face is circular), and no nudge (§9 gives the snap point, and the Cap must not be movable off the
// plane §11 fixes at zero external spacing).

#pragma once

#include "CoreMinimal.h"
#include "ACPRLog.h"
#include "ACPRHologramCommon.h"
#include "ACPRSpec.h"
#include "Hologram/FGBuildableHologram.h"
#include "ACPRCapHologram.generated.h"

class UACPRTerminalComponent;

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API AACPRCapHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	AACPRCapHologram();

	virtual void BeginPlay() override;

	/** Takes §12's highlight off the aimed host. See AACPRRailHologram::Destroyed. */
	virtual void Destroyed() override;

	/** See the header. Same widening as the Junction's and the Rail's. */
	virtual bool IsValidHitResult( const FHitResult& hitResult ) const override;

	/**
	 * §9's whole targeting rule.
	 *
	 * Returning true is the documented way to tell the build gun "location and snapping are applied"
	 * (FGHologram.h:175), which is exactly right here: there is no valid Cap placement that is not on
	 * a terminal, so either this places the hologram or the frame produces nothing.
	 *
	 * Like every other snap in this mod it creates no Coupling and no capped state — it only puts the
	 * actor where the terminal is. The Cap applies itself at BeginPlay: place exactly, and let the
	 * actor own its own consequence.
	 */
	virtual bool TrySnapToActor( const FHitResult& hitResult ) override;

	/** §9: the snap point is the terminal. Nothing may move it off the plane §11 fixes at 0 m. */
	virtual bool CanNudgeHologram() const override;

	/**
	 * The one per-frame hook: the preview refit and §12's highlight both live here.
	 *
	 * Post, not SetHologramLocationAndRotation, and the vanilla header says why in one line.
	 * FGHologram.h:178-179 documents SetHologramLocationAndRotation as *"will only be called if we
	 * have a valid hit result AND DID NOT SNAP"* — so on every frame this hologram accepts a
	 * terminal, which is the only frame that matters here, it does not run at all; a refit and a
	 * highlight placed there would be invisible precisely when a target is found. FGHologram.h:191
	 * says Post runs "after all the placement logic", snapped or not. The Junction hologram's roll
	 * relies on the same fact.
	 */
	virtual void PostHologramPlacement( const FHitResult& hitResult, bool callForChildren ) override;

protected:
	virtual void CheckValidFloor() override;
	virtual void CheckValidPlacement() override;
	virtual void ConfigureActor( class AFGBuildable* inBuildable ) const override;

public:
	/** §7.3's 1.5 m, shared. How near an end counts as aiming at it is one question, asked once. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	float mTerminalSnapRange = ACPRSpec::TerminalSnapRange;

	/** Invariant 8. Two candidates within this of each other refuse rather than picking one. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	float mAmbiguityTolerance = 5.0f;

	/** The face filter — how closely a terminal's outward must agree with the hit normal. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	float mFaceAlignmentMinimum = 0.5f;

	/**
	 * §12's "vanilla-style red feedback with an appropriate disqualifier".
	 *
	 * Defaulted to UFGCDInvalidAimLocation in the constructor, which says why not UFGCDMustSnap. The
	 * concrete classes are in FGConstructDisqualifier.h: UFGCDMustSnap at :172,
	 * UFGCDInvalidPlacement at :83, and about thirty more.
	 *
	 * The default matters rather than being a nicety: with this null, CheckValidPlacement adds nothing,
	 * the hologram goes green on a frame with no terminal under the crosshair, and a Cap is built
	 * capping nothing. Editable so it can be pointed at a better-worded disqualifier without a rebuild.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	TSubclassOf< class UFGConstructDisqualifier > mNoTerminalDisqualifier;

private:
	void LogSnap( const TCHAR* verdict, const struct FACPRTerminalPick& pick );

	/**
	 * B.2's consequence, and the Junction's BindPreviewMesh comment is the long form of why this is a
	 * capture rather than a per-frame search: a search that runs repeatedly answers a different
	 * question each time it is asked, and the set that belongs to the BUILDABLE is fixed the moment
	 * the hologram is constructed.
	 */
	void BindPreviewMesh();

	/** Re-fits the preview to the CURRENT target's face width — §11 gives 0.4 m on a Rail, 0.9 on a Junction. */
	void FitPreviewMesh();

	/** §12. Shows every eligible terminal on the aimed host, with this frame's target marked. */
	void UpdateTerminalHighlight( const FHitResult& hitResult );

	TWeakObjectPtr< class UStaticMeshComponent > mPreviewMesh;
	bool mPreviewBindWarned = false;
	bool mPreviewFitLogged = false;
	float mLastFittedWidth = -1.0f;

	/** The host this hologram last wrote §12's highlight onto, so it can be cleared. */
	TWeakObjectPtr< AActor > mHighlightHost;

	/** The terminal this frame's snap landed on, and its host — what ConfigureActor writes down. */
	TWeakObjectPtr< UACPRTerminalComponent > mTargetTerminal;
	/**
	 * What ConfigureActor writes is part of the construct message (CustomSerialization,
	 * FGHologram.h:342), so a server building from a client's hologram writes the same host. A TObjectPtr
	 * because that is what the message serializer carries; Pre/TrySnapToActor rewrite it every frame.
	 * Untested in multiplayer.
	 */
	UPROPERTY( CustomSerialization )
	TObjectPtr< AActor > mTargetHost = nullptr;
	UPROPERTY( CustomSerialization )
	uint8 mTargetIndex = 0;

	/** True while the current frame has a valid target. CheckValidPlacement's input. */
	UPROPERTY( CustomSerialization )
	bool mHasTarget = false;

	mutable FACPRLogGate mValidityLog{ 24 };
	/** The last aim IsValidHitResult saw; CheckValidFloor reads its normal (ACPRHologramCommon.h). */
	mutable FACPRAimRecord mAim;

	mutable FACPRLogGate mSnapLog{ 24 };

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
