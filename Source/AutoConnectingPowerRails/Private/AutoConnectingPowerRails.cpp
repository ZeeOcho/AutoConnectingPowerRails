// Copyright Epic Games, Inc. All Rights Reserved.

#include "AutoConnectingPowerRails.h"

#include "ACPRBlueprintTerminalManager.h"
#include "ACPRWirelessLinkSubsystem.h"
#include "ACPRWireAutoOutlet.h"
#include "ACPRWirelessPreviewComponent.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "Buildables/FGBuildableWire.h"
#include "FGBuildableSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"
#include "HAL/IConsoleManager.h"
#include "Hologram/FGBlueprintHologram.h"
#include "Hologram/FGBuildableHologram.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"
#include "Patching/NativeHookManager.h"

DEFINE_LOG_CATEGORY( LogAutoConnectingPowerRails );

static TAutoConsoleVariable< int32 > CVarACPRTrace(
	TEXT( "acpr.Trace" ), 0,
	TEXT( "Power Rails: 1 turns on the diagnostic lines (timing, cost meter, placement/nudge traces, bridge anatomy). 0 in play." ),
	ECVF_Default );

bool FACPRTrace::Enabled()
{
	return CVarACPRTrace.GetValueOnGameThread() != 0;
}

#define LOCTEXT_NAMESPACE "FAutoConnectingPowerRailsModule"

