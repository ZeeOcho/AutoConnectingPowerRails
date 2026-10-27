// Auto-Connecting Power Rails — the Power Rail Wireless Outlet.

#include "ACPRWirelessOutlet.h"

#include "ACPRPowerConnectionComponent.h"
#include "ACPRSocketTable.h"
#include "ACPRTerminalHost.h"
#include "ACPRWirelessLinkSubsystem.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableCornerWall.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Buildables/FGBuildableWall.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FGPowerConnectionComponent.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AACPRWirelessOutlet::AACPRWirelessOutlet()
{
	// §16: "no Power Line can be attached to it (mMaxNumConnectionLinks = 0)". A zero budget is
	// what ConfigureAsWireSocket turns into a HIDDEN connection, and hidden is what
	// AddHiddenConnection requires of one side of every link (FGCircuitConnectionComponent.h:91).
	// The base constructor configured the socket with its own four; this is the override, in the
	// constructor so the CDO — which the hologram and the scripts read — already says so.
	mMaxPowerLines = 0;
	if( UACPRPowerConnectionComponent* socket = GetPowerConnection() )
	{
		socket->ConfigureAsWireSocket( 0 );
	}

	// The socket table asset is only ever named by its path (FACPRSocketTable), and nothing the
	// cooker follows would reach it; this hard reference on the class defaults is what makes it
	// part of the package. Absent in a checkout that has not run acpr_sockets.py yet: a null here,
	// and the table loader says so at world start.
	static ConstructorHelpers::FObjectFinder< UACPRSocketTableAsset > socketTable(
		TEXT( "/AutoConnectingPowerRails/Data/DA_ACPR_SocketTable.DA_ACPR_SocketTable" ) );
	mSocketTable = socketTable.Object;

	// §16.2's list: what sections a factory. The foundation family (foundations, ramps, roofs —
	// a roof is an AFGBuildableRamp with mIsRoof, FGBuildableRamp.h:35) and the wall family (walls,
	// doors, windows, gates; corner walls are their own branch, FGBuildableCornerWall.h). Pillars,
	// beams, walkways, stairs, machines, Rails and cables are not partitions and do not block.
	mBlockingClasses.Add( AFGBuildableFoundation::StaticClass() );
	mBlockingClasses.Add( AFGBuildableWall::StaticClass() );
	mBlockingClasses.Add( AFGBuildableCornerWall::StaticClass() );

	// §16.5's lines. A plain instanced mesh: no swatch, no cue, no collision, no shadow — and
	// Movable, because the instances are rewritten whenever a link changes.
	mLinkLines = CreateDefaultSubobject< UInstancedStaticMeshComponent >( TEXT( "LinkLines" ) );
	mLinkEnds = CreateDefaultSubobject< UInstancedStaticMeshComponent >( TEXT( "LinkEnds" ) );
	mLinkEndsInner = CreateDefaultSubobject< UInstancedStaticMeshComponent >( TEXT( "LinkEndsInner" ) );
	for( UInstancedStaticMeshComponent* instances : { mLinkLines.Get(), mLinkEnds.Get(), mLinkEndsInner.Get() } )
	{
		if( !instances )
		{
			continue;
		}
		instances->SetMobility( EComponentMobility::Movable );
		instances->SetCollisionEnabled( ECollisionEnabled::NoCollision );
		instances->SetCastShadow( false );
		instances->SetCanEverAffectNavigation( false );
		if( USceneComponent* root = GetRootComponent() )
		{
			instances->SetupAttachment( root );
		}
	}
}

