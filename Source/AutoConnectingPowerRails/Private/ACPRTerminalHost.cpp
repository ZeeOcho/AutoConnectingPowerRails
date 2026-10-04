// Auto-Connecting Power Rails — §6.1, once, for every terminal host.

#include "ACPRTerminalHost.h"

#include "ACPRCap.h"
#include "ACPRCouplingCue.h"
#include "ACPRDesignerSpace.h"
#include "ACPROutlet.h"
#include "ACPRPowerConnectionComponent.h"
#include "ACPRTerminalComponent.h"
#include "ACPRTerminalRegistry.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildable.h"
#include "Engine/World.h"
#include "UObject/UObjectArray.h"
#include "FGBlueprintProxy.h"
#include "FGColorInterface.h"
#include "FGColoredInstanceMeshProxy.h"
#include "FGFactoryColoringTypes.h"
#include "FGPowerCircuit.h"
#include "FGPowerConnectionComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Hologram/FGBuildableHologram.h"
#include "Hologram/FGHologram.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

void FACPRTerminalHighlight::HideAll( TWeakObjectPtr< AActor >& lastHost )
{
	if( AActor* previous = lastHost.Get() )
	{
		if( IACPRTerminalHost* host = Cast< IACPRTerminalHost >( previous ) )
		{
			host->SetHighlight( FACPRHighlight() );
		}
	}
	lastHost = nullptr;
}

void FACPRTerminalHighlight::Show( TWeakObjectPtr< AActor >& lastHost,
                                   AActor* hitActor,
                                   const UACPRTerminalComponent* selected,
                                   const FACPRSpaceFilter& space,
                                   const UACPRTerminalComponent* pinned,
                                   const UACPRTerminalComponent* pinned2 )
{
	// Resolved, so a ray that lands on a Cap still lights up the host behind it. Otherwise aiming at
	// a capped face would show no markers at all, which reads as "this buildable has no terminals"
	// rather than as "that one is taken and these others are free".
	const FACPRAimedHost aimed = FACPRAimedHost::Resolve( hitActor );
	IACPRTerminalHost* terminalHost =
		aimed.HostActor ? Cast< IACPRTerminalHost >( aimed.HostActor ) : nullptr;

	// §10.1: a host in another build space offers nothing, however open its faces are. Nothing is
	// lit, and the hologram's own refusal carries vanilla's Designer message.
	if( !terminalHost || ( space.Enforce && FACPRSpace::OfActor( aimed.HostActor ) != space.Space ) )
	{
		HideAll( lastHost );
		return;
	}

	// The aim moved from one host to another: the old one goes dark before the new one lights.
	if( lastHost.Get() != aimed.HostActor )
	{
		HideAll( lastHost );
	}

	FACPRHighlight highlight;
	const int32 count = FMath::Min( terminalHost->GetTerminalCount(), 8 );
	for( int32 i = 0; i < count; ++i )
	{
		const UACPRTerminalComponent* terminal =
			terminalHost->GetTerminalAtIndex( static_cast< uint8 >( i ) );

		// The same two conditions the picker uses, and that is the point. §12 says capped, coupled and
		// Outlet-occupied terminals "never display as valid", which is only true for as long as this
		// test and the picker's agree — so they are written against the same two calls, in the same
		// file, rather than being two statements of one rule that can drift. Only the target lights;
		// an open terminal that is not the target shows its ordinary open cue.
		if( !terminal || terminal->IsPrivateInterface() || !terminal->IsOpen() )
		{
			continue;
		}
		if( terminal == selected || terminal == pinned || terminal == pinned2 )
		{
			highlight.Selected |= static_cast< uint8 >( 1u << i );
		}
	}

	terminalHost->SetHighlight( highlight );
	lastHost = aimed.HostActor;
}

void FACPRTerminalHighlight::Pin( TWeakObjectPtr< AActor >& lastHost,
                                  const UACPRTerminalComponent* terminal,
                                  const AActor* skipHost )
{
	AActor* owner = terminal ? terminal->GetOwner() : nullptr;
	IACPRTerminalHost* host = owner ? Cast< IACPRTerminalHost >( owner ) : nullptr;
	if( !host || owner == skipHost )
	{
		// Nothing to pin, or Show() owns that host this frame. Either way whatever this pinned
		// before (a different host, or the same one before the aim came back to it) is let go —
		// when it IS skipHost, Show() has just rewritten it and clearing it here would undo that.
		if( lastHost.Get() != skipHost )
		{
			HideAll( lastHost );
		}
		else
		{
			lastHost = nullptr;
		}
		return;
	}

	if( lastHost.Get() != owner )
	{
		HideAll( lastHost );
	}

	const int32 index = host->GetIndexOfTerminal( terminal );
	if( index < 0 || index >= 8 )
	{
		return;
	}

	FACPRHighlight highlight;
	highlight.Selected = static_cast< uint8 >( 1u << index );

	host->SetHighlight( highlight );
	lastHost = owner;
}

int32 FACPRCoupling::CountCouplingsBetween( const IACPRTerminalHost* host, const AActor* other )
{
	if( !host || !other )
	{
		return 0;
	}
	int32 count = 0;
	const int32 terminals = host->GetTerminalCount();
	for( int32 i = 0; i < terminals; ++i )
	{
		const UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( static_cast< uint8 >( i ) );
		const UACPRTerminalComponent* partner = terminal ? terminal->GetCoupledTo() : nullptr;
		if( partner && partner->GetOwner() == other )
		{
			++count;
		}
	}
	return count;
}

void FACPRCoupling::Release( AActor* actor, IACPRTerminalHost* host, uint8 localIndex, const TCHAR* why )
{
	if( !actor || !host )
	{
		return;
	}

	UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( localIndex );
	UACPRTerminalComponent* partner = terminal ? terminal->GetCoupledTo() : nullptr;
	if( !terminal || !partner )
	{
		return;
	}

	AActor* partnerActor = IsValid( partner ) ? partner->GetOwner() : nullptr;
	IACPRTerminalHost* partnerHost = partnerActor ? Cast< IACPRTerminalHost >( partnerActor ) : nullptr;
	const int32 partnerIndex = partnerHost ? partnerHost->GetIndexOfTerminal( partner ) : INDEX_NONE;

	// The topology half, both sides. Ours first: from here on CountCouplingsBetween does not count
	// this pair, which is what decides whether the hosts' hidden edge goes too.
	terminal->SetCoupledTo( nullptr );
	if( IsValid( partner ) && partner->GetCoupledTo() == terminal )
	{
		partner->SetCoupledTo( nullptr );
	}

	// The saved half, both hosts' records for this pair. A record goes where its coupling goes, so
	// none is left naming a dead actor for the next load to warn about.
	host->GetMutableSavedCouplings().RemoveAll(
		[ localIndex ]( const FACPRSavedCoupling& record ) { return record.LocalTerminalIndex == localIndex; } );
	if( partnerHost && partnerIndex != INDEX_NONE )
	{
		partnerHost->GetMutableSavedCouplings().RemoveAll(
			[ actor, partnerIndex ]( const FACPRSavedCoupling& record )
			{
				return record.OtherActor == actor && record.LocalTerminalIndex == partnerIndex;
			} );
	}

	// The electrical half: one hidden edge serves every coupling between two hosts, so it is removed
	// only with the last of them. RemoveHiddenConnection "disconnects both ends"
	// (FGCircuitConnectionComponent.h:101), so one call.
	bool edgeRemoved = false;
	if( partnerHost && CountCouplingsBetween( host, partnerActor ) == 0 )
	{
		UACPRPowerConnectionComponent* ours = host->GetPowerConnection();
		UACPRPowerConnectionComponent* theirs = partnerHost->GetPowerConnection();
		if( ours && IsValid( theirs ) && ours->HasHiddenConnection( theirs ) )
		{
			ours->RemoveHiddenConnection( theirs );
			edgeRemoved = true;
		}
	}

	ACPR_LOG( Verbose, TERM,
		TEXT( "RELEASED %s.%s[%d] <-> %s[%d] | %s | edgeRemoved=%d" ),
		*actor->GetName(), *terminal->GetName(), localIndex,
		partnerActor ? *partnerActor->GetName() : TEXT( "<gone>" ), partnerIndex,
		why, edgeRemoved ? 1 : 0 );
}

