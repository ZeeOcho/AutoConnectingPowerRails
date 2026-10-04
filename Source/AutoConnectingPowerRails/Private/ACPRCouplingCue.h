// Auto-Connecting Power Rails — §12's coupling cue: a spark at every new Coupling, one sound per transaction.
//
// §12: "Creating a Coupling plays the vanilla electrical-connection cue — spark VFX plus the electrical
// sound. It fires regardless of whether the network carries power ... For a placement creating many
// Couplings at once, the sound fires once per construction transaction to avoid a pile-up, while the
// spark plays at each coupling point."
//
// Nothing in the FactoryGame headers names the asset vanilla plays when a Power Line snaps to a pole: the
// wire's cue lives in Blueprint. So the cue is chosen, not known:
//
//   * Sound — defaults to UFGFactorySettings::mHologramSnapSound (FGFactorySettings.h:206, "Snapping sound
//     for holograms, e.g. when a conveyor snaps to an output"), a vanilla Wwise event read off the settings
//     CDO by reflection: vanilla's own "this snapped" sound.
//   * Spark — picked automatically from the cooked asset registry: the best-scoring Niagara or Cascade
//     system whose name says spark / zap / electric / arc. The log names what was picked.
//   * Audition — `acpr.ListCueCandidates` prints numbered candidates (S0.., E0..) from the registry;
//     `acpr.CouplingCueTest S3` / `E2` plays one in front of the camera; `acpr.CouplingCueTest` plays the
//     configured cue. `acpr.CouplingSound` / `acpr.CouplingSpark` take an asset path (or `none`).
//
// How it plays without linking Wwise or Niagara: Build.cs keeps AkAudio and Niagara out (their headers are
// not needed anywhere else), so both are called through reflection: UAkGameplayStatics::PostEventAtLocation
// and UNiagaraFunctionLibrary::SpawnSystemAtLocation are static UFUNCTIONs, found by name and invoked with
// ProcessEvent on their class default object, parameters filled by type and name. A Cascade system goes
// through the engine's own UGameplayStatics::SpawnEmitterAtLocation.
//
// What fires it: FACPRCoupling::Form — every Coupling made by construction (a snap, a coincident pair, a
// blueprint boundary, a bridge's two ends, an Outlet's mount). Never a restore: a save load replays records
// and never calls Form, and the one load path that can (a record re-resolved by coincidence) is wrapped in
// FSuppressScope. Couplings restored inside a placed blueprint are not new either, and are silent.

#pragma once

#include "CoreMinimal.h"

class UWorld;

struct FACPRCouplingCue
{
	/**
	 * One new Coupling at `location`. Batched per frame: the first request schedules a flush for the next
	 * tick, and then the sound plays once (at the first point) and a spark plays at each point (capped). Ignored outside a game
	 * world with a player (the blueprint preview world has none) and while an FSuppressScope is open.
	 */
	static void Request( UWorld* world, const FVector& location, const FVector& outward, const FString& context );

	/** While one of these is alive, Request is a no-op. For the load paths that may call Form. */
	struct FSuppressScope
	{
		FSuppressScope();
		~FSuppressScope();
	};
};
