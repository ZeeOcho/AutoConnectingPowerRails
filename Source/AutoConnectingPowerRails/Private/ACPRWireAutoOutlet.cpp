// Auto-Connecting Power Rails — §8.4: a Power Line aimed at a Rail builds a Power Rail Outlet.

#include "ACPRWireAutoOutlet.h"

#include "ACPROutlet.h"
#include "ACPROutletHologram.h"
#include "ACPRPowerConnectionComponent.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildableWire.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "FGBuildableSubsystem.h"
#include "FGCircuitConnectionComponent.h"
#include "FGPowerConnectionComponent.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "Hologram/FGHologram.h"
#include "Hologram/FGPowerPoleHologram.h"
#include "Hologram/FGPowerPoleWallHologram.h"
#include "Hologram/FGWireHologram.h"
#include "Patching/NativeHookManager.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	const TCHAR* OUTLET_RECIPE = TEXT( "/AutoConnectingPowerRails/Recipes/Recipe_PowerRailOutlet.Recipe_PowerRailOutlet_C" );
	const FName CHILD_NAME( TEXT( "ACPRRailOutlet" ) );

	FDelegateHandle gBeginPlayHook;
	FDelegateHandle gMultiStepHook;
	FDelegateHandle gSnapHook;
	FDelegateHandle gPlacedHook;
	FDelegateHandle gConstructHook;

	/** The last line logged per wire and place, so a per-frame step logs only when something changed. */
	TMap< FString, FString > gLastLines;

	/** A recipe problem, once found, is said once and ends the feature for the run — no child is spawned again. */
	bool gRecipeMissingReported = false;

	FString Describe( const AFGHologram* hologram )
	{
		if( !IsValid( hologram ) )
		{
			return TEXT( "None" );
		}
		return FString::Printf( TEXT( "%s(%s%s)" ), *hologram->GetName(),
			hologram->IsDisabled() ? TEXT( "disabled" ) : TEXT( "enabled" ), hologram->IsHidden() ? TEXT( ",hidden" ) : TEXT( "" ) );
	}

	FString DescribeHit( const FHitResult& hit )
	{
		const AActor* actor = hit.GetActor();
		return actor ? FString::Printf( TEXT( "%s [%s]" ), *actor->GetName(), *actor->GetClass()->GetName() ) : TEXT( "nothing" );
	}
}

AACPROutletHologram* FACPRWireAutoOutlet::ChildOf( const AFGWireHologram* wire )
{
	if( !IsValid( wire ) )
	{
		return nullptr;
	}
	for( AFGHologram* child : wire->GetHologramChildren() )
	{
		if( AACPROutletHologram* outlet = Cast< AACPROutletHologram >( child ) )
		{
			return outlet;
		}
	}
	return nullptr;
}

FString FACPRWireAutoOutlet::State( const AFGWireHologram* wire )
{
	const UFGCircuitConnectionComponent* second = wire->GetConnection( 1 );
	return FString::Printf( TEXT( "conn=%d pole=%s wall=%s active=%s railOutlet=%s end=%s" ),
		wire->mCurrentConnection, *Describe( wire->mPowerPole.Get() ), *Describe( wire->mPowerPoleWall.Get() ),
		*Describe( wire->mActivePoleHologram.Get() ), *Describe( ChildOf( wire ) ), second ? *GetNameSafe( second->GetOwner() ) : TEXT( "None" ) );
}

void FACPRWireAutoOutlet::LogChange( const AFGWireHologram* wire, const TCHAR* where, const FString& line )
{
	if( !ACPR_LOG_ACTIVE( Verbose ) )
	{
		return;
	}
	if( gLastLines.Num() > 256 )
	{
		gLastLines.Empty();
	}
	FString& last = gLastLines.FindOrAdd( wire->GetName() + TEXT( "|" ) + where );
	if( last != line )
	{
		last = line;
		ACPR_LOG( Verbose, OUTLET_HOLO, TEXT( "%s %s: %s" ), *wire->GetName(), where, *line );
	}
}