void FACPRCoupling::ReleaseAll( AActor* actor, IACPRTerminalHost* host )
{
	if( !IsValid( actor ) || !host )
	{
		return;
	}

	UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( actor );

	const int32 count = host->GetTerminalCount();
	int32 released = 0;

	for( int32 i = 0; i < count; ++i )
	{
		UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( static_cast< uint8 >( i ) );
		if( !terminal )
		{
			continue;
		}

		// Both sides, and the partner's is the one that matters — ours is about to cease to exist
		// either way, while the survivor is the terminal the player is waiting to be able to use.
		if( terminal->GetCoupledTo() )
		{
			Release( actor, host, static_cast< uint8 >( i ), TEXT( "dismantle" ) );
			++released;
		}

		// Out of §6.1's candidate pool immediately. A host that is playing its dismantle effect is
		// still standing, still registered and still at its cell, so without this a Rail built into
		// the gap during those seconds would couple to something that is leaving.
		if( registry )
		{
			registry->UnregisterTerminal( terminal );
		}
	}

	// Whatever records are left (none should be) go too: they are replayed verbatim on load.
	host->GetMutableSavedCouplings().Reset();

	ACPR_LOG( Verbose, TERM,
		TEXT( "%s released at dismantle | terminals=%d couplingsCleared=%d | registry=%s" ),
		*actor->GetName(), count, released,
		registry ? TEXT( "unregistered" ) : TEXT( "<no registry>" ) );
}

void FACPRAttachments::Register( AActor* hostActor, AActor* attachment )
{
	if( !IsValid( hostActor ) || !IsValid( attachment ) )
	{
		return;
	}

	const IACPRTerminalHost* host = Cast< IACPRTerminalHost >( hostActor );
	TArray< TWeakObjectPtr< AActor > >* list = host ? host->GetAttachmentList() : nullptr;
	if( !list )
	{
		return;
	}

	// AddUnique on the weak pointer, because BeginPlay can run more than once in an editor session
	// and a doubled entry would make the dismantle preview offer the same Cap twice.
	list->AddUnique( attachment );
}

void FACPRAttachments::Unregister( AActor* hostActor, AActor* attachment )
{
	if( !IsValid( hostActor ) )
	{
		return;
	}

	const IACPRTerminalHost* host = Cast< IACPRTerminalHost >( hostActor );
	TArray< TWeakObjectPtr< AActor > >* list = host ? host->GetAttachmentList() : nullptr;
	if( !list )
	{
		return;
	}

	list->Remove( attachment );
}

void FACPRIndicators::ConfigureProxy( UFGColoredInstanceMeshProxy* proxy )
{
	if( !proxy )
	{
		return;
	}
	// Blocked, so this renders exactly as the UStaticMeshComponent it replaces. mBlockInstancing is
	// public on the proxy (FGColoredInstanceMeshProxy.h:58) and is what vanilla sets for the same
	// purpose; SetNumCustomDataFloats is the public setter for the protected count (:40, :85).
	proxy->mBlockInstancing = true;
	proxy->SetNumCustomDataFloats( BUILDABLE_CUSTOM_DATA_NUM );
}

// The dials, adjustable from the in-game console. Each state material is wrapped in a dynamic
// instance whose EmissiveColor is the authored colour times a console variable, re-applied whenever
// a variable moves. Authored values are the defaults, so 1 / 1 / -1 is exactly what
// acpr_materials.py ships; numbers that look right in game are copied back into
// CUE_EMISSIVE_STRENGTH / CUE_OPEN_STRENGTH / CUE_COUPLED_DIM there.
static TAutoConsoleVariable< float > CVarACPRCueGlow(
	TEXT( "acpr.CueGlow" ), 1.0f,
	TEXT( "Multiplier on every lit cue's emissive (open, coupled, powered). Bloom starts a few units above 1." ),
	ECVF_Default );
static TAutoConsoleVariable< float > CVarACPRCueOpen(
	TEXT( "acpr.CueOpen" ), 1.0f,
	TEXT( "Extra multiplier on the OPEN (amber) cue only. 0 makes an open terminal dark." ),
	ECVF_Default );
static TAutoConsoleVariable< float > CVarACPRCueDim(
	TEXT( "acpr.CueDim" ), -1.0f,
	TEXT( "Coupled-but-unpowered as a fraction of powered (0..1). -1 = as authored (CUE_COUPLED_DIM)." ),
	ECVF_Default );
// The highlight is the cue's own colour turned up, and how far up is the one thing about it that
// can only be judged in game. A multiplier on the authored instance (CUE_SELECTED_STRENGTH in
// acpr_materials.py), so 1 is the authored value.
static TAutoConsoleVariable< float > CVarACPRCueSelect(
	TEXT( "acpr.CueSelect" ), 1.0f,
	TEXT( "Multiplier on the SELECTED terminal's highlight (the hologram's exact target)." ),
	ECVF_Default );

static UMaterialInterface* ACPRIndicatorMaterial( EACPRIndicator state )
{
	// Soft paths, loaded once, so a clean checkout binds these with no script and no click —
	// the same argument as AACPRRail::mRailMeshAsset. Made by acpr_materials.py.
	static const TCHAR* paths[] = {
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_Cue.MI_ACPR_Cue" ),
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_CueCoupled.MI_ACPR_CueCoupled" ),
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_CuePowered.MI_ACPR_CuePowered" ),
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_CueCapped.MI_ACPR_CueCapped" ),
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_CueSelected.MI_ACPR_CueSelected" ),
	};
	static_assert( UE_ARRAY_COUNT( paths ) == ACPR_INDICATOR_STATES, "one instance per EACPRIndicator" );
	static TSoftObjectPtr< UMaterialInterface > authored[ ACPR_INDICATOR_STATES ];
	static TObjectPtr< UMaterialInstanceDynamic > live[ ACPR_INDICATOR_STATES ];
	static FLinearColor authoredEmissive[ ACPR_INDICATOR_STATES ];
	static float appliedGlow = -1.0f, appliedOpen = -1.0f, appliedDim = -2.0f;
	static float appliedSelect = -1.0f;

	const int32 i = static_cast< int32 >( state );
	if( authored[ i ].IsNull() )
	{
		authored[ i ] = TSoftObjectPtr< UMaterialInterface >( FSoftObjectPath( paths[ i ] ) );
	}
	UMaterialInterface* const parent = authored[ i ].LoadSynchronous();
	if( !parent )
	{
		return nullptr;
	}

	if( !live[ i ] )
	{
		// Rooted: four objects for the life of the process, shared by every ACPR actor.
		live[ i ] = UMaterialInstanceDynamic::Create( parent, GetTransientPackage() );
		live[ i ]->AddToRoot();
		const FMaterialParameterInfo info( FName( TEXT( "EmissiveColor" ) ) );
		if( !parent->GetVectorParameterValue( FHashedMaterialParameterInfo( info ), authoredEmissive[ i ] ) )
		{
			authoredEmissive[ i ] = FLinearColor::Black;
		}
		appliedGlow = -1.0f;   // force a first application below
	}

	const float glow = CVarACPRCueGlow.GetValueOnGameThread();
	const float open = CVarACPRCueOpen.GetValueOnGameThread();
	const float dim = CVarACPRCueDim.GetValueOnGameThread();
	const float select = CVarACPRCueSelect.GetValueOnGameThread();
	if( glow != appliedGlow || open != appliedOpen || dim != appliedDim || select != appliedSelect )
	{
		appliedGlow = glow; appliedOpen = open; appliedDim = dim;
		appliedSelect = select;
		for( int32 k = 0; k < ACPR_INDICATOR_STATES; ++k )
		{
			if( !live[ k ] )
			{
				continue;
			}
			FLinearColor colour = authoredEmissive[ k ] * glow;
			if( k == static_cast< int32 >( EACPRIndicator::Open ) )
			{
				colour *= open;
			}
			else if( k == static_cast< int32 >( EACPRIndicator::Selected ) )
			{
				colour *= select;
			}
			else if( k == static_cast< int32 >( EACPRIndicator::Coupled ) && dim >= 0.0f
			         && live[ static_cast< int32 >( EACPRIndicator::Powered ) ] )
			{
				colour = authoredEmissive[ static_cast< int32 >( EACPRIndicator::Powered ) ] * glow * dim;
			}
			colour.A = 1.0f;
			live[ k ]->SetVectorParameterValue( FName( TEXT( "EmissiveColor" ) ), colour );
		}
		ACPR_LOG( Log, CUE,
			TEXT( "dials applied | acpr.CueGlow=%.2f acpr.CueOpen=%.2f acpr.CueDim=%.2f acpr.CueSelect=%.2f" ),
			glow, open, dim, select );
	}
	return live[ i ];
}

