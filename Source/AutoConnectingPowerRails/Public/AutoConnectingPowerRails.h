// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "ACPRLog.h"

/**
 * The diagnostics gate. Everything that exists to answer a question rather than to make the mod work
 * — per-evaluation timing, the cost meter, placement and nudge traces, the bridge anatomy, the
 * rigidity check's line — runs only while `acpr.Trace` is non-zero. The test is one cached integer
 * read, made before any string is built, so the cost of a gated line in shipping is the branch and
 * nothing else. A mechanism that makes a problem go away and counts how often it did is not a
 * diagnostic and does not belong behind this gate.
 */
struct AUTOCONNECTINGPOWERRAILS_API FACPRTrace
{
	static bool Enabled();
};

class FAutoConnectingPowerRailsModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/**
	 * §10's registration hook: before AFGBlueprintHologram::BeginPlay, register the Power Rail
	 * open-connection manager. The class NAME matters — Config/AccessTransformers.ini makes this class
	 * a friend of AFGBlueprintHologram so it may call the protected RegisterOpenConnectionManager< T >().
	 */
	FDelegateHandle mBlueprintBeginPlayHook;
	FDelegateHandle mBlueprintBuildModeHook;

};