AACPROutletHologram* FACPRWireAutoOutlet::EnsureChild( AFGWireHologram* wire )
{
	if( AACPROutletHologram* child = ChildOf( wire ) )
	{
		return child;
	}
	// Vanilla's Wall Outlet child is its sign that the wire is in automatic-pole mode; without it
	// there is nothing to take the place of.
	if( !wire->mPowerPoleWall || gRecipeMissingReported )
	{
		return nullptr;
	}
	UWorld* world = wire->GetWorld();
	TSubclassOf< UFGRecipe > recipe = LoadClass< UFGRecipe >( nullptr, OUTLET_RECIPE );
	if( !world || !recipe )
	{
		// Asked again on every aimed frame; said once.
		if( !gRecipeMissingReported )
		{
			gRecipeMissingReported = true;
			ACPR_LOG( Warning, OUTLET_HOLO, TEXT( "the Rail Outlet recipe %s is missing — no automatic Rail Outlet on cables" ), OUTLET_RECIPE );
		}
		return nullptr;
	}
	// Locked, like vanilla's own automatic poles before their unlock: no child, no switch.
	const AFGRecipeManager* recipes = AFGRecipeManager::Get( world );
	if( !recipes || !recipes->IsRecipeAvailable( recipe ) )
	{
		LogChange( wire, TEXT( "child" ), TEXT( "Rail Outlet recipe not available yet — no automatic Rail Outlet child" ) );
		return nullptr;
	}
	// Not the templated SpawnChildHologram: that one is a CastChecked, and a recipe whose hologram
	// is not a Rail Outlet hologram should disable the child and say so, not assert.
	AFGHologram* spawned = AFGHologram::SpawnChildHologramFromRecipe( wire, CHILD_NAME, recipe, wire->GetOwner(), wire->GetActorLocation() );
	if( spawned )
	{
		// Out of placement, cost and construction until an aim at a Rail makes it the automatic pole.
		spawned->SetDisabled( true );
		spawned->SetActorHiddenInGame( true );
	}
	AACPROutletHologram* child = Cast< AACPROutletHologram >( spawned );
	if( !child )
	{
		if( !gRecipeMissingReported )
		{
			gRecipeMissingReported = true;
			ACPR_LOG( Warning, OUTLET_HOLO, TEXT( "the Rail Outlet recipe %s does not spawn a Rail Outlet hologram (%s) — no automatic Rail Outlet on cables" ),
				OUTLET_RECIPE, spawned ? *spawned->GetClass()->GetName() : TEXT( "nothing" ) );
		}
		return nullptr;
	}
	ACPR_LOG( Verbose, OUTLET_HOLO, TEXT( "%s: Rail Outlet child %s spawned | %s" ), *wire->GetName(), *child->GetName(), *State( wire ) );
	return child;
}

void FACPRWireAutoOutlet::OnWireAimed( AFGWireHologram* wire, const FHitResult& hit )
{
	if( !IsValid( wire ) )
	{
		return;
	}
	AACPROutletHologram* child = EnsureChild( wire );
	if( !child )
	{
		LogChange( wire, TEXT( "aim" ), FString::Printf( TEXT( "%s | %s" ), *DescribeHit( hit ), *State( wire ) ) );
		return;
	}
	// The first end attaches to an existing connection; the automatic pole is for the second.
	// Vanilla has just made its choice for this frame (pole, wall outlet or none). When the aim is a
	// Rail body or an open terminal — the Rail Outlet hologram's own tests, the ones a hand-built
	// outlet uses — the Rail Outlet takes the place instead; otherwise vanilla's choice stands and
	// the Rail Outlet is disabled, as vanilla's unchosen pole is.
	bool railAim = wire->mCurrentConnection == 1 && child->IsValidHitResult( hit );
	if( railAim )
	{
		// The build gun tells the hologram it holds — the cable — about Guidelines (Ctrl); the child
		// mounts with the same state, so a body mount snaps to the metre as a hand-built one does.
		if( child->SnapsToGuideLines() != wire->mSnapToGuideLines )
		{
			child->SetSnapToGuideLines( wire->mSnapToGuideLines );
		}
		child->PreHologramPlacement( hit, false );
		child->TrySnapToActor( hit );
		railAim = child->HasTarget();
	}
	AFGBuildableHologram* active = wire->mActivePoleHologram.Get();
	if( railAim )
	{
		if( active != child )
		{
			// Vanilla's setter enables the one it is given and disables the one it replaces, whichever
			// child either is.
			wire->SetActiveAutomaticPoleHologram( child );
		}
		// Vanilla's setter shows what it enables; said here as well so the child's visibility does not
		// rest on that.
		if( child->IsHidden() )
		{
			child->SetActorHiddenInGame( false );
		}
		child->PostHologramPlacement( hit, false );
		// The cable's second end is the automatic pole's power connection: vanilla points it at its
		// own choice's, so the Rail Outlet's takes its place. The component sits where the built
		// Outlet's will, so the cable ends where it will end.
		UFGPowerConnectionComponent* connection = child->FindComponentByClass< UFGPowerConnectionComponent >();
		if( connection )
		{
			wire->SetConnection( 1, connection );
		}
		LogChange( wire, TEXT( "aim" ), FString::Printf( TEXT( "%s — Rail Outlet is the automatic pole, cable end %s | %s" ),
			*DescribeHit( hit ), connection ? TEXT( "set" ) : TEXT( "NOT FOUND (no power connection on the child hologram)" ), *State( wire ) ) );
	}
	else
	{
		if( active == child )
		{
			// Vanilla left the Rail Outlet active; step aside for its choice.
			wire->SetActiveAutomaticPoleHologram( nullptr );
		}
		child->SetDisabled( true );
		child->SetActorHiddenInGame( true );
		LogChange( wire, TEXT( "aim" ), FString::Printf( TEXT( "%s | %s" ), *DescribeHit( hit ), *State( wire ) ) );
	}
}

