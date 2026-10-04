// Auto-Connecting Power Rails — the Junction hologram.

#include "ACPRJunctionHologram.h"

#include "ACPRAttachment.h"
#include "ACPRCap.h"

#include "ACPRDesignerSpace.h"
#include "ACPRJunction.h"
#include "ACPROutlet.h"
#include "ACPRRail.h"
#include "ACPRTerminalComponent.h"
#include "ACPRTerminalRegistry.h"
#include "ACPRTerminalHost.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "Buildables/FGBuildableFactoryBuilding.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "FGConstructDisqualifier.h"

AACPRJunctionHologram::AACPRJunctionHologram()
{
	// No Tick override; the tick settings are left as AFGHologram's constructor made them (see
	// AACPRRailHologram's note).

	// A real default, for AACPRCapHologram's reason: a null disqualifier makes CheckValidPlacement
	// silent, the hologram goes green, and §7.3's refusal does not exist. See the header.
	//
	// UFGCDInvalidPlacement is the better-worded class and this is deliberately not it. An unproven
	// vanilla symbol can take the game down at load with GetLastError=127, so the C++ default is the
	// class the Cap and the Outlet already load, and the better wording is one editor edit away on an
	// EditDefaultsOnly field, where it costs no DLL import at all. See the header.
	mOccupiedTerminalDisqualifier = UFGCDInvalidAimLocation::StaticClass();
	mInsertionDisqualifier = UFGCDInvalidAimLocation::StaticClass();

	// Vanilla's attachment-point snap places this hologram — its PosX face onto the aimed terminal —
	// within §7.3's 1.5 m. Both are EditDefaultsOnly on AFGBuildableHologram (:541, :549).
	mCanSnapWithAttachmentPoints = true;
	mAttachmentPointSnapDistanceThreshold = mTerminalSnapRange;
}

void AACPRJunctionHologram::BeginPlay()
{
	Super::BeginPlay();

	// BUILD first, because nothing else on the line means anything if the binary is stale. The names
	// are built only when the line prints.
	if( ACPR_LOG_ACTIVE( Verbose ) )
	{
		TArray< UStaticMeshComponent* > meshes;
		GetComponents< UStaticMeshComponent >( meshes );
		FString meshNames;
		for( const UStaticMeshComponent* m : meshes )
		{
			if( m )
			{
				meshNames += FString::Printf( TEXT( "%s(%s) " ), *m->GetName(), *m->GetClass()->GetName() );
			}
		}
		ACPR_LOG( Verbose, JUNC_HOLO,
			TEXT( "BeginPlay (%s) | BUILD=%s | meshes=%d [%s]" ),
			*GetName(), ACPR_BUILD_STAMP(), meshes.Num(), *meshNames );
	}

	// Print the live values before theorising about them: every number the placement and nudge rules
	// depend on, read off this instance, including the ones we do not set.
	//
	// mGridSnapSize is read and not written, deliberately. Setting it to 50 looks like the obvious
	// lever and is the wrong one twice over: it does not govern the nudge (on the Rail it reads 100
	// while the nudge offset walks 25/50/75/100), and it governs vanilla's placement snapping, which
	// this class overrides outright. Two mechanisms arguing about the same position is the kind of
	// placement bug that is hardest to find. If the phase below is ever wrong, this line says what
	// vanilla was snapping to when it happened.
	const AACPRJunction* cdo = GetBuildClass()
		? Cast< AACPRJunction >( GetBuildClass()->GetDefaultObject() )
		: nullptr;

	ACPR_LOG( Verbose, JUNC_HOLO,
		TEXT( "placement config (%s) | halfExtent=%.1f -> gridPhase=%.1f "
		      "pitch=%.1f surfaceOffset=%.1f | mGridSnapSize=%.1f (read, not set) "
		      "mDefaultNudgeDistance=%.1f (dead) mCanNudgeHologram=%d "
		      "| ours coarse=%.1f fine=%.1f" ),
		*GetName(),
		cdo ? cdo->mHalfExtent : -1.0f,
		GetGridPhase(),
		mGridPitch, mSurfaceOffset,
		mGridSnapSize,
		mDefaultNudgeDistance,
		mCanNudgeHologram ? 1 : 0,
		mNudgeDistanceCoarse, mNudgeDistanceFine );

	if( !cdo )
	{
		ACPR_LOG( Warning, JUNC_HOLO,
			TEXT( "build class is not an AACPRJunction — the grid phase fell back to "
			      "the default half extent. Build_PowerRailJunction's parent class is wrong." ) );
	}

	BindPreviewMesh();
	FitPreviewMesh();

	// What a Junction may be aimed at, on vanilla's list for it (AddValidHitClass, FGHologram.h:612).
	// §7.1's free positioning is about architecture — foundations, walls, pillars, beams, all
	// AFGBuildableFactoryBuilding — and the two hosts §7.2/§7.3 snap to. A machine or a belt stays
	// whatever vanilla says.
	AddValidHitClass( AFGBuildableFactoryBuilding::StaticClass() );
	AddValidHitClass( AACPRRail::StaticClass() );
	AddValidHitClass( AACPRJunction::StaticClass() );

	ACPR_LOG( Verbose, JUNC_HOLO,
		TEXT( "feedback | preview=%s | clearance=%d (%s answers §7.4's crossing rule)" ),
		mPreviewMesh.IsValid() ? *mPreviewMesh->GetName() : TEXT( "<none>" ),
		HasClearance() ? 1 : 0,
		HasClearance() ? TEXT( "vanilla's clearance overlap" ) : TEXT( "the registry sweep" ) );
}

void AACPRJunctionHologram::Destroyed()
{
	// §12's highlight sits on the aimed host's materials; the hologram's end takes it down.
	FACPRTerminalHighlight::HideAll( mHighlightHost );

	Super::Destroyed();
}

bool AACPRJunctionHologram::IsChanged() const
{
	return !FMath::IsNearlyZero( mRollDegrees ) || Super::IsChanged();
}

bool AACPRJunctionHologram::HandleReset()
{
	if( !FMath::IsNearlyZero( mRollDegrees ) )
	{
		mRollDegrees = 0.0;
		ApplyRoll();
		return true;
	}
	return Super::HandleReset();
}

void AACPRJunctionHologram::BindPreviewMesh()
{
	// Bound once, at BeginPlay. At that moment the plain static mesh set is exactly one; the log reads
	// `meshes=2 [JunctionMesh(StaticMeshComponent) GuidelineMeshes(InstancedStaticMeshComponent)]`.
	//
	// Locking the hologram adds more: the nudge gizmo is built from ordinary static meshes — a
	// cylinder for each arrow shaft and a cone for each head — and a per-frame search for "every plain
	// UStaticMeshComponent" would find them too and scale each one up to a 100 uu cube: a giant pipe
	// segment and two cones inside the preview, flickering as the gizmo rebuilds itself.
	//
	// A search that runs repeatedly answers a different question each time it is asked. The set of
	// components that belong to the buildable is fixed the moment the hologram is constructed, so it
	// is captured then and never re-derived. Weak, because the hologram may drop it.
	TArray< UStaticMeshComponent* > meshes;
	GetComponents< UStaticMeshComponent >( meshes );

	for( UStaticMeshComponent* m : meshes )
	{
		// The instanced one is GuidelineMeshes, which owns its own scale. The remaining plain mesh
		// at this moment is the Junction's cube, copied from the buildable's component template.
		if( !m || m->IsA< UInstancedStaticMeshComponent >() )
		{
			continue;
		}

		mPreviewMesh = m;
		break;
	}

	// Once. This is re-callable from the per-frame path, and a warning that repeats sixty times a
	// second is a denial-of-service on the log it is trying to be found in.
	if( !mPreviewMesh.IsValid() && !mPreviewBindWarned )
	{
		mPreviewBindWarned = true;

		ACPR_LOG( Warning, JUNC_HOLO,
			TEXT( "no plain static mesh component at BeginPlay — the preview cube "
			      "will not be sized. Build_PowerRailJunction's JunctionMesh is missing or empty." ) );
	}
}

