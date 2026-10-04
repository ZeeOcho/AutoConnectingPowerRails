// Auto-Connecting Power Rails — the Power Rail hologram.

#include "ACPRRailHologram.h"

#include "ACPRAttachment.h"
#include "ACPRBlueprintTerminalManager.h"
#include "ACPRDesignerSpace.h"
#include "ACPRJunction.h"
#include "ACPRRail.h"
#include "ACPRTerminalComponent.h"
#include "ACPRTerminalHost.h"
#include "ACPRTerminalRegistry.h"
#include "AutoConnectingPowerRails.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FGConstructDisqualifier.h"
#include "ItemAmount.h"

AACPRRailHologram::AACPRRailHologram()
{
	// No Tick override. A bridge's re-evaluation is the manager's, run from vanilla's own update and
	// gated on the body's real transform; vanilla's placement cascade is per frame already and is where
	// the two per-frame writes (the roll pin, the preview length) live. The tick settings themselves are
	// left as AFGHologram's constructor made them: whether vanilla's own Tick does work this class
	// depends on cannot be read from the headers, so it is not switched off.

	// A real default, for AACPRCapHologram's reason: a null one makes CheckValidPlacement silent and
	// §5.1's block does not exist. The class is the one the other holograms use.
	mBlockedDisqualifier = UFGCDInvalidAimLocation::StaticClass();
}

void AACPRRailHologram::Destroyed()
{
	// §12's highlight is written onto the aimed host's materials; the hologram going away must take it
	// with it, whatever ended the hologram. All three hosts: the aimed one, the pinned anchor, the far end.
	FACPRTerminalHighlight::HideAll( mHighlightHost );
	FACPRTerminalHighlight::HideAll( mAnchorHighlightHost );
	FACPRTerminalHighlight::HideAll( mFarHighlightHost );

	Super::Destroyed();
}

bool AACPRRailHologram::IsChanged() const
{
	// A roll off zero is a change of ours vanilla's own accumulator never sees (ScrollRotate does not
	// call Super). Everything else is vanilla's answer.
	return !FMath::IsNearlyZero( mRollDegrees ) || Super::IsChanged();
}

bool AACPRRailHologram::HandleReset()
{
	if( !FMath::IsNearlyZero( mRollDegrees ) )
	{
		mRollDegrees = 0.0;
		ApplyRoll();
		return true;
	}
	return Super::HandleReset();
}

void AACPRRailHologram::CaptureRollBase( const FQuat& base )
{
	mRollFreeRotation = base;
	mRollBaseFrame = GFrameCounter;
}

void AACPRRailHologram::ApplyRoll()
{
	// Roll about the Rail's own forward, taken from the base rather than stored separately. The beam
	// axis is the actor's local X (measured against the rendered world extent), so the base's X axis
	// IS the mating axis when snapped and the drag axis when not — one expression, no branch, and
	// nothing to keep in step with the direction when it changes.
	//
	// A quaternion about an explicit axis, not FRotator::Roll: Euler roll only means "about the
	// forward axis" away from ±90 pitch, and a Rail dropping straight down off a Junction's bottom
	// face sits exactly there.
	const FQuat rolled = FMath::IsNearlyZero( mRollDegrees )
		? mRollFreeRotation
		: FQuat( mRollFreeRotation.GetAxisX(), FMath::DegreesToRadians( mRollDegrees ) ) * mRollFreeRotation;

	SetActorRotation( rolled );
	RecordAppliedRotation();
}

void AACPRRailHologram::ScrollRotate( int32 delta, int32 step )
{
	// §5.2: a bridge's roll is the BP side's, never the player's.
	if( mIsBridge )
	{
		return;
	}

	// See the header. Vanilla's accumulator is 10° per notch and its `step` argument reads 10 with or
	// without the modifier, so neither is consulted; the modifier is `mSnapToGuideLines`, read
	// directly, and Super is not called so vanilla's counter cannot run underneath.
	const double previous = mRollDegrees;

	const bool fine = mSnapToGuideLines;
	const float stepDegrees = fine ? mRollStepFineDegrees : mRollStepDegrees;

	mRollDegrees = FMath::UnwindDegrees( mRollDegrees + delta * stepDegrees );

	ApplyRoll();

	if( mRollLogBudget > 0 && ACPR_LOG_ACTIVE( Verbose ) )
	{
		--mRollLogBudget;

		ACPR_LOG( Verbose, RAIL_HOLO,
			TEXT( "ScrollRotate | delta=%d vanillaStep=%d | fine=%d ourStep=%.1f "
			      "| roll %.1f -> %.1f | snapped=%d latched=%d | axis=%s rot=%s" ),
			delta, step,
			fine ? 1 : 0, stepDegrees,
			previous, mRollDegrees,
			IsAnchorSnapped() ? 1 : 0,
			mAnchorLatched ? 1 : 0,
			*mRollFreeRotation.GetAxisX().ToString(),
			*GetActorRotation().ToString() );
	}
}

void AACPRRailHologram::RecordAppliedRotation()
{
	mLastAppliedRotation = GetActorQuat();
}

void AACPRRailHologram::BeginPlay()
{
	Super::BeginPlay();

	// Our share of a bridge's spawn (MarkAsBridge runs pre-spawn, so mIsBridge is already set).
	TOptional< FACPRCostScope > bridgeCost;
	if( mIsBridge )
	{
		bridgeCost.Emplace( EACPRCost::BridgeBeginPlay );
	}

	// Two structural facts, once per hologram, and both are load-bearing.
	//
	// buildModes: AFGBeamHologram picks between CreateVerticalBeam and CreateFreeformBeam from
	// mBuildModeDiagonal / mBuildModeFreeForm, which live on the vanilla hologram BLUEPRINT and are
	// not inherited. An empty list is the whole "aim at the sky, get a horizontal rail" story.
	//
	// meshes: the hologram copies every mesh component of the buildable, and vanilla adds an empty zoop
	// ISM per copy plus the guidelines. More than expected means the buildable has grown a component,
	// and the hologram's first-match mBeamMesh bind may be pointing at an empty instanced container.
	TArray< TSubclassOf< UFGBuildGunModeDescriptor > > modes;
	GetSupportedBuildModes( modes );

	TArray< UStaticMeshComponent* > meshes;
	GetComponents< UStaticMeshComponent >( meshes );

	// buildModes=0 is this file's own documented cause of "aim at the sky, get a horizontal rail" (the
	// Warning below). meshes=0 means the buildable's mesh component reached the hologram empty, which
	// happens whenever acpr_meshes.py has deleted and recreated SM_ACPR_RailBody without
	// acpr_bind_meshes.py being re-run — the built Rail survives that on the C++ soft-path fallback and
	// the PREVIEW does not, so the two disagree and it looks like a hologram bug. Once per hologram; the
	// names are built only when the line prints.
	if( ACPR_LOG_ACTIVE( Verbose ) )
	{
		FString modeNames;
		for( const TSubclassOf< UFGBuildGunModeDescriptor >& m : modes )
		{
			modeNames += FString::Printf( TEXT( "%s " ), m ? *m->GetName() : TEXT( "<null>" ) );
		}
		FString meshNames;
		for( const UStaticMeshComponent* m : meshes )
		{
			if( m )
			{
				meshNames += FString::Printf( TEXT( "%s(%s) " ), *m->GetName(), *m->GetClass()->GetName() );
			}
		}
		ACPR_LOG( Verbose, RAIL_HOLO,
			TEXT( "BeginPlay (%s) | BUILD=%s | buildModes=%d [%s] | meshes=%d [%s]" ),
			*GetName(), ACPR_BUILD_STAMP(), modes.Num(), *modeNames, meshes.Num(), *meshNames );
	}

	// The preview's orientation, once, here.
	//
	// The preview mesh is OUR RailMesh component, copied from the buildable CDO, not a component
	// vanilla made. So it inherits the CDO's transform — and the CDO never receives the -90 pitch,
	// because AACPRRail::SetUpRailMesh applies it in BeginPlay and a hologram never runs one. Without
	// this the preview lies along the actor's +Z while the built Rail lies along +X.
	//
	// Rotation only, and once. The length scale is written per frame (ApplyLengthToPreview), the
	// orientation here, and they are separated so they cannot fight.
	for( UStaticMeshComponent* m : meshes )
	{
		if( !m || m->IsA< UInstancedStaticMeshComponent >() )
		{
			continue;
		}

		// The collars are hidden in the preview. The buildable's two terminal collars are copied in
		// here like every other component, at identity — so they would both sit at the Rail's origin,
		// facing the same way, until something placed them. Placing them needs the length, which
		// changes every frame. Hidden rather than left at the origin, because a collar sitting in the
		// middle of the Rail's start reads as a bug rather than as an omission.
		if( m->GetName().StartsWith( TEXT( "TerminalMesh" ) ) )
		{
			m->SetVisibility( false );
			continue;
		}

		AACPRRail::OrientRailMesh( m );
	}

	// A bridge announces itself at Display: one line per generated-Rail hologram, and the parent it
	// hangs off is the evidence that SpawnChildHologramFromRecipe did what FGHologram.h:98 says.
	if( mIsBridge )
	{
		ACPR_LOG( Verbose, BRIDGE,
			TEXT( "%s BeginPlay as a blueprint bridge | parent=%s | BUILD=%s | meshes=%d | "
			      "buildClass=%s | needsValidFloor=%d" ),
			*GetName(), *GetNameSafe( GetParentHologram() ), ACPR_BUILD_STAMP(), meshes.Num(),
			*GetNameSafe( GetBuildClass().Get() ), mNeedsValidFloor ? 1 : 0 );
	}

	// The nudge fields, printed from the live instance — and NOT vanilla's GetNudgeDistance(). Calling
	// that virtual from BeginPlay runs every mod's hook on it (InfiniteNudge reads the hologram's player
	// through it) on a bridge child hologram, which has no player, and crashes the game — only with
	// Verbose on, because a Verbose argument is evaluated only when the line prints. Vanilla's answer is
	// a measurement on record (100 / 20); it is not re-asked.
	//
	// mDefaultNudgeDistance=25.0 arrives correctly and vanilla ignores it; mGridSnapSize reads 100
	// throughout while the offset walks 25 / 50 / 75 / 100 — innocent, and it governs placement
	// snapping too, so it is never written.
	ACPR_LOG( Verbose, RAIL_HOLO,
		TEXT( "nudge config (%s) | mDefaultNudgeDistance=%.1f (dead — vanilla "
		      "ignores it) mGridSnapSize=%.1f mCanNudgeHologram=%d mCanLockHologram=%d "
		      "| ours coarse=%.1f fine=%.1f" ),
		*GetName(),
		mDefaultNudgeDistance, mGridSnapSize,
		mCanNudgeHologram ? 1 : 0, mCanLockHologram ? 1 : 0,
		mNudgeDistanceCoarse, mNudgeDistanceFine );

	// The hosts a Rail may snap to, on vanilla's own list for it (FGHologram.h:612,
	// "use this to add a valid hit class for this hologram in blueprints begin play"). A Rail aims at
	// Rail ends and Junction faces; an Outlet's mount is private and a Cap has no terminal, so neither
	// is a target and neither is added — aiming at them stays whatever vanilla says.
	AddValidHitClass( AACPRRail::StaticClass() );
	AddValidHitClass( AACPRJunction::StaticClass() );

	if( modes.Num() == 0 )
	{
		ACPR_LOG( Warning, RAIL_HOLO,
			TEXT( "no build modes — mBuildModeDiagonal / mBuildModeFreeForm are "
			      "unset on Holo_PowerRail." ) );
	}

	// Record the beam's mesh ASSET, and the component carrying it, while the component set is still
	// only the buildable's. See ApplyLengthToPreview for what starts appearing in that set once the
	// hologram is locked.
	for( UStaticMeshComponent* m : meshes )
	{
		if( m && !m->IsA< UInstancedStaticMeshComponent >() && m->GetStaticMesh() &&
			!m->GetName().StartsWith( TEXT( "TerminalMesh" ) ) )
		{
			mPreviewMeshAsset = m->GetStaticMesh();
			mPreviewMesh = m;
			break;
		}
	}

	ACPR_LOG( Verbose, RAIL_HOLO,
		TEXT( "preview mesh bound: %s on %s (the length scale touches this component "
		      "and nothing else — the nudge gizmo's meshes are left alone)" ),
		mPreviewMeshAsset.IsValid() ? *mPreviewMeshAsset->GetName() : TEXT( "<none>" ),
		mPreviewMesh.IsValid() ? *mPreviewMesh->GetName() : TEXT( "<none — the preview will not stretch>" ) );

	// A sane starting base, assigned without stamping the frame. CaptureRollBase would set
	// mRollBaseFrame to THIS frame, and PostHologramPlacement's first guard is "our writer did not run
	// this frame". On the BeginPlay frame that guard would therefore be false, no capture would happen,
	// and ApplyRoll would write the SPAWN quat over whatever vanilla had just placed — and then latch,
	// because from the next frame the second guard compares our own still-standing write against
	// itself and declines to capture too. A free-hand Rail would sit at its spawn orientation for the
	// whole of step 1.
	//
	// Leaving mRollBaseFrame at 0 makes the first Post capture from the real placement instead, and a
	// scroll before anything has been placed still composes onto where the hologram actually is,
	// which is all this line is for.
	mRollFreeRotation = GetActorQuat();

	// Eleven is the expected count: the Rail body and the two terminal collars (see
	// AACPRRail::mTerminalMeshA), one empty zoop ISM per buildable mesh component, and the guidelines.
	// A further component announces itself here.
	if( meshes.Num() > 11 )
	{
		ACPR_LOG( Warning, RAIL_HOLO,
			TEXT( "%d mesh components, expected 11 (five buildable meshes, five zoop ISMs, guidelines). The cost, measured: "
			      "ONE EMPTY ZOOP ISM PER BUILDABLE MESH COMPONENT, so three components become "
			      "three ISMs plus the guidelines plus themselves. "
			      "If the preview stopped stretching, this line is why." ),
			meshes.Num() );
	}

}

void AACPRRailHologram::ClearAnchor()
{
	// All four together, from one place. Clearing only the pointer would leave the axis behind from a
	// previous frame — unobservable exactly as long as every consumer happens to check the pointer,
	// which is not the invariant: consumers gate on the axis.
	mAnchorTerminal = nullptr;
	mAnchorPoint = FVector::ZeroVector;
	mAnchorOutward = FVector::ZeroVector;
	mAnchorUp = FVector::ZeroVector;
}

const AACPRRail* AACPRRailHologram::GetRailCDO() const
{
	return GetBuildClass() ? Cast< AACPRRail >( GetBuildClass()->GetDefaultObject() ) : nullptr;
}

bool AACPRRailHologram::IsValidHitResult( const FHitResult& hitResult ) const
{
	// The widening is vanilla's own list, filled in BeginPlay (AddValidHitClass); the
	// shared verdict (ACPRHologramCommon.h) keeps AFGBeamHologram's own IsValidHitResult — whose body
	// cannot be read here — from bypassing it for a blocking hit on one of our hosts.
	const bool superSaid = Super::IsValidHitResult( hitResult );
	return ACPRHologram::WidenedHitVerdict( superSaid, hitResult,
		hitResult.GetActor() && IsValidHitActor( hitResult.GetActor() ), mValidityLog, ACPR_TAG_TEXT( RAIL_HOLO ) );
}

