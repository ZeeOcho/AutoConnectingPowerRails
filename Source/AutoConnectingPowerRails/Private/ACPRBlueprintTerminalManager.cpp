// Auto-Connecting Power Rails — §10's blueprint open-connection manager: generated bridge Rails.
//
// Vanilla surface used here, each checked against the header and classified (only a non-virtual,
// non-inline, non-reflected export is a load risk, and every one below is either not that, or already
// proven in shipping by Vertical Conveyor Auto-Connect):
//
//   AFGHologram::SpawnChildHologramFromRecipe   FGHologram.h:98    static plain export — VCAC ships it
//   AFGHologram::FindChildHologramByName        FGHologram.h:119   FORCEINLINE
//   AFGHologram::SetDisabled / IsDisabled       FGHologram.h:377/380 plain exports — VCAC ships both
//   AFGHologram::CanConstruct                   FGHologram.h:320   UFUNCTION — VCAC ships it
//   AFGHologram::Construct                      FGHologram.h:331   virtual
//   AFGHologram::GetCost                        FGHologram.h:360   virtual
//   AFGHologram::SetPlacementMaterialState      FGHologram.h:285   virtual
//   AFGHologram::SetShouldSpawnChildHolograms   FGHologram.h:431   inline
//   AFGHologram::mBlueprintDesigner / mConstructDisqualifiers  FGHologram.h:776/772  protected FIELDS, read
//       through the AccessTransformers Friend on AFGBlueprintHologram (friend of the derived class, object
//       of the derived class — [class.protected])
//   AFGBlueprintHologram::mBuildableToNewRoot   FGBlueprintHologram.h:84   public field
//   AFGBuildable::GetBuiltWithRecipe            FGBuildable.h:328  FORCEINLINE
//   AFGBuildable::GetBlueprintProxy / SetBlueprintProxy  FGBuildable.h:537/603  FORCEINLINE / inline
//   AFGBuildable::GetCustomizationData_Native   FGBuildable.h:242  inline (const)
//   IFGColorInterface::SetCustomizationData_Native  FGColorInterface.h:23  pure virtual — called through the
//       interface pointer, so a vtable call
//   AFGBlueprintProxy::GetBuildables            FGBlueprintProxy.h:74  inline — read only (the private
//       mBuildables is never written; a bridge vanilla did not join is reported by the audit)
//   UFGRecipe::GetProducts() const              FGRecipe.h:126     FORCEINLINE
//   UFGBuildingDescriptor::GetBuildableClass    FGBuildingDescriptor.h:26  UFUNCTION

#include "ACPRBlueprintTerminalManager.h"

#include "ACPRCap.h"
#include "ACPRDesignerSpace.h"
#include "ACPRJunction.h"
#include "ACPROutlet.h"
#include "ACPRPowerConnectionComponent.h"
#include "ACPRRail.h"
#include "ACPRRailHologram.h"
#include "ACPRTerminalComponent.h"
#include "ACPRTerminalHost.h"
#include "ACPRTerminalRegistry.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FGBlueprintProxy.h"
#include "FGColorInterface.h"
#include "FGConstructDisqualifier.h"
#include "FGFactorySettings.h"
#include "FGGlobalSettings.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "FGRecipe.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Hologram/FGHologram.h"
#include "ItemAmount.h"
#include "Resources/FGBuildingDescriptor.h"
#include "TimerManager.h"
#include "UObject/UObjectArray.h"
#include "UObject/SoftObjectPtr.h"

/** §10.4 step 2 and the coincident test's territory: anything nearer than this along the ray is §6.1's. */
static constexpr double ACPRRayMinT = 0.5;

/** "Every distinct terminal occluding at that distance" — half the 1 uu registration lattice. */
static constexpr double ACPRRayTieT = 0.5;

/** Lateral reach of the registry pre-query: the largest disc radius there is, with room to spare. */
static constexpr double ACPRRayQueryMargin = 60.0;

/** A ray this close to parallel with a disc's plane never enters it (it would graze it edge-on). */
static constexpr double ACPRRayParallel = 1e-3;

/**
 * The Rail recipe's asset path, from Scripts/acpr_buildables.py (RECIPE_DIR + "/Recipe_PowerRail"). Only the
 * FALLBACK — a blueprint containing any Rail hands over the recipe that Rail was built with.
 */
static const TCHAR* ACPRRailRecipePath = TEXT( "/AutoConnectingPowerRails/Recipes/Recipe_PowerRail.Recipe_PowerRail_C" );

/** §11 / §10.4: occlusion disc radius, "face width - 0.1 m" halved. */
static double ACPRDiscRadiusFor( const AActor* owner )
{
	if( const AACPRJunction* junction = Cast< AACPRJunction >( owner ) )
	{
		return FMath::Max( 1.0, static_cast< double >( junction->mHalfExtent ) - 5.0 );
	}

	if( owner && owner->IsA< AACPRRail >() )
	{
		return AACPRRail::OcclusionDiscRadius;
	}

	return 20.0;
}

/** Ray p + t·d against a disc (centre c, normal n, radius r). True and t when it enters, within (min, maxT]. */
static bool ACPRIntersectDisc( const FVector& p, const FVector& d, const FVector& c, const FVector& n,
                               double r, double maxT, double& out_t )
{
	const FVector normal = n.GetSafeNormal();
	const double denom = FVector::DotProduct( d, normal );
	if( FMath::Abs( denom ) < ACPRRayParallel )
	{
		return false;
	}

	const double t = FVector::DotProduct( c - p, normal ) / denom;
	if( t < ACPRRayMinT || t > maxT )
	{
		return false;
	}

	const FVector hit = p + d * t;
	if( FVector::DistSquared( hit, c ) > r * r )
	{
		return false;
	}

	out_t = t;
	return true;
}

static FIntVector ACPRNegate( const FIntVector& v )
{
	return FIntVector( -v.X, -v.Y, -v.Z );
}

// §10.6's link icon, tunable in game without a rebuild. The helper vanilla uses to place its own icon is not
// callable (see the header), so its scale and offset are not known; these start at the neutral values and the
// first icon's log line prints the mesh bounds they apply to.
static TAutoConsoleVariable< int32 > CVarACPRLinkIconMode(
	TEXT( "acpr.LinkIconMode" ), 1,
	TEXT( "Power Rails: how the blue two-link icon is drawn on accepted blueprint auto-connect pairs. "
	      "0 = none, 1 = vanilla's own icon through its connection-state delegate (default), "
	      "2 = our own mesh, if vanilla draws none." ),
	ECVF_Default );

static TAutoConsoleVariable< float > CVarACPRLinkIconScale(
	TEXT( "acpr.LinkIconScale" ), 1.0f,
	TEXT( "Power Rails: uniform scale of our own blue two-link icon (acpr.LinkIconMode 3)." ),
	ECVF_Default );

static TAutoConsoleVariable< float > CVarACPRLinkIconOffset(
	TEXT( "acpr.LinkIconOffset" ), 0.0f,
	TEXT( "Power Rails: how far (uu) our own icon sits outward from the blueprint terminal (acpr.LinkIconMode 3)." ),
	ECVF_Default );

/** Parent blueprint hologram -> its manager. Game thread only; entries live exactly as long as the manager. */
static TMap< const AFGHologram*, FACPRBlueprintTerminalManager* >& ACPRManagersByHologram()
{
	static TMap< const AFGHologram*, FACPRBlueprintTerminalManager* > managers;
	return managers;
}

FACPRBlueprintTerminalManager::FACPRBlueprintTerminalManager( AFGBlueprintHologram* blueprintHologram )
	: FGBlueprintOpenConnectionManagerBase( blueprintHologram )
{
	mHologramName = blueprintHologram ? blueprintHologram->GetName() : FString( TEXT( "?" ) );

	if( blueprintHologram )
	{
		mRegistryKey = blueprintHologram;
		ACPRManagersByHologram().Add( mRegistryKey, this );
	}
}

void FACPRBlueprintTerminalManager::OnBuildModeChanged( const AFGBlueprintHologram* hologram,
                                                        TSubclassOf< UFGHologramBuildModeDescriptor > buildMode )
{
	FACPRBlueprintTerminalManager* const* manager = ACPRManagersByHologram().Find( hologram );
	if( !manager || !*manager || !IsValid( ( *manager )->GetHologram() ) )
	{
		return;
	}

	FACPRBlueprintTerminalManager& self = **manager;
	int32 shown = 0;
	for( const FState& state : self.mStates )
	{
		shown += state.BridgeEnabled ? 1 : 0;
	}

	ACPR_LOG( Display, BP,
		TEXT( "%s build mode -> %s | %d bridge(s) shown, resetting; the next update rebuilds" ),
		*self.mHologramName, buildMode ? *buildMode->GetName() : TEXT( "<none>" ), shown );

	self.ResetAutomaticConnections();
}

FACPRBlueprintTerminalManager::~FACPRBlueprintTerminalManager()
{
	if( mRegistryKey )
	{
		FACPRBlueprintTerminalManager* const* registered = ACPRManagersByHologram().Find( mRegistryKey );
		if( registered && *registered == this )
		{
			ACPRManagersByHologram().Remove( mRegistryKey );
		}
	}

	FString bySource;
	for( const TPair< FString, int32 >& entry : mEvaluationsBy )
	{
		bySource += FString::Printf( TEXT( "%s=%d " ), *entry.Key, entry.Value );
	}

	// Display: one line per blueprint hologram. constructs=0 on a hologram that was built means vanilla did
	// not route that placement through this manager — the Default modes.
	ACPR_LOG( Display, BP,
		TEXT( "manager on %s destroyed | updates=%d (while locked %d) evaluations=%d [ %s] snapQueries=%d "
		      "resets=%d remaps=%d constructs=%d | bridgesSpawned=%d latched=%d blockedClicks=%d built=%d "
		      "| icons created=%d shows=%d materialReapplied=%d | broadcasts=%d previewDuplicates=%d iconMode=%d | uobjects=%d" ),
		*mHologramName, mUpdateCount, mUpdatesWhileLocked, mEvaluationCount, *bySource, mSnapQueryCount,
		mResetCount, mRemapCount, mConstructCount, mBridgesSpawned, mLatchCount, mBlockedClicks, mBridgesBuilt,
		mIconsCreated, mIconShows, mIconMaterialReapplied,
		mBroadcasts, mDuplicatesMade, static_cast< int32 >( CurrentIconMode() ),
		GUObjectArray.GetObjectArrayNumMinusAvailable() );
}

void FACPRBlueprintTerminalManager::RegisterNearbyActor( AActor* actor )
{
	// Fed by the blueprint hologram's clearance overlaps. §10.4 asks the terminal registry, which knows
	// every registered terminal, rather than the overlap broadphase; kept for the log only.
	if( !IsValid( actor ) || !Cast< IACPRTerminalHost >( actor ) )
	{
		return;
	}

	const int32 before = mNearby.Num();
	mNearby.AddUnique( actor );

	if( mNearby.Num() != before && mNearbyLogBudget > 0 )
	{
		--mNearbyLogBudget;
		ACPR_LOG( Verbose, BP,
			TEXT( "%s nearby + %s | space=%s | nearby hosts=%d" ),
			*mHologramName, *actor->GetName(),
			*FACPRSpace::Describe( FACPRSpace::OfActor( actor ) ), mNearby.Num() );
	}
}

void FACPRBlueprintTerminalManager::UnregisterNearbyActor( AActor* actor )
{
	mNearby.RemoveAll( [ actor ]( const TWeakObjectPtr< AActor >& weak )
	{
		return !weak.IsValid() || weak.Get() == actor;
	} );
}

void FACPRBlueprintTerminalManager::Initialize( TArray< AFGBuildable* > buildables )
{
	FACPRCostScope cost( EACPRCost::ManagerInitialize );
	// Bridges already spawned stay registered as children under their deterministic names (EnsureBridge
	// finds them again); only their state here is dropped. Disable them so a re-initialized blueprint does
	// not carry a stale preview.
	for( FState& state : mStates )
	{
		DisableBridge( state );
	}

	// Same for vanilla's icons: a re-initialised blueprint must not leave one lit for a state that is gone.
	for( int32 i = 0; i < mStates.Num(); ++i )
	{
		mStates[ i ].Kind = EVerdict::None;
		BroadcastConnectionState( i, mStates[ i ] );
	}

	// The icon components are kept (mIcons is indexed by state and reused); only their visibility goes.
	HideAllIcons();

	mBPTerminals.Reset();
	mStates.Reset();
	mPlaced.Reset();
	mHasEvaluated = false;

	// Indexed by state, so it means nothing once the states are gone. The components stay on the hologram and
	// are found again through vanilla's own mDuplicateConnectionToOriginalMap.
	mBroadcastDuplicates.Reset();


	if( AFGBlueprintHologram* hologram = GetHologram() )
	{
		mHologramName = hologram->GetName();
	}

	// Membership first. A saved record is "internal" when its other actor is another member of this
	// blueprint, and "outside" otherwise.
	TSet< const AActor* > members;
	for( const AFGBuildable* buildable : buildables )
	{
		if( buildable )
		{
			members.Add( buildable );
		}
	}

	// Occupants second: a terminal a Cap or a terminal-mounted Outlet sits on inside the blueprint is not a
	// boundary terminal. Both save their host and terminal index, so this reads saved state only.
	TMap< const AActor*, uint32 > occupiedMask;
	int32 occupantsOutside = 0;

	for( const AFGBuildable* buildable : buildables )
	{
		const AActor* host = nullptr;
		int32 index = INDEX_NONE;

		if( const AACPRCap* cap = Cast< AACPRCap >( buildable ) )
		{
			host = cap->GetHostActor();
			index = cap->GetHostTerminalIndex();
		}
		else if( const AACPROutlet* outlet = Cast< AACPROutlet >( buildable ) )
		{
			if( outlet->GetHostMode() == EACPROutletHostMode::Terminal )
			{
				host = outlet->GetHostActor();
				index = outlet->GetHostTerminalIndex();
			}
		}

		if( index == INDEX_NONE )
		{
			continue;
		}

		if( host && members.Contains( host ) && index < 32 )
		{
			occupiedMask.FindOrAdd( host ) |= ( 1u << index );
		}
		else
		{
			++occupantsOutside;
		}
	}

	int32 hosts = 0;
	int32 privateCount = 0;
	int32 internalCount = 0;
	int32 occupiedCount = 0;
	int32 noFrameCount = 0;
	int32 outsideRecords = 0;

	for( int32 buildableIndex = 0; buildableIndex < buildables.Num(); ++buildableIndex )
	{
		AFGBuildable* buildable = buildables[ buildableIndex ];
		const IACPRTerminalHost* host = buildable ? Cast< IACPRTerminalHost >( buildable ) : nullptr;
		if( !host )
		{
			continue;
		}

		++hosts;

		FString records;
		for( const FACPRSavedCoupling& record : host->GetSavedCouplings() )
		{
			const AActor* other = record.OtherActor.Get();
			const bool inside = other && members.Contains( other );
			if( !inside )
			{
				++outsideRecords;
			}

			records += FString::Printf( TEXT( "%d->%s[%d] %s  " ),
				record.LocalTerminalIndex,
				other ? *other->GetName() : TEXT( "<null>" ),
				record.OtherTerminalIndex,
				!other ? TEXT( "null" ) : ( inside ? TEXT( "in-bp" ) : TEXT( "OUTSIDE" ) ) );
		}

		const double disc = ACPRDiscRadiusFor( buildable );
		const AACPRRail* rail = Cast< AACPRRail >( buildable );
		const bool isJunction = Cast< AACPRJunction >( buildable ) != nullptr;

		if( mInitLogBudget > 0 )
		{
			--mInitLogBudget;
			ACPR_LOG( Verbose, BP,
				TEXT( "%s init #%d %s | space=%s | disc=%.1f | recipe=%s | records=[ %s]" ),
				*mHologramName, buildableIndex, *buildable->GetName(),
				*FACPRSpace::Describe( host->GetHostDesigner() ), disc,
				*GetNameSafe( buildable->GetBuiltWithRecipe().Get() ), *records );
		}

		const uint32 mask = occupiedMask.FindRef( buildable );
		const int32 count = host->GetTerminalCount();

		for( int32 t = 0; t < count; ++t )
		{
			const uint8 index = static_cast< uint8 >( t );
			const UACPRTerminalComponent* terminal = host->GetTerminalAtIndex( index );

			// §4: the private mounting interface "is not a continuation terminal, not a blueprint candidate"
			// — and not a face, so it occludes nothing either.
			if( terminal && terminal->IsPrivateInterface() )
			{
				++privateCount;
				continue;
			}

			FBPTerminal entry;
			if( !host->GetTerminalLocalFrame( index, entry.LocalFrame ) )
			{
				++noFrameCount;
				continue;
			}

			entry.Buildable = buildable;
			entry.BuildableIndex = buildableIndex;
			entry.TerminalIndex = index;
			entry.DiscRadius = disc;

			// The point §6.1 derives the outward from, by the placed host's own rule (see FBPTerminal).
			if( rail )
			{
				FTransform other;
				if( host->GetTerminalLocalFrame( static_cast< uint8 >( 1 - FMath::Min( t, 1 ) ), other ) )
				{
					entry.ReferenceLocal = other.GetLocation();
					entry.HasReference = true;
				}
			}
			else if( isJunction )
			{
				entry.ReferenceLocal = FVector::ZeroVector;
				entry.HasReference = true;
			}

			const bool internal = host->GetSavedCouplings().ContainsByPredicate(
				[ index, &members ]( const FACPRSavedCoupling& record )
				{
					return record.LocalTerminalIndex == index && record.OtherActor
						&& members.Contains( record.OtherActor.Get() );
				} );

			const bool occupied = t < 32 && ( mask & ( 1u << t ) ) != 0;

			if( internal )
			{
				++internalCount;
			}
			else if( occupied )
			{
				++occupiedCount;
			}
			else
			{
				// §10.3's BP candidate.
				FState state;
				state.Terminal = mBPTerminals.Num();
				entry.State = mStates.Num();
				mStates.Add( state );
			}

			mBPTerminals.Add( entry );
		}
	}

	ResolveRailRecipe( buildables );
	BuildOccluderIndex();

	ACPR_LOG( Display, BP,
		TEXT( "%s initialized | buildables=%d hosts=%d | occluders=%d candidates=%d internal=%d "
		      "occupied=%d private=%d noFrame=%d | recordsPointingOutside=%d occupantsWithHostOutside=%d | "
		      "space=%s | uobjects=%d" ),
		*mHologramName, buildables.Num(), hosts,
		mBPTerminals.Num(), mStates.Num(), internalCount, occupiedCount, privateCount, noFrameCount,
		outsideRecords, occupantsOutside,
		*FACPRSpace::Describe( GetPlacementSpace() ),
		GUObjectArray.GetObjectArrayNumMinusAvailable() );

	DumpDuplicatedMeshes( TEXT( "initialize" ) );
	FixUpDuplicatedMeshes();
}