void AACPRJunctionHologram::FitPreviewMesh()
{
	// The preview is fitted by the same function as the built actor, on purpose.
	//
	// The hologram builds its mesh components by copying the buildable's templates, so whatever
	// transform those templates carry is what the preview shows. Leaving the fit to the buildable's
	// BeginPlay alone would mean the preview is one size and the thing you get is another — the
	// player aims a 0.43 m cube and a 1 m cube appears. One function, two call sites, no drift.
	UStaticMeshComponent* mesh = mPreviewMesh.Get();

	// Rebind, but only while unlocked. A captured pointer is the right shape here — the Rail matches
	// on the mesh asset instead, because AFGBeamHologram can regenerate its beam component — but a
	// capture that goes stale would leave the preview permanently unfitted with nothing in the log to
	// say so, since the warning lives in BindPreviewMesh.
	//
	// Re-binding from here self-heals within one frame, and the lock guard is what keeps it safe: the
	// nudge gizmo's cylinders and cones exist only while the hologram is locked, so a search that
	// never runs in that state cannot pick one up.
	if( !mesh && !IsHologramLocked() )
	{
		BindPreviewMesh();
		mesh = mPreviewMesh.Get();
	}

	if( !mesh )
	{
		return;
	}

	const float halfExtent = GetHalfExtent();

	const FVector fitted = AACPRJunction::FitMeshComponentToCube( mesh, halfExtent );

	// Once per hologram. The fit itself is re-applied from PostHologramPlacement as well — it is
	// absolute, so re-applying costs a vector write — but a line per frame would drown the placement
	// trail it sits next to.
	if( !mPreviewFitLogged )
	{
		mPreviewFitLogged = true;

		ACPR_LOG( Verbose, JUNC_HOLO,
			TEXT( "preview fit | component=%s mesh=%s -> fitted=%s | want=%.0f cube "
			      "(bound once; the nudge gizmo's own meshes are never touched)" ),
			*mesh->GetName(),
			mesh->GetStaticMesh() ? *mesh->GetStaticMesh()->GetName() : TEXT( "<none>" ),
			*fitted.ToString(),
			2.0f * halfExtent );
	}
}

float AACPRJunctionHologram::GetGridPhase() const
{
	// The escape hatch reports itself. Without this the clamp below turns a disabled grid into
	// Fmod( 50, 1 ) = 0 and the log prints phase=0.0 — indistinguishable from a computed phase of
	// zero, on the very lines the header calls the acceptance test.
	if( mGridPitch <= 0.0f )
	{
		return 0.0f;
	}

	const float pitch = FMath::Max( mGridPitch, 1.0f );

	// The phase is the cube, not a constant. A face centre sits mHalfExtent from the origin, so the
	// origin has to sit mHalfExtent off a cell boundary for the face to land ON one. Writing 50 here
	// would be writing the cube's size down a second time, in a second file, where it could disagree
	// with mHalfExtent silently — and mHalfExtent is EditDefaultsOnly precisely so the cube can be
	// re-measured against the eventual art.
	//
	// fmod rather than half the pitch, so a 2 m cube gives phase 0 (face centres already land on
	// cells) and a 1 m cube gives 50. Both are correct for the same reason.
	float phase = FMath::Fmod( GetHalfExtent(), pitch );
	if( phase < 0.0f )
	{
		phase += pitch;
	}
	return phase;
}

void AACPRJunctionHologram::ApplyPlacementOffsets( const FHitResult& hitResult, const TCHAR* hook )
{
	if( !hitResult.bBlockingHit )
	{
		return;
	}

	// A locked hologram is the player's, not ours. Both corrections below are absolute — they set the
	// distance from the surface and snap to a lattice — so running them while the player is nudging
	// would undo the nudge before it could be seen. Locking is the signal that the player has taken
	// over, and it is also the only state in which a nudge happens at all.
	if( IsHologramLocked() )
	{
		return;
	}

	// Vanilla snapped it; that decision wins. See the note on TrySnapToActor in the header — this
	// hook still runs after a successful snap, and without this the lattice rule would pull a snapped
	// Junction back off its attachment point every frame.
	if( mSnappedThisFrame )
	{
		return;
	}

	const FVector normal = hitResult.ImpactNormal.GetSafeNormal();
	if( normal.IsNearlyZero() )
	{
		return;
	}

	// Aiming at one of our own buildables is a coupling intent, not a mounting surface.
	//
	// Offsetting off another Rail's face would march successive Rails half a metre further out each
	// time, and the Junction's case is worse: a Junction aimed at a Rail is the player asking for the
	// thing §7.2 does, and answering it with "0.5 m off the Rail's skin, snapped to a lattice that
	// has nothing to do with the Rail's terminal" would be an invented rule dressed up as a placement
	// rule.
	//
	// So the aim is left exactly where vanilla put it; §7.2's terminal snap and §7.3's insertion are
	// what answer it. Any terminal host — Rail, Junction, Outlet — not a class list.
	if( Cast< IACPRTerminalHost >( hitResult.GetActor() ) )
	{
		return;
	}

	const FVector current = GetActorLocation();

	// Rule 1 — distance from the surface, set rather than added.
	//
	// Measured from the hit point, so whatever plane vanilla chose to snap to drops out: a wall snaps
	// 25 uu inside its own face, a foundation snaps to its surface, and both end up mSurfaceOffset
	// from the thing the player aimed at. Idempotent for free — once the distance IS mSurfaceOffset
	// the correction is zero, which is what makes it safe to run every frame from two hooks.
	const float along = FVector::DotProduct( current - hitResult.ImpactPoint, normal );
	FVector target = current + normal * ( mSurfaceOffset - along );

	// Rule 2 — half-cell phase on the axes the surface does not constrain.
	//
	// The normal's axis is already decided by rule 1 and must not be touched, so the grid is applied
	// only to the other two. Testing each world axis against the normal handles both cases without a
	// basis: on a foundation ( normal +Z ) that is X and Y; on a wall ( normal ±X or ±Y ) it is the
	// horizontal axis along the wall and world Z, which is the same pair the Rail's lane rule uses.
	//
	// Only on axis-aligned surfaces. On a landscape hit the normal has three non-trivial components,
	// every axis reads as "perpendicular enough", and all three would be snapped — including the one
	// rule 1 just set, which would quietly push the Junction into or off the hill. A tilted surface
	// has no lattice worth snapping to anyway: there is no Rail terminal grid there to meet. So the
	// grid is skipped and the surface offset stands alone, and the log says which happened.

	// Both double, so FMath::GridSnap deduces one type. FVector is double-precision in UE5 and mixing
	// a double coordinate with a float grid is a template-deduction failure, not a rounding question.
	const double phase = static_cast< double >( GetGridPhase() );
	const double pitch = static_cast< double >( FMath::Max( mGridPitch, 1.0f ) );

	// Sum of components, not largest component. For a unit vector the sum of the absolute components
	// is exactly 1 when it lies on an axis and strictly greater otherwise, so this is the test itself
	// rather than a proxy for it.
	//
	// Asking whether one component exceeds 0.99 would admit a normal up to 8° off axis — and on such
	// a surface the two "perpendicular enough" axes below still get snapped by up to 50 uu each,
	// which tilts the result back off the surface by several uu, the exact failure this guard exists
	// to prevent.
	const double axisSum =
		FMath::Abs( normal.X ) + FMath::Abs( normal.Y ) + FMath::Abs( normal.Z );

	const bool axisAligned = axisSum < 1.001;

	const bool gridWanted = axisAligned && mGridPitch > 0.0f;

	if( gridWanted )
	{
		if( FMath::Abs( normal.X ) < 0.5f )
		{
			target.X = FMath::GridSnap( target.X - phase, pitch ) + phase;
		}
		if( FMath::Abs( normal.Y ) < 0.5f )
		{
			target.Y = FMath::GridSnap( target.Y - phase, pitch ) + phase;
		}
		if( FMath::Abs( normal.Z ) < 0.5f )
		{
			target.Z = FMath::GridSnap( target.Z - phase, pitch ) + phase;
		}
	}

	mLastGridApplied = gridWanted;

	const FVector delta = target - current;
	if( delta.IsNearlyZero( 0.5f ) )
	{
		return;
	}

	SetActorLocation( target );

	LogPlacement( hitResult, hook );
}

