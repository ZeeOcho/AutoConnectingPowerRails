// Auto-Connecting Power Rails — the terminal.
//
// §4: "Every terminal is open, coupled or capped, never two at once. Only an open terminal accepts
// anything." This is the one component that carries that state, for every buildable in the mod.
// B.3 requires exactly that: "Use one shared terminal component for position, outward axis,
// occlusion radius, state, occupant identity, saved Coupling, candidate registration and
// diagnostics."
//
// ==========================================================================================
// A terminal is a place, not a circuit member.
// ==========================================================================================
//
// Deriving this class from UFGPowerConnectionComponent, so that a Coupling could be a hidden circuit
// edge between the two terminals themselves, would make every terminal a circuit member: a Rail
// would put six of them into the subsystem (two ends and a pool of four Hoverpack nodes), a Junction
// seven, each pair linked by an internal hidden edge, and every hologram would duplicate them all.
// Instead a host owns ONE power connection (UACPRPowerConnectionComponent) and a Coupling is one
// hidden edge between the two HOSTS' connections; the terminal is the scene component that says
// where the coupling happens, which way it faces, and what occupies it. Vanilla's own snap points
// are scene components for the same reason.
//
// What a terminal does: position (its transform), outward axis (local +X), the quantized cell and
// reduced outward direction §6.1 compares, its §4 state, its partner, its private-interface flag,
// registry membership (EndPlay unregisters) and teardown: a terminal that is destroyed by any path
// clears its partner's half through OnComponentDestroyed, the hook where vanilla's own connection
// components leave their circuit (FGCircuitConnectionComponent.h:26).
//
// Events, not polls. SetCoupledTo and SetCapped tell the owner
// (IACPRTerminalHost::OnTerminalStateChanged) the moment a terminal's state changes; the indicators
// are pushed from there and nowhere else. Power arrives on the host's connection (OnHasPowerChanged).

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "ACPRTerminalComponent.generated.h"

/**
 * §4's three states. "Never two at once" is the whole point of an enum here rather than a pair of
 * booleans, which is what makes "open and capped" unrepresentable instead of merely wrong.
 */
UENUM( BlueprintType )
enum class EACPRTerminalState : uint8
{
	/** Accepts a coupling, a Cap, or a private attachment interface. Nothing occupies it. */
	Open		UMETA( DisplayName = "Open" ),

	/** Joined to exactly one other terminal. Invariant 4: exactly one occupant. */
	Coupled		UMETA( DisplayName = "Coupled" ),

	/** Insulated. Invariant 5: a capped terminal is not a geometric snap target. */
	Capped		UMETA( DisplayName = "Capped" )
};