void FACPRBlueprintTerminalManager::FixUpDuplicatedMeshes()
{
	FACPRCostScope cost( EACPRCost::ManagerFixUp );
	const AFGBlueprintHologram* hologram = GetHologram();
	if( !hologram )
	{
		return;
	}

	int32 buildablesTouched = 0;
	int32 componentsTouched = 0;
	// Which engine calls the fix-up makes. Each is engine work (a mesh swap recreates render and physics
	// state; a material set dirties the render state), tens of microseconds per component, so a call that
	// would change nothing is skipped; these four counts say what the template copies differ in, per class.
	int32 meshSet = 0, materialSet = 0, transformSet = 0, visibilitySet = 0;
	// (class, component, what-was-set bits) -> how many. Bits: 1 mesh, 2 materials, 4 transform, 8 visibility.
	TMap< TTuple< FName, FName, uint8 >, int32 > byClass;
	FString sample;

	for( const TPair< TObjectPtr< AFGBuildable >, TObjectPtr< USceneComponent > >& pair : hologram->mBuildableToNewRoot )
	{
		const AFGBuildable* buildable = pair.Key.Get();
		USceneComponent* root = pair.Value.Get();
		if( !buildable || !root )
		{
			continue;
		}
		if( !buildable->IsA< AACPRRail >() && !buildable->IsA< AACPRJunction >()
		    && !buildable->IsA< AACPRCap >() && !buildable->IsA< AACPROutlet >() )
		{
			continue;
		}

		TArray< USceneComponent* > children;
		root->GetChildrenComponents( true, children );

		TArray< UStaticMeshComponent* > originals;
		buildable->GetComponents< UStaticMeshComponent >( originals );

		bool touched = false;
		for( const UStaticMeshComponent* original : originals )
		{
			if( !original )
			{
				continue;
			}
			for( USceneComponent* child : children )
			{
				UStaticMeshComponent* duplicate = Cast< UStaticMeshComponent >( child );
				if( !duplicate || duplicate->GetFName() != original->GetFName() )
				{
					continue;
				}

				// The hologram material vanilla applied to the template copy, kept for every slot of
				// the real mesh — which has more slots (cue, accent, power floors) than the template
				// it was applied to, and an uncovered slot would show a lit cue inside a hologram.
				UMaterialInterface* holo = duplicate->GetNumMaterials() > 0 ? duplicate->GetMaterial( 0 ) : nullptr;

				uint8 what = 0;
				if( duplicate->GetStaticMesh() != original->GetStaticMesh() )
				{
					duplicate->SetStaticMesh( original->GetStaticMesh() );
					++meshSet;
					what |= 1;
				}
				if( holo )
				{
					// One render-state dirty per component, not one per slot: SetMaterial marks the render
					// state dirty on every call, and a component has several slots. The override array is
					// UMeshComponent's own public slot table (what SetMaterial writes before marking the
					// render state dirty); writing it directly and marking once is the same result with
					// the per-call overhead paid once.
					const int32 slots = duplicate->GetNumMaterials();
					bool dirty = false;
					for( int32 slot = 0; slot < slots; ++slot )
					{
						if( duplicate->GetMaterial( slot ) != holo )
						{
							if( duplicate->OverrideMaterials.Num() < slots )
							{
								duplicate->OverrideMaterials.SetNum( slots );
							}
							duplicate->OverrideMaterials[ slot ] = holo;
							dirty = true;
							++materialSet;
							what |= 2;
						}
					}
					if( dirty )
					{
						duplicate->MarkRenderStateDirty();
					}
				}
				if( !duplicate->GetRelativeTransform().Equals( original->GetRelativeTransform(), 1e-3 ) )
				{
					duplicate->SetRelativeTransform( original->GetRelativeTransform() );
					++transformSet;
					what |= 4;
				}
				if( duplicate->IsVisible() != original->IsVisible() )
				{
					duplicate->SetVisibility( original->IsVisible() );
					++visibilitySet;
					what |= 8;
				}
				++componentsTouched;
				++byClass.FindOrAdd( MakeTuple( buildable->GetClass()->GetFName(), duplicate->GetFName(), what ) );
				touched = true;

				if( sample.Len() < 600 && ACPR_LOG_ACTIVE( Verbose ) )
				{
					sample += FString::Printf( TEXT( " | %s.%s -> %s rot=%s scale=%s vis=%d" ),
						*buildable->GetName(), *duplicate->GetName(),
						duplicate->GetStaticMesh() ? *duplicate->GetStaticMesh()->GetName() : TEXT( "<none>" ),
						*duplicate->GetRelativeRotation().ToString(),
						*duplicate->GetRelativeScale3D().ToString(),
						duplicate->IsVisible() ? 1 : 0 );
				}
				break;
			}
		}
		if( touched )
		{
			++buildablesTouched;
		}
	}

	// The per-component breakdown is built only when tracing; the summary line always prints.
	FString classes;
	for( const TPair< TTuple< FName, FName, uint8 >, int32 >& pair : byClass )
	{
		if( !FACPRTrace::Enabled() )
		{
			break;
		}
		const uint8 what = pair.Key.Get< 2 >();
		classes += FString::Printf( TEXT( " %s.%s%s%s%s%s%s x%d" ),
			*pair.Key.Get< 0 >().ToString(), *pair.Key.Get< 1 >().ToString(),
			( what & 1 ) ? TEXT( "+mesh" ) : TEXT( "" ),
			( what & 2 ) ? TEXT( "+materials" ) : TEXT( "" ),
			( what & 4 ) ? TEXT( "+transform" ) : TEXT( "" ),
			( what & 8 ) ? TEXT( "+visibility" ) : TEXT( "" ),
			what == 0 ? TEXT( " unchanged" ) : TEXT( "" ),
			pair.Value );
	}

	ACPR_LOG( Display, BP_DUP,
		TEXT( "%s fix-up | buildables=%d components=%d | set: mesh=%d materialSlots=%d "
		      "transform=%d visibility=%d | per component:%s" ),
		*mHologramName, buildablesTouched, componentsTouched,
		meshSet, materialSet, transformSet, visibilitySet, *classes );
	ACPR_LOG( Verbose, BP_DUP, TEXT( "%s fix-up sample:%s" ), *mHologramName, *sample );
}

void FACPRBlueprintTerminalManager::DumpDuplicatedMeshes( const TCHAR* when ) const
{
	// Thousands of lines of string building per blueprint spawn, for Verbose output: skipped entirely
	// unless Verbose is on.
	if( !ACPR_LOG_ACTIVE( Verbose ) )
	{
		return;
	}
	const AFGBlueprintHologram* hologram = GetHologram();
	if( !hologram )
	{
		return;
	}

	int32 railsShown = 0;
	for( const TPair< TObjectPtr< AFGBuildable >, TObjectPtr< USceneComponent > >& pair : hologram->mBuildableToNewRoot )
	{
		const AFGBuildable* buildable = pair.Key.Get();
		const USceneComponent* root = pair.Value.Get();
		if( !buildable || !root )
		{
			continue;
		}
		const bool isCap = buildable->IsA< AACPRCap >();
		const bool isRail = buildable->IsA< AACPRRail >();
		if( !isCap && !( isRail && railsShown < 1 ) )
		{
			continue;
		}
		if( isRail )
		{
			++railsShown;
		}

		TArray< USceneComponent* > children;
		root->GetChildrenComponents( true, children );

		FString list;
		for( const USceneComponent* child : children )
		{
			const UStaticMeshComponent* mesh = Cast< UStaticMeshComponent >( child );
			if( !mesh )
			{
				continue;
			}
			list += FString::Printf( TEXT( " | %s mesh=%s rot=%s scale=%s loc=%s vis=%d" ),
				*mesh->GetName(),
				mesh->GetStaticMesh() ? *mesh->GetStaticMesh()->GetName() : TEXT( "<none>" ),
				*mesh->GetRelativeRotation().ToString(),
				*mesh->GetRelativeScale3D().ToString(),
				*mesh->GetRelativeLocation().ToString(),
				mesh->IsVisible() ? 1 : 0 );
		}

		// And what the blueprint-world original holds right now, for the comparison.
		FString original;
		TArray< UStaticMeshComponent* > own;
		buildable->GetComponents< UStaticMeshComponent >( own );
		for( const UStaticMeshComponent* mesh : own )
		{
			original += FString::Printf( TEXT( " | %s mesh=%s rot=%s scale=%s" ),
				*mesh->GetName(),
				mesh->GetStaticMesh() ? *mesh->GetStaticMesh()->GetName() : TEXT( "<none>" ),
				*mesh->GetRelativeRotation().ToString(),
				*mesh->GetRelativeScale3D().ToString() );
		}

		ACPR_LOG( Verbose, BP_DUP,
			TEXT( "%s (%s) %s | root=%s rot=%s | duplicates(%d):%s || original:%s" ),
			*mHologramName, when, *buildable->GetName(),
			*root->GetName(), *root->GetRelativeRotation().ToString(),
			children.Num(), *list, *original );
	}
}

void FACPRBlueprintTerminalManager::ResolveRailRecipe( const TArray< AFGBuildable* >& buildables )
{
	// The recipe a bridge is built from. A blueprint Rail was built with it, and the blueprint carries
	// mBuiltWithRecipe (SaveGame on AFGBuildable) into its private world — so the Rail in the blueprint is
	// the authority. A blueprint of Junctions only has no Rail to ask, and falls back to the asset path.
	mRailRecipe = nullptr;
	mRailRecipeSource = TEXT( "none" );
	const AACPRRail* railCdo = nullptr;

	for( const AFGBuildable* buildable : buildables )
	{
		if( const AACPRRail* rail = Cast< AACPRRail >( buildable ) )
		{
			railCdo = Cast< AACPRRail >( rail->GetClass()->GetDefaultObject() );

			if( rail->GetBuiltWithRecipe() )
			{
				mRailRecipe = rail->GetBuiltWithRecipe();
				mRailRecipeSource = FString::Printf( TEXT( "blueprint Rail %s" ), *rail->GetName() );
				break;
			}
		}
	}

	if( !mRailRecipe )
	{
		// TSoftClassPtr::LoadSynchronous — the same call FGBuildable.h:735 and FGGameState.h:560 make inline.
		if( UClass* loaded = TSoftClassPtr< UFGRecipe >( FSoftObjectPath( ACPRRailRecipePath ) ).LoadSynchronous() )
		{
			mRailRecipe = loaded;
			mRailRecipeSource = TEXT( "asset path" );
		}
	}

	// The build class's §11 limits, through the recipe's product when the blueprint had no Rail to read
	// them from: UFGRecipe::GetProducts() const (FGRecipe.h:126, FORCEINLINE) and
	// UFGBuildingDescriptor::GetBuildableClass (FGBuildingDescriptor.h:26, UFUNCTION).
	if( !railCdo && mRailRecipe )
	{
		if( const UFGRecipe* recipe = mRailRecipe->GetDefaultObject< UFGRecipe >() )
		{
			for( const FItemAmount& product : recipe->GetProducts() )
			{
				if( product.ItemClass && product.ItemClass->IsChildOf( UFGBuildingDescriptor::StaticClass() ) )
				{
					const TSubclassOf< AFGBuildable > buildClass = UFGBuildingDescriptor::GetBuildableClass(
						TSubclassOf< UFGBuildingDescriptor >( product.ItemClass.Get() ) );
					if( buildClass )
					{
						railCdo = Cast< AACPRRail >( buildClass->GetDefaultObject() );
					}
				}
			}
		}
	}

	if( railCdo )
	{
		mRailMin = FMath::Max( 1.0, static_cast< double >( railCdo->GetSize() ) );
		mRailMax = FMath::Max( mRailMin, static_cast< double >( railCdo->GetMaxLength() ) );
		mRailPerCost = FMath::Max( 1.0, static_cast< double >( railCdo->GetLengthPerCost() ) );
	}

	ACPR_LOG( Display, BP,
		TEXT( "%s bridge recipe=%s (from %s) | buildClass=%s | length %.0f..%.0f uu, one cost unit per %.0f uu" ),
		*mHologramName, *GetNameSafe( mRailRecipe.Get() ), *mRailRecipeSource,
		railCdo ? *railCdo->GetClass()->GetName() : TEXT( "<unknown - defaults>" ),
		mRailMin, mRailMax, mRailPerCost );
}

AFGBuildableBlueprintDesigner* FACPRBlueprintTerminalManager::GetPlacementSpace() const
{
	// AFGHologram::mBlueprintDesigner (FGHologram.h:776, protected), named through AFGBlueprintHologram,
	// whose AccessTransformers Friend is this class. A blueprint placed inside a Designer targets that
	// Designer's terminals and nothing else (§10.1).
	const AFGBlueprintHologram* hologram = GetHologram();
	return hologram ? hologram->mBlueprintDesigner.Get() : nullptr;
}

