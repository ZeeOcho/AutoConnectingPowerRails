// Auto-Connecting Power Rails — the Power Rail Outlet hologram.

#include "ACPROutletHologram.h"

#include "ACPRRail.h"
#include "ACPRDesignerSpace.h"
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

AACPROutletHologram::AACPROutletHologram()
{
	// See AACPRCapHologram's constructor: with a null default nothing would refuse an invalid target,
	// and §15's "Coupled/capped terminal, or Junction body | Invalid" would go unenforced.
	mInvalidTargetDisqualifier = UFGCDInvalidAimLocation::StaticClass();
}

void AACPROutletHologram::BeginPlay()
{
	Super::BeginPlay();

	BindPreviewMesh();
	FitPreviewMesh();

	// The two hosts this hologram targets, on vanilla's list for it (FGHologram.h:612).
	AddValidHitClass( AACPRRail::StaticClass() );
	AddValidHitClass( AACPRJunction::StaticClass() );

	ACPR_LOG( Verbose, OUTLET_HOLO,
		TEXT( "BeginPlay (%s) | BUILD=%s | range=%.1f endFace=%.2f "
		      "railHalfWidth=%.1f guideline=%.1f | disqualifier=%s | preview=%s" ),
		*GetName(), ACPR_BUILD_STAMP(),
		mTerminalSnapRange, mEndFaceAlignment, mRailHalfWidth, mBodyGuidelineStep,
		mInvalidTargetDisqualifier ? *mInvalidTargetDisqualifier->GetName()
		                           : TEXT( "<none set — see AACPRCapHologram's equivalent>" ),
		mPreviewMesh.IsValid() ? *mPreviewMesh->GetName() : TEXT( "<none>" ) );
}

void AACPROutletHologram::PostHologramPlacement( const FHitResult& hitResult, bool callForChildren )
{
	// Super first: the placement pass, TrySnapToActor included, is what writes this frame's target,
	// and both calls below read it.
	Super::PostHologramPlacement( hitResult, callForChildren );

	// The fit depends on the seat — pad mesh, width and inset all follow what is aimed at — so this
	// runs every frame and FitPreviewMesh itself returns early while the seat is the one it last
	// fitted.
	FitPreviewMesh();

	FACPRSpace::WatchPlacementSpace( this, GetPlacementDesigner(), mLastPlacementSpace, mPlacementSpaceSeen );

	UpdateTerminalHighlight( hitResult );
}

void AACPROutletHologram::Destroyed()
{
	// §12's highlight sits on the aimed host's materials; the hologram's end takes it down.
	FACPRTerminalHighlight::HideAll( mHighlightHost );
	Super::Destroyed();
}

AFGBuildableBlueprintDesigner* AACPROutletHologram::GetPlacementDesigner() const
{
	return mBlueprintDesigner.Get();
}

void AACPROutletHologram::BindPreviewMesh()
{
	TArray< UStaticMeshComponent* > meshes;
	GetComponents< UStaticMeshComponent >( meshes );

	for( UStaticMeshComponent* m : meshes )
	{
		if( !m || m->IsA< UInstancedStaticMeshComponent >() )
		{
			continue;
		}

		// Two plain meshes, the cylinder and the pad, BOTH by name. The hologram also makes vanilla's own
		// wire-connection indicator for a visible power connection (PowerLineHologramMesh, for the
		// socket), which "the first plain mesh that is not the pad" would pick up and scale to a 40 cm
		// cube. The buildable's components are named; nothing else is ours.
		if( m->GetName().StartsWith( TEXT( "OutletBase" ) ) )
		{
			mPreviewBase = m;
		}
		else if( m->GetName().StartsWith( TEXT( "OutletMesh" ) ) )
		{
			mPreviewMesh = m;
		}
	}

	if( !mPreviewMesh.IsValid() && !mPreviewBindWarned )
	{
		mPreviewBindWarned = true;

		ACPR_LOG( Warning, OUTLET_HOLO,
			TEXT( "no plain static mesh component at BeginPlay — the preview will "
			      "not be sized. Build_PowerRailOutlet's OutletMesh is missing or empty." ) );
	}
}

