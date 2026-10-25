// Auto-Connecting Power Rails — the wireless link subsystem.
//
// §16.2, §16.3 and §16.4 are rules about SEVERAL outlets and the machines between them: which
// machine is eligible, which ONE outlet it links to, and which event changes that. None of them
// can be answered by one outlet alone, so they live here — one per world, like the terminal
// registry, and for the same reasons (ACPRTerminalRegistry.h).
//
// ==========================================================================================
// EVENTS, NOT POLLING (invariant 17).
// ==========================================================================================
//
// Every link change is the direct result of one event. Four are vanilla broadcasts this subsystem
// binds once per world; three are vanilla calls that have no broadcast, reached by the module's
// SML hooks (AutoConnectingPowerRails.cpp) and handed in here:
//
//   a buildable begins play        AFGBuildableSubsystem::mBuildableAddedDelegate
//   a buildable is removed         AFGBuildableSubsystem::mBuildableRemovedDelegate
//   a Power Line is attached       UFGCircuitConnectionComponent::OnConnectionChanged, on every
//                                  machine connection within range of an outlet (watched here)
//   a Power Line is removed        AFGBuildable::Dismantle_Implementation on the wire (hook) —
//                                  the connection's own OnConnectionChanged fires only once the
//                                  wire is already gone, and a wire's removal is not broadcast
//   an outlet begins or ends       AACPRWirelessOutlet, through Register/Unregister
//   a foundation or wall is built  AFGLightweightBuildableSubsystem::AddFromBuildable (hook) — a
//                                  lightweight buildable converts to an instance inside its own
//                                  BeginPlay, before AddBuildable is reached (FGBuildable.cpp,
//                                  ShouldConvertToLightweight), so no delegate ever fires for it
//   a foundation or wall is removed  AFGLightweightBuildableSubsystem::RemoveByInstanceIndex (hook),
//                                  before the instance goes, which is where its transform still is
//   a machine is built in, or removed from, a Blueprint Designer
//                                  AFGBuildableSubsystem::OnDesignerBuildableSpawned and
//                                  AFGBuildableBlueprintDesigner::OnBuildableDismantledInsideDesigner
//                                  (hooks) — Designer buildables are kept out of the buildable
//                                  subsystem by vanilla, so its delegates never mention them
//   the console overrides change   acpr.WirelessRange / acpr.WirelessBlockers
//
// A machine's arrival asks "which outlet"; an outlet's arrival asks "which machines"; a wire asks
// "still eligible"; anything built or removed near an outlet asks "still clear". Each answer is one
// bounded operation: the outlet index is a spatial grid bucketed at the longest tier's range, so
// "outlets whose range reaches this point" is a lookup over a constant number of buckets, and the
// machines around an outlet come from one physics overlap of its range.
//
// The placement previews (§16.5) are the one per-frame consumer, and they are gated by events too:
// a preview exists only while a hologram does, re-asks only when that hologram has moved or a link
// event has happened since (mWorldVersion), and each ask is the same bounded lookup.
//
// ==========================================================================================
// THE ONE-OUTLET RULE (invariant 16).
// ==========================================================================================
//
// A machine connection is linked to at most one outlet: the nearest in range with a clear line,
// ties broken by the outlets' names so every load agrees. A machine between two outlets on
// unrelated networks therefore joins the nearer and never both — which is the one thing a cable
// never does by accident, and what priority switches and separate grids depend on.
//
// ==========================================================================================
// ONE SPACE (§10.1, §16.2).
// ==========================================================================================
//
// An outlet inside a Blueprint Designer links only machines inside that same Designer, and a world
// outlet links only world machines: the rule every Coupling in this mod already obeys. The links an
// outlet makes inside a Designer are saved hidden edges on both ends, so they are carried by the
// blueprint and come back when it is placed.

#pragma once

#include "CoreMinimal.h"
#include "ACPRSpatialGrid.h"
#include "ACPRSpec.h"
#include "Subsystems/WorldSubsystem.h"
#include "ACPRWirelessLinkSubsystem.generated.h"