bool FACPRBlueprintTerminalManager::GetBuildableTransform( const FBPTerminal& t, bool placed, FTransform& out_transform ) const
{
	if( placed )
	{
		const TWeakObjectPtr< AFGBuildable >* weak = mPlaced.Find( t.BuildableIndex );
		const AFGBuildable* buildable = weak ? weak->Get() : nullptr;
		if( !buildable )
		{
			return false;
		}

		out_transform = buildable->GetActorTransform();
		return true;
	}

	AFGBlueprintHologram* hologram = GetHologram();
	AFGBuildable* buildable = t.Buildable.Get();
	if( !hologram || !buildable )
	{
		return false;
	}

	// mBuildableToNewRoot is PUBLIC on AFGBlueprintHologram (FGBlueprintHologram.h:84): "the buildable
	// (which is instantiated in the blueprint world) to the new root component that represents it visually
	// in the game world". Vertical Conveyor Auto-Connect reads its endpoints the same way.
	const TObjectPtr< USceneComponent >* root = hologram->mBuildableToNewRoot.Find( buildable );
	if( !root || !IsValid( root->Get() ) )
	{
		return false;
	}

	out_transform = root->Get()->GetComponentTransform();
	return true;
}

bool FACPRBlueprintTerminalManager::ComputeFrame( const FBPTerminal& t, bool placed, FTerminalFrame& out_frame ) const
{
	FTransform buildable;
	if( !GetBuildableTransform( t, placed, buildable ) )
	{
		return false;
	}

	const FTransform world = t.LocalFrame * buildable;

	out_frame.P = world.GetLocation();
	out_frame.D = world.GetUnitAxis( EAxis::X );
	out_frame.Up = world.GetUnitAxis( EAxis::Z );
	out_frame.PQ = UACPRTerminalComponent::QuantizeLocation( out_frame.P );

	if( t.HasReference )
	{
		const FIntVector reference = UACPRTerminalComponent::QuantizeLocation(
			buildable.TransformPosition( t.ReferenceLocal ) );
		out_frame.OutQ = UACPRTerminalComponent::ReduceDirection( out_frame.PQ - reference );
	}
	else
	{
		out_frame.OutQ = UACPRTerminalComponent::ReduceDirection(
			UACPRTerminalComponent::QuantizeLocation( out_frame.P + out_frame.D * 100.0 ) - out_frame.PQ );
	}

	return true;
}

void FACPRBlueprintTerminalManager::BuildOccluderIndex()
{
	mOccluderLocal.Reset();
	mBuildableLocal.Reset();
	mOccluderGrid.Reset();
	mBodyValid = false;
	mBodyChecked = 0;

	// Body space: the first preview root's own transform. Every other buildable is expressed relative to it.
	FTransform body;
	bool haveBody = false;

	for( const FBPTerminal& t : mBPTerminals )
	{
		if( mBuildableLocal.Contains( t.BuildableIndex ) )
		{
			continue;
		}
		FTransform world;
		if( !GetBuildableTransform( t, false, world ) )
		{
			continue;
		}
		if( !haveBody )
		{
			body = world;
			haveBody = true;
		}
		// world = local * body  <=>  local = world * body^-1, which is what GetRelativeTransform computes.
		mBuildableLocal.Add( t.BuildableIndex, world.GetRelativeTransform( body ) );
	}

	mOccluderLocal.SetNum( mBPTerminals.Num() );
	int32 indexed = 0;
	for( int32 i = 0; i < mBPTerminals.Num(); ++i )
	{
		const FTransform* local = mBuildableLocal.Find( mBPTerminals[ i ].BuildableIndex );
		if( !local )
		{
			continue;  // No preview root: not an occluder, exactly as ComputeFrame would have failed for it.
		}
		const FTransform frame = mBPTerminals[ i ].LocalFrame * *local;
		mOccluderLocal[ i ].P = frame.GetLocation();
		mOccluderLocal[ i ].D = frame.GetUnitAxis( EAxis::X );
		mOccluderGrid.Add( mOccluderLocal[ i ].P, i );
		++indexed;
	}

	ACPR_LOG( Display, BP,
		TEXT( "%s occluder index | occluders=%d indexed=%d buildables=%d buckets=%d (%.0f uu)" ),
		*mHologramName, mBPTerminals.Num(), indexed, mBuildableLocal.Num(),
		mOccluderGrid.GetBucketCount(), mOccluderGrid.GetBucketSize() );
}

bool FACPRBlueprintTerminalManager::RefreshBody( bool placed ) const
{
	mBodyValid = false;
	mBodyPlaced = placed;

	// Any one buildable with a known local transform and a transform now: world = local * body.
	const FTransform* referenceLocal = nullptr;
	FTransform referenceWorld;
	for( const FBPTerminal& t : mBPTerminals )
	{
		const FTransform* local = mBuildableLocal.Find( t.BuildableIndex );
		if( local && GetBuildableTransform( t, placed, referenceWorld ) )
		{
			referenceLocal = local;
			break;
		}
	}
	if( !referenceLocal )
	{
		return false;
	}

	mBodyToWorld = referenceLocal->Inverse() * referenceWorld;
	mBodyValid = true;

	// The rigidity check, once per mode: the design rests on it, so it is measured, not assumed.
	const uint8 bit = placed ? 2 : 1;
	if( ( mBodyChecked & bit ) == 0 )
	{
		mBodyChecked |= bit;

		int32 checked = 0, disagree = 0;
		double worst = 0.0, worstTurn = 0.0;
		TSet< int32 > seen;
		for( const FBPTerminal& t : mBPTerminals )
		{
			if( seen.Contains( t.BuildableIndex ) )
			{
				continue;
			}
			seen.Add( t.BuildableIndex );

			const FTransform* local = mBuildableLocal.Find( t.BuildableIndex );
			FTransform actual;
			if( !local || !GetBuildableTransform( t, placed, actual ) )
			{
				continue;
			}
			++checked;
			const FTransform predicted = *local * mBodyToWorld;
			const double off = FVector::Dist( predicted.GetLocation(), actual.GetLocation() );
			const double turned = FMath::RadiansToDegrees( predicted.GetRotation().AngularDistance( actual.GetRotation() ) );
			worst = FMath::Max( worst, off );
			worstTurn = FMath::Max( worstTurn, turned );
			if( off > 1.0 || turned > 0.1 )
			{
				++disagree;
			}
		}

		if( disagree > 0 )
		{
			ACPR_LOG( Warning, BP,
				TEXT( "%s body is NOT rigid in %s mode: %d of %d buildables disagree with the body "
				      "transform (worst %.1f uu, %.2f deg) — occlusion inside the blueprint is unreliable" ),
				*mHologramName, placed ? TEXT( "placed" ) : TEXT( "preview" ), disagree, checked, worst, worstTurn );
		}
		else if( FACPRTrace::Enabled() )
		{
			ACPR_LOG( Display, BP,
				TEXT( "%s body rigid in %s mode: %d buildables agree (worst %.3f uu, %.3f deg)" ),
				*mHologramName, placed ? TEXT( "placed" ) : TEXT( "preview" ), checked, worst, worstTurn );
		}
	}

	return true;
}

void FACPRBlueprintTerminalManager::CastRay( const FTerminalFrame& frame, int32 sourceTerminal, bool placed,
                                            TArray< FRayHit >& out_first, int32& out_examined ) const
{
	out_first.Reset();
	out_examined = 0;

	TArray< FRayHit > hits;

	// World terminals (§10.3: "all world terminals are possible OB candidates or blockers regardless of
	// state"). The registry's axis query is the broadphase; the disc test below is the rule.
	if( const UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( GetHologram() ) )
	{
		TArray< UACPRTerminalComponent* > world;
		registry->FindAlongAxis( frame.P, frame.D, -ACPRRayQueryMargin, mRailMax + ACPRRayQueryMargin,
			ACPRRayQueryMargin, world );

		for( UACPRTerminalComponent* terminal : world )
		{
			if( !terminal || terminal->IsPrivateInterface() )
			{
				continue;
			}

			++out_examined;

			double t = 0.0;
			if( ACPRIntersectDisc( frame.P, frame.D, terminal->GetComponentLocation(), terminal->GetOutwardAxis(),
					ACPRDiscRadiusFor( terminal->GetOwner() ), mRailMax, t ) )
			{
				FRayHit& hit = hits.AddDefaulted_GetRef();
				hit.World = terminal;
				hit.T = t;
			}
		}
	}

	// Blueprint terminals (§10.3: "terminals inside the hologram occlude other BP rays but can never be
	// targets"). Every one, open or not — an internally coupled pair occludes as two terminals at one t.
	// The ray is carried into body space, where the occluders and their grid are fixed.
	if( !mBodyValid || mBodyPlaced != placed )
	{
		RefreshBody( placed );
	}

	if( mBodyValid )
	{
		const FVector p = mBodyToWorld.InverseTransformPositionNoScale( frame.P );
		const FVector d = mBodyToWorld.InverseTransformVectorNoScale( frame.D );

		mOccluderGrid.ForEachAlongSegment( p, d, -ACPRRayQueryMargin, mRailMax + ACPRRayQueryMargin,
			ACPRRayQueryMargin, [ & ]( const TArray< int32 >& occluders )
			{
				for( const int32 i : occluders )
				{
					if( i == sourceTerminal )
					{
						continue;
					}
					// At Construct, a buildable that was not placed is not there to occlude.
					if( placed && !mPlaced.Contains( mBPTerminals[ i ].BuildableIndex ) )
					{
						continue;
					}

					const FOccluderLocal& other = mOccluderLocal[ i ];
					++out_examined;

					double t = 0.0;
					if( ACPRIntersectDisc( p, d, other.P, other.D, mBPTerminals[ i ].DiscRadius, mRailMax, t ) )
					{
						FRayHit& hit = hits.AddDefaulted_GetRef();
						hit.BPTerminal = i;
						hit.T = t;
					}
				}
			} );
	}

	if( hits.Num() == 0 )
	{
		return;
	}

	double first = hits[ 0 ].T;
	for( const FRayHit& hit : hits )
	{
		first = FMath::Min( first, hit.T );
	}

	for( const FRayHit& hit : hits )
	{
		if( hit.T <= first + ACPRRayTieT )
		{
			out_first.Add( hit );
		}
	}
}

FACPRBlueprintTerminalManager::EPairFault FACPRBlueprintTerminalManager::EvaluatePair(
	const FTerminalFrame& frame, const UACPRTerminalComponent* target, double& out_length,
	FIntVector& out_cell, double& out_value ) const
{
	out_length = 0.0;
	out_cell = FIntVector( 0, 0, 0 );
	out_value = 0.0;
	if( !target )
	{
		return EPairFault::TargetGone;
	}

	const FVector c = target->GetComponentLocation();
	const double length = FVector::DotProduct( c - frame.P, frame.D );
	out_length = length;

	if( length < ACPRRayMinT )
	{
		out_value = length;
		return EPairFault::NotAhead;
	}

	// §10.4 step 9, "matching quantized position": the Rail a hand would build from this terminal along its
	// own axis must end on the target's cell. Terminal B of the generated Rail is exactly Start + D * L.
	// Not `far`: <windows.h> defines `far` (and `near`) as empty macros.
	const FVector farPoint = frame.P + frame.D * length;
	const FIntVector farQ = UACPRTerminalComponent::QuantizeLocation( farPoint );
	const FIntVector targetQ = target->GetQuantizedLocation();

	if( farQ != targetQ )
	{
		out_cell = farQ;
		out_value = FVector::Dist( farPoint, c );
		return EPairFault::FarEndMisses;
	}

	// §6.1's integer opposition, predicted for BOTH ends by the Rail's own rule
	// (ACPRRail.cpp: A = Reduce( cellA - cellB ), B = Reduce( cellB - cellA )).
	const FIntVector railA = UACPRTerminalComponent::ReduceDirection( frame.PQ - farQ );
	const FIntVector railB = ACPRNegate( railA );

	if( target->GetQuantizedOutward() != railA )
	{
		out_cell = target->GetQuantizedOutward();
		return EPairFault::TargetNotOpposing;
	}

	if( frame.OutQ != railB )
	{
		out_cell = frame.OutQ;
		return EPairFault::BPNotOpposing;
	}

	// §5.1 / §11: "Length 1-40 m". Half a unit of slack for the float projection.
	if( length < mRailMin - 0.5 )
	{
		out_value = length;
		return EPairFault::TooShort;
	}

	if( length > mRailMax + 0.5 )
	{
		out_value = length;
		return EPairFault::TooLong;
	}

	return EPairFault::None;
}

void FACPRBlueprintTerminalManager::Discover( FState& state, const FTerminalFrame& frame, bool placed )
{
	state.Target = nullptr;
	state.Blocker = nullptr;
	state.Length = 0.0;
	state.Fault = EPairFault::None;
	state.Kind = EVerdict::None;
	state.Reason = EReason::None;
	state.Aux = 0;
	state.T = 0.0;

	const AFGBuildableBlueprintDesigner* space = GetPlacementSpace();

	const UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( GetHologram() );
	if( !registry )
	{
		state.Reason = EReason::NoRegistry;
		return;
	}

	// §6.1 first — the coincident pair, exactly what the placed host's own registration will ask. Reported,
	// not latched: coincidence couples in every mode, so there is nothing for a click to decide.
	{
		TArray< UACPRTerminalComponent* > inCell;
		registry->FindInCell( frame.PQ, nullptr, nullptr, inCell );

		int32 direct = 0;
		UACPRTerminalComponent* directTarget = nullptr;
		const UACPRTerminalComponent* refused = nullptr;

		for( UACPRTerminalComponent* candidate : inCell )
		{
			if( !candidate || candidate->IsPrivateInterface() )
			{
				continue;
			}

			const bool opposing = candidate->GetQuantizedOutward() == ACPRNegate( frame.OutQ );
			const bool open = candidate->IsOpen();
			const bool sameSpace = FACPRSpace::OfActor( candidate->GetOwner() ) == space;

			if( opposing && open && sameSpace )
			{
				++direct;
				directTarget = candidate;
			}
			else if( !refused )
			{
				refused = candidate;
			}
		}

		if( direct == 1 )
		{
			state.Kind = EVerdict::Direct;
			state.Reason = EReason::Direct;
			state.Target = directTarget;
			return;
		}

		if( direct > 1 )
		{
			state.Kind = EVerdict::Blocked;
			state.Reason = EReason::AmbiguousCoincident;
			state.Aux = direct;
			return;
		}

		if( refused )
		{
			state.Kind = EVerdict::Blocked;
			state.Reason = EReason::CoincidentRefused;
			state.Blocker = refused;
			return;
		}
	}

	// §10.4 — the ray.
	TArray< FRayHit > first;
	int32 examined = 0;
	CastRay( frame, state.Terminal, placed, first, examined );

	if( first.Num() == 0 )
	{
		state.Reason = EReason::NoneInRange;
		state.Aux = examined;
		return;
	}

	state.T = first[ 0 ].T;

	if( first.Num() > 1 )
	{
		state.Kind = EVerdict::Blocked;
		state.Reason = EReason::AmbiguousRay;
		state.Aux = first.Num();
		return;
	}

	const FRayHit& hit = first[ 0 ];

	if( !hit.World )
	{
		state.Kind = EVerdict::Blocked;
		state.Reason = EReason::BlockedByBPTerminal;
		state.Aux = hit.BPTerminal;
		return;
	}

	UACPRTerminalComponent* target = hit.World;
	state.Blocker = target;

	if( FACPRSpace::OfActor( target->GetOwner() ) != space )
	{
		state.Kind = EVerdict::Blocked;
		state.Reason = EReason::BlockedSpace;
		return;
	}

	if( !target->IsOpen() )
	{
		state.Kind = EVerdict::Blocked;
		state.Reason = EReason::BlockedNotOpen;
		return;
	}

	double length = 0.0;
	const EPairFault fault = EvaluatePair( frame, target, length, state.AuxCell, state.AuxValue );
	if( fault != EPairFault::None )
	{
		state.Kind = EVerdict::Blocked;
		state.Reason = EReason::PairFault;
		state.Fault = fault;
		return;
	}

	state.Kind = EVerdict::Bridge;
	state.Reason = EReason::Bridge;
	state.Blocker = nullptr;
	state.Target = target;
	state.Length = length;
}

