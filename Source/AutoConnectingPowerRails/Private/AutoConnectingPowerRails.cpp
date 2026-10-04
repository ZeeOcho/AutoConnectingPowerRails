// Copyright Epic Games, Inc. All Rights Reserved.

#include "AutoConnectingPowerRails.h"

#include "ACPRBlueprintTerminalManager.h"
#include "HAL/IConsoleManager.h"
#include "Hologram/FGBlueprintHologram.h"
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

	ACPR_LOG( Display, MODULE, TEXT( "module unloaded" ) );
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAutoConnectingPowerRailsModule, AutoConnectingPowerRails)