void AACPRWirelessOutlet::BeginPlay()
{
	// The Outlet's whole BeginPlay first: mount or bond, register, resolve, swatch. This outlet
	// is on its Rail network before it links anything.
	Super::BeginPlay();

	{
		UInstancedStaticMeshComponent* components[] = { mLinkLines.Get(), mLinkEnds.Get(), mLinkEndsInner.Get() };
		TSoftObjectPtr< UStaticMesh >* meshes[] = { &mLinkLineMesh, &mLinkEndMesh, &mLinkEndMesh };
		for( int32 i = 0; i < 3; ++i )
		{
			UInstancedStaticMeshComponent* instances = components[ i ];
			if( !instances )
			{
				continue;
			}
			if( USceneComponent* root = GetRootComponent() )
			{
				if( !instances->IsAttachedTo( root ) )
				{
					instances->AttachToComponent( root, FAttachmentTransformRules::KeepRelativeTransform );
				}
			}
			if( !instances->GetStaticMesh() )
			{
				if( UStaticMesh* mesh = meshes[ i ]->LoadSynchronous() )
				{
					instances->SetStaticMesh( mesh );
				}
				else
				{
					ACPR_LOG( Warning, WIRELESS,
						TEXT( "%s has no mesh for %s (%s) — links will work and not be drawn there; run acpr_meshes.py." ),
						*GetName(), *instances->GetName(), *meshes[ i ]->ToString() );
				}
			}
		}
	}

	// §16.2's one-space rule is the subsystem's: an outlet inside a Blueprint Designer registers
	// like any other and links the machines of that Designer only; the hidden edges it makes there
	// are saved on both ends, so the blueprint carries them and a placed copy adopts them below.
	if( HasAuthority() )
	{
		if( UACPRWirelessLinkSubsystem* links = UACPRWirelessLinkSubsystem::Get( this ) )
		{
			links->RegisterOutlet( this );
			mRegistered = true;

			// Invariant 6: a loaded outlet's links ARE its saved hidden edges. A built or
			// blueprint-placed one additionally asks §16.4's construction question; a placed
			// blueprint may carry edges to its own placed machines, and those are adopted first.
			AdoptSavedLinks();
			if( !WasLoadedFromSave() || WasPlacedFromBlueprint() )
			{
				links->LinkAround( this, WasPlacedFromBlueprint() ? TEXT( "placed" ) : TEXT( "built" ) );
			}
		}
		else
		{
			ACPR_LOG( Warning, WIRELESS, TEXT( "%s found no link subsystem — it will link nothing." ), *GetName() );
		}
	}

	CaptureLinkEnds();
	RefreshLineMaterial();
	LogWireless( WasLoadedFromSave() ? ( WasPlacedFromBlueprint() ? TEXT( "BeginPlay(placed)" ) : TEXT( "BeginPlay(loaded)" ) )
	                                 : TEXT( "BeginPlay(built)" ) );
}

void AACPRWirelessOutlet::StartIsLookedAtForDismantle_Implementation( AFGCharacterPlayer* byCharacter )
{
	Super::StartIsLookedAtForDismantle_Implementation( byCharacter );
	// §16.5: aimed at in dismantle mode, the outlet shows what it reaches and what it holds.
	mAimedForDismantle = true;
	SetRangeShownBy( this, true );
	RefreshLineMaterial();
}

void AACPRWirelessOutlet::StopIsLookedAtForDismantle_Implementation( AFGCharacterPlayer* byCharacter )
{
	Super::StopIsLookedAtForDismantle_Implementation( byCharacter );
	mAimedForDismantle = false;
	SetRangeShownBy( this, false );
	RefreshLineMaterial();
}

void AACPRWirelessOutlet::RefreshIndicators()
{
	Super::RefreshIndicators();
	RefreshLineMaterial();
}

