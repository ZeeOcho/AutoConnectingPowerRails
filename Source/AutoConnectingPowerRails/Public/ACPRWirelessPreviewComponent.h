// Auto-Connecting Power Rails — §16.5's placement preview.
//
// While a hologram is held, the player sees what §16.3 would decide the moment it is built:
//
//   placing a MACHINE   every Wireless Outlet whose range reaches a power connection of the hologram
//                       shows its range sphere; the outlet the machine would link to draws a powered
//                       line to that connection; every other outlet in range draws a faint line —
//                       the whole line when its path is clear (the outlet the machine would fall
//                       back to), cut short at the obstacle when it is not (what blocks it);
//   placing an OUTLET   the hologram draws its own range sphere (AACPRWirelessOutletHologram) and,
//                       through this component, a powered line to every machine connection it would
//                       take, and a faint line cut short at the obstacle to every one it is blocked
//                       from. Machines wired, or kept by a nearer outlet, draw nothing: the outlet
//                       would not touch them.
//
// The component lives on the hologram and dies with it. It is the one per-frame consumer of the
// link subsystem, gated twice (invariant 17): it exists only while a local hologram does, and it
// re-asks only when the hologram has moved or a link event has happened since its last answer
// (UACPRWirelessLinkSubsystem::GetWorldVersion). Each answer is the subsystem's bounded lookup.
//
// Everything drawn is local to the player holding the hologram — the lines are two instanced mesh
// components of this component's own, the spheres are the outlets' (SetRangeShownBy, keyed by this
// component so several previews and the dismantle aim never fight over one sphere).

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ACPRWirelessPreviewComponent.generated.h"

class AACPRWirelessOutlet;
class AFGBuildableHologram;
class UInstancedStaticMeshComponent;

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API UACPRWirelessPreviewComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UACPRWirelessPreviewComponent();

	/**
	 * Attaches a preview to a machine's hologram when one is due: the hologram is the local
	 * player's and builds an AFGBuildableFactory. Called from the module's hook after
	 * AFGBuildableHologram::BeginPlay. Returns the component, or null when none is due.
	 */
	static UACPRWirelessPreviewComponent* AttachToMachineHologram( AFGBuildableHologram* hologram );

	/** Attaches the outlet-mode preview to a Wireless Outlet's hologram (its own BeginPlay). */
	static UACPRWirelessPreviewComponent* AttachToOutletHologram( AFGBuildableHologram* hologram );

	virtual void TickComponent( float deltaTime, ELevelTick tickType, FActorComponentTickFunction* thisTickFunction ) override;
	virtual void EndPlay( const EEndPlayReason::Type endPlayReason ) override;

private:
	enum class EMode : uint8 { Machine, Outlet };

	static UACPRWirelessPreviewComponent* Attach( AFGBuildableHologram* hologram, EMode mode );

	/** The hologram's build class default, which carries the mesh, the materials and (in outlet mode) the tier. */
	const AACPRWirelessOutlet* Rules() const;

	/** Makes the two line components on first use; false when the mesh or a material is missing. */
	bool EnsureLines();

	/** Re-asks the subsystem and redraws. */
	void Refresh();
	void RefreshMachine( class UACPRWirelessLinkSubsystem* links, AFGBuildableHologram* hologram );
	void RefreshOutlet( class UACPRWirelessLinkSubsystem* links, AFGBuildableHologram* hologram );

	void DrawLine( const FVector& from, const FVector& to, bool powered );
	void ShowSpheres( const TSet< TWeakObjectPtr< AACPRWirelessOutlet > >& outlets );
	void Clear();

	EMode mMode = EMode::Machine;
	TWeakObjectPtr< const AACPRWirelessOutlet > mRules;

	UPROPERTY()
	TObjectPtr< UInstancedStaticMeshComponent > mPoweredLines = nullptr;
	UPROPERTY()
	TObjectPtr< UInstancedStaticMeshComponent > mFaintLines = nullptr;
	bool mLinesMissing = false;

	/** The outlets whose spheres this preview is showing, to take back when they leave the set. */
	TSet< TWeakObjectPtr< AACPRWirelessOutlet > > mShowing;

	/** What the last answer was computed for. */
	FTransform mLastTransform;
	uint32 mLastWorldVersion = 0;
	bool mAnswered = false;
};
