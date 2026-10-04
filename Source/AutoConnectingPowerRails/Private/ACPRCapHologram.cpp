// Auto-Connecting Power Rails — the Insulated Terminal Cap hologram.

#include "ACPRCapHologram.h"

#include "ACPRCap.h"
#include "ACPRDesignerSpace.h"
#include "ACPRRail.h"
#include "ACPRJunction.h"
#include "ACPRTerminalComponent.h"
#include "ACPRTerminalHost.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "FGConstructDisqualifier.h"

AACPRCapHologram::AACPRCapHologram()
{
	// A real default, because a null one refuses nothing: with no disqualifier added,
	// CheckValidPlacement is silent, the hologram goes green on a frame with no terminal under the
	// crosshair, and a Cap gets built capping nothing.
	//
	// UFGCDInvalidAimLocation rather than UFGCDMustSnap: MustSnap's string is
	// ConstructDisqualifiers/BuildMode/RequiresSnapToFoundation, which would send the player looking
	// for a foundation. InvalidAimLocation says they are not aiming at anything valid, which is what
	// this failure actually is.
	mNoTerminalDisqualifier = UFGCDInvalidAimLocation::StaticClass();
}

void AACPRCapHologram::BeginPlay()
{
	Super::BeginPlay();

	BindPreviewMesh();
	FitPreviewMesh();

	// The two hosts this hologram targets, on vanilla's list for it (FGHologram.h:612).
	AddValidHitClass( AACPRRail::StaticClass() );
	AddValidHitClass( AACPRJunction::StaticClass() );

	ACPR_LOG( Verbose, CAP_HOLO,
		TEXT( "BeginPlay (%s) | BUILD=%s | range=%.1f ambiguity=%.1f faceAlign=%.2f "
		      "| disqualifier=%s | preview=%s" ),
		*GetName(), ACPR_BUILD_STAMP(),
		mTerminalSnapRange, mAmbiguityTolerance, mFaceAlignmentMinimum,
		mNoTerminalDisqualifier ? *mNoTerminalDisqualifier->GetName()
		                        : TEXT( "<none set — see the header>" ),
		mPreviewMesh.IsValid() ? *mPreviewMesh->GetName() : TEXT( "<none>" ) );
}

void AACPRCapHologram::PostHologramPlacement( const FHitResult& hitResult, bool callForChildren )
{
	// Super first: the placement pass, including TrySnapToActor, is what writes mTargetTerminal and
	// mTargetHost. Both of the calls below read that, so neither can run ahead of it.
	Super::PostHologramPlacement( hitResult, callForChildren );

	// Re-fitted per frame because the Cap's size depends on WHAT IT IS AIMED AT — §11 gives a Cap 0.4 m
	// on a Rail end and 0.9 m on a Junction face — so unlike the Junction's fixed cube this cannot be
	// a one-shot at BeginPlay. It is cheap (a scale and a location write, both absolute) and guarded on
	// the width actually having changed, so a steady aim costs nothing.
	FitPreviewMesh();

	FACPRSpace::WatchPlacementSpace( this, GetPlacementDesigner(), mLastPlacementSpace, mPlacementSpaceSeen );

	UpdateTerminalHighlight( hitResult );
}

void AACPRCapHologram::Destroyed()
{
	// §12's highlight sits on the aimed host's materials; the hologram's end takes it down.
	FACPRTerminalHighlight::HideAll( mHighlightHost );
	Super::Destroyed();
}

AFGBuildableBlueprintDesigner* AACPRCapHologram::GetPlacementDesigner() const
{
	return mBlueprintDesigner.Get();
}

void AACPRCapHologram::BindPreviewMesh()
{
	TArray< UStaticMeshComponent* > meshes;
	GetComponents< UStaticMeshComponent >( meshes );

	for( UStaticMeshComponent* m : meshes )
	{
		// Instanced ones own their own scale and are not the Cap's plate — and neither is anything
		// vanilla adds to the hologram; the plate is the buildable's CapMesh, by name (see the Outlet
		// hologram for what "the first plain mesh" would pick up).
		if( !m || m->IsA< UInstancedStaticMeshComponent >() || !m->GetName().StartsWith( TEXT( "CapMesh" ) ) )
		{
			continue;
		}

		mPreviewMesh = m;
		break;
	}

	if( !mPreviewMesh.IsValid() && !mPreviewBindWarned )
	{
		mPreviewBindWarned = true;

		ACPR_LOG( Warning, CAP_HOLO,
			TEXT( "no plain static mesh component at BeginPlay — the preview will not "
			      "be sized. Build_PowerRailCap's CapMesh is missing or empty." ) );
	}
}