void AACPRJunctionHologram::LogPlacement( const FHitResult& hitResult, const TCHAR* hook )
{
	if( !mPlacementLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	// Keyed on a coarse position so a held aim logs once and a moving aim logs a readable trail.
	const FString key = FString::Printf( TEXT( "%s|%s|%s" ),
		hook,
		hitResult.GetActor() ? *hitResult.GetActor()->GetName() : TEXT( "-" ),
		*GetActorLocation().GridSnap( 25.0f ).ToString() );

	if( !mPlacementLog.Admit( key ) )
	{
		return;
	}

	// cell= is the whole acceptance test in one field. The centre quantized the same way the terminals
	// are, so a Junction whose cell reads ( n*100 + phase ) on the two free axes and 5150 on Z has its
	// faces exactly where a Rail puts its terminals — and the COUPLED line in the buildable's log is
	// then the confirmation rather than the first place anyone finds out.
	const FIntVector cell = UACPRTerminalComponent::QuantizeLocation( GetActorLocation() );

	ACPR_LOG( Verbose, JUNC_HOLO,
		TEXT( "%s | actor=%s normal=%s | grid=%d phase=%.1f pitch=%.1f "
		      "surface=%.1f | loc=%s cell=%s" ),
		hook,
		hitResult.GetActor() ? *hitResult.GetActor()->GetName() : TEXT( "<none>" ),
		*hitResult.ImpactNormal.ToString(),
		mLastGridApplied ? 1 : 0,
		GetGridPhase(), mGridPitch, mSurfaceOffset,
		*GetActorLocation().ToString(),
		*cell.ToString() );
}

bool AACPRJunctionHologram::IsValidHitResult( const FHitResult& hitResult ) const
{
	// Polarity, by measurement: 1 = valid, whatever the header's @return says. The shared verdict
	// (ACPRHologramCommon.h) keeps vanilla's own list — the architecture §7.1 lets a Junction stand
	// on and the two hosts it snaps to, AddValidHitClass in BeginPlay — from being bypassed by the
	// base. Nothing downstream is bypassed: CheckValidPlacement, clearance and CheckValidFloor all
	// still run on the frames this lets through, and the whole hit is recorded here because this is
	// the one hook vanilla calls every frame with the raw hit, before the placement pass —
	// CheckValidPlacement's §7.3 distance is measured from its impact point.
	const bool superSaid = Super::IsValidHitResult( hitResult );
	mAim.Record( hitResult );
	return ACPRHologram::WidenedHitVerdict( superSaid, hitResult,
		hitResult.GetActor() && IsValidHitActor( hitResult.GetActor() ), mValidityLog, ACPR_TAG_TEXT( JUNC_HOLO ) );
}

void AACPRJunctionHologram::CheckValidFloor()
{
	// The steep-surface rule and the attached rule, ACPRHologram::FloorRuleAdmitsSuper: §7.2 (mated to
	// a terminal) and §7.3 (inserted into a Rail) define the placement by the host, and vanilla's
	// floor under a cube inside a Rail is the Rail's curved body.
	const bool attached = IsValid( mInsertHost ) || mSnappedTerminal.IsValid();
	if( ACPRHologram::FloorRuleAdmitsSuper( GetMinPlacementFloorZ(), mAim, attached, mFloorLog, ACPR_TAG_TEXT( JUNC_HOLO ) ) )
	{
		Super::CheckValidFloor();
	}
}

void AACPRJunctionHologram::CheckValidPlacement()
{
	Super::CheckValidPlacement();

	mOccupiedEnd = nullptr;

	// §10.1. The target is whichever host this frame is attached to: the Rail being inserted into, or
	// the owner of the terminal being snapped to. Refused here and nothing returns early, so a frame
	// that breaks §7.3 or §7.4 as well still says so — the disqualifiers stack.
	//
	// Insertion "across the boundary" is not possible (a Rail cannot straddle it and insertion keeps a
	// margin from both ends), so for mInsertHost this can only fire if vanilla's idea of where this
	// hologram is disagrees with where its host is — which is itself worth one line.
	{
		const AActor* target = IsValid( mInsertHost )
			? static_cast< const AActor* >( mInsertHost.Get() )
			: ( mSnappedTerminal.IsValid() ? mSnappedTerminal->GetOwner() : nullptr );

		if( FACPRSpace::IsForeignTarget( this,
				IsValid( mInsertHost ) ? TEXT( "insert host" ) : TEXT( "snap host" ),
				target, GetPlacementDesigner(), mLastSpaceLog ) )
		{
			AddConstructDisqualifier( UFGCDDesignerWorldCommingling::StaticClass() );
		}
	}

	// §7.4 first, because an insertion frame is not a standalone frame and none of the rules below
	// are about it. "It is rejected where it would overlap an attachment, leave an invalid one, sit
	// too close to another Junction or Outlet, or fail to leave two valid children."
	if( IsValid( mInsertHost ) )
	{
		if( const TCHAR* reason = FindInsertionRejection() )
		{
			AddConstructDisqualifier( mInsertionDisqualifier
				? mInsertionDisqualifier
				: TSubclassOf< UFGConstructDisqualifier >( UFGCDInvalidAimLocation::StaticClass() ) );

			LogInsertion( TEXT( "REFUSED" ), reason );
		}

		return;
	}

	// §7.4's crossing rule is about the cell, not about the mode. "Rejected when an unrelated Rail
	// body or terminal passes through the Junction cell" — and a standalone cube can be put on one as
	// easily as an inserted one: a cube placed on the foundation lattice whose cell sits exactly on a
	// Rail's axis (the aim slid off the Rail onto the foundation) looks inserted and is not — no
	// split, no Coupling, a Rail running straight through it. The host being mated to is the one
	// related actor here: its terminal lies on this cube's face by design.
	{
		const AActor* snapHost = mSnappedTerminal.IsValid() ? mSnappedTerminal->GetOwner() : nullptr;
		if( const TCHAR* reason = FindCrossingRejection( snapHost ) )
		{
			AddConstructDisqualifier( mInsertionDisqualifier
				? mInsertionDisqualifier
				: TSubclassOf< UFGConstructDisqualifier >( UFGCDInvalidAimLocation::StaticClass() ) );

			LogCrossing( snapHost ? TEXT( "snap" ) : TEXT( "standalone" ), reason );
		}
	}

	// A snap of our own settles the rest. §7.3's rule is about the case where neither mode is available,
	// and a successful terminal snap is one of the two being available. Asking this question on a
	// frame that already found an open terminal would refuse the very placement §7.2 exists for --
	// a Junction mating to one face of a host whose other faces happen to be coupled, which is
	// §6.2's "directly-coupled Junctions are supported" and is most of what a manifold is.
	if( mSnappedTerminal.IsValid() )
	{
		return;
	}

	if( !mAim.Hit.bBlockingHit )
	{
		return;
	}

	// Resolved through the attachment, because a capped face is exactly the case where the ray does
	// not reach the host. §9's Cap is its own actor standing 6 uu proud of the terminal plane, so
	// aiming at a capped Rail end hits `Build_PowerRailCap_C`; a cast on the hit actor alone would
	// find no terminal host, treat the frame as ordinary §7.1 standalone placement and put the
	// Junction on the Cap's face, 6 uu off the lattice. See FACPRAimedHost.
	const FACPRAimedHost aimed = FACPRAimedHost::Resolve( mAim.Hit.GetActor() );

	const AActor* hitActor = aimed.HostActor;
	const IACPRTerminalHost* host = hitActor ? Cast< IACPRTerminalHost >( hitActor ) : nullptr;
	if( !host )
	{
		return;
	}

	// Aimed straight at the occupant, which needs no search at all: the attachment names the
	// terminal it is sitting on, and an attachment only ever sits on an occupied one. Answering here
	// also avoids a distance test against an impact point that is on the Cap's face rather than the
	// terminal's, which is a small lie the scan below would have to live with.
	// !IsOpen() and not merely "an attachment was hit", because those two come apart:
	// FACPRCoupling::ReleaseAll reopens a host terminal the moment its Cap is dismantled, and the
	// Cap then stands there playing its effect for a second or two with collision intact. Without
	// this test the Junction would be refused from a face that is already free.
	if( aimed.OccupiedTerminal && !aimed.OccupiedTerminal->IsOpen() )
	{
		mOccupiedEnd = aimed.OccupiedTerminal;

		AddConstructDisqualifier( mOccupiedTerminalDisqualifier
			? mOccupiedTerminalDisqualifier
			: TSubclassOf< UFGConstructDisqualifier >( UFGCDInvalidAimLocation::StaticClass() ) );

		LogOccupiedEnd( hitActor, aimed.OccupiedTerminal, 0.0 );
		return;
	}

	// Nearest first, and state second. The order is the rule: §7.3 asks whether THE end being aimed
	// at is occupied, so the scan finds the nearest terminal in range and then asks about that one.
	// Scanning for "any occupied terminal in range" would answer a different question and would
	// refuse a Junction aimed at a Rail's open end simply because the far end of a 1.2 m Rail was
	// capped.
	//
	// The private mounting interface is skipped for the same reason it is skipped everywhere else
	// (§4): it is not a face, it sits on the cell of the terminal it mounts to, and it is never open
	// -- so leaving it in would make every terminal-mounted Outlet's host read as occupied at a
	// distance of zero and refuse everything near it.
	//
	// Non-const, and that is not a style choice: mOccupiedEnd is a TWeakObjectPtr< UACPRTerminalComponent >,
	// and TWeakObjectPtr's converting assignment is constrained on convertibility. A const pointer
	// loses a qualifier, the overload drops out, and the assignment has no viable operator=. See
	// FACPRTerminalPick::Host for the same constraint.
	UACPRTerminalComponent* nearest = nullptr;
	double nearestDistance = 0.0;

	const int32 count = host->GetTerminalCount();
	for( int32 i = 0; i < count; ++i )
	{
		UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( static_cast< uint8 >( i ) );
		if( !terminal || terminal->IsPrivateInterface() )
		{
			continue;
		}

		const double distance =
			FVector::Dist( terminal->GetComponentLocation(), mAim.Hit.ImpactPoint );

		if( distance > mTerminalSnapRange )
		{
			continue;
		}

		if( !nearest || distance < nearestDistance )
		{
			nearest = terminal;
			nearestDistance = distance;
		}
	}

	if( !nearest || nearest->IsOpen() )
	{
		// Nothing in range, or the nearest end is open and the snap failed for some other reason --
		// ambiguity, or the face filter. Neither is §7.3's case, and inventing a refusal for them
		// would take away §7.1's standalone placement anywhere near a Rail.
		return;
	}

	mOccupiedEnd = nearest;

	// The fallback is not belt-and-braces: it is what stops a null property from silently turning
	// the refusal off. §7.3 has no valid placement here, whatever this field has been edited to.
	AddConstructDisqualifier( mOccupiedTerminalDisqualifier
		? mOccupiedTerminalDisqualifier
		: TSubclassOf< UFGConstructDisqualifier >( UFGCDInvalidAimLocation::StaticClass() ) );

	LogOccupiedEnd( hitActor, nearest, nearestDistance );
}

/**
 * Shortest distance from a point to a segment, written out rather than called.
 *
 * FMath has one of these, and the engine headers are not in the local set, so its exact name and
 * signature cannot be verified; five lines of arithmetic cost less than a GetLastError=127 at load.
 */
static double ACPRDistToSegment( const FVector& point, const FVector& a, const FVector& b )
{
	const FVector ab = b - a;
	const double lengthSq = ab.SizeSquared();

	if( lengthSq <= UE_KINDA_SMALL_NUMBER )
	{
		return FVector::Dist( point, a );
	}

	const double t = FMath::Clamp( FVector::DotProduct( point - a, ab ) / lengthSq, 0.0, 1.0 );
	return FVector::Dist( point, a + ab * t );
}

bool AACPRJunctionHologram::TryInsertIntoRail( const FHitResult& hitResult )
{
	// A locked hologram keeps the host it had, for the same reason a locked snap keeps its
	// terminal: placement runs every frame while locked, and re-deriving the host from a drifting
	// aim would let the cube jump to a different Rail between the lock and the click.
	if( IsHologramLocked() )
	{
		return IsValid( mInsertHost ) && PlaceOnRail();
	}

	mInsertHost = nullptr;

	if( !hitResult.bBlockingHit )
	{
		return false;
	}

	// §7.3: "Snaps to ONE specifically targeted Rail body; the previewed host is authoritative and
	// nearby Rails are irrelevant." The aimed actor is the only candidate source, exactly as it is
	// for every other targeting question in this mod. Resolved through any attachment standing in
	// front of it, so aiming at a body-mounted Outlet still means its Rail.
	const FACPRAimedHost aimed = FACPRAimedHost::Resolve( hitResult.GetActor() );
	AACPRRail* host = Cast< AACPRRail >( aimed.HostActor );
	if( !host )
	{
		return false;
	}

	double low = 0.0, high = 0.0;
	if( !InsertableSpan( *host, low, high ) )
	{
		// Shorter than 3 m. Not a refusal to report — §7.3 simply gives this Rail no insertion mode,
		// and the frame falls through to §7.1 standalone placement, which is a legal thing to do
		// near a short Rail.
		return false;
	}

	double at = host->GetOffsetAlong( hitResult.ImpactPoint );

	// §7.3: "slides freely, or snaps in 1 m Rail-local increments on Guidelines". mSnapToGuideLines
	// is vanilla's own flag (FGHologram.h:698), the same one ScrollRotate reads for the fine step —
	// §12's modifier convention, where Ctrl means whichever thing the input in use calls for.
	// On the half-cell phase, the same one standalone placement uses; without it every inserted cube
	// would sit exactly half a Junction off the lattice.
	//
	// Rounding the centre to whole metres from terminal A would be wrong: terminal A is itself a
	// face — typically of the Junction the Rail was built out of — and this class's whole lattice
	// rule, stated in the header and implemented in GetGridPhase, is that the centre sits one phase
	// off a cell boundary so that the faces land on one. Rounding the centre would land faces on
	// half metres, and the children would come out 1.5 m, 2.5 m ... long rather than whole metres.
	//
	// So the phase is subtracted, rounded and added back: faces on whole Rail-local metres, children
	// whole-metre long, and an inserted cube in line with every standalone one on the same grid. It
	// also lands the first snap position exactly on the insertable span's near bound (a 1 m child),
	// because that bound is one half-extent plus one minimum child — the same arithmetic.
	if( mSnapToGuideLines && mGridPitch > 0.0f )
	{
		const double phase = static_cast< double >( GetGridPhase() );
		at = FMath::RoundToDouble( ( at - phase ) / mGridPitch ) * mGridPitch + phase;
	}

	// Outside the span is a refusal, not a clamp. Clamping would slide the cube up to 1.5 m from
	// where the player aimed and call it a snap, a silent fallback. Falling through instead lets the
	// terminal snap or CheckValidPlacement's occupied-end rule answer, which is what §7.3 asks for.
	if( at < low || at > high )
	{
		return false;
	}

	mInsertHost = host;
	mInsertCentre = at;

	if( !PlaceOnRail() )
	{
		mInsertHost = nullptr;
		return false;
	}

	LogInsertion( TEXT( "TARGETED" ), nullptr );
	return true;
}

bool AACPRJunctionHologram::InsertableSpan( const AACPRRail& host, double& out_low, double& out_high ) const
{
	const double half = static_cast< double >( GetHalfExtent() );
	const double total = static_cast< double >( host.GetLength() );
	const double minChild = FMath::Max( 1.0, static_cast< double >( host.GetSize() ) );

	// The insertable span, whose bounds are §7.3's two sentences at once. One half-extent plus one
	// minimum child from each end is 1.5 m, which is both "the shortest insertable host is therefore
	// 3 m" and the 1.5 m at which the mode switches to terminal-snapped.
	out_low = half + minChild;
	out_high = total - half - minChild;
	return out_high >= out_low;
}

bool AACPRJunctionHologram::AdoptCellInsertion()
{
	// A standalone cell centred on a Rail's axis is an insertion of that Rail. A Rail leaving a
	// Junction face runs at exactly the height §7.1 puts a standalone cube's centre, so an aim that
	// slides off the Rail onto the surface beside it lands the cube in the same cell the insertion
	// had — a cell §7.4 refuses outright (a Rail runs through it), so this converts no valid
	// standalone placement. If it is geometrically in a snapped place, it couples — the Rail hologram
	// already reaches a terminal its aim never touched (§5.1's far end). The aim stays authoritative
	// everywhere else (§7.3): a Rail merely NEAR the cell is not a candidate; the axis has to pass
	// through the centre on the registry's own 1 uu lattice (B.3), within §7.3's span, and exactly
	// one Rail may qualify (invariant 8: two is ambiguity, and ambiguity refuses).
	const UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this );
	if( !registry )
	{
		return false;
	}

	const FVector cellCentre = GetActorLocation();
	const AFGBuildableBlueprintDesigner* space = GetPlacementDesigner();

	// The same reach as the crossing sweep, for the same reason: a body's terminals may be a whole Rail away.
	TArray< UACPRTerminalComponent* > nearby;
	registry->FindNear( cellCentre, mCrossingSearchRadius, nearby );

	AACPRRail* found = nullptr;
	double foundAt = 0.0;
	TSet< const AActor* > seen;
	for( const UACPRTerminalComponent* terminal : nearby )
	{
		AACPRRail* rail = terminal ? Cast< AACPRRail >( terminal->GetOwner() ) : nullptr;
		if( !IsValid( rail ) || seen.Contains( rail ) )
		{
			continue;
		}
		seen.Add( rail );

		// §10.1: a Rail in another build space is not a host.
		if( FACPRSpace::OfActor( rail ) != space )
		{
			continue;
		}

		double low = 0.0, high = 0.0;
		if( !InsertableSpan( *rail, low, high ) )
		{
			continue;
		}

		const double at = rail->GetOffsetAlong( cellCentre );
		if( at < low || at > high )
		{
			continue;
		}

		const FVector onAxis = rail->GetActorLocation() + rail->GetActorQuat().GetAxisX() * at;
		if( FVector::DistSquared( onAxis, cellCentre )
			>= UACPRTerminalComponent::LatticeQuantum * UACPRTerminalComponent::LatticeQuantum )
		{
			continue;
		}

		if( found )
		{
			// Two Rails through one cell: §7.4 refuses the frame as a crossing, and that is the right answer.
			return false;
		}
		found = rail;
		foundAt = at;
	}

	if( !found )
	{
		return false;
	}

	mInsertHost = found;
	mInsertCentre = foundAt;
	if( !PlaceOnRail() )
	{
		mInsertHost = nullptr;
		return false;
	}

	LogInsertion( TEXT( "TARGETED" ), TEXT( "by cell" ) );
	return true;
}