void AACPROutletHologram::FitPreviewMesh()
{
	UStaticMeshComponent* mesh = mPreviewMesh.Get();

	// Self-healing rebind with the Junction's lock guard: a hologram's own gizmo meshes exist only
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

	const AACPROutlet* cdo = GetBuildClass()
		? Cast< AACPROutlet >( GetBuildClass()->GetDefaultObject() )
		: nullptr;

	const float visualExtent = cdo ? cdo->mVisualExtent : 40.0f;

	// The preview sinks the same way the built one does, which is the reason this file shares
	// FitOutletMesh at all. mMode is already the mount kind this frame, and the host is the
	// one the picker accepted, so the same terminal-COUNT test applies here as in FitMesh.
	// mTargetHost is a TWeakObjectPtr, so it is resolved before it is cast.
	AActor* const fitHostActor = IsValid( mTargetHost ) ? mTargetHost.Get() : nullptr;
	const IACPRTerminalHost* fitHost = ( mMode == EACPROutletHostMode::Terminal && fitHostActor )
		? Cast< IACPRTerminalHost >( fitHostActor ) : nullptr;
	const AACPROutlet::ESeat seat = AACPROutlet::SeatFor( mMode, fitHost );
	if( mPreviewFitted && static_cast< int32 >( seat ) == mPreviewSeat )
	{
		return;
	}
	mPreviewSeat = static_cast< int32 >( seat );
	const float standoff = 0.0f;   // reported only; the seat decides the real one inside FitOutletMeshes

	AACPROutlet::FitOutletMeshes( mesh, mPreviewBase.Get(), seat, cdo );
	const FVector fitted = mesh->GetRelativeScale3D();
	mPreviewFitted = true;

	if( !mPreviewFitLogged )
	{
		mPreviewFitLogged = true;

		ACPR_LOG( Verbose, OUTLET_HOLO,
			TEXT( "preview fit | component=%s mesh=%s -> fitted=%s | extent=%.1f "
			      "standoff=%.1f | cdo=%d" ),
			*mesh->GetName(),
			mesh->GetStaticMesh() ? *mesh->GetStaticMesh()->GetName() : TEXT( "<none>" ),
			*fitted.ToString(), visualExtent, standoff, cdo ? 1 : 0 );
	}
}

void AACPROutletHologram::UpdateTerminalHighlight( const FHitResult& hitResult )
{
	// The host is the AIMED actor rather than the picked terminal's owner — the cue has to appear
	// while the player is still sweeping towards a face. Show() clears everything when the actor is
	// not a terminal host, so a frame aimed at a foundation needs no test here.
	FACPRSpaceFilter spaceFilter;
	spaceFilter.Enforce = true;
	spaceFilter.Space = GetPlacementDesigner();

	FACPRTerminalHighlight::Show( mHighlightHost, hitResult.GetActor(), mTargetTerminal.Get(), spaceFilter );
}

bool AACPROutletHologram::IsValidHitResult( const FHitResult& hitResult ) const
{
	// The shared verdict (ACPRHologramCommon.h): vanilla's list, kept from being bypassed by the base.
	const bool superSaid = Super::IsValidHitResult( hitResult );
	mAim.Record( hitResult );
	return ACPRHologram::WidenedHitVerdict( superSaid, hitResult,
		hitResult.GetActor() && IsValidHitActor( hitResult.GetActor() ), mValidityLog, ACPR_TAG_TEXT( OUTLET_HOLO ) );
}