void AACPRWirelessOutlet::RefreshLineMaterial()
{
	if( !mLinkLines || !mLinkEnds || !mLinkEndsInner )
	{
		return;
	}
	// Static instances swapped, never a dynamic one: every outlet's lines in one state share one
	// material and batch together, and the power colour is the material's, not a parameter.
	const bool powered = HasNetworkPower() || mAimedForDismantle;
	if( mLinesMaterialSet && powered == mLinesPowered )
	{
		return;
	}
	UMaterialInterface* line = powered ? mLinkMaterialPowered.LoadSynchronous() : mLinkMaterial.LoadSynchronous();
	UMaterialInterface* end = powered ? mLinkEndMaterialPowered.LoadSynchronous() : mLinkEndMaterial.LoadSynchronous();
	UMaterialInterface* inner = powered ? mLinkEndInnerMaterialPowered.LoadSynchronous() : mLinkEndInnerMaterial.LoadSynchronous();
	if( !line || !end || !inner )
	{
		ACPR_LOG( Warning, WIRELESS, TEXT( "%s: no %s link materials (line=%d end=%d inner=%d) — run acpr_materials.py." ),
			*GetName(), powered ? TEXT( "powered" ) : TEXT( "unpowered" ), line ? 1 : 0, end ? 1 : 0, inner ? 1 : 0 );
		return;
	}
	mLinkLines->SetMaterial( 0, line );
	mLinkEnds->SetMaterial( 0, end );
	mLinkEndsInner->SetMaterial( 0, inner );
	mLinesPowered = powered;
	mLinesMaterialSet = true;
	// Which tubes a link end shows depends on the state (RebuildLines), so the instances follow.
	RebuildLines();
	ACPR_LOG( Verbose, WIRELESS, TEXT( "%s lines now %s (%s, %s) | instances=%d" ),
		*GetName(), powered ? TEXT( "powered" ) : TEXT( "unpowered" ), *line->GetName(), *end->GetName(),
		mLinkLines->GetInstanceCount() );
}

void AACPRWirelessOutlet::SetRangeShownBy( const UObject* who, bool shown )
{
	if( !who )
	{
		return;
	}
	if( shown )
	{
		mRangeShownBy.Add( who );
	}
	else
	{
		mRangeShownBy.Remove( who );
	}
	// Anyone still asking? Stale askers (a hologram that died without saying so) count as gone.
	bool wanted = false;
	for( auto it = mRangeShownBy.CreateIterator(); it; ++it )
	{
		if( it->IsValid() )
		{
			wanted = true;
		}
		else
		{
			it.RemoveCurrent();
		}
	}
	if( wanted && !mRangeSphere )
	{
		const AACPRWirelessOutlet* cdo = GetClass()->GetDefaultObject< AACPRWirelessOutlet >();
		mRangeSphere = MakeRangeSphere( this, GetRootComponent(), cdo ? cdo : this,
		                                FVector( mLinkOriginHeight, 0.0, 0.0 ),
		                                UACPRWirelessLinkSubsystem::EffectiveRange( this ) );
	}
	if( mRangeSphere )
	{
		mRangeSphere->SetVisibility( wanted, true );
	}
}

UStaticMeshComponent* AACPRWirelessOutlet::MakeRangeSphere( UObject* outer, USceneComponent* root, const AACPRWirelessOutlet* cdo,
                                                            const FVector& relativeLocation, double range )
{
	if( !outer || !root || !cdo )
	{
		return nullptr;
	}
	UStaticMesh* mesh = cdo->mRangeSphereMesh.LoadSynchronous();
	if( !mesh )
	{
		ACPR_LOG( Warning, WIRELESS,
			TEXT( "%s: no range sphere mesh at %s — run acpr_meshes.py. Everything works without it." ),
			*outer->GetName(), *cdo->mRangeSphereMesh.ToString() );
		return nullptr;
	}
	UStaticMeshComponent* sphere = NewObject< UStaticMeshComponent >( outer );
	if( !sphere )
	{
		return nullptr;
	}
	// Made the way the blueprint manager makes its link icons (FACPRBlueprintTerminalManager::EnsureIcon):
	// attached, registered, no collision, no shadow. Not a hologram mesh and not a buildable mesh: it
	// keeps its own translucent material, is never swatched, and nothing copies it anywhere.
	sphere->SetupAttachment( root );
	sphere->RegisterComponent();
	sphere->SetStaticMesh( mesh );
	sphere->SetCollisionEnabled( ECollisionEnabled::NoCollision );
	sphere->SetCastShadow( false );
	sphere->SetRenderCustomDepth( false );
	const double scale = range / FMath::Max( 1.0f, cdo->mRangeSphereAuthoredRadius );
	sphere->SetRelativeLocation( relativeLocation );
	sphere->SetRelativeScale3D( FVector( scale ) );
	ACPR_LOG( Verbose, WIRELESS,
		TEXT( "%s range sphere | range=%.0f (authored radius %.0f -> scale %.2f) | mesh=%s" ),
		*outer->GetName(), range, cdo->mRangeSphereAuthoredRadius, scale, *mesh->GetName() );
	return sphere;
}

