// Auto-Connecting Power Rails — the Power Rail Outlet.

#include "ACPROutlet.h"

#include "FGColoredInstanceMeshProxy.h"
#include "FGSwatchGroup.h"

#include "ACPRCap.h"
#include "ACPRSpec.h"
#include "ACPRDesignerSpace.h"
#include "ACPRPowerConnectionComponent.h"
#include "ACPRTerminalComponent.h"
#include "ACPRTerminalRegistry.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "Buildables/FGBuildablePowerPole.h"
#include "Components/WidgetComponent.h"
#include "UI/FGPoleConnectionsWidget.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

AACPROutlet::AACPROutlet()
{
	// Both connections are CDO subobjects, and for this class the second reason is the feature
	// itself: the Hoverpack cannot see a connection created at BeginPlay, and neither can a saved
	// Power Line looking for the component it ends on — and this Outlet's whole purpose is to hold up
	// to four of those. A runtime-created socket would lose every wire on every reload.
	//
	// These NAMES are save format. Renaming either orphans every Power Line attached to it in every
	// existing save — and the socket's is the Wall Outlet's connection component's on purpose
	// (§15): an Outlet redirected to a Wall Outlet after the mod is removed keeps its Power Lines
	// only if their endpoint resolves there.
	// The socket IS this host's one power connection: visible, four Power Lines, and the end of the
	// mount's Coupling edge or of the body bond. The mount is a terminal — a place.
	mPowerConnection = CreateDefaultSubobject< UACPRPowerConnectionComponent >( ACPR_POWER_CONNECTION_NAME );
	mMountInterface  = CreateDefaultSubobject< UACPRTerminalComponent >( TEXT( "ACPROutletMount" ) );
	if( mPowerConnection )
	{
		mPowerConnection->ConfigureAsWireSocket( mMaxPowerLines );
	}
	if( mMountInterface )
	{
		mMountInterface->SetPrivateInterface( true );
	}

	// The default swatch is the Foundation's. AFGBuildable::mSwatchGroup decides which of the
	// player's per-category default swatches a fresh build wears; the Standard group's is FICSIT
	// orange, and on an unpainted Rail that orange accent fights the amber and white of the
	// indicators, so the colours stop meaning anything. The Foundation group's default is the dark
	// grey every foundation wears, so an unpainted ACPR part reads as dark infrastructure and the
	// accent only appears when someone paints it. Any swatch still applies.
	// UFGSwatchGroup_FicsitFoundation: FGSwatchGroup.h:65. A blueprint that overrides mSwatchGroup
	// wins, and the push line's swatch= field is what says which one took.
	mSwatchGroup = UFGSwatchGroup_FicsitFoundation::StaticClass();

	// The connections readout (see the header). Screen-space like a pole's, hidden until the wire
	// hologram looks at us, attached to the socket in BeginPlay so it floats where the cable lands.
	mConnectionsWidgetComponent = CreateDefaultSubobject< UWidgetComponent >( TEXT( "ConnectionsWidget" ) );
	if( mConnectionsWidgetComponent )
	{
		mConnectionsWidgetComponent->SetWidgetSpace( EWidgetSpace::Screen );
		mConnectionsWidgetComponent->SetDrawAtDesiredSize( true );
		mConnectionsWidgetComponent->SetHiddenInGame( true );
		mConnectionsWidgetComponent->SetCollisionEnabled( ECollisionEnabled::NoCollision );
	}

	mOutletMesh = CreateDefaultSubobject< UFGColoredInstanceMeshProxy >( TEXT( "OutletMesh" ) );
	FACPRIndicators::ConfigureProxy( mOutletMesh );
	mOutletBase = CreateDefaultSubobject< UFGColoredInstanceMeshProxy >( TEXT( "OutletBase" ) );
	FACPRIndicators::ConfigureProxy( mOutletBase );
	if( mOutletBase )
	{
		mOutletBase->SetMobility( EComponentMobility::Movable );
		mOutletBase->SetCollisionProfileName( TEXT( "BuildingMesh" ) );
	}

	// RootComponent is normally null in a buildable's C++ constructor — the root is authored on the
	// Blueprint leaf — so this is best effort and BeginPlay finishes what is left unattached.
	if( USceneComponent* root = GetRootComponent() )
	{
		if( mPowerConnection ) { mPowerConnection->SetupAttachment( root ); }
		if( mMountInterface )  { mMountInterface->SetupAttachment( root ); }
		if( mOutletMesh )      { mOutletMesh->SetupAttachment( root ); }
		if( mOutletBase )      { mOutletBase->SetupAttachment( root ); }
	// BuildingMesh, the same profile AACPRRail sets on its three components — and vanilla's own,
	// read off the instance data our blueprint carries (CollisionProfileName="BuildingMesh"). An
	// explicit, matching profile is what the aim trace sees, independent of where the mesh is.
		if( mOutletMesh ) { mOutletMesh->SetCollisionProfileName( TEXT( "BuildingMesh" ) ); }
	}
}