void AACPROutletHologram::CheckValidFloor()
{
	// An Outlet mounts on whichever face is aimed at — a Rail's underside, a wall-mounted
	// Rail's side, a Junction's bottom terminal — and vanilla's floor-angle rule refuses every one of
	// those. The rule itself is ACPRHologram::FloorRuleAdmitsSuper.
	if( ACPRHologram::FloorRuleAdmitsSuper( GetMinPlacementFloorZ(), mAim, false, mFloorLog, ACPR_TAG_TEXT( OUTLET_HOLO ) ) )
	{
		Super::CheckValidFloor();
	}
}

bool AACPROutletHologram::CanNudgeHologram() const
{
	// §8.1 and §8.2 both say "cannot be nudged". The host face decides the position; a nudge would
	// move a body-mounted Outlet off its Rail and a terminal-mounted one off its terminal plane.
	return false;
}

void AACPROutletHologram::ClearTarget()
{
	mHasTarget = false;
	mTargetHost = nullptr;
	mTargetTerminal = nullptr;
	mTargetIndex = 0;
	mBodyAlong = 0.0;
	mMode = EACPROutletHostMode::Terminal;
}

bool AACPROutletHologram::TrySnapToActor( const FHitResult& hitResult )
{
	ClearTarget();

	// §8.2 first, always. §8.1's "aiming at an end face resolves to terminal-mounted or invalid,
	// never silently to a body attachment" is an ordering requirement before it is a geometry one:
	// ask the terminal question, and only consider the body if the answer is no.
	if( TryTerminalMount( hitResult ) )
	{
		return true;
	}

	if( TryBodyMount( hitResult ) )
	{
		return true;
	}

	// Neither. Super still positions the hologram so it stays visible while the player sweeps toward
	// something valid; CheckValidPlacement is what refuses the build.
	return Super::TrySnapToActor( hitResult );
}

bool AACPROutletHologram::TryTerminalMount( const FHitResult& hitResult )
{
	const FACPRTerminalPick pick = FACPRTerminalPicker::FindAimed(
		hitResult, mTerminalSnapRange, mAmbiguityTolerance, mFaceAlignmentMinimum );

	if( !pick.Terminal )
	{
		if( pick.Tied > 1 )
		{
			LogMount( TEXT( "REFUSED (ambiguous - invariant 8)" ), TEXT( "terminal" ) );
		}
		return false;
	}

	const IACPRTerminalHost* host = pick.Host ? Cast< IACPRTerminalHost >( pick.Host ) : nullptr;
	const int32 index = host ? host->GetIndexOfTerminal( pick.Terminal ) : INDEX_NONE;
	if( index == INDEX_NONE )
	{
		LogMount( TEXT( "REFUSED (terminal not owned by its host)" ), TEXT( "terminal" ) );
		return false;
	}

	// §8.2: "Mounting normal from the terminal axis, rotation about it." Local +X is the mounting
	// normal by the convention AACPROutlet::FitMesh stands the body off along, and MakeFromXZ rather
	// than a bare direction, because a direction alone invents the other two axes and gimbal-locks
	// on a vertical one, which is a Junction's top and bottom faces exactly.
	const FTransform frame = pick.Terminal->GetComponentTransform();
	SetActorLocationAndRotation( pick.Terminal->GetComponentLocation(),
		FRotationMatrix::MakeFromXZ( frame.GetUnitAxis( EAxis::X ),
		                             frame.GetUnitAxis( EAxis::Z ) ).Rotator() );

	mMode = EACPROutletHostMode::Terminal;
	mTargetHost = pick.Host;
	mTargetTerminal = pick.Terminal;
	mTargetIndex = static_cast< uint8 >( index );
	mHasTarget = true;

	LogMount( TEXT( "ACCEPTED" ), TEXT( "terminal" ) );
	return true;
}