void AACPRWirelessOutlet::GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );
	DOREPLIFETIME( AACPRWirelessOutlet, mLinkTargets );
}

void AACPRWirelessOutlet::Dismantle_Implementation()
{
	// §16.4, "outlet dismantled": out of the index FIRST, so the machines it releases find the
	// next nearest outlet rather than this one; then every edge is undone, each release asking the
	// subsystem on the machine's behalf. Idempotent, because vanilla calls Dismantle once per
	// selected actor sharing a blueprint proxy (see AACPROutlet::Dismantle_Implementation).
	if( mRegistered )
	{
		if( UACPRWirelessLinkSubsystem* links = UACPRWirelessLinkSubsystem::Get( this ) )
		{
			links->UnregisterOutlet( this );
		}
		mRegistered = false;
		UnlinkAll( TEXT( "dismantle" ) );
	}
	Super::Dismantle_Implementation();
}

void AACPRWirelessOutlet::EndPlay( const EEndPlayReason::Type endPlayReason )
{
	// The same two steps for an end that did not come through Dismantle (a world tearing down, a
	// mod reload). On a world teardown the machines are going too, so the releases are cheap.
	if( mRegistered )
	{
		if( UACPRWirelessLinkSubsystem* links = UACPRWirelessLinkSubsystem::Get( this ) )
		{
			links->UnregisterOutlet( this );
		}
		mRegistered = false;
		if( endPlayReason == EEndPlayReason::Destroyed || endPlayReason == EEndPlayReason::RemovedFromWorld )
		{
			UnlinkAll( TEXT( "endplay" ) );
		}
	}
	Super::EndPlay( endPlayReason );
}

FVector AACPRWirelessOutlet::GetLinkOrigin() const
{
	// Local +X is the mounting normal in both host modes (AACPROutlet::FitOutletMesh), so this is
	// the top of the body, whichever face the outlet sits on.
	return GetActorLocation() + GetActorForwardVector() * mLinkOriginHeight;
}

bool AACPRWirelessOutlet::IsLinkedTo( const UFGPowerConnectionComponent* connection ) const
{
	for( const TWeakObjectPtr< UFGPowerConnectionComponent >& link : mLinks )
	{
		if( link.Get() == connection )
		{
			return true;
		}
	}
	return false;
}

bool AACPRWirelessOutlet::Link( UFGPowerConnectionComponent* connection, const TCHAR* why )
{
	UACPRPowerConnectionComponent* socket = GetPowerConnection();
	if( !HasAuthority() || !socket || !IsValid( connection ) || connection == socket )
	{
		return false;
	}

	// The same safety every Coupling takes (FACPRCoupling::VerifyPowerCircuitSafe): a connection
	// that declares a power circuit but is not a power connection would crash the subsystem tick.
	if( !FACPRCoupling::VerifyPowerCircuitSafe( this, socket, TEXT( "wireless socket" ) ) ||
		!FACPRCoupling::VerifyPowerCircuitSafe( this, connection, TEXT( "wireless link" ) ) )
	{
		return false;
	}

	if( !socket->HasHiddenConnection( connection ) )
	{
		socket->AddHiddenConnection( connection );
	}
	const bool added = mLinks.AddUnique( connection ) >= 0;
	CaptureLinkEnds();

	ACPR_LOG( Verbose, WIRELESS,
		TEXT( "%s links %s.%s (%s) | links=%d | socket=%s" ),
		*GetName(), *GetNameSafe( connection->GetOwner() ), *connection->GetName(), why,
		mLinks.Num(), *socket->Describe() );
	return added;
}

