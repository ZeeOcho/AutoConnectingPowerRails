// Auto-Connecting Power Rails — which build space a buildable or a placement belongs to.
//
// §10.1: "Terminal construction inside one Designer may couple only actors of that same Designer;
// Designer-to-world and cross-Designer Couplings are rejected."
//
// Without this rule nothing in the mod would know a Designer existed: §6.1's coincident-pair rule,
// the Rail's anchor snap and the load replay would all treat a terminal inside the Designer exactly
// like one outside it, and a Designer Rail flush against a world Rail would couple onto the world's
// circuit. The Hoverpack masks that in play — it does not attach to the Designer Rail even when that
// Rail is powered — so the rule is enforced here rather than left to observation.
//
// A space is the world (null) or one particular AFGBuildableBlueprintDesigner. Two terminals may
// couple, and a hologram may target a host, only within one space.
//
// Where the answer comes from, and why it is not a vanilla call:
//
//   AFGBuildable::mBlueprintDesigner   FGBuildable.h:1014   protected, SaveGame, Replicated
//   AFGHologram::mBlueprintDesigner    FGHologram.h:776     protected
//
// Both have getters (FGBuildable.h:530, FGHologram.h:448) and both getters are plain exports, which
// the shipping game does not provide for linking. The fields are protected members of our own base
// classes, so each of our classes reads its own field inline and no import is generated. That is why
// a terminal host answers through IACPRTerminalHost::GetHostDesigner rather than this file asking
// vanilla.

#pragma once

#include "CoreMinimal.h"
// TWeakObjectPtr lives in CoreUObject, not in CoreMinimal.
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class AFGBuildableBlueprintDesigner;

/**
 * An optional filter for FACPRTerminalHighlight::Show: when Enforce is set, a host outside Space shows
 * no markers at all. §12 says ineligible terminals "never display as valid", and a terminal in another
 * build space is ineligible however open it is.
 */
struct FACPRSpaceFilter
{
	bool Enforce = false;
	const AFGBuildableBlueprintDesigner* Space = nullptr;
};

struct AUTOCONNECTINGPOWERRAILS_API FACPRSpace
{
	/**
	 * The Designer a terminal host belongs to, or null for the world.
	 *
	 * Anything that is not an IACPRTerminalHost answers null. Every caller in this mod asks about a
	 * terminal's owner or a hologram's target host, and both are always hosts; a Cap is an
	 * attachment, never a coupling participant.
	 */
	static AFGBuildableBlueprintDesigner* OfActor( const AActor* actor );

	/** "world" or the Designer's name. */
	static FString Describe( const AFGBuildableBlueprintDesigner* designer );

	/**
	 * The hologram-side rule, once for all four holograms.
	 *
	 * True when `targetHost` exists and lies in a different space from `placementSpace` — the caller
	 * then adds vanilla's own disqualifier, UFGCDDesignerWorldCommingling (FGConstructDisqualifier.h:720,
	 * text key ConstructDisqualifiers/BlueprintDesigner/CannotConnect), so the player reads the same
	 * message vanilla shows for its own Designer-to-world connections.
	 *
	 * Logs the refusal, keyed so a hologram held still prints it once rather than every frame.
	 *
	 * @param hologram        for the log line only
	 * @param role            what the target is to this hologram: "anchor", "far end", "insert host", ...
	 * @param targetHost      the host being snapped to or inserted into; null means "no target", never foreign
	 * @param placementSpace  the hologram's own AFGHologram::mBlueprintDesigner
	 * @param lastLogKey      the caller's own throttle state
	 */
	static bool IsForeignTarget( const AActor* hologram, const TCHAR* role, const AActor* targetHost,
	                             const AFGBuildableBlueprintDesigner* placementSpace, FString& lastLogKey );

	/**
	 * Logs when a hologram's placement space changes — the one fact about vanilla's Designer handling
	 * no header states: when AFGHologram::mBlueprintDesigner is written, and whether it follows the
	 * aim, the hit actor or the hologram's own location. If it lags a snap, the refusals above are late
	 * by the same amount, and this line is what shows it.
	 */
	static void WatchPlacementSpace( const AActor* hologram,
	                                 const AFGBuildableBlueprintDesigner* placementSpace,
	                                 TWeakObjectPtr< AFGBuildableBlueprintDesigner >& lastSpace,
	                                 bool& seenOnce );
};