void FACPRBlueprintTerminalManager::FollowLatched( FState& state, const FTerminalFrame& frame, bool placed )
{
	// §10.2 step 3: "each latched auto-connect adapts its geometry in real time but never retargets, showing
	// red where ordinary building rules would prevent it". Same target, whatever the ray now says.
	UACPRTerminalComponent* target = state.LatchedTarget.Get();

	state.Kind = EVerdict::Bridge;
	state.Target = target;
	state.Blocker = nullptr;
	state.Aux = 0;
	state.T = 0.0;

	// Nudged exactly onto the latched target: the pair is now coincident, §6.1 couples it with no Rail at
	// all, and a zero-length bridge is not a Rail. Still latched, still valid — just nothing to build.
	if( target && target->IsOpen() && target->GetQuantizedLocation() == frame.PQ &&
		target->GetQuantizedOutward() == ACPRNegate( frame.OutQ ) &&
		FACPRSpace::OfActor( target->GetOwner() ) == GetPlacementSpace() )
	{
		state.Kind = EVerdict::Direct;
		state.Reason = EReason::LatchedCoincident;
		state.Fault = EPairFault::None;
		state.Length = 0.0;
		state.BridgeValid = true;
		return;
	}

	double length = 0.0;
	EPairFault fault = EvaluatePair( frame, target, length, state.AuxCell, state.AuxValue );

	if( fault == EPairFault::None && target && !target->IsOpen() )
	{
		fault = EPairFault::TargetNotOpen;
	}

	if( fault == EPairFault::None && target && FACPRSpace::OfActor( target->GetOwner() ) != GetPlacementSpace() )
	{
		fault = EPairFault::OtherSpace;
	}

	// Still the FIRST thing on the ray? A nudge can bring another terminal in between, and §10.4 step 11
	// says a failure never falls through — so the latched pair turns red rather than passing through it.
	if( fault == EPairFault::None )
	{
		TArray< FRayHit > first;
		int32 examined = 0;
		CastRay( frame, state.Terminal, placed, first, examined );

		if( first.Num() != 1 || first[ 0 ].World != target )
		{
			if( first.Num() == 0 )
			{
				fault = EPairFault::RayLost;
			}
			else
			{
				fault = EPairFault::Occluded;
				state.Blocker = first[ 0 ].World;
				state.Aux = first[ 0 ].World ? first.Num() : first[ 0 ].BPTerminal;
				state.AuxValue = first[ 0 ].T;
			}
		}
	}

	state.Fault = fault;
	state.Reason = ( fault == EPairFault::None ) ? EReason::LatchedValid : EReason::LatchedInvalid;

	// What to DRAW: the exact length when there is one, else the nearest a Rail can be, along the BP
	// terminal's own axis. Always a Rail's length, so the preview never degenerates.
	state.Length = FMath::Clamp( length, mRailMin, mRailMax );
}

void FACPRBlueprintTerminalManager::EvaluateAll( const TCHAR* why, bool force, bool& out_playSnap )
{
	AFGBlueprintHologram* hologram = GetHologram();
	if( !hologram )
	{
		return;
	}

	// Only when the preview has actually moved (or the caller insists). The registry scan is linear in
	// registered cells, and a hologram held still asks the same question every frame.
	//
	// The gate reads what the frames read. Every frame below is derived from the body transform (a
	// preview root, RefreshBody), so the gate compares that — not the actor's location plus the nudge
	// offset, which changes on the input while the roots move later in the frame. With the gate on the
	// roots themselves, vanilla's next UpdateAutomaticConnections after the move is the evaluation, and
	// the bridge holograms need no tick of their own.
	if( !RefreshBody( false ) )
	{
		return;
	}
	const FVector location = mBodyToWorld.GetLocation();
	const FQuat rotation = mBodyToWorld.GetRotation();

	if( !force && mHasEvaluated &&
		location.Equals( mLastEvaluatedLocation, 0.1 ) &&
		rotation.Equals( mLastEvaluatedRotation, 1e-4 ) )
	{
		return;
	}

	mHasEvaluated = true;
	mLastEvaluatedLocation = location;
	mLastEvaluatedRotation = rotation;
	++mEvaluationCount;
	++mEvaluationsBy.FindOrAdd( FString( why ) );

	// Where an evaluation's time goes, per evaluation, at Display. Evaluations happen only when the
	// preview moves, and a moving preview evaluates every frame — so this is the per-frame cost of an
	// auto-connect blueprint. Cycles64, not a timer.
	const uint64 tEvalStart = FPlatformTime::Cycles64();
	const uint64 tOccluders = tEvalStart;

	// §10.3's "one immutable snapshot": every frame is computed, and every proposal produced, before any
	// bridge is touched.
	TArray< FTerminalFrame > frames;
	TArray< bool > haveFrame;
	frames.SetNum( mStates.Num() );
	haveFrame.SetNumZeroed( mStates.Num() );

	for( int32 i = 0; i < mStates.Num(); ++i )
	{
		FState& state = mStates[ i ];
		haveFrame[ i ] = ComputeFrame( mBPTerminals[ state.Terminal ], false, frames[ i ] );

		if( !haveFrame[ i ] )
		{
			state.Kind = EVerdict::None;
			state.Reason = EReason::NoPreviewRoot;
			state.BridgeValid = false;
			continue;
		}

		if( state.Latched && !state.LatchedTarget.IsValid() )
		{
			// Vanilla's own rule (FGBlueprintOpenConnectionManager.h:334): a target that goes away releases
			// the latch rather than silently carrying it to a new candidate.
			state.Latched = false;
			if( AACPRRailHologram* bridge = state.Bridge.Get() )
			{
				bridge->SetBridgeLatched( false );
			}
			LogStateLine( i, state, TEXT( "UNLATCHED (target gone)" ), true );
		}

		if( state.Latched )
		{
			FollowLatched( state, frames[ i ], false );
		}
		else
		{
			Discover( state, frames[ i ], false );
		}
	}

	const uint64 tDiscover = FPlatformTime::Cycles64();

	// §10.5: "the lowest-scoring proposal wins and the others are dropped for that frame", latched targets
	// claimed first. Geometry makes this unreachable in play; it is here to log the defect if it happens.
	TMap< const UACPRTerminalComponent*, int32 > claims;
	for( int32 i = 0; i < mStates.Num(); ++i )
	{
		if( mStates[ i ].Latched && mStates[ i ].Target.IsValid() )
		{
			claims.Add( mStates[ i ].Target.Get(), i );
		}
	}

	TArray< int32 > proposals;
	for( int32 i = 0; i < mStates.Num(); ++i )
	{
		if( !mStates[ i ].Latched && mStates[ i ].Kind == EVerdict::Bridge )
		{
			proposals.Add( i );
		}
	}
	proposals.Sort( [ this ]( int32 a, int32 b ) { return mStates[ a ].Length < mStates[ b ].Length; } );

	for( int32 i : proposals )
	{
		FState& state = mStates[ i ];
		const UACPRTerminalComponent* target = state.Target.Get();
		if( const int32* winner = claims.Find( target ) )
		{
			if( mConflictLogBudget > 0 )
			{
				--mConflictLogBudget;
				ACPR_LOG( Warning, BP,
					TEXT( "%s CONFLICT (10.5) | #%d loses %s to #%d | lengths %.1f vs %.1f" ),
					*mHologramName, i, *DescribeWorld( target ), *winner, state.Length, mStates[ *winner ].Length );
			}

			state.Kind = EVerdict::Blocked;
			state.Reason = EReason::Conflict;
			state.Aux = *winner;
			state.Target = nullptr;
			continue;
		}

		claims.Add( target, i );
	}

	// Only now are bridges configured, validated and shown.
	for( int32 i = 0; i < mStates.Num(); ++i )
	{
		FState& state = mStates[ i ];

		if( haveFrame[ i ] )
		{
			UpdateBridge( i, state, frames[ i ], out_playSnap );
		}
		else
		{
			DisableBridge( state );
		}

		// After UpdateBridge, which is where a proposal that fails Rail validation turns Blocked.
		UpdateIcon( i, state, haveFrame[ i ] ? &frames[ i ] : nullptr );
		BroadcastConnectionState( i, state );

		// Change detection on the data, not on a string.
		if( state.Reason != state.LoggedReason || state.Fault != state.LoggedFault ||
			state.Target != state.LoggedTarget || state.Aux != state.LoggedAux )
		{
			state.LoggedReason = state.Reason;
			state.LoggedFault = state.Fault;
			state.LoggedTarget = state.Target;
			state.LoggedAux = state.Aux;
			LogStateLine( i, state, why, false );
		}
	}

	const uint64 tEnd = FPlatformTime::Cycles64();
	if( FACPRTrace::Enabled() )
	{
		const double ms = 1000.0;
		int32 discovered = 0, latched = 0, shown = 0;
		for( const FState& state : mStates )
		{
			discovered += ( state.Kind != EVerdict::None ) ? 1 : 0;
			latched += state.Latched ? 1 : 0;
			shown += state.BridgeEnabled ? 1 : 0;
		}
		const UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( hologram );
		ACPR_LOG( Display, BP_TIME,
			TEXT( "%s evaluate(%s) #%d | candidates=%d occluders=%d (buckets=%d) "
			      "registryCells=%d (buckets=%d) "
			      "| body %.2f ms | discover %.2f ms | bridges+icons %.2f ms | total %.2f ms "
			      "| found=%d latched=%d shown=%d reHidden=%d" ),
			*mHologramName, why, mEvaluationCount, mStates.Num(), mBPTerminals.Num(),
			mOccluderGrid.GetBucketCount(),
			registry ? registry->GetCellCount() : -1,
			registry ? registry->GetBucketCount() : -1,
			ms * FPlatformTime::ToSeconds64( tOccluders - tEvalStart ),
			ms * FPlatformTime::ToSeconds64( tDiscover - tOccluders ),
			ms * FPlatformTime::ToSeconds64( tEnd - tDiscover ),
			ms * FPlatformTime::ToSeconds64( tEnd - tEvalStart ),
			discovered, latched, shown, mRevealsReasserted );
	}

	LogBridgeScaling();

	// The nudge case, at Display. A locked hologram only moves by nudging, so every evaluation while locked
	// is one nudge step: where the blueprint is, which caller noticed, and — per shown bridge — where its
	// BP end is now, its length, and where the bridge actor actually stands after this evaluation. The last
	// two must agree with each other and the far end must not move.
	// IsHologramLocked and GetHologramNudgeOffset are inline (FGHologram.h:457, :473).
	if( hologram->IsHologramLocked() && mLockedLogBudget > 0 && FACPRTrace::Enabled() )
	{
		FString bridges;
		for( int32 i = 0; i < mStates.Num(); ++i )
		{
			const FState& state = mStates[ i ];
			const AACPRRailHologram* bridge = state.Bridge.Get();
			if( !bridge || !state.BridgeEnabled || !haveFrame[ i ] )
			{
				continue;
			}

			bridges += FString::Printf( TEXT( "#%d%s start=%s L=%.1f far=%s actor=%s | " ),
				i, state.Latched ? TEXT( "(latched)" ) : TEXT( "" ),
				*frames[ i ].P.ToString(), state.Length,
				*( frames[ i ].P + frames[ i ].D * state.Length ).ToString(),
				*bridge->GetActorLocation().ToString() );
		}

		if( !bridges.IsEmpty() )
		{
			--mLockedLogBudget;
			ACPR_LOG( Display, BP,
				TEXT( "%s evaluated while locked (%s) | at %s nudge=%s | %s" ),
				*mHologramName, why, *location.ToString(),
				*hologram->GetHologramNudgeOffset().ToString(), *bridges );
		}
	}
}

AACPRRailHologram* FACPRBlueprintTerminalManager::EnsureBridge( int32 index, FState& state, const FTerminalFrame& frame )
{
	if( AACPRRailHologram* existing = state.Bridge.Get() )
	{
		return existing;
	}

	AFGBlueprintHologram* parent = GetHologram();
	if( !parent || !mRailRecipe )
	{
		if( mSpawnFailLogBudget > 0 )
		{
			--mSpawnFailLogBudget;
			ACPR_LOG( Warning, BP,
				TEXT( "%s cannot spawn bridge #%d: parent=%s recipe=%s (%s)" ),
				*mHologramName, index, *GetNameSafe( parent ), *GetNameSafe( mRailRecipe.Get() ), *mRailRecipeSource );
		}
		return nullptr;
	}

	// A deterministic name per candidate, and the hologram is reused for the whole placement. Vertical
	// Conveyor Auto-Connect found that AFGHologram's child-name lookup is not released synchronously when a
	// child is destroyed, and a respawn under the same name trips AddChild's duplicate-name assertion. So
	// nothing here is ever destroyed; it is disabled.
	const FName childName( *FString::Printf( TEXT( "ACPRBridge_%d" ), index ) );

	AFGHologram* child = parent->FindChildHologramByName( childName );
	const bool reused = child != nullptr;

	if( !child )
	{
		AActor* owner = IsValid( parent->GetOwner() ) ? parent->GetOwner() : static_cast< AActor* >( parent );

		const int32 childrenBefore = parent->mChildren.Num();
		const uint64 spawnStart = FPlatformTime::Cycles64();
		FACPRCostScope spawnCost( EACPRCost::BridgeSpawn );
		child = AFGHologram::SpawnChildHologramFromRecipe( parent, childName, mRailRecipe, owner, frame.P,
			[]( AFGHologram* spawned )
			{
				// Before BeginPlay, so the hologram's own BeginPlay already knows it is a bridge.
				if( AACPRRailHologram* rail = Cast< AACPRRailHologram >( spawned ) )
				{
					rail->MarkAsBridge();
				}
				spawned->SetShouldSpawnChildHolograms( false );
			} );
		mSpawnSamples.Add( TPair< int32, double >( childrenBefore,
			1e3 * FPlatformTime::ToSeconds64( FPlatformTime::Cycles64() - spawnStart ) ) );
	}
	else if( !IsValid( child ) )
	{
		if( mSpawnFailLogBudget > 0 )
		{
			--mSpawnFailLogBudget;
			ACPR_LOG( Warning, BP,
				TEXT( "%s bridge #%d: child %s is registered but pending kill; not respawning under the same name" ),
				*mHologramName, index, *childName.ToString() );
		}
		return nullptr;
	}

	AACPRRailHologram* bridge = Cast< AACPRRailHologram >( child );
	if( !bridge )
	{
		if( IsValid( child ) )
		{
			child->SetDisabled( true );
		}

		if( mSpawnFailLogBudget > 0 )
		{
			--mSpawnFailLogBudget;
			ACPR_LOG( Error, BP,
				TEXT( "%s bridge #%d: recipe %s spawned %s (%s), not a Power Rail hologram" ),
				*mHologramName, index, *GetNameSafe( mRailRecipe.Get() ),
				*GetNameSafe( child ), child ? *child->GetClass()->GetName() : TEXT( "null" ) );
		}
		return nullptr;
	}

	bridge->MarkAsBridge();
	bridge->SetBridgeLatched( false );
	bridge->SetBridgeLabel( FString::Printf( TEXT( "#%d %s" ), index, *DescribeTerminal( state.Terminal ) ) );
	SetBridgeActive( state, bridge, false );
	state.Bridge = bridge;
	state.BridgeEnabled = false;
	++mBridgesSpawned;

	// What a bridge hologram is, once per session: spawning and showing one are vanilla calls that scale
	// with the components and material slots vanilla set up on it, and this is the list those calls
	// work through.
	static bool dumped = false;
	if( !reused && !dumped && FACPRTrace::Enabled() )
	{
		dumped = true;
		TArray< UActorComponent* > components;
		bridge->GetComponents( components );
		FString list;
		int32 slotsTotal = 0;
		for( const UActorComponent* component : components )
		{
			if( !component )
			{
				continue;
			}
			if( const UStaticMeshComponent* mesh = Cast< UStaticMeshComponent >( component ) )
			{
				slotsTotal += mesh->GetNumMaterials();
				list += FString::Printf( TEXT( " | %s (%s) mesh=%s slots=%d collision=%d visible=%d" ),
					*mesh->GetName(), *mesh->GetClass()->GetName(),
					*GetNameSafe( mesh->GetStaticMesh() ), mesh->GetNumMaterials(),
					mesh->IsCollisionEnabled() ? 1 : 0, mesh->IsVisible() ? 1 : 0 );
			}
			else
			{
				list += FString::Printf( TEXT( " | %s (%s)" ), *component->GetName(), *component->GetClass()->GetName() );
			}
		}
		ACPR_LOG( Display, BRIDGE,
			TEXT( "%s anatomy | components=%d meshSlots=%d%s" ),
			*bridge->GetName(), components.Num(), slotsTotal, *list );
	}

	ACPR_LOG( Verbose, BP,
		TEXT( "%s bridge #%d hologram %s %s | class=%s | parent=%s | recipe=%s" ),
		*mHologramName, index, reused ? TEXT( "reused" ) : TEXT( "spawned" ), *bridge->GetName(),
		*bridge->GetClass()->GetName(), *GetNameSafe( bridge->GetParentHologram() ),
		*GetNameSafe( mRailRecipe.Get() ) );

	return bridge;
}