bool AACPROutlet::ShouldSave_Implementation() const
{
	return true;
}

void AACPROutlet::PostLoadGame_Implementation( int32 saveVersion, int32 gameVersion )
{
	Super::PostLoadGame_Implementation( saveVersion, gameVersion );
	mCameFromSave = true;
}

AFGBuildableBlueprintDesigner* AACPROutlet::GetHostDesigner() const
{
	return mBlueprintDesigner.Get();
}

void AACPROutlet::PreSerializedToBlueprint()
{
	Super::PreSerializedToBlueprint();

	ACPR_LOG( Verbose, BP,
		TEXT( "%s PreSerializedToBlueprint | space=%s | mode=%s host=%s[%d] | records=%s" ),
		*GetName(), *FACPRSpace::Describe( GetHostDesigner() ),
		mHostMode == EACPROutletHostMode::Terminal ? TEXT( "terminal" ) : TEXT( "body" ),
		IsValid( mHostActor ) ? *mHostActor->GetName() : TEXT( "<null>" ),
		static_cast< int32 >( mHostTerminalIndex ),
		*FACPRCoupling::DescribeRecords( this, this ) );
}

void AACPROutlet::PostSerializedFromBlueprint( bool isBlueprintWorld )
{
	Super::PostSerializedFromBlueprint( isBlueprintWorld );

	mBlueprintState = isBlueprintWorld ? TEXT( "bpworld" ) : TEXT( "placed" );
	mPlacedFromBlueprint = !isBlueprintWorld;

	// Fitted here for the blueprint world. AFGBlueprintHologram duplicates each blueprint-world
	// buildable's mesh components as they are at duplication time, and BeginPlay never runs in that
	// world. Everything this actor does to its meshes at BeginPlay — pick, scale, rotate — therefore
	// has to have happened by the end of this call, or the hologram shows the CDO's meshes:
	// unrotated, unscaled, and the Rail-sized Cap on a Junction face. Idempotent, so the placed
	// copy's BeginPlay doing it again costs nothing.
	if( isBlueprintWorld )
	{
		SetUpMeshes();
	}

	// hostWorld= says whether the blueprint remapped the host: mHostActor is a SaveGame actor
	// reference exactly like a coupling record's, and a blueprint that does not remap it leaves this
	// Outlet mounted on the blueprint's private copy of its Rail.
	ACPR_LOG( Verbose, BP,
		TEXT( "%s PostSerializedFromBlueprint(isBlueprintWorld=%d) | begunPlay=%d fromSave=%d "
		      "| space=%s | mode=%s host=%s[%d] hostWorld=%s | records=%s" ),
		*GetName(), isBlueprintWorld ? 1 : 0,
		HasActorBegunPlay() ? 1 : 0, mCameFromSave ? 1 : 0,
		*FACPRSpace::Describe( GetHostDesigner() ),
		mHostMode == EACPROutletHostMode::Terminal ? TEXT( "terminal" ) : TEXT( "body" ),
		IsValid( mHostActor ) ? *mHostActor->GetName() : TEXT( "<null>" ),
		static_cast< int32 >( mHostTerminalIndex ),
		!IsValid( mHostActor ) ? TEXT( "null" )
			: ( mHostActor->GetWorld() == GetWorld() ? TEXT( "same" ) : TEXT( "OTHER-WORLD" ) ),
		*FACPRCoupling::DescribeRecords( this, this ) );
}

UACPRTerminalComponent* AACPROutlet::GetTerminalAtIndex( uint8 index ) const
{
	// Null for an out-of-range index rather than an assert — indices come from saved data, and a
	// save written by an older schema is a thing to report rather than to crash on.
	return ( index == 0 ) ? mMountInterface : nullptr;
}

