// Auto-Connecting Power Rails — the Power Rail buildable.

#include "ACPRRail.h"

#include "ACPRAttachment.h"
#include "ACPRSpec.h"
#include "FGAttachmentPointComponent.h"

#include "ACPRCap.h"
#include "ACPRDesignerSpace.h"
#include "ACPROutlet.h"
#include "ACPRPowerConnectionComponent.h"
#include "ACPRTerminalComponent.h"
#include "ACPRTerminalHost.h"
#include "FGColoredInstanceMeshProxy.h"
#include "FGSwatchGroup.h"
#include "ACPRTerminalRegistry.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "FGBuildableSubsystem.h"
#include "FGRecipe.h"
#include "Components/SceneComponent.h"
// LogMeshSources calls GetStaticMesh() on each component it finds.
#include "Components/StaticMeshComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Kismet/KismetSystemLibrary.h"
#include "TimerManager.h"
#include "FGPowerCircuit.h"
#include "FGPowerConnectionComponent.h"
#include "InstanceData.h"
#include "Net/UnrealNetwork.h"

AACPRRail::AACPRRail()
{
	// §14: a real actor that owns saved, replicated topology. FGBuildable.h:606 makes these
	// independent:
	//
	//   ShouldConvertToLightweight() { return HasAuthority() && GetLightweightInstanceData()
	//       && ManagedByLightweightBuildableSubsystem() && mBlueprintDesigner == nullptr; }
	//
	// Both go off, as on the other three buildables. The Rail renders through its own mesh
	// component (B.2), so it has no instance data to carry, and clearing the second alone would
	// already keep it a real actor.
	mCanContainLightweightInstances = false;
	mManagedByLightweightBuildableSubsystem = false;

	// Every connection component is created here, on the CDO. One rule, no exceptions: anything
	// vanilla enumerates or holds a reference to must exist on the CDO. Electrical correctness is
	// not enough, for two reasons:
	//
	//   * the Hoverpack does not see a connection component created at runtime, however powered
	//     its circuit is;
	//   * an AFGBuildableWire stores a reference to the connection component it ends on, and the
	//     save system resolves such a reference by finding the component on the already-constructed
	//     actor. A component created in BeginPlay does not exist at that moment, so the reference
	//     resolves to nothing and the wire is left dangling from one end after a reload.
	//
	// A connection component earns no mesh in the hologram, so none of these shadows the Beam
	// hologram's first-match mBeamMesh bind the way a UStaticMeshComponent would.
	mTerminalA = CreateDefaultSubobject< UACPRTerminalComponent >( TEXT( "ACPRTerminalA" ) );
	mTerminalB = CreateDefaultSubobject< UACPRTerminalComponent >( TEXT( "ACPRTerminalB" ) );

	// The one power connection. Hidden, no wire budget (§4: Power Lines reach the
	// network only through an Outlet), at the midpoint from BeginPlay: the Hoverpack's node and the
	// end of every Coupling's hidden edge. Its NAME is the Wall Outlet's connection component's
	// (§15): a save references a connection by actor and component name, so a Rail redirected to a
	// Wall Outlet after the mod is removed keeps its circuit and its couplings only if the name
	// resolves there.
	mPower = CreateDefaultSubobject< UACPRPowerConnectionComponent >( ACPR_POWER_CONNECTION_NAME );

	// Terminal A as a vanilla attachment point, on the CDO so the hologram caches it as
	// its local point. Same frame as terminal A (origin, yaw 180: outward −X). Type and usage are set by
	// reflection (FACPRAttachment::ConfigureComponent); B is computed, see GetAttachmentPoints.
	mAttachmentA = CreateDefaultSubobject< UFGAttachmentPointComponent >( TEXT( "ACPRAttachA" ) );
	if( mAttachmentA )
	{
		mAttachmentA->SetRelativeRotation( FRotator( 0.0f, 180.0f, 0.0f ) );
		FACPRAttachment::ConfigureComponent( mAttachmentA, true );
	}

	// The renderer. See the note on mRailMeshComponent: the instance-data hooks are never called on
	// a Rail, so it draws from its own components, the same pattern the Junction, Outlet and Cap
	// render from.
	//
	// Movable rather than Static, because the length is only known at BeginPlay and scaling a
	// Static component after registration is what produces "Mobility is Static" warnings. The
	// Rail never actually moves.
	//
	// BuildingMesh is vanilla's own collision profile for a buildable's mesh (the one the blueprint's
	// instance data names). Without collision the Rail could not be targeted for dismantle and
	// nothing could be built onto it.
	//
	// The default swatch is the Foundation's. AFGBuildable::mSwatchGroup decides which of the
	// player's per-category default swatches a fresh build wears; the Standard group's is FICSIT
	// orange, and an orange accent on an unpainted Rail fights the amber and white of the
	// indicators, so the colours stop meaning anything. The Foundation group's default is
	// the dark grey every foundation wears, so an unpainted ACPR part reads as dark infrastructure
	// and the accent only appears when someone paints it. Any swatch still applies.
	// UFGSwatchGroup_FicsitFoundation: FGSwatchGroup.h:65. A blueprint that overrides mSwatchGroup
	// wins, and the push line's swatch= field says which one took.
	mSwatchGroup = UFGSwatchGroup_FicsitFoundation::StaticClass();

	mRailMeshComponent = CreateDefaultSubobject< UFGColoredInstanceMeshProxy >( TEXT( "RailMesh" ) );
	mTerminalMeshA = CreateDefaultSubobject< UFGColoredInstanceMeshProxy >( TEXT( "TerminalMeshA" ) );
	mTerminalMeshB = CreateDefaultSubobject< UFGColoredInstanceMeshProxy >( TEXT( "TerminalMeshB" ) );
	for( UFGColoredInstanceMeshProxy* proxy : { mRailMeshComponent.Get(), mTerminalMeshA.Get(), mTerminalMeshB.Get() } )
	{
		FACPRIndicators::ConfigureProxy( proxy );
	}

	for( UStaticMeshComponent* piece : { mRailMeshComponent.Get(), mTerminalMeshA.Get(),
	                                     mTerminalMeshB.Get() } )
	{
		if( piece )
		{
			piece->SetMobility( EComponentMobility::Movable );
			piece->SetCollisionProfileName( TEXT( "BuildingMesh" ) );
		}
	}

	// RootComponent is normally null in a buildable's C++ constructor — the root is authored on the
	// Blueprint leaf — so this is a best effort and BeginPlay finishes the job for whatever is left
	// unattached. root= in the topology log says which of the two actually happened.
	if( USceneComponent* root = GetRootComponent() )
	{
		if( mTerminalA ) { mTerminalA->SetupAttachment( root ); }
		if( mTerminalB ) { mTerminalB->SetupAttachment( root ); }
		if( mPower )     { mPower->SetupAttachment( root ); }

		if( mRailMeshComponent ) { mRailMeshComponent->SetupAttachment( root ); }
		if( mTerminalMeshA )     { mTerminalMeshA->SetupAttachment( root ); }
		if( mTerminalMeshB )     { mTerminalMeshB->SetupAttachment( root ); }
	}
}

void AACPRRail::GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );
	DOREPLIFETIME( AACPRRail, mTerminalStateRep );
	DOREPLIFETIME( AACPRRail, mHasPowerRep );
}

bool AACPRRail::HasNetworkPower() const
{
	if( !HasAuthority() )
	{
		return mHasPowerRep;
	}
	return mPower && mPower->HasPower();
}

void AACPRRail::CaptureReplicatedState()
{
	if( !HasAuthority() )
	{
		return;
	}
	mTerminalStateRep.SetNum( 2 );
	mTerminalStateRep[ 0 ] = static_cast< uint8 >( mTerminalA ? mTerminalA->GetTerminalState() : EACPRTerminalState::Open );
	mTerminalStateRep[ 1 ] = static_cast< uint8 >( mTerminalB ? mTerminalB->GetTerminalState() : EACPRTerminalState::Open );
	mHasPowerRep = mPower && mPower->HasPower();
}