static const TCHAR* ACPRIndicatorName( EACPRIndicator state )
{
	switch( state )
	{
		case EACPRIndicator::Coupled: return TEXT( "coupled" );
		case EACPRIndicator::Powered: return TEXT( "powered" );
		case EACPRIndicator::Capped:   return TEXT( "capped" );
		case EACPRIndicator::Selected: return TEXT( "SELECTED" );
		default:                       return TEXT( "open" );
	}
}

/** §4's state plus the host's network power, folded into the four lights. */
static EACPRIndicator ACPRIndicatorFor( const UACPRTerminalComponent* terminal, bool hostPowered )
{
	if( !terminal )
	{
		return EACPRIndicator::Open;
	}
	switch( terminal->GetTerminalState() )
	{
		case EACPRTerminalState::Capped:  return EACPRIndicator::Capped;
		case EACPRTerminalState::Coupled: return hostPowered ? EACPRIndicator::Powered : EACPRIndicator::Coupled;
		default:                          return EACPRIndicator::Open;
	}
}

static FString ACPRDescribeCustomData( const UPrimitiveComponent* component )
{
	if( !component )
	{
		return TEXT( "<null>" );
	}
	const TArray< float >& data = component->GetCustomPrimitiveData().Data;
	FString out = FString::Printf( TEXT( "%d floats [" ), data.Num() );
	for( int32 i = 0; i < data.Num(); ++i )
	{
		out += FString::Printf( TEXT( "%s%.2f" ), i ? TEXT( " " ) : TEXT( "" ), data[ i ] );
	}
	return out + TEXT( "]" );
}

namespace
{
	struct FACPRCostMeter
	{
		int32 Calls[ static_cast< int32 >( EACPRCost::Count ) ] = {};
		double Seconds[ static_cast< int32 >( EACPRCost::Count ) ] = {};
		double Worst[ static_cast< int32 >( EACPRCost::Count ) ] = {};
	};
	FACPRCostMeter GCostMeter;

	const TCHAR* ACPRCostName( EACPRCost phase )
	{
		switch( phase )
		{
			case EACPRCost::BeginPlayRail:     return TEXT( "beginPlay.rail" );
			case EACPRCost::BeginPlayJunction: return TEXT( "beginPlay.junction" );
			case EACPRCost::BeginPlayOutlet:   return TEXT( "beginPlay.outlet" );
			case EACPRCost::BeginPlayCap:      return TEXT( "beginPlay.cap" );
			case EACPRCost::Dismantle:         return TEXT( "dismantle" );
			case EACPRCost::EndPlay:           return TEXT( "endPlay" );
			case EACPRCost::ManagerInitialize: return TEXT( "bp.initialize" );
			case EACPRCost::ManagerRemap:      return TEXT( "bp.remap" );
			case EACPRCost::ManagerConstruct:  return TEXT( "bp.construct" );
			case EACPRCost::ManagerAudit:      return TEXT( "bp.audit" );
			case EACPRCost::ManagerFixUp:      return TEXT( "bp.fixUp" );
			case EACPRCost::BridgeSpawn:       return TEXT( "bridge.spawn" );
			case EACPRCost::BridgeBeginPlay:   return TEXT( "bridge.beginPlay" );
			case EACPRCost::BridgeApply:       return TEXT( "bridge.apply" );
			case EACPRCost::BridgeValidate:    return TEXT( "bridge.validate" );
			case EACPRCost::BridgeShow:        return TEXT( "bridge.show" );
			case EACPRCost::BridgeMaterial:    return TEXT( "bridge.material" );
			case EACPRCost::BridgeValidateVanilla: return TEXT( "bridge.validate.vanilla" );
			case EACPRCost::BridgeHidden:      return TEXT( "bridge.show.hidden" );
			case EACPRCost::BridgeDisable:     return TEXT( "bridge.disable" );
			default:                           return TEXT( "?" );
		}
	}

	/** The cost meter's window: wall clock, so it needs no world and no host. */
	double GCostWindowStart = -1.0;
	constexpr double ACPR_COST_WINDOW = 30.0;
}

FACPRCostScope::FACPRCostScope( EACPRCost phase )
	: Phase( phase ), StartCycles( FACPRTrace::Enabled() ? FPlatformTime::Cycles64() : 0 )
{
}

FACPRCostScope::~FACPRCostScope()
{
	if( StartCycles == 0 )
	{
		return;
	}
	const double seconds = FPlatformTime::ToSeconds64( FPlatformTime::Cycles64() - StartCycles );
	const int32 i = static_cast< int32 >( Phase );
	GCostMeter.Calls[ i ] += 1;
	GCostMeter.Seconds[ i ] += seconds;
	GCostMeter.Worst[ i ] = FMath::Max( GCostMeter.Worst[ i ], seconds );

	// The [ACPR-COST] line, printed from the meter itself once a window has passed. Only phases that
	// ran, and only when something ran, so a quiet game prints nothing. Trace was on at the scope's
	// start; a window that straddles it being switched off is simply dropped below.
	const double now = FPlatformTime::Seconds();
	if( GCostWindowStart < 0.0 )
	{
		GCostWindowStart = now;
		return;
	}
	if( now - GCostWindowStart < ACPR_COST_WINDOW )
	{
		return;
	}
	const double window = now - GCostWindowStart;
	GCostWindowStart = now;

	if( FACPRTrace::Enabled() )
	{
		FString cost;
		double costTotal = 0.0;
		for( int32 k = 0; k < static_cast< int32 >( EACPRCost::Count ); ++k )
		{
			if( GCostMeter.Calls[ k ] == 0 )
			{
				continue;
			}
			cost += FString::Printf( TEXT( " %s n=%d %.1f ms (avg %.0f us, worst %.1f ms)" ),
				ACPRCostName( static_cast< EACPRCost >( k ) ), GCostMeter.Calls[ k ],
				GCostMeter.Seconds[ k ] * 1000.0,
				1e6 * GCostMeter.Seconds[ k ] / GCostMeter.Calls[ k ],
				GCostMeter.Worst[ k ] * 1000.0 );
			costTotal += GCostMeter.Seconds[ k ];
		}
		if( !cost.IsEmpty() )
		{
			ACPR_LOG( Display, COST,
				TEXT( "%.0fs window | ours total %.1f ms |%s | uobjects=%d of %d" ),
				window, costTotal * 1000.0, *cost,
				GUObjectArray.GetObjectArrayNumMinusAvailable(), GUObjectArray.GetObjectArrayCapacity() );
		}
	}
	GCostMeter = FACPRCostMeter();
}

void FACPRIndicators::ReadBack( const UFGColoredInstanceMeshProxy* proxy, const TCHAR* tag, const TCHAR* label )
{
	FString slots;
	if( proxy )
	{
		for( int32 i = 0; i < proxy->GetNumMaterials(); ++i )
		{
			const UMaterialInterface* m = proxy->GetMaterial( i );
			slots += FString::Printf( TEXT( "%s%d=%s" ), i ? TEXT( " " ) : TEXT( "" ), i, m ? *m->GetName() : TEXT( "<none>" ) );
		}
	}
	ACPR_LOG_TAGGED( Verbose, tag, TEXT( "indicators readBack | %s | %s | slots: %s" ), label,
		*ACPRDescribeCustomData( proxy ), *slots );
}

int32 ACPRSlot::IndexOf( const UStaticMeshComponent* component, const FName& name )
{
	const UStaticMesh* mesh = component ? component->GetStaticMesh() : nullptr;
	if( !mesh )
	{
		return INDEX_NONE;
	}
	const int32 index = mesh->GetMaterialIndex( name );
	if( index == INDEX_NONE )
	{
		static TSet< const UStaticMesh* > warned;
		if( !warned.Contains( mesh ) )
		{
			warned.Add( mesh );
			ACPR_LOG( Warning, SLOT,
				TEXT( "%s has no material slot named '%s' (%d slots) — re-run acpr_meshes.py "
				      "and acpr_materials.py." ),
				*mesh->GetName(), *name.ToString(), mesh->GetStaticMaterials().Num() );
		}
	}
	return index;
}