void FACPRBlueprintTerminalManager::DisableBridge( FState& state )
{
	AACPRRailHologram* bridge = state.Bridge.Get();
	if( bridge )
	{
		// A hidden bridge must never block its parent: UpdateBridge re-asserts the flag whenever it shows one.
		bridge->SetBridgeLatched( false );

		SetBridgeActive( state, bridge, false );
		if( !bridge->IsHidden() )
		{
			bridge->SetActorHiddenInGame( true );
		}
	}

	// Hidden now, so the next time it is shown is an event worth a line again.
	if( state.BridgeEnabled )
	{
		state.LoggedBridgeSet = false;
	}
	state.BridgeEnabled = false;
}

void FACPRBlueprintTerminalManager::SetBridgeActive( FState& state, AACPRRailHologram* bridge, bool active )
{
	if( !bridge || bridge->IsDisabled() == !active )
	{
		return;
	}

	const AFGBlueprintHologram* parent = GetHologram();
	const int32 children = parent ? parent->mChildren.Num() : -1;
	const uint64 start = FPlatformTime::Cycles64();
	{
		FACPRCostScope cost( active ? EACPRCost::BridgeShow : EACPRCost::BridgeDisable );
		bridge->SetDisabled( !active );
	}
	( active ? mShowSamples : mDisableSamples ).Add( TPair< int32, double >( children,
		1e3 * FPlatformTime::ToSeconds64( FPlatformTime::Cycles64() - start ) ) );
}

void FACPRBlueprintTerminalManager::LogBridgeScaling()
{
	// Whether the per-bridge cost of vanilla's spawn / enable / disable is a function of how many children
	// the parent already has: ten buckets by child count at the time of the call, average ms in each.
	auto describe = []( const TArray< TPair< int32, double > >& samples ) -> FString
	{
		if( samples.Num() == 0 )
		{
			return FString( TEXT( "-" ) );
		}
		int32 maxChildren = 0;
		for( const TPair< int32, double >& sample : samples )
		{
			maxChildren = FMath::Max( maxChildren, sample.Key );
		}
		const int32 width = FMath::Max( 1, ( maxChildren + 10 ) / 10 );
		double sum[ 10 ] = {};
		int32 count[ 10 ] = {};
		for( const TPair< int32, double >& sample : samples )
		{
			const int32 bucket = FMath::Clamp( sample.Key / width, 0, 9 );
			sum[ bucket ] += sample.Value;
			++count[ bucket ];
		}
		FString out = FString::Printf( TEXT( "n=%d" ), samples.Num() );
		for( int32 i = 0; i < 10; ++i )
		{
			if( count[ i ] > 0 )
			{
				out += FString::Printf( TEXT( " [%d..%d]=%.2f" ), i * width, ( i + 1 ) * width - 1, sum[ i ] / count[ i ] );
			}
		}
		return out;
	};

	if( mSpawnSamples.Num() + mShowSamples.Num() + mDisableSamples.Num() == 0 )
	{
		return;
	}
	if( !FACPRTrace::Enabled() )
	{
		mSpawnSamples.Reset();
		mShowSamples.Reset();
		mDisableSamples.Reset();
		return;
	}

	ACPR_LOG( Display, BRIDGE_SCALE,
		TEXT( "%s ms per call by children-at-call | spawn: %s | enable: %s | disable: %s" ),
		*mHologramName, *describe( mSpawnSamples ), *describe( mShowSamples ), *describe( mDisableSamples ) );

	mSpawnSamples.Reset();
	mShowSamples.Reset();
	mDisableSamples.Reset();
}

void FACPRBlueprintTerminalManager::UpdateBridge( int32 index, FState& state, const FTerminalFrame& frame, bool& out_playSnap )
{
	const bool want = state.Kind == EVerdict::Bridge;

	if( !want )
	{
		const bool wasShown = state.BridgeEnabled;
		DisableBridge( state );
		if( wasShown )
		{
			LogStateLine( index, state, TEXT( "proposal withdrawn" ), true, true );
		}

		// A latched pair that became coincident stays valid (FollowLatched); everything else here has no
		// bridge to be valid with.
		state.BridgeValid = state.Latched && state.Kind == EVerdict::Direct;
		return;
	}

	AACPRRailHologram* bridge = EnsureBridge( index, state, frame );
	if( !bridge )
	{
		if( !state.Latched )
		{
			state.Kind = EVerdict::Blocked;
			state.Reason = EReason::NoBridgeHologram;
		}
		state.BridgeValid = false;
		return;
	}

	const FBPTerminal& terminal = mBPTerminals[ state.Terminal ];
	const AFGBuildable* bpHost = terminal.Buildable.Get();

	FACPRBridgeSpec spec;
	spec.Start = frame.P;
	spec.Outward = frame.D;
	spec.Up = frame.Up;
	spec.Length = state.Length;
	spec.FarTerminal = state.Target;
	spec.Space = GetPlacementSpace();
	spec.Faulted = state.Latched && state.IsFaulted();
	spec.Customization = bpHost ? &bpHost->GetCustomizationData_Native() : nullptr;

	{
		FACPRCostScope applyCost( EACPRCost::BridgeApply );
		bridge->ApplyBridgeSpec( spec );
		bridge->SetBridgeLatched( state.Latched );
	}

	FString disqualifiers;
	bool valid = false;
	{
		FACPRCostScope validateCost( EACPRCost::BridgeValidate );
		valid = bridge->ValidateBridgeNow( disqualifiers );
	}
	state.BridgeValid = valid;
	if( state.BridgeDisqualifiers != disqualifiers )
	{
		state.BridgeDisqualifiers = disqualifiers;
	}

	if( !state.Latched && !valid )
	{
		// Vanilla's template does the same for its own belts before the first click
		// (FGBlueprintOpenConnectionManager.h:427-433): an unsnapped bridge that cannot be built is hidden,
		// so it is never offered and never latched.
		const bool wasShown = state.BridgeEnabled;
		DisableBridge( state );
		state.Kind = EVerdict::Blocked;
		state.Reason = EReason::FailsValidation;
		if( wasShown )
		{
			LogStateLine( index, state, TEXT( "proposal withdrawn" ), true, true );
		}
		return;
	}

	// Active only when latched, visible whenever proposed. Vanilla's SetDisabled is the O(children) call
	// (measured: avg 5.7 ms, worst 10.8, growing with every bridge before it), and an unlatched proposal
	// has no business being an active child anyway — it is an offer, not part of the placement, priced and
	// constructed only once the click latches it. So a proposal is shown with the plain hide toggle (2 µs)
	// and stays a disabled child; the latch is what enables it. Same hologram, same validator
	// (ValidateBridgeNow runs on disabled bridges), only vanilla's enable is deferred to where it means something.
	SetBridgeActive( state, bridge, state.Latched );

	// The hologram's own flags are the truth: anything outside this manager that hid it must not leave the
	// state believing it is shown. Re-assertions are counted and printed in the evaluation line — a count
	// that keeps rising between evaluations means something of vanilla's re-hides disabled children.
	if( bridge->IsHidden() )
	{
		if( state.BridgeEnabled )
		{
			++mRevealsReasserted;
		}
		bridge->SetActorHiddenInGame( false );
	}
	if( !state.BridgeEnabled )
	{
		state.BridgeEnabled = true;
		out_playSnap = true;
	}

	// Pushed here as well as held by the override: nothing documents whether vanilla sets a child's
	// material, and a bridge that was red and became valid must not stay red.
	{
		FACPRCostScope materialCost( EACPRCost::BridgeMaterial );
		const bool soft = disqualifiers != TEXT( "<none>" );
		bridge->SetPlacementMaterialState( !valid ? EHologramMaterialState::HMS_ERROR
			: ( soft ? EHologramMaterialState::HMS_WARNING : EHologramMaterialState::HMS_OK ) );
	}

	if( !state.LoggedBridgeSet || state.LoggedLatched != state.Latched ||
		state.LoggedEnabled != state.BridgeEnabled || state.LoggedValid != valid )
	{
		state.LoggedBridgeSet = true;
		state.LoggedLatched = state.Latched;
		state.LoggedEnabled = state.BridgeEnabled;
		state.LoggedValid = valid;
		LogStateLine( index, state, state.Latched
			? ( valid ? TEXT( "latched bridge VALID" ) : TEXT( "latched bridge INVALID - blocks the placement" ) )
			: TEXT( "PROPOSAL shown" ), true, !state.Latched );
	}
}

void FACPRBlueprintTerminalManager::UpdateAutomaticConnections( const FHitResult& /*hitResult*/, bool& out_PlaySnapEffects )
{
	++mUpdateCount;

	if( const AFGBlueprintHologram* hologram = GetHologram() )
	{
		mUpdatesWhileLocked += hologram->IsHologramLocked() ? 1 : 0;
	}
	if( mUpdateCount == 1 )
	{
		DumpDuplicatedMeshes( TEXT( "first update" ) );
	}

	mDriven = true;

	bool playSnap = false;
	EvaluateAll( TEXT( "update" ), false, playSnap );

	if( playSnap )
	{
		out_PlaySnapEffects = true;
	}
}

void FACPRBlueprintTerminalManager::HandleBuildableConnectionRemapping( AFGBuildable* buildable, int32 blueprintBuildableIndex )
{
	FACPRCostScope cost( EACPRCost::ManagerRemap );
	// "Remaps open connections for the specified blueprint buildable index to the specified buildable"
	// (FGBlueprintOpenConnectionManager.h:187) — the newly placed buildable, before its BeginPlay, under
	// the index Initialize saw it at. Construct builds from these.
	++mRemapCount;

	if( !buildable )
	{
		return;
	}

	// The placed buildable must be what Initialize indexed at this position — same class and the same
	// terminal count — or every frame Construct derives for it is for the wrong actor. A mismatch is an
	// Error and the index is not taken: the states on it then read "no placed buildable" at Construct
	// and refuse, which is the safe answer.
	for( const FBPTerminal& t : mBPTerminals )
	{
		if( t.BuildableIndex != blueprintBuildableIndex )
		{
			continue;
		}
		const AFGBuildable* indexed = t.Buildable.Get();
		const IACPRTerminalHost* placedHost = Cast< IACPRTerminalHost >( buildable );
		const IACPRTerminalHost* indexedHost = indexed ? Cast< IACPRTerminalHost >( indexed ) : nullptr;
		const bool sameClass = !indexed || indexed->GetClass() == buildable->GetClass();
		const bool sameCount = !indexedHost || ( placedHost && placedHost->GetTerminalCount() == indexedHost->GetTerminalCount() );
		if( !sameClass || !sameCount )
		{
			ACPR_LOG( Error, BP,
				TEXT( "%s remap #%d -> %s (%s) does not match the indexed %s (%s): class=%d terminals=%d — not taken" ),
				*mHologramName, blueprintBuildableIndex, *buildable->GetName(), *buildable->GetClass()->GetName(),
				*GetNameSafe( indexed ), indexed ? *indexed->GetClass()->GetName() : TEXT( "-" ),
				sameClass ? 1 : 0, sameCount ? 1 : 0 );
			return;
		}
		break;
	}

	mPlaced.Add( blueprintBuildableIndex, buildable );

	if( mRemapLogBudget > 0 && ACPR_LOG_ACTIVE( Verbose ) )
	{
		if( const IACPRTerminalHost* host = Cast< IACPRTerminalHost >( buildable ) )
		{
			--mRemapLogBudget;
			ACPR_LOG( Verbose, BP,
				TEXT( "%s remap #%d -> %s | begunPlay=%d fromSave=%d space=%s | records=%s" ),
				*mHologramName, blueprintBuildableIndex, *buildable->GetName(),
				buildable->HasActorBegunPlay() ? 1 : 0,
				host->WasLoadedFromSave() ? 1 : 0,
				*FACPRSpace::Describe( host->GetHostDesigner() ),
				*FACPRCoupling::DescribeRecords( buildable, host ) );
		}
	}
}

bool FACPRBlueprintTerminalManager::AttemptConnectionStateSnap()
{
	++mSnapQueryCount;

	// The state at the moment of the click, not at the last frame the preview happened to move.
	bool playSnap = false;
	EvaluateAll( TEXT( "click" ), true, playSnap );

	int32 newlyLatched = 0;
	int32 latched = 0;
	int32 invalid = 0;
	int32 direct = 0;

	for( int32 i = 0; i < mStates.Num(); ++i )
	{
		FState& state = mStates[ i ];

		if( state.Kind == EVerdict::Direct )
		{
			++direct;
		}

		// §10.2 step 2: "The first click latches the exact BP- and OB-terminal identities." Every valid,
		// shown proposal, exactly vanilla's AttemptConnectionStateSnap (FGBlueprintOpenConnectionManager.h:646).
		if( !state.Latched && state.Kind == EVerdict::Bridge && state.BridgeEnabled && state.BridgeValid &&
			state.Target.IsValid() )
		{
			state.Latched = true;
			state.LatchedTarget = state.Target;
			if( AACPRRailHologram* bridge = state.Bridge.Get() )
			{
				bridge->SetBridgeLatched( true );
			}

			++newlyLatched;
			++mLatchCount;
			LogStateLine( i, state, TEXT( "LATCHED" ), true );
		}
	}

	for( const FState& state : mStates )
	{
		if( state.Latched )
		{
			++latched;
			if( !state.BridgeValid )
			{
				++invalid;
			}
		}
	}

	// §10.2 step 4: "only if every latched auto-connect is valid. A single red one blocks the whole
	// placement, and the click simply does nothing". The block is the parent's disqualifier — a latched
	// invalid bridge adds one to the blueprint hologram on every validation (AACPRRailHologram::
	// CheckValidPlacement), so the parent cannot construct. This returns what vanilla's contract asks
	// (FGBlueprintOpenConnectionManager.h:203, "true if a state was snapped") and nothing more.
	const bool result = newlyLatched > 0;
	if( invalid > 0 )
	{
		++mBlockedClicks;
	}

	AFGBlueprintHologram* hologram = GetHologram();
	ACPR_LOG( Display, BP,
		TEXT( "%s click #%d | newly latched=%d latched=%d invalid=%d direct=%d -> %s | "
		      "cost with bridges=[%s] without=[%s] | parent disqualifiers=[%s]" ),
		*mHologramName, mSnapQueryCount, newlyLatched, latched, invalid, direct,
		result ? TEXT( "latch step (returns true)" )
			: ( invalid > 0 ? TEXT( "no new latch (returns false; the parent's disqualifier blocks)" ) : TEXT( "build (returns false)" ) ),
		hologram ? *DescribeCost( hologram->GetCost( true ) ) : TEXT( "-" ),
		hologram ? *DescribeCost( hologram->GetCost( false ) ) : TEXT( "-" ),
		*DescribeParentDisqualifiers() );

	return result;
}