void AACPROutlet::AddSavedCoupling( const FACPRSavedCoupling& coupling )
{
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

void AACPROutlet::SetHost( EACPROutletHostMode mode, AActor* hostActor, uint8 terminalIndex )
{
	mHostMode = mode;
	mHostActor = hostActor;
	mHostTerminalIndex = terminalIndex;
}

UACPRTerminalComponent* AACPROutlet::ResolveHostTerminal() const
{
	if( mHostMode != EACPROutletHostMode::Terminal || !IsValid( mHostActor ) )
	{
		return nullptr;
	}

	const IACPRTerminalHost* host = Cast< IACPRTerminalHost >( mHostActor );
	return host ? host->GetTerminalAtIndex( mHostTerminalIndex ) : nullptr;
}

void AACPROutlet::BeginPlay()
{
	FACPRCostScope cost( EACPRCost::BeginPlayOutlet );
	Super::BeginPlay();

	SetUpMeshes();

	// §8 and invariant 7: the socket's budget is set explicitly (constructor and here, in case a
	// blueprint changed mMaxPowerLines), never inherited. A zero budget accepts no Power Line at
	// all — a cable that connects and carries nothing.
	if( mPowerConnection )
	{
		mPowerConnection->ConfigureAsWireSocket( mMaxPowerLines );
		mPowerConnection->OnHasPowerChanged.BindUObject( this, &AACPROutlet::HandleHasPowerChanged );

		if( mConnectionsWidgetComponent && mConnectionsWidgetComponent->GetAttachParent() != mPowerConnection )
		{
			mConnectionsWidgetComponent->AttachToComponent( mPowerConnection, FAttachmentTransformRules::KeepRelativeTransform );
		}
	}
	if( mMountInterface )
	{
		mMountInterface->SetPrivateInterface( true );
	}

	if( mHostMode == EACPROutletHostMode::Terminal )
	{
		PlaceMountInterface();
	}
	else
	{
		BondToBody();
	}

	// Register, then resolve at once. In Body mode there is nothing to resolve.
	RegisterTerminals();
	FACPRCoupling::Resolve( this, this );

	// §8.3: "Dismantling a host also dismantles dependent Outlets with full refunds, previewed."
	FACPRAttachments::Register( mHostActor, this );

	// A freshly built Outlet wears its host's swatch. A loaded one keeps its own.
	if( !WasLoadedFromSave() )
	{
		FACPRIndicators::AdoptSwatch( this, mHostActor, ACPR_TAG_TEXT( OUTLET ) );
	}

	LogOutlet( WasLoadedFromSave() ? TEXT( "BeginPlay(loaded)" ) : TEXT( "BeginPlay(built)" ) );
	FACPRCoupling::LogHostSummary( this, this, ACPR_TAG_TEXT( OUTLET ), TEXT( "BeginPlay" ), mBlueprintState );

	CaptureReplicatedState();
	RefreshIndicators();
}

void AACPROutlet::GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );
	DOREPLIFETIME( AACPROutlet, mHostMode );
	DOREPLIFETIME( AACPROutlet, mHostActor );
	DOREPLIFETIME( AACPROutlet, mHostTerminalIndex );
	DOREPLIFETIME( AACPROutlet, mTerminalStateRep );
	DOREPLIFETIME( AACPROutlet, mHasPowerRep );
}

bool AACPROutlet::HasNetworkPower() const
{
	if( !HasAuthority() )
	{
		return mHasPowerRep;
	}
	return mPowerConnection && mPowerConnection->HasPower();
}

void AACPROutlet::CaptureReplicatedState()
{
	if( !HasAuthority() )
	{
		return;
	}
	mTerminalStateRep.SetNum( 1 );
	mTerminalStateRep[ 0 ] = static_cast< uint8 >( mMountInterface ? mMountInterface->GetTerminalState() : EACPRTerminalState::Open );
	mHasPowerRep = mPowerConnection && mPowerConnection->HasPower();
}

void AACPROutlet::OnRep_TerminalState()
{
	if( mMountInterface && mTerminalStateRep.Num() > 0 )
	{
		mMountInterface->SetReplicatedState( static_cast< EACPRTerminalState >( mTerminalStateRep[ 0 ] ) );
	}
	RefreshIndicators();
}

void AACPROutlet::OnRep_Host()
{
	// A client's seat: the host fields arrived (possibly after BeginPlay ran with them empty), so the
	// meshes are fitted again from them. Idempotent. UNTESTED in multiplayer.
	if( HasActorBegunPlay() )
	{
		SetUpMeshes();
	}
}