void AACPRCapHologram::FitPreviewMesh()
{
	UStaticMeshComponent* mesh = mPreviewMesh.Get();

	// Self-healing rebind, with the Junction's lock guard: a hologram's own gizmo meshes exist only
	// while it is locked, so a search that never runs in that state cannot pick one up.
	if( !mesh && !IsHologramLocked() )
	{
		BindPreviewMesh();
		mesh = mPreviewMesh.Get();
	}

	if( !mesh || !mesh->GetStaticMesh() )
	{
		return;
	}

	// The same question AACPRCap::FitMesh asks of its host, asked of this frame's TARGET, and asked
	// the same way: by terminal COUNT rather than by a class cast. A Rail has two terminals, a
	// Junction six, and §8's Outlet gets a sensible width without being named.
	const AActor* host = mTargetHost.Get();
	const IACPRTerminalHost* terminalHost = host ? Cast< IACPRTerminalHost >( host ) : nullptr;

	const AACPRCap* cdo = GetBuildClass()
		? Cast< AACPRCap >( GetBuildClass()->GetDefaultObject() )
		: nullptr;

	// Fallbacks match the CDO's defaults. They only feed the log line and the width guard below, but
	// a guard comparing against a number no Cap is ever fitted to is a guard that fires on the wrong
	// frames.
	const float railWidth     = cdo ? cdo->mRailFaceWidth     : 44.0f;
	const float junctionWidth = cdo ? cdo->mJunctionFaceWidth : 93.172f;
	const bool  junctionLike  = terminalHost && terminalHost->GetTerminalCount() > 2;
	const float depth         = junctionLike
		? ( cdo ? cdo->mJunctionCapDepth : 2.0f )
		: ( cdo ? cdo->mRailCapDepth : 3.0f );
	const float standoff      = junctionLike
		? -( cdo ? cdo->mJunctionCapInset : 0.1f )
		: ( cdo ? cdo->mStandoff : 0.0f );

	const float width = ( terminalHost && terminalHost->GetTerminalCount() > 2 )
		? junctionWidth : railWidth;

	// Guarded on the width, not on a frame counter: the fit is absolute, so re-applying it is
	// harmless, but skipping it while nothing has changed keeps this off the per-frame cost sheet.
	if( FMath::IsNearlyEqual( width, mLastFittedWidth ) )
	{
		return;
	}
	mLastFittedWidth = width;

	// ApplyCapMesh, not FitCapMesh: the preview must wear the same one of the two cap meshes
	// the buildable will, and picking it is part of the same question as how wide it is.
	const FVector fitted = AACPRCap::ApplyCapMesh( mesh, cdo, junctionLike );

	if( !mPreviewFitLogged )
	{
		mPreviewFitLogged = true;

		ACPR_LOG( Verbose, CAP_HOLO,
			TEXT( "preview fit | component=%s mesh=%s -> fitted=%s | depth=%.1f "
			      "width=%.1f standoff=%.1f | cdo=%d" ),
			*mesh->GetName(),
			mesh->GetStaticMesh() ? *mesh->GetStaticMesh()->GetName() : TEXT( "<none>" ),
			*fitted.ToString(), depth, width, standoff, cdo ? 1 : 0 );
	}
}

void AACPRCapHologram::UpdateTerminalHighlight( const FHitResult& hitResult )
{
	// The host is the aimed actor, not the picked terminal's owner, and that is the whole point of §12:
	// the cue has to appear while the player is still sweeping towards a face, not only once a face has
	// been accepted. Show() hides everything when the actor is not a terminal host, so a frame aimed at
	// a foundation clears the highlight with no test here.
	FACPRSpaceFilter spaceFilter;
	spaceFilter.Enforce = true;
	spaceFilter.Space = GetPlacementDesigner();

	FACPRTerminalHighlight::Show( mHighlightHost, hitResult.GetActor(), mTargetTerminal.Get(), spaceFilter );
}

