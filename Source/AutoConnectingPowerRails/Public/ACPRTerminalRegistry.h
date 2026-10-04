// Auto-Connecting Power Rails — the terminal registry.
//
// B.3: "Quantize terminal positions on registration and compare quantized values for equality."
// This is the thing they register WITH. It is a spatial hash from quantized cell to the terminals
// standing in it, which makes §6.1's coincident-pair test a map lookup rather than a search.
//
// Why a subsystem rather than a search. The alternative is iterating actors when a Rail is built,
// which is O(actors) per construction and gets worse exactly as a factory gets bigger. It is also
// the wrong shape for §10: the blueprint resolver needs "every candidate terminal near this point"
// as an immutable snapshot (§10.3), and a registry can hand that over while an actor iteration
// cannot do it cheaply.
//
// Why a UWorldSubsystem rather than a static. One registry per world, created and destroyed with
// it, with no lifetime of its own to get wrong — which matters in an editor session that opens and
// closes PIE repeatedly, and in a dedicated-server process. A static map would leak stale entries
// across worlds and would need locking the moment anything touched it off the game thread.
//
// Terminals are held weakly. A registry that kept hard references would keep dismantled Rails alive.

#pragma once

#include "CoreMinimal.h"
#include "ACPRSpatialGrid.h"
#include "Subsystems/WorldSubsystem.h"
#include "ACPRTerminalRegistry.generated.h"

class UACPRTerminalComponent;

/**
 * The coarse index over occupied cells, one foundation (4 m) to a bucket. That is the scale
 * factories are laid out at, and it is far wider than any lateral tolerance a query uses (the ray
 * margin is 60 uu), so a ray's samples each touch at most 2×2×2 buckets — see TACPRSpatialGrid.
 */
static constexpr int32 ACPR_REGISTRY_BUCKET = 400;

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API UACPRTerminalRegistry : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** How many distinct positions hold terminals, and how many index buckets they occupy. */
	int32 GetCellCount() const { return mCells.Num(); }
	int32 GetBucketCount() const { return mBuckets.GetBucketCount(); }

	/** Buckets the last FindAlongAxis / FindNear touched — the cost of that query, for the log. */
	int32 GetLastBucketsVisited() const { return mBuckets.GetLastVisited(); }

	/** Null if there is no world, which is normal during shutdown — callers must check. */
	static UACPRTerminalRegistry* Get( const UObject* worldContext );

	/**
	 * Add or move a terminal to the cell it currently occupies.
	 *
	 * Deliberately NOT called from the terminal's own BeginPlay. A component's BeginPlay runs
	 * inside the actor's Super::BeginPlay, which is BEFORE the Rail has had a chance to position
	 * its far terminal from mLength — so self-registration would file every terminal under the
	 * actor origin. The owner calls this once the geometry is final, and may call it again if the
	 * geometry changes.
	 */
	void RegisterTerminal( UACPRTerminalComponent* terminal );

	void UnregisterTerminal( UACPRTerminalComponent* terminal );

	/**
	 * Every registered terminal in the same cell as `cell`, excluding `exclude` and anything whose
	 * owner is `excludeOwner` — a Rail's own two terminals must never be candidates for each other.
	 */
	void FindInCell( const FIntVector& cell,
	                 const UACPRTerminalComponent* exclude,
	                 const AActor* excludeOwner,
	                 TArray< UACPRTerminalComponent* >& out_terminals ) const;

	/**
	 * Every registered terminal on a ray, which is §5.1's "the first terminal encountered along the
	 * Rail's axis" asked as one question instead of reconstructed by the caller.
	 *
	 * Why this is here and not a physics query. UWorld::LineTraceMultiByChannel along the Rail's span
	 * looks right — the physics scene is already a spatial index and §11 makes our buildables
	 * query-blocking — but LineTraceMulti stops at the first BLOCKING hit, returning the touches
	 * before it and then terminating, and a trace that starts on the anchor host's own surface ends
	 * where it begins. Ignoring the anchor would not save it either: §11's soft clearance means a
	 * Rail routinely runs through walls and foundations, and every one of those would truncate the
	 * scan just the same. A query that stops at the first obstacle cannot answer a question about a
	 * Rail that is allowed to pass through obstacles.
	 *
	 * The registry has no such rule. It holds exactly the objects the question is about, with no
	 * opinion about what occludes what.
	 *
	 * Cost: the buckets along the ray are enumerated (TACPRSpatialGrid::ForEachAlongSegment), and
	 * within them one dot product and one distance per OCCUPIED CELL, rejected before any terminal
	 * is touched — the key is the quantized position, so it answers the geometric question itself.
	 * Proportional to the ray's length and the density around it, not to the world, which matters
	 * because the blueprint manager asks it hundreds of times per frame.
	 *
	 * @param minAlong   distance along the axis at which the span starts counting — excludes the
	 *                   anchor's own terminal at zero without needing to know which one it is
	 * @param maxAlong   and where it stops, which excludes the far end's own snap target
	 * @param tolerance  how far off the ray a terminal may sit and still be ON it
	 */
	void FindAlongAxis( const FVector& origin, const FVector& axis,
	                    double minAlong, double maxAlong, double tolerance,
	                    TArray< UACPRTerminalComponent* >& out_terminals ) const;

	/**
	 * Every registered terminal within `radius` of a point, for §7.4's cell-occupancy rules.
	 *
	 * The same bucketed cell pass as FindAlongAxis and for the same reason — see that comment for why
	 * this is not a physics overlap. The radius here has to reach a whole Rail length, because the
	 * question "does an unrelated Rail cross this Junction cell" is about a BODY whose terminals may
	 * be 40 m away; the caller groups the results by owner and tests each owner's geometry itself.
	 * A sphere that wide covers thousands of buckets, so in any world smaller than that the grid's
	 * hybrid rule walks the occupied buckets instead, so the sphere never costs more than the world.
	 */
	void FindNear( const FVector& centre, double radius,
	               TArray< UACPRTerminalComponent* >& out_terminals ) const;

	/** Registered terminals and occupied cells, for the log. Also prunes dead weak pointers. */
	void GetCounts( int32& out_terminals, int32& out_cells );

private:
	/** Cell key -> the terminals in it. The exact layer: FindInCell is one lookup. */
	TMap< FIntVector, TArray< TWeakObjectPtr< UACPRTerminalComponent > > > mCells;

	/** Bucket -> the cell keys in it. The coarse layer: which cells a ray or a sphere can touch. */
	TACPRSpatialGrid< FIntVector, ACPR_REGISTRY_BUCKET > mBuckets;

	void AppendCell( const FIntVector& cell, TArray< UACPRTerminalComponent* >& out_terminals ) const;

	/**
	 * Which cell each terminal is filed under, so a re-registration can remove it from the old one.
	 *
	 * The alternative — storing the cell on the terminal — would put a field on every terminal that
	 * only the registry is allowed to write, and would go stale the moment a terminal outlived a
	 * registry. Keeping both halves of the index in one object means they cannot disagree.
	 */
	TMap< TWeakObjectPtr< UACPRTerminalComponent >, FIntVector > mCellOf;
};