void AACPROutlet::OnTerminalStateChanged( int32 /*index*/ )
{
	if( HasActorBegunPlay() )
	{
		CaptureReplicatedState();
		RefreshIndicators();
	}
}

void AACPROutlet::HandleHasPowerChanged( bool /*hasPower*/ )
{
	OnTerminalStateChanged( INDEX_NONE );
}

void AACPROutlet::PreSaveGame_Implementation( int32 saveVersion, int32 gameVersion )
{
	Super::PreSaveGame_Implementation( saveVersion, gameVersion );
	mSavedOnce = true;  // See AACPRRail::PreSaveGame_Implementation.
}

bool AACPROutlet::GetTerminalLocalFrame( uint8 index, FTransform& out_frame ) const
{
	// The mount sits on the host terminal's cell facing back into it (PlaceMountInterface's rule),
	// so its frame is the host terminal's saved frame turned round, made relative to this actor.
	if( index != 0 || mHostMode != EACPROutletHostMode::Terminal || !IsValid( mHostActor ) )
	{
		return false;
	}
	const IACPRTerminalHost* host = Cast< IACPRTerminalHost >( mHostActor );
	FVector location, outward;
	if( !host || !FACPRCoupling::SavedTerminalFrame( mHostActor, host, mHostTerminalIndex, location, outward ) )
	{
		return false;
	}
	const FTransform world( FRotationMatrix::MakeFromX( -outward ).ToQuat(), location );
	out_frame = world.GetRelativeTransform( GetActorTransform() );
	return true;
}

void AACPROutlet::RefreshIndicators()
{
	// One cue slot; terminal 0 is the mount interface, which is what the Outlet's state IS.
	const FACPRIndicatorSlot slots[] = { { mOutletMesh.Get(), ACPRSlot::Cue(), 0 } };
	FACPRIndicators::Push( this, this, MakeArrayView( slots ), ACPR_TAG_TEXT( OUTLET ) );
}

void AACPROutlet::Dismantle_Implementation()
{
	FACPRCostScope cost( EACPRCost::Dismantle );
	// Vanilla's mass dismantle calls this once per actor in the SELECTION for every child it
	// collects, and a blueprint proxy makes every member a child of every other — a selection of a
	// few hundred actors dismantles one Outlet a few hundred times in one frame. Our release runs on
	// the first call only: nothing re-couples a dismantling actor, so the repeats have nothing to do
	// but reach Super. The count is logged once so the pattern stays visible.
	if( ++mDismantleCalls > 1 )
	{
		if( mDismantleCalls == 2 )
		{
			ACPR_LOG( Verbose, OUTLET,
				TEXT( "%s Dismantle called again in the same dismantle — vanilla repeats it for every selected actor sharing a blueprint proxy; once was enough" ),
				*GetName() );
		}
		Super::Dismantle_Implementation();
		return;
	}
	// §8.2: "Dismantling it reopens the terminal with no retroactive coupling." The Outlet is both
	// an attachment and a host, so both halves are released: its own mounting interface's coupling
	// to the host terminal, and its registration.
	FACPRCoupling::ReleaseAll( this, this );
	FACPRAttachments::Unregister( mHostActor, this );

	Super::Dismantle_Implementation();
}


void AACPROutlet::Remap( AActor* newHostActor, uint8 newTerminalIndex )
{
	AActor* oldHost = mHostActor;
	const EACPROutletHostMode mode = mHostMode;

	FACPRAttachments::Unregister( oldHost, this );

	if( mode == EACPROutletHostMode::Terminal )
	{
		// Release before re-placing, invariant 4's reason again: the interface must never be coupled to
		// two terminals, not even for the length of this function. Through the one implementation, so
		// both hosts' records and the hosts' hidden edge go with it.
		FACPRCoupling::Release( this, this, 0, TEXT( "remap" ) );

		SetHost( mode, newHostActor, newTerminalIndex );

		// Back on the NEW host terminal's cell with the opposing quantized axis. If the new host is
		// already registered the pair forms here; if it has not begun play yet (a Split's child, remapped
		// before FinishSpawning) its own BeginPlay finds the mount in the cell and forms it. Either way
		// §6.1 is the only coupling rule involved.
		PlaceMountInterface();
		RegisterTerminals();
		if( mMountInterface && HasAuthority() )
		{
			FACPRCoupling::TryCoupleTerminal( this, this, mMountInterface, 0 );
		}
	}
	else
	{
		// §8.1's hidden edge has to MOVE, not merely be added again: left on the old host it would keep
		// this Outlet on a circuit belonging to a Rail that is about to be destroyed.
		const IACPRTerminalHost* oldTerminalHost =
			IsValid( oldHost ) ? Cast< IACPRTerminalHost >( oldHost ) : nullptr;
		UACPRPowerConnectionComponent* oldPower = oldTerminalHost ? oldTerminalHost->GetPowerConnection() : nullptr;
		if( mPowerConnection && oldPower && mPowerConnection->HasHiddenConnection( oldPower ) )
		{
			mPowerConnection->RemoveHiddenConnection( oldPower );
		}

		SetHost( mode, newHostActor, newTerminalIndex );
		BondToBody();
	}

	FACPRAttachments::Register( mHostActor, this );

	LogOutlet( TEXT( "Remap" ) );
}