bool AACPRJunctionHologram::PlaceOnRail()
{
	AACPRRail* host = mInsertHost.Get();
	if( !host )
	{
		return false;
	}

	// The host's whole frame, not just its axis — the same rule as PlaceOnTerminal: a bare direction
	// invents the other two axes and gimbal-locks on a vertical one, and a Rail can be vertical.
	// Taking the Rail's X and Z means the cube also inherits the Rail's roll, so an inserted Junction
	// sits square to the host rather than to the world.
	const FTransform frame = host->GetActorTransform();
	const FVector axis = frame.GetUnitAxis( EAxis::X );
	const FVector up = frame.GetUnitAxis( EAxis::Z );

	const FQuat base = FRotationMatrix::MakeFromXZ( axis, up ).ToQuat();

	// §7.3: "rollable about the host with the same 45/5 control". About the MATING axis, which here
	// is the host's own axis — composed against the base rather than against the current rotation,
	// so a second call in the same frame lands on the same answer instead of rolling twice.
	const FQuat rolled = FQuat( axis, FMath::DegreesToRadians( mRollDegrees ) ) * base;

	SetActorLocationAndRotation( frame.GetLocation() + axis * mInsertCentre, rolled );
	return true;
}

const TCHAR* AACPRJunctionHologram::FindInsertionRejection() const
{
	const AACPRRail* host = mInsertHost.Get();
	if( !host )
	{
		return nullptr;
	}

	const double half = static_cast< double >( GetHalfExtent() );
	const double total = static_cast< double >( host->GetLength() );
	const double minChild = FMath::Max( 1.0, static_cast< double >( host->GetSize() ) );

	// §7.4: "fail to leave two valid children". TryInsertIntoRail's span already prevents it, so this
	// is the guard that survives someone widening that span rather than a live case.
	if( mInsertCentre - half < minChild || total - mInsertCentre - half < minChild )
	{
		return TEXT( "would leave a child shorter than the minimum" );
	}

	const FVector cellCentre = GetActorLocation();

	// §7.4: "rejected where it would overlap an attachment". A Cap or Outlet standing inside the cell
	// has nowhere to go — the cell is about to become a Junction, and §7.4's promise is that
	// attachments are remapped to a CHILD, which requires them to be on one.
	TArray< AActor* > attachments;
	FACPRAttachments::Collect( const_cast< AACPRRail* >( host ), attachments );

	for( const AActor* attachment : attachments )
	{
		if( !IsValid( attachment ) )
		{
			continue;
		}

		const double at = host->GetOffsetAlong( attachment->GetActorLocation() );
		if( at > mInsertCentre - half && at < mInsertCentre + half )
		{
			return TEXT( "an attachment lies inside the cell" );
		}
	}

	const UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this );
	if( !registry )
	{
		return nullptr;
	}

	// §7.4: "sit too close to another Junction or Outlet" — a PROXIMITY rule (a clear metre between
	// cells), not an overlap, so no clearance box reports it. A neighbour whose centre is within
	// half + mNeighbourClearance of ours has its terminals within one more half extent of that, so
	// this radius cannot miss one and reaches nothing else.
	TArray< UACPRTerminalComponent* > nearby;
	registry->FindNear( cellCentre, 2.0 * half + mNeighbourClearance, nearby );

	for( const UACPRTerminalComponent* terminal : nearby )
	{
		AActor* owner = terminal ? terminal->GetOwner() : nullptr;
		if( !IsValid( owner ) || owner == host )
		{
			continue;
		}
		if( ( owner->IsA< AACPRJunction >() || owner->IsA< AACPROutlet >() ) &&
			FVector::Dist( owner->GetActorLocation(), cellCentre ) < half + mNeighbourClearance )
		{
			return TEXT( "another Junction or Outlet is too close" );
		}
	}

	// The crossing half of §7.4 — an unrelated body or terminal in the cell — is the same question on
	// every placement frame and is asked by CheckValidPlacement for all three modes; the host being
	// split is the related actor here.
	return FindCrossingRejection( host );
}

