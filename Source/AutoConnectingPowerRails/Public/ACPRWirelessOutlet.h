// Auto-Connecting Power Rails — the Power Rail Wireless Outlet.
//
// §16: "Three tiers — Mk.1, Mk.2, Mk.3 — identical in every rule and differing only in range and
// recipe. Each has one Power Connection, which is hidden: no Power Line can be attached to it, and
// every link to a machine is a hidden connection from the machine's component to it."
//
// ==========================================================================================
// AN OUTLET THAT LINKS INSTEAD OF ACCEPTING CABLES.
// ==========================================================================================
//
// Everything about WHERE it sits is the Outlet's (§16.1 points at §8.1 and §8.2 without change), so
// this is an AACPROutlet with the socket's wire budget set to zero — which ConfigureAsWireSocket
// turns into a hidden connection, and hidden is what AddHiddenConnection requires of one side.
// Through that one connection the outlet is an ordinary node of its Rail network, exactly as the
// Outlet is; what differs is who the other end of each hidden edge belongs to: a machine's own
// power connection rather than a host's.
//
// The outlet itself holds no rule about which machines it serves. §16.2's eligibility, §16.3's
// one-outlet rule and §16.4's events are the link subsystem's (UACPRWirelessLinkSubsystem), one per
// world, because every one of them is a question about SEVERAL outlets and the machines between
// them. This class owns what is its alone: its range, its blocking classes, the links it currently
// holds (and their replication for the line visuals), and the two moments it enters and leaves the
// subsystem's index.
//
// ==========================================================================================
// WHAT IS SAVED, AND WHAT IS REBUILT.
// ==========================================================================================
//
// The links are the machines' saved hidden connections — vanilla saves and restores
// UFGCircuitConnectionComponent::mHiddenConnections on both ends (FGCircuitConnectionComponent.h:192,
// SaveGame) — so a loaded outlet infers nothing (invariant 6): it reads its own connection's hidden
// edges back and keeps every one whose other end is a machine. The replicated line ends are the only
// thing of our own, and they are rebuilt from the links, never saved.
//
// ==========================================================================================
// WHAT IS DRAWN (§16.5).
// ==========================================================================================
//
// One line per link, from the link origin to the machine's connection point, on one instanced mesh
// component, and one link END per link: a short open tube round the machine's power socket, on a
// second instanced component, wearing the same material at a few centimetres' bead length. The
// material is the state: unpowered, a faint dashed line and standing rings on the socket; powered,
// a bead travelling from the outlet into the machine and a band crawling up the socket
// (M_ACPR_Link, acpr_materials.py). The line says where a link goes; the end says, at the machine,
// that it has one. The state is the outlet's own power, from the same event the cue uses
// (RefreshIndicators), and while the outlet is aimed at in dismantle mode its range sphere shows
// and its lines are drawn powered. The range sphere is the same component a placement preview
// shows (UACPRWirelessPreviewComponent).

#pragma once

#include "CoreMinimal.h"
#include "ACPROutlet.h"
#include "ACPRSpec.h"
#include "ACPRWirelessOutlet.generated.h"

class UACPRSocketTableAsset;
class UFGPowerConnectionComponent;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMeshComponent;

/**
 * §16.7: one link's far end as a client draws it — the connection point the line ends at, and the
 * socket knob's fit (FACPRSocketTable) resolved to world space: a point on the knob's axis, the
 * axis, the radius, and the tube's extent along the axis from that point.
 */