void FACPRWireAutoOutlet::OnWireConstructed( AFGWireHologram* wire, AActor* built, const TArray< AActor* >& children )
{
	AFGBuildableWire* builtWire = Cast< AFGBuildableWire >( built );
	if( !IsValid( wire ) || !builtWire )
	{
		return;
	}
	AACPROutlet* outlet = nullptr;
	FString childList;
	for( AActor* child : children )
	{
		childList += FString::Printf( TEXT( " %s[%s]" ), *GetNameSafe( child ), child ? *child->GetClass()->GetName() : TEXT( "?" ) );
		if( AACPROutlet* candidate = Cast< AACPROutlet >( child ) )
		{
			outlet = candidate;
		}
	}
	UFGCircuitConnectionComponent* first = builtWire->GetConnection( 0 );
	UFGCircuitConnectionComponent* second = builtWire->GetConnection( 1 );
	ACPR_LOG( Verbose, OUTLET_HOLO, TEXT( "%s Construct -> %s | children:%s | %s | wire connections: %s on %s / %s on %s" ),
		*wire->GetName(), *builtWire->GetName(), childList.IsEmpty() ? TEXT( " none" ) : *childList, *State( wire ),
		*GetNameSafe( first ), first ? *GetNameSafe( first->GetOwner() ) : TEXT( "?" ),
		*GetNameSafe( second ), second ? *GetNameSafe( second->GetOwner() ) : TEXT( "?" ) );

	// Vanilla replaces its own automatic pole's hologram connection with the built pole's, but not
	// a child it did not spawn: the built wire's second end is still on the Rail Outlet hologram's
	// connection. A second end on a hologram (or on nothing) when a Rail Outlet was built is
	// re-made onto the built Outlet's connection.
	if( !outlet || !first )
	{
		return;
	}
	const bool dangling = !second || second->GetOwner() == nullptr || second->GetOwner()->IsA< AFGHologram >();
	if( !dangling )
	{
		return;
	}
	UFGPowerConnectionComponent* outletConnection = outlet->GetPowerConnection();
	if( !outletConnection )
	{
		ACPR_LOG( Warning, OUTLET_HOLO, TEXT( "%s: built Rail Outlet %s has no power connection — cable left dangling" ), *wire->GetName(), *outlet->GetName() );
		return;
	}
	if( outletConnection->GetNumFreeConnections() < 1 )
	{
		ACPR_LOG( Warning, OUTLET_HOLO, TEXT( "%s: built Rail Outlet %s has no free connection — cable left dangling" ), *wire->GetName(), *outlet->GetName() );
		return;
	}
	builtWire->Disconnect();
	if( builtWire->Connect( first, outletConnection ) )
	{
		ACPR_LOG( Verbose, OUTLET_HOLO, TEXT( "%s: cable's second end was on %s — re-made onto %s's %s" ),
			*wire->GetName(), second ? *GetNameSafe( second->GetOwner() ) : TEXT( "nothing" ), *outlet->GetName(), *outletConnection->GetName() );
	}
	else
	{
		ACPR_LOG( Warning, OUTLET_HOLO, TEXT( "%s: the cable %s could not be connected to the built Rail Outlet %s — it is dangling; dismantle and rebuild it" ),
			*wire->GetName(), *builtWire->GetName(), *outlet->GetName() );
	}
}