/**
 * Vanilla's own widget class, off the Mk1 pole's blueprint CDO, by reflection — the protected field
 * is read through its FProperty, not through the class. Once per session; null with one Warning if
 * the blueprint is not where it has been since Update 3, in which case the field is set in the editor.
 */
static TSubclassOf< UFGPoleConnectionsWidget > ACPRVanillaConnectionWidgetClass()
{
	static bool resolved = false;
	static TSubclassOf< UFGPoleConnectionsWidget > result;
	if( resolved )
	{
		return result;
	}
	resolved = true;

	static const TCHAR* PolePath = TEXT( "/Game/FactoryGame/Buildable/Factory/PowerPoleMk1/Build_PowerPoleMk1.Build_PowerPoleMk1_C" );
	UClass* poleClass = StaticLoadClass( AFGBuildablePowerPole::StaticClass(), nullptr, PolePath );
	const UObject* cdo = poleClass ? poleClass->GetDefaultObject() : nullptr;
	const FClassProperty* property = poleClass
		? CastField< FClassProperty >( poleClass->FindPropertyByName( TEXT( "mConnectionWidgetClass" ) ) )
		: nullptr;
	UClass* value = ( cdo && property ) ? Cast< UClass >( property->GetObjectPropertyValue_InContainer( cdo ) ) : nullptr;

	if( value && value->IsChildOf( UFGPoleConnectionsWidget::StaticClass() ) )
	{
		result = value;
		ACPR_LOG( Verbose, OUTLET, TEXT( "connections widget class from %s: %s" ), PolePath, *value->GetName() );
	}
	else
	{
		ACPR_LOG( Warning, OUTLET,
			TEXT( "no connections widget class: pole=%s property=%d value=%s — set mConnectionWidgetClass on Build_PowerRailOutlet to the Mk1 pole's" ),
			*GetNameSafe( poleClass ), property ? 1 : 0, *GetNameSafe( value ) );
	}
	return result;
}

void AACPROutlet::StartIsLookedAtForConnection( AFGCharacterPlayer* byCharacter, UFGCircuitConnectionComponent* overlappingConnection )
{
	Super::StartIsLookedAtForConnection( byCharacter, overlappingConnection );

	if( !mConnectionsWidgetComponent || !mPowerConnection )
	{
		return;
	}

	if( !mConnectionWidgetClass )
	{
		mConnectionWidgetClass = ACPRVanillaConnectionWidgetClass();
		if( !mConnectionWidgetClass )
		{
			return;
		}
	}

	if( mConnectionsWidgetComponent->GetWidgetClass() != mConnectionWidgetClass )
	{
		mConnectionsWidgetComponent->SetWidgetClass( mConnectionWidgetClass );
	}
	if( !mConnectionsWidgetComponent->GetUserWidgetObject() )
	{
		mConnectionsWidgetComponent->InitWidget();
	}

	// The count is the socket's: wires on it over its budget, which is what the pole's widget shows.
	if( UFGPoleConnectionsWidget* widget = Cast< UFGPoleConnectionsWidget >( mConnectionsWidgetComponent->GetUserWidgetObject() ) )
	{
		widget->SetConnection( mPowerConnection );
	}

	mConnectionsWidgetComponent->SetHiddenInGame( false );
}

void AACPROutlet::StopIsLookedAtForConnection( AFGCharacterPlayer* byCharacter )
{
	if( mConnectionsWidgetComponent )
	{
		mConnectionsWidgetComponent->SetHiddenInGame( true );
	}

	Super::StopIsLookedAtForConnection( byCharacter );
}