void AACPRRail::OnRep_TerminalState()
{
	// The client's event: the host's state arrived. Feed the terminals (their GetTerminalState reads
	// this on a client) and push once.
	UACPRTerminalComponent* terminals[ 2 ] = { mTerminalA, mTerminalB };
	for( int32 i = 0; i < 2 && i < mTerminalStateRep.Num(); ++i )
	{
		if( terminals[ i ] )
		{
			terminals[ i ]->SetReplicatedState( static_cast< EACPRTerminalState >( mTerminalStateRep[ i ] ) );
		}
	}
	RefreshIndicators();
}

void AACPRRail::OnTerminalStateChanged( int32 index )
{
	// The event. A terminal's occupant changed (SetCoupledTo / SetCapped), the network's power
	// changed (HandleHasPowerChanged, index INDEX_NONE) or a client's replicated copy changed.
	// Everything state-driven hangs here, and nothing polls: the cue materials, the collar's bridged
	// variant, and the replicated state a client is told.
	if( HasActorBegunPlay() )
	{
		if( index == 0 || index == 1 )
		{
			UpdateCollarMesh( index );
		}
		CaptureReplicatedState();
		RefreshIndicators();
	}
}

void AACPRRail::HandleHasPowerChanged( bool /*hasPower*/ )
{
	OnTerminalStateChanged( INDEX_NONE );
}

void AACPRRail::GetDismantleRefund_Implementation( TArray< FInventoryStack >& out_refund,
                                                   bool noBuildCostEnabled ) const
{
	// §7.5: "Dismantle previews always show actual stored refunds" (§13), and "the preview shows the
	// stored figure". So a Rail that carries a ledger reports it verbatim and never recomputes from
	// its own length — which is the whole guarantee: aggregate refund across every dismantle order
	// equals the original's exact paid ledger, however many times it has since been split.
	//
	// No ledger is the ordinary case and falls through. A hand-built Rail has none, so vanilla's
	// recipe calculation applies — and §10.1 requires the same of a blueprinted one, which is
	// "costed from the recipe rather than stored child ledgers". Nothing here has to know about
	// blueprints for that to hold: a blueprint reconstructs Rails, and a reconstructed Rail has no
	// ledger to carry.
	//
	// The flag, not the count. An empty ledger has two possible meanings and only one of them is
	// "recompute from the recipe": a Rail that was never split has no ledger, and a child that was
	// split and drew the short straw has an empty one. Reading the count would conflate them, and
	// the second case would refund a full fresh Rail — twice the original's cost across the pair.
	if( !mHasStoredLedger || noBuildCostEnabled )
	{
		Super::GetDismantleRefund_Implementation( out_refund, noBuildCostEnabled );
		return;
	}

	for( const FInventoryStack& stack : mStoredLedger )
	{
		if( stack.HasItems() )
		{
			out_refund.Add( stack );
		}
	}
}

void AACPRRail::SetStoredLedger( const TArray< FInventoryStack >& ledger )
{
	mStoredLedger = ledger;

	// The assignment is the fact, not its contents. See mHasStoredLedger: an empty share is a
	// legitimate outcome of §7.5's division and must still suppress the recipe fallback.
	mHasStoredLedger = true;
}

double AACPRRail::GetOffsetAlong( const FVector& worldLocation ) const
{
	// Local +X is the beam axis (B.2). Terminal A sits at the actor origin and terminal B at
	// origin + localX * mLength, which is what makes this one dot product rather than an inverse
	// transform.
	return FVector::DotProduct( worldLocation - GetActorLocation(),
	                            GetActorQuat().GetAxisX() );
}

void AACPRRail::CaptureLedgerFromRecipe()
{
	if( mHasStoredLedger )
	{
		// Already carries one, from an earlier split. §7.5: "repeated splits subdivide the stored
		// child ledger rather than a fresh recipe" — so the seed must never overwrite. Keyed on the
		// flag rather than on the count, or a child that legitimately received an empty share would
		// be re-seeded from the recipe on its next split and invent material.
		return;
	}

	const TSubclassOf< UFGRecipe > recipe = GetBuiltWithRecipe();
	if( !recipe )
	{
		// Nothing to seed from. The split still proceeds and both children simply carry no ledger,
		// which falls back to vanilla's per-length recipe calculation in GetDismantleRefund — the
		// same behaviour every unsplit Rail already has. Worth a line, because it means §7.5's
		// aggregate guarantee is not being kept for this Rail and the reason is upstream of us.
		ACPR_LOG( Warning, RAIL,
			TEXT( "%s has no mBuiltWithRecipe, so the split ledger cannot be seeded. "
			      "The children will refund from the recipe by length instead." ),
			*GetName() );
		return;
	}

	// The multiplier is computed here rather than asked for, and the reason is linking rather than
	// arithmetic. AFGBuildableBeam::GetDismantleReturnsMultiplierForBeam (FGBuildableBeam.h:71) says
	// exactly this and would be the obvious call — but it is a plain exported method with no
	// reflection, and the shipping game does not export that category of symbol: the DLL fails to
	// load with GetLastError=127. Reading a protected field of our own base class generates no
	// import at all.
	//
	// The formula is vanilla's and B.2 records it: the beam cost path applies one integer multiplier
	// derived from mLengthPerCost to the whole recipe. §13's ⌈L / 10 m⌉ is mLengthPerCost = 1000.
	const double perCost = ( mLengthPerCost > 0.0f ) ? static_cast< double >( mLengthPerCost ) : 1000.0;
	const int32 multiplier = FMath::Max( 1,
		FMath::CeilToInt32( static_cast< double >( GetLength() ) / perCost ) );

	const TArray< FItemAmount > ingredients = UFGRecipe::GetIngredients( this, recipe );

	mStoredLedger.Reset();
	mHasStoredLedger = true;

	for( const FItemAmount& ingredient : ingredients )
	{
		if( !ingredient.ItemClass || ingredient.Amount <= 0 )
		{
			continue;
		}

		mStoredLedger.Add( FInventoryStack( ingredient.Amount * multiplier, ingredient.ItemClass ) );
	}

	FString printed;
	for( const FInventoryStack& stack : mStoredLedger )
	{
		printed += FString::Printf( TEXT( "%dx%s " ),
			stack.NumItems,
			stack.Item.GetItemClass() ? *stack.Item.GetItemClass()->GetName() : TEXT( "<none>" ) );
	}

	ACPR_LOG( Verbose, RAIL,
		TEXT( "%s ledger seeded | recipe=%s length=%.1f perCost=%.1f multiplier=%d "
		      "-> [ %s]" ),
		*GetName(), *recipe->GetName(), GetLength(), perCost, multiplier, *printed );
}

