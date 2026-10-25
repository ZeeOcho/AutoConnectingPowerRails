// Auto-Connecting Power Rails — §16.5's socket table: how the link end tube fits each machine's power socket.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ACPRSocketTable.generated.h"

/**
 * The fit of the link end tube to one power connection's socket knob, in the connection
 * component's frame: the knob's axis, the offset from the connection point to that axis, the
 * knob's radius and the straight wall's extent along the axis. Measured in the editor by
 * Scripts/acpr_sockets.py — a packaged game cannot read mesh vertices — and replicated per link
 * once resolved to world space (FACPRLinkTarget).
 */
USTRUCT()
struct AUTOCONNECTINGPOWERRAILS_API FACPRSocketFit
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Axis = FVector::UpVector;

	UPROPERTY()
	FVector Offset = FVector::ZeroVector;

	/** The outer radius: the discs of the insulator stack, which the outer tube wraps. */
	UPROPERTY()
	float Radius = 29.5f;

	/** The knob's own cylinder between the discs, which the inner tube wraps; no inner tube when it is not inside Radius. */
	UPROPERTY()
	float Core = 17.8f;

	UPROPERTY()
	float Bottom = -28.0f;

	UPROPERTY()
	float Top = 26.0f;
};

/**
 * The shipped socket table as an asset: the text of Config/ACPR_Sockets.json, written into
 * Content/Data/DA_ACPR_SocketTable by Scripts/acpr_sockets.py alongside the JSON. A loose file in
 * the plugin folder is not packaged; an asset is.
 */
UCLASS()
class AUTOCONNECTINGPOWERRAILS_API UACPRSocketTableAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY( EditDefaultsOnly, Category = "ACPR" )
	FString Json;
};

/**
 * The table of socket fits, keyed by buildable class path and power connection component name.
 * Loaded once per process from the shipped asset (UACPRSocketTableAsset, the generated and
 * committed Config/ACPR_Sockets.json), then overlaid with the player's own
 * Configs/AutoConnectingPowerRails_Sockets.json beside SML's config files when there is one —
 * same shape, any subset of keys; an entry there replaces the plugin's for that class and
 * connection, and a "default" there replaces the default fit.
 *
 * A machine is looked up by its class, then by each parent class in turn, so a mod's child of a
 * vanilla machine (same mesh) gets the vanilla fit while a mod's own same-named class (different
 * path) does not; a class the table does not know gets the default fit, and is logged once.
 */
class AUTOCONNECTINGPOWERRAILS_API FACPRSocketTable
{
public:
	static const FACPRSocketTable& Get();

	/**
	 * Reads the table again — the asset and the player's override — so a correction in the override
	 * file can be seen without restarting the game (the acpr.ReloadSockets console command, which
	 * then refits every outlet's link ends). Returns a line saying what was loaded.
	 */
	static FString Reload();

	/** The fit for `connection` on a buildable of `buildableClass`; `known` says whether the table had it. */
	const FACPRSocketFit& Find( const UClass* buildableClass, FName connection, bool* known = nullptr ) const;

	const FACPRSocketFit& Default() const { return mDefault; }
	const FString& GameChangelist() const { return mGameChangelist; }
	int32 ClassCount() const { return mSockets.Num(); }

private:
	FACPRSocketTable();
	/** Reads one JSON text into the table; entries replace what is there. Returns how many classes it held, -1 if invalid. */
	int32 Load( const FString& text, const FString& source, const TCHAR* what );

	FACPRSocketFit mDefault;
	FString mGameChangelist;
	TMap< FString, TMap< FName, FACPRSocketFit > > mSockets;
	mutable TSet< FString > mReportedUnknown;
};