void AACPROutlet::EndPlay( const EEndPlayReason::Type endPlayReason )
{
	FACPRCostScope cost( EACPRCost::EndPlay );
	// §8.2: "Dismantling it reopens the terminal with no retroactive coupling." Reopening is the
	// mount's own OnComponentDestroyed and ReleaseAll at dismantle; the "no
	// retroactive coupling" half is satisfied by scheduling nothing, exactly as §9's Cap satisfies
	// §6.3's "removing a Cap never initiates a coupling search".
	FACPRAttachments::Unregister( mHostActor, this );

	Super::EndPlay( endPlayReason );
}

void AACPROutlet::PlaceMountInterface()
{
	const IACPRTerminalHost* host = ( mHostMode == EACPROutletHostMode::Terminal && IsValid( mHostActor ) )
		? Cast< IACPRTerminalHost >( mHostActor ) : nullptr;
	UACPRTerminalComponent* hostTerminal = ResolveHostTerminal();

	if( !mMountInterface || !host || !hostTerminal )
	{
		ACPR_LOG( Warning, OUTLET,
			TEXT( "%s is terminal-mounted and has no host terminal: actor=%s index=%d. "
			      "It will carry power to nothing." ),
			*GetName(),
			IsValid( mHostActor ) ? *mHostActor->GetName() : TEXT( "<null>" ),
			static_cast< int32 >( mHostTerminalIndex ) );
		return;
	}

	// Exactly coincident, exactly opposing, and then nothing else: the interface lands on the host
	// terminal's own cell facing back into it, and §6.1's coincident-pair rule forms the Coupling.
	// No coupling code lives here.
	//
	// From the host's saved geometry. On a load this Outlet's BeginPlay can run before the host's
	// has placed its terminal components, which would leave the mount on an unplaced terminal with a
	// zero axis. The saved frame is where the terminal WILL be (FACPRCoupling::SavedTerminalFrame),
	// whatever has run.
	FVector location, outward;
	if( !FACPRCoupling::SavedTerminalFrame( mHostActor, host, mHostTerminalIndex, location, outward ) )
	{
		const FTransform frame = hostTerminal->GetComponentTransform();
		location = frame.GetLocation();
		outward = frame.GetUnitAxis( EAxis::X );
	}
	mMountInterface->SetWorldLocationAndRotation( location, FRotationMatrix::MakeFromX( -outward ).ToQuat() );

	// And the quantized axis, which the transform does not carry: B.3 makes the opposing test exact
	// integer equality on a separately set reduced direction. The host terminal's own reduced outward
	// is set by the host at its BeginPlay; if that has not run yet (a loaded host after this Outlet),
	// the same integer is derived here from the saved frame the way the host derives it — the delta
	// of two quantized points — so the pair compares equal whichever side placed first.
	FIntVector hostOutward = hostTerminal->GetQuantizedOutward();
	if( hostOutward == FIntVector( 0, 0, 0 ) )
	{
		hostOutward = UACPRTerminalComponent::ReduceDirection(
			UACPRTerminalComponent::QuantizeLocation( location + outward * 100.0 )
			- UACPRTerminalComponent::QuantizeLocation( location ) );
	}
	mMountInterface->SetQuantizedOutward( FIntVector( -hostOutward.X, -hostOutward.Y, -hostOutward.Z ) );
}

void AACPROutlet::BondToBody()
{
	// §8.1: "Bonds without a visible Power Line, changes no terminal state, does not shorten the
	// host." All three are properties of a HIDDEN edge from the socket to the host's one connection:
	// hidden edges do not consume mMaxNumConnectionLinks (invariant 7), no terminal
	// is touched, and nothing about the host's geometry is read. The host's connection is a CDO
	// subobject, so this holds before the host's BeginPlay too.
	if( !HasAuthority() || !mPowerConnection )
	{
		return;
	}

	const IACPRTerminalHost* host = IsValid( mHostActor ) ? Cast< IACPRTerminalHost >( mHostActor ) : nullptr;
	UACPRPowerConnectionComponent* hostPower = host ? host->GetPowerConnection() : nullptr;
	if( !hostPower )
	{
		ACPR_LOG( Warning, OUTLET,
			TEXT( "%s is body-mounted on %s, which owns no power connection. Nothing to bond "
			      "to, so the Outlet will carry no power." ),
			*GetName(),
			IsValid( mHostActor ) ? *mHostActor->GetName() : TEXT( "<null>" ) );
		return;
	}

	if( !FACPRCoupling::VerifyPowerCircuitSafe( this, hostPower, TEXT( "outlet body host" ) ) ||
		!FACPRCoupling::VerifyPowerCircuitSafe( this, mPowerConnection, TEXT( "outlet socket" ) ) )
	{
		return;
	}

	if( !mPowerConnection->HasHiddenConnection( hostPower ) )
	{
		mPowerConnection->AddHiddenConnection( hostPower );
	}
}

