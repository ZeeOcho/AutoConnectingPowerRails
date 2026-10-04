// Auto-Connecting Power Rails — what the four holograms do identically, written once.
//
// The Rail, Junction, Cap and Outlet holograms inherit from two different vanilla classes
// (AFGBeamHologram, AFGBuildableHologram), so they cannot share a base of ours without
// re-deriving vanilla's; what they share instead is a value (the last aim the validity hook saw) and
// two rules, which live here as functions the thin overrides call. The vanilla virtuals they need —
// IsValidHitActor, GetMinPlacementFloorZ — are protected, so each override evaluates those itself and
// hands the answer in.

#pragma once

#include "CoreMinimal.h"
#include "ACPRLog.h"
#include "Engine/HitResult.h"
#include "GameFramework/Actor.h"

/**
 * The last aim IsValidHitResult saw. Recorded whatever the verdict, because that hook is the last one
 * to see the hit before a rejected surface stops producing any other line — and CheckValidFloor, which
 * vanilla calls without the hit, reads the normal from here.
 */
struct FACPRAimRecord
{
	FVector Normal = FVector::ZeroVector;
	FString ActorName;
	FHitResult Hit;

	void Record( const FHitResult& hit )
	{
		Normal = hit.ImpactNormal;
		ActorName = hit.GetActor() ? hit.GetActor()->GetName() : FString( TEXT( "<none>" ) );
		Hit = hit;
	}
};

namespace ACPRHologram
{
	/**
	 * Vanilla's IsValidHitResult returns 0 for our own buildables, which would hide the hologram before
	 * TrySnapToActor is ever reached. The widening is vanilla's own list (AFGHologram::mValidHitClasses,
	 * filled by AddValidHitClass in BeginPlay); this verdict only keeps the base's own IsValidHitResult
	 * — whose body cannot be read — from bypassing that list. `listedByClass` is the caller's
	 * IsValidHitActor( actor ), evaluated only for a non-null actor. 1 = valid: the header's "@return
	 * true if invalid" is contradicted by what the engine does.
	 */
	inline bool WidenedHitVerdict( bool superSaid, const FHitResult& hit, bool listedByClass,
	                               FACPRLogGate& log, const TCHAR* tag )
	{
		const AActor* hitActor = hit.GetActor();
		const bool listed = !superSaid && hit.bBlockingHit && hitActor && listedByClass;

		// One line per ( surface, verdict ) pair; the key is built only while it can be printed.
		if( log.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
		{
			const FString key = FString::Printf( TEXT( "%s|%d%d" ),
				hitActor ? *hitActor->GetName() : TEXT( "-" ), superSaid ? 1 : 0, listed ? 1 : 0 );
			if( log.Admit( key ) )
			{
				// listed=1 with super=0 is the case the override exists for.
				ACPR_LOG_TAGGED( Verbose, tag,
					TEXT( "IsValidHitResult | actor=%s (%s) component=%s | normal=%s | super=%d listed=%d -> %d" ),
					hitActor ? *hitActor->GetName() : TEXT( "<none>" ),
					hitActor ? *hitActor->GetClass()->GetName() : TEXT( "-" ),
					hit.GetComponent() ? *hit.GetComponent()->GetName() : TEXT( "-" ),
					*hit.ImpactNormal.ToString(),
					superSaid ? 1 : 0, listed ? 1 : 0, ( superSaid || listed ) ? 1 : 0 );
			}
		}

		return superSaid || listed;
	}

	/**
	 * Only the angle rule is relaxed, not the whole check. Vanilla's CheckValidFloor refuses a floor that
	 * is "too steep, another building etc." by adding a disqualifier, so skipping it outright drops every
	 * rule it holds. It is skipped only where its own angle test would have refused — the steep surfaces
	 * §7.1's free positioning is about — and where the hologram is attached to a host (§7.2's mate,
	 * §7.3's insertion; there is no floor to ask about). Raising the angle to 90 instead is not an
	 * option because cos(90°) is about -4.4e-8, a float coin toss on exactly the surface in question.
	 * Returns whether Super::CheckValidFloor should run; logs the verdict.
	 */
	inline bool FloorRuleAdmitsSuper( float minZ, const FACPRAimRecord& aim, bool attached,
	                                  FACPRLogGate& log, const TCHAR* tag )
	{
		const bool vanillaWouldReject = aim.Normal.Z < minZ;
		const bool admit = !attached && !vanillaWouldReject;

		if( log.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) )
		{
			const FString key = FString::Printf( TEXT( "%s|%d|%d" ), *aim.ActorName, vanillaWouldReject ? 1 : 0, attached ? 1 : 0 );
			if( log.Admit( key ) )
			{
				// A wall that does not place while this prints SKIPPED was refused somewhere else —
				// IsValidHitResult is the next place to look.
				ACPR_LOG_TAGGED( Verbose, tag,
					TEXT( "CheckValidFloor %s | actor=%s normalZ=%.3f | minZ=%.3f" ),
					attached ? TEXT( "SKIPPED (attached to a host — no floor)" )
					         : ( vanillaWouldReject ? TEXT( "SKIPPED (too steep for vanilla)" ) : TEXT( "-> Super" ) ),
					*aim.ActorName, aim.Normal.Z, minZ );
			}
		}

		return admit;
	}
}