UCLASS( ClassGroup = ( ACPR ), meta = ( BlueprintSpawnableComponent ) )
class AUTOCONNECTINGPOWERRAILS_API UACPRTerminalComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UACPRTerminalComponent();

	// Begin UActorComponent interface
	virtual void EndPlay( const EEndPlayReason::Type endPlayReason ) override;
	virtual void OnComponentDestroyed( bool destroyingHierarchy ) override;
	// End UActorComponent interface

	/**
	 * §4. Derived from the occupant rather than stored twice, so the two cannot disagree — on the
	 * authority. On a client nothing here is ever set (coupling is server-side, invariant 13), so a
	 * client reads the state the host replicated to it (SetReplicatedState) — vanilla's own pattern
	 * of a replicated cached bit on the buildable (FGBuildablePowerPole.h:110).
	 */
	UFUNCTION( BlueprintPure, Category = "ACPR|Terminal" )
	EACPRTerminalState GetTerminalState() const;

	UFUNCTION( BlueprintPure, Category = "ACPR|Terminal" )
	bool IsOpen() const { return GetTerminalState() == EACPRTerminalState::Open; }

	/** The terminal this one is coupled to, or null. Invariant 4: at most one. */
	UFUNCTION( BlueprintPure, Category = "ACPR|Terminal" )
	UACPRTerminalComponent* GetCoupledTo() const { return mCoupledTo; }

	/**
	 * The direction the terminal faces, in world space — the direction a Rail continuing from here
	 * would travel (§5.1: "A snapped Rail continues along the terminal's outward axis").
	 *
	 * Local +X by convention, because the beam axis is the actor's local X.
	 * A terminal at the far end of a Rail is therefore rotated 180° about Z relative to the near
	 * one, and both point AWAY from their own body.
	 */
	UFUNCTION( BlueprintPure, Category = "ACPR|Terminal" )
	FVector GetOutwardAxis() const { return GetComponentTransform().GetUnitAxis( EAxis::X ); }

	/**
	 * B.3: "Quantize terminal positions on registration and compare quantized values for equality."
	 *
	 * Deliberately not an epsilon compare. Two terminals are coincident when their quantized cells
	 * are equal and by no other test, which is what keeps §6.1's coincident-pair rule from becoming
	 * a proximity rule — invariant 2 says proximity never couples on its own.
	 *
	 * The quantum is 1 uu. Everything here lands on the 1 m grid or on our own 25 uu nudge: the two
	 * terminals of a 5 m Rail quantize to cells exactly 500 uu apart from placement floats such as
	 * 5150.008. The known cost is a boundary: two terminals 0.2 uu apart that straddle a
	 * cell edge quantize differently and will not couple. That is accepted rather than papered over
	 * — B.3 forbids the runtime epsilon that would fix it, and terminals produced by the same
	 * snapping machinery produce identical values rather than near ones.
	 */
	static FIntVector QuantizeLocation( const FVector& worldLocation );

	/** The quantum, in uu: two points closer than this are the same point everywhere the mod compares geometry. */
	static constexpr double LatticeQuantum = 1.0;

	/** This terminal's cell. */
	FIntVector GetQuantizedLocation() const { return QuantizeLocation( GetComponentLocation() ); }

	/**
	 * B.3: "Derive a Rail's outward axis from its two quantized endpoints rather than storing a
	 * separately quantized angle."
	 *
	 * This is that axis, as a reduced integer direction — the endpoint delta divided by the GCD of
	 * its components, so a 500 uu Rail and a 600 uu Rail pointing the same way both give (0,1,0).
	 *
	 * The point is that §6.1's "opposing" test becomes exact integer equality, `a == -b`, with no
	 * dot product and no angle tolerance anywhere. The owner sets it, because only the owner knows
	 * where its other endpoint is.
	 */
	void SetQuantizedOutward( const FIntVector& direction ) { mQuantizedOutward = direction; }
	FIntVector GetQuantizedOutward() const { return mQuantizedOutward; }

	/** delta / gcd(|x|,|y|,|z|), or zero for a zero delta. */
	static FIntVector ReduceDirection( const FIntVector& delta );

	/**
	 * §6.1: exactly coincident, opposing, and both open. No epsilon appears anywhere in here.
	 *
	 * And §10.1: both owners in the same build space — the world, or one particular Blueprint Designer.
	 * `checkSpace` exists for one caller only: FACPRCoupling::TryCoupleTerminal asks a second time with
	 * it off, so a pair refused ONLY for its spaces can be logged. Nothing may couple on that answer.
	 */
	bool CanCoupleWith( const UACPRTerminalComponent* other, bool checkSpace = true ) const;

	/** One line of state, for the log. */
	FString Describe() const;

	/**
	 * Runtime state, not saved state. The authoritative record of a Coupling is the owner's explicit
	 * list (invariant 6: "Couplings are stored explicitly. Loading restores them; it never infers them
	 * from geometry"); these are what that list is replayed into, so there is exactly one place a
	 * Coupling can be recorded and therefore exactly one place it can be wrong.
	 *
	 * Both tell the owner when the state actually changes — that call is the indicator event.
	 */
	void SetCoupledTo( UACPRTerminalComponent* other );
	void SetCapped( bool capped );

	/**
	 * A client's view of the state. Written by the owner's OnRep; read by GetTerminalState on a
	 * non-authority owner only. Never set on the server, where the derived answer is the truth.
	 */
	void SetReplicatedState( EACPRTerminalState state );

	/**
	 * §4's private one-sided mounting interface, as a property of the terminal rather than as a rule
	 * every caller has to remember.
	 *
	 * "A terminal-mounted Outlet owns a private one-sided mounting interface; attaching it creates a
	 * Coupling to that interface, keeping the host terminal inside the three-state model. That
	 * interface is not a continuation terminal, not a blueprint candidate, and cannot accept a Rail."
	 *
	 * All three of those are one thing: it is not a target. Marking it here means FACPRTerminalPicker
	 * skips it for every hologram at once, and §10.3's "Outlet-occupied terminals are excluded as
	 * sources" gets the same treatment — rather than four separate places each remembering to
	 * special-case an Outlet.
	 *
	 * It deliberately does NOT affect IsOpen(). The interface must stay open long enough for §6.1's
	 * coincident-pair rule to couple it to the host terminal — the same property every buildable
	 * relies on: place exactly, and let geometry couple.
	 */
	void SetPrivateInterface( bool isPrivate ) { mIsPrivateInterface = isPrivate; }
	bool IsPrivateInterface() const { return mIsPrivateInterface; }

private:
	/** Tells the owner that this terminal's state changed. The owner pushes its indicators from there. */
	void NotifyOwner();

	UPROPERTY( Transient )
	TObjectPtr< UACPRTerminalComponent > mCoupledTo = nullptr;

	/**
	 * §9's Cap. TRANSIENT, and that is the design rather than an omission: the AACPRCap actor is the
	 * saved record and re-applies this at its own BeginPlay. Storing it here as well would be a second
	 * source of truth for one fact.
	 */
	UPROPERTY( Transient )
	bool mIsCapped = false;

	/** §4's private mounting interface. See SetPrivateInterface. Set by the owner at construction. */
	UPROPERTY( Transient )
	bool mIsPrivateInterface = false;

	/** A client's copy of the state (see SetReplicatedState). Meaningless on the authority. */
	UPROPERTY( Transient )
	EACPRTerminalState mReplicatedState = EACPRTerminalState::Open;

	/** Set by the owning buildable; see SetQuantizedOutward. */
	FIntVector mQuantizedOutward = FIntVector( 0, 0, 0 );
};