bool FACPRIndicators::Push( AFGBuildable* buildable, const IACPRTerminalHost* host,
                            TArrayView< const FACPRIndicatorSlot > slots, const TCHAR* tag )
{
	if( !buildable || !host )
	{
		return false;
	}

	// The whole-host summary for TerminalIndex -1 — the body's strip: powered, or not. A body is not
	// a terminal, so it never shows the terminal's amber "open"; a Rail with both ends open reads
	// unpowered on its body and open on its collars, which is exactly its state. The "amber line =
	// unfinished here" cue is carried by the collars alone.
	const bool powered = host->HasNetworkPower();
	const EACPRIndicator summary = powered ? EACPRIndicator::Powered : EACPRIndicator::Coupled;

	// The aiming hologram's highlight. Cleared by the hologram itself (aim-away and Destroyed), so
	// what is set is current.
	const FACPRHighlight* highlight = host->GetHighlight();
	if( highlight && !highlight->IsSet() )
	{
		highlight = nullptr;
	}

	bool changed = false;
	FString report;
	for( const FACPRIndicatorSlot& entry : slots )
	{
		if( !entry.Proxy )
		{
			continue;
		}
		EACPRIndicator state;
		if( entry.PowerFloor )
		{
			const UACPRTerminalComponent* const terminal = ( entry.TerminalIndex >= 0 )
				? host->GetTerminalAtIndex( static_cast< uint8 >( entry.TerminalIndex ) ) : nullptr;
			const bool bare = terminal && terminal->GetTerminalState() == EACPRTerminalState::Open;
			state = !bare ? EACPRIndicator::Capped
			      : ( summary == EACPRIndicator::Powered ) ? EACPRIndicator::Powered : EACPRIndicator::Coupled;
		}
		else if( entry.TerminalIndex < 0 )
		{
			state = summary;
		}
		else
		{
			state = ACPRIndicatorFor( host->GetTerminalAtIndex( static_cast< uint8 >( entry.TerminalIndex ) ), powered );
			// Only an OPEN terminal can be highlighted — Show() writes only those — so this never
			// hides a coupled or capped state behind the highlight.
			if( highlight && state == EACPRIndicator::Open && entry.TerminalIndex < 8 )
			{
				if( highlight->Selected & ( 1u << entry.TerminalIndex ) )
				{
					state = EACPRIndicator::Selected;
				}
			}
		}
		UMaterialInterface* const wanted = ACPRIndicatorMaterial( state );
		if( !wanted )
		{
			ACPR_LOG_TAGGED( Warning, tag,
				TEXT( "indicator material for '%s' did not load — run acpr_materials.py." ),
				ACPRIndicatorName( state ) );
			return false;
		}
		const int32 slot = ACPRSlot::IndexOf( entry.Proxy, entry.Slot );
		if( slot != INDEX_NONE && entry.Proxy->GetMaterial( slot ) != wanted )
		{
			entry.Proxy->SetMaterial( slot, wanted );
			changed = true;
		}
	}

	if( changed && ACPR_LOG_ACTIVE( Verbose ) )
	{
		// Only when there is a line to print: string formatting per slot on every poll, 2000 polls a
		// second at 1000 hosts, is too much for a line that is Verbose. The states are recomputed
		// for it (cheap: the same reads as above).
		for( const FACPRIndicatorSlot& entry : slots )
		{
			if( !entry.Proxy )
			{
				continue;
			}
			const int32 slot = ACPRSlot::IndexOf( entry.Proxy, entry.Slot );
			const UMaterialInterface* m = slot != INDEX_NONE ? entry.Proxy->GetMaterial( slot ) : nullptr;
			report += FString::Printf( TEXT( " %s[%s]=%s" ), *entry.Proxy->GetName(), *entry.Slot.ToString(), m ? *m->GetName() : TEXT( "<none>" ) );
		}
		const FFactoryCustomizationData& data = buildable->GetCustomizationData_Native();
		ACPR_LOG_TAGGED( Verbose, tag,
			TEXT( "indicators push | %s | swatch=%s slot=%d |%s" ),
			*buildable->GetName(),
			data.SwatchDesc ? *data.SwatchDesc->GetName() : TEXT( "<none>" ),
			static_cast< int32 >( data.ColorSlot ), *report );
	}
	return changed;
}

void FACPRIndicators::AdoptSwatch( AFGBuildable* built, const AActor* partnerActor, const TCHAR* tag )
{
	const AFGBuildable* const partner = Cast< AFGBuildable >( partnerActor );
	if( !built || !partner || built == partner || !built->HasAuthority() )
	{
		return;
	}

	const FFactoryCustomizationData& data = partner->GetCustomizationData_Native();
	if( built->GetCustomizationData_Native() == data )
	{
		return;
	}

	// Through the colour interface's pure virtual — a vtable call, so no FactoryGame export is needed
	// (ACPRBlueprintTerminalManager does the same). FGColorInterface.h:22: "Set the
	// active/saved customization. This in turn should call ApplyCustomizationData", so one call.
	IFGColorInterface* colour = Cast< IFGColorInterface >( built );
	if( !colour )
	{
		return;
	}
	colour->SetCustomizationData_Native( data, false );

	const FFactoryCustomizationData& now = built->GetCustomizationData_Native();
	ACPR_LOG_TAGGED( Verbose, tag,
		TEXT( "%s adopts swatch from %s | swatch=%s slot=%d | readBack swatch=%s slot=%d" ),
		*built->GetName(), *partner->GetName(),
		data.SwatchDesc ? *data.SwatchDesc->GetName() : TEXT( "<none>" ),
		static_cast< int32 >( data.ColorSlot ),
		now.SwatchDesc ? *now.SwatchDesc->GetName() : TEXT( "<none>" ),
		static_cast< int32 >( now.ColorSlot ) );
}

void FACPRAttachments::Collect( AActor* hostActor, TArray< AActor* >& out_actors )
{
	if( !IsValid( hostActor ) )
	{
		return;
	}

	const IACPRTerminalHost* host = Cast< IACPRTerminalHost >( hostActor );
	TArray< TWeakObjectPtr< AActor > >* list = host ? host->GetAttachmentList() : nullptr;
	if( !list )
	{
		return;
	}

	// Pruned while walking, backwards so the removals do not disturb the indices still to be read.
	// An attachment destroyed by something other than its host — a world reset, a mod reload — would
	// otherwise sit in the list forever and be offered to the dismantle preview as a null.
	for( int32 i = list->Num() - 1; i >= 0; --i )
	{
		AActor* attachment = ( *list )[ i ].Get();
		if( IsValid( attachment ) )
		{
			out_actors.AddUnique( attachment );
		}
		else
		{
			list->RemoveAt( i );
		}
	}
}

FACPRAimedHost FACPRAimedHost::Resolve( AActor* hitActor )
{
	FACPRAimedHost out;

	if( !IsValid( hitActor ) )
	{
		return out;
	}

	// Already a host: the ordinary case, and it must stay free.
	if( Cast< IACPRTerminalHost >( hitActor ) )
	{
		out.HostActor = hitActor;
		return out;
	}

	// §9's Cap. It saves WHICH terminal it occupies (that is the whole of its design), so it can
	// answer both halves of the question directly rather than anything having to search for it.
	if( AACPRCap* cap = Cast< AACPRCap >( hitActor ) )
	{
		out.Attachment = cap;
		out.HostActor = cap->GetHostActor();
		out.OccupiedTerminal = cap->ResolveHostTerminal();
		return out;
	}

	// §8's Outlet, terminal-mounted only.
	//
	// A BODY-MOUNTED Outlet is deliberately not resolved, and the difference is the rule rather than
	// an omission: §8.1 says a body mount "changes no terminal state", so it stands in front of no
	// terminal and there is nothing behind it to resolve to. Resolving it would hand the caller a
	// host whose terminals are nowhere near the aim point. An Outlet is also itself an
	// IACPRTerminalHost, so the cast above already returned for both modes — this branch is
	// unreachable today and is written for the day the Outlet stops being one.
	if( AACPROutlet* outlet = Cast< AACPROutlet >( hitActor ) )
	{
		if( outlet->GetHostMode() == EACPROutletHostMode::Terminal )
		{
			out.Attachment = outlet;
			out.HostActor = outlet->GetHostActor();
			out.OccupiedTerminal = outlet->ResolveHostTerminal();
		}
		return out;
	}

	return out;
}

