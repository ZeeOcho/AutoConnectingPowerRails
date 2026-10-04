// Auto-Connecting Power Rails — `acpr.TraceAim`, the instrument for "what is the build gun
// actually hitting".
//
// Why it exists: when a buildable cannot be aimed at, geometric reasoning about why is no substitute
// for a measurement of the thing that decides — which component the trace returns first. This
// command is that measurement, for the aim path.
//
// How to use it: stand where the target is unaimable and aim at it. Open the console — the camera
// does not move while the console is open, so the shot is still lined up — and type:
//
//     acpr.TraceAim
//
// It traces from the player view point and logs every hit in order, for a line trace against simple
// shapes, a line trace against complex (render triangles), and sphere sweeps at three radii. The
// first line of each block is what an aim of that kind would return.
//
// How to read it, with a Rail Cap as the example:
//
//   * `Build_PowerRailCap_C … CapMesh` first everywhere  -> the trace is fine and the fault is
//     downstream of it, in whatever decides a dismantle target. Geometry is not the problem.
//   * `Build_PowerRail_C … TerminalMeshA` first on the sweeps but not the line traces -> the
//     collar's end ring wins a swept aim; RAIL_TERMINAL_SETBACK is the knob.
//   * `Build_PowerRail_C … TerminalMeshA` first on the simple line trace only -> the collar's
//     collision hull is the occluder and COLLISION_TRACE_FLAG is not taking on that asset.
//   * nothing at all on a channel -> `BuildingMesh` does not respond to that channel, which is
//     itself worth knowing and is why two channels are traced rather than one.
//
// Distances are along the ray from the view point, so the order and the gaps are the measurement:
// a Cap 1 cm in front of the collar shows a ~1 cm gap between the two first hits.

#include "AutoConnectingPowerRails.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

namespace
{
	/** One line per hit, in the order the query returned them, with the component named. */
	void LogHits( const TCHAR* label, const TArray< FHitResult >& hits, const FVector& from )
	{
		if( hits.Num() == 0 )
		{
			ACPR_LOG( Log, AIM, TEXT( "  %-28s | NO HITS" ), label );
			return;
		}

		for( int32 i = 0; i < hits.Num(); ++i )
		{
			const FHitResult& hit = hits[ i ];
			const AActor* const actor = hit.GetActor();
			const UPrimitiveComponent* const comp = hit.GetComponent();

			ACPR_LOG( Log, AIM,
				TEXT( "  %-28s | #%d dist=%7.2f | actor=%s | component=%s | "
				      "profile=%s | impact=%s" ),
				( i == 0 ) ? label : TEXT( "" ),
				i,
				FVector::Dist( from, hit.ImpactPoint ),
				actor ? *actor->GetName() : TEXT( "<null>" ),
				comp ? *comp->GetName() : TEXT( "<null>" ),
				comp ? *comp->GetCollisionProfileName().ToString() : TEXT( "-" ),
				*hit.ImpactPoint.ToString() );
		}
	}

	void TraceAim( UWorld* world )
	{
		if( !world )
		{
			ACPR_LOG( Warning, AIM, TEXT( "no world." ) );
			return;
		}

		APlayerController* const pc = world->GetFirstPlayerController();
		if( !pc )
		{
			ACPR_LOG( Warning, AIM, TEXT( "no local player controller." ) );
			return;
		}

		// GetPlayerViewPoint is the same view point the build gun's own trace starts from, and it
		// does not move while the console is open — which is what makes this usable at all.
		FVector from = FVector::ZeroVector;
		FRotator look = FRotator::ZeroRotator;
		pc->GetPlayerViewPoint( from, look );

		const double reach = 2000.0;
		const FVector to = from + look.Vector() * reach;

		ACPR_LOG( Log, AIM,
			TEXT( "==== from=%s look=%s reach=%.0f ====" ),
			*from.ToString(), *look.ToString(), reach );

		// The player is ignored so their own capsule cannot be hit #0 on every line — and everything
		// attached to the player: the build gun in the player's hand carries a dangling trinket
		// (`BP_BuildGun_C … TrinketChain | profile=Ragdoll`, about 38 uu out) that a 15 cm sphere
		// leaving the camera cannot miss, so without this the wider sweeps measure the player's own
		// equipment.
		TArray< AActor* > ignored;
		if( APawn* const pawn = pc->GetPawn() )
		{
			ignored.Add( pawn );
			pawn->GetAttachedActors( ignored, /*bResetArray*/ false, /*bRecursivelyIncludeAttachedActors*/ true );
		}

		FCollisionQueryParams simple( FName( TEXT( "ACPRTraceAim" ) ), /*bTraceComplex*/ false );
		simple.AddIgnoredActors( ignored );

		FCollisionQueryParams complex( FName( TEXT( "ACPRTraceAim" ) ), /*bTraceComplex*/ true );
		complex.AddIgnoredActors( ignored );

		ACPR_LOG( Log, AIM, TEXT( "ignoring %d actor(s): the pawn and what is attached to it" ),
			ignored.Num() );

		// Two channels, because which one Satisfactory's build gun uses is not something this
		// module can read, and a channel that returns nothing is a finding rather than a failure.
		const ECollisionChannel channels[] = { ECC_Visibility, ECC_Camera };
		const TCHAR* channelNames[] = { TEXT( "Visibility" ), TEXT( "Camera" ) };

		for( int32 c = 0; c < UE_ARRAY_COUNT( channels ); ++c )
		{
			TArray< FHitResult > hits;

			world->LineTraceMultiByChannel( hits, from, to, channels[ c ], simple );
			LogHits( *FString::Printf( TEXT( "%s line/simple" ), channelNames[ c ] ), hits, from );

			hits.Reset();
			world->LineTraceMultiByChannel( hits, from, to, channels[ c ], complex );
			LogHits( *FString::Printf( TEXT( "%s line/complex" ), channelNames[ c ] ), hits, from );

			// Sweeps cannot use a triangle mesh as the sweeping shape, but they do hit one — and a
			// sweep takes the frontmost surface anywhere in its disc, which is how a neighbour's edge
			// can win over the surface under the crosshair. Three radii, so the radius at which the
			// target stops winning is visible rather than inferred.
			const float radii[] = { 5.0f, 15.0f, 25.0f };
			for( int32 r = 0; r < UE_ARRAY_COUNT( radii ); ++r )
			{
				hits.Reset();
				world->SweepMultiByChannel( hits, from, to, FQuat::Identity, channels[ c ],
					FCollisionShape::MakeSphere( radii[ r ] ), simple );
				LogHits( *FString::Printf( TEXT( "%s sweep r=%.0f" ), channelNames[ c ],
					radii[ r ] ), hits, from );
			}
		}

		ACPR_LOG( Log, AIM, TEXT( "==== end ====" ) );
	}
}

static FAutoConsoleCommandWithWorld GACPRTraceAim(
	TEXT( "acpr.TraceAim" ),
	TEXT( "Traces from the player view point and logs every hit, for line/simple, line/complex and "
	      "three sphere sweeps, on the Visibility and Camera channels. Aim first, then run it." ),
	FConsoleCommandWithWorldDelegate::CreateStatic( &TraceAim ) );
