// Auto-Connecting Power Rails — a coarse uniform grid over points, for "what is near this ray" and
// "what is near this point" in time proportional to the QUESTION rather than to the WORLD.
//
// The registry's FindAlongAxis and FindNear, and the blueprint manager's occluder pass, would
// otherwise be linear scans: every occupied position in the world, one dot product each, for every
// ray cast. A large blueprint asks hundreds of rays per evaluation of a world with thousands of
// occluders — a million disc tests per frame while aiming. A factory keeps growing; a Rail does
// not. This index makes a ray cost O(its length × the local density) and never more than a linear
// scan (see the hybrid rule below).
//
// Why a grid and not the physics scene. UACPRTerminalRegistry::FindAlongAxis says why the scene
// cannot answer this question at all (a trace stops at the first blocking hit; a Rail is allowed
// through walls). And the manager's occluders are not in any scene: they are the blueprint's own
// terminals, in a world nobody can trace.
//
// Why a template and not a UObject. It holds plain values (a cell key, an array index), belongs to
// exactly one owner, and needs no lifetime of its own. The owner decides what an item is and what
// the exact test on it is; the grid only says which buckets a query touches.
//
// The hybrid rule. Every query first counts the buckets it would enumerate. When that is no fewer
// than the buckets that exist, it walks the existing ones with the same geometric reject instead
// — so a small world, or a query as wide as the world (FindNear at one Rail length), costs what a
// linear scan costs and never more. The index only ever helps.

#pragma once

#include "CoreMinimal.h"

/**
 * The bucket size is a template parameter (whole uu) rather than a constructor argument: a UCLASS
 * member has to be default-constructible for UHT's generated constructors, and the size is a
 * property of the index's type, not of any one instance.
 */
template< typename T, int32 BucketSizeUU >
class TACPRSpatialGrid
{
	static_assert( BucketSizeUU > 0, "a bucket needs a size" );

public:
	static constexpr double mBucketSize = static_cast< double >( BucketSizeUU );

	double GetBucketSize() const { return mBucketSize; }
	int32 GetBucketCount() const { return mBuckets.Num(); }

	/** Buckets the last query enumerated or walked, for the log. */
	int32 GetLastVisited() const { return mLastVisited; }

	FIntVector KeyOf( const FVector& position ) const
	{
		return FIntVector(
			FMath::FloorToInt32( position.X / mBucketSize ),
			FMath::FloorToInt32( position.Y / mBucketSize ),
			FMath::FloorToInt32( position.Z / mBucketSize ) );
	}

	void Add( const FVector& position, const T& item )
	{
		mBuckets.FindOrAdd( KeyOf( position ) ).AddUnique( item );
	}

	/** True if the item was filed under that position. */
	bool Remove( const FVector& position, const T& item )
	{
		const FIntVector key = KeyOf( position );
		TArray< T >* bucket = mBuckets.Find( key );
		if( !bucket )
		{
			return false;
		}
		const bool removed = bucket->RemoveSingle( item ) > 0;
		if( bucket->Num() == 0 )
		{
			mBuckets.Remove( key );
		}
		return removed;
	}

	void Reset()
	{
		mBuckets.Reset();
	}