class AACPRWirelessOutlet;
class AFGBuildable;
class AFGBuildableBlueprintDesigner;
class AFGBuildableFactory;
class UFGCircuitConnectionComponent;
class UFGPowerConnectionComponent;

/**
 * The outlet index's bucket: the longest tier's range, so a sphere of that radius touches at most
 * 3x3x3 buckets and a lookup never costs more than the outlets inside them.
 */
static constexpr int32 ACPR_WIRELESS_BUCKET = static_cast< int32 >( ACPRSpec::WirelessRangeMax );

/** One outlet's answer for one point: in range, and either clear or blocked at a place. */
struct FACPRLinkCandidate
{
	AACPRWirelessOutlet* Outlet = nullptr;
	double Distance = 0.0;
	bool Clear = false;
	/** Where the line stops when not clear, world space; the preview draws the line that far. */
	FVector BlockedAt = FVector::ZeroVector;
	FString Blocker;
};

/** One line of a placement preview (§16.5): from an outlet (or the outlet being placed) toward a point. */
struct FACPRPreviewLine
{
	FVector From = FVector::ZeroVector;
	FVector To = FVector::ZeroVector;
	/** The link this placement would make: drawn as a powered line. Otherwise a faint one, possibly cut short. */
	bool Chosen = false;
};

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API UACPRWirelessLinkSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Null if there is no world, which is normal during shutdown — callers must check. */
	static UACPRWirelessLinkSubsystem* Get( const UObject* worldContext );

	virtual void OnWorldBeginPlay( UWorld& world ) override;
	virtual void Deinitialize() override;

	// ---------------------------------------------------------------------------------------
	// The outlet side.
	// ---------------------------------------------------------------------------------------

	/** Files the outlet under its link origin. Idempotent. Binds the vanilla delegates on first use. */
	void RegisterOutlet( AACPRWirelessOutlet* outlet );

	/** Takes it out of the index; the outlet has already unlinked everything it held. */
	void UnregisterOutlet( AACPRWirelessOutlet* outlet );

	/**
	 * §16.4, "outlet built or placed": one overlap of the range for factory buildables; every
	 * eligible connection is linked here if unlinked, or moved here if its current outlet is
	 * farther. Also what a removed obstacle re-runs.
	 */
	void LinkAround( AACPRWirelessOutlet* outlet, const TCHAR* why );

	/**
	 * Re-traces every link this outlet holds and drops the ones no longer clear (an obstacle built).
	 * `onlyThrough` limits the walk to links crossing that box.
	 */
	void RetestLinks( AACPRWirelessOutlet* outlet, const FBox* onlyThrough, const TCHAR* why );

	// ---------------------------------------------------------------------------------------
	// The machine side.
	// ---------------------------------------------------------------------------------------

	/**
	 * §16.4, "machine built, placed or loaded", and "Power Line removed": if the connection is a
	 * candidate and unwired, it links to the nearest outlet in range with a clear line, if any.
	 * Idempotent: a connection already linked to that outlet is left alone.
	 */
	void ConsiderConnection( UFGPowerConnectionComponent* connection, const TCHAR* why );

	/** Every outlet re-runs its query and re-tests its links: the console overrides changed. */
	void ReevaluateAll( const TCHAR* why );

	// ---------------------------------------------------------------------------------------
	// The hooked events (see the header comment). Called by the module's SML hooks.
	// ---------------------------------------------------------------------------------------

	/** A lightweight buildable's instance was just added: its footprint may cut links. */
	void LightweightArrived( TSubclassOf< AFGBuildable > buildableClass, const FTransform& at );

	/** A lightweight buildable's instance is about to be removed: its footprint may have been the obstacle on a line. */
	void LightweightLeaving( TSubclassOf< AFGBuildable > buildableClass, const FTransform& at );

	/** A Power Line is being dismantled: both of its ends are unwired again, as far as this wire goes. */
	void WireDismantling( const class AFGBuildableWire* wire );

	/** A buildable began play inside a Blueprint Designer / is being dismantled inside one. */
	void DesignerBuildableSpawned( AFGBuildable* buildable );
	void DesignerBuildableDismantled( AFGBuildable* buildable );

	// ---------------------------------------------------------------------------------------
	// The rules, as predicates — one place, so the outlet and the machine side cannot disagree.
	// ---------------------------------------------------------------------------------------

	/** §16.2: a machine — a factory buildable; not infrastructure such as the Personnel Elevator's floor stops. */
	static bool IsMachine( const AActor* actor );

	/** §16.2: a visible power connection on a machine. (The space is the chooser's test.) */
	static bool IsCandidateConnection( const UFGPowerConnectionComponent* connection );

	/** §16.2: no live Power Line, and no hidden edge that is not a Wireless Outlet's. */
	static bool IsUnwired( const UFGPowerConnectionComponent* connection );

	/** Power Lines on the connection that are not being dismantled — the count every wire rule uses. */
	static int32 LiveWireCount( const UFGCircuitConnectionComponent* connection );

	/** The connection's wires, hidden edges and link, for the log. */
	static FString DescribeConnection( const UFGPowerConnectionComponent* connection );

	/** The outlet whose socket a connection's hidden edge ends on, or null. */
	static AACPRWirelessOutlet* LinkedOutletOf( const UFGPowerConnectionComponent* connection );

	/** §16.6: the tier's own range, or the console override while it is set. */
	static double EffectiveRange( const AACPRWirelessOutlet* outlet );

	/** §16.2's space: the Designer a buildable or hologram is in, or null for the world. */
	static const AFGBuildableBlueprintDesigner* SpaceOf( const UObject* buildableOrHologram );

	/**
	 * §16.2's clear line: one trace from the outlet's link origin to the connection point, ignoring
	 * the outlet, its host and the machine. A buildable hit blocks when its class matches the
	 * outlet's blocking list (or the console override); anything that is not a buildable blocks; a
	 * Power Line never does. `out_blocker` names what blocked, for the log.
	 */
	bool LineIsClear( const AACPRWirelessOutlet* outlet, const UFGPowerConnectionComponent* connection,
	                  FString* out_blocker ) const;

	/**
	 * The same test between two points. `rules` supplies the blocking list (a built outlet, or the
	 * class default of the tier being placed); `ignored` are the actors at either end. `out_blockedAt`
	 * is where the line stops when it is not clear.
	 */
	bool LineIsClear( const AACPRWirelessOutlet* rules, const FVector& from, const FVector& to,
	                  TArrayView< const AActor* const > ignored, FVector* out_blockedAt, FString* out_blocker ) const;

	/** Every registered outlet in `space` whose range reaches `point`, nearest first. */
	void FindOutletsReaching( const FVector& point, const AFGBuildableBlueprintDesigner* space,
	                          TArray< AACPRWirelessOutlet* >& out_outlets ) const;

	/**
	 * §16.3 for one point, in full: every outlet reaching it, nearest first, each with its clear or
	 * blocked verdict. The chosen outlet is the first clear one. `ignored` are the actors at the
	 * point's end (a machine, or a hologram).
	 */
	void ClassifyOutletsFor( const FVector& point, const AFGBuildableBlueprintDesigner* space,
	                         TArrayView< const AActor* const > ignored, TArray< FACPRLinkCandidate >& out_candidates ) const;

	/**
	 * §16.5's preview for an outlet being placed: the lines it would make from `origin` with
	 * `rules`' tier — chosen for every eligible, unwired connection in range it would take (nearest
	 * rule included), cut short for every one it is blocked from.
	 */
	void PreviewOutletAt( const AACPRWirelessOutlet* rules, const FVector& origin, const AFGBuildableBlueprintDesigner* space,
	                      const AActor* hologram, TArray< FACPRPreviewLine >& out_lines ) const;

	int32 GetOutletCount() const { return mOutletAt.Num(); }

	/** Bumped by every link event; a preview re-asks when it changes. */
	uint32 GetWorldVersion() const { return mWorldVersion; }