bool AACPRRailHologram::TrySnapToActor( const FHitResult& hitResult )
{
	// A bridge is never aimed. Nothing documents vanilla calling this on a child, but if it does, the
	// answer is "nothing snapped" and SetHologramLocationAndRotation below stands down too.
	if( mIsBridge )
	{
		return false;
	}

	// Step 1 only. Once the anchor is latched the aim means length, and a snap attempt here would
	// drag the already-placed near end to whatever the player happened to be pointing at.
	//
	// Who snaps the near end depends on what is aimed at. A Junction (anything that is not a beam):
	// vanilla's attachment-point pipeline, through Super — it filters our points, asks our type, places
	// with its own mating transform (measured identical to ours) and records the start;
	// AdoptVanillaSnapAsAnchor reads its result into the anchor state. A Rail: our own picker, because
	// AFGBeamHologram takes a private path for a BEAM target that never reaches the generic code and
	// mates the far end of the beam whichever end was aimed at. Super is NOT called for a Rail target
	// at all, so that path cannot place anything.
	if( !mAnchorLatched )
	{
		if( Cast< AACPRRail >( hitResult.GetActor() ) )
		{
			return TrySnapAnchorToTerminal( hitResult );
		}

		ClearAnchor();
		const bool snapped = Super::TrySnapToActor( hitResult );
		AdoptVanillaSnapAsAnchor();
		return snapped;
	}

	// The other path into vanilla, and it has to be constrained too.
	//
	// Returning true from here is the documented way to bypass SetHologramLocationAndRotation — which
	// is where the axis constraint lives. So if the beam's own snapping fires during the drag it would
	// place from an UNCONSTRAINED hit and suppress the only code that would have corrected it, and
	// nothing downstream catches that: PostHologramPlacement's re-assert is gated on !mAnchorLatched
	// and ApplyPlacementOffsets early-returns on it. Feeding Super the projected hit closes the
	// asymmetry for the cost of one copy.
	if( mAnchorLatched )
	{
		// §11 lane Rail: the frame is ours. Vanilla's building-snap chooses a length and a direction,
		// and a lane Rail's start, axis and length are all decided in DriveLane — which only runs if
		// this returns false. The far-end snap is attempted there too, once the axis (and so the ray it
		// must lie on) is known; attempting it here would measure from last frame's start.
		if( mLaneMode && !IsAnchorSnapped() )
		{
			return false;
		}

		// A snapped near end decides this without a far-end query. Once the anchor is on a terminal the
		// answer below is false whatever the far end does, and SetHologramLocationAndRotation — the
		// frame's one owner then — runs the far-end snap itself.
		if( IsAnchorSnapped() )
		{
			return false;
		}

		// A free near end: whether the far end snaps decides whether vanilla's building-snap may have the
		// frame, so it is asked here; when it does, SetHologramLocationAndRotation asks it once more on
		// the same hit — the same six-terminal pass rather than a per-frame result carried between two
		// of vanilla's virtuals.
		FHitResult adjusted = hitResult;
		const bool farSnapped = TryFarEndSnap( adjusted );

		// Returning false is what keeps the far-end snap in charge. Aimed at a Junction, the beam's own
		// building-snap returns true, takes the frame, and puts the far end on the far side of the
		// target's collision: 700 uu is its near face, 800 its far one. That is a rail built through the
		// junction to the other end's terminal, and 800 also lands the far end one whole metre past the
		// cell the snap chose, so no Coupling forms.
		//
		// Vanilla's building-snap chooses a LENGTH and a DIRECTION. §5.1 gives a Rail that has begun at
		// a terminal neither of those — and even with a free near end, a far-end snap we accepted has
		// already chosen the length. So once placement is ours, Super is not consulted: returning false
		// hands the frame to SetHologramLocationAndRotation, where the rewritten aim actually governs
		// and where vanilla still computes its own length from it.
		//
		// Super still runs in step 1, where the building-snap is what puts a free Rail on the world
		// grid and nothing of ours is competing with it.
		if( farSnapped )
		{
			return false;
		}

		return Super::TrySnapToActor( adjusted );
	}

	return Super::TrySnapToActor( hitResult );
}

UACPRTerminalComponent* AACPRRailHologram::FindAimedTerminal( const FHitResult& hitResult,
                                                              double& out_distance, int32& out_open,
                                                              int32& out_candidates, int32& out_tied ) const
{
	// A wrapper over FACPRTerminalPicker, which holds the body.
	//
	// "Which terminal is the player aiming at" is the Rail's near-end question, and §9's Cap, §8's
	// Outlet, the Junction's terminal snap and §10's resolver all ask the identical one. A copy per
	// hologram would be four copies of the face filter, the two-pass tie count and invariant 8, and
	// four chances to get one of them subtly different. The out-parameters are kept rather than
	// returning the struct, so the Rail's own call sites and its log line stay plain.
	const FACPRTerminalPick pick = FACPRTerminalPicker::FindAimed(
		hitResult, mTerminalSnapRange, mAmbiguityTolerance, mFaceAlignmentMinimum );

	out_distance = pick.Distance;
	out_open = pick.OpenInRange;
	out_candidates = pick.Candidates;
	out_tied = pick.Tied;

	return pick.Terminal;
}

bool AACPRRailHologram::TrySnapAnchorToTerminal( const FHitResult& hitResult )
{
	ClearAnchor();

	double bestDistance = 0.0;
	int32 openInRange = 0;
	int32 candidates = 0;
	int32 tied = 0;

	UACPRTerminalComponent* best = FindAimedTerminal( hitResult, bestDistance, openInRange, candidates, tied );

	if( !best )
	{
		// Ambiguity is worth a line; "nothing in range" is the ordinary state of every frame the player
		// is not pointing at a terminal, and logging that would drown the file.
		if( tied > 1 )
		{
			LogAnchorSnap( hitResult.GetActor(), nullptr, bestDistance, openInRange, candidates, tied );
		}
		return false;
	}

	// The placement, and it is entirely geometry.
	//
	// Terminal A sits at the actor origin with yaw 180, so its outward is the actor's local -X. For A
	// to oppose the target, local +X must BE the target's outward — and then the Rail extends from
	// the snap point along that axis, which is §5.1's "a snapped Rail continues along the terminal's
	// outward axis" falling out of the arithmetic rather than being imposed on top of it.
	//
	// MakeFromXZ rather than FVector::Rotation(): a bare direction invents the other two axes and
	// gimbal-locks on a vertical one. The target's own Z carries its roll across, which is also what
	// §5.2 wants at a Coupling.
	const FTransform targetFrame = best->GetComponentTransform();
	const FVector outward = targetFrame.GetUnitAxis( EAxis::X );
	const FVector targetUp = targetFrame.GetUnitAxis( EAxis::Z );

	SetActorLocationAndRotation( best->GetComponentLocation(),
		FRotationMatrix::MakeFromXZ( outward, targetUp ).Rotator() );

	mAnchorTerminal = best;
	mAnchorPoint = best->GetComponentLocation();
	mAnchorOutward = outward;
	mAnchorUp = targetUp;

	// AFTER the anchor fields, not after the transform write above: both of these are only meaningful
	// once IsAnchorSnapped() is true, and ClearAnchor() at the top of this function has just made it
	// false. PostHologramPlacement re-asserts the identical rotation later in the same call stack, but
	// the base is captured here regardless rather than left to something else.
	//
	// The anchor rotation is also the base the roll composes onto, captured here rather than left to
	// PostHologramPlacement so that scrolling BEFORE the first click rolls about the terminal's own
	// outward — which is what §5.2 means by roll carrying across a Coupling. ApplyRoll records the
	// result, so there is one writer and one record rather than two of each.
	CaptureRollBase( GetActorQuat() );
	ApplyRoll();

	LogAnchorSnap( hitResult.GetActor(), best, bestDistance, openInRange, candidates, tied );
	return true;
}

void AACPRRailHologram::ClearFarSnap()
{
	mFarTerminal = nullptr;
	mFarPoint = FVector::ZeroVector;
	mFarSnapped = false;
	mRequestedFarLength = -1.0f;
}

bool AACPRRailHologram::TryFarEndSnap( FHitResult& hitResult )
{
	ClearFarSnap();

	if( !hitResult.bBlockingHit )
	{
		return false;
	}

	AActor* hitActor = hitResult.GetActor();
	IACPRTerminalHost* host = hitActor ? Cast< IACPRTerminalHost >( hitActor ) : nullptr;
	if( !host )
	{
		return false;
	}

	// The far end does not ask the near end's question.
	//
	// FindAimedTerminal answers "which FACE is being pointed at", which is exactly right for the near
	// end: the player is choosing a face to stand the Rail on. The far end is a different question.
	// There the player is pointing at a BUILDING and the Rail must end on whichever of its faces the
	// Rail can actually reach and couple to — a choice the geometry makes, not the crosshair. Asking
	// the near end's question instead would, with the crosshair a little high on a Junction, return
	// its TOP face, which the opposition test refuses, and decline the whole snap while the face it
	// should use sits there qualifying on every rule.
	//
	// So the far end filters by its own three rules first and ranks what survives by nearness to the
	// aim. A Junction aimed at anywhere on its body ends the Rail on the face that can couple.
	const FVector origin = GetActorLocation();

	// FMath::Max, because the whole zero-length guard rests on this number and this number is Blueprint
	// data. AFGBuildableBeam::mSize has no in-class initializer (FGBuildableBeam.h:78), so a
	// Build_PowerRail_C that ever shipped with it at 0 would let a coincident terminal through the
	// range test and divide by zero below. One clamp costs nothing and stops the guard depending on an
	// editor field staying right.
	const AACPRRail* cdo = GetRailCDO();
	const double minimum = FMath::Max( 1.0, cdo ? static_cast< double >( cdo->GetSize() ) : 100.0 );
	const double maximum = cdo ? static_cast< double >( cdo->GetMaxLength() ) : 4000.0;

	int32 open = 0;
	int32 rejRange = 0;
	int32 rejOpposing = 0;
	int32 rejAxis = 0;

	// `count` is false on the tie pass so the rejection tallies describe the geometry once rather than
	// twice. The selection and the tie check are two queries over one set, and fusing them is what
	// makes the answer depend on terminal index order.
	auto Qualifies = [ & ]( UACPRTerminalComponent* terminal, double& out_length, double& out_residual,
	                        bool count ) -> bool
	{
		out_length = 0.0;
		out_residual = 0.0;

		// The anchor's own terminal is not a far-end candidate. The near end is standing on it, so it
		// is zero away and the range rule would refuse it — correctly, but as a side effect rather than
		// a rule, and with `length=0.0` log lines that look like a failure and are not. Excluding it by
		// identity says what is meant.
		// IsPrivateInterface is §4's private mounting interface — "not a continuation terminal, not a
		// blueprint candidate, and cannot accept a Rail". The far end is exactly a Rail continuing, so
		// it is refused here as well as in FACPRTerminalPicker. This search does its own selection and
		// would otherwise be the one place in the mod the rule did not reach.
		if( !terminal || !terminal->IsOpen() || terminal->IsPrivateInterface() ||
			terminal == mAnchorTerminal.Get() )
		{
			return false;
		}

		if( count )
		{
			++open;
		}

		const FVector delta = terminal->GetComponentLocation() - origin;
		const double length = delta.Size();
		out_length = length;

		if( length < minimum || length > maximum )
		{
			if( count ) { ++rejRange; }
			return false;
		}

		// Invariant 4's opposition. Terminal B's outward is the Rail's local +X, which is `direction`,
		// so the target's outward must be its negative or §6.1 will refuse the pair a tick later.
		const FVector direction = delta / length;
		const double opposition =
			FVector::DotProduct( terminal->GetOutwardAxis().GetSafeNormal(), direction );

		if( opposition > -mFaceAlignmentMinimum )
		{
			out_residual = opposition;
			if( count ) { ++rejOpposing; }
			return false;
		}

		// With a snapped near end the axis is already decided and §5.1 gives the drag only length and
		// roll, so the target has to be ON that axis. With a free near end there is no axis yet and the
		// aim is allowed to choose one — "the second end may snap even if the first end was placed
		// without snapping", falling out of the same rule rather than needing its own path.
		//
		// A §11 lane Rail has an axis too — the lane DriveLane just chose, measured from the lane start
		// DriveLane has just placed the actor on. So a far end on the lane snaps (at its exact length, a
		// terminal snap winning over whole metres), and one off the lane does not bend the Rail off it.
		const FVector constraint = IsAnchorSnapped()
			? mAnchorOutward
			: ( IsLaneDriving() ? mLaneAxis : FVector::ZeroVector );

		if( !constraint.IsNearlyZero() )
		{
			const double along = FVector::DotProduct( delta, constraint );
			const double offAxis = FVector::Dist( delta, constraint * along );
			out_residual = offAxis;

			if( along <= 0.0 || offAxis > mAxisTolerance )
			{
				if( count ) { ++rejAxis; }
				return false;
			}
		}

		return true;
	};

	const int32 terminalCount = host->GetTerminalCount();

	UACPRTerminalComponent* best = nullptr;
	double bestAim = 0.0;
	double bestLength = 0.0;
	double bestResidual = 0.0;
	int32 survivors = 0;

	for( int32 i = 0; i < terminalCount; ++i )
	{
		UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( static_cast< uint8 >( i ) );

		double length = 0.0;
		double residual = 0.0;
		if( !Qualifies( terminal, length, residual, true ) )
		{
			continue;
		}

		++survivors;

		const double aim = FVector::Dist( terminal->GetComponentLocation(), hitResult.ImpactPoint );
		if( !best || aim < bestAim )
		{
			best = terminal;
			bestAim = aim;
			bestLength = length;
			bestResidual = residual;
		}
	}

	int32 tied = 0;
	if( best )
	{
		for( int32 i = 0; i < terminalCount; ++i )
		{
			UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( static_cast< uint8 >( i ) );

			double length = 0.0;
			double residual = 0.0;
			if( Qualifies( terminal, length, residual, false ) &&
				FMath::Abs( FVector::Dist( terminal->GetComponentLocation(), hitResult.ImpactPoint ) - bestAim )
					<= mAmbiguityTolerance )
			{
				++tied;
			}
		}
	}

	if( !best )
	{
		// Only worth a line when the host had something to offer and a rule ate it. A Junction with no
		// open faces at all is the ordinary state of a finished network.
		if( rejRange + rejOpposing + rejAxis > 0 )
		{
			LogFarSnap( hitActor, nullptr, TEXT( "no candidate" ), 0.0, 0.0, open, 0, 0,
				rejRange, rejOpposing, rejAxis );
		}
		return false;
	}

	// Invariant 8, and it can genuinely happen here: two opposite faces of one Junction both qualify
	// when the Rail is aimed end-on at a cube that is open on both sides.
	if( tied > 1 )
	{
		LogFarSnap( hitActor, best, TEXT( "REFUSED (ambiguous - invariant 8)" ), bestLength, bestResidual,
			open, survivors, tied, rejRange, rejOpposing, rejAxis );
		return false;
	}

	// §5.1: "A terminal snap considers only the first terminal encountered along the Rail's axis; if it
	// is occupied or non-opposing it blocks, and the search never falls through to a farther one."
	//
	// This is the through-build guard. Without it, a Junction whose Rail-facing terminal is capped has
	// the Rail built straight through the cube to the open face on the far side, and a capped Rail is
	// traversed end to end. The ranking above is by nearness to the AIM POINT, which is right for
	// choosing which face of a building the player means — and it is blind to anything standing
	// between the near end and that face, because a capped terminal never enters the candidate set at
	// all. Its absence reads as clear space.
	//
	// So the aim chooses the DIRECTION and the geometry then chooses the FACE. With a direction fixed,
	// the host's faces have an unambiguous order along it, and only the first one counts. Two outcomes
	// follow and both are correct: a nearer face that qualifies becomes the target (the aim merely
	// pointed past it), and a nearer face that does not refuses the snap outright.
	//
	// This cannot be part of the loop above. "First along the axis" needs an axis, and with a FREE
	// near end there is no axis until a candidate has been chosen — §5.1's "the second end may snap
	// even if the first end was placed without snapping". Two stages is not a stylistic choice here.
	{
		const FVector dir = ( best->GetComponentLocation() - origin ).GetSafeNormal();

		UACPRTerminalComponent* first = best;
		double firstAlong = bestLength;

		for( int32 i = 0; i < terminalCount; ++i )
		{
			UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( static_cast< uint8 >( i ) );

			// §4's private mounting interface is not a physical face — it is an Outlet's internal
			// coupling point, sitting on the very cell of the terminal it mounts to. Letting one block
			// would mean an Outlet on a Rail's end made that whole Rail unreachable, and it would block
			// at exactly the distance of the face it shares, which is a coin toss rather than a rule.
			if( !terminal || terminal == mAnchorTerminal.Get() || terminal->IsPrivateInterface() )
			{
				continue;
			}

			const FVector delta = terminal->GetComponentLocation() - origin;
			const double along = FVector::DotProduct( delta, dir );

			if( along <= 0.0 )
			{
				continue;
			}

			// Off-axis faces are passed BESIDE, not through. mAxisTolerance is 5 cm and a Junction's
			// transverse faces sit 45 cm off the centreline, so the four faces a Rail runs past are
			// never mistaken for the two it runs into.
			if( FVector::Dist( delta, dir * along ) > mAxisTolerance )
			{
				continue;
			}

			if( along < firstAlong - UE_KINDA_SMALL_NUMBER )
			{
				first = terminal;
				firstAlong = along;
			}
		}

		if( first != best )
		{
			double length = 0.0;
			double residual = 0.0;

			if( !Qualifies( first, length, residual, false ) )
			{
				// The refusal is the feature: the snap declines and §12's markers show which face is in
				// the way by NOT lighting up.
				LogFarSnap( hitActor, first, TEXT( "REFUSED (blocked by nearer terminal - 5.1)" ),
					firstAlong, 0.0, open, survivors, tied, rejRange, rejOpposing, rejAxis );
				return false;
			}

			// It qualifies on its own account, so it is the target. Nothing about the aim is discarded
			// — the aim is what fixed the direction this was measured along.
			//
			// The counts printed below (open/cand/tied) describe the AIM PICK, not this one, and that
			// is deliberate: they are the record of how the direction was chosen, which is the part a
			// reader needs in order to understand why a farther face was under the crosshair at all.
			LogFarSnap( hitActor, first, TEXT( "RETARGETED (nearer terminal on axis - 5.1)" ),
				length, residual, open, survivors, tied, rejRange, rejOpposing, rejAxis );

			best = first;
			bestLength = length;
			bestResidual = residual;
		}
	}

	// The rewrite: the far end is the length, length is what vanilla derives from the hit point, so
	// the snap is an exact aim point rather than a correction applied afterwards.
	mFarTerminal = best;
	mFarPoint = best->GetComponentLocation();
	mFarSnapped = true;
	mRequestedFarLength = static_cast< float >( bestLength );

	// The aim handed to vanilla is half a cell short of the terminal. Measured per far-snapped frame:
	// vanilla's length is the next whole cell above |aim − its own start|, and when ITS snap placed the
	// start (the attachment route) that start is the anchor exactly — so a far end 800 from it would
	// read 900. A point half a cell back lands inside the right cell for every whole-metre snap; off
	// the lattice (7.5 m) vanilla can only show 8 m and the build stays exact. mFarPoint keeps the true
	// point for everything of ours.
	{
		const FVector direction = ( mFarPoint - origin ).GetSafeNormal();
		const FVector handed = mFarPoint - direction * FMath::Min( 0.5 * minimum, 0.5 * bestLength );
		hitResult.ImpactPoint = handed;
		hitResult.Location = handed;
	}

	LogFarSnap( hitActor, best, TEXT( "ACCEPTED" ), bestLength, bestResidual, open, survivors, tied,
		rejRange, rejOpposing, rejAxis );
	return true;
}