USTRUCT()
struct FACPRLinkTarget
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Point = FVector::ZeroVector;

	UPROPERTY()
	FVector Centre = FVector::ZeroVector;

	UPROPERTY()
	FVector Axis = FVector::UpVector;

	UPROPERTY()
	float Radius = 29.5f;

	UPROPERTY()
	float Core = 17.8f;

	UPROPERTY()
	float Bottom = -28.0f;

	UPROPERTY()
	float Top = 26.0f;
};

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API AACPRWirelessOutlet : public AACPROutlet
{
	GENERATED_BODY()

public:
	AACPRWirelessOutlet();

	virtual void BeginPlay() override;
	virtual void EndPlay( const EEndPlayReason::Type endPlayReason ) override;
	virtual void Dismantle_Implementation() override;
	virtual void GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const override;

	/** §16.5's dismantle-aim readout: the range sphere, and the lines drawn powered, while aimed at. */
	virtual void StartIsLookedAtForDismantle_Implementation( class AFGCharacterPlayer* byCharacter ) override;
	virtual void StopIsLookedAtForDismantle_Implementation( class AFGCharacterPlayer* byCharacter ) override;

	/**
	 * §16.6: the tier's range in uu, from the outlet's link origin to a machine's power connection
	 * point. The C++ default is Mk.1's; acpr_buildables.py writes each tier's own onto its Build_
	 * blueprint, and the console variable acpr.WirelessRange overrides every tier for testing (read
	 * by the subsystem, never written here).
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	float mRange = ACPRSpec::WirelessRangeMk1;

	/**
	 * §16.2's blocking list: a buildable on the line between the outlet and a machine blocks the link
	 * when its class is, or derives from, one of these. The default is what sections a factory —
	 * the foundation family and the wall family (constructor) — so a machine behind a machine, a
	 * pillar or a Rail still links. Non-buildable world geometry — terrain, cliffs — blocks
	 * regardless. The console variable acpr.WirelessBlockers overrides this list by class name for
	 * testing.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	TArray< TSubclassOf< class AFGBuildable > > mBlockingClasses;

	/**
	 * Where a link line leaves the outlet and where the range is measured from: this far along the
	 * mounting normal (local +X) from the mount point, which is the top of the authored body
	 * (acpr_meshes.py, make_wireless_outlet). A value, not a measurement, because the hologram has to
	 * draw the range sphere about the same point before any body exists.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	float mLinkOriginHeight = 12.0f;

	/** §16.5: the link line's thickness in uu. The line mesh is authored 2 uu across; this scales it. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	float mLinkLineWidth = 1.5f;

	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	TSoftObjectPtr< UStaticMesh > mLinkLineMesh = TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_Link.SM_ACPR_Link" ) ) );

	/** §16.5's two states of a line, both instances of M_ACPR_Link (acpr_materials.py). */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	TSoftObjectPtr< UMaterialInterface > mLinkMaterial = TSoftObjectPtr< UMaterialInterface >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_Link.MI_ACPR_Link" ) ) );
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	TSoftObjectPtr< UMaterialInterface > mLinkMaterialPowered = TSoftObjectPtr< UMaterialInterface >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_LinkPowered.MI_ACPR_LinkPowered" ) ) );

	/**
	 * §16.5's link end: the tube round a linked machine's power socket. Its fit — axis, offset,
	 * radius, extent — is the machine's entry in the socket table (FACPRSocketTable, measured off
	 * the machines' meshes by Scripts/acpr_sockets.py), not a property here. The asset reference
	 * below is what ships the table: the loader names it by path, which the cooker does not follow.
	 */
	UPROPERTY( VisibleDefaultsOnly, Category = "ACPR|Wireless" )
	TObjectPtr< UACPRSocketTableAsset > mSocketTable = nullptr;

	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	TSoftObjectPtr< UStaticMesh > mLinkEndMesh = TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_LinkEnd.SM_ACPR_LinkEnd" ) ) );
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	TSoftObjectPtr< UMaterialInterface > mLinkEndMaterial = TSoftObjectPtr< UMaterialInterface >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_LinkEnd.MI_ACPR_LinkEnd" ) ) );
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	TSoftObjectPtr< UMaterialInterface > mLinkEndMaterialPowered = TSoftObjectPtr< UMaterialInterface >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_LinkEndPowered.MI_ACPR_LinkEndPowered" ) ) );
	/** The inner tube's materials: the same, half a period behind, so the band climbs the outer tube and then the inner. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	TSoftObjectPtr< UMaterialInterface > mLinkEndInnerMaterial = TSoftObjectPtr< UMaterialInterface >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_LinkEndInner.MI_ACPR_LinkEndInner" ) ) );
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	TSoftObjectPtr< UMaterialInterface > mLinkEndInnerMaterialPowered = TSoftObjectPtr< UMaterialInterface >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Materials/MI_ACPR_LinkEndInnerPowered.MI_ACPR_LinkEndInnerPowered" ) ) );

	/**
	 * The range sphere's mesh, authored at mRangeSphereAuthoredRadius (acpr_meshes.py,
	 * make_range_sphere) and scaled to the tier's effective range wherever it is shown: on this
	 * outlet while aimed at in dismantle mode or while a placement preview reaches it, and on the
	 * tier's hologram while placing.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	TSoftObjectPtr< UStaticMesh > mRangeSphereMesh = TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_RangeSphere.SM_ACPR_RangeSphere" ) ) );

	/** The radius the sphere mesh is authored at, uu. Duplicated from the script; the log reports the fit. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Wireless" )
	float mRangeSphereAuthoredRadius = 50.0f;

	/** The point the range is measured from and the lines are drawn from. */
	FVector GetLinkOrigin() const;

	/** The current links, authority only: the machine connections whose hidden edge ends on this outlet's socket. */
	const TArray< TWeakObjectPtr< UFGPowerConnectionComponent > >& GetLinks() const { return mLinks; }
	int32 GetLinkCount() const { return mLinks.Num(); }
	bool IsLinkedTo( const UFGPowerConnectionComponent* connection ) const;

	/**
	 * The hidden edge from `connection` to this outlet's socket, and the bookkeeping behind it. Both
	 * are the subsystem's calls — the rules live there — but the edge is this actor's to make, so
	 * that an outlet leaving the world can undo every one of its own (Dismantle, EndPlay).
	 * Returns true when the edge exists afterwards.
	 */
	bool Link( UFGPowerConnectionComponent* connection, const TCHAR* why );
	bool Unlink( UFGPowerConnectionComponent* connection, const TCHAR* why );
	void UnlinkAll( const TCHAR* why );

	/**
	 * Reads the socket's hidden edges back and adopts every one whose other end is a machine's
	 * connection: the load path (invariant 6), and the blueprint path for links a blueprint carried.
	 */
	void AdoptSavedLinks();

	/**
	 * Shows or hides this outlet's range sphere on behalf of `who` (a preview component, or this
	 * outlet itself while aimed at). The sphere is visible while anyone wants it. Local, never
	 * replicated: a readout for the player looking.
	 */
	void SetRangeShownBy( const UObject* who, bool shown );

	/**
	 * A range sphere about `relativeLocation` under `root`: the mesh above, scaled from its authored
	 * radius to `range`, no collision, no shadow, not a hologram mesh. Shared by the built outlet and
	 * the hologram so both draw the same sphere. Null when the mesh is missing (logged).
	 */
	static UStaticMeshComponent* MakeRangeSphere( UObject* outer, USceneComponent* root, const AACPRWirelessOutlet* cdo,
	                                              const FVector& relativeLocation, double range );

	/** The log's one line per outlet: tier, range, links. */
	void LogWireless( const TCHAR* stage ) const;

protected:
	/** The Outlet's cue refresh, plus the line material from the same state. */
	virtual void RefreshIndicators() override;

	UFUNCTION()
	void OnRep_LinkTargets();

	/** Rebuilds the line and link end instances from mLinkTargets. Server after a change, client on replication. */
	void RebuildLines();

	/** Puts the powered or the unpowered materials on the lines and the link ends, from HasNetworkPower and the aim. */
	void RefreshLineMaterial();

public:
	/** Authority: mLinkTargets from mLinks, then the lines. Public for acpr.ReloadSockets, which refits every outlet. */
	void CaptureLinkEnds();

private:
	/** Authority's list. Weak, so a machine destroyed without telling us can never be dereferenced. */
	TArray< TWeakObjectPtr< UFGPowerConnectionComponent > > mLinks;

	/** §16.7: the far end of every line, world space, for clients. Rebuilt, never saved. */
	UPROPERTY( ReplicatedUsing = OnRep_LinkTargets )
	TArray< FACPRLinkTarget > mLinkTargets;

	/** §16.5's lines: one instance per link, world-space transforms. Plain ISM — no swatch, no cue. */
	UPROPERTY( VisibleAnywhere, Category = "ACPR|Wireless" )
	TObjectPtr< UInstancedStaticMeshComponent > mLinkLines = nullptr;

	/** §16.5's link ends: one tube per link round the machine's socket's discs. The same kind of ISM. */
	UPROPERTY( VisibleAnywhere, Category = "ACPR|Wireless" )
	TObjectPtr< UInstancedStaticMeshComponent > mLinkEnds = nullptr;

	/** The inner tubes, round the socket's core between its discs, on the phase-shifted material. */
	UPROPERTY( VisibleAnywhere, Category = "ACPR|Wireless" )
	TObjectPtr< UInstancedStaticMeshComponent > mLinkEndsInner = nullptr;

	/** The range sphere, made on first request and hidden when nobody asks for it. */
	UPROPERTY()
	TObjectPtr< UStaticMeshComponent > mRangeSphere = nullptr;
	TSet< TWeakObjectPtr< const UObject > > mRangeShownBy;

	bool mRegistered = false;
	bool mAimedForDismantle = false;
	/** Which material the lines wear now, so a refresh that changes nothing touches nothing. */
	bool mLinesPowered = false;
	bool mLinesMaterialSet = false;
};
