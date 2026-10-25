// Auto-Connecting Power Rails — the Power Rail Wireless Outlet hologram.

#include "ACPRWirelessOutletHologram.h"

#include "ACPRWirelessLinkSubsystem.h"
#include "ACPRWirelessOutlet.h"
#include "ACPRWirelessPreviewComponent.h"
#include "AutoConnectingPowerRails.h"
#include "Components/StaticMeshComponent.h"

void AACPRWirelessOutletHologram::BeginPlay()
{
	Super::BeginPlay();

	const AACPRWirelessOutlet* cdo = GetBuildClass()
		? Cast< AACPRWirelessOutlet >( GetBuildClass()->GetDefaultObject() ) : nullptr;
	if( !cdo )
	{
		ACPR_LOG( Warning, WIRELESS_HOLO,
			TEXT( "%s: its build class is not an AACPRWirelessOutlet — no range sphere, no preview." ), *GetName() );
		return;
	}

	// §16.6: the tier's range (or the console override), about the outlet's link origin — which is
	// mLinkOriginHeight along the mounting normal, the same point the built outlet measures from.
	mRangeSphere = AACPRWirelessOutlet::MakeRangeSphere( this, GetRootComponent(), cdo,
	                                                     FVector( cdo->mLinkOriginHeight, 0.0, 0.0 ),
	                                                     UACPRWirelessLinkSubsystem::EffectiveRange( cdo ) );
	// Not a hologram mesh: it keeps its own translucent material rather than the valid/invalid tint.
	if( UStaticMeshComponent* sphere = mRangeSphere.Get() )
	{
		sphere->SetRenderCustomDepth( false );
	}

	mPreview = UACPRWirelessPreviewComponent::AttachToOutletHologram( this );
}