void FAutoConnectingPowerRailsModule::StartupModule()
{
	ACPR_LOG( Display, MODULE, TEXT( "module loaded | BUILD=%s" ), ACPR_BUILD_STAMP() );

	// SML's native hooks patch the shipping game's code. In the editor there is nothing to patch and no
	// blueprint hologram to register on, so the hook is not installed there — the same guard Vertical
	// Conveyor Auto-Connect ships with.
	if( WITH_EDITOR )
	{
		return;
	}

	// §10: register our manager on every blueprint hologram, before its BeginPlay, so it is in
	// AFGBlueprintHologram::mOpenConnectionManagers (FGBlueprintHologram.h:159) when vanilla's
	// BeginPlay initializes the managers with the blueprint's buildables.
	//
	// The route is a proven one, which is the point of choosing it. Vertical Conveyor Auto-Connect
	// 1.0.2 registers its own manager exactly this way, in shipping:
	// SUBSCRIBE_METHOD_VIRTUAL on AFGBlueprintHologram::BeginPlay (a virtual, so its symbol is already
	// referenced by every subclass vtable), and a Friend entry in Config/AccessTransformers.ini for the
	// protected RegisterOpenConnectionManager< T >() (FGBlueprintHologram.h:105, defined inline at
	// :180-189). A lambda inside a member function has that member's access, so the friendship covers it.
	mBlueprintBeginPlayHook = SUBSCRIBE_METHOD_VIRTUAL(
		AFGBlueprintHologram::BeginPlay,
		GetMutableDefault< AFGBlueprintHologram >(),
		[]( auto& /*scope*/, AFGBlueprintHologram* hologram )
		{
			if( IsValid( hologram ) )
			{
				hologram->RegisterOpenConnectionManager< FACPRBlueprintTerminalManager >();

				ACPR_LOG( Display, BP,
					TEXT( "manager registered on %s" ), *hologram->GetName() );
			}
		} );

	// The build-mode change, after vanilla has handled it, handed to whichever manager is
	// registered on that hologram. Same route as the BeginPlay hook above — a virtual, an after-hook
	// (no scope, cannot alter vanilla's own handling), the CDO as the vtable sample.
	mBlueprintBuildModeHook = SUBSCRIBE_METHOD_VIRTUAL_AFTER(
		AFGBlueprintHologram::OnBuildModeChanged,
		GetMutableDefault< AFGBlueprintHologram >(),
		[]( AFGBlueprintHologram* hologram, TSubclassOf< UFGHologramBuildModeDescriptor > buildMode )
		{
			if( IsValid( hologram ) )
			{
				FACPRBlueprintTerminalManager::OnBuildModeChanged( hologram, buildMode );
			}
		} );

	ACPR_LOG( Display, BP,
		TEXT( "hooks installed | before AFGBlueprintHologram::BeginPlay valid=%d | after OnBuildModeChanged valid=%d" ),
		mBlueprintBeginPlayHook.IsValid() ? 1 : 0, mBlueprintBuildModeHook.IsValid() ? 1 : 0 );

	// §16.4's four events without a broadcast, and §16.5's preview on vanilla holograms. Each
	// handler hands the fact to the world's link subsystem and nothing else; the subsystem's header
	// says why each of these is the right call to listen to.

	// A foundation or wall became an instance: after AddFromBuildable, so the instance exists. The
	// actor is still alive here (its BeginPlay destroys it right after), which is where the class
	// and the transform come from.
	mLightweightAddedHook = SUBSCRIBE_METHOD_AFTER(
		AFGLightweightBuildableSubsystem::AddFromBuildable,
		[]( const int32& index, AFGLightweightBuildableSubsystem* /*subsystem*/, AFGBuildable* buildable,
		    AActor* /*buildEffectInstigator*/, AFGBlueprintProxy* /*blueprintProxy*/ )
		{
			if( index != INDEX_NONE && IsValid( buildable ) )
			{
				if( UACPRWirelessLinkSubsystem* links = UACPRWirelessLinkSubsystem::Get( buildable ) )
				{
					links->LightweightArrived( buildable->GetClass(), buildable->GetActorTransform() );
				}
			}
		} );

	// A Power Line is being dismantled — by the player, or by a machine's own dismantle, which
	// dismantles its wires first (FGBuildable.cpp, Dismantle_Implementation). Before vanilla's
	// handling, with the flag it sets first (mIsDismantled) still clear, so a repeat is skipped;
	// the wire's removal is never broadcast otherwise.
	mBuildableDismantleHook = SUBSCRIBE_METHOD_VIRTUAL(
		AFGBuildable::Dismantle_Implementation,
		GetMutableDefault< AFGBuildable >(),
		[]( auto& /*scope*/, AFGBuildable* buildable )
		{
			const AFGBuildableWire* wire = Cast< AFGBuildableWire >( buildable );
			if( wire && !wire->GetIsDismantled() )
			{
				if( UACPRWirelessLinkSubsystem* links = UACPRWirelessLinkSubsystem::Get( wire ) )
				{
					links->WireDismantling( wire );
				}
			}
		} );

	// A foundation or wall is being removed: before its instance goes, while the lightweight
	// system still holds its transform under that class and index.
	mLightweightRemovedHook = SUBSCRIBE_METHOD(
		AFGLightweightBuildableSubsystem::RemoveByInstanceIndex,
		[]( auto& /*scope*/, AFGLightweightBuildableSubsystem* subsystem, TSubclassOf< AFGBuildable > buildableClass, int32 instanceIndex )
		{
			if( !IsValid( subsystem ) || !buildableClass || instanceIndex == INDEX_NONE )
			{
				return;
			}
			const FRuntimeBuildableInstanceData* data = subsystem->GetRuntimeDataForBuildableClassAndIndex( buildableClass, instanceIndex );
			if( UACPRWirelessLinkSubsystem* links = data ? UACPRWirelessLinkSubsystem::Get( subsystem ) : nullptr )
			{
				links->LightweightLeaving( buildableClass, data->Transform );
			}
		} );

	// A buildable began play inside a Blueprint Designer (AFGBuildable::BeginPlay routes Designer
	// buildables here instead of AddBuildable) / is being dismantled inside one (AFGBuildable::EndPlay).
	mDesignerSpawnedHook = SUBSCRIBE_METHOD_AFTER(
		AFGBuildableSubsystem::OnDesignerBuildableSpawned,
		[]( AFGBuildableSubsystem* /*subsystem*/, AFGBuildable* buildable )
		{
			if( UACPRWirelessLinkSubsystem* links = IsValid( buildable ) ? UACPRWirelessLinkSubsystem::Get( buildable ) : nullptr )
			{
				links->DesignerBuildableSpawned( buildable );
			}
		} );
	mDesignerDismantledHook = SUBSCRIBE_METHOD(
		AFGBuildableBlueprintDesigner::OnBuildableDismantledInsideDesigner,
		[]( auto& /*scope*/, AFGBuildableBlueprintDesigner* /*designer*/, AFGBuildable* buildable )
		{
			if( UACPRWirelessLinkSubsystem* links = IsValid( buildable ) ? UACPRWirelessLinkSubsystem::Get( buildable ) : nullptr )
			{
				links->DesignerBuildableDismantled( buildable );
			}
		} );

	// A machine's hologram began play: after AFGBuildableHologram::BeginPlay, so its connection
	// components are set up. Same route as the blueprint hologram hooks above (a virtual, the CDO as
	// the vtable sample).
	mMachineHologramHook = SUBSCRIBE_METHOD_VIRTUAL_AFTER(
		AFGBuildableHologram::BeginPlay,
		GetMutableDefault< AFGBuildableHologram >(),
		[]( AFGBuildableHologram* hologram )
		{
			UACPRWirelessPreviewComponent::AttachToMachineHologram( hologram );
		} );

	// §8.4: the wire hologram's third automatic pole. Installed from the friend class, which the
	// private setter it hooks requires (Config/AccessTransformers.ini).
	FACPRWireAutoOutlet::Install();

	ACPR_LOG( Display, WIRELESS,
		TEXT( "hooks installed | after AddFromBuildable valid=%d | before RemoveByInstanceIndex valid=%d | before AFGBuildable::Dismantle valid=%d | after OnDesignerBuildableSpawned valid=%d | before OnBuildableDismantledInsideDesigner valid=%d | after AFGBuildableHologram::BeginPlay valid=%d" ),
		mLightweightAddedHook.IsValid() ? 1 : 0, mLightweightRemovedHook.IsValid() ? 1 : 0, mBuildableDismantleHook.IsValid() ? 1 : 0,
		mDesignerSpawnedHook.IsValid() ? 1 : 0, mDesignerDismantledHook.IsValid() ? 1 : 0, mMachineHologramHook.IsValid() ? 1 : 0 );
}