TArray< AACPRRail* > AACPRRail::Split( AACPRRail* rail, double centreAlong, double halfExtent )
{
	TArray< AACPRRail* > children;

	if( !IsValid( rail ) )
	{
		return children;
	}

	UWorld* world = rail->GetWorld();
	AFGBuildableSubsystem* subsystem = world ? AFGBuildableSubsystem::Get( world ) : nullptr;
	if( !subsystem )
	{
		ACPR_LOG( Error, RAIL,
			TEXT( "Split(%s) found no buildable subsystem — nothing was changed." ),
			*rail->GetName() );
		return children;
	}

	const double total = rail->GetLength();
	const double minimum = FMath::Max( 1.0, static_cast< double >( rail->GetSize() ) );

	const double nearLength = centreAlong - halfExtent;
	const double farStart = centreAlong + halfExtent;
	const double farLength = total - farStart;

	// "Callee is responsible for not creating snakes or 'zero belts'" — the conveyor precedent's own
	// warning, and §7.3's "the shortest insertable host is therefore 3 m" is this arithmetic: a 1 m
	// cell between two 1 m minimum children. The hologram refuses long before here; this is the
	// guard that makes the function safe to call from anywhere, including a future blueprint path.
	if( nearLength < minimum || farLength < minimum )
	{
		ACPR_LOG( Warning, RAIL,
			TEXT( "Split(%s) refused | total=%.1f centre=%.1f half=%.1f -> near=%.1f "
			      "far=%.1f | minimum=%.1f" ),
			*rail->GetName(), total, centreAlong, halfExtent, nearLength, farLength, minimum );
		return children;
	}

	// §7.5's ledger, captured before anything is spawned or destroyed, because it is read off the
	// original and the original is about to go.
	rail->CaptureLedgerFromRecipe();

	TArray< FInventoryStack > nearLedger;
	TArray< FInventoryStack > farLedger;

	// The remainder's owner is decided by geometry, not by pointers. §7.5: "integer remainders
	// following a stable endpoint order from serialized endpoint identity". The near child's origin
	// is the original's origin and the far child's is further along a fixed axis, so "near first" is
	// a total order that survives save, load, reversal and re-splitting — which is what makes the
	// aggregate-refund guarantee a guarantee rather than a tendency.
	DivideLedger( rail->GetStoredLedger(), nearLength, farLength, true, nearLedger, farLedger );

	const FTransform frame = rail->GetActorTransform();
	const FVector axis = frame.GetUnitAxis( EAxis::X );

	const TSubclassOf< AFGBuildable > railClass = rail->GetClass();
	const TSubclassOf< UFGRecipe > recipe = rail->GetBuiltWithRecipe();
	AFGBlueprintProxy* proxy = rail->GetBlueprintProxy();

	// §7.3 inside a Blueprint Designer, and the children are ours to place in it.
	//
	// A buildable belongs to a Designer twice over: its own mBlueprintDesigner (FGBuildable.h:1014),
	// and the Designer's list of what it contains — mBuildables, FGBuildableBlueprintDesigner.h:236,
	// whose comment is the reason this matters: "When a buildable is constructed it informs the
	// designer of its existence. This way we don't need to gather them to serialize." The list IS the
	// blueprint. A hologram-built buildable gets both from vanilla's construction path; these children
	// are spawned here, directly, and get neither unless this function gives them.
	//
	// Read off the host, as a protected field of our own base. Split is a member of AACPRRail, and
	// access through an AACPRRail pointer is what C++ allows for a protected base member.
	AFGBuildableBlueprintDesigner* designer = rail->mBlueprintDesigner.Get();

	struct FChildSpec
	{
		FVector Origin;
		double Length;
		const TArray< FInventoryStack >* Ledger;
	};

	const FChildSpec specs[ 2 ] =
	{
		{ frame.GetLocation(),                    nearLength, &nearLedger },
		{ frame.GetLocation() + axis * farStart,  farLength,  &farLedger  },
	};

	// The order is the transaction. Both children are spawned first, deferred, so a failure to spawn
	// is found before anything of the host's is touched; then the host's couplings are released and
	// its attachments handed to the children; then the children finish spawning — which runs their
	// BeginPlay, which couples them at once into a world where the neighbours are already open and
	// the Caps already sit on the children's terminals.
	FTransform spawnAt[ 2 ];
	for( int32 i = 0; i < 2; ++i )
	{
		spawnAt[ i ] = FTransform( frame.GetRotation(), specs[ i ].Origin, frame.GetScale3D() );

		// Deferred spawn, and the deferral is the whole point. FGBuildableSubsystem.h:279 documents
		// BeginSpawnBuildable as needing FinishSpawning afterwards, which means the actor exists and
		// has not yet run BeginPlay. That is the only window in which SetLength lands before the Rail
		// positions terminal B from mLength.
		AFGBuildable* spawned = subsystem->BeginSpawnBuildable( railClass, spawnAt[ i ] );
		AACPRRail* child = Cast< AACPRRail >( spawned );

		if( !child )
		{
			ACPR_LOG( Error, RAIL,
				TEXT( "Split(%s) could not spawn child %d of class %s." ),
				*rail->GetName(), i, *railClass->GetName() );
			continue;
		}

		child->SetLength( static_cast< float >( specs[ i ].Length ) );

		// Carried across before BeginPlay: the refund path and §7.5's preview both read these.
		if( recipe ) { child->SetBuiltWithRecipe( recipe ); }
		child->SetStoredLedger( *specs[ i ].Ledger );

		// §7.5: "Both children and the Junction inherit the original's Blueprint Dismantle group."
		if( proxy ) { child->SetBlueprintProxy( proxy ); }

		// The field half, before BeginPlay. mReplicatedBuiltInsideBlueprintDesigner is "derived from
		// mBlueprintDesigner on load and on begin play on authority" (FGBuildable.h:1016).
		if( designer )
		{
			child->mBlueprintDesigner = designer;
			child->mReplicatedBuiltInsideBlueprintDesigner = true;
		}

		children.Add( child );
	}

	if( children.Num() != 2 )
	{
		// Half a split is worse than none. Nothing of the host's has been touched yet; whatever did
		// spawn is removed again, unfinished.
		for( AACPRRail* child : children )
		{
			if( IsValid( child ) ) { child->Destroy(); }
		}
		children.Reset();

		ACPR_LOG( Error, RAIL,
			TEXT( "Split(%s) rolled back — only part of the pair could be spawned." ),
			*rail->GetName() );
		return children;
	}

	AACPRRail* nearChild = children[ 0 ];
	AACPRRail* farChild = children[ 1 ];

	// The host's couplings go first, so its neighbours are open when the children register. The
	// original is Destroy'd rather than Dismantle'd below: Dismantle would refund it on top of the
	// children carrying its ledger, which is §7.5's value counted twice.
	FACPRCoupling::ReleaseAll( rail, rail );

	// §7.4's attachment remapping, onto children that have not begun play. "Insertion preserves …
	// body-mounted Outlets, remapped to the correct child without moving them; terminal Caps and
	// Outlets at the original endpoints." A Cap remapped now caps the child's terminal (a CDO
	// subobject, present before BeginPlay) before that terminal is registered — so §6.2's "cap first
	// to build adjacent but uncoupled" survives the split. Collected first: Remap rewrites the list.
	TArray< AActor* > attachments;
	FACPRAttachments::Collect( rail, attachments );

	int32 remapped = 0;

	for( AActor* attachment : attachments )
	{
		if( !IsValid( attachment ) )
		{
			continue;
		}

		// A terminal attachment follows its end, by index. The original's terminal 0 is the near
		// child's terminal 0 and its terminal 1 is the far child's terminal 1. Nothing moves in world space.
		if( AACPRCap* cap = Cast< AACPRCap >( attachment ) )
		{
			const bool atNearEnd = ( cap->GetHostTerminalIndex() == 0 );
			cap->Remap( atNearEnd ? static_cast< AActor* >( nearChild )
			                      : static_cast< AActor* >( farChild ),
			            atNearEnd ? 0 : 1 );
			++remapped;
			continue;
		}

		if( AACPROutlet* outlet = Cast< AACPROutlet >( attachment ) )
		{
			if( outlet->GetHostMode() == EACPROutletHostMode::Terminal )
			{
				const bool atNearEnd = ( outlet->GetHostTerminalIndex() == 0 );
				outlet->Remap( atNearEnd ? static_cast< AActor* >( nearChild )
				                         : static_cast< AActor* >( farChild ),
				               atNearEnd ? 0 : 1 );
			}
			else
			{
				// §8.1's body mount has no terminal index, so the child is chosen by where it is.
				const double at = rail->GetOffsetAlong( outlet->GetActorLocation() );
				const bool onNear = ( at <= centreAlong );

				outlet->Remap( onNear ? static_cast< AActor* >( nearChild )
				                      : static_cast< AActor* >( farChild ), 0 );
			}
			++remapped;
			continue;
		}

		ACPR_LOG( Warning, RAIL,
			TEXT( "Split(%s) does not know how to remap attachment %s (%s) — it will be "
			      "orphaned when the original is destroyed." ),
			*rail->GetName(), *attachment->GetName(), *attachment->GetClass()->GetName() );
	}

	// Now the children begin play — and couple, at once, to the open neighbours and the Junction's faces.
	for( int32 i = 0; i < 2; ++i )
	{
		AACPRRail* child = children[ i ];
		child->FinishSpawning( spawnAt[ i ] );

		// The list half, after BeginPlay: if vanilla's BeginPlay registers a buildable with its
		// Designer the child is already listed; if not, it is added through the AccessTransformers
		// friend entry. listed= says which path ran.
		if( designer )
		{
			const bool listed = designer->GetBuildablesInBlueprintDesigner().Contains( static_cast< AFGBuildable* >( child ) );
			if( !listed )
			{
				designer->mBuildables.AddUnique( TObjectPtr< AFGBuildable >( child ) );
			}

			ACPR_LOG( Display, SPACE,
				TEXT( "Split(%s) child %d = %s inside %s | listedAfterBeginPlay=%d%s | insideFlag=%d" ),
				*rail->GetName(), i, *child->GetName(), *designer->GetName(),
				listed ? 1 : 0, listed ? TEXT( "" ) : TEXT( " -> ADDED to the Designer's list" ),
				child->IsBuildableInsideBlueprintDesigner() ? 1 : 0 );
		}

		ACPR_LOG( Display, RAIL,
			TEXT( "Split(%s) child %d = %s | length asked=%.1f got=%.1f | ledger=%d has=%d "
			      "| origin=%s | proxy=%d recipe=%d" ),
			*rail->GetName(), i, *child->GetName(),
			specs[ i ].Length, child->GetLength(), specs[ i ].Ledger->Num(),
			child->HasStoredLedger() ? 1 : 0,
			*specs[ i ].Origin.ToString(),
			proxy ? 1 : 0, recipe ? 1 : 0 );
	}

	ACPR_LOG( Display, RAIL,
		TEXT( "Split(%s) remapped %d of %d attachments | near=%s far=%s" ),
		*rail->GetName(), remapped, attachments.Num(),
		*nearChild->GetName(), *farChild->GetName() );

	ACPR_LOG( Display, RAIL,
		TEXT( "Split(%s) complete | total=%.1f -> near=%.1f cell=[%.1f,%.1f] far=%.1f "
		      "| original destroyed" ),
		*rail->GetName(), total, nearLength, nearLength, farStart, farLength );

	rail->Destroy();

	// And the original comes off the list. Destroy runs EndPlay synchronously, so whatever vanilla does
	// there has already happened; a stale entry would be a destroyed actor inside the blueprint's own
	// buildable list, which is the list the Designer serializes.
	if( designer )
	{
		const bool stillListed = designer->GetBuildablesInBlueprintDesigner().Contains( static_cast< AFGBuildable* >( rail ) );
		if( stillListed )
		{
			designer->mBuildables.Remove( TObjectPtr< AFGBuildable >( rail ) );
		}

		ACPR_LOG( Display, SPACE,
			TEXT( "Split(%s) original destroyed inside %s | stillListedAfterDestroy=%d%s | designerNowLists=%d" ),
			*rail->GetName(), *designer->GetName(),
			stillListed ? 1 : 0, stillListed ? TEXT( " -> REMOVED from the Designer's list" ) : TEXT( "" ),
			designer->GetBuildablesInBlueprintDesigner().Num() );
	}

	return children;
}