FACPRTerminalPick FACPRTerminalPicker::FindAimed( const FHitResult& hitResult,
                                                  double snapRange,
                                                  double ambiguityTol,
                                                  double faceAlignMin,
                                                  const UACPRTerminalComponent* exclude )
{
	FACPRTerminalPick pick;

	if( !hitResult.bBlockingHit )
	{
		return pick;
	}

	// Invariant 3: "Bare Rail bodies are not coupling targets." Candidates come only from the actor
	// being AIMED AT, never from a registry query — a registry answers "what is near", and the
	// question here is "what is being pointed at". Invariant 2 is the same distinction from the other
	// side: proximity may locate a candidate, proximity alone never couples.
	// Resolved through any attachment in front of the host. See FACPRAimedHost: a Cap is its own
	// actor standing proud of the terminal plane, so a ray at a capped face hits the Cap and the
	// host is never consulted. Resolving here rather than in each caller is what keeps the answer
	// the same for the picker, the highlight and §7.3's refusal.
	//
	// The GEOMETRY is not adjusted and does not need to be: an attachment sits on its terminal's own
	// plane with its terminal's own outward, so the impact point and normal already read correctly
	// for the host's faces. Only the actor needs resolving.
	const FACPRAimedHost aimed = FACPRAimedHost::Resolve( hitResult.GetActor() );
	AActor* hitActor = aimed.HostActor;
	const IACPRTerminalHost* host = hitActor ? Cast< IACPRTerminalHost >( hitActor ) : nullptr;
	if( !host )
	{
		return pick;
	}

	pick.Host = hitActor;

	const int32 count = host->GetTerminalCount();
	const FVector hitNormal = hitResult.ImpactNormal.GetSafeNormal();
	const bool useAlignment = faceAlignMin > 0.0 && !hitNormal.IsNearlyZero();

	auto IsOpenInRange = [ & ]( const UACPRTerminalComponent* terminal, double& out_dist ) -> bool
	{
		// IsPrivateInterface is §4's "not a continuation terminal, not a blueprint candidate, and
		// cannot accept a Rail" — three sentences that are one property, enforced once here rather
		// than remembered separately by every hologram that targets terminals.
		if( !terminal || terminal == exclude || terminal->IsPrivateInterface() || !terminal->IsOpen() )
		{
			return false;
		}

		const double distance = FVector::Dist( terminal->GetComponentLocation(), hitResult.ImpactPoint );
		if( distance > snapRange )
		{
			return false;
		}

		out_dist = distance;
		return true;
	};

	// The pre-pass, and it is a pre-pass rather than a filter for a reason. The face rule can
	// only apply when some face IS being pointed at; aiming at a Rail's SIDE near its end gives a
	// normal perpendicular to both terminals, nothing aligns, and the rule has to stand down to
	// distance alone rather than refusing everything. Deciding that needs one look at the whole set
	// before any candidate is judged.
	bool anyAligned = false;
	for( int32 i = 0; i < count; ++i )
	{
		const UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( static_cast< uint8 >( i ) );

		double distance = 0.0;
		if( !IsOpenInRange( terminal, distance ) )
		{
			continue;
		}

		++pick.OpenInRange;

		if( useAlignment &&
			FVector::DotProduct( terminal->GetOutwardAxis().GetSafeNormal(), hitNormal ) > faceAlignMin )
		{
			anyAligned = true;
		}
	}

	auto IsCandidate = [ & ]( const UACPRTerminalComponent* terminal, double& out_dist ) -> bool
	{
		if( !IsOpenInRange( terminal, out_dist ) )
		{
			return false;
		}

		if( anyAligned &&
			FVector::DotProduct( terminal->GetOutwardAxis().GetSafeNormal(), hitNormal ) <= faceAlignMin )
		{
			return false;
		}

		return true;
	};

	// PASS 1: the strict minimum, with no tolerance anywhere in the comparison.
	UACPRTerminalComponent* best = nullptr;
	double bestDistance = 0.0;

	for( int32 i = 0; i < count; ++i )
	{
		UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( static_cast< uint8 >( i ) );

		double distance = 0.0;
		if( !IsCandidate( terminal, distance ) )
		{
			continue;
		}

		++pick.Candidates;

		if( !best || distance < bestDistance )
		{
			best = terminal;
			bestDistance = distance;
		}
	}

	if( !best )
	{
		return pick;
	}

	pick.Distance = bestDistance;

	// PASS 2: how many share it. Fusing this into pass 1 makes the count depend on
	// terminal index order, and terminal index order is save format. Six terminals maximum, so the
	// second pass is free.
	for( int32 i = 0; i < count; ++i )
	{
		double distance = 0.0;
		if( IsCandidate( host->GetTerminalAtIndex( static_cast< uint8 >( i ) ), distance ) &&
			FMath::Abs( distance - bestDistance ) <= ambiguityTol )
		{
			++pick.Tied;
		}
	}

	// Invariant 8: ambiguity produces no coupling. The counts survive the null return, so a caller
	// can say "REFUSED (ambiguous)" rather than "nothing found" — two very different things to read
	// in a log, and the reason this refuses here instead of leaving it to five callers.
	if( pick.Tied > 1 )
	{
		return pick;
	}

	pick.Terminal = best;
	return pick;
}

int32 IACPRTerminalHost::GetIndexOfTerminal( const UACPRTerminalComponent* terminal ) const
{
	// A default implementation rather than a pure virtual: every host that can answer
	// GetTerminalAtIndex can answer this by asking it, and two of them would otherwise write the
	// same loop. A host with many terminals and a faster lookup is free to override.
	if( !terminal )
	{
		return INDEX_NONE;
	}

	const int32 count = GetTerminalCount();
	for( int32 i = 0; i < count; ++i )
	{
		if( GetTerminalAtIndex( static_cast< uint8 >( i ) ) == terminal )
		{
			return i;
		}
	}

	return INDEX_NONE;
}

bool FACPRCoupling::VerifyPowerCircuitSafe( const AActor* actor,
                                            UFGCircuitConnectionComponent* connection,
                                            const TCHAR* what )
{
	if( !connection )
	{
		return false;
	}

	const bool declaresPower = connection->GetCircuitType()
		&& connection->GetCircuitType()->IsChildOf( UFGPowerCircuit::StaticClass() );
	const bool isPowerConnection = connection->IsA< UFGPowerConnectionComponent >();

	if( declaresPower && !isPowerConnection )
	{
		ACPR_LOG( Error, TERM,
			TEXT( "%s REFUSED to connect %s (%s, class %s): it declares circuit type %s "
			      "but is not a UFGPowerConnectionComponent. UFGPowerCircuit::OnCircuitChanged "
			      "CastChecks every member of a power circuit to that type on the subsystem tick, "
			      "so adding this would have crashed the game a frame later with no mod code on the "
			      "stack." ),
			actor ? *actor->GetName() : TEXT( "?" ), what,
			*connection->GetName(), *connection->GetClass()->GetName(),
			*connection->GetCircuitType()->GetName() );
		return false;
	}

	return true;
}

void FACPRCoupling::Resolve( AActor* actor, IACPRTerminalHost* host )
{
	if( !actor || !host )
	{
		return;
	}

	// Invariant 13: "Topology operations are atomic and server-authoritative."
	if( !actor->HasAuthority() )
	{
		return;
	}

	if( host->WasLoadedFromSave() )
	{
		// Invariant 6, and the reason this is an if/else rather than a sequence: a loaded host must
		// NOT also run the coincidence search. Two Caps face to face are coincident and opposing,
		// and §6.2 says they must stay uncoupled — so re-deriving topology from geometry on every
		// load would quietly couple things the player deliberately separated.
		RestoreSaved( actor, host );

		// A placed blueprint is construction, not a load — see IACPRTerminalHost::WasPlacedFromBlueprint.
		// The replay above has restored the blueprint's inside; its boundary still has to meet the world,
		// and that is §6.1's question for every terminal left open. TryCoupleTerminal refuses anything
		// that is not open, so restored and capped terminals pass straight through, and every rule a
		// hand-built terminal obeys — space, opposition, ambiguity — applies unchanged.
		if( host->WasPlacedFromBlueprint() )
		{
			int32 searched = 0;
			int32 coupled = 0;

			const int32 count = host->GetTerminalCount();
			for( int32 i = 0; i < count; ++i )
			{
				const uint8 index = static_cast< uint8 >( i );
				UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( index );
				if( !terminal || !terminal->IsOpen() )
				{
					continue;
				}

				++searched;
				TryCoupleTerminal( actor, host, terminal, index );

				if( !terminal->IsOpen() )
				{
					++coupled;
				}
			}

			ACPR_LOG( Verbose, BP,
				TEXT( "%s placed from a blueprint: boundary search | open=%d coupled=%d" ),
				*actor->GetName(), searched, coupled );
		}
	}
	else
	{
		const int32 count = host->GetTerminalCount();
		for( int32 i = 0; i < count; ++i )
		{
			const uint8 index = static_cast< uint8 >( i );
			TryCoupleTerminal( actor, host, host->GetTerminalAtIndex( index ), index );
		}

		// A hand-built host takes its FIRST coupled partner's swatch — index order, so
		// a Rail takes its anchor end's (terminal A is the end it was grown from). A host that
		// coupled to nothing keeps the build gun's.
		for( int32 i = 0; i < count; ++i )
		{
			const UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( static_cast< uint8 >( i ) );
			const UACPRTerminalComponent* partner = terminal ? terminal->GetCoupledTo() : nullptr;
			if( partner && partner->GetOwner() )
			{
				FACPRIndicators::AdoptSwatch( Cast< AFGBuildable >( actor ), partner->GetOwner(), ACPR_TAG_TEXT( TERM ) );
				break;
			}
		}
	}

	// Both paths. See the header: a hand-built host finds nothing here, and everything it can find
	// comes from a save file or a blueprint rather than from §6.1.
	ScrubStrayHiddenEdges( actor, host );
}