void FAutoConnectingPowerRailsModule::ShutdownModule()
{
	if( mBlueprintBeginPlayHook.IsValid() )
	{
		UNSUBSCRIBE_METHOD( AFGBlueprintHologram::BeginPlay, mBlueprintBeginPlayHook );
		mBlueprintBeginPlayHook.Reset();
	}
	if( mBlueprintBuildModeHook.IsValid() )
	{
		UNSUBSCRIBE_METHOD( AFGBlueprintHologram::OnBuildModeChanged, mBlueprintBuildModeHook );
		mBlueprintBuildModeHook.Reset();
	}
	FACPRWireAutoOutlet::Uninstall();
	if( mLightweightAddedHook.IsValid() )
	{
		UNSUBSCRIBE_METHOD( AFGLightweightBuildableSubsystem::AddFromBuildable, mLightweightAddedHook );
		mLightweightAddedHook.Reset();
	}
	if( mLightweightRemovedHook.IsValid() )
	{
		UNSUBSCRIBE_METHOD( AFGLightweightBuildableSubsystem::RemoveByInstanceIndex, mLightweightRemovedHook );
		mLightweightRemovedHook.Reset();
	}
	if( mBuildableDismantleHook.IsValid() )
	{
		UNSUBSCRIBE_METHOD( AFGBuildable::Dismantle_Implementation, mBuildableDismantleHook );
		mBuildableDismantleHook.Reset();
	}
	if( mDesignerSpawnedHook.IsValid() )
	{
		UNSUBSCRIBE_METHOD( AFGBuildableSubsystem::OnDesignerBuildableSpawned, mDesignerSpawnedHook );
		mDesignerSpawnedHook.Reset();
	}
	if( mDesignerDismantledHook.IsValid() )
	{
		UNSUBSCRIBE_METHOD( AFGBuildableBlueprintDesigner::OnBuildableDismantledInsideDesigner, mDesignerDismantledHook );
		mDesignerDismantledHook.Reset();
	}
	if( mMachineHologramHook.IsValid() )
	{
		UNSUBSCRIBE_METHOD( AFGBuildableHologram::BeginPlay, mMachineHologramHook );
		mMachineHologramHook.Reset();
	}

	ACPR_LOG( Display, MODULE, TEXT( "module unloaded" ) );
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAutoConnectingPowerRailsModule, AutoConnectingPowerRails)