void FACPRWireAutoOutlet::Install()
{
	const AFGWireHologram* sample = GetMutableDefault< AFGWireHologram >();

	gBeginPlayHook = SUBSCRIBE_METHOD_VIRTUAL_AFTER( AFGWireHologram::BeginPlay, sample,
		[]( AFGWireHologram* wire )
		{
			if( IsValid( wire ) )
			{
				ACPR_LOG( Verbose, OUTLET_HOLO, TEXT( "%s BeginPlay: %s | poleAvailable=%d wallPoleEnabled=%d spawnChildren=%d poleRecipe=%s wallRecipe=%s" ),
					*wire->GetName(), *State( wire ), wire->mAutomaticPoleAvailable ? 1 : 0, wire->mAutomaticWallPoleEnabled ? 1 : 0,
					wire->ShouldSpawnChildHolograms() ? 1 : 0, *GetNameSafe( wire->mDefaultPowerPoleRecipe.Get() ), *GetNameSafe( wire->mDefaultPowerPoleWallRecipe.Get() ) );
			}
		} );

	gMultiStepHook = SUBSCRIBE_METHOD_VIRTUAL_AFTER( AFGWireHologram::DoMultiStepPlacement, sample,
		[]( const bool& finished, AFGWireHologram* wire, bool fromRelease )
		{
			if( IsValid( wire ) )
			{
				ACPR_LOG( Verbose, OUTLET_HOLO, TEXT( "%s DoMultiStepPlacement(release=%d) -> %d | %s | connections: %s / %s" ),
					*wire->GetName(), fromRelease ? 1 : 0, finished ? 1 : 0, *State( wire ),
					*GetNameSafe( wire->GetConnection( 0 ) ), *GetNameSafe( wire->GetConnection( 1 ) ) );
			}
		} );

	// Vanilla's wall outlet snaps to a beam, and a Rail is one: on such a frame TrySnapToActor returns
	// true, the wall outlet is the active pole, and SetHologramLocationAndRotation is not called
	// (FGHologram.h: "Will only be called if we have a valid hit result and did not snap"). So the
	// choice is made here on a snapped frame and in the placement hook on every other.
	gSnapHook = SUBSCRIBE_METHOD_VIRTUAL_AFTER( AFGWireHologram::TrySnapToActor, sample,
		[]( const bool& snapped, AFGWireHologram* wire, const FHitResult& hit )
		{
			if( snapped )
			{
				OnWireAimed( wire, hit );
			}
		} );

	gPlacedHook = SUBSCRIBE_METHOD_VIRTUAL_AFTER( AFGWireHologram::SetHologramLocationAndRotation, sample,
		[]( AFGWireHologram* wire, const FHitResult& hit )
		{
			OnWireAimed( wire, hit );
		} );

	gConstructHook = SUBSCRIBE_METHOD_VIRTUAL_AFTER( AFGWireHologram::Construct, sample,
		[]( AActor* const& built, AFGWireHologram* wire, TArray< AActor* >& children, FNetConstructionID /*id*/ )
		{
			OnWireConstructed( wire, built, children );
		} );

	ACPR_LOG( Display, OUTLET_HOLO,
		TEXT( "cable hooks installed | after BeginPlay=%d DoMultiStepPlacement=%d TrySnapToActor=%d SetHologramLocationAndRotation=%d Construct=%d" ),
		gBeginPlayHook.IsValid() ? 1 : 0, gMultiStepHook.IsValid() ? 1 : 0, gSnapHook.IsValid() ? 1 : 0,
		gPlacedHook.IsValid() ? 1 : 0, gConstructHook.IsValid() ? 1 : 0 );
}

void FACPRWireAutoOutlet::Uninstall()
{
	if( gBeginPlayHook.IsValid() ) { UNSUBSCRIBE_METHOD( AFGWireHologram::BeginPlay, gBeginPlayHook ); gBeginPlayHook.Reset(); }
	if( gMultiStepHook.IsValid() ) { UNSUBSCRIBE_METHOD( AFGWireHologram::DoMultiStepPlacement, gMultiStepHook ); gMultiStepHook.Reset(); }
	if( gSnapHook.IsValid() ) { UNSUBSCRIBE_METHOD( AFGWireHologram::TrySnapToActor, gSnapHook ); gSnapHook.Reset(); }
	if( gPlacedHook.IsValid() ) { UNSUBSCRIBE_METHOD( AFGWireHologram::SetHologramLocationAndRotation, gPlacedHook ); gPlacedHook.Reset(); }
	if( gConstructHook.IsValid() ) { UNSUBSCRIBE_METHOD( AFGWireHologram::Construct, gConstructHook ); gConstructHook.Reset(); }
	gLastLines.Empty();
}
