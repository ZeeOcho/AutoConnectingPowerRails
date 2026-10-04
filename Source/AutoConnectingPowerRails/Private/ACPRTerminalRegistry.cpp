// Auto-Connecting Power Rails — the terminal registry.

#include "ACPRTerminalRegistry.h"

#include "ACPRTerminalComponent.h"
#include "AutoConnectingPowerRails.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

/** A cell key is the 1 uu quantized position, so it is its own coordinate. */
static FVector ACPRCellPosition( const FIntVector& cell )
{
	return FVector( cell.X, cell.Y, cell.Z );
}

UACPRTerminalRegistry* UACPRTerminalRegistry::Get( const UObject* worldContext )
{
	if( !worldContext )
	{
		return nullptr;
	}

	// EGetWorldErrorMode::ReturnNull rather than the asserting variant: this is called from
	// BeginPlay and EndPlay, and EndPlay during world teardown legitimately has no world.
	UWorld* world = GEngine
		? GEngine->GetWorldFromContextObject( worldContext, EGetWorldErrorMode::ReturnNull )
		: nullptr;

	return world ? world->GetSubsystem< UACPRTerminalRegistry >() : nullptr;
}

void UACPRTerminalRegistry::RegisterTerminal( UACPRTerminalComponent* terminal )
{
	if( !terminal )
	{
		return;
	}

	const FIntVector cell = terminal->GetQuantizedLocation();
	const TWeakObjectPtr< UACPRTerminalComponent > key( terminal );

	if( const FIntVector* existing = mCellOf.Find( key ) )
	{
		if( *existing == cell )
		{
			return;  // Already filed correctly. Re-registration is idempotent by design.
		}

		// Moved. Remove from the old cell before filing under the new one, or the terminal would
		// answer a coincidence query from a position it has left.
		if( TArray< TWeakObjectPtr< UACPRTerminalComponent > >* old = mCells.Find( *existing ) )
		{
			old->Remove( key );
			if( old->Num() == 0 )
			{
				mCells.Remove( *existing );
				mBuckets.Remove( ACPRCellPosition( *existing ), *existing );
			}
		}
	}

	TArray< TWeakObjectPtr< UACPRTerminalComponent > >* bucket = mCells.Find( cell );
	if( !bucket )
	{
		// A new cell: the coarse layer learns it once, here — the only place a cell is created.
		bucket = &mCells.Add( cell );
		mBuckets.Add( ACPRCellPosition( cell ), cell );
	}
	bucket->AddUnique( key );
	mCellOf.Add( key, cell );
}

void UACPRTerminalRegistry::UnregisterTerminal( UACPRTerminalComponent* terminal )
{
	if( !terminal )
	{
		return;
	}

	const TWeakObjectPtr< UACPRTerminalComponent > key( terminal );

	if( const FIntVector* cell = mCellOf.Find( key ) )
	{
		if( TArray< TWeakObjectPtr< UACPRTerminalComponent > >* bucket = mCells.Find( *cell ) )
		{
			bucket->Remove( key );
			if( bucket->Num() == 0 )
			{
				mBuckets.Remove( ACPRCellPosition( *cell ), *cell );
				mCells.Remove( *cell );
			}
		}
		mCellOf.Remove( key );
	}
}

void UACPRTerminalRegistry::AppendCell( const FIntVector& cell, TArray< UACPRTerminalComponent* >& out_terminals ) const
{
	if( const TArray< TWeakObjectPtr< UACPRTerminalComponent > >* bucket = mCells.Find( cell ) )
	{
		for( const TWeakObjectPtr< UACPRTerminalComponent >& weak : *bucket )
		{
			if( UACPRTerminalComponent* terminal = weak.Get() )
			{
				out_terminals.Add( terminal );
			}
		}
	}
}

