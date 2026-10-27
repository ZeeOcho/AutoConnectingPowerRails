// Auto-Connecting Power Rails — the wireless link subsystem.

#include "ACPRWirelessLinkSubsystem.h"

#include "ACPRDesignerSpace.h"
#include "ACPRSocketTable.h"
#include "ACPRWirelessOutlet.h"
#include "AutoConnectingPowerRails.h"
#include "AbstractInstanceManager.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableWire.h"
#include "CollisionQueryParams.h"
#include "EngineUtils.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FGBuildableSubsystem.h"
#include "FGCircuitConnectionComponent.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGPowerConnectionComponent.h"
#include "HAL/IConsoleManager.h"
#include "InstanceData.h"
#include "UObject/UObjectGlobals.h"

// ---------------------------------------------------------------------------------------------
// §16.6's test facilities. Both re-evaluate every link in every world when changed.
// ---------------------------------------------------------------------------------------------

static TAutoConsoleVariable< float > CVarACPRWirelessRange(
	TEXT( "acpr.WirelessRange" ), 0.0f,
	TEXT( "Overrides every Wireless Outlet tier's range, in cm. 0 = each tier's own value." ),
	ECVF_Default );

static TAutoConsoleVariable< FString > CVarACPRWirelessBlockers(
	TEXT( "acpr.WirelessBlockers" ), TEXT( "" ),
	TEXT( "Overrides the blocking list: comma-separated buildable class names (FGBuildable to make everything block; FGBuildableFoundation,FGBuildableWall,FGBuildableCornerWall is the shipped list). Empty = each outlet's own list." ),
	ECVF_Default );

static void ACPRWirelessOverrideChanged( IConsoleVariable* )
{
	if( !GEngine )
	{
		return;
	}
	for( const FWorldContext& context : GEngine->GetWorldContexts() )
	{
		if( UWorld* world = context.World() )
		{
			if( UACPRWirelessLinkSubsystem* links = world->GetSubsystem< UACPRWirelessLinkSubsystem >() )
			{
				links->ReevaluateAll( TEXT( "console override changed" ) );
			}
		}
	}
}

static void ACPRBindWirelessOverrides()
{
	static bool bound = false;
	if( bound )
	{
		return;
	}
	bound = true;
	CVarACPRWirelessRange.AsVariable()->SetOnChangedCallback( FConsoleVariableDelegate::CreateStatic( &ACPRWirelessOverrideChanged ) );
	CVarACPRWirelessBlockers.AsVariable()->SetOnChangedCallback( FConsoleVariableDelegate::CreateStatic( &ACPRWirelessOverrideChanged ) );
}

// ---------------------------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------------------------

UACPRWirelessLinkSubsystem* UACPRWirelessLinkSubsystem::Get( const UObject* worldContext )
{
	const UWorld* world = worldContext ? worldContext->GetWorld() : nullptr;
	return world ? world->GetSubsystem< UACPRWirelessLinkSubsystem >() : nullptr;
}