const TCHAR* AACPRJunctionHologram::FindCrossingRejection( const AActor* related ) const
{
	// With clearance data, vanilla answers "what overlaps this cell". The hologram's clearance box is
	// tested against everything in reach every frame already (AFGHologram::CheckClearance,
	// FGHologram.h:543), and GetConstructDisqualifierFromClearanceOverlap (:552) is the per-actor hook that
	// turns an overlap with an unrelated host into §7.4's hard refusal — see the override below. The
	// sweep that follows is the fallback for a hologram without clearance data (HasClearance, :370),
	// where no overlap is ever reported; which path this build runs is in the Verbose line at BeginPlay.
	if( HasClearance() )
	{
		return nullptr;
	}

	const UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this );
	if( !registry )
	{
		return nullptr;
	}

	const double half = static_cast< double >( GetHalfExtent() );
	const FVector cellCentre = GetActorLocation();

	// The radius has to reach a whole Rail, because §7.4's crossing rule is about a body: a 40 m Rail
	// passing through this cell may have both of its terminals far away, and the terminals are what
	// the registry holds. One of them is always within one Rail length of any point the body reaches.
	// Cost: the registry's coarse buckets within the radius, or every bucket when there are fewer
	// (TACPRSpatialGrid::ForEachNear) — bounded by the index, not the world, once per placement frame.
	TArray< UACPRTerminalComponent* > nearby;
	registry->FindNear( cellCentre, mCrossingSearchRadius, nearby );

	TSet< AActor* > owners;
	for( const UACPRTerminalComponent* terminal : nearby )
	{
		if( terminal && terminal->GetOwner() )
		{
			owners.Add( terminal->GetOwner() );
		}
	}

	for( AActor* owner : owners )
	{
		if( !IsValid( owner ) || owner == related )
		{
			continue;
		}

		// §7.4: "rejected when an unrelated Rail body or terminal passes through the Junction cell —
		// an explicit rule, not a clearance consequence, since soft clearance would permit the
		// overlap and a Junction visually swallowing a Rail it is not coupled to would imply a
		// coupling that does not exist."
		//
		// A sphere round the cell rather than the rotated box, deliberately: the test errs toward
		// refusing, and §7.4 wants refusal. "That Rail is never split or absorbed."
		if( const AACPRRail* other = Cast< AACPRRail >( owner ) )
		{
			const FVector a = other->GetActorLocation();
			const FVector b = a + other->GetActorQuat().GetAxisX() * other->GetLength();

			if( ACPRDistToSegment( cellCentre, a, b ) < half + mRailHalfWidth )
			{
				return TEXT( "an unrelated Rail crosses the cell" );
			}
		}
	}

	// And a bare terminal in the cell, which the owner sweep above does not cover: a Junction whose
	// CENTRE is far enough away can still have a face inside this cell.
	for( const UACPRTerminalComponent* terminal : nearby )
	{
		if( !terminal || terminal->GetOwner() == related || terminal->IsPrivateInterface() )
		{
			continue;
		}

		if( FVector::Dist( terminal->GetComponentLocation(), cellCentre ) <= half )
		{
			return TEXT( "an unrelated terminal lies inside the cell" );
		}
	}

	return nullptr;
}