void AACPRRailHologram::LogFarSnap( const AActor* host, const UACPRTerminalComponent* terminal,
                                    const TCHAR* verdict, double length, double residual, int32 open,
                                    int32 survivors, int32 tied, int32 rejRange, int32 rejOpposing,
                                    int32 rejAxis )
{
	if( !mFarSnapLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	// Keyed on the verdict and the target, not on the length: a drag towards a terminal changes the
	// length every frame and would otherwise print a line per frame for a refusal that never changes.
	// The host is in the key because a refusal has no terminal, and without it every "no candidate"
	// in a session would collapse onto the first one. A key that cannot distinguish two events reports
	// one of them and silently eats the other.
	const FString key = FString::Printf( TEXT( "%s|%s|%s" ),
		verdict,
		host ? *host->GetName() : TEXT( "-" ),
		terminal ? *terminal->GetName() : TEXT( "-" ) );

	if( !mFarSnapLog.Admit( key ) )
	{
		return;
	}

	// rej= is the diagnostic. "The second end will not snap" has three distinct causes and they are
	// indistinguishable from outside the game; the tally says which rule ate the faces rather than
	// only that something did. far= is printed quantised, in the form the registry files terminals
	// under, so it can be compared by eye against the COUPLED line a tick later.
	ACPR_LOG( Display, RAIL_HOLO,
		TEXT( "far-snap %s | target=%s host=%s | length=%.1f residual=%.1f "
		      "| open=%d survivors=%d tied=%d rej(range=%d opposing=%d axis=%d) | anchored=%d lane=%d "
		      "| far=%s origin=%s" ),
		verdict,
		terminal ? *terminal->GetName() : TEXT( "<none>" ),
		host ? *host->GetName() : TEXT( "-" ),
		length, residual,
		open, survivors, tied,
		rejRange, rejOpposing, rejAxis,
		IsAnchorSnapped() ? 1 : 0,
		IsLaneDriving() ? 1 : 0,
		terminal ? *terminal->GetQuantizedLocation().ToString() : TEXT( "-" ),
		*UACPRTerminalComponent::QuantizeLocation( GetActorLocation() ).ToString() );
}

void AACPRRailHologram::ConstrainAimToAxis( FHitResult& hitResult ) const
{
	if( mAnchorOutward.IsNearlyZero() )
	{
		return;
	}

	// §11's 1 m minimum and 40 m maximum, read off the beam rather than written down again. GetSize
	// IS the 1 m cell the beam quantises its own length to, so using it as the floor keeps one number
	// in one place instead of two that can disagree.
	const AACPRRail* cdo = GetRailCDO();
	const double minimum = cdo ? static_cast< double >( cdo->GetSize() ) : 100.0;
	const double maximum = cdo ? static_cast< double >( cdo->GetMaxLength() ) : 4000.0;

	// Only the component ALONG the axis survives. Everything perpendicular is discarded, which is
	// what makes the direction inherited rather than chosen — the player is aiming at a length.
	const double along = FMath::Clamp(
		FVector::DotProduct( hitResult.ImpactPoint - mAnchorPoint, mAnchorOutward ),
		minimum, maximum );

	const FVector projected = mAnchorPoint + mAnchorOutward * along;

	// Recorded because CheckValidPlacement has no other way to know how long this Rail is about to be.
	// `along` is the number vanilla derives mCurrentLength from (B.2) — so this is the same value one
	// step earlier rather than an estimate of it. Mutable because this function is const and is the
	// only place the value exists.
	mAimAlong = along;

	// Both, because vanilla reads one or the other depending on the path. ImpactPoint is what the
	// beam's own placement uses; Location is what a snap would.
	hitResult.ImpactPoint = projected;
	hitResult.Location = projected;
}

void AACPRRailHologram::LogAnchorSnap( const AActor* hitActor, const UACPRTerminalComponent* terminal,
                                       double distance, int32 openInRange, int32 candidates, int32 tied )
{
	if( !mSnapLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	const FString key = FString::Printf( TEXT( "%s|%s|%d" ),
		hitActor ? *hitActor->GetName() : TEXT( "-" ),
		terminal ? *terminal->GetName() : TEXT( "<refused>" ),
		tied );

	if( !mSnapLog.Admit( key ) )
	{
		return;
	}

	// anchor= must equal target= exactly, or the near terminal will not land on the target's cell and
	// no Coupling forms a tick after construction. axis= is what step 2 is then confined to.
	ACPR_LOG( Display, RAIL_HOLO,
		TEXT( "anchor-snap %s | host=%s terminal=%s | dist=%.1f open=%d cand=%d "
		      "tied=%d | anchor=%s target=%s | axis=%s rot=%s" ),
		terminal ? TEXT( "ACCEPTED" ) : TEXT( "REFUSED (ambiguous — invariant 8)" ),
		hitActor ? *hitActor->GetName() : TEXT( "<none>" ),
		terminal ? *terminal->GetName() : TEXT( "-" ),
		distance, openInRange, candidates, tied,
		*UACPRTerminalComponent::QuantizeLocation( GetActorLocation() ).ToString(),
		terminal ? *terminal->GetQuantizedLocation().ToString() : TEXT( "-" ),
		*mAnchorOutward.ToString(),
		*GetActorRotation().ToString() );
}

void AACPRRailHologram::LogConstrainedAim( const FHitResult& raw, const FHitResult& constrained,
                                           const FTransform& beforeSuper, const FTransform& afterSuper,
                                           const FRotator& pinned ) const
{
	if( !mAimLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	const double along = FVector::DotProduct( constrained.ImpactPoint - mAnchorPoint, mAnchorOutward );

	// Keyed on the resulting length in 50 uu steps, so a held aim logs once and a drag logs a readable
	// trail: at one line per uu of drag the budget would buy about a metre of travel and then go
	// silent. The far-end state is in the key as well as the length, or the frame the snap takes hold
	// shares a bucket with the frame before it and never prints.
	const FString key = FString::Printf( TEXT( "%d|%d|%s" ),
		FMath::RoundToInt32( along / 50.0 ),
		mFarSnapped ? 1 : 0,
		mFarTerminal.IsValid() ? *mFarTerminal->GetName() : TEXT( "-" ) );
	if( !mAimLog.Admit( key ) )
	{
		return;
	}

	// Two numbers, because one alone would measure two things. offAxis= is the genuine perpendicular
	// part — how far off the ray the player was aiming. moved= is the total displacement, which equals
	// offAxis until the clamp bites and then also carries the along-axis correction. Reporting only
	// the total would make "aiming 20 cm short of the 1 m minimum" read identically to "aiming 20 cm
	// off the axis", and the clamp is exactly where a reader would most want to tell those apart.
	const double rawAlong = FVector::DotProduct( raw.ImpactPoint - mAnchorPoint, mAnchorOutward );
	const double offAxis = FVector::Dist( raw.ImpactPoint, mAnchorPoint + mAnchorOutward * rawAlong );
	const double moved = FVector::Dist( raw.ImpactPoint, constrained.ImpactPoint );
	const bool clamped = !FMath::IsNearlyEqual( rawAlong, along, 0.5 );

	// vanillaRot= is what AFGBeamHologram decided the Rail should face after being handed a hit that
	// lies exactly on the axis. If it differs from pinned=, vanilla is taking the direction from
	// something other than the hit point, and the pin is what corrects it. movedBy= is how far vanilla
	// shifted the actor — non-zero would mean pinning the rotation alone is not sufficient either.
	ACPR_LOG( Verbose, RAIL_HOLO,
		TEXT( "aim constrained | along=%.1f (raw %.1f, clamped=%d) | offAxis=%.1f "
		      "moved=%.1f | anchor=%s axis=%s | raw=%s -> %s "
		      "| vanillaRot=%s pinned=%s | movedBy=%.1f anchorHeld=%d" ),
		along, rawAlong, clamped ? 1 : 0, offAxis, moved,
		*mAnchorPoint.ToString(),
		*mAnchorOutward.ToString(),
		*raw.ImpactPoint.ToString(),
		*constrained.ImpactPoint.ToString(),
		*afterSuper.Rotator().ToString(),
		*pinned.ToString(),
		FVector::Dist( beforeSuper.GetLocation(), afterSuper.GetLocation() ),
		GetActorLocation().Equals( mAnchorPoint, 1.0 ) ? 1 : 0 );
}

UStaticMeshComponent* AACPRRailHologram::GetPreviewMesh() const
{
	if( mPreviewMesh.IsValid() )
	{
		return mPreviewMesh.Get();
	}

	// The cached component rotted (nothing observed regenerates it; this is the fallback, not the path).
	// Re-derive it from the asset recorded at BeginPlay, never from "any plain mesh": locking the
	// hologram spawns the nudge gizmo out of ordinary static meshes, and a scale written into those
	// shows in the preview.
	if( !mPreviewMeshAsset.IsValid() )
	{
		return nullptr;
	}

	TArray< UStaticMeshComponent* > meshes;
	GetComponents< UStaticMeshComponent >( meshes );
	for( UStaticMeshComponent* m : meshes )
	{
		if( m && !m->IsA< UInstancedStaticMeshComponent >() && m->GetStaticMesh() == mPreviewMeshAsset.Get() )
		{
			mPreviewMesh = m;
			return m;
		}
	}
	return nullptr;
}

void AACPRRailHologram::ApplyLengthToPreview()
{
	// The preview has to show the length the built Rail will have. ConfigureActor makes the BUILT Rail
	// exactly the snapped length; vanilla's own length is quantised, so a Junction 7 m away would show
	// an 8 m Rail reaching through it right up until the click.
	//
	// The length is always written by us. Whatever AFGBeamHologram scales, it is not this component
	// (measured: thirty readings while dragging from a Junction, every one with RailMesh at scale
	// (1,1,1)) — it is OUR RailMesh, copied from the buildable CDO, and only its rotation is vanilla's
	// business. mCurrentLength is AFGBeamHologram's own length (FGBeamHologram.h:81, private, reached
	// through the AccessTransformers Friend entry) — the same value ConfigureActor reads back from the
	// constructed Rail as GetLength(). Ours when we have one (GetOwnLength: a far-end snap, a §11 lane
	// Rail, a bridge), vanilla's otherwise.
	const AACPRRail* cdo = GetRailCDO();
	if( !cdo )
	{
		return;
	}

	const double defaultLength = cdo->GetDefaultLength();
	const double ownLength = GetOwnLength();
	const double previewLength = ( ownLength > 0.0 ) ? ownLength : static_cast< double >( mCurrentLength );
	if( previewLength <= 0.0 || defaultLength <= 0.0 )
	{
		// Before the first placement mCurrentLength is 0: nothing to show yet, the component is left alone.
		return;
	}

	const double z = previewLength / defaultLength;
	if( FMath::IsNearlyEqual( z, mPreviewScaleZ, 1e-6 ) )
	{
		return;
	}

	UStaticMeshComponent* preview = GetPreviewMesh();
	if( !preview )
	{
		return;
	}

	// The whole vector, absolutely: X and Y are the mesh's own (the body is authored at the §11
	// profile), Z is the length. Idempotent whatever calls it.
	const FVector current = preview->GetRelativeScale3D();
	preview->SetRelativeScale3D( FVector( current.X, current.Y, z ) );
	mPreviewScaleZ = z;
}

void AACPRRailHologram::CheckRailPlacement()
{
	{
		// On a bridge, how much of bridge.validate is vanilla's clearance pass.
		TOptional< FACPRCostScope > vanillaCost;
		if( mIsBridge )
		{
			vanillaCost.Emplace( EACPRCost::BridgeValidateVanilla );
		}
		Super::CheckValidPlacement();
	}

	// §10.1, before the step-2 early return below, because the anchor is decided in step 1 and a
	// Designer Rail anchored on a world terminal is refused from the first frame it snaps, not only
	// once it is being dragged. Both ends are asked: the anchor from step 1, the far end from step 2.
	//
	// Vanilla's own disqualifier, so the message is the one vanilla already shows for its own
	// Designer-to-world connections — "Buildables in a designer cannot be connected to world
	// buildables". Vanilla adds it on some aims and not on others (never through IsValidHitResult);
	// ours is unconditional for any terminal target.
	{
		const AFGBuildableBlueprintDesigner* space = GetPlacementDesigner();
		const AActor* anchorHost = ( IsAnchorSnapped() && mAnchorTerminal.IsValid() )
			? mAnchorTerminal->GetOwner() : nullptr;
		const AActor* farHost = ( mFarSnapped && mFarTerminal.IsValid() )
			? mFarTerminal->GetOwner() : nullptr;

		// Evaluated separately so both are logged when both are foreign; || would skip the second.
		const bool anchorForeign = FACPRSpace::IsForeignTarget( this, TEXT( "anchor" ), anchorHost, space, mLastSpaceLog );
		const bool farForeign = FACPRSpace::IsForeignTarget( this, TEXT( "far end" ), farHost, space, mLastSpaceLogFar );

		if( anchorForeign || farForeign )
		{
			AddConstructDisqualifier( UFGCDDesignerWorldCommingling::StaticClass() );
		}
	}

	// Step 2 only, every step-2 Rail, and the scope is the rule rather than a shortcut.
	//
	// §5.1's clause is about what a Rail may continue THROUGH: a Rail through two of a Junction's faces
	// with no Coupling at either is the picture §5.1 and §7.4 both refuse, because it implies a coupling
	// that does not exist, and the Junction has no clearance box that would catch it otherwise. Before
	// the first click there is no axis and no length — vanilla owns both — so there is nothing to test.
	// Once placed, the line and length of ANY Rail are known: a snapped anchor's axis, a §11 lane
	// Rail's lane, a far-snapped Rail's line to its terminal, or the actor's X and vanilla's own
	// mCurrentLength for a free-hand one. One rule, no modes. The drag stops short, or aims at the
	// Junction and the far-end snap takes it.
	const bool laneSpan = !IsAnchorSnapped() && IsLaneDriving();
	const bool farSpan = !IsAnchorSnapped() && !laneSpan && mFarSnapped && mRequestedFarLength > 0.0f;
	if( !mAnchorLatched )
	{
		return;
	}

	// The length that will actually be built, which is not always the length being aimed at.
	//
	// Far-snapped: ConfigureActor calls SetLength( mRequestedFarLength ), so that is the real one. A
	// lane Rail's length is always ours (GetOwnLength: its far snap or its whole metres); mAimAlong is
	// the anchored path's number and a lane Rail never writes it. Otherwise it is vanilla's
	// mCurrentLength — the number its ConfigureActor hands SetLength (read back as GetLength), which
	// is read through the Friend entry rather than estimated from the aim: an estimate is wrong at
	// exact multiples of the cell.
	const double length = laneSpan
		? GetOwnLength()
		: ( ( mFarSnapped && mRequestedFarLength > 0.0f )
			? static_cast< double >( mRequestedFarLength )
			: static_cast< double >( mCurrentLength ) );

	if( length <= 0.0 )
	{
		return;
	}

	const FVector origin = laneSpan ? mLaneStart : ( IsAnchorSnapped() ? mAnchorPoint : GetActorLocation() );
	const FVector axis = laneSpan ? mLaneAxis
		: ( IsAnchorSnapped() ? mAnchorOutward
			: ( farSpan ? ( mFarPoint - GetActorLocation() ).GetSafeNormal() : GetActorForwardVector() ) );
	if( axis.IsNearlyZero() )
	{
		return;
	}

	// The registry, not a line trace: LineTraceMulti stops at the first BLOCKING hit, and a trace that
	// starts on the anchor host's own surface ends where it began, reporting a span that crosses a
	// whole Junction as clear. See UACPRTerminalRegistry::FindAlongAxis.
	const UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this );
	if( !registry )
	{
		return;
	}

	TArray< UACPRTerminalComponent* > candidates;

	// The bounds ARE the rule. Starting at mAxisTolerance excludes the anchor's own terminal at
	// zero and anything behind the near end; stopping mSpanEndTolerance short of the far end
	// excludes the terminal the Rail is meant to end ON, which §5.1 permits and which vanilla's
	// length quantisation means may be a few uu adrift of the exact end.
	registry->FindAlongAxis( origin, axis,
	                         mAxisTolerance,
	                         length - mSpanEndTolerance,
	                         mAxisTolerance,
	                         candidates );

	const UACPRTerminalComponent* blocker = nullptr;
	double blockerAlong = 0.0;

	for( const UACPRTerminalComponent* terminal : candidates )
	{
		// §4's private mounting interface is not a face — it sits on the cell of the terminal it
		// mounts to, so letting it block would make an Outlet on a Rail's end block that whole Rail
		// at a distance it shares with something else. The anchor's own terminal is excluded by the
		// bounds above; excluding it again by identity costs nothing and says what is meant.
		if( !terminal || terminal->IsPrivateInterface() || terminal == mAnchorTerminal.Get() )
		{
			continue;
		}

		const double along =
			FVector::DotProduct( terminal->GetComponentLocation() - origin, axis );

		if( !blocker || along < blockerAlong )
		{
			blocker = terminal;
			blockerAlong = along;
		}
	}

	if( !blocker )
	{
		// Only when the span actually had something in it. A drag across open ground produces a
		// clear span on every frame and would spend the whole budget saying so; a span that found
		// candidates and rejected every one is the case worth a line. Keyed in 50 uu buckets, so a
		// drag prints a trail rather than a line per frame.
		if( candidates.Num() > 0 )
		{
			LogBlocked( nullptr, 0.0, length, candidates.Num(), TEXT( "clear" ) );
		}
		return;
	}

	// The fallback is not belt-and-braces: it is what stops a null property from turning the rule
	// off. §5.1 has no valid Rail that runs through a terminal, whatever this field is edited to.
	AddConstructDisqualifier( mBlockedDisqualifier
		? mBlockedDisqualifier
		: TSubclassOf< UFGConstructDisqualifier >( UFGCDInvalidAimLocation::StaticClass() ) );

	LogBlocked( blocker, blockerAlong, length, candidates.Num(), TEXT( "BLOCKED" ) );
}

void AACPRRailHologram::LogBlocked( const UACPRTerminalComponent* blocker, double along, double length,
                                    int32 candidates, const TCHAR* verdict )
{
	if( !mBlockLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	// The length is in the key in 50 uu buckets, because the interesting transition is the frame a
	// drag crosses a face — and keying on the blocker alone would collapse the whole drag onto its
	// first frame.
	const FString key = FString::Printf( TEXT( "%s|%s|%d" ),
		verdict,
		blocker ? *blocker->GetName() : TEXT( "-" ),
		FMath::RoundToInt32( length / 50.0 ) );

	if( !mBlockLog.Admit( key ) )
	{
		return;
	}

	// state= is the message §5.1 wants named. cand= is the tripwire: a run of `clear` lines with
	// cand=0 while the Rail is visibly crossing a Junction means the SEARCH is blind, not that the
	// path is free. The span's own origin and axis: the anchor's, or a §11 lane Rail's.
	const bool lane = !IsAnchorSnapped() && IsLaneDriving();
	const FVector spanOrigin = lane ? mLaneStart : mAnchorPoint;
	const FVector spanAxis = lane ? mLaneAxis : mAnchorOutward;

	const TCHAR* state = blocker
		? ( blocker->IsOpen() ? TEXT( "Open" )
			: ( blocker->GetTerminalState() == EACPRTerminalState::Capped ? TEXT( "Capped" )
				: TEXT( "Coupled" ) ) )
		: TEXT( "-" );

	// A refusal stays visible; a clear span is settled instrumentation and goes to Verbose. UE_LOG's
	// verbosity is a compile-time token, hence two statements for one format.
	if( blocker )
	{
		ACPR_LOG( Display, RAIL_HOLO,
			TEXT( "span %s | blocker=%s state=%s | along=%.1f of length=%.1f "
			      "| cand=%d | origin=%s axis=%s lane=%d" ),
			verdict, *blocker->GetName(), state, along, length, candidates,
			*spanOrigin.ToString(), *spanAxis.ToString(), lane ? 1 : 0 );
	}
	else
	{
		ACPR_LOG( Verbose, RAIL_HOLO,
			TEXT( "span %s | blocker=%s state=%s | along=%.1f of length=%.1f "
			      "| cand=%d | origin=%s axis=%s lane=%d" ),
			verdict, TEXT( "<none>" ), state, along, length, candidates,
			*spanOrigin.ToString(), *spanAxis.ToString(), lane ? 1 : 0 );
	}
}

AFGBuildableBlueprintDesigner* AACPRRailHologram::GetPlacementDesigner() const
{
	// A bridge's space is the placement's, handed over by the manager. A child hologram's own
	// mBlueprintDesigner is whatever SpawnChildHologramFromRecipe left there, which nothing documents.
	return mIsBridge ? mBridgeSpace.Get() : mBlueprintDesigner.Get();
}

void AACPRRailHologram::CheckValidPlacement()
{
	// The hand-placed Rail's validator, for both kinds of Rail. Invariant 10: "generated auto-connect
	// geometry uses the same validator as manual construction — no stricter, no looser".
	//
	// But not every frame for a bridge. Vanilla runs CheckValidPlacement on every child hologram every
	// frame, and for a Rail that means CheckClearance: overlap tests against everything in reach, which
	// inside a blueprint hologram is every one of ITS buildables. Measured: 20 bridges in a large
	// blueprint +30 ms a frame, 100 bridges in a 1575-buildable one +1000 ms — bridges × blueprint, not
	// bridges. A bridge's placement depends only on its two endpoints, and those change only when the
	// manager evaluates (ValidateBridgeNow, on movement); between evaluations nothing has moved relative
	// to it. So outside a manager validation the verdict from the last one is replayed and the validator
	// is not run — invariant 10 holds: the SAME validator, at the moments its inputs change. The bridge's
	// own disqualifiers (fault, parent block) are still added below, every frame.
	if( mIsBridge && !mInManagerValidation )
	{
		for( const TSubclassOf< UFGConstructDisqualifier >& cached : mCachedBridgeDisqualifiers )
		{
			AddConstructDisqualifier( cached );
		}
	}
	else
	{
		CheckRailPlacement();
	}

	if( !mIsBridge )
	{
		return;
	}

	// §10.4's geometry verdict. The manager has already decided whether this pair is on-axis, opposing,
	// open and in range; a latched pair that stops being so stays visible and turns red rather than
	// retargeting (§10.2 step 3), and this is what turns it red.
	if( mBridgeFaulted )
	{
		AddConstructDisqualifier( mBlockedDisqualifier
			? mBlockedDisqualifier
			: TSubclassOf< UFGConstructDisqualifier >( UFGCDInvalidAimLocation::StaticClass() ) );
	}

	// §10.2 step 4: "A single red one blocks the whole placement." Only from vanilla's own pass over its
	// children — that pass runs inside the parent's validation, after the parent has reset its list, so
	// an entry added here lives exactly one validation cycle. AddConstructDisqualifier is the same
	// FGHologram.h:395 export every hologram of ours already calls; GetParentHologram is FORCEINLINE.
	//
	// !IsDisabled(): a hidden bridge is not part of the placement and must never block it, whatever state
	// it was last left in. The disqualifier is this Rail's own (mBlockedDisqualifier, the class every
	// hologram of ours uses).
	if( !mInManagerValidation && mBridgeLatched && !IsDisabled() && !CanConstruct() )
	{
		if( AFGHologram* parent = GetParentHologram() )
		{
			parent->AddConstructDisqualifier( mBlockedDisqualifier
				? mBlockedDisqualifier
				: TSubclassOf< UFGConstructDisqualifier >( UFGCDInvalidAimLocation::StaticClass() ) );
		}
	}
}

void AACPRRailHologram::SetActorHiddenInGame( bool newHidden )
{
	TOptional< FACPRCostScope > hiddenCost;
	if( mIsBridge )
	{
		hiddenCost.Emplace( EACPRCost::BridgeHidden );
	}
	Super::SetActorHiddenInGame( newHidden );
}

void AACPRRailHologram::MarkAsBridge()
{
	mIsBridge = true;

	// Nothing to aim at, so nothing to stand on (see CheckValidFloor).
	mNeedsValidFloor = false;
}

void AACPRRailHologram::ApplyBridgeSpec( const FACPRBridgeSpec& spec )
{
	mIsBridge = true;
	mNeedsValidFloor = false;
	mBridgeFaulted = spec.Faulted;
	mBridgeSpace = spec.Space;

	// The child's own record of its space, so ConfigureActor puts a bridge built inside a Designer into
	// that Designer exactly as it would a hand-placed Rail's (AFGHologram::mBlueprintDesigner,
	// FGHologram.h:776, protected — written directly, no setter import).
	mBlueprintDesigner = spec.Space;

	const FVector outward = spec.Outward.GetSafeNormal();
	const FVector up = spec.Up.IsNearlyZero() ? FVector::UpVector : spec.Up.GetSafeNormal();

	// The anchor and far-end state a hand-placed Rail would have after a snapped first click and a
	// far-end snap — so CheckRailPlacement's §5.1 span test, its §10.1 far-end space test and
	// ConfigureActor's exact-length correction all run on a bridge exactly as they do by hand. There is
	// no world terminal under the near end (the BP terminal lives in the blueprint's private world), so
	// the anchor pointer stays null; every consumer already tests it before use.
	mAnchorTerminal = nullptr;
	mAnchorPoint = spec.Start;
	mAnchorOutward = outward;
	mAnchorUp = up;
	mAnchorLatched = true;

	mFarTerminal = spec.FarTerminal;
	mFarPoint = spec.Start + outward * spec.Length;
	mFarSnapped = spec.Length > 0.0;
	mRequestedFarLength = static_cast< float >( spec.Length );
	mAimAlong = spec.Length;

	// §5.2 — the roll is the BP side's, carried in `up`; the scroll accumulator has no say on a bridge.
	mRollDegrees = 0.0;

	// §10.6: "inherits BP-side customization". AFGBuildableHologram::mCustomizationData
	// (FGBuildableHologram.h:555, protected) is what ConfigureActor hands the buildable on the hand path;
	// setting it here puts the bridge on that same path. The manager checks the built Rail afterwards.
	if( spec.Customization )
	{
		mCustomizationData = *spec.Customization;
	}

	ApplyBridgeTransform();
	ApplyLengthToPreview();
}

void AACPRRailHologram::ApplyBridgeTransform()
{
	// No spec yet (a cascade reaching a bridge between its spawn and its first ApplyBridgeSpec): there is
	// no frame to put it on, and MakeFromXZ of a zero axis is not a rotation.
	if( !mIsBridge || mAnchorOutward.IsNearlyZero() )
	{
		return;
	}

	// The one write of a bridge's place. Only when the spec moved it: a re-applied spec on a hundred
	// unmoved bridges is a hundred no-ops, not a hundred actor moves.
	const FQuat wanted = FRotationMatrix::MakeFromXZ( mAnchorOutward, mAnchorUp ).ToQuat();
	if( FVector::Dist( GetActorLocation(), mAnchorPoint ) > 0.1 ||
		FMath::RadiansToDegrees( GetActorQuat().AngularDistance( wanted ) ) > 0.1 )
	{
		SetActorLocationAndRotation( mAnchorPoint, wanted );
	}
	CaptureRollBase( wanted );
	RecordAppliedRotation();

	// The build space is the manager's to decide (§10.1); whatever vanilla's placement hooks derived
	// from their hit does not apply to a Rail that was never aimed.
	mBlueprintDesigner = mBridgeSpace.Get();
}

void AACPRRailHologram::OnHologramTransformUpdated()
{
	Super::OnHologramTransformUpdated();

	// A bridge is placed once, by ApplyBridgeTransform. Vanilla's "the transform changed after the
	// initial move" event (FGHologram.h:530) is where any other mover would show — and a second mover
	// is a bug to see, not a symptom to correct, so this reports and does not put the bridge back.
	if( !mIsBridge || mAnchorOutward.IsNearlyZero() || mBridgeMoveLogBudget <= 0 )
	{
		return;
	}

	const FQuat wanted = FRotationMatrix::MakeFromXZ( mAnchorOutward, mAnchorUp ).ToQuat();
	const double moved = FVector::Dist( GetActorLocation(), mAnchorPoint );
	const double turned = FMath::RadiansToDegrees( GetActorQuat().AngularDistance( wanted ) );
	if( moved > 0.1 || turned > 0.1 )
	{
		--mBridgeMoveLogBudget;
		ACPR_LOG( Warning, BRIDGE,
			TEXT( "%s %s moved off its spec frame by something other than the manager | %.1f uu, %.1f deg — NOT put back; find the mover" ),
			*GetName(), *mBridgeLabel, moved, turned );
	}
}

bool AACPRRailHologram::ValidateBridgeNow( FString& out_disqualifiers )
{
	mInManagerValidation = true;

	// AFGHologram::mConstructDisqualifiers (FGHologram.h:772) is protected; clearing it directly is what
	// ResetConstructDisqualifiers does, without importing it.
	mConstructDisqualifiers.Reset();
	CheckValidPlacement();

	// The list is described only when it changed. Every shown bridge is validated on every
	// evaluation, and a string per bridge per frame is work for nothing while the verdict stands.
	bool sameList = mConstructDisqualifiers.Num() == mCachedBridgeDisqualifiers.Num();
	for( int32 i = 0; sameList && i < mConstructDisqualifiers.Num(); ++i )
	{
		sameList = mConstructDisqualifiers[ i ].Get() == mCachedBridgeDisqualifiers[ i ].Get();
	}

	// Vanilla's verdict, kept for the frames between evaluations — see CheckValidPlacement.
	mCachedBridgeDisqualifiers = mConstructDisqualifiers;

	mInManagerValidation = false;

	if( !sameList || mCachedDisqualifierText.IsEmpty() )
	{
		mCachedDisqualifierText = DescribeDisqualifiers();
	}
	out_disqualifiers = mCachedDisqualifierText;

	// CanConstruct, not "the list is empty": a soft disqualifier (UFGConstructDisqualifier::
	// mIsSoftDisqualifier, "the player is still allowed to construct") — the Rail through a wall — is on
	// the list and does not block. FGHologram.h:320, UFUNCTION; Vertical Conveyor Auto-Connect relies on it.
	const bool valid = CanConstruct();

	if( !ACPR_LOG_ACTIVE( Verbose ) || mBridgeLogBudget <= 0 )
	{
		return valid;
	}

	const FString key = FString::Printf( TEXT( "%d|%s|%d" ), valid ? 1 : 0, *out_disqualifiers, mBridgeFaulted ? 1 : 0 );
	if( key != mLastBridgeValidation )
	{
		mLastBridgeValidation = key;
		--mBridgeLogBudget;

		ACPR_LOG( Verbose, BRIDGE,
			TEXT( "%s %s validation -> %s | disqualifiers=[%s] faulted=%d | length=%.1f latched=%d space=%s" ),
			*GetName(), *mBridgeLabel,
			valid ? TEXT( "VALID" ) : TEXT( "INVALID" ),
			*out_disqualifiers,
			mBridgeFaulted ? 1 : 0,
			mRequestedFarLength,
			mBridgeLatched ? 1 : 0,
			mBridgeSpace.IsValid() ? *mBridgeSpace->GetName() : TEXT( "world" ) );
	}

	return valid;
}

FString AACPRRailHologram::DescribeDisqualifiers() const
{
	FString names;
	for( const TSubclassOf< UFGConstructDisqualifier >& disqualifier : mConstructDisqualifiers )
	{
		if( !names.IsEmpty() )
		{
			names += TEXT( "," );
		}
		names += GetNameSafe( disqualifier.Get() );
	}
	return names.IsEmpty() ? FString( TEXT( "<none>" ) ) : names;
}

int32 AACPRRailHologram::GetBridgeCostMultiplier() const
{
	const AACPRRail* cdo = GetRailCDO();
	const double perCost = FMath::Max( 1.0, cdo ? static_cast< double >( cdo->GetLengthPerCost() ) : 1000.0 );

	// §13: "The Rail's single multiplier rounds up, and generated blueprint Rails use exact constructed
	// length in the same formula." The same ceil the beam applies, measured at four lengths.
	return FMath::Max( 1, FMath::CeilToInt32( static_cast< double >( mRequestedFarLength ) / perCost ) );
}

int32 AACPRRailHologram::GetBaseCostMultiplier() const
{
	if( mIsBridge )
	{
		return GetBridgeCostMultiplier();
	}

	// A length of ours (far-end snap, lane Rail) is the length ConfigureActor builds, so §13's formula is
	// applied to it directly rather than to vanilla's quantised mCurrentLength. The two agree whenever
	// vanilla's length is the ceiling of ours (ceil(ceil(x)/n) = ceil(x/n)), so the two differ only at
	// exact 10 m multiples, where a far-snapped 1000.0001 uu would reach vanilla as 1100 and cost two.
	// The length is put on the registry's own 1 uu lattice first (B.3,
	// UACPRTerminalComponent::QuantizeLocation's quantum) — the two terminals this length spans compare
	// on that lattice, so the cost is computed from the same number — rather than shifted by a constant.
	const double own = GetOwnLength();
	if( own > 0.0 )
	{
		const AACPRRail* cdo = GetRailCDO();
		const double perCost = FMath::Max( 1.0, cdo ? static_cast< double >( cdo->GetLengthPerCost() ) : 1000.0 );
		const double onLattice = FMath::RoundToDouble( own );
		return FMath::Max( 1, FMath::CeilToInt32( onLattice / perCost ) );
	}

	return Super::GetBaseCostMultiplier();
}

FString AACPRRailHologram::DescribeOwnCost() const
{
	FString text;
	for( const FItemAmount& amount : GetCost( false ) )
	{
		text += FString::Printf( TEXT( "%dx%s " ), amount.Amount, *GetNameSafe( amount.ItemClass.Get() ) );
	}
	return text.IsEmpty() ? FString( TEXT( "<free>" ) ) : text.TrimEnd();
}

void AACPRRailHologram::SetPlacementMaterialState( EHologramMaterialState materialState )
{
	if( mIsBridge && materialState != EHologramMaterialState::HMS_ERROR && !CanConstruct() )
	{
		materialState = EHologramMaterialState::HMS_ERROR;
	}

	Super::SetPlacementMaterialState( materialState );
}

void AACPRRailHologram::CheckValidFloor()
{
	if( mIsBridge )
	{
		return;
	}

	Super::CheckValidFloor();
}

void AACPRRailHologram::ApplyPlacementOffsets( const FHitResult& hitResult )
{
	// §11's lane candidate is re-decided on every step-1 frame, so every early return below leaves it
	// false — a locked, anchored, off-surface or Rail-aimed step 1 must not start a lane Rail on the click.
	if( !mAnchorLatched )
	{
		mLaneCandidate = false;
	}

	if( mAnchorLatched || !hitResult.bBlockingHit )
	{
		return;
	}

	// A snapped anchor is exact; the lattice rule must not touch it. The surface offset and lane
	// phase exist to put a free-hand Rail on the grid — a Rail that begins at a terminal is already
	// on it, by construction, and moving it half a metre would cost the Coupling the snap exists to
	// create. Same stand-down as the Junction's.
	if( IsAnchorSnapped() )
	{
		return;
	}

	// A locked hologram is the player's, not ours.
	//
	// Placement keeps running every frame while the hologram is locked, and both offsets below are
	// ABSOLUTE — they set the distance from the surface to mSurfaceOffset and snap Z to the lane grid.
	// Applied while locked, a nudge along the wall normal would be undone before it could be seen, and
	// a vertical nudge quantised straight back to the nearest 1 m lane.
	//
	// Idempotence is what makes the offsets safe frame to frame, and it is also what makes them
	// override deliberate input. Locking is the signal that the player has taken over.
	if( IsHologramLocked() )
	{
		return;
	}

	const FVector normal = hitResult.ImpactNormal.GetSafeNormal();
	if( normal.IsNearlyZero() )
	{
		return;
	}

	// Never offset off one of our own hosts. Aiming at an existing Rail's face and building would give
	// a second Rail half a metre further out again; repeat that and the lanes march away from the wall.
	//
	// The offset exists to lift a Rail off a SURFACE it is mounted on. One of our terminal hosts —
	// a Rail, a Junction, an Outlet — is not a mounting surface, it is a coupling target, and the
	// terminal snap is what answers that aim (the rule, not a class list).
	if( Cast< IACPRTerminalHost >( hitResult.GetActor() ) )
	{
		return;
	}

	const FVector current = GetActorLocation();

	// §11 lane lattice — the Default build mode on an axis-aligned surface.
	//
	// The preview's start goes to the CENTRE of the aimed 1 m cell, mSurfaceOffset off the surface: the
	// point a Junction placed here would stand on (ACPRJunctionHologram's rule 2 uses the same world-axis
	// half-cell phase). Which edge of the cell the Rail actually starts on is decided after the click, by
	// the drag — see DriveLane.
	//
	// The cell is taken from the impact point, not from vanilla's snapped location. Vanilla snaps to the
	// whole-metre grid LINES, which are the cell boundaries; snapping a boundary to a half-cell phase is a
	// coin toss between two cells (GridSnap rounds up), whereas the impact point lies inside exactly one.
	//
	// Only where the world grid is the surface's grid: an axis-aligned normal (the Junction's own
	// sum-of-components test) on an actor whose own axes are world-aligned. A foundation yawed 45° has a
	// world-aligned normal and a grid that is not, and forcing world cells onto it would put every Rail
	// across its squares; it takes the path below, as do landscapes and anything tilted.
	{
		const double axisSum = FMath::Abs( normal.X ) + FMath::Abs( normal.Y ) + FMath::Abs( normal.Z );
		const bool normalAligned = axisSum < 1.001;

		bool actorAligned = true;
		if( const AActor* hitActor = hitResult.GetActor() )
		{
			const FVector fwd = hitActor->GetActorForwardVector();
			const double largest = FMath::Max3( FMath::Abs( fwd.X ), FMath::Abs( fwd.Y ), FMath::Abs( fwd.Z ) );
			actorAligned = largest > 0.999;
		}

		if( IsLaneBuildMode() && normalAligned && actorAligned )
		{
			// The normal as an exact world axis direction — the sum test has already said it is one.
			FVector axisNormal = FVector::ZeroVector;
			if( FMath::Abs( normal.X ) > 0.5 ) { axisNormal.X = FMath::Sign( normal.X ); }
			else if( FMath::Abs( normal.Y ) > 0.5 ) { axisNormal.Y = FMath::Sign( normal.Y ); }
			else { axisNormal.Z = FMath::Sign( normal.Z ); }

			// Both double, for FMath::GridSnap's single template type (the Junction's note).
			const double pitch = 100.0;
			const double phase = static_cast< double >( mLaneOffset );
			const FVector impact = hitResult.ImpactPoint;

			FVector centre = impact + axisNormal * static_cast< double >( mSurfaceOffset );
			if( axisNormal.X == 0.0 ) { centre.X = FMath::GridSnap( impact.X - phase, pitch ) + phase; }
			if( axisNormal.Y == 0.0 ) { centre.Y = FMath::GridSnap( impact.Y - phase, pitch ) + phase; }
			if( axisNormal.Z == 0.0 ) { centre.Z = FMath::GridSnap( impact.Z - phase, pitch ) + phase; }

			mLaneCandidate = true;
			mLaneCandidateCentre = centre;
			mLaneCandidateNormal = axisNormal;
			mLastLaneAxis = FVector::ZeroVector;

			// The key is a string per frame; built only while it can be printed.
			if( mLaneCellLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
			{
				const FString cellKey = FString::Printf( TEXT( "%s|%s" ),
					*UACPRTerminalComponent::QuantizeLocation( centre ).ToString(), *axisNormal.ToString() );
				if( mLaneCellLog.Admit( cellKey ) )
				{
					ACPR_LOG( Verbose, RAIL_LANE,
						TEXT( "cell | centre=%s normal=%s | impact=%s vanillaLoc=%s | actor=%s "
						      "| phase=%.1f surface=%.1f" ),
						*centre.ToString(), *axisNormal.ToString(), *impact.ToString(), *current.ToString(),
						hitResult.GetActor() ? *hitResult.GetActor()->GetName() : TEXT( "<none>" ),
						phase, mSurfaceOffset );
				}
			}

			if( !centre.Equals( current, 0.5 ) )
			{
				SetActorLocation( centre );
				LogPlacement( hitResult, TEXT( "lane-cell" ) );
			}
			return;
		}
	}

	// Distance from the surface, set rather than added.
	//
	// A fixed delta added to wherever vanilla had snapped would have to absorb whatever plane vanilla
	// chose — 25 uu inside the face on a wall, the face itself on a foundation — and one number cannot
	// be right for both. Measuring from the hit point makes the rule the one §11 states: the Rail's
	// centre sits mSurfaceOffset from the surface it is mounted on. It is also idempotent by
	// construction — once the distance IS mSurfaceOffset, the correction is zero — so no
	// remembered-location guard is needed.
	const float along = FVector::DotProduct( current - hitResult.ImpactPoint, normal );
	FVector delta = normal * ( mSurfaceOffset - along );

	// §11's half-cell lane phase, on an axis derived from the SURFACE and nothing else.
	//
	// The axis cannot come from the Rail. In step 1 the hologram's forward IS the surface normal, so
	// cross( normal, forward ) is the zero vector on every surface-aligned hit (only a landscape hit
	// has them differ), and the Rail has no direction yet: the direction is chosen by dragging in
	// step 2, long after the anchor this rule has to place.
	//
	// On a WALL — a surface whose normal is horizontal — the lanes run UP the wall, and that axis
	// is world Z whatever the Rail will eventually do. Nothing else is needed, and it is available
	// in step 1.
	//
	// Foundations get no lane phase on this path, which is only reached in the Diagonal and FreeForm
	// build modes, or on a surface the lane lattice above declined. The Default mode's floors and
	// walls are handled by that block: both axes across the surface, on the Junction's phase.
	const bool wallLike = FMath::Abs( normal.Z ) < 0.5f;
	const FVector laneAxis = wallLike ? FVector::UpVector : FVector::ZeroVector;

	if( !laneAxis.IsNearlyZero() && !FMath::IsNearlyZero( mLaneOffset ) )
	{
		const float lanePitch = 100.0f;
		const float along_lane = FVector::DotProduct( current, laneAxis );
		const float wanted = FMath::GridSnap( along_lane - mLaneOffset, lanePitch ) + mLaneOffset;
		delta += laneAxis * ( wanted - along_lane );
	}

	mLastLaneAxis = laneAxis;

	if( delta.IsNearlyZero( 0.5f ) )
	{
		return;
	}

	SetActorLocation( current + delta );

	LogPlacement( hitResult, TEXT( "offset" ) );
}

void AACPRRailHologram::LogPlacement( const FHitResult& hitResult, const TCHAR* step )
{
	if( !mPlacementLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	// Keyed on a coarse position so a held aim logs once and a moving aim logs a readable trail.
	const FString key = FString::Printf( TEXT( "%s|%s|%s" ),
		step,
		hitResult.GetActor() ? *hitResult.GetActor()->GetName() : TEXT( "-" ),
		*GetActorLocation().GridSnap( 25.0f ).ToString() );

	if( !mPlacementLog.Admit( key ) )
	{
		return;
	}

	ACPR_LOG( Verbose, RAIL_HOLO,
		TEXT( "%s | actor=%s normal=%s | fwd=%s laneAxis=%s | "
		      "surface=%.1f lane=%.1f latched=%d | loc=%s" ),
		step,
		hitResult.GetActor() ? *hitResult.GetActor()->GetName() : TEXT( "<none>" ),
		*hitResult.ImpactNormal.ToString(),
		*GetActorForwardVector().ToString(),
		*mLastLaneAxis.ToString(),
		mSurfaceOffset, mLaneOffset, mAnchorLatched ? 1 : 0,
		*GetActorLocation().ToString() );
}

void AACPRRailHologram::SetHologramNudgeLocation()
{
	if( mIsBridge )
	{
		return;   // see the header
	}
	Super::SetHologramNudgeLocation();
}

void AACPRRailHologram::SetHologramLocationAndRotation( const FHitResult& hitResult )
{
	// This does NOT offset: it never runs during step 1, so an offset here would only ever reach the
	// already-latched anchor, and the Rail would walk away with every frame. It is the right place to
	// re-assert the preview profile.
	//
	// It is also where the drag is constrained, and the fact that it only runs after the anchor is
	// latched is what makes it the right place: in step 2 the aim means length, and §5.1 gives a
	// snapped Rail only length and roll. The hit is projected onto the anchor's outward ray BEFORE
	// vanilla sees it, so AFGBeamHologram computes its own length along an axis we chose — its
	// private mCurrentLength included — rather than being corrected afterwards.
	FHitResult adjusted = hitResult;

	// §5.1's second end, and it runs before the axis constraint rather than after it.
	//
	// A far-end snap produces an aim point that is exactly a terminal's location — which, when the
	// near end is snapped, TryFarEndSnap has already proved lies on the anchor's ray to within
	// mAxisTolerance. Projecting it again could only move it off the cell it was chosen for, by
	// whatever float noise the projection carries, and the whole point of the snap is that the two
	// cells compare EQUAL as integers. So the constraint stands down when the snap fires.
	//
	// Step 2 only: the far end is length, and there is no length until the near end is placed.
	if( mIsBridge )
	{
		// Never aimed: the spec placed it, the nudge is refused, nothing here applies.
		return;
	}

	// §11's lane Rail owns the whole step-2 frame when it applies; otherwise the ordinary path below
	// runs.
	if( mAnchorLatched && DriveLane( hitResult ) )
	{
		ApplyLengthToPreview();
		return;
	}

	bool farSnapped = false;
	if( mAnchorLatched )
	{
		farSnapped = TryFarEndSnap( adjusted );
	}
	else
	{
		ClearFarSnap();
	}

	if( IsAnchorSnapped() )
	{
		if( !farSnapped )
		{
			ConstrainAimToAxis( adjusted );
		}

		const FTransform beforeSuper = GetActorTransform();
		Super::SetHologramLocationAndRotation( adjusted );
		const FTransform afterSuper = GetActorTransform();

		// Pinned, because constraining the input is not enough for the direction. The projection is
		// right — anchor + axis·along matches the constrained point to three decimals — and the Rail
		// still swings off axis: vanilla is not deriving the beam's direction from the hit POINT but
		// from somewhere else in the hit, or from the aim ray, or from its own state. Constraining the
		// input holds for length and not for direction.
		//
		// Direction is invariant 9's business and it is not negotiable, so it is written last and
		// absolutely: the actor sits at the anchor, facing the terminal's outward axis, whatever
		// vanilla just decided. Rotation only — the location is left alone because nothing in the log
		// says vanilla moves it, and pinning a position on a guess is how a Rail walks off.
		//
		// beforeSuper and afterSuper say exactly what vanilla changed, so if this pin is ever not
		// enough, the log names it.
		const FRotator pinned = FRotationMatrix::MakeFromXZ( mAnchorOutward, mAnchorUp ).Rotator();
		CaptureRollBase( pinned.Quaternion() );
		ApplyRoll();

		LogConstrainedAim( hitResult, adjusted, beforeSuper, afterSuper, pinned );
	}
	else
	{
		Super::SetHologramLocationAndRotation( adjusted );

		// A free near end: vanilla owns the direction and there is nothing to pin. What it writes here
		// is roll-free — it recomputes the rotation from the hit on every step-2 frame — so it is
		// exactly the base the roll composes onto, and capturing it per frame means the base follows
		// the drag without the roll ever folding into it.
		CaptureRollBase( GetActorQuat() );
		ApplyRoll();
	}

	ApplyLengthToPreview();
}

bool AACPRRailHologram::DoMultiStepPlacement( bool isInputFromARelease )
{
	// A bridge has no steps of its own: the blueprint's clicks are the only ones (§10.2).
	if( mIsBridge )
	{
		return true;
	}

	const bool wasLatched = mAnchorLatched;
	const bool wasSnapped = IsAnchorSnapped();
	const bool result = Super::DoMultiStepPlacement( isInputFromARelease );

	// false means "another step follows" — the first click, which latches the anchor. From here on
	// the position is carried rather than recomputed, and the offset is already baked into it.
	//
	// true means the placement is done, and the latch and anchor are reset. A stale anchor actively
	// drives geometry, so a reused hologram instance would otherwise start its second Rail constrained
	// to the first one's axis. Instances are not observed to be reused; this is insurance.
	if( !result )
	{
		mAnchorLatched = true;

		// §11: the click that latches the start also decides whether this is a lane Rail. Only from a
		// step-1 frame that snapped to a lane cell — which already excludes a locked hologram, a snapped
		// anchor, Diagonal/FreeForm, a tilted or yawed surface and a Rail aimed at — and re-checked here
		// because the build mode or the lock can change between that frame and the click.
		//
		// On the latching click only: a false return while ALREADY latched is not a new start, and
		// re-deciding there would read a step-1 candidate that describes nothing current.
		if( !wasLatched )
		{
			const bool lane = mLaneCandidate && !IsAnchorSnapped() && IsLaneBuildMode() && !IsHologramLocked();

			ClearLane();
			if( lane )
			{
				mLaneMode = true;
				mLaneCentre = mLaneCandidateCentre;
				mLaneNormal = mLaneCandidateNormal;
			}

			LogLaneEvent( lane
				? FString::Printf( TEXT( "ON | cell centre=%s normal=%s | actorAtClick=%s" ),
					*mLaneCentre.ToString(), *mLaneNormal.ToString(), *GetActorLocation().ToString() )
				: FString::Printf( TEXT( "off for this Rail | candidate=%d anchored=%d laneBuildMode=%d locked=%d" ),
					mLaneCandidate ? 1 : 0, IsAnchorSnapped() ? 1 : 0, IsLaneBuildMode() ? 1 : 0,
					IsHologramLocked() ? 1 : 0 ) );
		}
	}
	else
	{
		mAnchorLatched = false;
		ClearAnchor();

		// The far-end state is deliberately not cleared here, and this is the one ordering in the file
		// that has to be got right. FGHologram.h:219 — "when it returns true, Construct can be called
		// to construct the actor" — and FGBuildableHologram puts ConfigureActor inside that Construct.
		// So returning true leads to ConfigureActor LATER IN THIS CALL STACK, and ConfigureActor is
		// where the exact-length correction lives. Clearing the far state here would build every Rail
		// at vanilla's quantised length and make the correction Warning unreachable. It is cleared at
		// the start of the NEXT placement instead, in PreHologramPlacement. The §11 lane length is kept
		// for the identical reason: GetOwnLength reads it in that same ConfigureActor.
		//
		// The roll does go with them. A reused hologram instance that kept 45° would start its next
		// Rail pre-rolled, with nothing on screen or in the log to say where the angle came from.
		// The angle and the frame stamp, but NOT the base itself. Resetting it to identity would leave
		// ApplyRoll a world-identity rotation to write on any later frame where the capture guards
		// decline — the same synthetic-base failure as the BeginPlay line above, reached from the
		// other end. Clearing the stamp is enough: it makes the next Post re-capture from vanilla.
		mRollDegrees = 0.0;
		mRollBaseFrame = 0;
	}

	// The click that builds, with the validity vanilla saw: what the list held, whether CanConstruct
	// agreed, and the lengths the span rule and ConfigureActor work from. A Rail built through a
	// Junction despite a BLOCKED verdict means either the last validation before the click was not that
	// one, or a disqualifier on the list did not stop the build; this line says which.
	if( result )
	{
		ACPR_LOG( Display, RAIL_HOLO,
			TEXT( "step complete | canConstruct=%d disqualifiers=[%s] | snapped=%d aimAlong=%.1f farSnap=%d requested=%.1f lane=%.1f | loc=%s" ),
			CanConstruct() ? 1 : 0, *DescribeDisqualifiers(),
			wasSnapped ? 1 : 0, mAimAlong, mFarSnapped ? 1 : 0, mRequestedFarLength, mLaneLength,
			*GetActorLocation().ToString() );
	}
	else
	{
		ACPR_LOG( Verbose, RAIL_HOLO,
			TEXT( "DoMultiStepPlacement( release=%d ) -> %d | latched=%d lane=%d | loc=%s" ),
			isInputFromARelease ? 1 : 0, result ? 1 : 0, mAnchorLatched ? 1 : 0, mLaneMode ? 1 : 0,
			*GetActorLocation().ToString() );
	}

	ApplyLengthToPreview();
	return result;
}

void AACPRRailHologram::PreHologramPlacement( const FHitResult& hitResult, bool callForChildren )
{
	// A bridge receives its parent's hit through the callForChildren cascade (FGHologram.h:188). That hit
	// is where the player is aiming the BLUEPRINT and means nothing to this Rail, so vanilla's own Pre
	// (AFGBuildableHologram, FGBuildableHologram.h:155) is handed an empty one instead.
	if( mIsBridge )
	{
		Super::PreHologramPlacement( FHitResult(), callForChildren );
		return;
	}

	Super::PreHologramPlacement( hitResult, callForChildren );

	// Step 1 is the start of a new Rail, so the previous one's far-end state goes here rather than at
	// the end of the last placement — see the note in DoMultiStepPlacement for why the difference
	// matters. TryFarEndSnap re-derives everything from scratch on every step-2 frame, so this is the
	// only reset the far end needs.
	if( !mAnchorLatched )
	{
		ClearFarSnap();

		// And the §11 lane state, for the same reason and at the same moment: the last Rail's lane length
		// had to survive until its ConfigureActor, and this is the first hook of the next placement.
		ClearLane();
	}

	ApplyLengthToPreview();
}

void AACPRRailHologram::PostHologramPlacement( const FHitResult& hitResult, bool callForChildren )
{
	// A bridge: the parent's hit is not ours (see PreHologramPlacement), so vanilla's Post gets an empty
	// one, and nothing else runs — no terminal highlight (the player is aiming a blueprint), no placement
	// (if Super moved it, OnHologramTransformUpdated says so).
	if( mIsBridge )
	{
		Super::PostHologramPlacement( FHitResult(), callForChildren );
		return;
	}

	// The only hook that runs in step 1 after vanilla has positioned and snapped, which makes it
	// the one place the offset can be applied and be visible before the first click.
	Super::PostHologramPlacement( hitResult, callForChildren );

	// Vanilla's attachment snap (a Junction target) is the anchor; TrySnapToActor adopted it the
	// moment it happened, and this keeps the two in step if vanilla's state changed between.
	if( !mAnchorLatched && mSnappedAttachmentPoint )
	{
		AdoptVanillaSnapAsAnchor();
	}

	// Re-asserted after Super, in both steps: returning true from TrySnapToActor suppresses
	// SetHologramLocationAndRotation and nothing else, so a placement made there can still be
	// overwritten by whatever runs later in the frame — and the direction pin lives in
	// SetHologramLocationAndRotation, which is step 2, and THIS hook runs after it every frame. The
	// anchor placement is absolute, so re-applying it costs one transform write.
	//
	// Location only in step 1. In step 2 the position is vanilla's to carry and pinning it on a guess
	// is how a Rail walks off; the direction is invariant 9's and is not negotiable either way.
	if( IsAnchorSnapped() )
	{
		// Location only in step 1 — see above. The rotation is written by ApplyRoll in both steps,
		// absolutely, so the pin and the roll are one write rather than two that could disagree about
		// which of them ran last.
		if( !mAnchorLatched )
		{
			SetActorLocation( mAnchorPoint );
		}

		CaptureRollBase( FRotationMatrix::MakeFromXZ( mAnchorOutward, mAnchorUp ).ToQuat() );
		ApplyRoll();
	}
	else if( IsLaneDriving() )
	{
		// §11 lane Rail, step 2: the same re-assert, for the same reason — this hook runs after
		// SetHologramLocationAndRotation every frame, and a lane Rail's start and axis are ours outright.
		// The location is re-asserted too (unlike the anchored branch's step 2), because DriveLane is what
		// moved it off vanilla's start in the first place. Not while locked: the nudge owns it then.
		if( IsHologramLocked() )
		{
			FreezeLaneForLock();
		}
		else
		{
			SetActorLocation( mLaneStart );
		}

		CaptureRollBase( LaneRotationFor( mLaneAxis ) );
		ApplyRoll();
	}
	else
	{
		// A free near end, in either step, and the frame stamp is doing real work here.
		//
		// Step 1 is included: the build gun forwards scroll to whichever hologram is active regardless
		// of step, and SetHologramLocationAndRotation does not fire in step 1 at all, so this is the
		// only place the roll is re-asserted after vanilla's later writes and mRollFreeRotation is
		// refreshed there. Without it a scroll in step 1 would compose onto the BeginPlay spawn quat
		// and snap the preview to its spawn orientation. With mRollDegrees at 0 the write is the
		// captured base unchanged, so the branch is safe in both steps.
		//
		// On an ordinary frame SetHologramLocationAndRotation already captured vanilla's rotation and
		// rolled it, so re-applying is idempotent and re-capturing would fold the roll into its own
		// base. On a frame where TrySnapToActor returned true that function never ran, and the
		// rotation on the actor is vanilla's snap — roll-free, and the base we want.
		//
		// Two independent tests, because either alone can be fooled: the stamp says our writer did not
		// run this frame, and the comparison against the drift watcher's record says somebody else
		// actually wrote. Capturing our own output as a base is the one mistake this whole mechanism
		// has to avoid, and it is silent when it happens — the roll simply doubles every frame.
		if( mRollBaseFrame != GFrameCounter &&
			!GetActorQuat().Equals( mLastAppliedRotation, 1e-4 ) )
		{
			CaptureRollBase( GetActorQuat() );
		}

		ApplyRoll();
	}

	ApplyPlacementOffsets( hitResult );
	ApplyLengthToPreview();

	FACPRSpace::WatchPlacementSpace( this, GetPlacementDesigner(), mLastPlacementSpace, mPlacementSpaceSeen );

	UpdateTerminalHighlight( hitResult );
}

void AACPRRailHologram::UpdateTerminalHighlight( const FHitResult& hitResult )
{
	// The host is the aimed actor, not the pick's owner. §12's cue has to appear while the player is
	// still sweeping towards a face, and — for a Rail continued from a CAPPED end — the whole point is
	// what does NOT light up. A capped terminal fails the same IsOpen() test the picker uses, so the
	// aimed Rail shows a marker on its open end and nothing on the capped one. That is what tells a
	// free-placed Rail from a snapped one.
	//
	// Show() hides everything for an actor that is not a terminal host, so a frame on a foundation
	// (or one with no hit at all, which passes null) clears the highlight with no test here.
	const UACPRTerminalComponent* selected = mAnchorLatched
		? mFarTerminal.Get()
		: mAnchorTerminal.Get();

	FACPRSpaceFilter spaceFilter;
	spaceFilter.Enforce = true;
	spaceFilter.Space = GetPlacementDesigner();

	// The anchor stays lit through step 2: a marker that goes away while the length is being set looks
	// like a lost snap. In step 2 the crosshair is on the far end, so Show() alone lights that host and
	// the anchor's goes dark. So the latched anchor is handed to Show() as `pinned` (in case the far
	// aim is on the anchor's own host — a Rail between two faces of one Junction) and, otherwise,
	// pinned on its own host separately. Show() first, Pin() second: Show() may clear the anchor's
	// host on the frame the aim leaves it, and Pin() then relights it.
	const UACPRTerminalComponent* pinned = mAnchorLatched ? mAnchorTerminal.Get() : nullptr;

	// The terminal the far end will couple to by geometry, lit Selected whether or not it is under
	// the crosshair. A far end that lands on an open, opposing terminal's cell couples
	// at construction (§6.1) even when nothing snapped it there — the aim just happened to give that
	// length — and a Selected lamp is the promise that it will. One cell lookup per placement frame.
	// Handed to Show() as a second pin when it lives on the aimed host, pinned on its own host otherwise
	// (a far end on the anchor's own host — a Rail between two faces of one Junction — is the one case
	// the anchor's Pin does not carry it; it shows open until the aim rests on that Junction).
	const UACPRTerminalComponent* byGeometry = mAnchorLatched ? FindFarEndCoincidence() : nullptr;

	FACPRTerminalHighlight::Show( mHighlightHost, hitResult.GetActor(), selected, spaceFilter, pinned, byGeometry );
	FACPRTerminalHighlight::Pin( mAnchorHighlightHost, pinned, mHighlightHost.Get() );
	FACPRTerminalHighlight::Pin( mFarHighlightHost, byGeometry,
		byGeometry && ( byGeometry->GetOwner() == mHighlightHost.Get() || byGeometry->GetOwner() == mAnchorHighlightHost.Get() )
			? byGeometry->GetOwner() : nullptr );
}

const UACPRTerminalComponent* AACPRRailHologram::FindFarEndCoincidence() const
{
	const AACPRRail* cdo = GetRailCDO();
	const double own = GetOwnLength();
	const double length = own > 0.0 ? own : static_cast< double >( mCurrentLength );
	if( !cdo || length <= 0.0 )
	{
		return nullptr;
	}

	const UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this );
	if( !registry )
	{
		return nullptr;
	}

	// Terminal B of the Rail this hologram builds: at `length` along the actor's X, outward +X.
	const FVector forward = GetActorForwardVector();
	const FIntVector farCell = UACPRTerminalComponent::QuantizeLocation( GetActorLocation() + forward * length );
	const FIntVector railB = UACPRTerminalComponent::ReduceDirection(
		farCell - UACPRTerminalComponent::QuantizeLocation( GetActorLocation() ) );

	TArray< UACPRTerminalComponent* > inCell;
	registry->FindInCell( farCell, nullptr, nullptr, inCell );

	const AFGBuildableBlueprintDesigner* space = GetPlacementDesigner();
	for( const UACPRTerminalComponent* terminal : inCell )
	{
		if( terminal && terminal->IsOpen() && !terminal->IsPrivateInterface() &&
			terminal->GetQuantizedOutward() == FIntVector( -railB.X, -railB.Y, -railB.Z ) &&
			FACPRSpace::OfActor( terminal->GetOwner() ) == space )
		{
			return terminal;
		}
	}
	return nullptr;
}

float AACPRRailHologram::GetNudgeDistance() const
{
	// The mode is the modifier flag, read directly — the same `mSnapToGuideLines` ScrollRotate uses,
	// not a threshold on vanilla's returned value. Zero or less on either of ours means "leave vanilla
	// alone" for that mode.
	const bool fine = mSnapToGuideLines;
	const float configured = fine ? mNudgeDistanceFine : mNudgeDistanceCoarse;
	const float ours = ( configured > 0.0f ) ? configured : Super::GetNudgeDistance();

	// Logged per DISTINCT value rather than once, so both modes appear.
	if( !FMath::IsNearlyEqual( ours, mLastLoggedNudgeDistance ) && ACPR_LOG_ACTIVE( Verbose ) )
	{
		mLastLoggedNudgeDistance = ours;

		ACPR_LOG( Verbose, RAIL_HOLO,
			TEXT( "GetNudgeDistance -> %.1f | mode=%s (coarse=%.1f fine=%.1f)" ),
			ours, fine ? TEXT( "fine" ) : TEXT( "coarse" ),
			mNudgeDistanceCoarse, mNudgeDistanceFine );
	}

	return ours;
}

void AACPRRailHologram::AdoptVanillaSnapAsAnchor()
{
	UACPRTerminalComponent* terminal = mSnappedAttachmentPoint ? FACPRAttachment::TerminalFor( *mSnappedAttachmentPoint ) : nullptr;
	if( !terminal || mAnchorTerminal.Get() == terminal )
	{
		return;
	}

	// The same four fields TrySnapAnchorToTerminal writes, from the terminal vanilla's snap landed on. The
	// actor's placement is vanilla's: measured equal to §6.1's mating (0.0 uu, 0.0°) on every Junction
	// face over 842 frames, so it is adopted, not corrected.
	const FTransform frame = terminal->GetComponentTransform();
	mAnchorTerminal = terminal;
	mAnchorPoint = terminal->GetComponentLocation();
	mAnchorOutward = frame.GetUnitAxis( EAxis::X );
	mAnchorUp = frame.GetUnitAxis( EAxis::Z );
	CaptureRollBase( GetActorQuat() );

	// Invariant 9's tripwire, not a fix: if vanilla ever places the near end off the terminal's cell or
	// off its outward axis, the Coupling will not form and this says why. Once per adopted terminal.
	const double offBy = FVector::Dist( GetActorLocation(), mAnchorPoint );
	const double offDeg = FMath::RadiansToDegrees( FMath::Acos( FMath::Clamp(
		FVector::DotProduct( GetActorForwardVector(), mAnchorOutward ), -1.0, 1.0 ) ) );
	if( offBy > UACPRTerminalComponent::LatticeQuantum || offDeg > 1.0 )
	{
		ACPR_LOG( Warning, RAIL_HOLO,
			TEXT( "vanilla's attachment snap disagrees with the terminal frame | %s -> %s on %s | actor at %s facing %s | terminal at %s outward %s | off by %.1f uu, %.1f deg" ),
			*GetName(), *terminal->GetName(), *GetNameSafe( terminal->GetOwner() ),
			*GetActorLocation().ToString(), *GetActorForwardVector().ToString(),
			*mAnchorPoint.ToString(), *mAnchorOutward.ToString(), offBy, offDeg );
	}

	if( mSnapLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) && mSnapLog.Admit( terminal->GetPathName() ) )
	{
		ACPR_LOG( Verbose, RAIL_HOLO,
			TEXT( "anchor-snap ADOPTED (vanilla's attachment pipeline) | host=%s terminal=%s | anchor=%s target=%s | axis=%s rot=%s" ),
			*GetNameSafe( terminal->GetOwner() ), *terminal->GetName(),
			*UACPRTerminalComponent::QuantizeLocation( GetActorLocation() ).ToString(),
			*terminal->GetQuantizedLocation().ToString(),
			*mAnchorOutward.ToString(), *GetActorRotation().ToString() );
	}
}

void AACPRRailHologram::FilterAttachmentPoints( TArray< const FFGAttachmentPoint* >& Points, AFGBuildable* pBuildable, const FHitResult& HitResult ) const
{
	Super::FilterAttachmentPoints( Points, pBuildable, HitResult );

	// §10.1: a host in another build space is not a target; vanilla refuses the pair with its own
	// Designer message later, but it must not be offered or lit in the first place.
	const AFGBuildableBlueprintDesigner* space = GetPlacementDesigner();
	Points.RemoveAll( [ space ]( const FFGAttachmentPoint* point )
	{
		return point && point->Owner && FACPRSpace::OfActor( point->Owner.Get() ) != space;
	} );
}

void AACPRRailHologram::ConfigureActor( AFGBuildable* inBuildable ) const
{
	Super::ConfigureActor( inBuildable );

	// Non-zero length here means vanilla's AFGBeamHologram::ConfigureActor pushed its private
	// mCurrentLength through the public SetLength — the whole length chain, confirmed per build.
	if( AACPRRail* rail = Cast< AACPRRail >( inBuildable ) )
	{
		// AFGBeamHologram quantises: the length it derives is rounded up to a whole multiple of mSize,
		// so 700.0 of ours becomes 800. Constraining the input works whenever the derived value can
		// represent what we asked for, and a length cannot: a Junction 7.5 m away needs 750, vanilla
		// can only produce multiples of 100, and no aim point anywhere makes 750 reachable. There is no
		// input that yields the right output, so correcting the output is the only route.
		//
		// SetLength is public on AFGBuildableBeam (FGBuildableBeam.h:68) and this runs inside Construct,
		// BEFORE the buildable's BeginPlay positions terminal B from mLength (the order is ConfigureActor,
		// ConfigureComponents, BeginPlay). So the terminal lands on the cell the snap chose and §6.1
		// coincides a tick later, with nothing else touched.
		// GetOwnLength: the far-end snap's exact length or a §11 lane Rail's whole-metre length — both
		// are lengths vanilla's quantised mCurrentLength may not equal, and both are corrected the same
		// way. -1 means vanilla's length is the one to build.
		const float requested = static_cast< float >( GetOwnLength() );
		const float vanillaLength = rail->GetLength();
		const bool correcting =
			requested > 0.0f && !FMath::IsNearlyEqual( vanillaLength, requested, 0.5f );

		if( correcting )
		{
			rail->SetLength( requested );
		}

		ACPR_LOG( Display, RAIL_HOLO,
			TEXT( "ConfigureActor -> %s | length=%.1f (vanilla=%.1f) | farSnap=%d lane=%d "
			      "requested=%.1f delta=%.1f corrected=%d | at %s axis=%s | bridge=%d%s%s" ),
			*rail->GetName(), rail->GetLength(), vanillaLength,
			mFarSnapped ? 1 : 0,
			( mLaneLength > 0.0 ) ? 1 : 0,
			requested,
			( requested >= 0.0f ) ? vanillaLength - requested : 0.0f,
			correcting ? 1 : 0,
			*rail->GetActorLocation().ToString(),
			*rail->GetActorForwardVector().ToString(),
			mIsBridge ? 1 : 0,
			mIsBridge ? TEXT( " " ) : TEXT( "" ),
			mIsBridge ? *mBridgeLabel : TEXT( "" ) );

		// What the hologram handed over, beside what the Rail took. §10.6's customization inheritance and
		// §10.1's space both ride on vanilla's ConfigureActor; these two fields say whether they arrived.
		if( mIsBridge )
		{
			ACPR_LOG( Display, BRIDGE,
				TEXT( "ConfigureActor -> %s | swatch holo=%s rail=%s | pattern holo=%s rail=%s "
				      "| space holo=%s rail=%s | recipe=%s | at %s rot=%s" ),
				*rail->GetName(),
				*GetNameSafe( mCustomizationData.SwatchDesc.Get() ),
				*GetNameSafe( rail->GetCustomizationData_Native().SwatchDesc.Get() ),
				*GetNameSafe( mCustomizationData.PatternDesc.Get() ),
				*GetNameSafe( rail->GetCustomizationData_Native().PatternDesc.Get() ),
				mBlueprintDesigner ? *mBlueprintDesigner->GetName() : TEXT( "world" ),
				rail->GetHostDesigner() ? *rail->GetHostDesigner()->GetName() : TEXT( "world" ),
				*GetNameSafe( rail->GetBuiltWithRecipe().Get() ),
				*rail->GetActorLocation().ToString(),
				*rail->GetActorRotation().ToString() );
		}

		// A Warning for the correction not taking. If SetLength is ever overridden or clamped, this is
		// the line that says so — and it is the only way left for terminal B to miss its cell.
		if( requested > 0.0f && !FMath::IsNearlyEqual( rail->GetLength(), requested, 0.5f ) )
		{
			ACPR_LOG( Warning, RAIL_HOLO,
				TEXT( "length correction did not take: asked %.1f, SetLength "
				      "left %.1f. Terminal B will miss the target cell by %.1f uu and no Coupling will "
				      "form." ),
				requested, rail->GetLength(), rail->GetLength() - requested );
		}
	}
	else
	{
		ACPR_LOG( Error, RAIL_HOLO,
			TEXT( "ConfigureActor -> %s is not an AACPRRail. Build_PowerRail's "
			      "parent class is wrong." ),
			inBuildable ? *inBuildable->GetName() : TEXT( "null" ) );
	}
}

void AACPRRailHologram::ConfigureComponents( AFGBuildable* inBuildable ) const
{
	Super::ConfigureComponents( inBuildable );

	// Appendix B.2: this runs ONLY for hand placement. It never runs on the blueprint path and is
	// skipped on save load, so nothing a Rail needs in order to function may live here. Kept as a
	// marker so the difference between the two paths stays visible in the log.
	ACPR_LOG( Verbose, RAIL_HOLO,
		TEXT( "ConfigureComponents -> %s  (hand placement only)" ),
		inBuildable ? *inBuildable->GetName() : TEXT( "null" ) );
}

// =====================================================================================================
// §11 lane lattice. See the header block for the rule and who decides what.
// =====================================================================================================

bool AACPRRailHologram::IsLaneBuildMode() const
{
	// The two protected TSubclassOf fields of AFGBeamHologram (FGBeamHologram.h:58-62) and the reflected
	// AFGHologram::IsCurrentBuildMode (FGHologram.h:266). A null field is simply a mode that is not current.
	const bool diagonal = mBuildModeDiagonal && IsCurrentBuildMode( mBuildModeDiagonal );
	const bool freeForm = mBuildModeFreeForm && IsCurrentBuildMode( mBuildModeFreeForm );
	return !diagonal && !freeForm;
}

double AACPRRailHologram::GetOwnLength() const
{
	if( mFarSnapped && mRequestedFarLength > 0.0f )
	{
		return static_cast< double >( mRequestedFarLength );
	}

	return ( mLaneLength > 0.0 ) ? mLaneLength : -1.0;
}

void AACPRRailHologram::ClearLane()
{
	mLaneMode = false;
	mLaneCentre = FVector::ZeroVector;
	mLaneNormal = FVector::ZeroVector;
	mLaneAxis = FVector::ZeroVector;
	mLaneStart = FVector::ZeroVector;
	mLaneLength = -1.0;
	mLaneFrozen = false;
}

FVector AACPRRailHologram::LaneStartFor( const FVector& axis ) const
{
	// ACROSS THE SURFACE: the edge of the clicked cell on the drag side, which is exactly the face centre a
	// Junction standing in that cell would offer — whole metre along the axis, half metre across it.
	//
	// ALONG THE NORMAL (away from the surface, or into it): mid-cell ON the surface, so a Rail standing up
	// from a floor stands on it rather than starting a metre in the air, and its far end lands on the
	// bottom face of a Junction stacked from that floor (whole metres from the surface).
	if( FMath::Abs( FVector::DotProduct( axis, mLaneNormal ) ) > 0.5 )
	{
		return mLaneCentre - mLaneNormal * static_cast< double >( mSurfaceOffset );
	}

	return mLaneCentre + axis * 50.0;
}

FQuat AACPRRailHologram::LaneRotationFor( const FVector& axis ) const
{
	// Roll is cosmetic (invariant 12) and the profile is square, so `up` only has to be deterministic and
	// perpendicular: world up for any horizontal Rail; for a vertical one, the wall's normal on a wall and
	// world +X on a floor or ceiling. The player's scroll roll composes on top (ApplyRoll).
	FVector up = FVector::UpVector;
	if( FMath::Abs( axis.Z ) > 0.5 )
	{
		up = ( FMath::Abs( mLaneNormal.Z ) < 0.5 ) ? mLaneNormal : FVector::ForwardVector;
	}

	return FRotationMatrix::MakeFromXZ( axis, up ).ToQuat();
}

double AACPRRailHologram::LaneAimAlong( const FHitResult& hitResult, const FVector& start, const FVector& axis,
                                        const TCHAR*& out_source, double& out_other ) const
{
	const FVector rayVector = hitResult.TraceEnd - hitResult.TraceStart;
	const bool hasRay = rayVector.SizeSquared() > 1.0;

	// The aim POINT: the impact when something was hit, otherwise the trace's far end.
	const FVector point = hitResult.bBlockingHit
		? FVector( hitResult.ImpactPoint )
		: ( hasRay ? FVector( hitResult.TraceEnd ) : FVector( hitResult.Location ) );

	const double projected = FVector::DotProduct( point - start, axis );

	// The ray's closest approach to the Rail's line, for a Rail along the NORMAL only. A Rail standing up
	// from a floor is usually aimed at empty air or at a wall far behind it; projecting that far point onto
	// the vertical gives its height, not the height the crosshair shows at the Rail. The closest approach is
	// the latter. Across the surface the impact point is on (or near) the surface under the Rail and its
	// projection is what the crosshair shows — and it is what the anchored path has always used.
	//
	//   minimise |start + axis·t − (O + r·s)|²  →  t = (b·e − d) / (1 − b²),  s = e + b·t
	//   with b = axis·r, d = axis·(start − O), e = r·(start − O)
	const bool normalAxis = FMath::Abs( FVector::DotProduct( axis, mLaneNormal ) ) > 0.5;
	if( normalAxis && hasRay )
	{
		const FVector r = rayVector.GetSafeNormal();
		const FVector w0 = start - hitResult.TraceStart;
		const double b = FVector::DotProduct( axis, r );
		const double d = FVector::DotProduct( axis, w0 );
		const double e = FVector::DotProduct( r, w0 );
		const double denominator = 1.0 - b * b;

		// Nearly parallel (looking straight along the Rail) has no meaningful closest point; nor does one
		// behind the camera. Both fall back to the projection.
		if( denominator > 1e-3 )
		{
			const double t = ( b * e - d ) / denominator;
			const double s = e + b * t;
			if( s > 0.0 )
			{
				out_source = TEXT( "ray" );
				out_other = projected;
				return t;
			}
		}
	}

	out_source = hitResult.bBlockingHit ? TEXT( "impact" ) : ( hasRay ? TEXT( "trace-end" ) : TEXT( "location" ) );
	out_other = projected;
	return projected;
}

double AACPRRailHologram::ReadPreviewLength() const
{
	// The same component ApplyLengthToPreview writes; its Z scale is a length over the default length.
	const AACPRRail* cdo = GetRailCDO();
	if( !cdo || cdo->GetDefaultLength() <= 0.0f )
	{
		return -1.0;
	}

	const UStaticMeshComponent* preview = GetPreviewMesh();
	return preview ? preview->GetRelativeScale3D().Z * static_cast< double >( cdo->GetDefaultLength() ) : -1.0;
}

void AACPRRailHologram::FreezeLaneForLock()
{
	// The drag means nothing once the player has locked: axis and length stay as they were, and the
	// location belongs to vanilla's nudge placement (AFGHologram::SetHologramNudgeLocation, FGHologram.h:209)
	// — the same "a locked hologram is the player's" rule ApplyPlacementOffsets follows.
	//
	// A far snap that was live becomes a plain length. Its terminal was right for the start the Rail had
	// when locked; a nudge moves the start, so keeping the association would build a Rail claiming a target
	// its end does not reach. The length itself is kept, exact.
	if( !mLaneFrozen )
	{
		mLaneFrozen = true;

		const bool hadFar = mFarSnapped && mRequestedFarLength > 0.0f;
		if( hadFar )
		{
			mLaneLength = static_cast< double >( mRequestedFarLength );
			ClearFarSnap();
		}

		LogLaneEvent( FString::Printf( TEXT( "FROZEN by the lock | axis=%s length=%.1f%s | nudges move it from here" ),
			*mLaneAxis.ToString(), mLaneLength, hadFar ? TEXT( " (far snap released, length kept)" ) : TEXT( "" ) ) );
	}

	mLaneStart = GetActorLocation();
}

bool AACPRRailHologram::DriveLane( const FHitResult& hitResult )
{
	if( !mLaneMode || IsAnchorSnapped() )
	{
		return false;
	}

	// The mode switched after the click. Diagonal and FreeForm are vanilla's for the rest of this Rail.
	if( !IsLaneBuildMode() )
	{
		LogLaneEvent( TEXT( "OFF | the build mode changed after the first click; vanilla placement for the rest of this Rail" ) );
		ClearLane();
		ClearFarSnap();
		return false;
	}

	if( IsHologramLocked() )
	{
		// Locked before the first lane frame chose an axis: nothing to freeze, and vanilla's own step 2 is
		// the honest fallback for this one frame.
		if( mLaneAxis.IsNearlyZero() )
		{
			return false;
		}

		FreezeLaneForLock();
		CaptureRollBase( LaneRotationFor( mLaneAxis ) );
		ApplyRoll();
		return true;
	}

	if( mLaneFrozen )
	{
		mLaneFrozen = false;
		LogLaneEvent( TEXT( "THAWED by unlocking | the lane is live again from the clicked cell" ) );
	}

	const AACPRRail* cdo = GetRailCDO();
	const double size = FMath::Max( 1.0, cdo ? static_cast< double >( cdo->GetSize() ) : 100.0 );
	const double maximum = FMath::Max( size, cdo ? static_cast< double >( cdo->GetMaxLength() ) : 4000.0 );

	// Pass 1 — vanilla chooses the direction, from the cell centre. Its Default mode is what "the drag picks
	// the axis" feels like in every other beam, so its answer is taken rather than imitated. Placed at C so
	// the choice cannot depend on which edge last frame's axis happened to start from (that would oscillate
	// at every near-diagonal aim).
	// Roll-free as well: the player's scroll roll is ours to compose afterwards, and must not be a frame
	// vanilla could read its axes from.
	SetActorLocationAndRotation( mLaneCentre, mRollFreeRotation );
	Super::SetHologramLocationAndRotation( hitResult );

	const FVector vanillaDirection = GetActorForwardVector();
	const FVector vanillaLocation = GetActorLocation();
	const double vanillaLength1 = ReadPreviewLength();

	// The nearest world axis direction to vanilla's.
	FVector axis = FVector::ZeroVector;
	double alignment = 0.0;
	{
		const double ax = FMath::Abs( vanillaDirection.X );
		const double ay = FMath::Abs( vanillaDirection.Y );
		const double az = FMath::Abs( vanillaDirection.Z );

		if( ax >= ay && ax >= az ) { axis = FVector( FMath::Sign( vanillaDirection.X ), 0.0, 0.0 ); alignment = ax; }
		else if( ay >= az )        { axis = FVector( 0.0, FMath::Sign( vanillaDirection.Y ), 0.0 ); alignment = ay; }
		else                       { axis = FVector( 0.0, 0.0, FMath::Sign( vanillaDirection.Z ) ); alignment = az; }
	}

	// Vanilla chose something that is not a world axis — not expected in the Default mode on a surface the
	// step-1 test accepted, so it is logged, and this frame keeps vanilla's placement untouched rather than
	// forcing a Rail off the direction vanilla drew.
	if( alignment < 0.999 )
	{
		mLaneAxis = FVector::ZeroVector;
		mLaneLength = -1.0;
		ClearFarSnap();
		CaptureRollBase( GetActorQuat() );
		ApplyRoll();

		// Once per run of such frames, so a stretch of them cannot spend the event budget the ON / FROZEN
		// lines need.
		if( !mLaneOffAxisLogged )
		{
			mLaneOffAxisLogged = true;
			LogLaneEvent( FString::Printf( TEXT( "vanilla direction %s is %.4f off a world axis — vanilla placement "
				"while it stays so (a click now builds off the lattice)" ), *vanillaDirection.ToString(), 1.0 - alignment ) );
		}
		return true;
	}
	mLaneOffAxisLogged = false;

	const FVector start = LaneStartFor( axis );
	SetActorLocation( start );
	mLaneAxis = axis;
	mLaneStart = start;

	// Pass 2 — the length, and vanilla's agreement with it.
	FHitResult adjusted = hitResult;
	const bool farSnapped = TryFarEndSnap( adjusted );

	const TCHAR* source = TEXT( "far-snap" );
	double aim = 0.0;
	double other = 0.0;
	double length = 0.0;

	if( farSnapped )
	{
		// A terminal on the lane wins, at its exact length; TryFarEndSnap has already put the aim on it.
		length = static_cast< double >( mRequestedFarLength );
		aim = length;
	}
	else
	{
		aim = LaneAimAlong( hitResult, start, axis, source, other );

		// Whole metres, rounded UP as vanilla rounds its own drag, within the Rail's limits.
		length = FMath::Clamp( FMath::CeilToDouble( aim / size ) * size, size, maximum );

		// The aim handed to vanilla, placed so that its ceiling is `length` whether vanilla measures from
		// the actor (now at `start`) or from where the actor stood at the click (the cell centre) — both are
		// on this axis, `delta` apart, and a point (length − k) from start with k = (size + delta) / 2 lies
		// inside (length − size, length] from each. Vanilla's own readout and mCurrentLength then agree with
		// ours; the log below prints what vanilla actually produced, so a third behaviour would show.
		const double delta = FVector::DotProduct( start - mLaneCentre, axis );
		const double k = FMath::Clamp( 0.5 * ( size + delta ), 1.0, size - 1.0 );
		const FVector target = start + axis * ( length - k );
		adjusted.ImpactPoint = target;
		adjusted.Location = target;
	}

	mLaneLength = length;

	Super::SetHologramLocationAndRotation( adjusted );
	const double vanillaLength2 = ReadPreviewLength();
	const FVector vanillaLocation2 = GetActorLocation();

	SetActorLocation( start );
	CaptureRollBase( LaneRotationFor( axis ) );
	ApplyRoll();

	// One line per change of axis, 1 m of length, or far target — a drag prints a readable trail.
	if( !mLaneLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return true;
	}

	const FString key = FString::Printf( TEXT( "%s|%d|%s" ),
		*axis.ToString(), FMath::RoundToInt32( length / 100.0 ),
		farSnapped && mFarTerminal.IsValid() ? *mFarTerminal->GetName() : TEXT( "-" ) );

	if( mLaneLog.Admit( key ) )
	{
		ACPR_LOG( Verbose, RAIL_LANE,
			TEXT( "drive | axis=%s start=%s end=%s | length=%.1f aim=%.1f via %s (projection %.1f) "
			      "farSnap=%d | vanilla: dir=%s loc=%s len1=%.1f -> len2=%.1f loc2=%s | centre=%s | hit=%d trace=%s->%s" ),
			*axis.ToString(), *start.ToString(), *( start + axis * length ).ToString(),
			length, aim, source, other,
			farSnapped ? 1 : 0,
			*vanillaDirection.ToString(), *vanillaLocation.ToString(), vanillaLength1, vanillaLength2,
			*vanillaLocation2.ToString(),
			*mLaneCentre.ToString(),
			hitResult.bBlockingHit ? 1 : 0,
			*hitResult.TraceStart.ToString(), *hitResult.TraceEnd.ToString() );
	}

	return true;
}

void AACPRRailHologram::LogLaneEvent( const FString& what )
{
	if( mLaneEventLogBudget <= 0 || !ACPR_LOG_ACTIVE( Verbose ) )
	{
		return;
	}
	--mLaneEventLogBudget;

	ACPR_LOG( Verbose, RAIL_LANE, TEXT( "%s | %s" ), *GetName(), *what );
}