bool AACPRWirelessOutlet::Unlink( UFGPowerConnectionComponent* connection, const TCHAR* why )
{
	UACPRPowerConnectionComponent* socket = GetPowerConnection();
	if( !HasAuthority() || !socket )
	{
		return false;
	}

	// The edge, if the other end still exists; the bookkeeping either way, so a machine destroyed
	// under us leaves no stale entry behind.
	if( IsValid( connection ) && socket->HasHiddenConnection( connection ) )
	{
		socket->RemoveHiddenConnection( connection );
	}
	const int32 removed = mLinks.Remove( connection );
	CaptureLinkEnds();

	if( removed > 0 )
	{
		ACPR_LOG( Verbose, WIRELESS,
			TEXT( "%s unlinks %s.%s (%s) | links=%d" ),
			*GetName(), IsValid( connection ) ? *GetNameSafe( connection->GetOwner() ) : TEXT( "<gone>" ),
			IsValid( connection ) ? *connection->GetName() : TEXT( "-" ), why, mLinks.Num() );
	}
	return removed > 0;
}

void AACPRWirelessOutlet::UnlinkAll( const TCHAR* why )
{
	// Over a copy: each release may send the machine to another outlet, and the subsystem's
	// answer must not disturb this walk.
	TArray< TWeakObjectPtr< UFGPowerConnectionComponent > > links = mLinks;
	UACPRWirelessLinkSubsystem* subsystem = UACPRWirelessLinkSubsystem::Get( this );
	for( const TWeakObjectPtr< UFGPowerConnectionComponent >& link : links )
	{
		UFGPowerConnectionComponent* connection = link.Get();
		Unlink( connection, why );
		if( subsystem && IsValid( connection ) )
		{
			subsystem->ConsiderConnection( connection, why );
		}
	}
	mLinks.Reset();
	CaptureLinkEnds();
}

void AACPRWirelessOutlet::AdoptSavedLinks()
{
	UACPRPowerConnectionComponent* socket = GetPowerConnection();
	UACPRWirelessLinkSubsystem* subsystem = UACPRWirelessLinkSubsystem::Get( this );
	if( !HasAuthority() || !socket )
	{
		return;
	}

	// Every hidden edge on the socket that ends on a machine is a link; the edge to a host (a
	// body bond, §8.1) is not, and is left exactly as the Outlet manages it. A machine edge is
	// adopted AND handed to the subsystem, which watches its wires from now on.
	TArray< UFGCircuitConnectionComponent* > hidden;
	socket->GetHiddenConnections( hidden );
	int32 adopted = 0;
	for( UFGCircuitConnectionComponent* other : hidden )
	{
		UFGPowerConnectionComponent* connection = Cast< UFGPowerConnectionComponent >( other );
		if( !connection || !UACPRWirelessLinkSubsystem::IsCandidateConnection( connection ) )
		{
			continue;
		}
		if( mLinks.AddUnique( connection ) >= 0 )
		{
			++adopted;
		}
		if( subsystem )
		{
			subsystem->ConsiderConnection( connection, TEXT( "adopted" ) );
		}
	}
	if( adopted > 0 )
	{
		ACPR_LOG( Verbose, WIRELESS, TEXT( "%s adopts %d saved link(s) | hidden edges=%d" ),
			*GetName(), adopted, hidden.Num() );
	}
}