void AACPROutlet::RegisterTerminals()
{
	// Registered in Terminal mode only. §6.1 finds coincident pairs through the registry, and in
	// Body mode there is no pair to find — the interface sits unused inside the Outlet's own body,
	// where a Rail end could coincidentally land on it and produce a Coupling §8.1 explicitly does
	// not have ("changes no terminal state"). Not registering it is how that is prevented, rather
	// than by a rule somewhere else that has to remember Outlets exist.
	if( mHostMode != EACPROutletHostMode::Terminal )
	{
		return;
	}

	UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this );
	if( !registry )
	{
		ACPR_LOG( Warning, OUTLET,
			TEXT( "%s found no terminal registry — it will couple to nothing." ),
			*GetName() );
		return;
	}

	if( mMountInterface )
	{
		registry->RegisterTerminal( mMountInterface );
	}
}

void AACPROutlet::SetUpMeshes()
{
	// One function for every door the saved state arrives through — see AACPRCap::SetUpMeshes.
	// The two connection components are attached here as well, so the blueprint world gets them
	// for free.
	if( USceneComponent* root = GetRootComponent() )
	{
		USceneComponent* const parts[ 4 ] = { mPowerConnection.Get(), mMountInterface.Get(), mOutletMesh.Get(), mOutletBase.Get() };
		for( USceneComponent* comp : parts )
		{
			if( comp && !comp->IsAttachedTo( root ) )
			{
				comp->AttachToComponent( root, FAttachmentTransformRules::KeepRelativeTransform );
			}
		}
	}
	FitMesh();
}

void AACPROutlet::FitMesh()
{
	if( !mOutletMesh )
	{
		ACPR_LOG( Warning, OUTLET,
			TEXT( "no OutletMesh component — Build_PowerRailOutlet's parent class is "
			      "wrong, and the Outlet will be invisible." ) );
		return;
	}

	if( !mOutletMesh->GetStaticMesh() )
	{
		ACPR_LOG( Warning, OUTLET,
			TEXT( "OutletMesh has no mesh assigned — run the editor script." ) );
		return;
	}

	// A Junction terminal's socket is a hole behind the face, so the Outlet sinks into it; a Rail
	// has nothing to sink into. Same terminal-COUNT test the Cap uses, so the two parts cannot
	// disagree about what kind of host they are on.
	const IACPRTerminalHost* fitHost = IsValid( mHostActor )
		? Cast< IACPRTerminalHost >( mHostActor ) : nullptr;
	FitOutletMeshes( mOutletMesh, mOutletBase, SeatFor( mHostMode, fitHost ), this );
}

AACPROutlet::ESeat AACPROutlet::SeatFor( EACPROutletHostMode mode, const IACPRTerminalHost* host )
{
	// Same terminal-COUNT test the Cap uses: a Junction has six, a Rail two.
	if( mode == EACPROutletHostMode::Body )
	{
		return ESeat::Body;
	}
	return ( host && host->GetTerminalCount() > 2 ) ? ESeat::JunctionTerminal : ESeat::RailTerminal;
}

void AACPROutlet::FitOutletMeshes( UStaticMeshComponent* cylinder, UStaticMeshComponent* base,
                                   ESeat seat, const AACPROutlet* cdo )
{
	if( !cdo )
	{
		return;
	}
	float inset = 0.0f, width = 0.0f;
	UStaticMesh* mesh = nullptr;
	switch( seat )
	{
		case ESeat::Body:
			inset = cdo->mBodyInset; width = cdo->mBaseWidthBody; mesh = cdo->mBaseMeshBody.LoadSynchronous(); break;
		case ESeat::JunctionTerminal:
			inset = cdo->mJunctionTerminalInset; width = cdo->mBaseWidthJunction; mesh = cdo->mBaseMeshJunction.LoadSynchronous(); break;
		default:
			inset = cdo->mRailTerminalInset; width = cdo->mBaseWidthRail; mesh = cdo->mBaseMeshRail.LoadSynchronous(); break;
	}

	// The cylinder's base sits on the pad's top, so both take the same standoff: minus the inset.
	FitOutletMesh( cylinder, cdo->mVisualExtent, cdo->mStandoff - inset );

	if( base )
	{
		if( mesh && base->GetStaticMesh() != mesh )
		{
			base->SetStaticMesh( mesh );
		}
		if( !mesh )
		{
			ACPR_LOG( Warning, OUTLET,
				TEXT( "no pad mesh for seat %d — run acpr_meshes.py (SM_ACPR_OutletBase*)." ),
				static_cast< int32 >( seat ) );
		}
		// The pad is authored like a Cap — face at z = 0, thickness reaching -Z — so the Cap's fit
		// places it: standoff is where its top lands relative to the mounting plane.
		AACPRCap::FitCapMesh( base, cdo->mBaseDepth, width, -inset );
	}
}