bool FACPRBlueprintTerminalManager::CanSnapConnectionStates() const
{
	for( const FState& state : mStates )
	{
		if( !state.Latched && state.Kind == EVerdict::Bridge && state.BridgeEnabled && state.BridgeValid )
		{
			return true;
		}

		if( state.Latched && !state.BridgeValid )
		{
			return true;
		}
	}

	return false;
}

void FACPRBlueprintTerminalManager::Construct( TArray< AFGBuildable* >& out_ConstructedBridgeBuildables,
                                              FNetConstructionID netConstructionID )
{
	FACPRCostScope cost( EACPRCost::ManagerConstruct );
	// Called after every buildable is spawned and remapped and before any of them runs BeginPlay
	// (FGBlueprintOpenConnectionManager.h:581). The blueprint's own terminals are read from the PLACED
	// actors' transforms and their saved lengths, not from components that are not positioned yet.
	++mConstructCount;

	// The placement is being built; its preview icons have done their job.
	HideAllIcons();

	AFGBlueprintHologram* hologram = GetHologram();
	RefreshBody( true );

	// Two phases. §10.6: "The server builds exactly the previewed set, or nothing." Every latched
	// bridge is revalidated against the placed geometry first, with the registry holding only the
	// world as it was (a bridge Rail built earlier in the same loop would register its terminals and could
	// occlude or be the first hit for a later one); only if every one of them holds is any of them built.
	// A refusal in the first phase builds nothing and says why at Error — the placement then stands
	// without its bridges rather than with some of them.
	struct FPlan
	{
		int32 State = INDEX_NONE;
		AACPRRailHologram* Bridge = nullptr;
		AFGBuildable* PlacedHost = nullptr;
		const AFGBuildable* CustomizationSource = nullptr;
		double Drift = -1.0;
	};
	TArray< FPlan > plans;

	int32 refused = 0;
	int32 unlatchedShown = 0;
	int32 direct = 0;

	for( int32 i = 0; i < mStates.Num(); ++i )
	{
		FState& state = mStates[ i ];
		const FBPTerminal& terminal = mBPTerminals[ state.Terminal ];

		if( !state.Latched )
		{
			if( state.Kind == EVerdict::Direct )
			{
				++direct;
			}

			if( state.Kind == EVerdict::Bridge && state.BridgeEnabled )
			{
				// A shown proposal the click did not latch should not exist at this point — the click latches
				// every one.
				++unlatchedShown;
				ACPR_LOG( Warning, BP,
					TEXT( "%s construct #%d: shown but never latched, NOT built | %s" ),
					*mHologramName, i, *DescribeVerdict( state ) );
			}
			else if( mConstructLogBudget > 0 )
			{
				--mConstructLogBudget;
				LogStateLine( i, state, TEXT( "construct" ), true );
			}
			continue;
		}

		if( state.Kind == EVerdict::Direct )
		{
			++direct;
			DisableBridge( state );
			LogStateLine( i, state, TEXT( "construct (latched, coincident - no Rail)" ), true );
			continue;
		}

		FTerminalFrame placedFrame;
		FTerminalFrame previewFrame;
		const bool havePlaced = ComputeFrame( terminal, true, placedFrame );
		const bool havePreview = ComputeFrame( terminal, false, previewFrame );

		if( !havePlaced )
		{
			ACPR_LOG( Error, BP,
				TEXT( "%s construct #%d: no placed buildable for blueprint index %d - remapping never "
				      "arrived; using the preview frame" ),
				*mHologramName, i, terminal.BuildableIndex );

			if( !havePreview )
			{
				++refused;
				continue;
			}
			placedFrame = previewFrame;
		}

		// §10.6: "The server revalidates the exact latched identities and geometry." Same target, the placed
		// geometry, the whole of §10.4 again.
		FollowLatched( state, placedFrame, true );

		// The placed geometry can be coincident where the preview was not (or vice versa): then there is no
		// Rail to build, §6.1 couples the pair, and building a zero-length bridge would be wrong.
		if( state.Kind == EVerdict::Direct )
		{
			++direct;
			DisableBridge( state );
			LogStateLine( i, state, TEXT( "construct (latched, placed coincident - no Rail)" ), true );
			continue;
		}

		FPlan plan;
		plan.State = i;
		plan.Bridge = state.Bridge.Get();
		const TWeakObjectPtr< AFGBuildable >* placedWeak = mPlaced.Find( terminal.BuildableIndex );
		plan.PlacedHost = placedWeak ? placedWeak->Get() : nullptr;
		plan.CustomizationSource = plan.PlacedHost ? plan.PlacedHost : terminal.Buildable.Get();
		plan.Drift = havePreview ? FVector::Dist( placedFrame.P, previewFrame.P ) : -1.0;

		if( !plan.Bridge )
		{
			ACPR_LOG( Error, BP,
				TEXT( "%s CONSTRUCT REFUSED #%d: the latched bridge hologram is gone" ), *mHologramName, i );
			++refused;
			continue;
		}

		FACPRBridgeSpec spec;
		spec.Start = placedFrame.P;
		spec.Outward = placedFrame.D;
		spec.Up = placedFrame.Up;
		spec.Length = state.Length;
		spec.FarTerminal = state.Target;
		spec.Space = GetPlacementSpace();
		spec.Faulted = state.IsFaulted();
		spec.Customization = plan.CustomizationSource ? &plan.CustomizationSource->GetCustomizationData_Native() : nullptr;

		plan.Bridge->ApplyBridgeSpec( spec );
		plan.Bridge->SetBridgeLatched( true );

		FString disqualifiers;
		if( !plan.Bridge->ValidateBridgeNow( disqualifiers ) )
		{
			// §10.2 step 4 was supposed to make this unreachable: an invalid latched bridge refuses the click.
			ACPR_LOG( Error, BP,
				TEXT( "%s CONSTRUCT REFUSED #%d: %s [%s] - the click should have been blocked (10.2)" ),
				*mHologramName, i, *DescribeVerdict( state ), *disqualifiers );
			++refused;
			continue;
		}

		plans.Add( plan );
	}

	int32 built = 0;

	if( refused > 0 )
	{
		// All or nothing. The bridges that would have passed are hidden too, so nothing prices them.
		for( const FPlan& plan : plans )
		{
			DisableBridge( mStates[ plan.State ] );
		}
		ACPR_LOG( Error, BP,
			TEXT( "%s Construct builds NO bridges: %d of %d latched bridge(s) refused revalidation (10.6: the previewed set or nothing)" ),
			*mHologramName, refused, refused + plans.Num() );
		plans.Reset();
	}

	for( const FPlan& plan : plans )
	{
		FState& state = mStates[ plan.State ];
		const FBPTerminal& terminal = mBPTerminals[ state.Terminal ];
		AACPRRailHologram* bridge = plan.Bridge;

		SetBridgeActive( state, bridge, true );

		const UACPRTerminalComponent* target = state.Target.Get();
		const FString bridgeCost = bridge->DescribeOwnCost();

		TArray< AActor* > children;
		AActor* builtActor = bridge->Construct( children, netConstructionID );
		AACPRRail* rail = Cast< AACPRRail >( builtActor );

		if( !rail )
		{
			ACPR_LOG( Error, BP,
				TEXT( "%s construct #%d: the bridge hologram built %s (%s), not a Power Rail" ),
				*mHologramName, plan.State, *GetNameSafe( builtActor ),
				builtActor ? *builtActor->GetClass()->GetName() : TEXT( "null" ) );
			DisableBridge( state );
			continue;
		}

		// The Rail's BeginPlay runs inside Construct (FinishSpawning in a begun world), registers its
		// terminals and resolves its couplings. The far end's partner is in the world already, so terminal B
		// is coupled to the target now or something is wrong — the revalidation said it would be.
		// Terminal A's partner is the placed BP host, whose BeginPlay has not run yet; that side resolves when
		// it registers (symmetric), and the audit next tick reads both.
		UACPRTerminalComponent* b = rail->GetTerminalB();
		const bool bCoupled = b && target && b->GetCoupledTo() == target && target->GetCoupledTo() == b;
		if( !bCoupled )
		{
			ACPR_LOG( Error, BP,
				TEXT( "%s construct #%d: %s built but its far end did not couple to %s (B=%s target=%s) — destroyed" ),
				*mHologramName, plan.State, *rail->GetName(), *DescribeWorld( target ),
				b ? *FACPRCoupling::DescribeState( b ) : TEXT( "-" ),
				target ? *FACPRCoupling::DescribeState( target ) : TEXT( "-" ) );
			rail->Destroy();
			DisableBridge( state );
			continue;
		}

		out_ConstructedBridgeBuildables.Add( rail );
		for( AActor* child : children )
		{
			if( AFGBuildable* childBuildable = Cast< AFGBuildable >( child ) )
			{
				out_ConstructedBridgeBuildables.Add( childBuildable );
			}
		}

		++built;
		++mBridgesBuilt;

		// §10.6: "inherits BP-side customization". Checked on the built Rail rather than trusted; if vanilla's
		// ConfigureActor did not carry it across, it is applied here through the colour interface's virtual.
		bool customizationFixed = false;
		if( plan.CustomizationSource &&
			rail->GetCustomizationData_Native() != plan.CustomizationSource->GetCustomizationData_Native() )
		{
			if( IFGColorInterface* colour = Cast< IFGColorInterface >( rail ) )
			{
				colour->SetCustomizationData_Native( plan.CustomizationSource->GetCustomizationData_Native(), false );
				customizationFixed = true;
			}
		}

		const FIntVector cellA = UACPRTerminalComponent::QuantizeLocation( rail->GetActorLocation() );
		const FIntVector cellB = UACPRTerminalComponent::QuantizeLocation(
			rail->GetActorTransform().TransformPosition( FVector( rail->GetLength(), 0.0, 0.0 ) ) );

		ACPR_LOG( Display, BP,
			TEXT( "%s BUILT bridge #%d %s | length=%.1f (asked %.1f) | A=%s B=%s target=%s on %s | "
			      "B coupled=1 | placed-vs-preview drift=%.2f uu | cost=[%s] | swatch=%s fixed=%d | proxy rail=%s host=%s" ),
			*mHologramName, plan.State, *rail->GetName(), rail->GetLength(), state.Length,
			*DescribeCell( cellA ), *DescribeCell( cellB ),
			*DescribeWorld( target ), target ? *DescribeCell( target->GetQuantizedLocation() ) : TEXT( "-" ),
			plan.Drift, *bridgeCost,
			*GetNameSafe( rail->GetCustomizationData_Native().SwatchDesc.Get() ), customizationFixed ? 1 : 0,
			*GetNameSafe( rail->GetBlueprintProxy() ),
			plan.PlacedHost ? *GetNameSafe( plan.PlacedHost->GetBlueprintProxy() ) : TEXT( "-" ) );

		// The audit, next tick — after the placed buildables' BeginPlay: both Couplings, the dismantle group,
		// the customization. Read-only: it reports, it does not repair.
		FBridgeAudit audit;
		audit.Hologram = mHologramName;
		audit.State = plan.State;
		audit.Rail = rail;
		audit.BPHost = plan.PlacedHost;
		audit.BPTerminalIndex = terminal.TerminalIndex;
		audit.Target = state.Target;
		audit.Length = state.Length;
		if( plan.CustomizationSource )
		{
			audit.Customization = plan.CustomizationSource->GetCustomizationData_Native();
		}

		rail->GetWorldTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateLambda( [ audit ]()
			{
				FACPRBlueprintTerminalManager::RunBridgeAudit( audit );
			} ) );
	}

	const FVector location = hologram ? hologram->GetActorLocation() : FVector::ZeroVector;

	ACPR_LOG( Display, BP,
		TEXT( "%s Construct | at %s rot=%s | candidates=%d built=%d refused=%d direct=%d "
		      "shownNotLatched=%d | remapped=%d updates=%d clicks=%d" ),
		*mHologramName, *location.ToString(),
		hologram ? *hologram->GetActorRotation().ToString() : TEXT( "-" ),
		mStates.Num(), built, refused, direct, unlatchedShown,
		mRemapCount, mUpdateCount, mSnapQueryCount );
}

void FACPRBlueprintTerminalManager::RunBridgeAudit( const FBridgeAudit& audit )
{
	FACPRCostScope cost( EACPRCost::ManagerAudit );
	AACPRRail* rail = audit.Rail.Get();
	if( !rail )
	{
		ACPR_LOG( Warning, BP,
			TEXT( "bridge audit %s #%d | the bridge Rail is gone" ), *audit.Hologram, audit.State );
		return;
	}

	UACPRTerminalComponent* a = rail->GetTerminalA();
	UACPRTerminalComponent* b = rail->GetTerminalB();

	AFGBuildable* host = audit.BPHost.Get();
	IACPRTerminalHost* hostInterface = host ? Cast< IACPRTerminalHost >( host ) : nullptr;
	UACPRTerminalComponent* bpTerminal = hostInterface ? hostInterface->GetTerminalAtIndex( audit.BPTerminalIndex ) : nullptr;
	UACPRTerminalComponent* target = audit.Target.Get();

	const bool aCoupled = a && bpTerminal && a->GetCoupledTo() == bpTerminal && bpTerminal->GetCoupledTo() == a;
	const bool bCoupled = b && target && b->GetCoupledTo() == target && target->GetCoupledTo() == b;

	// §10.6: "It joins the placed blueprint's Blueprint Dismantle group". Vanilla does this for
	// out_ConstructedBridgeBuildables; read here, never written (the proxy's list is private for a
	// reason, and a bridge missing from it is a finding, not a thing to paper over).
	const AFGBlueprintProxy* hostProxy = host ? host->GetBlueprintProxy() : nullptr;
	const AFGBlueprintProxy* railProxy = rail->GetBlueprintProxy();
	const bool listed = railProxy && railProxy->GetBuildables().Contains( rail );

	const bool swatchMatches = rail->GetCustomizationData_Native().SwatchDesc == audit.Customization.SwatchDesc;

	ACPR_LOG( Display, BP,
		TEXT( "bridge audit %s #%d | %s length=%.1f | A %s -> %s | B %s -> %s | dismantle group: "
		      "proxy=%s hostProxy=%s listed=%d | swatch rail=%s bp=%s match=%d" ),
		*audit.Hologram, audit.State, *rail->GetName(), rail->GetLength(),
		aCoupled ? TEXT( "COUPLED" ) : TEXT( "NOT coupled" ),
		*DescribeWorld( bpTerminal ),
		bCoupled ? TEXT( "COUPLED" ) : TEXT( "NOT coupled" ),
		*DescribeWorld( target ),
		*GetNameSafe( railProxy ), *GetNameSafe( hostProxy ), listed ? 1 : 0,
		*GetNameSafe( rail->GetCustomizationData_Native().SwatchDesc.Get() ),
		*GetNameSafe( audit.Customization.SwatchDesc.Get() ), swatchMatches ? 1 : 0 );

	if( !aCoupled || !bCoupled )
	{
		ACPR_LOG( Warning, BP,
			TEXT( "bridge audit %s #%d | %s is not coupled at both ends: A=%s B=%s | bp=%s target=%s" ),
			*audit.Hologram, audit.State, *rail->GetName(),
			a ? *FACPRCoupling::DescribeState( a ) : TEXT( "-" ),
			b ? *FACPRCoupling::DescribeState( b ) : TEXT( "-" ),
			bpTerminal ? *FACPRCoupling::DescribeState( bpTerminal ) : TEXT( "-" ),
			target ? *FACPRCoupling::DescribeState( target ) : TEXT( "-" ) );
	}

	if( hostProxy && railProxy != hostProxy )
	{
		ACPR_LOG( Warning, BP,
			TEXT( "bridge audit %s #%d | %s is not in the placed blueprint's dismantle group (rail proxy=%s host proxy=%s)" ),
			*audit.Hologram, audit.State, *rail->GetName(), *GetNameSafe( railProxy ), *GetNameSafe( hostProxy ) );
	}
}

