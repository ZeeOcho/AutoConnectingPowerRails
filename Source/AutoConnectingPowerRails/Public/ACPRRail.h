// Auto-Connecting Power Rails — the Power Rail buildable.
//
// What the Beam base gives (Appendix C.2):
//
//   * lightweight behaviour is only AFGBuildableFactoryBuildingLightweight's constructor defaults,
//     and the two flags are independent — the Rail stays unmanaged by the lightweight subsystem
//     (which keeps it a real actor) and renders from its own mesh components;
//   * AFGBeamHologram::ConfigureActor pushes its private mCurrentLength through the public
//     SetLength, so the whole length chain is free;
//   * the beam axis is the actor's local X.
//
// Every component is a CDO subobject: anything vanilla enumerates or holds a reference to must
// exist on the CDO, or the Hoverpack cannot see the connection and a saved Power Line cannot
// resolve it.
//
// The electrical model. A Rail is: two terminals (scene components — where a coupling happens and
// which way it faces), one power connection at the midpoint (the Hoverpack's node and the end of
// every Coupling's hidden edge), one body mesh and two collar meshes whose mesh is swapped for the
// bridged variant while that end is coupled to a Junction. Coupling happens at registration in
// BeginPlay, and the indicators are pushed when a terminal's state or the network's power changes;
// nothing polls.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"
#include "ACPRTerminalHost.h"
#include "FGBuildableBeam.h"
#include "ACPRRail.generated.h"