FVector AACPROutlet::FitOutletMesh( UStaticMeshComponent* comp,
                                    float visualExtent, float standoff )
{
	if( !comp )
	{
		return FVector::ZeroVector;
	}

	// §11's 0.9 m TRANSVERSE target, fitted in C++: the fit and the thing being fitted must come
	// from the same measurement, taken in the process that renders. AACPRCap's fit is reused rather
	// than copied — it is static and public for exactly this.
	//
	// Rotated onto the mounting normal, not fitted into a cube. The mesh is "a capped cylinder with
	// three ring grooves cut into it, authored along +Z with its base on the mounting plane"
	// (acpr_meshes.py, make_outlet); fitted unrotated into a cube of visualExtent, the barrel would
	// run along the actor's +Z while +X is the mounting normal, and a 40 x 40 x 26 cylinder would be
	// stretched to 90 x 90 x 90 — sideways and shapeless.
	//
	// Uniform, because a ribbed cylinder stretched to fill a cube stops reading as a socket. The
	// transverse pair decides the factor; the barrel then keeps its authored proportions along the
	// normal, however long that makes it.
	//
	// Stood off along local +X, which is the mounting normal in both modes: §8.2 takes it from the
	// terminal axis, §8.1 from the target face. One convention, so the body sits proud of whatever
	// it is clamped to rather than inside it — and the mesh's base is already on the plane, so the
	// standoff is a gap rather than a correction.
	return AACPRCap::FitZAuthoredMeshToLocalX(
		comp, FVector( visualExtent, visualExtent, visualExtent ), standoff, true );
}

void AACPROutlet::LogOutlet( const TCHAR* stage ) const
{
	const UACPRTerminalComponent* hostTerminal = ResolveHostTerminal();

	// wires= and max= are the pair to read first: max=0 means no Power Line can ever attach, which
	// looks exactly like a cable that connects and carries nothing. mount= is the other half — in
	// Terminal mode it must read Coupled by Tick+1 or no power reaches the socket at all. The mesh's
	// own transform is on this line because which way the socket points cannot be settled from a
	// screenshot: rot must read P=-90 for a mesh authored along +Z to lie along the actor's +X;
	// anything else and FitMesh either did not run or ran on the wrong component.
	const FString meshState = mOutletMesh
		? FString::Printf( TEXT( "%s rot=%s scale=%s loc=%s" ),
			mOutletMesh->GetStaticMesh() ? *mOutletMesh->GetStaticMesh()->GetName()
			                             : TEXT( "<none>" ),
			*mOutletMesh->GetRelativeRotation().ToString(),
			*mOutletMesh->GetRelativeScale3D().ToString(),
			*mOutletMesh->GetRelativeLocation().ToString() )
		: FString( TEXT( "<no component>" ) );

	ACPR_LOG( Verbose, OUTLET,
		TEXT( "%s %s | BUILD=%s | mode=%s host=%s index=%d | socket=%s | mount=%s "
		      "| hostTerminal=%s | actorRot=%s | mesh: %s" ),
		*GetName(), stage, ACPR_BUILD_STAMP(),
		mHostMode == EACPROutletHostMode::Terminal ? TEXT( "terminal" ) : TEXT( "body" ),
		IsValid( mHostActor ) ? *mHostActor->GetName() : TEXT( "<null>" ),
		static_cast< int32 >( mHostTerminalIndex ),
		mPowerConnection ? *mPowerConnection->Describe() : TEXT( "<none>" ),
		mMountInterface ? *mMountInterface->Describe() : TEXT( "<none>" ),
		hostTerminal ? *hostTerminal->Describe() : TEXT( "-" ),
		*GetActorRotation().ToString(),
		*meshState );
}