void FACPRBlueprintTerminalManager::SerializeConstructMessage( FArchive& ar, FNetConstructionID /*id*/ )
{
	// Multiplayer, untested. A client's blueprint hologram is serialized to the server, which constructs
	// from its own hologram and its own managers — so the latched picture has to travel, or the server
	// builds the blueprint with no bridges. What travels per latched state: which BP terminal, the latched
	// target component (an object reference the archive resolves through the net package map), and the
	// length. The server side rebuilds mStates from it in PostConstructMessageDeserialization; Construct
	// then revalidates everything against the placed geometry, as for a local placement. Symmetric: the
	// same code writes and reads.
	int32 count = 0;
	if( ar.IsSaving() )
	{
		for( const FState& state : mStates )
		{
			count += ( state.Latched && state.LatchedTarget.IsValid() ) ? 1 : 0;
		}
	}
	ar << count;

	if( ar.IsSaving() )
	{
		for( const FState& state : mStates )
		{
			if( !( state.Latched && state.LatchedTarget.IsValid() ) )
			{
				continue;
			}
			int32 terminal = state.Terminal;
			UObject* target = state.LatchedTarget.Get();
			double length = state.Length;
			ar << terminal;
			ar << target;
			ar << length;
		}
	}
	else
	{
		mDeserializedLatches.Reset();
		for( int32 i = 0; i < count; ++i )
		{
			FDeserializedLatch latch;
			UObject* target = nullptr;
			ar << latch.Terminal;
			ar << target;
			ar << latch.Length;
			latch.Target = Cast< UACPRTerminalComponent >( target );
			mDeserializedLatches.Add( latch );
		}
	}
}

void FACPRBlueprintTerminalManager::PostConstructMessageDeserialization()
{
	// The server's manager: latch what the client latched, on the states Initialize built here. Untested.
	int32 applied = 0;
	for( const FDeserializedLatch& latch : mDeserializedLatches )
	{
		for( FState& state : mStates )
		{
			if( state.Terminal != latch.Terminal )
			{
				continue;
			}
			state.Latched = true;
			state.LatchedTarget = latch.Target;
			state.Target = latch.Target;
			state.Length = latch.Length;
			state.Kind = EVerdict::Bridge;
			state.Reason = EReason::LatchedValid;
			++applied;
		}
	}

	ACPR_LOG( Display, BP,
		TEXT( "%s construct message deserialized | latches=%d applied=%d states=%d (multiplayer path, untested)" ),
		*mHologramName, mDeserializedLatches.Num(), applied, mStates.Num() );
	mDeserializedLatches.Reset();
}

void FACPRBlueprintTerminalManager::ResetAutomaticConnections()
{
	// Vanilla's reset (FGBlueprintOpenConnectionManager.h:609): drop every target and every latch, and
	// hide every bridge.
	int32 wasLatched = 0;
	int32 wasShown = 0;

	for( FState& state : mStates )
	{
		wasLatched += state.Latched ? 1 : 0;
		wasShown += state.BridgeEnabled ? 1 : 0;

		state.Latched = false;
		state.LatchedTarget = nullptr;
		state.Target = nullptr;
		state.Kind = EVerdict::None;
		state.Reason = EReason::None;
		state.Fault = EPairFault::None;
		state.LoggedReason = EReason::None;
		state.LoggedTarget = nullptr;
		state.LoggedBridgeSet = false;
		state.BridgeValid = false;

		if( AACPRRailHologram* bridge = state.Bridge.Get() )
		{
			bridge->SetBridgeLatched( false );
		}
		DisableBridge( state );
	}

	// Vanilla's template broadcasts its own reset (FGBlueprintOpenConnectionManager.h:637) so the hologram takes
	// its icons down; the states are cleared above, so this says "no longer valid" for every one that was.
	for( int32 i = 0; i < mStates.Num(); ++i )
	{
		BroadcastConnectionState( i, mStates[ i ] );
	}

	HideAllIcons();

	mHasEvaluated = false;
	mDriven = false;   // until the next update says otherwise
	++mResetCount;

	if( wasLatched + wasShown > 0 )
	{
		ACPR_LOG( Display, BP,
			TEXT( "%s reset | released latched=%d hid shown=%d" ), *mHologramName, wasLatched, wasShown );
	}
	else
	{
		ACPR_LOG( Verbose, BP, TEXT( "%s reset" ), *mHologramName );
	}
}

FString FACPRBlueprintTerminalManager::DescribeCost( const TArray< FItemAmount >& cost )
{
	FString text;
	for( const FItemAmount& amount : cost )
	{
		text += FString::Printf( TEXT( "%dx%s " ), amount.Amount, *GetNameSafe( amount.ItemClass.Get() ) );
	}
	return text.TrimEnd();
}

FString FACPRBlueprintTerminalManager::DescribeParentDisqualifiers() const
{
	// AFGHologram::mConstructDisqualifiers (FGHologram.h:772, protected), through the Friend entry. Printed
	// at every click so §10.2 step 4's parent-level block can be seen doing its job — or not.
	const AFGBlueprintHologram* hologram = GetHologram();
	if( !hologram )
	{
		return TEXT( "-" );
	}

	FString names;
	for( const TSubclassOf< UFGConstructDisqualifier >& disqualifier : hologram->mConstructDisqualifiers )
	{
		names += GetNameSafe( disqualifier.Get() );
		names += TEXT( " " );
	}
	return names.IsEmpty() ? FString( TEXT( "<none>" ) ) : names.TrimEnd();
}

FString FACPRBlueprintTerminalManager::DescribeTerminal( int32 terminal ) const
{
	if( !mBPTerminals.IsValidIndex( terminal ) )
	{
		return TEXT( "?" );
	}

	const FBPTerminal& entry = mBPTerminals[ terminal ];
	const AFGBuildable* buildable = entry.Buildable.Get();
	return FString::Printf( TEXT( "bp#%d %s t%d" ), entry.BuildableIndex,
		buildable ? *buildable->GetName() : TEXT( "<gone>" ), entry.TerminalIndex );
}

FString FACPRBlueprintTerminalManager::DescribeWorld( const UACPRTerminalComponent* terminal )
{
	if( !terminal )
	{
		return TEXT( "<none>" );
	}

	return FString::Printf( TEXT( "%s.%s" ),
		terminal->GetOwner() ? *terminal->GetOwner()->GetName() : TEXT( "?" ), *terminal->GetName() );
}

FString FACPRBlueprintTerminalManager::DescribeCell( const FIntVector& cell )
{
	return FString::Printf( TEXT( "(%d,%d,%d)" ), cell.X, cell.Y, cell.Z );
}

const TCHAR* FACPRBlueprintTerminalManager::DescribePairFault( EPairFault fault )
{
	switch( fault )
	{
		case EPairFault::None:              return TEXT( "-" );
		case EPairFault::TargetGone:        return TEXT( "target gone" );
		case EPairFault::NotAhead:          return TEXT( "target is not ahead of the BP terminal" );
		case EPairFault::FarEndMisses:      return TEXT( "far end lands off the target's cell" );
		case EPairFault::TargetNotOpposing: return TEXT( "target outward does not oppose the bridge's far end" );
		case EPairFault::BPNotOpposing:     return TEXT( "BP outward does not oppose the bridge's near end" );
		case EPairFault::TooShort:          return TEXT( "too short for a Rail" );
		case EPairFault::TooLong:           return TEXT( "too long for a Rail" );
		case EPairFault::TargetNotOpen:     return TEXT( "target is no longer open" );
		case EPairFault::OtherSpace:        return TEXT( "target is in another build space (10.1)" );
		case EPairFault::RayLost:           return TEXT( "ray no longer reaches the target" );
		case EPairFault::Occluded:          return TEXT( "occluded" );
	}
	return TEXT( "?" );
}

FString FACPRBlueprintTerminalManager::DescribeVerdict( const FState& state ) const
{
	const UACPRTerminalComponent* target = state.Target.Get();
	const UACPRTerminalComponent* blocker = state.Blocker.Get();

	switch( state.Reason )
	{
		case EReason::None:           return TEXT( "not evaluated" );
		case EReason::NoPreviewRoot:  return TEXT( "no preview root" );
		case EReason::NoRegistry:     return TEXT( "no registry" );
		case EReason::Direct:
			return FString::Printf( TEXT( "DIRECT -> %s (6.1 couples it at registration)" ), *DescribeWorld( target ) );
		case EReason::AmbiguousCoincident:
			return FString::Printf( TEXT( "AMBIGUOUS coincidence (%d in cell - invariant 8)" ), state.Aux );
		case EReason::CoincidentRefused:
			return FString::Printf( TEXT( "COINCIDENT BUT REFUSED %s state=%s space=%s" ),
				*DescribeWorld( blocker ),
				blocker ? *FACPRCoupling::DescribeState( blocker ) : TEXT( "-" ),
				blocker ? *FACPRSpace::Describe( FACPRSpace::OfActor( blocker->GetOwner() ) ) : TEXT( "-" ) );
		case EReason::NoneInRange:
			return FString::Printf( TEXT( "none within %.0f uu (examined %d)" ), mRailMax, state.Aux );
		case EReason::AmbiguousRay:
			return FString::Printf( TEXT( "AMBIGUOUS: %d terminals at %.1f uu (10.4 step 7)" ), state.Aux, state.T );
		case EReason::BlockedByBPTerminal:
			return FString::Printf( TEXT( "BLOCKED at %.1f uu by blueprint terminal %s (10.3)" ), state.T, *DescribeTerminal( state.Aux ) );
		case EReason::BlockedSpace:
			return FString::Printf( TEXT( "BLOCKED at %.1f uu by %s in space %s (10.1)" ), state.T, *DescribeWorld( blocker ),
				blocker ? *FACPRSpace::Describe( FACPRSpace::OfActor( blocker->GetOwner() ) ) : TEXT( "-" ) );
		case EReason::BlockedNotOpen:
			return FString::Printf( TEXT( "BLOCKED at %.1f uu by %s state=%s (10.4 step 8)" ), state.T, *DescribeWorld( blocker ),
				blocker ? *FACPRCoupling::DescribeState( blocker ) : TEXT( "-" ) );
		case EReason::PairFault:
			return FString::Printf( TEXT( "BLOCKED at %.1f uu by %s: %s (cell=%s value=%.1f) (10.4)" ), state.T, *DescribeWorld( blocker ),
				DescribePairFault( state.Fault ), *DescribeCell( state.AuxCell ), state.AuxValue );
		case EReason::Bridge:
			return FString::Printf( TEXT( "BRIDGE %.1f uu -> %s" ), state.Length, *DescribeWorld( target ) );
		case EReason::Conflict:
			return FString::Printf( TEXT( "CONFLICT: target claimed by #%d (10.5)" ), state.Aux );
		case EReason::NoBridgeHologram:
			return TEXT( "no bridge hologram" );
		case EReason::FailsValidation:
			return FString::Printf( TEXT( "BRIDGE -> %s fails Rail validation [%s]" ), *DescribeWorld( target ), *state.BridgeDisqualifiers );
		case EReason::LatchedCoincident:
			return FString::Printf( TEXT( "LATCHED -> %s, now COINCIDENT (6.1 couples it; no Rail)" ), *DescribeWorld( target ) );
		case EReason::LatchedValid:
			return FString::Printf( TEXT( "LATCHED %.1f uu -> %s" ), state.Length, *DescribeWorld( target ) );
		case EReason::LatchedInvalid:
			if( state.Fault == EPairFault::Occluded )
			{
				return FString::Printf( TEXT( "LATCHED -> %s but INVALID: occluded at %.1f uu by %s" ),
					*DescribeWorld( target ), state.AuxValue,
					blocker ? *DescribeWorld( blocker ) : *DescribeTerminal( state.Aux ) );
			}
			return FString::Printf( TEXT( "LATCHED -> %s but INVALID: %s (cell=%s value=%.1f)" ), *DescribeWorld( target ),
				DescribePairFault( state.Fault ), *DescribeCell( state.AuxCell ), state.AuxValue );
	}
	return TEXT( "?" );
}

void FACPRBlueprintTerminalManager::LogStateLine( int32 index, const FState& state, const TCHAR* why, bool display,
                                                  bool proposal )
{
	// Verbose for the per-frame trail; Display (budgeted) for the events a test is judged by: a proposal
	// appearing or going, a latch, a latched bridge turning red or back, and the construct picture.
	// Proposals have their own budget: sweeping a blueprint across a lane shows and withdraws them in
	// pairs, and must not spend the lines the click and the construct need.
	if( display )
	{
		int32& budget = proposal ? mProposalLogBudget : mStateLogBudget;
		if( budget <= 0 )
		{
			return;
		}
		--budget;

		ACPR_LOG( Display, BP,
			TEXT( "%s %s | #%d %s | %s | bridge=%s shown=%d valid=%d [%s]" ),
			*mHologramName, why, index, *DescribeTerminal( state.Terminal ), *DescribeVerdict( state ),
			*GetNameSafe( state.Bridge.Get() ), state.BridgeEnabled ? 1 : 0, state.BridgeValid ? 1 : 0,
			*state.BridgeDisqualifiers );
		return;
	}

	if( ACPR_LOG_ACTIVE( Verbose ) )
	{
		ACPR_LOG( Verbose, BP,
			TEXT( "%s %s | #%d %s | %s" ),
			*mHologramName, why, index, *DescribeTerminal( state.Terminal ), *DescribeVerdict( state ) );
	}
}

// =====================================================================================================
// §10.6 — the blue two-link icon
// =====================================================================================================

bool FACPRBlueprintTerminalManager::ResolveIconAssets()
{
	if( mIconAssetsResolved )
	{
		return mIconAssetsOk;
	}
	mIconAssetsResolved = true;

	// UFGGlobalSettings::GetFactorySettingsCDO — static UFUNCTION (FGGlobalSettings.h:19), so reflected and
	// safe to call. UFGFactorySettings::Get (FGFactorySettings.h:77) is a plain static and is not used.
	const UFGFactorySettings* settings = UFGGlobalSettings::GetFactorySettingsCDO();

	UStaticMesh* mesh = settings ? settings->mBlueprintAutoConnectionMesh.Get() : nullptr;
	UMaterialInterface* material = settings ? settings->mDefaultAutomaticBlueprintConnectionMaterial.Get() : nullptr;

	mIconMesh = mesh;
	mIconMaterial = material;
	mIconAssetsOk = mesh != nullptr;

	if( mesh )
	{
		const FBoxSphereBounds bounds = mesh->GetBounds();
		ACPR_LOG( Display, BP_ICON,
			TEXT( "%s assets | settings=%s | mesh=%s bounds origin=%s extent=%s | material=%s | "
			      "cvars LinkIconMode=%d scale=%.2f offset=%.1f" ),
			*mHologramName, *GetNameSafe( settings ), *mesh->GetName(),
			*bounds.Origin.ToString(), *bounds.BoxExtent.ToString(), *GetNameSafe( material ),
			static_cast< int32 >( CurrentIconMode() ), CVarACPRLinkIconScale.GetValueOnGameThread(),
			CVarACPRLinkIconOffset.GetValueOnGameThread() );
	}
	else
	{
		ACPR_LOG( Warning, BP_ICON,
			TEXT( "%s | no auto-connection mesh (settings=%s material=%s) — the link icon is off" ),
			*mHologramName, *GetNameSafe( settings ), *GetNameSafe( material ) );
	}

	return mIconAssetsOk;
}