bool AACPROutletHologram::TryBodyMount( const FHitResult& hitResult )
{
	if( !hitResult.bBlockingHit )
	{
		return false;
	}

	// §8.1: "Targets one Rail body — the four longitudinal faces only, never a Junction body." The
	// class test IS the rule; a Junction reaching this point has already failed the terminal question
	// above, which means the player is aiming at its body rather than a face centre, and §15 calls
	// that invalid rather than a body mount.
	AACPRRail* rail = Cast< AACPRRail >( hitResult.GetActor() );
	if( !rail )
	{
		return false;
	}

	const FVector normal = hitResult.ImpactNormal.GetSafeNormal();
	if( normal.IsNearlyZero() )
	{
		return false;
	}

	// The beam axis is the actor's local X (measured against the rendered world extent).
	const FTransform railFrame = rail->GetActorTransform();
	const FVector axis = railFrame.GetUnitAxis( EAxis::X );

	// §8.1's end-face rule, and it is a refusal rather than a fall-through. A normal lying along the
	// Rail's own axis is an end cap, and §8.1 says such a hit resolves to terminal-mounted or
	// invalid. The terminal question was already asked and said no — which means that end's terminal
	// is coupled, capped, or ambiguous — so the answer here is invalid, and returning false lands it
	// there. Falling through to a body mount would clamp a Power Connection onto the cap of a Rail at
	// a position that moves the moment §7.3 splits it.
	if( FMath::Abs( FVector::DotProduct( normal, axis ) ) >= mEndFaceAlignment )
	{
		LogMount( TEXT( "REFUSED (end face - terminal is occupied or ambiguous)" ), TEXT( "body" ) );
		return false;
	}

	// Where along the Rail. Rail-local, because §8.1's Guidelines step is Rail-local and because
	// §11 wants a coordinate "stable under Rail reversal, save/load, blueprint rotation and later
	// splitting" — the same reason §7.3's insertion uses one.
	const FVector origin = railFrame.GetLocation();
	double along = FVector::DotProduct( hitResult.ImpactPoint - origin, axis );

	// §8.1: "Slides freely, snaps to the 1 m grid on Guidelines." mSnapToGuideLines is the flag the
	// build gun hands down, read directly rather than decoded from a value.
	if( mSnapToGuideLines && mBodyGuidelineStep > 0.0f )
	{
		along = FMath::GridSnap( along, static_cast< double >( mBodyGuidelineStep ) );
	}

	along = FMath::Clamp( along, 0.0, static_cast< double >( rail->GetLength() ) );

	// The face normal is quantised to the Rail's own frame rather than used raw, so the Outlet sits
	// square on one of the four longitudinal faces instead of at whatever angle the crosshair hit.
	// §11 wants four Rail runs to sit without overlap on a 4 m wall; an Outlet at 7 degrees to its
	// host is the kind of thing that makes an adjacent lane unusable.
	const FVector up = railFrame.GetUnitAxis( EAxis::Z );
	const FVector right = railFrame.GetUnitAxis( EAxis::Y );
	const double dotUp = FVector::DotProduct( normal, up );
	const double dotRight = FVector::DotProduct( normal, right );

	const FVector faceNormal = ( FMath::Abs( dotUp ) >= FMath::Abs( dotRight ) )
		? ( dotUp >= 0.0 ? up : -up )
		: ( dotRight >= 0.0 ? right : -right );

	const FVector location = origin + axis * along + faceNormal * mRailHalfWidth;

	// Local +X is the mounting normal, matching the terminal branch and what FitMesh stands off
	// along. The Rail's own axis carries the roll, so an Outlet on a rolled Rail stays square to it.
	SetActorLocationAndRotation( location,
		FRotationMatrix::MakeFromXZ( faceNormal, axis ).Rotator() );

	mMode = EACPROutletHostMode::Body;
	mTargetHost = rail;
	mTargetIndex = 0;
	mBodyAlong = along;
	mHasTarget = true;

	LogMount( TEXT( "ACCEPTED" ), TEXT( "body" ) );
	return true;
}