void FACPRCoupling::TryCoupleTerminal( AActor* actor, IACPRTerminalHost* host,
                                       UACPRTerminalComponent* terminal, uint8 localIndex )
{
	if( !terminal || !terminal->IsOpen() )
	{
		return;
	}

	UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( actor );
	if( !registry )
	{
		return;
	}

	TArray< UACPRTerminalComponent* > inCell;
	registry->FindInCell( terminal->GetQuantizedLocation(), terminal, actor, inCell );

	// Coincidence is already true by construction for everything in the cell, so this filters on
	// open, opposing and — §10.1 — the same build space.
	TArray< UACPRTerminalComponent* > candidates;
	for( UACPRTerminalComponent* other : inCell )
	{
		if( terminal->CanCoupleWith( other ) )
		{
			candidates.Add( other );
			continue;
		}

		// A pair that would couple on every other rule and is refused only for sitting in different
		// spaces is invisible in game, because a Rail looks the same coupled or not. So it gets a
		// line. Pairs refused on geometry or state stay silent.
		if( terminal->CanCoupleWith( other, /*checkSpace*/ false ) )
		{
			ACPR_LOG( Display, SPACE,
				TEXT( "NOT COUPLED (10.1) %s.%s <-> %s.%s | coincident and opposing, but %s vs %s "
				      "| cell=(%d,%d,%d)" ),
				*actor->GetName(), *terminal->GetName(),
				other->GetOwner() ? *other->GetOwner()->GetName() : TEXT( "?" ), *other->GetName(),
				*FACPRSpace::Describe( FACPRSpace::OfActor( actor ) ),
				*FACPRSpace::Describe( FACPRSpace::OfActor( other->GetOwner() ) ),
				terminal->GetQuantizedLocation().X, terminal->GetQuantizedLocation().Y,
				terminal->GetQuantizedLocation().Z );
		}
	}

	if( candidates.Num() == 0 )
	{
		return;
	}

	// Invariant 8: "Ambiguity produces no coupling." Three terminals meeting in one cell is not a
	// three-way joint, it is a question nobody asked us to answer — and invariant 4 allows each
	// terminal exactly one occupant, so picking one would be choosing on the player's behalf.
	if( candidates.Num() > 1 )
	{
		FString names;
		for( const UACPRTerminalComponent* c : candidates )
		{
			names += FString::Printf( TEXT( "%s.%s " ),
				c->GetOwner() ? *c->GetOwner()->GetName() : TEXT( "?" ), *c->GetName() );
		}

		ACPR_LOG( Warning, TERM,
			TEXT( "%s.%s found %d coincident opposing terminals [ %s] — invariant 8 "
			      "says ambiguity produces no coupling, so none was made." ),
			*actor->GetName(), *terminal->GetName(), candidates.Num(), *names );
		return;
	}

	Form( actor, host, terminal, localIndex, candidates[ 0 ] );
}

void FACPRCoupling::Form( AActor* actor, IACPRTerminalHost* host,
                          UACPRTerminalComponent* ours, uint8 localIndex,
                          UACPRTerminalComponent* theirs )
{
	if( !ours || !theirs )
	{
		return;
	}

	// Asking the OWNER for the index — through the interface it must implement to have terminals at
	// all — works for two terminals or six, and for classes that do not exist yet.
	AActor* otherActor = theirs->GetOwner();
	IACPRTerminalHost* otherHost = Cast< IACPRTerminalHost >( otherActor );

	if( !otherHost )
	{
		ACPR_LOG( Warning, TERM,
			TEXT( "%s.%s found a coincident terminal on %s, which does not implement "
			      "IACPRTerminalHost. A terminal whose owner cannot name it cannot be coupled to." ),
			*actor->GetName(), *ours->GetName(),
			otherActor ? *otherActor->GetName() : TEXT( "?" ) );
		return;
	}

	const int32 otherIndexSigned = otherHost->GetIndexOfTerminal( theirs );
	if( otherIndexSigned == INDEX_NONE )
	{
		ACPR_LOG( Warning, TERM,
			TEXT( "%s.%s found terminal %s on %s, but that host does not recognise it "
			      "as one of its own. The Coupling is refused rather than recorded against an index "
			      "that would not survive a reload." ),
			*actor->GetName(), *ours->GetName(), *theirs->GetName(),
			otherActor ? *otherActor->GetName() : TEXT( "?" ) );
		return;
	}

	const uint8 otherIndex = static_cast< uint8 >( otherIndexSigned );

	// The electrical half: one hidden edge between the two HOSTS' connections. Both are CDO
	// subobjects, so they exist whether or not the partner's BeginPlay has run (the load path).
	// AddHiddenConnection is idempotent, so the second host's replay is harmless.
	UACPRPowerConnectionComponent* ourPower = host->GetPowerConnection();
	UACPRPowerConnectionComponent* theirPower = otherHost->GetPowerConnection();
	if( !VerifyPowerCircuitSafe( actor, ourPower, TEXT( "coupling, our side" ) ) ||
	    !VerifyPowerCircuitSafe( actor, theirPower, TEXT( "coupling, their side" ) ) )
	{
		return;
	}
	if( !ourPower->HasHiddenConnection( theirPower ) )
	{
		ourPower->AddHiddenConnection( theirPower );
	}

	// The topology half. Reciprocal, because invariant 4 is about both ends: a terminal that
	// believes it is coupled while its partner believes it is open is exactly the state the
	// three-state model exists to make unrepresentable.
	ours->SetCoupledTo( theirs );
	theirs->SetCoupledTo( ours );

	// The saved half, recorded on BOTH hosts.
	FACPRSavedCoupling mine;
	mine.OtherActor = otherActor;
	mine.LocalTerminalIndex = localIndex;
	mine.OtherTerminalIndex = otherIndex;
	host->AddSavedCoupling( mine );

	FACPRSavedCoupling theirRecord;
	theirRecord.OtherActor = actor;
	theirRecord.LocalTerminalIndex = otherIndex;
	theirRecord.OtherTerminalIndex = localIndex;
	otherHost->AddSavedCoupling( theirRecord );

	ACPR_LOG( Verbose, TERM,
		TEXT( "COUPLED %s.%s[%d] <-> %s.%s[%d] | cell=(%d,%d,%d) | "
		      "out (%d,%d,%d) vs (%d,%d,%d)" ),
		*actor->GetName(), *ours->GetName(), localIndex,
		otherActor ? *otherActor->GetName() : TEXT( "?" ), *theirs->GetName(), otherIndex,
		ours->GetQuantizedLocation().X, ours->GetQuantizedLocation().Y, ours->GetQuantizedLocation().Z,
		ours->GetQuantizedOutward().X, ours->GetQuantizedOutward().Y, ours->GetQuantizedOutward().Z,
		theirs->GetQuantizedOutward().X, theirs->GetQuantizedOutward().Y, theirs->GetQuantizedOutward().Z );

	// §12: "Creating a Coupling plays the vanilla electrical-connection cue". Every Coupling construction
	// makes comes through here; FACPRCouplingCue batches a frame's worth into one sound with a spark at
	// each point, and stays silent inside an FSuppressScope (the load paths).
	FACPRCouplingCue::Request( actor->GetWorld(), ours->GetComponentLocation(), ours->GetOutwardAxis(),
		FString::Printf( TEXT( "%s.%s<->%s.%s" ), *actor->GetName(), *ours->GetName(),
			otherActor ? *otherActor->GetName() : TEXT( "?" ), *theirs->GetName() ) );
}