void AACPRWirelessOutlet::CaptureLinkEnds()
{
	if( !HasAuthority() )
	{
		return;
	}
	mLinkTargets.Reset();
	for( int32 i = mLinks.Num() - 1; i >= 0; --i )
	{
		UFGPowerConnectionComponent* connection = mLinks[ i ].Get();
		if( !IsValid( connection ) )
		{
			mLinks.RemoveAt( i );
			continue;
		}
		// The socket fit is in the connection component's frame; resolve it to world space here, once,
		// so clients draw without the table.
		const AActor* machine = connection->GetOwner();
		bool known = false;
		const FACPRLinkTarget target = ResolveLinkTarget( connection, &known );
		mLinkTargets.Add( target );
		ACPR_LOG( Verbose, WIRELESS, TEXT( "%s link end for %s / %s: %s fit r=%.1f core=%.1f %.1f..%.1f axis %s" ),
			*GetName(), *GetNameSafe( machine ), *connection->GetName(), known ? TEXT( "table" ) : TEXT( "default" ),
			target.Radius, target.Core, target.Bottom, target.Top, *target.Axis.ToString() );
		// The table was measured off the blueprint's meshes; what the game renders may be another
		// mesh in another pose. Each mesh component of the machine, as it stands now, and how far
		// its bounds are from the connection point — the check of that assumption.
		if( machine && ACPR_LOG_ACTIVE( Verbose ) )
		{
			TArray< UMeshComponent* > meshes;
			machine->GetComponents( meshes );
			for( const UMeshComponent* mesh : meshes )
			{
				if( !mesh )
				{
					continue;
				}
				const FBoxSphereBounds bounds = mesh->Bounds;
				const FVector local = target.Point - mesh->GetComponentLocation();
				ACPR_LOG( Verbose, WIRELESS, TEXT( "    mesh %s (%s) %s | at %s | bounds origin %s extent %s | connection %.0f from bounds, %s from the component" ),
					*mesh->GetName(), *mesh->GetClass()->GetName(),
					mesh->IsVisible() && !mesh->bHiddenInGame ? TEXT( "visible" ) : TEXT( "hidden" ),
					*mesh->GetComponentLocation().ToString(), *bounds.Origin.ToString(), *bounds.BoxExtent.ToString(),
					bounds.GetBox().ComputeSquaredDistanceToPoint( target.Point ) > 0 ? FMath::Sqrt( bounds.GetBox().ComputeSquaredDistanceToPoint( target.Point ) ) : 0.0,
					*mesh->GetComponentTransform().InverseTransformPosition( target.Point ).ToString() );
			}
		}
	}
	RebuildLines();
}

FACPRLinkTarget AACPRWirelessOutlet::ResolveLinkTarget( const UFGPowerConnectionComponent* connection, bool* known )
{
	FACPRLinkTarget target;
	if( !IsValid( connection ) )
	{
		if( known ) { *known = false; }
		return target;
	}
	const AActor* machine = connection->GetOwner();
	const FACPRSocketFit& fit = FACPRSocketTable::Get().Find( machine ? machine->GetClass() : nullptr, connection->GetFName(), known );
	const FQuat frame = connection->GetComponentQuat();
	target.Point = connection->GetComponentLocation();
	target.Centre = target.Point + frame.RotateVector( fit.Offset );
	target.Axis = frame.RotateVector( fit.Axis ).GetSafeNormal( 0.01, FVector::UpVector );
	target.Radius = fit.Radius;
	target.Core = fit.Core;
	target.Bottom = fit.Bottom;
	target.Top = fit.Top;
	return target;
}

bool AACPRWirelessOutlet::OuterEndTransform( const FACPRLinkTarget& target, FTransform& out_transform )
{
	const double height = static_cast< double >( target.Top ) - static_cast< double >( target.Bottom );
	const double radius = static_cast< double >( target.Radius );
	if( height < 1.0 || radius <= 0.0 )
	{
		return false;
	}
	const FVector axis = target.Axis.GetSafeNormal( 0.01, FVector::UpVector );
	out_transform = FTransform( FRotationMatrix::MakeFromX( axis ).ToQuat(), target.Centre + axis * target.Bottom,
	                            FVector( height / 100.0, radius, radius ) );
	return true;
}

void AACPRWirelessOutlet::OnRep_LinkTargets()
{
	RebuildLines();
}