class UACPRTerminalComponent;
class UACPRPowerConnectionComponent;
class UStaticMesh;

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API AACPRRail : public AFGBuildableBeam, public IACPRTerminalHost
{
	GENERATED_BODY()

public:
	AACPRRail();

	// ---------------------------------------------------------------------------------------
	// IACPRTerminalHost — §4's terminals, from the resolver's point of view
	//
	// The Rail owns two: index 0 is A at local X=0, index 1 is B at local X=mLength. The indices
	// are saved, so they are part of the format and must not be reordered.
	// ---------------------------------------------------------------------------------------
	virtual int32 GetTerminalCount() const override { return 2; }
	virtual UACPRTerminalComponent* GetTerminalAtIndex( uint8 index ) const override;
	virtual void AddSavedCoupling( const FACPRSavedCoupling& coupling ) override;
	virtual const TArray< FACPRSavedCoupling >& GetSavedCouplings() const override { return mSavedCouplings; }
	virtual TArray< FACPRSavedCoupling >& GetMutableSavedCouplings() override { return mSavedCouplings; }
	virtual UACPRPowerConnectionComponent* GetPowerConnection() const override { return mPower; }
	virtual bool HasNetworkPower() const override;
	virtual void OnTerminalStateChanged( int32 index ) override;
	virtual bool WasLoadedFromSave() const override { return mCameFromSave || mSavedOnce; }	virtual bool WasPlacedFromBlueprint() const override { return mPlacedFromBlueprint; }
	/** §12's highlight: stored, and pushed at once when it changes. */
	virtual void SetHighlight( const FACPRHighlight& highlight ) override;
	virtual const FACPRHighlight* GetHighlight() const override { return &mHighlight; }
	virtual TArray< TWeakObjectPtr< AActor > >* GetAttachmentList() const override { return &mAttachments; }

	/** §10.1. AFGBuildable::mBlueprintDesigner, our own protected field (FGBuildable.h:1014). */
	virtual class AFGBuildableBlueprintDesigner* GetHostDesigner() const override;

	/** A at X=0 facing -X, B at X=mLength facing +X — CreateTerminals' rule, from mLength alone. */
	virtual bool GetTerminalLocalFrame( uint8 index, FTransform& out_frame ) const override;

	/**
	 * §10's blueprint path. Both are AFGBuildable virtuals (FGBuildable.h:547, 559). PostSerializedFrom-
	 * Blueprint is "called from blueprint subsystem after being loaded", with isBlueprintWorld telling
	 * the blueprint's private copy from the one being placed; the private copy gets its meshes fitted
	 * here because BeginPlay never runs in that world. PreSerializedToBlueprint clears the stored
	 * ledger: a blueprint Rail is costed from the recipe (§10.1).
	 */
	virtual void PreSerializedToBlueprint() override;
	virtual void PostSerializedFromBlueprint( bool isBlueprintWorld ) override;

	/**
	 * §8.3 and §9: dismantling a host takes its Caps and Outlets with it (FGBuildable.h:303); the
	 * list it reports is the one the attachments registered themselves into.
	 */
	virtual void GetChildDismantleActors_Implementation( TArray< AActor* >& out_ChildDismantleActors ) const override;

	/**
	 * §7.5's stored ledger: "The original's paid ledger is never recomputed from child lengths …
	 * Aggregate refund across every dismantle order therefore equals the original's exact paid
	 * ledger." Empty means "no split has happened to me" and the refund falls through to vanilla's
	 * recipe calculation; non-empty means this Rail is a child of a split and carries its share.
	 */
	virtual void GetDismantleRefund_Implementation( TArray< FInventoryStack >& out_refund,
	                                                bool noBuildCostEnabled ) const override;

	/**
	 * Writes this Rail's share of a split. An empty share is still a share: calling this at all means
	 * "this Rail's refund is decided by a ledger".
	 */
	void SetStoredLedger( const TArray< FInventoryStack >& ledger );
	bool HasStoredLedger() const { return mHasStoredLedger; }
	const TArray< FInventoryStack >& GetStoredLedger() const { return mStoredLedger; }

	/**
	 * Divides a ledger between two children in proportion to their effective lengths, integer
	 * remainders to the child the caller names (§7.5's stable endpoint order, decided from geometry).
	 */
	static void DivideLedger( const TArray< FInventoryStack >& source,
	                          double lengthA, double lengthB, bool firstWinsRemainder,
	                          TArray< FInventoryStack >& out_a, TArray< FInventoryStack >& out_b );

	/**
	 * The ledger seed: the first insertion captures what this Rail cost, once, from the recipe it was
	 * built with (UFGRecipe::GetIngredients, FGRecipe.h:59; AFGBuildable::GetBuiltWithRecipe,
	 * FGBuildable.h:328) times the beam's own ceil( L / mLengthPerCost ). Idempotent.
	 */
	void CaptureLedgerFromRecipe();

	/**
	 * Splits this Rail in two around a Junction cell, and removes it — modelled on vanilla's own
	 * one-into-three case (AFGBuildableConveyorBelt::Split, FGBuildableConveyorBelt.h:92). It creates no
	 * Couplings: the children are placed so their inner ends land on the Junction's faces and §6.1's
	 * coincident-pair rule joins all four at the children's own BeginPlay. The host's couplings are
	 * released first, so the children register into a world where both neighbours are open — correct
	 * by construction, with no deferral.
	 *
	 * @return the two children, near end first; empty on any refusal
	 */
	static TArray< AACPRRail* > Split( AACPRRail* rail, double centreAlong, double halfExtent );

	/** Local +X distance from terminal A to `worldLocation`, projected onto the beam axis. */
	double GetOffsetAlong( const FVector& worldLocation ) const;

	/** §13's single divisor, AFGBuildableBeam::mLengthPerCost (FGBuildableBeam.h:86). */
	float GetLengthPerCost() const { return mLengthPerCost; }

	/**
	 * §10.4 / §11's occlusion disc for a Rail end: "face width − 0.1 m" halved would be 20 uu on the
	 * 50 cm profile; the blueprint manager uses 45, and it is kept here as the one number rather than
	 * derived twice.
	 */
	static constexpr double OcclusionDiscRadius = 45.0;

	virtual void BeginPlay() override;

	/**
	 * §14's "dismantling any element immediately removes graph edges". FACPRCoupling::ReleaseAll runs
	 * here, before the dismantle effect; the terminals' own OnComponentDestroyed covers every other
	 * destruction path.
	 */
	virtual void Dismantle_Implementation() override;

	/** §14. Without this the Rail does not survive a reload. */
	virtual bool ShouldSave_Implementation() const override;

	/** mLength is SaveGame; this is where a loaded Rail confirms it came back. */
	virtual void PostLoadGame_Implementation( int32 saveVersion, int32 gameVersion ) override;

	/** The data-side witness that this host has been through a save (see WasLoadedFromSave). */
	virtual void PreSaveGame_Implementation( int32 saveVersion, int32 gameVersion ) override;

	virtual void GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const override;

	/**
	 * The Rail's two terminals as vanilla attachment points (FACPRAttachment). Replaces the
	 * painted beam's single front point (AFGBuildableBeam::GetAttachmentPoints, FGBuildableBeam.h:42) —
	 * a Rail is not chained onto like a beam; it is coupled to at either end. A is the CDO component
	 * (so the hologram caches it as its local point); B is computed from the length, as the beam does
	 * for its own front point.
	 */
	virtual void GetAttachmentPoints( TArray< const FFGAttachmentPoint* >& out_points ) const override;

	/**
	 * No instance data from a Rail that already draws itself. The blueprint hologram asks for it and
	 * would show the Rail twice. Returns nothing.
	 */
	virtual TArray< struct FInstanceData > GetActorLightweightInstanceData_Implementation() const override;

	/** The body mesh, bound in the blueprint by acpr_bind_meshes.py; mRailMeshAsset is the fallback. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	TObjectPtr< UStaticMesh > mRailMesh = nullptr;

	/** The same mesh as a path with a default in source, so a clean checkout renders a Rail. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	TSoftObjectPtr< UStaticMesh > mRailMeshAsset =
		TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
			TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_RailBody.SM_ACPR_RailBody" ) ) );

	/** The collar, and the collar with the 2 cm bridge slice grown on. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	TSoftObjectPtr< UStaticMesh > mTerminalMeshAsset =
		TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
			TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_RailTerminal.SM_ACPR_RailTerminal" ) ) );

	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Rail" )
	TSoftObjectPtr< UStaticMesh > mTerminalBridgedMeshAsset =
		TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
			TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_RailTerminalBridged.SM_ACPR_RailTerminalBridged" ) ) );

	/**
	 * The Rail's mesh components. The body (a UFGColoredInstanceMeshProxy: a UStaticMeshComponent that
	 * takes customization data, instancing blocked so it renders as a plain component), and the two
	 * collars at the ends — never stretched, which is why they are separate meshes. Each collar wears
	 * SM_ACPR_RailTerminal, or SM_ACPR_RailTerminalBridged while its terminal is coupled to a Junction
	 * (the slice fills the Junction's cap seat so the four power strips run into the socket). The swap
	 * happens on the coupling event, and in PostSerializedFromBlueprint from the saved records.
	 */
	UPROPERTY( VisibleAnywhere, Category = "ACPR|Rail" )
	TObjectPtr< class UFGColoredInstanceMeshProxy > mRailMeshComponent = nullptr;

	UPROPERTY( VisibleAnywhere, Category = "ACPR|Rail" )
	TObjectPtr< class UFGColoredInstanceMeshProxy > mTerminalMeshA = nullptr;

	UPROPERTY( VisibleAnywhere, Category = "ACPR|Rail" )
	TObjectPtr< class UFGColoredInstanceMeshProxy > mTerminalMeshB = nullptr;

	/** Terminal A's attachment-point component (vanilla caches it on the hologram). See GetAttachmentPoints. */
	UPROPERTY( VisibleAnywhere, Category = "ACPR|Rail" )
	TObjectPtr< class UFGAttachmentPointComponent > mAttachmentA = nullptr;

	/** Pushes the cue materials from the current state. Called on events only, never polled. */
	void RefreshIndicators();

	/** The aiming hologram's highlight; read by FACPRIndicators::Push. */
	FACPRHighlight mHighlight;

	/** §4: "Each Rail is continuous between its two terminals." A sits at local X=0, B at X=mLength. */
	UFUNCTION( BlueprintPure, Category = "ACPR|Rail" )
	UACPRTerminalComponent* GetTerminalA() const { return mTerminalA; }

	UFUNCTION( BlueprintPure, Category = "ACPR|Rail" )
	UACPRTerminalComponent* GetTerminalB() const { return mTerminalB; }

	// ---------------------------------------------------------------------------------------
	// Mesh fitting — shared with the hologram (two renderers, one fit)
	// ---------------------------------------------------------------------------------------

	/**
	 * Rotates, stretches and centres the Rail's body mesh for `length`, inset by the collars' reach.
	 * The mesh is authored 4 m along its own +Z with its base at the origin; the beam axis is the
	 * actor's local X, so the component carries a -90° pitch and only the mesh's Z is stretched.
	 */
	static FVector FitRailMeshToLength( class UStaticMeshComponent* comp, double length,
	                                    double inset = 0.0 );

	/** The -90 pitch alone — the hologram's preview mesh inherits the CDO's transform and needs only this. */
	static void OrientRailMesh( class UStaticMeshComponent* comp );

	/**
	 * Gives the body its mesh and fits it to GetLength(), then places the collars. Called from BeginPlay
	 * (the first moment the length is known) and from PostSerializedFromBlueprint(true).
	 */
	void SetUpRailMesh();

protected:
	/** mRailMesh if it is set, otherwise mRailMeshAsset loaded on first use. May return null. */
	UStaticMesh* ResolveRailMesh() const;

	/** Places the two collars: mesh, orientation and position, and never a scale. */
	void SetUpTerminalMeshes();

	/** Rebuilds mTerminalPoints from the terminal frames (BeginPlay, PostSerializedFromBlueprint). */
	void RefreshAttachmentPoints();
	TArray< FFGAttachmentPoint > mTerminalPoints;

	/**
	 * Swaps collar `index` between the plain and the bridged mesh according to what its terminal is
	 * coupled to — a host with more than two terminals, which is a Junction. Returns true
	 * if the mesh changed.
	 */
	bool UpdateCollarMesh( int32 index );

	/** Every UStaticMeshComponent on the placed actor with the mesh it holds, for a Verbose line. */
	void LogMeshSources( const TCHAR* stage ) const;

	/** One line. The build stamp it carries is the only reliable stale-binary check. */
	void LogRailState( const TCHAR* stage ) const;

	/** The power delegate's handler: the network's power changed on this host. */
	void HandleHasPowerChanged( bool hasPower );

	/** A client received the host's replicated terminal states / power. */
	UFUNCTION()
	void OnRep_TerminalState();

private:
	mutable TArray< TWeakObjectPtr< AActor > > mAttachments;

	/** §7.5's stored ledger. SaveGame; cleared before a blueprint is written (§10.1). */
	UPROPERTY( SaveGame )
	TArray< FInventoryStack > mStoredLedger;

	/** Whether a ledger was assigned — not the same question as whether it has anything in it. */
	UPROPERTY( SaveGame )
	bool mHasStoredLedger = false;

	/** Attaches both terminals and puts A at local X=0 and B at local X=mLength, both facing outward. */
	void CreateTerminals();

	/** Puts the one power connection at the midpoint and binds its power delegate. */
	void PlacePowerConnection();

	/** Files both terminals under their quantized cells, once the geometry above is final. */
	void RegisterTerminals();

	/** The Rail's topology in three lines. Verbose. */
	void LogTopology( const TCHAR* stage );

	/** Writes the per-terminal replicated state from the authoritative one (server). */
	void CaptureReplicatedState();

	/** Set by PostLoadGame. */
	bool mCameFromSave = false;

	/** Set in PreSaveGame and saved, so a loaded host knows it was loaded whatever ran first. */
	UPROPERTY( SaveGame )
	bool mSavedOnce = false;

	/** How often vanilla has called Dismantle on this actor — see Dismantle_Implementation. */
	int32 mDismantleCalls = 0;

	/** "-", "placed" or "bpworld", from PostSerializedFromBlueprint. For the log. */
	const TCHAR* mBlueprintState = TEXT( "-" );

	/** Set by PostSerializedFromBlueprint( false ): this host was placed as part of a blueprint. */
	bool mPlacedFromBlueprint = false;

	/** §4's two terminals. CDO subobjects. */
	UPROPERTY()
	TObjectPtr< UACPRTerminalComponent > mTerminalA = nullptr;

	UPROPERTY()
	TObjectPtr< UACPRTerminalComponent > mTerminalB = nullptr;

	/** The one power connection. CDO subobject; at the midpoint from BeginPlay. */
	UPROPERTY()
	TObjectPtr< UACPRPowerConnectionComponent > mPower = nullptr;

	/**
	 * §6.1's Couplings, as the explicit saved record invariant 6 demands. On the host rather than on
	 * the terminal so there is exactly one place a Coupling is recorded.
	 */
	UPROPERTY( SaveGame )
	TArray< FACPRSavedCoupling > mSavedCouplings;

	/**
	 * A client's view. One byte per terminal (EACPRTerminalState) and the network's power, written by
	 * the server from the same events that push the indicators, applied by OnRep. Vanilla's pattern: a
	 * replicated cached bit on the buildable (FGBuildablePowerPole.h:110). Untested in multiplayer.
	 */
	UPROPERTY( ReplicatedUsing = OnRep_TerminalState )
	TArray< uint8 > mTerminalStateRep;

	UPROPERTY( ReplicatedUsing = OnRep_TerminalState )
	bool mHasPowerRep = false;
};