void AACPRRail::DivideLedger( const TArray< FInventoryStack >& source,
                              double lengthA, double lengthB, bool firstWinsRemainder,
                              TArray< FInventoryStack >& out_a, TArray< FInventoryStack >& out_b )
{
	out_a.Reset();
	out_b.Reset();

	const double total = lengthA + lengthB;
	if( total <= 0.0 )
	{
		// Not a discard. Returning with both shares empty would lose the entire ledger, and §7.5's
		// guarantee is that "aggregate refund across every dismantle order equals the original's
		// exact paid ledger" — losing all of it fails that by the whole amount rather than by one
		// item. A degenerate split has no proportion to divide by, so the named remainder side takes
		// everything, which keeps the sum exact for an input that should never occur anyway.
		( firstWinsRemainder ? out_a : out_b ) = source;
		return;
	}

	for( const FInventoryStack& stack : source )
	{
		if( !stack.HasItems() )
		{
			continue;
		}

		// Proportional, then the remainder goes to one named side. The floor plus the complement is
		// what makes the two shares sum to the original exactly, for every item and every length —
		// rounding both independently would lose or invent an item whenever the split was uneven,
		// and §7.5's aggregate guarantee would be false by one Steel Beam.
		const int32 share = FMath::FloorToInt32(
			static_cast< double >( stack.NumItems ) * ( firstWinsRemainder ? lengthB : lengthA ) / total );

		const int32 floored = FMath::Clamp( share, 0, stack.NumItems );
		const int32 rest = stack.NumItems - floored;

		const int32 amountA = firstWinsRemainder ? rest : floored;
		const int32 amountB = firstWinsRemainder ? floored : rest;

		if( amountA > 0 ) { out_a.Add( FInventoryStack( amountA, stack.Item.GetItemClass() ) ); }
		if( amountB > 0 ) { out_b.Add( FInventoryStack( amountB, stack.Item.GetItemClass() ) ); }
	}
}

void AACPRRail::GetChildDismantleActors_Implementation( TArray< AActor* >& out_ChildDismantleActors ) const
{
	Super::GetChildDismantleActors_Implementation( out_ChildDismantleActors );

	// Appended rather than replacing, so whatever vanilla already considers a child of this buildable
	// survives. The mod's own children are the Caps and Outlets that registered themselves.
	FACPRAttachments::Collect( const_cast< AACPRRail* >( this ), out_ChildDismantleActors );
}

bool AACPRRail::ShouldSave_Implementation() const
{
	return true;
}

void AACPRRail::Dismantle_Implementation()
{
	FACPRCostScope cost( EACPRCost::Dismantle );
	// Vanilla's mass dismantle calls this once per actor in the selection for every child it
	// collects, and a blueprint proxy makes every member a child of every other — one actor can be
	// dismantled hundreds of times in one frame, once per selected actor. Our release runs on the
	// first call only: nothing re-couples a dismantling actor, so the repeats have nothing to do but
	// reach Super. The count is logged once so the pattern stays visible.
	if( ++mDismantleCalls > 1 )
	{
		if( mDismantleCalls == 2 )
		{
			ACPR_LOG( Verbose, RAIL,
				TEXT( "%s Dismantle called again in the same dismantle — vanilla repeats it for every selected actor sharing a blueprint proxy; once was enough" ),
				*GetName() );
		}
		Super::Dismantle_Implementation();
		return;
	}
	// Before Super, because Super is what starts the dismantle effect and, at its end, the destroy.
	// Idempotent: the terminals' own OnComponentDestroyed releases again on every destruction path,
	// so a dismantled Rail simply reaches it twice.
	FACPRCoupling::ReleaseAll( this, this );

	Super::Dismantle_Implementation();
}


void AACPRRail::BeginPlay()
{
	FACPRCostScope cost( EACPRCost::BeginPlayRail );
	Super::BeginPlay();

	LogRailState( TEXT( "BeginPlay" ) );

	// Before the terminals, because this is the first moment mLength is known and the mesh is
	// sized from it.
	SetUpRailMesh();
	LogMeshSources( TEXT( "BeginPlay" ) );

	// Order is load-bearing, top to bottom:
	//   terminals — mLength is only known now, and everything below reads the placed geometry
	//   power     — the one connection, at the midpoint, its delegate bound
	//   register  — files terminals under their final cells
	//   resolve   — coupling at registration, on this very call. Every neighbour of a built host
	//               exists and is registered; a loaded host restores from saved geometry, whichever
	//               side begins play first (FACPRCoupling::RestoreSaved).
	CreateTerminals();
	RefreshAttachmentPoints();
	PlacePowerConnection();
	RegisterTerminals();
	FACPRCoupling::Resolve( this, this );

	LogTopology( TEXT( "BeginPlay" ) );

	// The one Display line per Rail: both ends' state, save/blueprint origin, space and group.
	FACPRCoupling::LogHostSummary( this, this, ACPR_TAG_TEXT( TERM ), TEXT( "BeginPlay" ), mBlueprintState );

	// Indicators: once now, from the state the resolve left; then only on events.
	UpdateCollarMesh( 0 );
	UpdateCollarMesh( 1 );
	CaptureReplicatedState();
	RefreshIndicators();
}