void AACPRWirelessOutlet::RebuildLines()
{
	if( !mLinkLines || !mLinkEnds || !mLinkEndsInner )
	{
		return;
	}
	mLinkLines->ClearInstances();
	mLinkEnds->ClearInstances();
	mLinkEndsInner->ClearInstances();
	const bool lines = mLinkLines->GetStaticMesh() != nullptr;
	const bool ends = mLinkEnds->GetStaticMesh() != nullptr;
	const bool inners = mLinkEndsInner->GetStaticMesh() != nullptr;

	// Both meshes are authored 100 uu long along +X from their origin, unit-sized across
	// (acpr_meshes.py, make_link / make_link_end), and the material reads the length back from the
	// X scale (M_ACPR_Link), so the transform is all an instance carries. The line: from the link
	// origin to the connection point, the cross-section mLinkLineWidth. The end: a tube round the
	// socket's discs, from Bottom to Top along the knob's axis through Centre, Radius across — and,
	// powered, a second tube round the knob's core between the discs, Core across, on the material
	// half a period behind, so the band climbs the outer tube and then the inner. Unpowered, the
	// standing rings are on the outer tube alone: it is outside the whole socket, so it shows on
	// every machine, where the core is buried on a grooved plug.
	const FVector origin = GetLinkOrigin();
	const double across = FMath::Max( 0.1, static_cast< double >( mLinkLineWidth ) * 0.5 );
	for( const FACPRLinkTarget& target : mLinkTargets )
	{
		const FVector delta = target.Point - origin;
		const double length = delta.Size();
		if( lines && length >= 1.0 )
		{
			const FTransform line( FRotationMatrix::MakeFromX( delta / length ).ToQuat(), origin,
			                       FVector( length / 100.0, across, across ) );
			mLinkLines->AddInstance( line, /*bWorldSpace*/ true );
		}
		FTransform outer;
		if( ends && OuterEndTransform( target, outer ) )
		{
			mLinkEnds->AddInstance( outer, /*bWorldSpace*/ true );
			const double core = static_cast< double >( target.Core );
			if( mLinesPowered && inners && core > 0.0 && core < static_cast< double >( target.Radius ) - 1.0 )
			{
				// The same tube, Core across.
				mLinkEndsInner->AddInstance( FTransform( outer.GetRotation(), outer.GetLocation(), FVector( outer.GetScale3D().X, core, core ) ), /*bWorldSpace*/ true );
			}
		}
		ACPR_LOG( Verbose, WIRELESS, TEXT( "%s link target at %s | knob axis %s through %s r=%.1f core=%.1f %.1f..%.1f" ),
			*GetName(), *target.Point.ToString(), *target.Axis.ToString(), *target.Centre.ToString(),
			target.Radius, target.Core, target.Bottom, target.Top );
	}
	ACPR_LOG( Verbose, WIRELESS, TEXT( "%s lines rebuilt | lines=%d ends=%d inner=%d materials=%s, %s, %s" ),
		*GetName(), mLinkLines->GetInstanceCount(), mLinkEnds->GetInstanceCount(), mLinkEndsInner->GetInstanceCount(),
		*GetNameSafe( mLinkLines->GetMaterial( 0 ) ), *GetNameSafe( mLinkEnds->GetMaterial( 0 ) ), *GetNameSafe( mLinkEndsInner->GetMaterial( 0 ) ) );
}

void AACPRWirelessOutlet::LogWireless( const TCHAR* stage ) const
{
	const UACPRPowerConnectionComponent* socket = GetPowerConnection();
	FString ends;
	for( const TWeakObjectPtr< UFGPowerConnectionComponent >& link : mLinks )
	{
		const UFGPowerConnectionComponent* connection = link.Get();
		if( !ends.IsEmpty() )
		{
			ends += TEXT( ", " );
		}
		ends += connection ? FString::Printf( TEXT( "%s (%.0f uu)" ), *GetNameSafe( connection->GetOwner() ),
		                                      FVector::Dist( GetLinkOrigin(), connection->GetComponentLocation() ) )
		                   : FString( TEXT( "<gone>" ) );
	}
	ACPR_LOG( Verbose, WIRELESS,
		TEXT( "%s %s | BUILD=%s | tier=%s range=%.0f effective=%.0f blockers=%d origin=%s | links=%d [%s] | socket=%s" ),
		*GetName(), stage, ACPR_BUILD_STAMP(), *GetClass()->GetName(), mRange,
		UACPRWirelessLinkSubsystem::EffectiveRange( this ), mBlockingClasses.Num(), *GetLinkOrigin().ToString(),
		mLinks.Num(), *ends, socket ? *socket->Describe() : TEXT( "<none>" ) );
}