bool AACPRCapHologram::IsValidHitResult( const FHitResult& hitResult ) const
{
	// The shared verdict (ACPRHologramCommon.h): vanilla's list, kept from being bypassed by the base.
	const bool superSaid = Super::IsValidHitResult( hitResult );
	mAim.Record( hitResult );
	return ACPRHologram::WidenedHitVerdict( superSaid, hitResult,
		hitResult.GetActor() && IsValidHitActor( hitResult.GetActor() ), mValidityLog, ACPR_TAG_TEXT( CAP_HOLO ) );
}

void AACPRCapHologram::CheckValidFloor()
{
	// Vanilla's floor-angle rule is relaxed for the same reason the Junction's is: a Cap goes wherever a
	// terminal points — straight out of a wall-mounted Rail's end, or down off a Junction's bottom face.
	// The rule itself is ACPRHologram::FloorRuleAdmitsSuper.
	if( ACPRHologram::FloorRuleAdmitsSuper( GetMinPlacementFloorZ(), mAim, false, mFloorLog, ACPR_TAG_TEXT( CAP_HOLO ) ) )
	{
		Super::CheckValidFloor();
	}
}

bool AACPRCapHologram::CanNudgeHologram() const
{
	// §9 gives the Cap no freedom at all: its outer face is the terminal snap plane and §11 fixes its
	// external spacing at zero. A nudge would move it off both, and the two opposing Caps §6.2 allows
	// back-to-back would stop fitting.
	return false;
}

bool AACPRCapHologram::TrySnapToActor( const FHitResult& hitResult )
{
	mHasTarget = false;
	mTargetTerminal = nullptr;
	mTargetHost = nullptr;
	mTargetIndex = 0;

	// One implementation of the face filter, the two-pass tie count and invariant 8, shared with
	// every other hologram that asks this question. Nothing to get subtly different here.
	const FACPRTerminalPick pick = FACPRTerminalPicker::FindAimed(
		hitResult, mTerminalSnapRange, mAmbiguityTolerance, mFaceAlignmentMinimum );

	if( !pick.Terminal )
	{
		// Ambiguity is worth a line; "nothing in range" is the ordinary state of every frame the
		// player is not pointing at a terminal, and logging that would drown the file.
		if( pick.Tied > 1 )
		{
			LogSnap( TEXT( "REFUSED (ambiguous - invariant 8)" ), pick );
		}

		// Falling through to Super rather than returning false outright: a Cap has no other valid
		// placement, but letting vanilla position the hologram is what makes it VISIBLE while the
		// player sweeps towards a terminal, instead of vanishing until it snaps.
		return Super::TrySnapToActor( hitResult );
	}

	const IACPRTerminalHost* host = pick.Host ? Cast< IACPRTerminalHost >( pick.Host ) : nullptr;
	const int32 index = host ? host->GetIndexOfTerminal( pick.Terminal ) : INDEX_NONE;

	if( index == INDEX_NONE )
	{
		// The picker found a terminal on this actor and the actor does not own it. Not reachable
		// today — the picker only ever asks one host for its own terminals — but it is the kind of
		// thing that becomes reachable when §10's resolver starts handing terminals around, and a
		// silent out-of-range index would write a Cap onto the wrong face of the right actor.
		LogSnap( TEXT( "REFUSED (terminal not owned by its host)" ), pick );
		return Super::TrySnapToActor( hitResult );
	}

	// The placement, and it is entirely the terminal's frame.
	//
	// The Cap's local +X is its OUTWARD direction — the same convention the terminal itself uses and
	// the same one AACPRCap::FitMesh relies on when it pushes the mesh along local -X so the body
	// extends inward. The terminal's own X and Z are taken rather than a rotation built from the
	// outward direction alone, because a bare direction invents the other two axes and gimbal-locks
	// on a vertical one, which is exactly a Junction's top and bottom faces.
	const FTransform frame = pick.Terminal->GetComponentTransform();
	SetActorLocationAndRotation( pick.Terminal->GetComponentLocation(),
		FRotationMatrix::MakeFromXZ( frame.GetUnitAxis( EAxis::X ),
		                             frame.GetUnitAxis( EAxis::Z ) ).Rotator() );

	mTargetTerminal = pick.Terminal;
	mTargetHost = pick.Host;
	mTargetIndex = static_cast< uint8 >( index );
	mHasTarget = true;

	LogSnap( TEXT( "ACCEPTED" ), pick );
	return true;
}

