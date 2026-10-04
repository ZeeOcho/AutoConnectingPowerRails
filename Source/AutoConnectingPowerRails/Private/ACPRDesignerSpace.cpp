// Auto-Connecting Power Rails — which build space a buildable or a placement belongs to.

#include "ACPRDesignerSpace.h"

#include "ACPRTerminalHost.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "GameFramework/Actor.h"

AFGBuildableBlueprintDesigner* FACPRSpace::OfActor( const AActor* actor )
{
	if( !IsValid( actor ) )
	{
		return nullptr;
	}

	// Cast< IACPRTerminalHost > on a const actor: the interface cast only needs the UClass, and the
	// accessor it reaches is const, so the const_cast is the whole cost of keeping this signature const.
	const IACPRTerminalHost* host = Cast< IACPRTerminalHost >( const_cast< AActor* >( actor ) );
	return host ? host->GetHostDesigner() : nullptr;
}

FString FACPRSpace::Describe( const AFGBuildableBlueprintDesigner* designer )
{
	return designer ? designer->GetName() : FString( TEXT( "world" ) );
}

bool FACPRSpace::IsForeignTarget( const AActor* hologram, const TCHAR* role, const AActor* targetHost,
                                  const AFGBuildableBlueprintDesigner* placementSpace, FString& lastLogKey )
{
	if( !IsValid( targetHost ) )
	{
		return false;
	}

	const AFGBuildableBlueprintDesigner* targetSpace = OfActor( targetHost );
	if( targetSpace == placementSpace )
	{
		return false;
	}

	// Keyed on the pair of spaces and the target, so a hologram held on one refused target prints once,
	// and moving to a different refused target prints again.
	const FString key = FString::Printf( TEXT( "%s|%s|%s|%s" ),
		role, *targetHost->GetName(), *Describe( placementSpace ), *Describe( targetSpace ) );

	if( key != lastLogKey )
	{
		lastLogKey = key;

		ACPR_LOG( Display, SPACE,
			TEXT( "%s REFUSED (10.1) | %s=%s is in %s, the placement is in %s | "
			      "disqualifier=DesignerWorldCommingling" ),
			hologram ? *hologram->GetName() : TEXT( "?" ),
			role, *targetHost->GetName(),
			*Describe( targetSpace ), *Describe( placementSpace ) );
	}

	return true;
}

void FACPRSpace::WatchPlacementSpace( const AActor* hologram,
                                      const AFGBuildableBlueprintDesigner* placementSpace,
                                      TWeakObjectPtr< AFGBuildableBlueprintDesigner >& lastSpace,
                                      bool& seenOnce )
{
	if( seenOnce && lastSpace.Get() == placementSpace )
	{
		return;
	}

	const FString before = seenOnce ? Describe( lastSpace.Get() ) : FString( TEXT( "<first frame>" ) );

	seenOnce = true;
	lastSpace = const_cast< AFGBuildableBlueprintDesigner* >( placementSpace );

	ACPR_LOG( Display, SPACE,
		TEXT( "%s placement space %s -> %s | at %s" ),
		hologram ? *hologram->GetName() : TEXT( "?" ),
		*before, *Describe( placementSpace ),
		hologram ? *hologram->GetActorLocation().ToString() : TEXT( "?" ) );
}