	/**
	 * Every bucket that can hold a point within `tolerance` of the segment origin + unit·[minAlong,
	 * maxAlong]. `visit( const TArray< T >& )` is called once per bucket; the caller applies its own
	 * exact test to the items — the grid over-approximates and never under-approximates.
	 *
	 * How: slab by slab along the segment's dominant axis. Each slab is one bucket thick,
	 * grown by the tolerance; the segment is clipped to it, and the lateral extent of that clipped
	 * piece, grown by the tolerance, gives the bucket ranges on the other two axes. Every bucket that
	 * a point within tolerance of the segment can lie in is in some slab's lateral box (the box is the
	 * bounding box of the tolerance-grown piece), and slabs are disjoint along the axis, so no bucket
	 * is visited twice and no set is needed. An axis-aligned segment — the common case — touches one
	 * bucket per slab unless it runs within tolerance of a lateral bucket face, then two or four.
	 */
	template< typename F >
	void ForEachAlongSegment( const FVector& origin, const FVector& unit,
	                          double minAlong, double maxAlong, double tolerance, F&& visit ) const
	{
		mLastVisited = 0;
		if( mBuckets.Num() == 0 || maxAlong < minAlong )
		{
			return;
		}

		// The dominant axis: |unit| is 1, so its largest component is at least 1/sqrt(3).
		int32 a = 0;
		for( int32 axis = 1; axis < 3; ++axis )
		{
			if( FMath::Abs( unit[ axis ] ) > FMath::Abs( unit[ a ] ) )
			{
				a = axis;
			}
		}
		const int32 b1 = ( a + 1 ) % 3;
		const int32 b2 = ( a + 2 ) % 3;
		const double ua = unit[ a ];
		if( FMath::Abs( ua ) < UE_DOUBLE_SMALL_NUMBER )
		{
			return;  // A zero direction is the caller's bug; nothing lies along it.
		}

		const FVector p0 = origin + unit * minAlong;
		const FVector p1 = origin + unit * maxAlong;
		const int32 slabLo = FMath::FloorToInt32( ( FMath::Min( p0[ a ], p1[ a ] ) - tolerance ) / mBucketSize );
		const int32 slabHi = FMath::FloorToInt32( ( FMath::Max( p0[ a ], p1[ a ] ) + tolerance ) / mBucketSize );

		// The hybrid rule. Per slab the lateral box is 2·tolerance plus the segment's lateral travel
		// across one slab, on each of two axes; estimated in buckets, rounded up, plus the straddle.
		const double travel1 = FMath::Abs( unit[ b1 ] / ua ) * mBucketSize;
		const double travel2 = FMath::Abs( unit[ b2 ] / ua ) * mBucketSize;
		const int64 lateral = static_cast< int64 >( ( 2.0 * tolerance + travel1 ) / mBucketSize + 2.0 )
		                    * static_cast< int64 >( ( 2.0 * tolerance + travel2 ) / mBucketSize + 2.0 );
		if( static_cast< int64 >( slabHi - slabLo + 1 ) * lateral >= mBuckets.Num() )
		{
			WalkAll( [ & ]( const FIntVector& key, const TArray< T >& bucket )
			{
				if( SegmentTouchesBucket( key, origin, unit, minAlong, maxAlong, tolerance ) )
				{
					visit( bucket );
				}
			} );
			return;
		}

		for( int32 slab = slabLo; slab <= slabHi; ++slab )
		{
			// The slab, grown by the tolerance, as a parameter range along the segment.
			const double edge0 = slab * mBucketSize - tolerance;
			const double edge1 = ( slab + 1 ) * mBucketSize + tolerance;
			double t0 = ( edge0 - origin[ a ] ) / ua;
			double t1 = ( edge1 - origin[ a ] ) / ua;
			if( t0 > t1 )
			{
				Swap( t0, t1 );
			}
			t0 = FMath::Max( t0, minAlong );
			t1 = FMath::Min( t1, maxAlong );
			if( t0 > t1 )
			{
				continue;
			}

			const FVector q0 = origin + unit * t0;
			const FVector q1 = origin + unit * t1;
			const int32 lo1 = FMath::FloorToInt32( ( FMath::Min( q0[ b1 ], q1[ b1 ] ) - tolerance ) / mBucketSize );
			const int32 hi1 = FMath::FloorToInt32( ( FMath::Max( q0[ b1 ], q1[ b1 ] ) + tolerance ) / mBucketSize );
			const int32 lo2 = FMath::FloorToInt32( ( FMath::Min( q0[ b2 ], q1[ b2 ] ) - tolerance ) / mBucketSize );
			const int32 hi2 = FMath::FloorToInt32( ( FMath::Max( q0[ b2 ], q1[ b2 ] ) + tolerance ) / mBucketSize );

			FIntVector key;
			key[ a ] = slab;
			for( int32 i1 = lo1; i1 <= hi1; ++i1 )
			{
				key[ b1 ] = i1;
				for( int32 i2 = lo2; i2 <= hi2; ++i2 )
				{
					key[ b2 ] = i2;
					if( const TArray< T >* bucket = mBuckets.Find( key ) )
					{
						++mLastVisited;
						visit( *bucket );
					}
				}
			}
		}
	}