void AACPRRail::PlacePowerConnection()
{
	USceneComponent* root = GetRootComponent();
	if( !mPower || !root )
	{
		return;
	}

	if( mPower->GetAttachParent() != root )
	{
		mPower->AttachToComponent( root, FAttachmentTransformRules::KeepRelativeTransform );
	}

	// §5.4 with S at the 40 m maximum: N = 1, at L/2. 58.7 m of guaranteed Hoverpack reach at the worst
	// point of any chain, against the Hoverpack's 62 m search radius (6200 uu, from Equip_HoverPack).
	mPower->SetRelativeLocation( FVector( GetLength() * 0.5, 0.0, 0.0 ) );

	// The power event. UFGPowerConnectionComponent::OnHasPowerChanged is a single-cast delegate
	// (FGPowerConnectionComponent.h:52-54, "will fire whenever mHasPower has changed"); the pole
	// binds it on its own connections (FGBuildablePowerPole.h:77) and so does this host.
	mPower->OnHasPowerChanged.BindUObject( this, &AACPRRail::HandleHasPowerChanged );
}

void AACPRRail::SetHighlight( const FACPRHighlight& highlight )
{
	// Written every frame the aim holds; the lights move only when the set does.
	if( highlight != mHighlight )
	{
		mHighlight = highlight;
		RefreshIndicators();
	}
}

void AACPRRail::RefreshIndicators()
{
	// The body's groove ("Cue") summarises the network; each collar's groove segment (its own
	// "Cue") is that terminal's state — the hue over the last 35 cm. The collars' pocket floors
	// ("Power") are the network's power. The bridged collar carries the same slots, so the slice
	// lights with its collar. Slots are addressed by material-slot name (B.2).
	const FACPRIndicatorSlot slots[] = {
		{ mRailMeshComponent.Get(), ACPRSlot::Cue(),   ACPR_INDICATOR_HOST },
		{ mTerminalMeshA.Get(),     ACPRSlot::Cue(),   0 },
		{ mTerminalMeshB.Get(),     ACPRSlot::Cue(),   1 },
		{ mTerminalMeshA.Get(),     ACPRSlot::Power(), 0, true },
		{ mTerminalMeshB.Get(),     ACPRSlot::Power(), 1, true },
	};
	FACPRIndicators::Push( this, this, MakeArrayView( slots ), ACPR_TAG_TEXT( RAIL ) );
}

bool AACPRRail::UpdateCollarMesh( int32 index )
{
	// The bridged collar exactly while its terminal is coupled to a Junction — a host with more than
	// two terminals. Rail-to-Rail couplings have no seat to fill and a slice there would stand in the
	// other Rail. A mesh swap on one component, on the coupling event, rather than a second hidden
	// component per end.
	UStaticMeshComponent* collar = ( index == 0 ) ? mTerminalMeshA.Get() : ( index == 1 ) ? mTerminalMeshB.Get() : nullptr;
	const UACPRTerminalComponent* terminal = ( index == 0 ) ? mTerminalA.Get() : ( index == 1 ) ? mTerminalB.Get() : nullptr;
	if( !collar || !terminal )
	{
		return false;
	}

	const UACPRTerminalComponent* other = terminal->GetCoupledTo();
	const IACPRTerminalHost* partner = other ? Cast< IACPRTerminalHost >( other->GetOwner() ) : nullptr;
	const bool bridged = partner && partner->GetTerminalCount() > 2;

	UStaticMesh* wanted = bridged ? mTerminalBridgedMeshAsset.LoadSynchronous() : mTerminalMeshAsset.LoadSynchronous();
	if( !wanted || collar->GetStaticMesh() == wanted )
	{
		return false;
	}
	collar->SetStaticMesh( wanted );
	ACPR_LOG( Verbose, RAIL, TEXT( "%s collar %s -> %s" ),
		*GetName(), index ? TEXT( "B" ) : TEXT( "A" ), *wanted->GetName() );
	return true;
}

UACPRTerminalComponent* AACPRRail::GetTerminalAtIndex( uint8 index ) const
{
	// The indices are saved, so this mapping is part of the save format: 0 is the near end, 1 is
	// the far end, and swapping them would silently reverse every stored Coupling.
	if( index == 0 ) { return mTerminalA; }
	if( index == 1 ) { return mTerminalB; }
	return nullptr;
}

void AACPRRail::AddSavedCoupling( const FACPRSavedCoupling& coupling )
{
	// AddUnique would need operator== on the struct; this is the same thing, said once.
	for( const FACPRSavedCoupling& existing : mSavedCouplings )
	{
		if( existing.OtherActor == coupling.OtherActor &&
		    existing.LocalTerminalIndex == coupling.LocalTerminalIndex &&
		    existing.OtherTerminalIndex == coupling.OtherTerminalIndex )
		{
			return;
		}
	}

	mSavedCouplings.Add( coupling );
}

void AACPRRail::PostLoadGame_Implementation( int32 saveVersion, int32 gameVersion )
{
	Super::PostLoadGame_Implementation( saveVersion, gameVersion );
	LogRailState( TEXT( "PostLoadGame" ) );

	// Read one tick later through WasLoadedFromSave, which is why this does not care whether it runs
	// before or after BeginPlay — by the time the timer fires, either order has set it.
	//
	// It is the whole of invariant 6's enforcement: a loaded Rail replays its saved Couplings and
	// never searches for new ones. Without it, §6.2's "to build adjacent but uncoupled, cap the
	// terminal first" would survive exactly until the next reload.
	mCameFromSave = true;
	LogTopology( TEXT( "PostLoadGame" ) );
}

void AACPRRail::PreSaveGame_Implementation( int32 saveVersion, int32 gameVersion )
{
	Super::PreSaveGame_Implementation( saveVersion, gameVersion );
	// The witness in the data. Whether PostLoadGame runs before or after BeginPlay on a load
	// (FGSaveInterface.h does not say), a host that has been saved knows it on the next load from
	// this flag alone, so BeginPlay can take the loaded path at once (invariant 6).
	mSavedOnce = true;
}

AFGBuildableBlueprintDesigner* AACPRRail::GetHostDesigner() const
{
	return mBlueprintDesigner.Get();
}

void AACPRRail::RefreshAttachmentPoints()
{
	mTerminalPoints.Reset();
	for( uint8 i = 0; i < 2; ++i )
	{
		FFGAttachmentPoint point;
		if( FACPRAttachment::MakeTerminalPoint( this, this, i, point ) )
		{
			mTerminalPoints.Add( point );
		}
	}
	if( mAttachmentA )
	{
		if( USceneComponent* root = GetRootComponent() )
		{
			if( mAttachmentA->GetAttachParent() != root )
			{
				mAttachmentA->AttachToComponent( root, FAttachmentTransformRules::KeepRelativeTransform );
			}
		}
		FTransform frame;
		if( GetTerminalLocalFrame( 0, frame ) )
		{
			mAttachmentA->SetRelativeTransform( frame );
		}
	}
}

void AACPRRail::GetAttachmentPoints( TArray< const FFGAttachmentPoint* >& out_points ) const
{
	// Ours only — not Super's: the beam's front point would let a painted beam chain onto a Rail's end
	// and a Rail onto a beam's, neither of which is a coupling (§4). Empty until BeginPlay or the
	// blueprint path has run RefreshAttachmentPoints, which is also when the terminals exist.
	for( const FFGAttachmentPoint& point : mTerminalPoints )
	{
		out_points.Add( &point );
	}
}

