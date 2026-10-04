// Auto-Connecting Power Rails — the one power connection a host owns.
//
// One connection per host. A connection per terminal — a Rail carrying six
// UFGPowerConnectionComponents (two terminals and a pool of four Hoverpack nodes), a Junction seven
// (six faces and a centre node), all hidden, all linked to each other by internal hidden edges —
// would have the circuit subsystem see six members and five edges per Rail and rebuild over all of
// them on every change, with every hologram duplicating the lot. So the terminals are plain scene
// components (UACPRTerminalComponent: position, outward, state, partner) and the ELECTRICAL side of
// a host is this single component, at the host's electrical centre. A Coupling between two hosts is
// one hidden edge between their two connections; nothing internal is linked, because there is
// nothing internal to link.
//
// Why a subclass. UFGCircuitConnectionComponent keeps mMaxNumConnectionLinks and mIsHiddenConnection
// protected (FGCircuitConnectionComponent.h:162-171), so only a subclass can say "hidden, no wires"
// (a Rail or a Junction) or "visible, four wires" (the Outlet's socket, §8 / invariant 7). And it is
// a UFGPowerConnectionComponent, not the circuit base: mCircuitType = UFGPowerCircuit is a promise
// about the class, because UFGPowerCircuit::OnCircuitChanged casts every member to
// UFGPowerConnectionComponent on the next subsystem tick (FGCircuit.h:74, FGPowerCircuit.h:271).
//
// The Hoverpack finds a power connection by searching within its radius on a timer
// (FGHoverPack.h:238, :242) and keeps one (`mCurrentPowerConnection`, :273). One node at the
// midpoint of a ≤ 40 m Rail gives √(62² − 20²) = 58.7 m of guaranteed reach at the worst point of
// any chain — 94.7 % of the 62 m radius — which is what §5.4's default spacing of 40 m gives.
//
// Every connection component is a CDO subobject: the Hoverpack cannot see a runtime-created
// connection, and a saved Power Line cannot find one.

#pragma once

#include "CoreMinimal.h"
#include "FGPowerConnectionComponent.h"
#include "ACPRPowerConnectionComponent.generated.h"

UCLASS( ClassGroup = ( ACPR ), meta = ( BlueprintSpawnableComponent ) )
class AUTOCONNECTINGPOWERRAILS_API UACPRPowerConnectionComponent : public UFGPowerConnectionComponent
{
	GENERATED_BODY()

public:
	UACPRPowerConnectionComponent();

	/**
	 * Hidden with no wire budget (a Rail's or a Junction's), or visible with a budget of `maxWires`
	 * Power Lines (the Outlet's socket). Hidden is also what AddHiddenConnection requires of at least
	 * one side ("One of the connections must be hidden for this to be valid",
	 * FGCircuitConnectionComponent.h:91-94); a Coupling is always hidden-to-hidden or socket-to-hidden.
	 */
	void ConfigureAsWireSocket( int32 maxWires );

	/**
	 * §10.6's link icon: makes a PREVIEW COPY harmless. AFGBlueprintHologram::SetupBuildableComponent
	 * copies a live component as an archetype, so the copy arrives believing it is in the original's
	 * circuit, holding the original's wire and hidden-edge arrays. It exists only for vanilla to hang the
	 * blue two-link icon on and dies with the hologram. Those four fields are private on the base;
	 * Config/AccessTransformers.ini makes this class its friend.
	 */
	void MakePreviewCopyInert();

	/** One line for the log: circuit, power, hidden edges, wires, budget. */
	FString Describe() const;
};