void AACPRJunctionHologram::LogCrossing( const TCHAR* mode, const TCHAR* reason ) const
{
	if( !mCrossingLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	// One line per (mode, reason, cell): a cube swept along a Rail prints a trail, a cube held still prints once.
	const FString key = FString::Printf( TEXT( "%s|%s|%s" ), mode, reason,
		*UACPRTerminalComponent::QuantizeLocation( GetActorLocation() ).ToString() );
	if( !mCrossingLog.Admit( key ) )
	{
		return;
	}

	ACPR_LOG( Display, JUNC_HOLO,
		TEXT( "REFUSED (crossing - 7.4) | mode=%s | %s | cell=%s | disqualifier=%s" ),
		mode, reason, *UACPRTerminalComponent::QuantizeLocation( GetActorLocation() ).ToString(),
		*GetNameSafe( mInsertionDisqualifier.Get() ) );
}

TSubclassOf< UFGConstructDisqualifier > AACPRJunctionHologram::GetConstructDisqualifierFromClearanceOverlap(
	const EClearanceOverlapResult& overlapResult, AActor* otherActor ) const
{
	// §7.4: "rejected when an unrelated Rail body or terminal passes through the Junction cell — an
	// explicit rule, not a clearance consequence, since soft clearance would permit the overlap and a
	// Junction visually swallowing a Rail it is not coupled to would imply a coupling that does not
	// exist." This virtual is exactly where vanilla lets a hologram upgrade a soft overlap to a hard
	// disqualifier per actor. The Rail being split (insert mode) and the host being mated to (snap
	// mode) overlap the cell by design and keep vanilla's verdict.
	if( overlapResult != EClearanceOverlapResult::COR_None && IsValid( otherActor ) &&
		( Cast< IACPRTerminalHost >( otherActor ) || otherActor->IsA< AACPRCap >() ) )
	{
		const AActor* insertHost = mInsertHost.Get();
		const AActor* snapHost = mSnappedTerminal.IsValid() ? mSnappedTerminal->GetOwner() : nullptr;
		if( otherActor != insertHost && otherActor != snapHost )
		{
			return mInsertionDisqualifier
				? mInsertionDisqualifier
				: TSubclassOf< UFGConstructDisqualifier >( UFGCDInvalidAimLocation::StaticClass() );
		}
	}

	return Super::GetConstructDisqualifierFromClearanceOverlap( overlapResult, otherActor );
}

void AACPRJunctionHologram::LogInsertion( const TCHAR* verdict, const TCHAR* detail ) const
{
	if( !mInsertLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	const AACPRRail* host = mInsertHost.Get();

	// The offset is in the key in 50 uu buckets, so a slide prints a trail rather than a line per
	// frame, and the verdict is in it so the frame a rejection starts or stops always prints.
	const FString key = FString::Printf( TEXT( "%s|%s|%s|%d" ),
		verdict,
		detail ? detail : TEXT( "-" ),
		host ? *host->GetName() : TEXT( "-" ),
		FMath::RoundToInt32( mInsertCentre / 50.0 ) );

	if( !mInsertLog.Admit( key ) )
	{
		return;
	}

	ACPR_LOG( Display, JUNC_HOLO,
		TEXT( "insert %s%s%s | host=%s length=%.1f | centre=%.1f cell=[%.1f,%.1f] "
		      "| guidelines=%d roll=%.1f | loc=%s" ),
		verdict,
		detail ? TEXT( " - " ) : TEXT( "" ),
		detail ? detail : TEXT( "" ),
		host ? *host->GetName() : TEXT( "<none>" ),
		host ? host->GetLength() : -1.0f,
		mInsertCentre,
		mInsertCentre - GetHalfExtent(), mInsertCentre + GetHalfExtent(),
		mSnapToGuideLines ? 1 : 0, mRollDegrees,
		*GetActorLocation().ToString() );
}

AActor* AACPRJunctionHologram::Construct( TArray< AActor* >& out_children,
                                          FNetConstructionID constructionID )
{
	// Captured before anything runs, because Split destroys the host and Super may clear our state.
	AACPRRail* host = mInsertHost;
	const double centre = mInsertCentre;
	const double half = static_cast< double >( GetHalfExtent() );

	// The NAME, not the pointer, because Split destroys the host and the log lines below run after.
	const FString hostName = host ? host->GetName() : FString( TEXT( "<none>" ) );

	// §7.3 inside a Designer: the host's Designer, captured for the same reason as its name.
	AFGBuildableBlueprintDesigner* hostDesigner = host ? host->GetHostDesigner() : nullptr;
	AFGBlueprintProxy* hostProxy = host ? host->GetBlueprintProxy() : nullptr;

	// The risky half first. Split is the operation that can refuse (it returns empty and
	// leaves the host untouched when it cannot spawn both children); the Junction's own construction is
	// vanilla's and does not. So the Rail is split before the Junction exists, and if the split refused,
	// no Junction is built inside an intact Rail — the placement is reported at Error and vanilla builds
	// the cube exactly as it validated it, where the player can see what happened and dismantle it.
	// The children need no Junction to place themselves: Split places them from geometry, and §6.1's
	// coincident-pair rule joins all four terminals when the Junction's own BeginPlay resolves.
	TArray< AACPRRail* > children;
	if( host )
	{
		children = AACPRRail::Split( host, centre, half );
		if( children.Num() < 2 )
		{
			ACPR_LOG( Error, JUNC_HOLO,
				TEXT( "insert REFUSED by Split | host=%s centre=%.1f half=%.1f — the Rail is untouched; "
				      "the Junction is built where it was validated (inside the Rail) and should be dismantled" ),
				*hostName, centre, half );
		}
	}

	// The Junction itself, at the transform PlaceOnRail already put this hologram at. Everything
	// vanilla does for an ordinary single-buildable construction happens here, unchanged.
	AActor* junction = Super::Construct( out_children, constructionID );

	if( !host || !junction )
	{
		return junction;
	}

	// §7.5: "Both children and the Junction inherit the original's Blueprint Dismantle group."
	if( hostProxy )
	{
		if( AFGBuildable* buildable = Cast< AFGBuildable >( junction ) )
		{
			buildable->SetBlueprintProxy( hostProxy );
		}
	}

	for( AACPRRail* child : children )
	{
		if( IsValid( child ) )
		{
			out_children.Add( child );
		}
	}

	// The Junction's own Designer membership comes from vanilla's construction path in Super::Construct,
	// which is what this reports — AACPRRail::Split logs the children's. Read-only: the Junction is not
	// ours to register, and if vanilla gets it wrong that is a finding rather than a thing to paper over.
	if( hostDesigner )
	{
		const AFGBuildable* junctionBuildable = Cast< AFGBuildable >( junction );
		const IACPRTerminalHost* junctionHost = Cast< IACPRTerminalHost >( junction );

		ACPR_LOG( Display, SPACE,
			TEXT( "insert inside %s | junction=%s space=%s listed=%d insideFlag=%d | "
			      "hologram space=%s" ),
			*hostDesigner->GetName(), *junction->GetName(),
			*FACPRSpace::Describe( junctionHost ? junctionHost->GetHostDesigner() : nullptr ),
			hostDesigner->GetBuildablesInBlueprintDesigner().Contains(
				const_cast< AFGBuildable* >( junctionBuildable ) ) ? 1 : 0,
			( junctionBuildable && junctionBuildable->IsBuildableInsideBlueprintDesigner() ) ? 1 : 0,
			*FACPRSpace::Describe( GetPlacementDesigner() ) );
	}

	// No coupling code here, and that is the point. Split placed the children so their inner
	// terminals land exactly on this Junction's two consumed faces, with opposing quantized axes;
	// §6.1's coincident-pair rule joins all four when the Junction's BeginPlay resolves, exactly as
	// it does for a hand-placed cube.
	ACPR_LOG( Display, JUNC_HOLO,
		TEXT( "insert CONSTRUCTED | junction=%s | host=%s -> %d children | "
		      "centre=%.1f half=%.1f | proxy=%d" ),
		*junction->GetName(),
		*hostName,
		children.Num(), centre, half, hostProxy ? 1 : 0 );

	return junction;
}

void AACPRJunctionHologram::LogOccupiedEnd( const AActor* hitActor,
                                            const UACPRTerminalComponent* terminal,
                                            double distance ) const
{
	if( !mOccupiedLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}

	const FString key = FString::Printf( TEXT( "%s|%s" ),
		hitActor ? *hitActor->GetName() : TEXT( "-" ),
		terminal ? *terminal->GetName() : TEXT( "-" ) );

	if( !mOccupiedLog.Admit( key ) )
	{
		return;
	}

	// state= is the message. §7.3 says the disqualifier must name the occupied terminal rather than
	// reporting proximity to the end, and vanilla's disqualifier text cannot carry a terminal name --
	// so the name lives here, where it can be read, alongside §12's highlight.
	ACPR_LOG( Display, JUNC_HOLO,
		TEXT( "REFUSED (occupied end - 7.3) | host=%s terminal=%s state=%s "
		      "| dist=%.1f range=%.1f | disqualifier=%s" ),
		hitActor ? *hitActor->GetName() : TEXT( "<none>" ),
		terminal ? *terminal->GetName() : TEXT( "-" ),
		terminal ? ( ( terminal->GetTerminalState() == EACPRTerminalState::Capped )
			? TEXT( "Capped" ) : TEXT( "Coupled" ) ) : TEXT( "-" ),
		distance, mTerminalSnapRange,
		mOccupiedTerminalDisqualifier ? *mOccupiedTerminalDisqualifier->GetName()
		                              : TEXT( "<fallback>" ) );
}

float AACPRJunctionHologram::GetHalfExtent() const
{
	const AACPRJunction* cdo = GetBuildClass()
		? Cast< AACPRJunction >( GetBuildClass()->GetDefaultObject() )
		: nullptr;

	return cdo ? cdo->mHalfExtent : 50.0f;
}

void AACPRJunctionHologram::LogSnapAdopted( const UACPRTerminalComponent* terminal )
{
	if( !mSnapLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
	{
		return;
	}
	const FString key = terminal ? terminal->GetName() + GetNameSafe( terminal->GetOwner() ) : FString();
	if( !mSnapLog.Admit( key ) )
	{
		return;
	}
	ACPR_LOG( Verbose, JUNC_HOLO,
		TEXT( "snapped by vanilla's attachment pipeline -> %s on %s | actor=%s rot=%s" ),
		*GetNameSafe( terminal ), terminal ? *GetNameSafe( terminal->GetOwner() ) : TEXT( "-" ),
		*GetActorLocation().ToString(), *GetActorRotation().ToString() );
}

bool AACPRJunctionHologram::PlaceOnTerminal( UACPRTerminalComponent* target )
{
	if( !target )
	{
		return false;
	}

	// The whole target frame, not just its direction.
	//
	// Building the rotation from ( -outward ).Rotation() would take a bare direction and invent the
	// remaining two axes. For a horizontal mating axis that invention happens to agree with the
	// world, so side faces look right. For a vertical one it is a gimbal lock: pitch reaches ±90,
	// yaw collapses into roll, and the cube lands at world yaw 0 no matter how the target is
	// oriented — a Junction stacked on another Junction's top face misaligned by exactly the lower
	// one's yaw.
	//
	// A terminal is not a direction, it is a frame. Taking its X and Z axes and building the rotation
	// from both inherits the target's orientation exactly and has no degenerate case: the two are
	// orthogonal by construction, being axes of one transform.
	const FTransform targetFrame = target->GetComponentTransform();
	const FVector outward = targetFrame.GetUnitAxis( EAxis::X );
	const FVector targetUp = targetFrame.GetUnitAxis( EAxis::Z );

	// No zero check on outward: GetUnitAxis transforms a unit basis vector by a quaternion, so it is
	// unit by construction, and a guard here would be a safety net over solid ground.

	const float halfExtent = GetHalfExtent();

	// §7.2: "The cube extends outward from the terminal plane, consuming no Rail length." The centre
	// therefore sits one half extent along the target's own outward axis, which puts the mating face
	// exactly back on the terminal's snap point.
	const FVector snapPoint = target->GetComponentLocation();
	const FVector centre = snapPoint + outward * halfExtent;

	// Terminal 0 is local +X (ACPRJunction.h's face table), so aiming local +X back down the target's
	// outward axis makes face 0 the mating face. Its outward is then exactly -outward, which is what
	// §6.1's opposing test wants, and its cell lands on snapPoint by construction:
	//
	//     face0 = centre + (-outward) * halfExtent = snapPoint + outward*h - outward*h = snapPoint
	//
	// Exact, not approximate — which matters, because B.3 forbids an epsilon in the coupling test and
	// the two cells have to quantize to the same FIntVector.
	const FQuat facing = FRotationMatrix::MakeFromXZ( -outward, targetUp ).ToQuat();

	// §7.2's roll, as a rotation about the mating axis in world space rather than a number stuffed
	// into FRotator::Roll. Euler roll is only "about the forward axis" when pitch is away from ±90;
	// at a vertical mating axis it silently becomes yaw, which is the same gimbal that broke the
	// facing above. A quaternion about an explicit axis has no such case.
	//
	// Note for reading this in game: on a cube whose faces are axis-aligned, a 90° roll maps the four
	// side faces onto each other and is therefore invisible and functionally identity. That is
	// geometry, not a bug — it is why rotation appears to do nothing on a foundation or at a Rail
	// terminal. Only a non-multiple of 90 moves anything, which is what §7.2's 45° step is for.
	const FQuat roll( -outward, FMath::DegreesToRadians( mRollDegrees ) );

	SetActorLocationAndRotation( centre, ( roll * facing ).Rotator() );

	mSnappedTerminal = target;
	return true;
}

bool AACPRJunctionHologram::CanNudgeHologram() const
{
	// §7.2: the snap point is authoritative. A nudge from here would move the mating face off the
	// cell it was placed on and silently cost the Coupling — the one failure this mode must not have.
	if( mSnappedTerminal.IsValid() )
	{
		return false;
	}

	// §7.3: "The hologram slides freely, or snaps in 1 m Rail-local increments on Guidelines. No
	// nudge." Same argument as §7.2's: the cell the children are cut around is decided by the host
	// and the offset, and a nudge would move the cube off both.
	if( IsValid( mInsertHost ) )
	{
		return false;
	}

	return Super::CanNudgeHologram();
}

void AACPRJunctionHologram::ScrollRotate( int32 delta, int32 step )
{
	// We own the rotation. Vanilla's is wrong for this buildable in three separate ways, all measured
	// rather than assumed:
	//
	//   The step. The scroll accumulator advances 10° per notch — roll= walks 0, -10, -20 ... -90 in
	//   the log — not the 90 that GetRotationStep reports. Those are different numbers from different
	//   places: GetRotationStep does not drive this, so no threshold on it can read the mode.
	//
	//   The axis. Standalone on a wall, vanilla rotates about world Z, so the cube's top and bottom
	//   faces stay put and the four that lie in the wall plane sweep out of it — the one axis of the
	//   three that is no use on a wall. A cube wants to rotate about the surface it is sitting on.
	//
	//   The granularity. 10° on a cube is neither a lattice orientation nor a useful one: it puts all
	//   six faces off every axis at once. The useful set is multiples of 45.
	//
	// Super is deliberately not called. Its accumulator would keep counting in 10° underneath ours
	// and reappear the moment anything else consulted it.
	const double previous = mRollDegrees;

	// The fine modifier, taken from vanilla's own state rather than decoded from a value.
	//
	// It cannot be recovered from a number: GetRotationStep does not drive this, and ScrollRotate's
	// own `step` argument reads 10 whether the modifier is held or not. A modifier is state, and this
	// one is handed to the hologram directly rather than encoded in anything.
	//
	//   FGHologram.h:279   virtual void SetSnapToGuideLines( bool isEnabled );
	//   FGHologram.h:698   bool mSnapToGuideLines;                          (protected)
	//
	// AFGBuildGunBuild owns it as mSnapToGuideLinesMode, toggles it from Input_SnapToGuideLines and
	// pushes it down. One protected bool, no build-gun chain, no AccessTransformers entry.
	//
	// It is the same flag as §7.1's "Snap to Guidelines", and that is correct rather than a
	// collision: §12's modifier convention says Ctrl means whichever thing the input in use calls
	// for — guidelines while aiming, the fine step while scrolling. The two inputs never overlap.
	const bool fine = mSnapToGuideLines;
	const float stepDegrees = fine ? mRollStepFineDegrees : mRollStepDegrees;

	mRollDegrees = FMath::UnwindDegrees( mRollDegrees + delta * stepDegrees );

	ApplyRoll();

	if( mRollLogBudget > 0 && ACPR_LOG_ACTIVE( Verbose ) )
	{
		--mRollLogBudget;

		ACPR_LOG( Verbose, JUNC_HOLO,
			TEXT( "ScrollRotate | delta=%d vanillaStep=%d | fine=%d ourStep=%.1f "
			      "| roll %.1f -> %.1f | snapped=%d | rot=%s" ),
			delta, step,
			fine ? 1 : 0, stepDegrees,
			previous, mRollDegrees,
			mSnappedTerminal.IsValid() ? 1 : 0,
			*GetActorRotation().ToString() );
	}
}

void AACPRJunctionHologram::ApplyRoll()
{
	// Inserted: §7.3's other four faces are "rollable about the host with the same 45/5 control".
	// PlaceOnRail reads mRollDegrees and is absolute, so this is idempotent however many hooks call it.
	if( IsValid( mInsertHost ) )
	{
		if( PlaceOnRail() )
		{
			RecordAppliedRotation();
		}
		return;
	}

	// Snapped: the whole transform is ours, so recompute it. PlaceOnTerminal reads mRollDegrees and
	// is absolute, so this is idempotent however many hooks call it.
	if( UACPRTerminalComponent* target = mSnappedTerminal.Get() )
	{
		// Guarded on the return, so the record below never holds a rotation that was not actually
		// written.
		if( PlaceOnTerminal( target ) )
		{
			RecordAppliedRotation();
		}
		return;
	}

	// Standalone: vanilla owns the base orientation — which surface, which facing — and we add the
	// roll on top, about the surface normal. On a foundation that is world Z, which is what vanilla
	// would have done anyway; on a wall it is the wall normal, which is the axis that actually moves
	// a cube's faces around the wall plane instead of out of it.
	//
	// Composed against the captured base rather than against the current rotation, so a second call
	// in the same frame lands on the same answer instead of rolling twice.
	if( mBaseNormal.IsNearlyZero() )
	{
		return;
	}

	SetActorRotation( FQuat( mBaseNormal, FMath::DegreesToRadians( mRollDegrees ) ) * mBaseRotation );

	RecordAppliedRotation();
}

void AACPRJunctionHologram::RecordAppliedRotation()
{
	mLastAppliedRotation = GetActorQuat();
}

bool AACPRJunctionHologram::TrySnapToActor( const FHitResult& hitResult )
{
	// The terminal first. A terminal is a more specific target than anything else vanilla would snap
	// this cube to, and §7.2 is the mode the player asked for by aiming at one. Returning true here
	// is the documented way to tell the build gun the placement is settled for this frame
	// (FGHologram.h:175), which is also what stops ApplyPlacementOffsets from dragging the mated
	// cube back onto the standalone lattice.
	// §7.2's terminal snap is vanilla's. The six faces are attachment points, PosX the hologram's
	// local one; Super filters the target's points (our CanAttach: open, not private;
	// FilterAttachmentPoints: same build space), selects within §7.3's 1.5 m, and mates PosX onto the
	// face with its own transform — identical to PlaceOnTerminal's, on Rail ends and Junction faces
	// alike. The target becomes mSnappedTerminal, which the nudge refusal, the roll and the highlight
	// read. A locked cube keeps the terminal it had.
	if( IsHologramLocked() && mSnappedTerminal.IsValid() )
	{
		mSnappedThisFrame = PlaceOnTerminal( mSnappedTerminal.Get() );
		mInsertHost = nullptr;
		return mSnappedThisFrame;
	}

	mSnappedThisFrame = Super::TrySnapToActor( hitResult );
	if( mSnappedThisFrame && mSnappedAttachmentPoint )
	{
		if( UACPRTerminalComponent* terminal = FACPRAttachment::TerminalFor( *mSnappedAttachmentPoint ) )
		{
			mSnappedTerminal = terminal;
			mInsertHost = nullptr;
			LogSnapAdopted( terminal );
			return true;
		}
	}

	// §7.3 second, and the order is §7.3's own. "Aiming within 1.5 m of a Rail end switches the
	// hologram to terminal-snapped mode" — so the terminal question is asked first (vanilla's threshold
	// is that 1.5 m) and insertion only sees a body the player cannot have meant as an end.
	if( !mSnappedThisFrame && TryInsertIntoRail( hitResult ) )
	{
		mSnappedThisFrame = true;
		return true;
	}

	// The base has to be captured here too.
	//
	// Returning true from here also suppresses SetHologramLocationAndRotation, which is the only
	// other place the base is written. On such a frame mSnappedTerminal may be null — vanilla
	// snapped, not us — so PostHologramPlacement's ApplyRoll would take the standalone branch and
	// compose the roll against a base captured on an earlier frame against a different surface,
	// overwriting the placement vanilla had just made. ApplyPlacementOffsets stands down on these
	// frames; ApplyRoll should keep working, which means it needs a base that belongs to this frame.
	if( mSnappedThisFrame )
	{
		mBaseRotation = GetActorQuat();
		mBaseNormal = hitResult.ImpactNormal.GetSafeNormal();
	}

	// Logged once per hologram, because it changes the meaning of every placement line below it. A
	// function-local static would be process-wide: one snap logged ever, and then silence for the
	// rest of the session including after a reload, which is the opposite of what a per-placement
	// note is for.
	if( mSnappedThisFrame && !mSnapReported )
	{
		mSnapReported = true;
		ACPR_LOG( Verbose, JUNC_HOLO,
			TEXT( "TrySnapToActor succeeded on %s — vanilla owns this placement, "
			      "the grid rule stands down." ),
			hitResult.GetActor() ? *hitResult.GetActor()->GetName() : TEXT( "<none>" ) );
	}

	return mSnappedThisFrame;
}

void AACPRJunctionHologram::SetHologramLocationAndRotation( const FHitResult& hitResult )
{
	Super::SetHologramLocationAndRotation( hitResult );

	// Captured before our roll goes on, so the roll can be applied absolutely rather than
	// accumulated. Both hooks below re-apply it, and re-applying a relative rotation would spin the
	// cube a little further every frame.
	mBaseRotation = GetActorQuat();
	mBaseNormal = hitResult.ImpactNormal.GetSafeNormal();

	ApplyPlacementOffsets( hitResult, TEXT( "place(SetLoc)" ) );
	ApplyRoll();
}

void AACPRJunctionHologram::PreHologramPlacement( const FHitResult& hitResult, bool callForChildren )
{
	// Top of the frame's placement pass, before TrySnapToActor writes either. See the header.
	mSnappedThisFrame = false;

	// The snap survives locking. Pre runs BEFORE TrySnapToActor, so clearing it unconditionally would
	// wipe the record a locked frame is about to rely on — and CanNudgeHologram would then re-enable
	// the nudge on a mated cube. An unlocked frame re-chooses it in TrySnapToActor.
	if( !IsHologramLocked() )
	{
		mSnappedTerminal = nullptr;
		mInsertHost = nullptr;
	}

	Super::PreHologramPlacement( hitResult, callForChildren );
}

void AACPRJunctionHologram::PostHologramPlacement( const FHitResult& hitResult, bool callForChildren )
{
	Super::PostHologramPlacement( hitResult, callForChildren );

	// Vanilla's attachment snap is the terminal snap; TrySnapToActor adopted its target the moment it
	// happened, and this keeps the two in step if vanilla's state changed between.
	if( mSnappedAttachmentPoint && !mSnappedTerminal.IsValid() )
	{
		if( UACPRTerminalComponent* terminal = FACPRAttachment::TerminalFor( *mSnappedAttachmentPoint ) )
		{
			mSnappedTerminal = terminal;
			mSnappedThisFrame = true;
		}
	}
	ApplyPlacementOffsets( hitResult, TEXT( "place(Post)" ) );

	// A standalone frame whose cell sits on a Rail's axis becomes that Rail's insertion. After the
	// lattice has placed the cube (the cell is the input) and only on a frame nothing else claimed;
	// a locked cube keeps whatever it had (TryInsertIntoRail's locked branch).
	if( !mSnappedThisFrame && !IsValid( mInsertHost ) && !IsHologramLocked() && AdoptCellInsertion() )
	{
		mSnappedThisFrame = true;
	}

	// The roll is re-applied here, last. TrySnapToActor sets the rotation and vanilla can set it
	// back afterwards: FGHologram.h:172's "no further location and rotation will be updated this
	// frame" covers SetHologramLocationAndRotation only — PostHologramPlacement still runs, and so
	// does whatever it calls. Writing last is the whole remedy, and ApplyRoll is absolute so writing
	// twice costs one quaternion multiply.
	ApplyRoll();

	// Re-asserted rather than trusted to BeginPlay. A plain AFGBuildableHologram has no per-frame
	// scale write, so once should be enough — but the Rail's preview profile needs re-applying from
	// three separate hooks to stay put, and the cost of being wrong here is a player aiming a cube
	// that is not the size of the cube they get.
	FitPreviewMesh();

	FACPRSpace::WatchPlacementSpace( this, GetPlacementDesigner(), mLastPlacementSpace, mPlacementSpaceSeen );

	UpdateTerminalHighlight( hitResult );
}

void AACPRJunctionHologram::FilterAttachmentPoints( TArray< const FFGAttachmentPoint* >& Points, AFGBuildable* pBuildable, const FHitResult& HitResult ) const
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

AFGBuildableBlueprintDesigner* AACPRJunctionHologram::GetPlacementDesigner() const
{
	return mBlueprintDesigner.Get();
}

void AACPRJunctionHologram::UpdateTerminalHighlight( const FHitResult& hitResult )
{
	// The host is the AIMED actor, not mSnappedTerminal's owner: §12's cue has to appear while the
	// player is still sweeping towards a face, not only once one has been accepted. Show() clears
	// everything when the aimed actor is not a terminal host, so a frame on a foundation needs no
	// test here — and a frame with no hit at all passes a null actor, which does the same.
	FACPRSpaceFilter spaceFilter;
	spaceFilter.Enforce = true;
	spaceFilter.Space = GetPlacementDesigner();

	FACPRTerminalHighlight::Show( mHighlightHost, hitResult.GetActor(), mSnappedTerminal.Get(), spaceFilter );
}

float AACPRJunctionHologram::GetNudgeDistance() const
{
	// The mode is the modifier flag itself, `mSnapToGuideLines` — the field ScrollRotate reads. Zero
	// or less on either of ours means "leave vanilla alone" for that mode.
	const bool fine = mSnapToGuideLines;
	const float configured = fine ? mNudgeDistanceFine : mNudgeDistanceCoarse;
	const float ours = ( configured > 0.0f ) ? configured : Super::GetNudgeDistance();

	if( !FMath::IsNearlyEqual( ours, mLastLoggedNudgeDistance ) && ACPR_LOG_ACTIVE( Verbose ) )
	{
		mLastLoggedNudgeDistance = ours;

		ACPR_LOG( Verbose, JUNC_HOLO,
			TEXT( "GetNudgeDistance -> %.1f | mode=%s (coarse=%.1f fine=%.1f)" ),
			ours, fine ? TEXT( "fine" ) : TEXT( "coarse" ),
			mNudgeDistanceCoarse, mNudgeDistanceFine );
	}

	return ours;
}