void AACPRCapHologram::CheckValidPlacement()
{
	Super::CheckValidPlacement();

	// §12's red feedback, and it is unconditional. Guarded on the disqualifier field being set, a
	// null field would mean nothing refused the placement at all: the hologram green on a frame with
	// no terminal, and a Cap built capping nothing. A rule that switches itself off when its message
	// is missing is not a rule.
	if( !mHasTarget )
	{
		// The fallback is not belt-and-braces — it is what stops a null field from silently turning
		// the refusal off. §9 has no valid Cap placement that is not on an open terminal, so a frame
		// without one must never be buildable, whatever this property has been edited to.
		AddConstructDisqualifier( mNoTerminalDisqualifier
			? mNoTerminalDisqualifier : TSubclassOf< UFGConstructDisqualifier >( UFGCDInvalidAimLocation::StaticClass() ) );
	}

	// §10.1: a Cap in one build space may not occupy a terminal in another. A Designer Cap on a world
	// Rail's end would be saved into the blueprint without the Rail it caps.
	if( mHasTarget && FACPRSpace::IsForeignTarget( this, TEXT( "cap host" ), mTargetHost.Get(),
			GetPlacementDesigner(), mLastSpaceLog ) )
	{
		AddConstructDisqualifier( UFGCDDesignerWorldCommingling::StaticClass() );
	}
}

void AACPRCapHologram::ConfigureActor( AFGBuildable* inBuildable ) const
{
	Super::ConfigureActor( inBuildable );

	AACPRCap* cap = Cast< AACPRCap >( inBuildable );
	if( !cap )
	{
		ACPR_LOG( Error, CAP_HOLO,
			TEXT( "ConfigureActor -> %s is not an AACPRCap. Build_PowerRailCap's "
			      "parent class is wrong." ),
			inBuildable ? *inBuildable->GetName() : TEXT( "null" ) );
		return;
	}

	// Written here because this is the last moment that works:
	// FGBuildableHologram.h:322-327 puts ConfigureActor inside Construct, ahead of the
	// buildable's BeginPlay — which is exactly where AACPRCap::ApplyCap reads these fields. A frame
	// later would be too late, and ConfigureComponents is not an option because B.2 records that it
	// never runs on the blueprint path.
	cap->SetHostTerminal( mTargetHost.Get(), mTargetIndex );

	ACPR_LOG( Display, CAP_HOLO,
		TEXT( "ConfigureActor -> %s | host=%s index=%d | hadTarget=%d" ),
		*cap->GetName(),
		IsValid( mTargetHost ) ? *mTargetHost->GetName() : TEXT( "<none>" ),
		static_cast< int32 >( mTargetIndex ),
		mHasTarget ? 1 : 0 );
}

void AACPRCapHologram::LogSnap( const TCHAR* verdict, const FACPRTerminalPick& pick )
{
	if( !mSnapLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	const FString key = FString::Printf( TEXT( "%s|%s" ),
		verdict, pick.Terminal ? *pick.Terminal->GetName() : TEXT( "-" ) );

	if( !mSnapLog.Admit( key ) )
	{
		return;
	}

	// state= is the interesting one on a refusal: a terminal that is coupled or capped never reaches
	// the picker at all, because IsOpen() is false, so "nothing found" while the crosshair is clearly
	// on a face means the face is already occupied — which is §9's rule working, not a failure.
	ACPR_LOG( Verbose, CAP_HOLO,
		TEXT( "cap-snap %s | host=%s terminal=%s | dist=%.1f open=%d cand=%d tied=%d "
		      "| cell=%s rot=%s" ),
		verdict,
		pick.Host ? *pick.Host->GetName() : TEXT( "<none>" ),
		pick.Terminal ? *pick.Terminal->GetName() : TEXT( "-" ),
		pick.Distance, pick.OpenInRange, pick.Candidates, pick.Tied,
		pick.Terminal ? *pick.Terminal->GetQuantizedLocation().ToString() : TEXT( "-" ),
		*GetActorRotation().ToString() );
}