namespace
{
	/**
	 * acpr.ReloadSockets: read the socket table again (the shipped asset and the player's override
	 * file) and refit every outlet's link ends from it. The tuning loop for an override entry
	 * without a game restart. Authority only: clients draw what the server replicates.
	 */
	void ReloadSocketsCommand( const TArray< FString >& args, UWorld* world, FOutputDevice& ar )
	{
		if( !world || !world->GetAuthGameMode() )
		{
			ar.Log( TEXT( "acpr.ReloadSockets: the server reads the table; run it there." ) );
			return;
		}
		ar.Log( *FACPRSocketTable::Reload() );
		int32 outlets = 0;
		for( TActorIterator< AACPRWirelessOutlet > it( world ); it; ++it )
		{
			if( it->HasAuthority() )
			{
				it->CaptureLinkEnds();
				++outlets;
			}
		}
		ar.Log( *FString::Printf( TEXT( "link ends refitted on %d outlet(s)" ), outlets ) );
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice ReloadSockets(
		TEXT( "acpr.ReloadSockets" ),
		TEXT( "Power Rails: read the socket table and the override file (FactoryGame/Configs/AutoConnectingPowerRails_Sockets.json) again and refit every outlet's link ends." ),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic( &ReloadSocketsCommand ) );
}

void UACPRWirelessLinkSubsystem::OnWorldBeginPlay( UWorld& world )
{
	Super::OnWorldBeginPlay( world );
	ACPRBindWirelessOverrides();
	EnsureBound();
	// Loads on first use; touching it here puts its Display line — table found, override found,
	// counts — at the start of the world's log rather than at the first link.
	FACPRSocketTable::Get();
}

void UACPRWirelessLinkSubsystem::Deinitialize()
{
	if( AFGBuildableSubsystem* buildables = mBoundTo.Get() )
	{
		buildables->mBuildableAddedDelegate.RemoveDynamic( this, &UACPRWirelessLinkSubsystem::HandleBuildableAdded );
		buildables->mBuildableRemovedDelegate.RemoveDynamic( this, &UACPRWirelessLinkSubsystem::HandleBuildableRemoved );
	}
	mBoundTo = nullptr;
	for( TPair< TWeakObjectPtr< UFGPowerConnectionComponent >, FWatch >& pair : mWatched )
	{
		if( UFGPowerConnectionComponent* connection = pair.Key.Get() )
		{
			connection->OnConnectionChanged.Remove( pair.Value.Handle );
		}
	}
	mWatched.Reset();
	mOutletAt.Reset();
	mOutlets.Reset();
	Super::Deinitialize();
}

void UACPRWirelessLinkSubsystem::EnsureBound()
{
	// The buildable subsystem is an actor the world settings spawn, so it may not exist yet at
	// OnWorldBeginPlay; every public entry tries again, and the first outlet to register always
	// finds it (vanilla's own AFGBuildable::BeginPlay fetched it a moment earlier).
	AFGBuildableSubsystem* buildables = GetWorld() ? AFGBuildableSubsystem::Get( GetWorld() ) : nullptr;
	if( !buildables || mBoundTo.Get() == buildables )
	{
		return;
	}
	if( AFGBuildableSubsystem* old = mBoundTo.Get() )
	{
		old->mBuildableAddedDelegate.RemoveDynamic( this, &UACPRWirelessLinkSubsystem::HandleBuildableAdded );
		old->mBuildableRemovedDelegate.RemoveDynamic( this, &UACPRWirelessLinkSubsystem::HandleBuildableRemoved );
	}
	buildables->mBuildableAddedDelegate.AddDynamic( this, &UACPRWirelessLinkSubsystem::HandleBuildableAdded );
	buildables->mBuildableRemovedDelegate.AddDynamic( this, &UACPRWirelessLinkSubsystem::HandleBuildableRemoved );
	mBoundTo = buildables;

	ACPR_LOG( Display, WIRELESS,
		TEXT( "link subsystem bound to %s | added and removed delegates | BUILD=%s" ),
		*buildables->GetName(), ACPR_BUILD_STAMP() );
}

// ---------------------------------------------------------------------------------------------
// The rules
// ---------------------------------------------------------------------------------------------

bool UACPRWirelessLinkSubsystem::IsMachine( const AActor* actor )
{
	// A factory buildable. Not the Personnel Elevator's floor stops, though they carry its socket
	// knobs: a floor stop's socket takes several cables and the elevator conducts between floors,
	// so it is a pole with a lift attached — infrastructure, which the outlet never links (§16.2).
	return actor && actor->IsA< AFGBuildableFactory >();
}

bool UACPRWirelessLinkSubsystem::IsCandidateConnection( const UFGPowerConnectionComponent* connection )
{
	if( !IsValid( connection ) || connection->IsHidden() )
	{
		return false;
	}
	return IsMachine( connection->GetOwner() );
}

/** The wire whose dismantle is being handled: still in its connections' lists, already gone for every rule. */
static TWeakObjectPtr< const AFGBuildableWire > GACPRDismantlingWire;

int32 UACPRWirelessLinkSubsystem::LiveWireCount( const UFGCircuitConnectionComponent* connection )
{
	// A wire being dismantled stays in the connection's list until its own EndPlay, so the count
	// that answers "unwired" leaves out the one whose dismantle is being handled right now (the hook
	// runs before vanilla sets mIsDismantled) and any already flagged.
	if( !IsValid( connection ) )
	{
		return 0;
	}
	TArray< AFGBuildableWire* > wires;
	connection->GetWires( wires );
	const AFGBuildableWire* dismantling = GACPRDismantlingWire.Get();
	int32 live = 0;
	for( const AFGBuildableWire* wire : wires )
	{
		if( IsValid( wire ) && !wire->GetIsDismantled() && wire != dismantling )
		{
			++live;
		}
	}
	return live;
}

FString UACPRWirelessLinkSubsystem::DescribeConnection( const UFGPowerConnectionComponent* connection )
{
	if( !IsValid( connection ) )
	{
		return TEXT( "<invalid>" );
	}
	TArray< AFGBuildableWire* > wires;
	connection->GetWires( wires );
	TArray< UFGCircuitConnectionComponent* > hidden;
	connection->GetHiddenConnections( hidden );
	FString edges;
	for( const UFGCircuitConnectionComponent* other : hidden )
	{
		edges += FString::Printf( TEXT( " %s.%s" ), other ? *GetNameSafe( other->GetOwner() ) : TEXT( "<null>" ),
			other ? *other->GetName() : TEXT( "-" ) );
	}
	return FString::Printf( TEXT( "%s.%s wires=%d live=%d hidden=%d [%s ] linked=%s" ),
		*GetNameSafe( connection->GetOwner() ), *connection->GetName(), wires.Num(), LiveWireCount( connection ),
		hidden.Num(), *edges, *GetNameSafe( LinkedOutletOf( connection ) ) );
}

bool UACPRWirelessLinkSubsystem::IsUnwired( const UFGPowerConnectionComponent* connection )
{
	if( !IsValid( connection ) || LiveWireCount( connection ) > 0 )
	{
		return false;
	}
	// A hidden edge to another BUILDABLE that is not ours is another feed — a mod's, say — and the
	// outlet never competes with it, exactly as it never competes with a wire. A hidden edge to
	// equipment is not a feed: the Hoverpack attaches to a machine's connection this way while the
	// player flies near it, and a machine does not become "wired" because someone is hovering by.
	TArray< UFGCircuitConnectionComponent* > hidden;
	connection->GetHiddenConnections( hidden );
	for( const UFGCircuitConnectionComponent* other : hidden )
	{
		const AFGBuildable* owner = other ? Cast< AFGBuildable >( other->GetOwner() ) : nullptr;
		if( owner && !owner->IsA< AACPRWirelessOutlet >() )
		{
			return false;
		}
	}
	return true;
}

AACPRWirelessOutlet* UACPRWirelessLinkSubsystem::LinkedOutletOf( const UFGPowerConnectionComponent* connection )
{
	if( !IsValid( connection ) )
	{
		return nullptr;
	}
	TArray< UFGCircuitConnectionComponent* > hidden;
	connection->GetHiddenConnections( hidden );
	for( const UFGCircuitConnectionComponent* other : hidden )
	{
		if( AACPRWirelessOutlet* outlet = other ? Cast< AACPRWirelessOutlet >( other->GetOwner() ) : nullptr )
		{
			return outlet;
		}
	}
	return nullptr;
}

double UACPRWirelessLinkSubsystem::EffectiveRange( const AACPRWirelessOutlet* outlet )
{
	const float override = CVarACPRWirelessRange.GetValueOnGameThread();
	if( override > 0.0f )
	{
		return override;
	}
	return outlet ? outlet->mRange : 0.0;
}

const AFGBuildableBlueprintDesigner* UACPRWirelessLinkSubsystem::SpaceOf( const UObject* buildableOrHologram )
{
	return FACPRSpace::OfObject( buildableOrHologram );
}

void UACPRWirelessLinkSubsystem::BlockingOverride( TArray< UClass* >& out_classes )
{
	// Parsed once per value: the string the console holds, split on commas, each name tried as
	// written and without a leading A (the console says AFGBuildableWall, the class is named
	// FGBuildableWall). A name that resolves to nothing is reported once and skipped.
	static FString parsedFrom;
	static TArray< TWeakObjectPtr< UClass > > parsed;
	const FString value = CVarACPRWirelessBlockers.GetValueOnGameThread().TrimStartAndEnd();
	if( value != parsedFrom )
	{
		parsedFrom = value;
		parsed.Reset();
		TArray< FString > names;
		value.ParseIntoArray( names, TEXT( "," ), true );
		for( FString name : names )
		{
			name = name.TrimStartAndEnd();
			if( name.IsEmpty() )
			{
				continue;
			}
			UClass* found = FindFirstObject< UClass >( *name, EFindFirstObjectOptions::NativeFirst );
			if( !found && name.Len() > 1 && name[ 0 ] == TEXT( 'A' ) )
			{
				found = FindFirstObject< UClass >( *name.Mid( 1 ), EFindFirstObjectOptions::NativeFirst );
			}
			if( found && found->IsChildOf( AFGBuildable::StaticClass() ) )
			{
				parsed.Add( found );
				ACPR_LOG( Display, WIRELESS, TEXT( "acpr.WirelessBlockers: %s -> %s" ), *name, *found->GetName() );
			}
			else
			{
				ACPR_LOG( Warning, WIRELESS, TEXT( "acpr.WirelessBlockers: '%s' is not a loaded AFGBuildable class — skipped" ), *name );
			}
		}
	}
	out_classes.Reset();
	for( const TWeakObjectPtr< UClass >& cls : parsed )
	{
		if( UClass* live = cls.Get() )
		{
			out_classes.Add( live );
		}
	}
}

/** §16.2's frame exception, by vanilla's own marker in the class name. */
static bool ACPRIsFramePiece( const UClass* cls )
{
	return cls && cls->GetName().Contains( TEXT( "Frame" ), ESearchCase::CaseSensitive );
}

/** What a trace hit is, for the blocking test: a buildable (actor or instance) with its class, or world. */
struct FACPRHitKind
{
	AActor* Owner = nullptr;
	UClass* BuildableClass = nullptr;
	/** A lightweight instance: no owner actor; identified by class and location. */
	bool Lightweight = false;
	FVector InstanceAt = FVector::ZeroVector;
	bool World = false;
	FString Name;
};

static FACPRHitKind ACPRClassifyHit( const FHitResult& hit )
{
	FACPRHitKind kind;
	AActor* actor = hit.GetActor();

	if( AFGBuildable* buildable = Cast< AFGBuildable >( actor ) )
	{
		kind.Owner = buildable;
		kind.BuildableClass = buildable->GetClass();
		kind.Name = buildable->GetName();
		// An actor of a lightweight class is the stand-in the lightweight system spawns for an
		// instance while the build gun aims at it, with its own collision at the instance's place:
		// the same obstacle as the instance, so it carries the instance's identity (class and
		// location) and is ignored when that instance is the one leaving. Flagged or not — the
		// stand-in is not always marked temporary by the time it is hit.
		kind.InstanceAt = buildable->GetActorLocation();
		if( buildable->GetIsLightweightTemporary() )
		{
			kind.Name += TEXT( " (temporary)" );
		}
		return kind;
	}

	// An abstract instance: a machine's instanced mesh (owner = the machine) or a lightweight
	// foundation or wall (no owner actor; the lightweight subsystem knows its class).
	if( AAbstractInstanceManager* manager = Cast< AAbstractInstanceManager >( actor ) )
	{
		FInstanceHandle handle;
		if( manager->ResolveHit( hit, handle ) && handle.IsInstanced() )
		{
			if( AFGBuildable* owner = Cast< AFGBuildable >( AAbstractInstanceManager::GetOwnerByHandle( handle ) ) )
			{
				// A machine's instanced mesh — or the lightweight system's stand-in actor, whose
				// meshes are instanced the same way: its identity is its class and place, as above.
				kind.Owner = owner;
				kind.BuildableClass = owner->GetClass();
				kind.InstanceAt = owner->GetActorLocation();
				kind.Name = owner->GetName();
				return kind;
			}
			FLightweightBuildableInstanceRef instance;
			if( AFGLightweightBuildableSubsystem::ResolveLightweightInstance( handle, instance ) && instance.GetBuildableClass() )
			{
				kind.BuildableClass = instance.GetBuildableClass();
				kind.Lightweight = true;
				kind.InstanceAt = instance.GetBuildableTransform().GetLocation();
				kind.Name = kind.BuildableClass->GetName() + TEXT( " (instance)" );
				return kind;
			}
		}
	}

	// Terrain, a cliff, anything that is not a buildable: blocks regardless of the list.
	kind.World = true;
	kind.Name = actor ? actor->GetName() : ( hit.GetComponent() ? hit.GetComponent()->GetName() : FString( TEXT( "<world>" ) ) );
	return kind;
}

bool UACPRWirelessLinkSubsystem::LineIsClear( const AACPRWirelessOutlet* outlet, const UFGPowerConnectionComponent* connection,
                                              FString* out_blocker ) const
{
	if( !IsValid( outlet ) || !IsValid( connection ) )
	{
		return false;
	}
	const AActor* const ends[] = { outlet, outlet->GetHostActor(), connection->GetOwner() };
	return LineIsClear( outlet, outlet->GetLinkOrigin(), connection->GetComponentLocation(),
	                    MakeArrayView( ends ), nullptr, out_blocker );
}

bool UACPRWirelessLinkSubsystem::LineIsClear( const AACPRWirelessOutlet* rules, const FVector& from, const FVector& to,
                                              TArrayView< const AActor* const > ignored, FVector* out_blockedAt, FString* out_blocker ) const
{
	UWorld* world = GetWorld();
	if( !world || !rules )
	{
		return false;
	}
	const FVector delta = to - from;
	const double length = delta.Size();
	if( length < 1.0 )
	{
		return true;
	}
	const FVector direction = delta / length;

	TArray< UClass* > blockers;
	BlockingOverride( blockers );
	const bool useOwn = blockers.Num() == 0;

	auto blocks = [ & ]( const UClass* cls )
	{
		// §16.2: a Power Line is never an obstacle — it is the thing a link stands in for.
		if( cls->IsChildOf( AFGBuildableWire::StaticClass() ) )
		{
			return false;
		}
		// §16.2: a frame piece is the one sectioning piece built to be seen through, and never
		// blocks. Vanilla gives it no class or flag of its own; its name carries the marker
		// (Build_Foundation_Frame_01, Build_Flat_Frame_01, Build_Ramp_Frame_01,
		// Build_Ramp_Frame_Inverted_01, Build_Wall_Frame_01), and nothing else on the blocking
		// list's classes does — windows, doors and gates carry their own words.
		if( ACPRIsFramePiece( cls ) )
		{
			return false;
		}
		if( useOwn )
		{
			for( const TSubclassOf< AFGBuildable >& entry : rules->mBlockingClasses )
			{
				if( entry && cls->IsChildOf( entry ) )
				{
					return true;
				}
			}
			return false;
		}
		for( const UClass* entry : blockers )
		{
			if( cls->IsChildOf( entry ) )
			{
				return true;
			}
		}
		return false;
	};

	// The ignored actors are the two ends and the outlet's host; an instanced hit cannot be
	// ignored by actor (the component is shared), so a hit that RESOLVES to one of them, to a
	// buildable not on the list, or to whatever is leaving the world right now, is stepped past and
	// the trace continues from just beyond it.
	FCollisionQueryParams params( FName( TEXT( "ACPRWirelessLink" ) ), /*bTraceComplex*/ false );
	for( const AActor* end : ignored )
	{
		if( end )
		{
			params.AddIgnoredActor( end );
		}
	}
	const AActor* leaving = mIgnoredDuringRemoval.Get();
	if( leaving )
	{
		params.AddIgnoredActor( leaving );
	}
	const UClass* leavingClass = mIgnoredLightweightClass.Get();

	auto isEnd = [ & ]( const AActor* owner )
	{
		if( !owner )
		{
			return false;
		}
		if( owner == leaving )
		{
			return true;
		}
		for( const AActor* end : ignored )
		{
			if( owner == end )
			{
				return true;
			}
		}
		return false;
	};

	// The instance added this very frame (see mArrivingLightweightClass): its box blocks as the
	// buildable of its class would, whether or not the trace below can see it yet.
	auto arrivingBlocks = [ & ]()
	{
		const UClass* arriving = mArrivingLightweightClass.Get();
		if( !arriving || !mArrivingLightweightBox.IsValid || !blocks( arriving )
			|| !FMath::LineBoxIntersection( mArrivingLightweightBox, from, to, delta ) )
		{
			return false;
		}
		if( out_blockedAt )
		{
			*out_blockedAt = FMath::ClosestPointOnSegment( mArrivingLightweightBox.GetCenter(), from, to );
		}
		if( out_blocker )
		{
			*out_blocker = FString::Printf( TEXT( "%s (instance, by bounds)" ), *arriving->GetName() );
		}
		return true;
	};

	FVector start = from;
	for( int32 step = 0; step < 16; ++step )
	{
		FHitResult hit;
		if( !world->LineTraceSingleByChannel( hit, start, to, ECC_Visibility, params ) || !hit.bBlockingHit )
		{
			return !arrivingBlocks();
		}
		const FACPRHitKind kind = ACPRClassifyHit( hit );
		const bool ownEnd = isEnd( kind.Owner );
		const bool leavingInstance = leavingClass && kind.BuildableClass == leavingClass
			&& kind.InstanceAt.Equals( mIgnoredLightweightAt, 1.0 );
		if( !ownEnd && !leavingInstance && ( kind.World || ( kind.BuildableClass && blocks( kind.BuildableClass ) ) ) )
		{
			if( out_blockedAt )
			{
				*out_blockedAt = hit.ImpactPoint;
			}
			if( out_blocker )
			{
				*out_blocker = FString::Printf( TEXT( "%s at %.0f uu" ), *kind.Name, FVector::Dist( from, hit.ImpactPoint ) );
			}
			return false;
		}
		// Not a blocker: the rest of the trace ignores the whole of it — the actor when the hit was
		// an actor's, the instanced component when it was an instance's (one component is one mesh
		// of one class, so what else it skips is of the same non-blocking kind). The one exception
		// is the leaving instance, which shares its component with its blocking siblings: that one
		// is stepped past by a little more than the trace's own tolerance.
		if( leavingInstance && !( hit.GetActor() == kind.Owner && kind.Owner ) )
		{
			start = hit.ImpactPoint + direction * 2.0;
		}
		else
		{
			if( hit.GetActor() == kind.Owner && kind.Owner )
			{
				params.AddIgnoredActor( kind.Owner );
			}
			else if( UPrimitiveComponent* component = hit.GetComponent() )
			{
				params.AddIgnoredComponent( component );
			}
			else
			{
				start = hit.ImpactPoint + direction * 2.0;
			}
		}
		if( FVector::DotProduct( to - start, direction ) <= 0.0 )
		{
			return !arrivingBlocks();
		}
	}
	// Sixteen non-blocking things on one line is not a factory layout; treated as clear and said so.
	ACPR_LOG( Verbose, WIRELESS, TEXT( "%s: trace budget spent on non-blocking hits; treated as clear" ),
		*GetNameSafe( rules ) );
	return !arrivingBlocks();
}

// ---------------------------------------------------------------------------------------------
// The index
// ---------------------------------------------------------------------------------------------

void UACPRWirelessLinkSubsystem::RegisterOutlet( AACPRWirelessOutlet* outlet )
{
	if( !IsValid( outlet ) )
	{
		return;
	}
	EnsureBound();
	const FVector at = outlet->GetLinkOrigin();
	if( const FVector* filed = mOutletAt.Find( outlet ) )
	{
		if( filed->Equals( at ) )
		{
			return;
		}
		mOutlets.Remove( *filed, outlet );
	}
	mOutlets.Add( at, outlet );
	mOutletAt.Add( outlet, at );
	++mWorldVersion;
}

void UACPRWirelessLinkSubsystem::UnregisterOutlet( AACPRWirelessOutlet* outlet )
{
	if( const FVector* filed = mOutletAt.Find( outlet ) )
	{
		mOutlets.Remove( *filed, outlet );
		mOutletAt.Remove( outlet );
		++mWorldVersion;
	}
}

/** Deterministic order: nearer first, then by name, so two outlets at one distance agree on every load. */
static bool ACPRPrecedes( const AACPRWirelessOutlet* a, double distA, const AACPRWirelessOutlet* b, double distB )
{
	if( !FMath::IsNearlyEqual( distA, distB, 0.5 ) )
	{
		return distA < distB;
	}
	return a->GetName() < b->GetName();
}

void UACPRWirelessLinkSubsystem::FindOutletsReaching( const FVector& point, const AFGBuildableBlueprintDesigner* space,
                                                      TArray< AACPRWirelessOutlet* >& out_outlets ) const
{
	TArray< TPair< AACPRWirelessOutlet*, double > > found;
	mOutlets.ForEachNear( point, ACPRSpec::WirelessRangeMax, [ & ]( const TArray< TWeakObjectPtr< AACPRWirelessOutlet > >& bucket )
	{
		for( const TWeakObjectPtr< AACPRWirelessOutlet >& weak : bucket )
		{
			AACPRWirelessOutlet* outlet = weak.Get();
			if( !IsValid( outlet ) || SpaceOf( outlet ) != space )
			{
				continue;
			}
			const double distance = FVector::Dist( outlet->GetLinkOrigin(), point );
			if( distance <= EffectiveRange( outlet ) )
			{
				found.Emplace( outlet, distance );
			}
		}
	} );
	found.Sort( []( const TPair< AACPRWirelessOutlet*, double >& a, const TPair< AACPRWirelessOutlet*, double >& b )
	{
		return ACPRPrecedes( a.Key, a.Value, b.Key, b.Value );
	} );
	for( const TPair< AACPRWirelessOutlet*, double >& pair : found )
	{
		out_outlets.Add( pair.Key );
	}
}

void UACPRWirelessLinkSubsystem::FindOutletsReachingBox( const FBox& bounds, TArray< AACPRWirelessOutlet* >& out_outlets ) const
{
	if( !bounds.IsValid )
	{
		return;
	}
	const FVector centre = bounds.GetCenter();
	const double reach = bounds.GetExtent().Size() + ACPRSpec::WirelessRangeMax;
	mOutlets.ForEachNear( centre, reach, [ & ]( const TArray< TWeakObjectPtr< AACPRWirelessOutlet > >& bucket )
	{
		for( const TWeakObjectPtr< AACPRWirelessOutlet >& weak : bucket )
		{
			AACPRWirelessOutlet* outlet = weak.Get();
			if( !IsValid( outlet ) )
			{
				continue;
			}
			const double range = EffectiveRange( outlet );
			if( FMath::SphereAABBIntersection( outlet->GetLinkOrigin(), range * range, bounds ) )
			{
				out_outlets.AddUnique( outlet );
			}
		}
	} );
}

void UACPRWirelessLinkSubsystem::ClassifyOutletsFor( const FVector& point, const AFGBuildableBlueprintDesigner* space,
                                                     TArrayView< const AActor* const > ignored, TArray< FACPRLinkCandidate >& out_candidates ) const
{
	TArray< AACPRWirelessOutlet* > reaching;
	FindOutletsReaching( point, space, reaching );
	for( AACPRWirelessOutlet* outlet : reaching )
	{
		FACPRLinkCandidate candidate;
		candidate.Outlet = outlet;
		candidate.Distance = FVector::Dist( outlet->GetLinkOrigin(), point );
		TArray< const AActor* > ends( ignored.GetData(), ignored.Num() );
		ends.Add( outlet );
		ends.Add( outlet->GetHostActor() );
		candidate.Clear = LineIsClear( outlet, outlet->GetLinkOrigin(), point, MakeArrayView( ends ),
		                               &candidate.BlockedAt, &candidate.Blocker );
		out_candidates.Add( MoveTemp( candidate ) );
	}
}

// ---------------------------------------------------------------------------------------------
// The machine side
// ---------------------------------------------------------------------------------------------

void UACPRWirelessLinkSubsystem::Watch( UFGPowerConnectionComponent* connection )
{
	if( !IsValid( connection ) || mWatched.Contains( connection ) )
	{
		return;
	}
	FWatch watch;
	watch.Wires = LiveWireCount( connection );
	watch.Handle = connection->OnConnectionChanged.AddUObject( this, &UACPRWirelessLinkSubsystem::HandleConnectionChanged );
	mWatched.Add( connection, watch );
}

void UACPRWirelessLinkSubsystem::Unwatch( UFGPowerConnectionComponent* connection )
{
	FWatch watch;
	if( mWatched.RemoveAndCopyValue( connection, watch ) && IsValid( connection ) )
	{
		connection->OnConnectionChanged.Remove( watch.Handle );
	}
}

void UACPRWirelessLinkSubsystem::HandleConnectionChanged( UFGCircuitConnectionComponent* changed )
{
	// OnConnectionChanged fires for wires and may fire for hidden edges — our own links included.
	// Only a change in the LIVE wire count is a §16.4 event; everything else returns here, which is
	// also what keeps a Link made inside this handler from re-entering it.
	UFGPowerConnectionComponent* connection = Cast< UFGPowerConnectionComponent >( changed );
	FWatch* watch = connection ? mWatched.Find( connection ) : nullptr;
	if( !watch )
	{
		return;
	}
	const int32 wires = LiveWireCount( connection );
	if( wires == watch->Wires )
	{
		return;
	}
	watch->Wires = wires;
	ConsiderConnection( connection, wires > 0 ? TEXT( "Power Line attached" ) : TEXT( "Power Line removed" ) );
}

AACPRWirelessOutlet* UACPRWirelessLinkSubsystem::ChooseOutletFor( const UFGPowerConnectionComponent* connection, FString* out_reason ) const
{
	const AActor* const ends[] = { connection->GetOwner() };
	TArray< FACPRLinkCandidate > candidates;
	ClassifyOutletsFor( connection->GetComponentLocation(), SpaceOf( connection->GetOwner() ), MakeArrayView( ends ), candidates );
	FString blocked;
	for( const FACPRLinkCandidate& candidate : candidates )
	{
		if( candidate.Clear )
		{
			return candidate.Outlet;
		}
		blocked += FString::Printf( TEXT( " %s blocked by %s;" ), *candidate.Outlet->GetName(), *candidate.Blocker );
	}
	if( out_reason )
	{
		*out_reason = candidates.Num() == 0 ? FString( TEXT( "no outlet in range" ) ) : blocked;
	}
	return nullptr;
}

void UACPRWirelessLinkSubsystem::ConsiderConnection( UFGPowerConnectionComponent* connection, const TCHAR* why )
{
	if( !IsCandidateConnection( connection ) )
	{
		return;
	}
	EnsureBound();
	Watch( connection );

	AACPRWirelessOutlet* current = LinkedOutletOf( connection );
	if( !IsUnwired( connection ) )
	{
		ACPR_LOG( Verbose, WIRELESS, TEXT( "%s is wired (%s): %s" ), *GetNameSafe( connection->GetOwner() ), why,
			*DescribeConnection( connection ) );
		if( current )
		{
			current->Unlink( connection, why );
			++mWorldVersion;
		}
		return;
	}

	// A link that exists is kept. The machine side never MOVES a link: the only event that can
	// make a nearer outlet exist is that outlet's own arrival, and LinkAround handles the move
	// there. This is also what keeps a load order-independent — a loaded machine whose saved
	// link is to an outlet that has not begun play yet still holds that edge, and keeps it.
	if( current )
	{
		ACPR_LOG( Verbose, WIRELESS, TEXT( "%s keeps its link (%s): %s" ), *GetNameSafe( connection->GetOwner() ), why,
			*DescribeConnection( connection ) );
		return;
	}

	FString reason;
	AACPRWirelessOutlet* chosen = ChooseOutletFor( connection, &reason );
	if( !chosen )
	{
		if( mOutletAt.Num() > 0 )
		{
			ACPR_LOG( Verbose, WIRELESS, TEXT( "%s.%s stays unlinked (%s): %s" ),
				*GetNameSafe( connection->GetOwner() ), *connection->GetName(), why, *reason );
		}
		return;
	}
	chosen->Link( connection, why );
	++mWorldVersion;
}

void UACPRWirelessLinkSubsystem::CollectConnections( AFGBuildable* factory, TArray< UFGPowerConnectionComponent* >& out_connections )
{
	if( !IsValid( factory ) )
	{
		return;
	}
	TArray< UFGPowerConnectionComponent* > components;
	factory->GetComponents( components );
	for( UFGPowerConnectionComponent* connection : components )
	{
		if( IsCandidateConnection( connection ) )
		{
			out_connections.Add( connection );
		}
	}
}

void UACPRWirelessLinkSubsystem::BuildableArrived( AFGBuildable* buildable, const TCHAR* why )
{
	if( !IsValid( buildable ) || buildable->IsA< AACPRWirelessOutlet >() )
	{
		return;
	}
	if( mOutletAt.Num() == 0 )
	{
		return;
	}

	// A machine: its connections ask for an outlet.
	if( IsMachine( buildable ) )
	{
		TArray< UFGPowerConnectionComponent* > connections;
		CollectConnections( buildable, connections );
		for( UFGPowerConnectionComponent* connection : connections )
		{
			ConsiderConnection( connection, why );
		}
	}

	// A wire has no body to lie across a line (§16.2) and a long thin box; it is done here.
	if( buildable->IsA< AFGBuildableWire >() )
	{
		return;
	}

	// Anything else: its body may now lie across a link.
	ObstacleArrived( buildable->GetComponentsBoundingBox( false ), why );
}

void UACPRWirelessLinkSubsystem::ObstacleArrived( const FBox& bounds, const TCHAR* why )
{
	// Only outlets whose range reaches the box are asked, and each re-traces only the links that
	// pass through it.
	if( !bounds.IsValid || mOutletAt.Num() == 0 )
	{
		return;
	}
	TArray< AACPRWirelessOutlet* > outlets;
	FindOutletsReachingBox( bounds, outlets );
	for( AACPRWirelessOutlet* outlet : outlets )
	{
		RetestLinks( outlet, &bounds, why );
	}
	if( outlets.Num() > 0 )
	{
		ACPR_LOG( Verbose, WIRELESS, TEXT( "%s | %d outlet(s) re-tested their lines through %s" ),
			why, outlets.Num(), *bounds.ToString() );
	}
}

void UACPRWirelessLinkSubsystem::ObstacleLeaving( const FBox& bounds, const TCHAR* why )
{
	if( !bounds.IsValid || mOutletAt.Num() == 0 )
	{
		return;
	}
	TArray< AACPRWirelessOutlet* > outlets;
	FindOutletsReachingBox( bounds, outlets );
	for( AACPRWirelessOutlet* outlet : outlets )
	{
		LinkAround( outlet, why );
	}
}

void UACPRWirelessLinkSubsystem::HandleBuildableAdded( AFGBuildable* buildable )
{
	BuildableArrived( buildable, TEXT( "added" ) );
}

void UACPRWirelessLinkSubsystem::HandleBuildableRemoved( AFGBuildable* buildable )
{
	if( !IsValid( buildable ) || buildable->IsA< AACPRWirelessOutlet >() )
	{
		return;
	}

	// A Power Line's removal is handled from its Dismantle (WireDismantling); nothing to re-trace.
	if( buildable->IsA< AFGBuildableWire >() )
	{
		return;
	}

	// A machine leaving: its links are dropped by their outlets and its wires are no longer watched.
	if( IsMachine( buildable ) )
	{
		TArray< UFGPowerConnectionComponent* > components;
		buildable->GetComponents( components );
		for( UFGPowerConnectionComponent* connection : components )
		{
			if( AACPRWirelessOutlet* outlet = LinkedOutletOf( connection ) )
			{
				outlet->Unlink( connection, TEXT( "machine removed" ) );
				++mWorldVersion;
			}
			Unwatch( connection );
		}
	}

	// Anything leaving may have been the obstacle on a line: the outlets reaching its bounds
	// re-run their query. The removed actor is still in the physics scene during this broadcast,
	// so the traces ignore it by name.
	mIgnoredDuringRemoval = buildable;
	ObstacleLeaving( buildable->GetComponentsBoundingBox( false ), TEXT( "obstacle removed" ) );
	mIgnoredDuringRemoval = nullptr;
}

// ---------------------------------------------------------------------------------------------
// The hooked events
// ---------------------------------------------------------------------------------------------

FBox UACPRWirelessLinkSubsystem::LightweightBounds( TSubclassOf< AFGBuildable > buildableClass, const FTransform& at )
{
	// The same sum AFGBuildable::BeginPlay makes for mCachedBounds: every instance mesh's bounds
	// under its relative transform under the buildable's. The actor itself has no mesh components
	// to measure — the meshes exist only as this instance data.
	FBox box( ForceInit );
	const AFGBuildable* cdo = buildableClass ? buildableClass->GetDefaultObject< AFGBuildable >() : nullptr;
	const UAbstractInstanceDataObject* data = cdo ? cdo->GetLightweightInstanceData() : nullptr;
	if( !data )
	{
		return box;
	}
	for( const FInstanceData& instance : data->GetInstanceData() )
	{
		if( instance.StaticMesh )
		{
			box += instance.StaticMesh->GetBoundingBox().TransformBy( instance.RelativeTransform * at );
		}
	}
	return box;
}

void UACPRWirelessLinkSubsystem::LightweightArrived( TSubclassOf< AFGBuildable > buildableClass, const FTransform& at )
{
	if( mOutletAt.Num() == 0 )
	{
		return;
	}
	const FBox bounds = LightweightBounds( buildableClass, at );
	ACPR_LOG( Verbose, WIRELESS, TEXT( "%s built as a lightweight instance at %s | bounds %s" ),
		*GetNameSafe( buildableClass ), *at.GetLocation().ToString(), *bounds.ToString() );
	// Its box stands in for its instance for every clear-line test made while this is handled.
	mArrivingLightweightClass = buildableClass;
	mArrivingLightweightBox = bounds;
	ObstacleArrived( bounds, TEXT( "lightweight built" ) );
	mArrivingLightweightClass = nullptr;
	mArrivingLightweightBox = FBox( ForceInit );
}

void UACPRWirelessLinkSubsystem::WireDismantling( const AFGBuildableWire* wire )
{
	if( !IsValid( wire ) || mOutletAt.Num() == 0 )
	{
		return;
	}
	// Both ends, with this wire already left out of every count (LiveWireCount), so a machine it
	// was the only cable of is unwired again and asks for an outlet now — the connection's own
	// OnConnectionChanged arrives only in the wire's EndPlay, and by then there is nothing new to say.
	GACPRDismantlingWire = wire;
	for( int32 end = 0; end < 2; ++end )
	{
		if( UFGPowerConnectionComponent* connection = Cast< UFGPowerConnectionComponent >( wire->GetConnection( end ) ) )
		{
			// A machine's own dismantle dismantles its wires first: that end is leaving too and asks nothing.
			const AFGBuildable* owner = Cast< AFGBuildable >( connection->GetOwner() );
			if( owner && owner->GetIsDismantled() )
			{
				continue;
			}
			ACPR_LOG( Verbose, WIRELESS, TEXT( "%s dismantled: %s" ), *wire->GetName(), *DescribeConnection( connection ) );
			if( FWatch* watch = mWatched.Find( connection ) )
			{
				watch->Wires = LiveWireCount( connection );
			}
			ConsiderConnection( connection, TEXT( "Power Line removed" ) );
		}
	}
	GACPRDismantlingWire = nullptr;
}

void UACPRWirelessLinkSubsystem::LightweightLeaving( TSubclassOf< AFGBuildable > buildableClass, const FTransform& at )
{
	if( mOutletAt.Num() == 0 )
	{
		return;
	}
	const FBox bounds = LightweightBounds( buildableClass, at );
	ACPR_LOG( Verbose, WIRELESS, TEXT( "%s dismantled as a lightweight instance at %s | bounds %s" ),
		*GetNameSafe( buildableClass ), *at.GetLocation().ToString(), *bounds.ToString() );
	// Its instance is still in the scene during the call; the traces step past it by identity.
	mIgnoredLightweightClass = buildableClass;
	mIgnoredLightweightAt = at.GetLocation();
	ObstacleLeaving( bounds, TEXT( "lightweight removed" ) );
	mIgnoredLightweightClass = nullptr;
}

void UACPRWirelessLinkSubsystem::DesignerBuildableSpawned( AFGBuildable* buildable )
{
	BuildableArrived( buildable, TEXT( "built in designer" ) );
}

void UACPRWirelessLinkSubsystem::DesignerBuildableDismantled( AFGBuildable* buildable )
{
	HandleBuildableRemoved( buildable );
}

// ---------------------------------------------------------------------------------------------
// The outlet side
// ---------------------------------------------------------------------------------------------

void UACPRWirelessLinkSubsystem::FindFactoriesAround( const FVector& origin, double range, const AActor* ignored,
                                                      TSet< AFGBuildable* >& out_factories ) const
{
	UWorld* world = GetWorld();
	if( !world )
	{
		return;
	}
	// One overlap of the range for everything with collision, resolved to factory buildables:
	// actors directly, instanced machines through their instance handles.
	TArray< FOverlapResult > overlaps;
	FCollisionQueryParams params( FName( TEXT( "ACPRWirelessAround" ) ), false );
	if( ignored )
	{
		params.AddIgnoredActor( ignored );
	}
	world->OverlapMultiByChannel( overlaps, origin, FQuat::Identity, ECC_Visibility,
	                              FCollisionShape::MakeSphere( static_cast< float >( range ) ), params );

	AAbstractInstanceManager* manager = AAbstractInstanceManager::GetInstanceManager( world );
	for( const FOverlapResult& overlap : overlaps )
	{
		AActor* actor = overlap.GetActor();
		if( IsMachine( actor ) )
		{
			out_factories.Add( Cast< AFGBuildable >( actor ) );
			continue;
		}
		if( manager && actor == manager )
		{
			FInstanceHandle handle;
			if( manager->ResolveOverlap( overlap, handle ) && handle.IsInstanced() )
			{
				if( AActor* owner = AAbstractInstanceManager::GetOwnerByHandle( handle ); IsMachine( owner ) )
				{
					out_factories.Add( Cast< AFGBuildable >( owner ) );
				}
			}
		}
	}
}

void UACPRWirelessLinkSubsystem::LinkAround( AACPRWirelessOutlet* outlet, const TCHAR* why )
{
	if( !GetWorld() || !IsValid( outlet ) || !outlet->HasAuthority() )
	{
		return;
	}
	EnsureBound();

	const FVector origin = outlet->GetLinkOrigin();
	const double range = EffectiveRange( outlet );
	const AFGBuildableBlueprintDesigner* space = SpaceOf( outlet );

	TSet< AFGBuildable* > factories;
	FindFactoriesAround( origin, range, outlet, factories );
	if( ACPR_LOG_ACTIVE( Verbose ) )
	{
		FString names;
		for( const AFGBuildable* factory : factories )
		{
			names += TEXT( " " ) + factory->GetName();
		}
		ACPR_LOG( Verbose, WIRELESS, TEXT( "%s overlap at %s r=%.0f found %d factory buildable(s):%s" ),
			*outlet->GetName(), *origin.ToString(), range, factories.Num(), *names );
	}

	int32 considered = 0, linked = 0, moved = 0, blocked = 0, wired = 0, farther = 0, elsewhere = 0;
	for( AFGBuildable* factory : factories )
	{
		if( SpaceOf( factory ) != space )
		{
			++elsewhere;
			continue;
		}
		TArray< UFGPowerConnectionComponent* > connections;
		CollectConnections( factory, connections );
		for( UFGPowerConnectionComponent* connection : connections )
		{
			const FVector point = connection->GetComponentLocation();
			const double distance = FVector::Dist( origin, point );
			if( distance > range )
			{
				continue;
			}
			++considered;
			Watch( connection );
			if( !IsUnwired( connection ) )
			{
				++wired;
				ACPR_LOG( Verbose, WIRELESS, TEXT( "%s -> wired: %s" ), *outlet->GetName(), *DescribeConnection( connection ) );
				continue;
			}
			AACPRWirelessOutlet* current = LinkedOutletOf( connection );
			if( current == outlet )
			{
				continue;
			}
			if( current && !ACPRPrecedes( outlet, distance, current, FVector::Dist( current->GetLinkOrigin(), point ) ) )
			{
				++farther;
				ACPR_LOG( Verbose, WIRELESS, TEXT( "%s -> %s.%s kept by %s (%.0f uu, this one %.0f uu)" ),
					*outlet->GetName(), *factory->GetName(), *connection->GetName(), *current->GetName(),
					FVector::Dist( current->GetLinkOrigin(), point ), distance );
				continue;
			}
			FString blocker;
			if( !LineIsClear( outlet, connection, &blocker ) )
			{
				++blocked;
				ACPR_LOG( Verbose, WIRELESS, TEXT( "%s -> %s.%s blocked by %s" ),
					*outlet->GetName(), *factory->GetName(), *connection->GetName(), *blocker );
				continue;
			}
			if( current )
			{
				current->Unlink( connection, TEXT( "nearer outlet" ) );
				++moved;
			}
			else
			{
				++linked;
			}
			outlet->Link( connection, why );
		}
	}
	if( linked + moved > 0 )
	{
		++mWorldVersion;
	}

	ACPR_LOG( Display, WIRELESS,
		TEXT( "%s %s | space=%s range=%.0f | factories=%d connections=%d | linked=%d moved=%d | wired=%d farther=%d blocked=%d elsewhere=%d | links=%d" ),
		*outlet->GetName(), why, *FACPRSpace::Describe( space ), range, factories.Num(), considered, linked, moved,
		wired, farther, blocked, elsewhere, outlet->GetLinkCount() );
}

void UACPRWirelessLinkSubsystem::RetestLinks( AACPRWirelessOutlet* outlet, const FBox* onlyThrough, const TCHAR* why )
{
	if( !IsValid( outlet ) || !outlet->HasAuthority() )
	{
		return;
	}
	const FVector origin = outlet->GetLinkOrigin();
	const double range = EffectiveRange( outlet );
	const FBox through = onlyThrough ? onlyThrough->ExpandBy( 10.0 ) : FBox( ForceInit );

	// Over a copy: a dropped link may send the machine to another outlet, and that outlet may be
	// this one's neighbour in the same walk.
	TArray< TWeakObjectPtr< UFGPowerConnectionComponent > > links = outlet->GetLinks();
	int32 dropped = 0;
	for( const TWeakObjectPtr< UFGPowerConnectionComponent >& weak : links )
	{
		UFGPowerConnectionComponent* connection = weak.Get();
		if( !IsValid( connection ) )
		{
			outlet->Unlink( connection, TEXT( "gone" ) );
			continue;
		}
		const FVector point = connection->GetComponentLocation();
		if( onlyThrough && !FMath::LineBoxIntersection( through, origin, point, point - origin ) )
		{
			continue;
		}
		FString blocker;
		const bool tooFar = FVector::Dist( origin, point ) > range;
		if( tooFar || !LineIsClear( outlet, connection, &blocker ) )
		{
			ACPR_LOG( Verbose, WIRELESS, TEXT( "%s -> %s.%s dropped (%s): %s" ),
				*outlet->GetName(), *GetNameSafe( connection->GetOwner() ), *connection->GetName(), why,
				tooFar ? TEXT( "out of range" ) : *blocker );
			outlet->Unlink( connection, why );
			++dropped;
			ConsiderConnection( connection, why );
		}
	}
	if( dropped > 0 )
	{
		++mWorldVersion;
	}
}

void UACPRWirelessLinkSubsystem::ReevaluateAll( const TCHAR* why )
{
	TArray< AACPRWirelessOutlet* > outlets;
	for( const TPair< TWeakObjectPtr< AACPRWirelessOutlet >, FVector >& pair : mOutletAt )
	{
		if( AACPRWirelessOutlet* outlet = pair.Key.Get() )
		{
			outlets.Add( outlet );
		}
	}
	ACPR_LOG( Display, WIRELESS, TEXT( "re-evaluating %d outlet(s): %s | acpr.WirelessRange=%.0f acpr.WirelessBlockers='%s'" ),
		outlets.Num(), why, CVarACPRWirelessRange.GetValueOnGameThread(), *CVarACPRWirelessBlockers.GetValueOnGameThread() );
	for( AACPRWirelessOutlet* outlet : outlets )
	{
		RetestLinks( outlet, nullptr, why );
	}
	for( AACPRWirelessOutlet* outlet : outlets )
	{
		LinkAround( outlet, why );
	}
	++mWorldVersion;
}

// ---------------------------------------------------------------------------------------------
// §16.5's placement preview for an outlet
// ---------------------------------------------------------------------------------------------

void UACPRWirelessLinkSubsystem::PreviewOutletAt( const AACPRWirelessOutlet* rules, const FVector& origin,
                                                  const AFGBuildableBlueprintDesigner* space, const AActor* hologram,
                                                  TArray< FACPRPreviewLine >& out_lines ) const
{
	if( !rules )
	{
		return;
	}
	const double range = EffectiveRange( rules );
	TSet< AFGBuildable* > factories;
	FindFactoriesAround( origin, range, hologram, factories );

	for( AFGBuildable* factory : factories )
	{
		if( SpaceOf( factory ) != space )
		{
			continue;
		}
		TArray< UFGPowerConnectionComponent* > connections;
		CollectConnections( factory, connections );
		for( const UFGPowerConnectionComponent* connection : connections )
		{
			const FVector point = connection->GetComponentLocation();
			const double distance = FVector::Dist( origin, point );
			if( distance > range || !IsUnwired( connection ) )
			{
				continue;
			}
			// LinkAround's own rule: a machine kept by a nearer outlet is not taken. The outlet
			// being placed has no name yet, so an exact tie goes to the one that exists.
			if( const AACPRWirelessOutlet* current = LinkedOutletOf( connection ) )
			{
				if( FVector::Dist( current->GetLinkOrigin(), point ) <= distance + 0.5 )
				{
					continue;
				}
			}
			FACPRPreviewLine line;
			line.From = origin;
			line.To = point;
			line.Connection = connection;
			const AActor* const ends[] = { hologram, factory };
			FVector blockedAt;
			line.Chosen = LineIsClear( rules, origin, point, MakeArrayView( ends ), &blockedAt, nullptr );
			if( !line.Chosen )
			{
				line.To = blockedAt;
			}
			out_lines.Add( line );
		}
	}
}
