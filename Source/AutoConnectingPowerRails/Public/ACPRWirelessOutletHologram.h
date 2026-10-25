// Auto-Connecting Power Rails — the Power Rail Wireless Outlet hologram.
//
// §16.1: the placement is the Outlet's, so this is AACPROutletHologram with two additions, both
// §16.5's readouts while placing:
//
//   the range, as the tier's translucent sphere about the link origin — the same sphere the built
//   outlet shows when aimed at (AACPRWirelessOutlet::MakeRangeSphere), on the hologram;
//   the links, as a preview: the lines this outlet would make the moment it is built, powered for
//   every machine it would take, cut short at the obstacle for every one it is blocked from
//   (UACPRWirelessPreviewComponent, in its outlet mode).
//
// Both are runtime components on the hologram, made the way the blueprint manager makes its link
// icons (FACPRBlueprintTerminalManager::EnsureIcon): outer = the hologram, attached, registered, no
// collision, no shadow. Neither is part of the buildable, so nothing copies them to the built actor,
// and they die with the hologram.

#pragma once

#include "CoreMinimal.h"
#include "ACPROutletHologram.h"
#include "ACPRWirelessOutletHologram.generated.h"

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API AACPRWirelessOutletHologram : public AACPROutletHologram
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;

private:
	TWeakObjectPtr< class UStaticMeshComponent > mRangeSphere;
	TWeakObjectPtr< class UACPRWirelessPreviewComponent > mPreview;
};