private:
	UFUNCTION()
	void HandleBuildableAdded( AFGBuildable* buildable );
	UFUNCTION()
	void HandleBuildableRemoved( AFGBuildable* buildable );

	void HandleConnectionChanged( UFGCircuitConnectionComponent* changed );

	/** Binds the vanilla delegates once the buildable subsystem exists. Safe to call often. */
	void EnsureBound();

	/** A machine (or anything) arrived: its connections ask for an outlet; its body may cut lines. */
	void BuildableArrived( AFGBuildable* buildable, const TCHAR* why );

	/** A box arrived (an actor's or a lightweight instance's): the outlets reaching it re-test lines through it. */
	void ObstacleArrived( const FBox& bounds, const TCHAR* why );

	/** A box is leaving: the outlets reaching it re-run their query, with the leaver ignored by the traces. */
	void ObstacleLeaving( const FBox& bounds, const TCHAR* why );

	/** Every visible power connection a machine owns. */
	static void CollectConnections( AFGBuildable* factory, TArray< UFGPowerConnectionComponent* >& out_connections );

	/** Machines with collision inside the sphere, actors and instanced alike. */
	void FindFactoriesAround( const FVector& origin, double range, const AActor* ignored, TSet< AFGBuildable* >& out_factories ) const;

	/** The §16.3 choice for one connection among the outlets reaching it. */
	AACPRWirelessOutlet* ChooseOutletFor( const UFGPowerConnectionComponent* connection, FString* out_reason ) const;

	/** Keeps the connection's OnConnectionChanged bound, with its wire count, so a cable change is seen. */
	void Watch( UFGPowerConnectionComponent* connection );
	void Unwatch( UFGPowerConnectionComponent* connection );

	/** Outlets whose range reaches any point of `bounds`, any space. */
	void FindOutletsReachingBox( const FBox& bounds, TArray< AACPRWirelessOutlet* >& out_outlets ) const;

	/** The console override of the blocking list, parsed once per change; empty = use each outlet's own. */
	static void BlockingOverride( TArray< UClass* >& out_classes );

	/** The world-space box a lightweight buildable of this class occupies at this transform, from its instance data. */
	static FBox LightweightBounds( TSubclassOf< AFGBuildable > buildableClass, const FTransform& at );

	/** Outlet -> where it is filed, so an unregister removes it from the right bucket. */
	TMap< TWeakObjectPtr< AACPRWirelessOutlet >, FVector > mOutletAt;
	TACPRSpatialGrid< TWeakObjectPtr< AACPRWirelessOutlet >, ACPR_WIRELESS_BUCKET > mOutlets;

	struct FWatch
	{
		FDelegateHandle Handle;
		int32 Wires = 0;
	};
	TMap< TWeakObjectPtr< UFGPowerConnectionComponent >, FWatch > mWatched;

	TWeakObjectPtr< class AFGBuildableSubsystem > mBoundTo;

	/**
	 * The buildable being removed while its removal is handled: still in the physics scene during
	 * the broadcast, so the traces that re-run because it is leaving must not see it.
	 */
	TWeakObjectPtr< const AActor > mIgnoredDuringRemoval;

	/** The same for a lightweight instance: its class and location identify it in a trace hit. */
	TWeakObjectPtr< const UClass > mIgnoredLightweightClass;
	FVector mIgnoredLightweightAt = FVector::ZeroVector;

	/**
	 * A lightweight instance that was just added, while its addition is handled: its instance may
	 * not answer a trace yet on that frame, so every clear-line test during the handling also
	 * treats its box as the buildable of its class — the drop and the re-choice then agree.
	 * Foundations and walls are boxes, so the box is the footprint.
	 */
	TWeakObjectPtr< const UClass > mArrivingLightweightClass;
	FBox mArrivingLightweightBox = FBox( ForceInit );

	uint32 mWorldVersion = 0;
};