UStaticMeshComponent* FACPRBlueprintTerminalManager::EnsureIcon( int32 index )
{
	if( index < 0 )
	{
		return nullptr;
	}

	if( mIcons.Num() <= index )
	{
		mIcons.SetNum( index + 1 );
	}

	if( UStaticMeshComponent* existing = mIcons[ index ].Get() )
	{
		return existing;
	}

	AFGBlueprintHologram* hologram = GetHologram();
	USceneComponent* root = hologram ? hologram->GetRootComponent() : nullptr;
	if( !root || !ResolveIconAssets() )
	{
		return nullptr;
	}

	// The runtime-component recipe: outer = the hologram, attach, register — the order a runtime scene
	// component wants — and never hit by a trace, never casting a shadow. Attached, the root's
	// AttachChildren keeps it alive for the manager's weak pointer.
	UStaticMeshComponent* icon = NewObject< UStaticMeshComponent >( hologram );
	if( !icon )
	{
		return nullptr;
	}

	icon->SetupAttachment( root );
	icon->RegisterComponent();
	icon->SetStaticMesh( mIconMesh.Get() );

	if( UMaterialInterface* material = mIconMaterial.Get() )
	{
		for( int32 slot = 0; slot < icon->GetNumMaterials(); ++slot )
		{
			icon->SetMaterial( slot, material );
		}
	}

	icon->SetCollisionEnabled( ECollisionEnabled::NoCollision );
	icon->SetCastShadow( false );
	icon->SetVisibility( false );

	mIcons[ index ] = icon;
	++mIconsCreated;

	return icon;
}

void FACPRBlueprintTerminalManager::UpdateIcon( int32 index, const FState& state, const FTerminalFrame* frame )
{
	// "Accepted": a coincident pair (it couples whatever the mode), or a bridge the player can see and build —
	// a shown, valid proposal before the click, a latched valid one after it. A red latched bridge is not
	// accepted (it blocks) and a hidden proposal was never offered.
	const bool accepted = state.Kind == EVerdict::Direct
		|| ( state.Kind == EVerdict::Bridge && state.BridgeEnabled && state.BridgeValid );

	const bool want = accepted && frame != nullptr && CurrentIconMode() == EIconMode::Own;

	UStaticMeshComponent* icon = want ? EnsureIcon( index ) : ( mIcons.IsValidIndex( index ) ? mIcons[ index ].Get() : nullptr );
	if( !icon )
	{
		return;
	}

	if( !want )
	{
		if( icon->IsVisible() )
		{
			icon->SetVisibility( false );
		}
		return;
	}

	// At the BP terminal's snap point, in its frame (+X outward) — the frame a vanilla icon would take from
	// the connection component it is made for. Offset and scale are the CVars above.
	const double offset = static_cast< double >( CVarACPRLinkIconOffset.GetValueOnGameThread() );
	const double scale = FMath::Max( 0.01, static_cast< double >( CVarACPRLinkIconScale.GetValueOnGameThread() ) );

	icon->SetWorldLocationAndRotation( frame->P + frame->D * offset, FRotationMatrix::MakeFromXZ( frame->D, frame->Up ).ToQuat() );
	icon->SetWorldScale3D( FVector( scale ) );

	// Nothing documents whether the blueprint hologram re-materials its components each frame. If it does,
	// the icon would render in the hologram's valid/invalid material instead; put it back, count it, and let
	// the destructor line say whether it keeps happening.
	UMaterialInterface* material = mIconMaterial.Get();
	if( material && icon->GetMaterial( 0 ) != material )
	{
		for( int32 slot = 0; slot < icon->GetNumMaterials(); ++slot )
		{
			icon->SetMaterial( slot, material );
		}

		++mIconMaterialReapplied;
		if( mIconLogBudget > 0 )
		{
			--mIconLogBudget;
			ACPR_LOG( Display, BP_ICON,
				TEXT( "%s #%d material was replaced by something else (now %s) — reapplied (%d so far)" ),
				*mHologramName, index, *GetNameSafe( icon->GetMaterial( 0 ) ), mIconMaterialReapplied );
		}
	}

	if( !icon->IsVisible() )
	{
		icon->SetVisibility( true );
		++mIconShows;

		if( mIconLogBudget > 0 )
		{
			--mIconLogBudget;
			ACPR_LOG( Verbose, BP_ICON,
				TEXT( "%s #%d shown | %s | at %s outward=%s | scale=%.2f offset=%.1f" ),
				*mHologramName, index,
				state.Kind == EVerdict::Direct ? TEXT( "coincident pair" )
					: ( state.Latched ? TEXT( "latched bridge" ) : TEXT( "bridge proposal" ) ),
				*frame->P.ToString(), *frame->D.ToString(), scale, offset );
		}
	}
}

void FACPRBlueprintTerminalManager::HideAllIcons()
{
	for( const TWeakObjectPtr< UStaticMeshComponent >& weak : mIcons )
	{
		if( UStaticMeshComponent* icon = weak.Get() )
		{
			icon->SetVisibility( false );
		}
	}
}

// =====================================================================================================
// §10.6 — vanilla's own blue two-link icon, through the delegate the hologram already subscribed to
// =====================================================================================================

FACPRBlueprintTerminalManager::EIconMode FACPRBlueprintTerminalManager::CurrentIconMode()
{
	const int32 mode = CVarACPRLinkIconMode.GetValueOnGameThread();
	return ( mode >= 0 && mode <= 2 ) ? static_cast< EIconMode >( mode ) : EIconMode::Vanilla;
}

UFGConnectionComponent* FACPRBlueprintTerminalManager::GetBroadcastConnection( int32 index, const FState& state )
{
	if( !mBPTerminals.IsValidIndex( state.Terminal ) )
	{
		return nullptr;
	}

	// The component we made last time, first: on a re-initialise the blueprint-world buildable may already be
	// gone while a state that was broadcast as valid still has to be broadcast as invalid.
	if( mBroadcastDuplicates.IsValidIndex( index ) )
	{
		if( UACPRPowerConnectionComponent* cached = mBroadcastDuplicates[ index ].Get() )
		{
			return cached;
		}
	}

	const FBPTerminal& terminal = mBPTerminals[ state.Terminal ];
	AFGBuildable* buildable = terminal.Buildable.Get();
	IACPRTerminalHost* host = buildable ? Cast< IACPRTerminalHost >( buildable ) : nullptr;
	UACPRPowerConnectionComponent* original = host ? host->GetPowerConnection() : nullptr;

	AFGBlueprintHologram* hologram = GetHologram();
	if( !original || !hologram )
	{
		return original;
	}

	// Vertical Conveyor Auto-Connect's resolution (its DEVELOPMENT.md, "Automatic-connection representation"):
	// the component to broadcast is the hologram's own duplicate of a blueprint-world connection, because the
	// automatic-link icon is drawn on it. Broadcasting the blueprint-world component makes vanilla draw the
	// icon in FactoryBlueprintWorld, where nobody can see it.
	//
	// A host has one power connection, at its midpoint or centre, and a terminal is a plain scene component —
	// so the icon's stand-in is a copy of that connection placed at the terminal's local frame, one per
	// candidate. Made with vanilla's own SetupComponent (below) under a name unique per (buildable, terminal),
	// and registered in the two maps exactly as vanilla registers its own duplicates.
	if( !buildable || !hologram->mBuildableToNewRoot.Contains( buildable ) )
	{
		// mBuildableToNewRoot[] asserts on a missing key, so this is checked rather than risked.
		if( mBroadcastLogBudget > 0 )
		{
			--mBroadcastLogBudget;
			ACPR_LOG( Warning, BP_ICON,
				TEXT( "%s #%d: no preview root for %s — broadcasting the blueprint-world connection, "
				      "whose icon will be drawn in the blueprint world" ),
				*mHologramName, index, *GetNameSafe( buildable ) );
		}
		return original;
	}

	// SetupComponent, not SetupBuildableComponent: the latter makes nothing of a power connection template,
	// while the former is exactly what vanilla's own DuplicateConnectionComponent template calls
	// (FGBlueprintHologram.h:173). Protected on AFGHologram (:595), reached through the Friend on the
	// derived class, like mBlueprintDesigner.
	const FName name( *FString::Printf( TEXT( "%s_%s_T%d" ), *buildable->GetName(), *original->GetName(), terminal.TerminalIndex ) );
	UACPRPowerConnectionComponent* duplicate = Cast< UACPRPowerConnectionComponent >(
		hologram->SetupComponent( hologram->mBuildableToNewRoot[ buildable ], original, name, NAME_None ) );

	if( !duplicate )
	{
		if( mBroadcastLogBudget > 0 )
		{
			--mBroadcastLogBudget;
			ACPR_LOG( Warning, BP_ICON,
				TEXT( "%s #%d: SetupComponent made no power connection for %s — using the "
				      "blueprint-world connection" ),
				*mHologramName, index, *GetNameSafe( original ) );
		}
		return original;
	}

	// Archetype-copied from a live connection: emptied of everything electrical (see MakePreviewCopyInert),
	// and moved from the host's midpoint to the terminal, where the icon belongs.
	duplicate->MakePreviewCopyInert();
	duplicate->SetRelativeTransform( terminal.LocalFrame );

	hologram->mDuplicateConnectionToOriginalMap.Add( duplicate, original );
	if( !hologram->mConnectionRepresentationMeshes.Contains( duplicate ) )
	{
		hologram->mConnectionRepresentationMeshes.Add( duplicate, TArray< UStaticMeshComponent* >() );
	}

	if( mBroadcastDuplicates.Num() <= index )
	{
		mBroadcastDuplicates.SetNum( index + 1 );
	}
	mBroadcastDuplicates[ index ] = duplicate;
	++mDuplicatesMade;

	if( mBroadcastLogBudget > 0 && ACPR_LOG_ACTIVE( Verbose ) )
	{
		--mBroadcastLogBudget;
		ACPR_LOG( Verbose, BP_ICON,
			TEXT( "%s #%d preview stand-in %s at %s (world=%s) | original %s at %s (world=%s)" ),
			*mHologramName, index, *duplicate->GetName(), *duplicate->GetComponentLocation().ToString(),
			*GetNameSafe( duplicate->GetWorld() ),
			*original->GetName(), *original->GetComponentLocation().ToString(),
			*GetNameSafe( original->GetWorld() ) );
	}

	return duplicate;
}

void FACPRBlueprintTerminalManager::BroadcastConnectionState( int32 index, FState& state )
{
	const bool vanilla = CurrentIconMode() == EIconMode::Vanilla;

	// The same "accepted" test the own-mesh icon uses: a coincident pair, or a bridge shown and valid.
	const bool accepted = vanilla
		&& ( state.Kind == EVerdict::Direct
			|| ( state.Kind == EVerdict::Bridge && state.BridgeEnabled && state.BridgeValid ) );

	UACPRTerminalComponent* target = state.Target.Get();

	if( accepted == state.BroadcastValid && target == state.BroadcastTarget.Get() )
	{
		return;
	}

	// Nothing to say if we have never said anything and there is nothing to say now.
	if( !accepted && !state.BroadcastValid )
	{
		state.BroadcastTarget = accepted ? target : nullptr;
		return;
	}

	UFGConnectionComponent* connection = GetBroadcastConnection( index, state );
	if( !connection )
	{
		// Nothing to broadcast with — the blueprint-world buildable is gone and no duplicate was ever made.
		// The bookkeeping is cleared anyway, so this is not retried on every frame.
		state.BroadcastValid = false;
		state.BroadcastTarget = nullptr;
		return;
	}

	// Exactly the template's call (FGBlueprintOpenConnectionManager.h:492): the state's open connections, the
	// previous target, the new one, and whether the pair is valid. The delegate speaks in connection
	// components; a target terminal is geometry, so the target host's one power connection
	// stands for it — vanilla draws the icon on our side's component, the target is bookkeeping to it.
	auto connectionOf = []( const UACPRTerminalComponent* terminal ) -> UFGConnectionComponent*
	{
		const IACPRTerminalHost* host = terminal ? Cast< IACPRTerminalHost >( terminal->GetOwner() ) : nullptr;
		return host ? host->GetPowerConnection() : nullptr;
	};

	TArray< UFGConnectionComponent* > connections;
	connections.Add( connection );

	mOnConnectionStateChanged.Broadcast( connections, connectionOf( state.BroadcastTarget.Get() ),
		accepted ? connectionOf( target ) : nullptr, accepted );
	++mBroadcasts;

	state.BroadcastValid = accepted;
	state.BroadcastTarget = accepted ? target : nullptr;

	LogVanillaIcon( index, connection, accepted );
}

void FACPRBlueprintTerminalManager::LogVanillaIcon( int32 index, const UFGConnectionComponent* connection, bool accepted )
{
	if( mBroadcastLogBudget <= 0 || !ACPR_LOG_ACTIVE( Verbose ) )
	{
		return;
	}
	--mBroadcastLogBudget;

	AFGBlueprintHologram* hologram = GetHologram();
	UFGConnectionComponent* key = const_cast< UFGConnectionComponent* >( connection );

	// The hologram's own private maps, through the AccessTransformers Friend this manager already has: did
	// vanilla make an icon for the component it was handed, and where is it.
	const UStaticMeshComponent* icon = nullptr;
	int32 arrowMeshes = 0;
	const TCHAR* kind = TEXT( "no hologram" );
	if( hologram )
	{
		// Which of VCAC's three cases this is, which decides whether vanilla can draw anything:
		// a duplicate the hologram drew an indicator for (the icon replaces it), a duplicate without one, or
		// no duplicate at all — a circuit connection the blueprint hologram never duplicated.
		const bool isDuplicate = hologram->mDuplicateConnectionToOriginalMap.Contains( key );
		const bool hasIndicator = hologram->mConnectionRepresentationMeshes.Contains( key );
		kind = isDuplicate
			? ( hasIndicator ? TEXT( "preview duplicate with indicator" ) : TEXT( "preview duplicate, no indicator" ) )
			: ( hasIndicator ? TEXT( "blueprint-world, has indicator" ) : TEXT( "blueprint-world, no duplicate" ) );

		if( const TObjectPtr< UStaticMeshComponent >* found = hologram->mAutomaticConnectionRepresentationMap.Find( key ) )
		{
			icon = found->Get();
		}
		if( const TArray< UStaticMeshComponent* >* arrows = hologram->mConnectionRepresentationMeshes.Find( key ) )
		{
			arrowMeshes = arrows->Num();
		}
	}

	ACPR_LOG( Verbose, BP_ICON,
		TEXT( "%s #%d broadcast %s | mode=%d | %s | connection=%s (owner=%s world=%s at %s) "
		      "| vanilla icon=%s at %s visible=%d attachedTo=%s world=%s | indicatorMeshes=%d" ),
		*mHologramName, index, accepted ? TEXT( "VALID" ) : TEXT( "invalid" ),
		static_cast< int32 >( CurrentIconMode() ),
		kind,
		*GetNameSafe( connection ),
		connection ? *GetNameSafe( connection->GetOwner() ) : TEXT( "-" ),
		connection ? *GetNameSafe( connection->GetWorld() ) : TEXT( "-" ),
		connection ? *connection->GetComponentLocation().ToString() : TEXT( "-" ),
		*GetNameSafe( icon ),
		icon ? *icon->GetComponentLocation().ToString() : TEXT( "-" ),
		icon ? ( icon->IsVisible() ? 1 : 0 ) : -1,
		icon ? *GetNameSafe( icon->GetAttachParent() ) : TEXT( "-" ),
		icon ? *GetNameSafe( icon->GetWorld() ) : TEXT( "-" ),
		arrowMeshes );
}