bool FACPRCoupling::SavedTerminalFrame( const AActor* actor, const IACPRTerminalHost* host, uint8 index,
                                        FVector& out_location, FVector& out_outward )
{
	FTransform local;
	if( !actor || !host || !host->GetTerminalLocalFrame( index, local ) )
	{
		return false;
	}
	const FTransform world = local * actor->GetActorTransform();
	out_location = world.GetLocation();
	out_outward = world.GetUnitAxis( EAxis::X );
	return true;
}

void FACPRCoupling::RestoreSaved( AActor* actor, IACPRTerminalHost* host )
{
	// Collected rather than removed in place, because the loop below is iterating the same array.
	TArray< int32 > dropped;
	TArray< uint8 > reresolve;
	int32 restored = 0;
	int32 alreadyDone = 0;
	int32 recordIndex = -1;

	const AFGBuildableBlueprintDesigner* ourSpace = FACPRSpace::OfActor( actor );

	for( const FACPRSavedCoupling& record : host->GetSavedCouplings() )
	{
		++recordIndex;
		UACPRTerminalComponent* ours = host->GetTerminalAtIndex( record.LocalTerminalIndex );
		AActor* otherActor = record.OtherActor.Get();

		if( !ours || !IsValid( otherActor ) )
		{
			// §6.3 allows "conservative save repair" to drop an invalid edge before it is
			// player-visible. Dropping it silently is what would be wrong.
			ACPR_LOG( Warning, TERM,
				TEXT( "%s could not restore a saved Coupling: local terminal %d -> %s "
				      "terminal %d. One end no longer exists; the edge is dropped." ),
				*actor->GetName(), record.LocalTerminalIndex,
				otherActor ? *otherActor->GetName() : TEXT( "<null>" ),
				record.OtherTerminalIndex );
			dropped.Add( recordIndex );
			continue;
		}

		// Another world: the one case that is re-resolved rather than simply dropped — see the header.
		if( otherActor->GetWorld() != actor->GetWorld() )
		{
			ACPR_LOG( Display, REPAIR,
				TEXT( "%s.%s[%d] saved coupling names %s[%d] in ANOTHER world (%s, ours %s) "
				      "— record dropped, terminal re-resolved by coincidence" ),
				*actor->GetName(), *ours->GetName(), record.LocalTerminalIndex,
				*otherActor->GetName(), record.OtherTerminalIndex,
				otherActor->GetWorld() ? *otherActor->GetWorld()->GetName() : TEXT( "<none>" ),
				actor->GetWorld() ? *actor->GetWorld()->GetName() : TEXT( "<none>" ) );

			dropped.Add( recordIndex );
			reresolve.AddUnique( record.LocalTerminalIndex );
			continue;
		}

		IACPRTerminalHost* otherHost = Cast< IACPRTerminalHost >( otherActor );
		UACPRTerminalComponent* theirs = otherHost
			? otherHost->GetTerminalAtIndex( record.OtherTerminalIndex )
			: nullptr;

		if( !theirs )
		{
			ACPR_LOG( Warning, TERM,
				TEXT( "%s could not restore a saved Coupling: local terminal %d -> %s "
				      "terminal %d, which is not a terminal of that actor. The edge is dropped." ),
				*actor->GetName(), record.LocalTerminalIndex,
				*otherActor->GetName(), record.OtherTerminalIndex );
			dropped.Add( recordIndex );
			continue;
		}

		// The partner may have done this already: whichever host begins play first completes the pair
		// on both sides (below), so the second finds its half in place. There is no ordering rule.
		if( ours->GetCoupledTo() == theirs && theirs->GetCoupledTo() == ours )
		{
			++alreadyDone;
			continue;
		}

		// The geometry, from saved state — not from component transforms, which the partner's BeginPlay
		// may not have placed yet. GetTerminalLocalFrame restates each host's placement rule from the
		// fields it saves (the Rail's length, the Junction's half extent, the Outlet's mount); composed
		// with the actor transform it is where the terminal WILL be, whatever has run.
		FVector ourLocation, ourOutward, theirLocation, theirOutward;
		const bool haveOurs = SavedTerminalFrame( actor, host, record.LocalTerminalIndex, ourLocation, ourOutward );
		const bool haveTheirs = SavedTerminalFrame( otherActor, otherHost, record.OtherTerminalIndex, theirLocation, theirOutward );

		const AFGBuildableBlueprintDesigner* theirSpace = FACPRSpace::OfActor( otherActor );
		const double distance = ( haveOurs && haveTheirs ) ? FVector::Dist( ourLocation, theirLocation ) : -1.0;
		const double opposition = ( haveOurs && haveTheirs )
			? FVector::DotProduct( ourOutward.GetSafeNormal(), theirOutward.GetSafeNormal() ) : 0.0;

		const bool reciprocal = otherHost->GetSavedCouplings().ContainsByPredicate(
			[ actor, &record ]( const FACPRSavedCoupling& mirror )
			{
				return mirror.OtherActor == actor
					&& mirror.LocalTerminalIndex == record.OtherTerminalIndex
					&& mirror.OtherTerminalIndex == record.LocalTerminalIndex;
			} );

		const UACPRTerminalComponent* oursNow = ours->GetCoupledTo();
		const UACPRTerminalComponent* theirsNow = theirs->GetCoupledTo();

		FString reason;

		if( !haveOurs || !haveTheirs )
		{
			reason = TEXT( "no saved geometry for one end" );
		}
		else if( ourSpace != theirSpace )
		{
			reason = FString::Printf( TEXT( "different build spaces (%s vs %s) - 10.1" ),
				*FACPRSpace::Describe( ourSpace ), *FACPRSpace::Describe( theirSpace ) );
		}
		else if( distance > RepairCoincidenceTolerance )
		{
			reason = FString::Printf( TEXT( "not coincident (%.1f uu apart)" ), distance );
		}
		else if( opposition > -0.99 )
		{
			reason = FString::Printf( TEXT( "not opposing (dot=%.3f)" ), opposition );
		}
		else if( !reciprocal )
		{
			reason = TEXT( "the other side holds no mirror record" );
		}
		else if( ours->GetTerminalState() == EACPRTerminalState::Capped ||
		         theirs->GetTerminalState() == EACPRTerminalState::Capped )
		{
			reason = TEXT( "a capped terminal cannot be coupled (invariant 4)" );
		}
		else if( oursNow && oursNow != theirs )
		{
			reason = FString::Printf( TEXT( "ours is already coupled to %s (invariant 4)" ),
				*DescribeState( ours ) );
		}
		else if( theirsNow && theirsNow != ours )
		{
			reason = FString::Printf( TEXT( "theirs is already coupled to %s (invariant 4)" ),
				*DescribeState( theirs ) );
		}

		if( !reason.IsEmpty() )
		{
			// Reciprocally: the partner's mirror record goes, and the hosts' hidden edge —
			// which vanilla restored on its own from the connection's SaveGame mHiddenConnections
			// (FGCircuitConnectionComponent.h:192) — goes with it if this was the last pair between them.
			// Neither end is Coupled (a half-formed pair was refused above), so only records and the edge.
			if( otherHost )
			{
				otherHost->GetMutableSavedCouplings().RemoveAll(
					[ actor, &record ]( const FACPRSavedCoupling& mirror )
					{
						return mirror.OtherActor == actor && mirror.LocalTerminalIndex == record.OtherTerminalIndex;
					} );
			}
			bool hadEdge = false;
			UACPRPowerConnectionComponent* ourPower = host->GetPowerConnection();
			UACPRPowerConnectionComponent* theirPower = otherHost ? otherHost->GetPowerConnection() : nullptr;
			if( ourPower && theirPower && ourPower->HasHiddenConnection( theirPower )
			    && CountCouplingsBetween( host, otherActor ) == 0 )
			{
				ourPower->RemoveHiddenConnection( theirPower );
				hadEdge = true;
			}

			ACPR_LOG( Display, REPAIR,
				TEXT( "DROPPED %s.%s[%d] <-> %s.%s[%d] | %s | hiddenEdgeRemoved=%d" ),
				*actor->GetName(), *ours->GetName(), record.LocalTerminalIndex,
				*otherActor->GetName(), *theirs->GetName(), record.OtherTerminalIndex,
				*reason, hadEdge ? 1 : 0 );

			dropped.Add( recordIndex );
			continue;
		}

		UACPRPowerConnectionComponent* ourPower = host->GetPowerConnection();
		UACPRPowerConnectionComponent* theirPower = otherHost->GetPowerConnection();
		if( !VerifyPowerCircuitSafe( actor, ourPower, TEXT( "restored coupling, our side" ) ) ||
		    !VerifyPowerCircuitSafe( actor, theirPower, TEXT( "restored coupling, their side" ) ) )
		{
			continue;
		}

		// Both sides, here. The partner's terminal and connection are CDO subobjects and exist whether or
		// not its BeginPlay has run; its record will find this pair already complete.
		if( !ourPower->HasHiddenConnection( theirPower ) )
		{
			ourPower->AddHiddenConnection( theirPower );
		}
		ours->SetCoupledTo( theirs );
		theirs->SetCoupledTo( ours );
		++restored;

		ACPR_LOG( Verbose, TERM,
			TEXT( "RESTORED %s.%s[%d] <-> %s.%s[%d]" ),
			*actor->GetName(), *ours->GetName(), record.LocalTerminalIndex,
			*otherActor->GetName(), *theirs->GetName(), record.OtherTerminalIndex );
	}

	// Back to front, so the earlier indices stay valid as entries are removed.
	for( int32 i = dropped.Num() - 1; i >= 0; --i )
	{
		host->GetMutableSavedCouplings().RemoveAt( dropped[ i ] );
	}

	// Only now, with the dropped records gone: a terminal whose record pointed into another world is
	// asked §6.1's question once. It is still refused by every rule a hand-built terminal would be —
	// space, opposition, ambiguity, state — so nothing here can couple what construction would not.
	// Silent: this is a restore finding its partner again, not a Coupling construction made.
	const FACPRCouplingCue::FSuppressScope quiet;

	for( const uint8 index : reresolve )
	{
		UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( index );
		TryCoupleTerminal( actor, host, terminal, index );

		ACPR_LOG( Display, REPAIR,
			TEXT( "%s terminal %d re-resolved by coincidence -> %s" ),
			*actor->GetName(), index, *DescribeState( terminal ) );
	}

	if( dropped.Num() > 0 )
	{
		ACPR_LOG( Display, REPAIR,
			TEXT( "%s load replay | restored=%d alreadyDone=%d dropped=%d reresolved=%d" ),
			*actor->GetName(), restored, alreadyDone, dropped.Num(), reresolve.Num() );
	}
}