void AACPROutletHologram::CheckValidPlacement()
{
	Super::CheckValidPlacement();

	// §15: "Outlet | Coupled/capped terminal, or Junction body | Invalid". With no target of either
	// kind there is nothing valid to build, and this is what says so — unconditionally, because a
	// guard on the disqualifier field being set would let an unset field switch the refusal off
	// entirely. See AACPRCapHologram's equivalent.
	if( !mHasTarget )
	{
		AddConstructDisqualifier( mInvalidTargetDisqualifier
			? mInvalidTargetDisqualifier : TSubclassOf< UFGConstructDisqualifier >( UFGCDInvalidAimLocation::StaticClass() ) );
	}

	// §10.1, for both host modes: a terminal mount couples to its host terminal, and a body mount
	// bonds to its host's network — either way across a Designer boundary is a Designer-to-world
	// connection.
	if( mHasTarget && FACPRSpace::IsForeignTarget( this, TEXT( "outlet host" ), mTargetHost.Get(),
			GetPlacementDesigner(), mLastSpaceLog ) )
	{
		AddConstructDisqualifier( UFGCDDesignerWorldCommingling::StaticClass() );
	}
}

void AACPROutletHologram::ConfigureActor( AFGBuildable* inBuildable ) const
{
	Super::ConfigureActor( inBuildable );

	AACPROutlet* outlet = Cast< AACPROutlet >( inBuildable );
	if( !outlet )
	{
		ACPR_LOG( Error, OUTLET_HOLO,
			TEXT( "ConfigureActor -> %s is not an AACPROutlet. "
			      "Build_PowerRailOutlet's parent class is wrong." ),
			inBuildable ? *inBuildable->GetName() : TEXT( "null" ) );
		return;
	}

	// ConfigureActor runs inside Construct, ahead of the buildable's BeginPlay — which
	// is where AACPROutlet reads every one of these. A frame later would be too late, and
	// ConfigureComponents is not an option because B.2 records it never runs on the blueprint path.
	outlet->SetHost( mMode, mTargetHost.Get(), mTargetIndex );

	ACPR_LOG( Display, OUTLET_HOLO,
		TEXT( "ConfigureActor -> %s | mode=%s host=%s index=%d along=%.1f "
		      "| hadTarget=%d" ),
		*outlet->GetName(),
		mMode == EACPROutletHostMode::Terminal ? TEXT( "terminal" ) : TEXT( "body" ),
		IsValid( mTargetHost ) ? *mTargetHost->GetName() : TEXT( "<none>" ),
		static_cast< int32 >( mTargetIndex ),
		mBodyAlong,
		mHasTarget ? 1 : 0 );
}

void AACPROutletHologram::LogMount( const TCHAR* verdict, const TCHAR* detail )
{
	if( !mMountLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	// The along value is in the key, bucketed, so sliding a body mount along a Rail logs a readable
	// trail instead of one line per frame — and so the frame a mode CHANGES always prints; a bucket
	// that swallowed that frame would hide the one that matters.
	const FString key = FString::Printf( TEXT( "%s|%s|%s|%d" ),
		verdict, detail,
		IsValid( mTargetHost ) ? *mTargetHost->GetName() : TEXT( "-" ),
		FMath::RoundToInt32( mBodyAlong / 50.0 ) );

	if( !mMountLog.Admit( key ) )
	{
		return;
	}

	ACPR_LOG( Display, OUTLET_HOLO,
		TEXT( "mount %s (%s) | host=%s index=%d | along=%.1f guidelines=%d "
		      "| loc=%s rot=%s" ),
		verdict, detail,
		IsValid( mTargetHost ) ? *mTargetHost->GetName() : TEXT( "<none>" ),
		static_cast< int32 >( mTargetIndex ),
		mBodyAlong,
		mSnapToGuideLines ? 1 : 0,
		*GetActorLocation().ToString(),
		*GetActorRotation().ToString() );
}