bool AACPRRail::GetTerminalLocalFrame( uint8 index, FTransform& out_frame ) const
{
	// CreateTerminals' placement, restated from mLength (SaveGame) so it is true of a Rail whose
	// BeginPlay has not run — which is what a blueprint's private copy may be. The two must agree; the
	// §10 manager logs both where it can, so a drift between them shows rather than hides.
	if( index == 0 )
	{
		out_frame = FTransform( FRotator( 0.0f, 180.0f, 0.0f ).Quaternion(), FVector::ZeroVector );
		return true;
	}

	if( index == 1 )
	{
		out_frame = FTransform( FRotator::ZeroRotator.Quaternion(), FVector( GetLength(), 0.0f, 0.0f ) );
		return true;
	}

	return false;
}

void AACPRRail::PreSerializedToBlueprint()
{
	Super::PreSerializedToBlueprint();

	// §10.1: a blueprint Rail is costed from the recipe, never from a stored child ledger — a Rail
	// split inside the Designer would otherwise carry its share into every placed copy.
	mStoredLedger.Reset();
	mHasStoredLedger = false;

	ACPR_LOG( Verbose, BP,
		TEXT( "%s PreSerializedToBlueprint | space=%s | records=%s" ),
		*GetName(), *FACPRSpace::Describe( GetHostDesigner() ),
		*FACPRCoupling::DescribeRecords( this, this ) );
}

void AACPRRail::PostSerializedFromBlueprint( bool isBlueprintWorld )
{
	Super::PostSerializedFromBlueprint( isBlueprintWorld );
	RefreshAttachmentPoints();

	mBlueprintState = isBlueprintWorld ? TEXT( "bpworld" ) : TEXT( "placed" );
	mPlacedFromBlueprint = !isBlueprintWorld;

	// Fitted here for the blueprint world. AFGBlueprintHologram duplicates each blueprint-world
	// buildable's mesh components as they are at duplication time, and BeginPlay never runs in that
	// world. Everything this actor does to its meshes at BeginPlay — pick, scale, rotate — therefore
	// has to have happened by the end of this call, or the hologram shows the CDO's meshes:
	// unrotated and unscaled. Idempotent, so the placed copy's BeginPlay doing it again costs
	// nothing. The meshFit line it prints (from=blueprint, in the blueprint world) is the check.
	if( isBlueprintWorld )
	{
		SetUpRailMesh();

		// The collar variant, from the saved records — BeginPlay never runs here, so no coupling event
		// will. A record naming a partner with more than two terminals means that end wears the bridge.
		for( const FACPRSavedCoupling& record : mSavedCouplings )
		{
			const IACPRTerminalHost* partner = Cast< IACPRTerminalHost >( record.OtherActor.Get() );
			UStaticMeshComponent* collar = ( record.LocalTerminalIndex == 0 ) ? mTerminalMeshA.Get()
				: ( record.LocalTerminalIndex == 1 ) ? mTerminalMeshB.Get() : nullptr;
			if( collar && partner && partner->GetTerminalCount() > 2 )
			{
				if( UStaticMesh* mesh = mTerminalBridgedMeshAsset.LoadSynchronous() )
				{
					collar->SetStaticMesh( mesh );
				}
			}
		}
	}

	ACPR_LOG( Verbose, BP,
		TEXT( "%s PostSerializedFromBlueprint(isBlueprintWorld=%d) | begunPlay=%d fromSave=%d "
		      "| space=%s | length=%.1f | records=%s" ),
		*GetName(), isBlueprintWorld ? 1 : 0,
		HasActorBegunPlay() ? 1 : 0, mCameFromSave ? 1 : 0,
		*FACPRSpace::Describe( GetHostDesigner() ), GetLength(),
		*FACPRCoupling::DescribeRecords( this, this ) );
}

void AACPRRail::CreateTerminals()
{
	USceneComponent* root = GetRootComponent();

	// SetupAttachment( nullptr ) is legal and silent: the component simply stays unparented, at
	// which point every "relative" position below is really a world position and every quantized
	// cell is nonsense. Refusing outright is better than producing terminals in the wrong place,
	// and the root= field in LogTopology says which happened.
	if( !root )
	{
		ACPR_LOG( Error, TERM,
			TEXT( "%s has no RootComponent at BeginPlay — no terminals created." ),
			*GetName() );
		return;
	}

	// §11: the beam axis is the actor's local X (B.2), so the near terminal sits at the origin and
	// the far one at X = mLength.
	const float length = GetLength();

	// The terminals exist already — they are CDO subobjects. All that is left is the attachment the
	// constructor could not make while RootComponent was still null, and a position, which cannot
	// be known any earlier because the far terminal's X is the Rail's length.
	auto place = [ & ]( UACPRTerminalComponent* terminal, float x, float yaw, const TCHAR* name )
	{
		if( !terminal )
		{
			ACPR_LOG( Error, TERM,
				TEXT( "%s has no %s — the CDO subobject did not survive. Check that "
				      "Build_PowerRail's parent class is still AACPRRail." ),
				*GetName(), name );
			return;
		}

		if( terminal->GetAttachParent() != root )
		{
			terminal->AttachToComponent( root, FAttachmentTransformRules::KeepRelativeTransform );
		}

		// Both terminals face OUTWARD along their own +X, which is what §5.1 means by "a snapped
		// Rail continues along the terminal's outward axis" — so the far one is turned around.
		terminal->SetRelativeLocationAndRotation( FVector( x, 0.0f, 0.0f ), FRotator( 0.0f, yaw, 0.0f ) );
	};

	place( mTerminalA, 0.0f,   180.0f, TEXT( "TerminalA" ) );
	place( mTerminalB, length,   0.0f, TEXT( "TerminalB" ) );

	// B.3: the outward axis is derived from the two quantized endpoints, not stored as a separately
	// quantized angle — so it is exactly consistent with the cells §6.1 compares, and "opposing"
	// becomes an integer equality instead of a dot product with a tolerance.
	//
	// Each terminal points away from the other, which is what §5.1's "a snapped Rail continues
	// along the terminal's outward axis" means at each end.
	if( mTerminalA && mTerminalB )
	{
		const FIntVector cellA = mTerminalA->GetQuantizedLocation();
		const FIntVector cellB = mTerminalB->GetQuantizedLocation();

		mTerminalA->SetQuantizedOutward( UACPRTerminalComponent::ReduceDirection( cellA - cellB ) );
		mTerminalB->SetQuantizedOutward( UACPRTerminalComponent::ReduceDirection( cellB - cellA ) );
	}
}

void AACPRRail::RegisterTerminals()
{
	UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this );
	if( !registry )
	{
		ACPR_LOG( Warning, TERM,
			TEXT( "%s found no terminal registry — coincidence tests will find "
			      "nothing." ),
			*GetName() );
		return;
	}

	if( mTerminalA ) { registry->RegisterTerminal( mTerminalA ); }
	if( mTerminalB ) { registry->RegisterTerminal( mTerminalB ); }
}