	/** Every bucket that intersects the sphere (centre, radius). Same contract as ForEachAlongSegment. */
	template< typename F >
	void ForEachNear( const FVector& centre, double radius, F&& visit ) const
	{
		mLastVisited = 0;
		if( mBuckets.Num() == 0 || radius <= 0.0 )
		{
			return;
		}

		const FIntVector lo = KeyOf( centre - FVector( radius ) );
		const FIntVector hi = KeyOf( centre + FVector( radius ) );
		const int64 count = static_cast< int64 >( hi.X - lo.X + 1 ) * ( hi.Y - lo.Y + 1 ) * ( hi.Z - lo.Z + 1 );

		if( count >= mBuckets.Num() )
		{
			WalkAll( [ & ]( const FIntVector& key, const TArray< T >& bucket )
			{
				if( SphereTouchesBucket( key, centre, radius ) )
				{
					visit( bucket );
				}
			} );
			return;
		}

		for( int32 x = lo.X; x <= hi.X; ++x )
		{
			for( int32 y = lo.Y; y <= hi.Y; ++y )
			{
				for( int32 z = lo.Z; z <= hi.Z; ++z )
				{
					const FIntVector key( x, y, z );
					if( !SphereTouchesBucket( key, centre, radius ) )
					{
						continue;
					}
					if( const TArray< T >* bucket = mBuckets.Find( key ) )
					{
						++mLastVisited;
						visit( *bucket );
					}
				}
			}
		}
	}

private:
	template< typename F >
	void WalkAll( F&& visit ) const
	{
		for( const TPair< FIntVector, TArray< T > >& pair : mBuckets )
		{
			++mLastVisited;
			visit( pair.Key, pair.Value );
		}
	}

	void BoundsOf( const FIntVector& key, FVector& out_min, FVector& out_max ) const
	{
		out_min = FVector( key.X, key.Y, key.Z ) * mBucketSize;
		out_max = out_min + FVector( mBucketSize );
	}

	/** Distance from a point to the bucket's box is at most `radius`. */
	bool SphereTouchesBucket( const FIntVector& key, const FVector& centre, double radius ) const
	{
		FVector lo, hi;
		BoundsOf( key, lo, hi );
		const FVector nearest = ClampVector( centre, lo, hi );
		return FVector::DistSquared( nearest, centre ) <= radius * radius;
	}

	/**
	 * The segment passes within `tolerance` of the bucket's box. The box is grown by the tolerance and
	 * the segment is clipped against it slab by slab — exact for the grown box, conservative for the
	 * rounded one it stands in for.
	 */
	bool SegmentTouchesBucket( const FIntVector& key, const FVector& origin, const FVector& unit,
	                           double minAlong, double maxAlong, double tolerance ) const
	{
		FVector lo, hi;
		BoundsOf( key, lo, hi );
		lo -= FVector( tolerance );
		hi += FVector( tolerance );

		double tEnter = minAlong;
		double tExit = maxAlong;
		for( int32 axis = 0; axis < 3; ++axis )
		{
			const double o = origin[ axis ];
			const double d = unit[ axis ];
			if( FMath::Abs( d ) < UE_DOUBLE_SMALL_NUMBER )
			{
				if( o < lo[ axis ] || o > hi[ axis ] )
				{
					return false;
				}
				continue;
			}
			double t0 = ( lo[ axis ] - o ) / d;
			double t1 = ( hi[ axis ] - o ) / d;
			if( t0 > t1 )
			{
				Swap( t0, t1 );
			}
			tEnter = FMath::Max( tEnter, t0 );
			tExit = FMath::Min( tExit, t1 );
			if( tEnter > tExit )
			{
				return false;
			}
		}
		return true;
	}

	TMap< FIntVector, TArray< T > > mBuckets;
	mutable int32 mLastVisited = 0;
};