void FACPRCoupling::ScrubStrayHiddenEdges( AActor* actor, IACPRTerminalHost* host )
{
	if( !IsValid( actor ) || !host || !actor->HasAuthority() )
	{
		return;
	}

	UACPRPowerConnectionComponent* ours = host->GetPowerConnection();
	if( !ours )
	{
		return;
	}

	// A copy, because RemoveHiddenConnection edits the array being read. GetHiddenConnections is
	// FORCEINLINE (FGCircuitConnectionComponent.h:66), so this generates no import.
	TArray< UFGCircuitConnectionComponent* > edges;
	ours->GetHiddenConnections( edges );

	for( UFGCircuitConnectionComponent* edge : edges )
	{
		UACPRPowerConnectionComponent* theirs = Cast< UACPRPowerConnectionComponent >( edge );
		if( !IsValid( theirs ) )
		{
			continue;  // Not ours (a vanilla hidden connection, if any ever appears) — never touched.
		}

		AActor* farOwner = theirs->GetOwner();
		if( !farOwner || farOwner == actor )
		{
			continue;
		}

		IACPRTerminalHost* farHost = Cast< IACPRTerminalHost >( farOwner );
		if( !farHost )
		{
			continue;
		}

		// The two legitimate reasons two hosts' connections share an edge: a coupling between them
		// (one edge for all couplings between the pair), and §8.1's body bond — an Outlet
		// mounted on a Rail body is joined to that Rail with no terminal involved.
		if( CountCouplingsBetween( host, farOwner ) > 0 )
		{
			continue;
		}
		const AACPROutlet* outletHere = Cast< AACPROutlet >( actor );
		const AACPROutlet* outletThere = Cast< AACPROutlet >( farOwner );
		if( ( outletHere && outletHere->IsBodyBondedTo( farOwner ) ) ||
		    ( outletThere && outletThere->IsBodyBondedTo( actor ) ) )
		{
			continue;
		}

		ours->RemoveHiddenConnection( theirs );

		ACPR_LOG( Display, REPAIR,
			TEXT( "%s stray hidden edge to %s removed | no coupling between them | %s vs %s | sameWorld=%d" ),
			*actor->GetName(), *farOwner->GetName(),
			*FACPRSpace::Describe( FACPRSpace::OfActor( actor ) ),
			*FACPRSpace::Describe( FACPRSpace::OfActor( farOwner ) ),
			( farOwner->GetWorld() == actor->GetWorld() ) ? 1 : 0 );
	}
}

FString FACPRCoupling::DescribeState( const UACPRTerminalComponent* terminal )
{
	if( !terminal )
	{
		return TEXT( "<null>" );
	}

	switch( terminal->GetTerminalState() )
	{
		case EACPRTerminalState::Open:   return TEXT( "open" );
		case EACPRTerminalState::Capped: return TEXT( "capped" );
		default: break;
	}

	const UACPRTerminalComponent* partner = terminal->GetCoupledTo();
	const AActor* owner = partner ? partner->GetOwner() : nullptr;
	const IACPRTerminalHost* ownerHost = owner ? Cast< IACPRTerminalHost >( const_cast< AActor* >( owner ) ) : nullptr;
	const int32 index = ownerHost ? ownerHost->GetIndexOfTerminal( partner ) : INDEX_NONE;

	return FString::Printf( TEXT( "coupled>%s[%d]" ), owner ? *owner->GetName() : TEXT( "?" ), index );
}

FString FACPRCoupling::DescribeRecords( const AActor* actor, const IACPRTerminalHost* host )
{
	if( !actor || !host )
	{
		return TEXT( "[ ]" );
	}

	FString out = TEXT( "[ " );

	for( const FACPRSavedCoupling& record : host->GetSavedCouplings() )
	{
		const AActor* other = record.OtherActor.Get();

		const TCHAR* where = !other ? TEXT( "null" )
			: ( other->GetWorld() == actor->GetWorld() ? TEXT( "same" ) : TEXT( "OTHER-WORLD" ) );

		out += FString::Printf( TEXT( "%d->%s[%d] %s  " ),
			record.LocalTerminalIndex,
			other ? *other->GetName() : TEXT( "<null>" ),
			record.OtherTerminalIndex,
			where );
	}

	out += TEXT( "]" );
	return out;
}

void FACPRCoupling::LogHostSummary( AActor* actor, const IACPRTerminalHost* host, const TCHAR* tag,
                                    const TCHAR* stage, const TCHAR* blueprintState )
{
	if( !actor || !host )
	{
		return;
	}

	FString states;
	const int32 count = host->GetTerminalCount();
	for( int32 i = 0; i < count; ++i )
	{
		const UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( static_cast< uint8 >( i ) );

		// Private interfaces are marked, because an Outlet's one terminal reading "coupled" is its
		// mount on the host, not a continuation — the reader should not have to remember that.
		states += FString::Printf( TEXT( "%d:%s%s " ), i,
			*DescribeState( terminal ),
			( terminal && terminal->IsPrivateInterface() ) ? TEXT( "(mount)" ) : TEXT( "" ) );
	}

	// GetBlueprintProxy is FORCEINLINE on AFGBuildable (FGBuildable.h:537): no import.
	const AFGBuildable* buildable = Cast< AFGBuildable >( actor );
	const AFGBlueprintProxy* proxy = buildable ? buildable->GetBlueprintProxy() : nullptr;

	ACPR_LOG_TAGGED( Verbose, tag,
		TEXT( "%s %s | %s| fromSave=%d bp=%s space=%s proxy=%s | at %s" ),
		*actor->GetName(), stage,
		*states,
		host->WasLoadedFromSave() ? 1 : 0,
		blueprintState,
		*FACPRSpace::Describe( FACPRSpace::OfActor( actor ) ),
		proxy ? *proxy->GetName() : TEXT( "-" ),
		*actor->GetActorLocation().ToString() );
}