void AACPRRail::LogTopology( const TCHAR* stage )
{
	if( !ACPR_LOG_ACTIVE( Verbose ) )
	{
		return;
	}
	int32 registeredTerminals = 0;
	int32 occupiedCells       = 0;
	if( UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this ) )
	{
		registry->GetCounts( registeredTerminals, occupiedCells );
	}

	// Read circuit= with this in mind: an unpowered chain reads circuit=0 at BeginPlay and
	// circuit=-1 one tick later, and that is not a race. The headers explain it:
	//
	//   UFGCircuit.h:80   virtual bool IsTrivial() const;
	//                     "@return true if this circuit can be removed without any impact on game
	//                      logic"
	//   FGPowerCircuit.h:272  virtual bool IsTrivial() const override;
	//   FGPowerCircuit.h:300  TArray< TObjectPtr< UFGPowerInfoComponent > > mPowerInfos;
	//
	// A power circuit made entirely of passive conductors has no power infos and nothing to
	// simulate, so it is trivial and the subsystem discards it on its tick — taking every member's
	// circuit id back to INDEX_NONE. The hidden connection holds; the circuit is collected.
	//
	// So circuit= only means something once something that produces or consumes power is on the
	// network. Until then the real evidence that the topology exists is conn= (hidden connection
	// count) and to= (the coupling partner), which do not depend on the subsystem's opinion.
	// §5.3 says a Rail "does not produce, consume, store, switch, prioritize or limit power", so an
	// unpowered Rail chain being circuit-less is correct behaviour, not a defect.
	ACPR_LOG( Verbose, TERM,
		TEXT( "%s %s | length=%.1f | A: %s" ),
		*GetName(), stage, GetLength(),
		mTerminalA ? *mTerminalA->Describe() : TEXT( "<null>" ) );

	ACPR_LOG( Verbose, TERM,
		TEXT( "%s %s | B: %s" ),
		*GetName(), stage,
		mTerminalB ? *mTerminalB->Describe() : TEXT( "<null>" ) );


	// root= is not decoration. Every component here is created at BeginPlay and attached to
	// whatever this names, and everything positional on the two lines above is relative to it — so
	// if it reads <null>, the components are unattached, the cells are world-origin nonsense, and
	// nothing else on these lines means anything. A healthy Rail reads root=RootComponent
	// attachedA=1 attachedB=1.
	const USceneComponent* root = GetRootComponent();
	const bool attachedA = mTerminalA && mTerminalA->GetAttachParent() == root;
	const bool attachedB = mTerminalB && mTerminalB->GetAttachParent() == root;

	ACPR_LOG( Verbose, TERM,
		TEXT( "%s %s | root=%s attachedA=%d attachedB=%d | couplings=%d fromSave=%d savedOnce=%d | "
		      "power: %s at X=%.0f | registry: %d terminals in %d cells | authority=%d" ),
		*GetName(), stage,
		root ? *root->GetName() : TEXT( "<null>" ),
		attachedA ? 1 : 0, attachedB ? 1 : 0,
		mSavedCouplings.Num(), mCameFromSave ? 1 : 0, mSavedOnce ? 1 : 0,
		mPower ? *mPower->Describe() : TEXT( "<null>" ),
		mPower ? mPower->GetRelativeLocation().X : 0.0,
		registeredTerminals, occupiedCells,
		HasAuthority() ? 1 : 0 );
}

TArray< FInstanceData > AACPRRail::GetActorLightweightInstanceData_Implementation() const
{
	// No instance data from a Rail that already draws itself. The Rail renders through its own
	// components and is not managed by the lightweight subsystem, so in the world this hook is never
	// called. The blueprint hologram is the one caller (SetInstanceDataBuildableComponent,
	// FGBlueprintHologram.h:72), and AFGBuildableBeam's answer — one beam mesh scaled along +Z — would
	// show the Rail a second time, standing up from terminal A. Nothing, on every path.
	return TArray< FInstanceData >();
}

// -90 in pitch takes a component's local +Z to local +X: UE pitch is a rotation about Y and a
// positive pitch carries +X up to +Z, so the negative one carries +Z forward to +X. One constant,
// because the buildable and the hologram must agree and a second literal is how they stop agreeing.
static const FRotator kRailMeshRotation( -90.0, 0.0, 0.0 );

UStaticMesh* AACPRRail::ResolveRailMesh() const
{
	if( mRailMesh )
	{
		return mRailMesh;
	}
	// LoadSynchronous is declared "T* LoadSynchronous() const" and returns null for a null path,
	// so no IsNull guard is needed and this stays callable from the const instance-data hooks.
	return mRailMeshAsset.LoadSynchronous();
}

void AACPRRail::OrientRailMesh( UStaticMeshComponent* comp )
{
	if( comp )
	{
		comp->SetRelativeRotation( kRailMeshRotation );
	}
}

FVector AACPRRail::FitRailMeshToLength( UStaticMeshComponent* comp, double length,
                                        double inset )
{
	if( !comp )
	{
		return FVector::ZeroVector;
	}
	const UStaticMesh* mesh = comp->GetStaticMesh();
	if( !mesh )
	{
		return FVector::ZeroVector;
	}

	// GetBounds().Origin / .BoxExtent are plain public members of FBoxSphereBounds rather than
	// methods — the same form AACPRJunction::FitMeshComponentToCube uses.
	const FBoxSphereBounds bounds = mesh->GetBounds();
	const FVector size = bounds.BoxExtent * 2.0;
	const FVector centre = bounds.Origin;

	const FRotator rotation = kRailMeshRotation;

	// The body spans only the gap between the collars, inset at both ends, so the three pieces abut
	// instead of overlapping and the terminal plane is empty until a Cap fills it. A body running
	// the whole length would put its end face on the terminal plane, exactly where a Cap's outer
	// face lands, and two coplanar overlapping faces under complex collision flicker. Guarded,
	// because a Rail shorter than two collars would otherwise invert — GetSize()'s 1 m minimum
	// makes that unreachable, which is not a reason to leave it unguarded.
	const double span = ( inset > 0.0 && length > 2.0 * inset ) ? ( length - 2.0 * inset ) : length;
	const double start = ( span < length ) ? inset : 0.0;

	FVector scale( 1.0, 1.0, 1.0 );
	if( FMath::Abs( size.Z ) > 0.01 && span > 0.0 )
	{
		scale.Z = span / size.Z;
	}

	comp->SetRelativeRotation( rotation );
	comp->SetRelativeScale3D( scale );

	// The cross-section is centred on the axis; the LENGTH is not re-centred, because the mesh's
	// base belongs on the actor origin where terminal A is. The offset is computed in the mesh's
	// own frame and then rotated, so it stays correct if the rotation above ever changes.
	const FVector localOffset( -centre.X * scale.X, -centre.Y * scale.Y, 0.0 );
	comp->SetRelativeLocation( rotation.Quaternion().RotateVector( localOffset )
		+ FVector( start, 0.0, 0.0 ) );

	return FVector( size.X * scale.X, size.Y * scale.Y, size.Z * scale.Z );
}

void AACPRRail::SetUpRailMesh()
{
	if( !mRailMeshComponent )
	{
		ACPR_LOG( Warning, RAIL,
			TEXT( "%s has no mesh component — it will be invisible and cannot be "
			      "targeted." ),
			*GetName() );
		return;
	}

	// The blueprint is the mesh's normal home (acpr_bind_meshes.py binds it there, the same as the
	// Junction's). ResolveRailMesh is the fallback that makes a clean checkout work before any
	// script has run — mRailMeshAsset carries the path as a C++ default.
	const bool fromBlueprint = ( mRailMeshComponent->GetStaticMesh() != nullptr );
	if( !fromBlueprint )
	{
		if( UStaticMesh* const fallback = ResolveRailMesh() )
		{
			mRailMeshComponent->SetStaticMesh( fallback );
		}
	}

	// The inset is the collar's own authored length, read from its bounds rather than restated as
	// a constant here — one number, in the mesh that defines it, so it cannot drift from
	// RAIL_TERMINAL_LENGTH.
	//
	// Reach, not size. BoxExtent.Z * 2.0 is how tall the collar is, which equals its reach only
	// while it starts exactly at z = 0. RAIL_TERMINAL_SETBACK starts it a centimetre in, so the
	// height is 34 and the reach is still 35 — and a body inset by 34 would poke a centimetre into
	// the collar. Origin.Z + BoxExtent.Z is the far face, which is what "how far the collar reaches
	// from the terminal plane" actually means, and it gives the same 35 either way.
	double inset = 0.0;
	if( const UStaticMesh* const collar = mTerminalMeshAsset.LoadSynchronous() )
	{
		const FBoxSphereBounds bounds = collar->GetBounds();
		inset = bounds.Origin.Z + bounds.BoxExtent.Z;
	}

	const FVector fitted = FitRailMeshToLength( mRailMeshComponent, GetLength(), inset );
	const UStaticMesh* const mesh = mRailMeshComponent->GetStaticMesh();

	ACPR_LOG( Verbose, RAIL,
		TEXT( "%s meshFit | mesh=%s from=%s | length=%.1f -> size=%s | "
		      "scale=%s loc=%s rot=%s | collision=%s" ),
		*GetName(),
		mesh ? *mesh->GetName() : TEXT( "<none>" ),
		fromBlueprint ? TEXT( "blueprint" ) : TEXT( "mRailMeshAsset" ),
		GetLength(), *fitted.ToString(),
		*mRailMeshComponent->GetRelativeScale3D().ToString(),
		*mRailMeshComponent->GetRelativeLocation().ToString(),
		*mRailMeshComponent->GetRelativeRotation().ToString(),
		*mRailMeshComponent->GetCollisionProfileName().ToString() );

	if( !mesh )
	{
		ACPR_LOG( Warning, RAIL,
			TEXT( "%s has a mesh component with NO mesh: neither the blueprint nor "
			      "mRailMeshAsset resolved one. Run acpr_meshes.py and acpr_bind_meshes.py." ),
			*GetName() );
	}

	SetUpTerminalMeshes();
}

