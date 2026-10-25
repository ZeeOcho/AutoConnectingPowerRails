// Auto-Connecting Power Rails — §8.4: a Power Line aimed at a Rail builds a Power Rail Outlet.

#pragma once

#include "CoreMinimal.h"

class AACPROutletHologram;
class AFGWireHologram;
struct FHitResult;

/**
 * Vanilla's wire hologram carries two child holograms, a Power Pole and a Wall Outlet, and makes
 * one of them its automatic pole on every aim: the pole on the ground, the wall outlet on a wall or
 * a beam. A Rail is a beam, so a cable aimed at a Rail builds a Wall Outlet on it — a near look-alike
 * of the Rail Outlet that does not connect to the Rail. This gives the wire hologram a third child,
 * the Rail Outlet's hologram, and makes it the automatic pole whenever the aim is a Rail body or an
 * open terminal (a Rail's or a Junction's) — the Rail Outlet hologram's own IsValidHitResult and
 * TrySnapToActor decide that, as they do when the outlet is built by hand. A child hologram that is
 * disabled (FGHologram.h: "Children can be optionally disabled with mIsDisabled … should not be
 * visible or constructed") takes no part in placement, cost or construction, which is how vanilla
 * keeps the pole it did not choose out of the way; the Rail Outlet child is disabled the same way
 * whenever it is not the choice.
 *
 * The child exists only once vanilla's own Wall Outlet child does — that is vanilla's sign that the
 * wire is in automatic-pole mode at all — and only when the Rail Outlet recipe is unlocked.
 *
 * Everything here runs where the hologram runs — the owning player's client, and the server for a
 * remote player's hologram — through SML hooks on AFGWireHologram, installed by Install() from the
 * module. The private setter and fields are reached through Config/AccessTransformers.ini, which
 * is also why the hooks are installed from inside this class: taking the address of a private
 * member needs the friendship.
 *
 * Besides the three hooks that do the work (after TrySnapToActor and after
 * SetHologramLocationAndRotation: the choice, whichever of the two vanilla ran for the frame; after
 * Construct: the cable's end on the built Outlet), the installation hooks vanilla's remaining
 * steps — BeginPlay and DoMultiStepPlacement — and logs at Verbose what vanilla holds at each,
 * whenever that changes. The wire hologram's source is not available; the log is the record of
 * how it behaves.
 */
class FACPRWireAutoOutlet
{
public:
	static void Install();
	static void Uninstall();

	/** The Rail Outlet child of a wire hologram, if it has one. */
	static AACPROutletHologram* ChildOf( const AFGWireHologram* wire );

private:
	/** The Rail Outlet child, spawned (disabled, hidden) when the wire has its Wall Outlet child and the recipe is unlocked. */
	static AACPROutletHologram* EnsureChild( AFGWireHologram* wire );

	/**
	 * After vanilla placed the wire for an aim (its snap or its placement): the Rail Outlet is the
	 * automatic pole and the cable's second end on a Rail aim, disabled otherwise.
	 */
	static void OnWireAimed( AFGWireHologram* wire, const FHitResult& hit );

	/** After vanilla built the wire: the cable's second end is on the built Rail Outlet, if one was built. */
	static void OnWireConstructed( AFGWireHologram* wire, AActor* built, const TArray< AActor* >& children );

	/** One line of what the wire holds: connection step, its children and which is active. */
	static FString State( const AFGWireHologram* wire );

	/** Logs `line` for `wire` under `where` at Verbose, when it differs from the last one logged there. */
	static void LogChange( const AFGWireHologram* wire, const TCHAR* where, const FString& line );
};