void UACPRTerminalRegistry::FindInCell( const FIntVector& cell,
                                        const UACPRTerminalComponent* exclude,
                                        const AActor* excludeOwner,
                                        TArray< UACPRTerminalComponent* >& out_terminals ) const
{
	const TArray< TWeakObjectPtr< UACPRTerminalComponent > >* bucket = mCells.Find( cell );
	if( !bucket )
	{
		return;
	}

	for( const TWeakObjectPtr< UACPRTerminalComponent >& weak : *bucket )
	{
		UACPRTerminalComponent* terminal = weak.Get();
		if( !terminal || terminal == exclude )
		{
			continue;
		}

		// A Rail's own two terminals are coincident with nothing, but a one-metre Rail's ends are
		// close enough that a future quantum could put them in the same cell. Excluding the owner
		// makes that impossible to get wrong rather than merely unlikely.
		if( excludeOwner && terminal->GetOwner() == excludeOwner )
		{
			continue;
		}

		out_terminals.Add( terminal );
	}
}

void UACPRTerminalRegistry::FindAlongAxis( const FVector& origin, const FVector& axis,
                                           double minAlong, double maxAlong, double tolerance,
                                           TArray< UACPRTerminalComponent* >& out_terminals ) const
{
	out_terminals.Reset();

	if( axis.IsNearlyZero() || maxAlong <= minAlong )
	{
		return;
	}

	const FVector unit = axis.GetSafeNormal();

	// Only the buckets the segment can touch, then the exact test on each cell key in them.
	mBuckets.ForEachAlongSegment( origin, unit, minAlong, maxAlong, tolerance,
		[ & ]( const TArray< FIntVector >& cells )
		{
			for( const FIntVector& cell : cells )
			{
				// The cell key is the position. QuantizeLocation rounds to 1 uu and world coordinates come
				// straight through it, so the key can be tested directly and no terminal is dereferenced
				// until its cell has already qualified.
				const FVector delta = ACPRCellPosition( cell ) - origin;

				const double along = FVector::DotProduct( delta, unit );
				if( along < minAlong || along > maxAlong )
				{
					continue;
				}

				if( FVector::Dist( delta, unit * along ) > tolerance )
				{
					continue;
				}

				AppendCell( cell, out_terminals );
			}
		} );
}

void UACPRTerminalRegistry::FindNear( const FVector& centre, double radius,
                                      TArray< UACPRTerminalComponent* >& out_terminals ) const
{
	out_terminals.Reset();

	if( radius <= 0.0 )
	{
		return;
	}

	const double radiusSq = radius * radius;

	mBuckets.ForEachNear( centre, radius,
		[ & ]( const TArray< FIntVector >& cells )
		{
			for( const FIntVector& cell : cells )
			{
				// Squared distance on the cell key, before any terminal is dereferenced — the same cheap
				// reject FindAlongAxis uses, for the same reason.
				if( FVector::DistSquared( ACPRCellPosition( cell ), centre ) > radiusSq )
				{
					continue;
				}

				AppendCell( cell, out_terminals );
			}
		} );
}

void UACPRTerminalRegistry::GetCounts( int32& out_terminals, int32& out_cells )
{
	out_terminals = 0;

	// Prune while counting. Dismantled buildables unregister in EndPlay, so this should find
	// nothing — a non-zero prune in the log means something is being destroyed without EndPlay,
	// which is worth knowing before it becomes a stale-candidate bug.
	int32 pruned = 0;

	for( auto it = mCells.CreateIterator(); it; ++it )
	{
		TArray< TWeakObjectPtr< UACPRTerminalComponent > >& bucket = it.Value();

		const int32 before = bucket.Num();
		bucket.RemoveAll( []( const TWeakObjectPtr< UACPRTerminalComponent >& weak )
		{
			return !weak.IsValid();
		} );
		pruned += before - bucket.Num();

		if( bucket.Num() == 0 )
		{
			mBuckets.Remove( ACPRCellPosition( it.Key() ), it.Key() );
			it.RemoveCurrent();
			continue;
		}

		out_terminals += bucket.Num();
	}

	out_cells = mCells.Num();

	if( pruned > 0 )
	{
		for( auto it = mCellOf.CreateIterator(); it; ++it )
		{
			if( !it.Key().IsValid() )
			{
				it.RemoveCurrent();
			}
		}

		ACPR_LOG( Warning, TERM,
			TEXT( "registry pruned %d dead terminal(s) — something was destroyed "
			      "without EndPlay unregistering it." ),
			pruned );
	}
}