void AACPRRail::SetUpTerminalMeshes()
{
	UStaticMesh* const collar = mTerminalMeshAsset.LoadSynchronous();
	UStaticMesh* const bridged = mTerminalBridgedMeshAsset.LoadSynchronous();

	// The two collars. Never scaled — make_rail_terminal exists as a separate mesh precisely so
	// these details do not stretch with the body — so this is placement and orientation only.
	//
	// The mesh is authored along +Z with z = 0 at the Rail's end and its body reaching inward.
	// Terminal A sits at local X = 0 and inward is +X, which is the same -90 pitch the body uses.
	// Terminal B sits at X = mLength and inward is -X, so it is the opposite pitch.
	const double length = GetLength();

	const TCHAR* labels[ 2 ] = { TEXT( "A" ), TEXT( "B" ) };
	UStaticMeshComponent* pieces[ 2 ] = { mTerminalMeshA, mTerminalMeshB };
	const FVector locations[ 2 ] = { FVector::ZeroVector, FVector( length, 0.0, 0.0 ) };
	const FRotator rotations[ 2 ] = { FRotator( -90.0, 0.0, 0.0 ), FRotator( 90.0, 0.0, 0.0 ) };

	for( int32 i = 0; i < 2; ++i )
	{
		UStaticMeshComponent* const piece = pieces[ i ];
		if( !piece )
		{
			ACPR_LOG( Warning, RAIL,
				TEXT( "%s has no TerminalMesh%s component — the end is unfinished, the "
				      "Cap has no pocket to sit in, and the terminal light is missing." ),
				*GetName(), labels[ i ] );
			continue;
		}

		// The collar is set unconditionally, not only when the component is empty: the collar is a
		// C++ decision (the reference lives in source), so a blueprint that binds something else —
		// a body mesh, say, which would bury the Cap and put a flat end face on the terminal plane —
		// is named in the log rather than obeyed. The bridged variant is a legitimate holder too — it
		// is what a coupled end wears (UpdateCollarMesh) — so only a foreign mesh warns.
		if( collar )
		{
			UStaticMesh* const held = piece->GetStaticMesh();
			const bool foreign = held && held != collar && held != bridged;
			if( foreign )
			{
				ACPR_LOG( Warning, RAIL,
					TEXT( "%s TerminalMesh%s held %s — replaced with the collar %s. Something "
					      "bound the wrong mesh to a collar component; check acpr_bind_meshes.py." ),
					*GetName(), labels[ i ], *held->GetName(), *collar->GetName() );
			}
			if( !held || foreign )
			{
				piece->SetStaticMesh( collar );
			}
		}
		piece->SetRelativeRotation( rotations[ i ] );
		piece->SetRelativeLocation( locations[ i ] );
		piece->SetRelativeScale3D( FVector::OneVector );
	}

	// Reports what the components hold, not only what was resolved: mesh= is what the soft path
	// found; A=/B= are what is actually attached.
	ACPR_LOG( Verbose, RAIL,
		TEXT( "%s terminalMeshes | mesh=%s | length=%.1f | A=%s at %s rot %s | B=%s at %s rot %s" ),
		*GetName(),
		collar ? *collar->GetName() : TEXT( "<none>" ),
		length,
		( mTerminalMeshA && mTerminalMeshA->GetStaticMesh() ) ? *mTerminalMeshA->GetStaticMesh()->GetName() : TEXT( "<none>" ),
		*locations[ 0 ].ToString(), *rotations[ 0 ].ToString(),
		( mTerminalMeshB && mTerminalMeshB->GetStaticMesh() ) ? *mTerminalMeshB->GetStaticMesh()->GetName() : TEXT( "<none>" ),
		*locations[ 1 ].ToString(), *rotations[ 1 ].ToString() );
}

void AACPRRail::LogMeshSources( const TCHAR* stage ) const
{
	// Everything below builds strings for a Verbose line; UE_LOG skips the format when the
	// verbosity is off, not the work that feeds it. So the work is skipped here.
	if( !ACPR_LOG_ACTIVE( Verbose ) )
	{
		return;
	}
	// GetComponents is "template<class ComponentType, class AllocatorType> void GetComponents(
	// TArray<ComponentType, AllocatorType>& OutComponents, bool bIncludeFromChildActors) const",
	// so a plain TArray matches and the call is legal from a const method.
	TArray< UStaticMeshComponent* > meshComponents;
	GetComponents( meshComponents, false );

	FString list;
	for( const UStaticMeshComponent* component : meshComponents )
	{
		if( !component )
		{
			continue;
		}
		const UStaticMesh* mesh = component->GetStaticMesh();
		list += FString::Printf( TEXT( "%s[%s]=%s  " ),
			*component->GetName(),
			*component->GetClass()->GetName(),
			mesh ? *mesh->GetName() : TEXT( "<none>" ) );
	}
	if( list.IsEmpty() )
	{
		list = TEXT( "none  " );
	}

	UStaticMesh* const resolved = ResolveRailMesh();

	// Read next to the meshFit line: two lines per Rail, both bounded, and between them they say
	// which object holds our mesh and whether anything on the actor could draw it at all.
	ACPR_LOG( Verbose, RAIL,
		TEXT( "%s meshSources(%s) | staticMeshComponents=%d | %s| "
		      "mRailMesh=%s resolved=%s" ),
		*GetName(), stage, meshComponents.Num(), *list,
		mRailMesh ? *mRailMesh->GetName() : TEXT( "<none>" ),
		resolved ? *resolved->GetName() : TEXT( "<none>" ) );
}

void AACPRRail::LogRailState( const TCHAR* stage ) const
{
	// BUILD is first because nothing else on the line means anything if it is stale. It is a
	// compile time, so it is compared against the clock, not against a remembered string.
	ACPR_LOG( Verbose, RAIL,
		TEXT( "%s %s | BUILD=%s | beam(length=%.1f default=%.1f max=%.1f size=%.1f "
		      "tiled=%d lengthPerCost=%.1f) | lightweight(canContain=%d managedBySubsystem=%d) "
		      "| authority=%d" ),
		*GetName(), stage, ACPR_BUILD_STAMP(),
		GetLength(), GetDefaultLength(), GetMaxLength(), GetSize(),
		IsMeshTiled() ? 1 : 0, mLengthPerCost,
		mCanContainLightweightInstances ? 1 : 0,
		mManagedByLightweightBuildableSubsystem ? 1 : 0,
		HasAuthority() ? 1 : 0 );
}
