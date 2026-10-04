// Auto-Connecting Power Rails — the Power Rail Outlet.
//
// §8: "One tier, equivalent to Wall Outlet Mk.1: one Power Connection, four Power Lines
// (mMaxNumConnectionLinks = 4), vanilla connector feedback. Its Coupling does not count toward the
// four; Rail topology cannot use the Power Connection."
//
// The Outlet is the only door into the network. §4 is categorical: "External Power Lines reach the
// network only through an Outlet's Power Connection".
//
// ==========================================================================================
// TWO HOST MODES, ONE ACTOR, AND THE DIFFERENCE IS WHERE THE POWER CROSSES.
// ==========================================================================================
//
// TERMINAL-MOUNTED (§8.2). The Outlet occupies an open terminal through a private one-sided
// mounting interface. §4 spells out why the interface exists rather than the Outlet simply marking
// the host terminal occupied: "attaching it creates a Coupling to that interface, keeping the host
// terminal inside the three-state model". So the host terminal becomes Coupled — one of its three
// legal states — rather than needing a fourth state meaning "has an Outlet on it".
//
// That also means the Outlet needs no coupling code. Its interface lands on the host terminal's
// exact cell with the exact opposing axis, and §6.1's coincident-pair rule does the rest a tick
// later, unchanged and unaware an Outlet was involved. The property every buildable in this mod
// shares: place exactly, and let geometry couple.
//
// BODY-MOUNTED (§8.1). No terminal is involved at all: the Outlet clamps to one of a Rail's four
// longitudinal faces, "bonds without a visible Power Line, changes no terminal state, does not
// shorten the host". The bond is an ordinary hidden connection to the host's network, which is the
// same mechanism a Coupling uses and carries the same guarantee — invariant 7's four Power Lines
// are untouched by it, because hidden connections do not consume mMaxNumConnectionLinks.
//
// ==========================================================================================
// WHY THE WIRE SOCKET AND THE MOUNTING INTERFACE ARE TWO COMPONENTS.
// ==========================================================================================
//
// They have opposite visibility requirements. The socket must be VISIBLE so a Power Line can attach
// to it, and must carry a budget of four. The interface must be HIDDEN so a Power Line cannot, and
// carries no budget at all. One component cannot be both, and invariant 7 — "Rail topology and
// Power Line capacity are separate" — is precisely the statement that they must not be.

#pragma once

#include "CoreMinimal.h"
#include "ACPRTerminalHost.h"
#include "Buildables/FGBuildable.h"
#include "ACPROutlet.generated.h"

class UACPRTerminalComponent;
class UACPRPowerConnectionComponent;

/** Which of §8's two host modes this Outlet is in. Save format — see mHostMode. */
UENUM( BlueprintType )
enum class EACPROutletHostMode : uint8
{
	/** §8.2: occupies an open terminal through the private mounting interface. */
	Terminal	UMETA( DisplayName = "Terminal-mounted" ),

	/** §8.1: clamped to one of a Rail's four longitudinal faces, no terminal involved. */
	Body		UMETA( DisplayName = "Body-mounted" )
};

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API AACPROutlet : public AFGBuildable, public IACPRTerminalHost
{
	GENERATED_BODY()

public:
	AACPROutlet();

	virtual void BeginPlay() override;
	virtual void EndPlay( const EEndPlayReason::Type endPlayReason ) override;

	/**
	 * §14's "dismantling any element immediately removes graph edges", and IMMEDIATELY is the word
	 * that matters. See FACPRCoupling::ReleaseAll: the dismantle effect runs for seconds before the
	 * actor is destroyed, so a release that waited for EndPlay would leave the terminal Coupled or
	 * Capped for the whole animation, and not a valid open target until then.
	 */
	virtual void Dismantle_Implementation() override;


	/** §14, and §8.3: "Host mode, transform, Power Connection state and Power Lines are saved." */
	virtual bool ShouldSave_Implementation() const override;
	virtual void PostLoadGame_Implementation( int32 saveVersion, int32 gameVersion ) override;

	// ---------------------------------------------------------------------------------------
	// IACPRTerminalHost — exactly one terminal, and it is the private mounting interface.
	//
	// One rather than zero even in Body mode, because GetTerminalCount is save format and a count
	// that changed with host mode would make a saved coupling index mean different things in
	// different Outlets. In Body mode the interface is simply never registered, so §6.1 cannot find
	// it and nothing can couple to it.
	// ---------------------------------------------------------------------------------------
	virtual int32 GetTerminalCount() const override { return 1; }
	virtual UACPRTerminalComponent* GetTerminalAtIndex( uint8 index ) const override;
	virtual void AddSavedCoupling( const FACPRSavedCoupling& coupling ) override;
	virtual const TArray< FACPRSavedCoupling >& GetSavedCouplings() const override { return mSavedCouplings; }
	virtual TArray< FACPRSavedCoupling >& GetMutableSavedCouplings() override { return mSavedCouplings; }
	virtual UACPRPowerConnectionComponent* GetPowerConnection() const override { return mPowerConnection; }
	virtual bool HasNetworkPower() const override;
	virtual void OnTerminalStateChanged( int32 index ) override;
	virtual bool WasLoadedFromSave() const override { return mCameFromSave || mSavedOnce; }
	virtual void PreSaveGame_Implementation( int32 saveVersion, int32 gameVersion ) override;
	virtual void GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const override;

	/**
	 * The used / total connections readout a vanilla pole shows while a Power Line is being wired
	 * to it. Vanilla's route: the wire hologram tells the aimed buildable it is
	 * being looked at for a connection (AFGBuildable::StartIsLookedAtForConnection, FGBuildable.h:321,
	 * virtual), and AFGBuildablePowerPole answers by showing a UWidgetComponent whose widget is a
	 * UFGPoleConnectionsWidget (UI/FGPoleConnectionsWidget.h) told which connection to count
	 * (SetConnection). Same route here: the Outlet is not a pole, so it answers the same call itself,
	 * with the same widget class — vanilla's own, read off the Mk1 pole's blueprint by reflection
	 * when this field is left empty, or whatever the blueprint sets here.
	 */
	virtual void StartIsLookedAtForConnection( class AFGCharacterPlayer* byCharacter,
	                                           class UFGCircuitConnectionComponent* overlappingConnection ) override;
	virtual void StopIsLookedAtForConnection( class AFGCharacterPlayer* byCharacter ) override;

	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	TSubclassOf< class UFGPoleConnectionsWidget > mConnectionWidgetClass;

	UPROPERTY( VisibleAnywhere, Category = "ACPR|Outlet" )
	TObjectPtr< class UWidgetComponent > mConnectionsWidgetComponent = nullptr;	virtual bool WasPlacedFromBlueprint() const override { return mPlacedFromBlueprint; }

	/** §10.1. AFGBuildable::mBlueprintDesigner, our own protected field (FGBuildable.h:1014). */
	virtual class AFGBuildableBlueprintDesigner* GetHostDesigner() const override;

	/** Always false: §4's private mounting interface "is not a blueprint candidate". */
	/**
	 * The mount's frame relative to this actor, from saved state: the host terminal's saved frame
	 * (FACPRCoupling::SavedTerminalFrame), faced back into the host, made relative to our transform.
	 * Answered for the load path's geometry check; the manager never asks for a private interface.
	 */
	virtual bool GetTerminalLocalFrame( uint8 index, FTransform& out_frame ) const override;

	/** §10's blueprint path, instrumented. See AACPRRail for why these add no new import. */
	virtual void PreSerializedToBlueprint() override;
	virtual void PostSerializedFromBlueprint( bool isBlueprintWorld ) override;

	/**
	 * Written by the hologram at construction, inside Construct and therefore before BeginPlay.
	 * Everything this actor does at BeginPlay reads these.
	 *
	 * For Body mode `terminalIndex` is meaningless and is stored as 0; the host actor is the Rail.
	 */
	void SetHost( EACPROutletHostMode mode, AActor* hostActor, uint8 terminalIndex );

	/**
	 * Moves this Outlet onto another host, keeping its mode and its world transform.
	 *
	 * §8.3: "blueprint construction and Junction insertion remap both modes." A terminal-mounted
	 * Outlet moves to the equivalent terminal of the child that inherited that end; a body-mounted
	 * one moves to whichever child its position now lies on, "without moving them" (§7.4).
	 *
	 * Both modes need more than a host swap. Terminal mode owns a private mounting interface that is
	 * coupled to the host terminal, so the old coupling is released and the interface re-placed on
	 * the new one; body mode holds a hidden connection to the host's network, which has to be moved
	 * or the Outlet keeps powering from a Rail that no longer exists.
	 */
	void Remap( AActor* newHostActor, uint8 newTerminalIndex );

	EACPROutletHostMode GetHostMode() const { return mHostMode; }
	AActor* GetHostActor() const { return mHostActor; }
	uint8 GetHostTerminalIndex() const { return mHostTerminalIndex; }

	/** §4's private one-sided interface. Null-safe to read in Body mode; it is simply unused there. */
	UACPRTerminalComponent* GetMountInterface() const { return mMountInterface; }

	/** §8.1: true while this Outlet is body-mounted on `host` — the one non-coupling edge between two hosts. */
	bool IsBodyBondedTo( const AActor* host ) const { return mHostMode == EACPROutletHostMode::Body && host && mHostActor == host; }

	/** Resolves the saved host reference to a live terminal, or null. Terminal mode only. */
	UACPRTerminalComponent* ResolveHostTerminal() const;

	/**
	 * Invariant 7's number, and §8's: "four Power Lines (mMaxNumConnectionLinks = 4)".
	 *
	 * Editable because §13 leaves tier questions open ("Players needing more than four Power Lines
	 * add another Outlet to the same Rail"), and because a connection whose budget is left at the
	 * base constructor's default may accept no Power Line at all — a cable that looks connected and
	 * carries nothing. It is set explicitly, always.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	int32 mMaxPowerLines = 4;

	/** §11: "Outlet transverse envelope | Visual target <= 0.9 m (Appendix C.1)." */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	// Matches the authored mesh, so the fit is 1:1 and the accent keeps its authored width.
	// SM_ACPR_Outlet is the cylinder alone — OUTLET_RADIUS * 2 = 40 across; any other value here
	// stretches it. The pads take their widths from mBaseWidth* below, not from this. Well inside
	// §11's "visual target <= 0.9 m".
	//
	// acpr_meshes.py owns OUTLET_RADIUS; a script cannot read a header, so the [ACPR-FIT] warning
	// is what catches the two drifting apart rather than a promise that they will not.
	float mVisualExtent = 40.0f;

	/**
	 * How far the Outlet's body stands off the face it is mounted on.
	 *
	 * Zero: SM_ACPR_Outlet is "authored along +Z with its base on the mounting plane", so once the
	 * mesh is rotated onto the actor's +X the base already sits on the face. A blueprint that
	 * overrides it wins, and will float the Outlet off its pad.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mStandoff = 0.0f;

	/**
	 * How far an Outlet sinks INTO a Junction's beam receptacle when it mounts on a Junction
	 * terminal rather than on a Rail.
	 *
	 * A Junction terminal sits on the face and the socket is a 7 cm hole behind it, so at zero the
	 * Outlet's pad sits on the face and the whole thing hovers over the receptacle. The pad's TOP
	 * sits flush with the Junction's seat floor instead, which is the face a coupled Rail's end
	 * meets, 2 cm (JUNCTION_CAP_SEAT_DEPTH) behind the terminal plane; the pad's own rim stays out
	 * of sight, and the Junction's face rim carries the accent.
	 *
	 * Duplicated here because a script cannot read a header; [ACPR-OUTLET]'s mesh line is what
	 * reports the result.
	 *
	 * Rails keep 0: the pad fills a Rail terminal's pocket with its top on the terminal plane, so
	 * there is nothing for it to sink into.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mJunctionTerminalInset = 2.0f;

	/**
	 * The pad is its own component, one mesh per seat, and it sits flush.
	 *
	 * Three seats, each with its own width and its own plane:
	 *   Rail body      47.172 (the chamfered flat), top sunk mBodyInset below the face — invisible on
	 *                  the flat, visible as the plug that crosses the groove, which is what §A.2's
	 *                  "interrupts the groove" was for
	 *   Rail terminal  44 (the pocket), top ON the terminal plane — the pocket is 3 deep, the pad 2
	 *   Junction       50 (the socket), top on the seat floor, mJunctionTerminalInset in
	 * All three chamfered at ACCENT_WIDTH to match what they sit in. The mesh is
	 * authored with its top at z = 0 reaching -Z, so FitCapMesh with a negative standoff sinks it.
	 */
	UPROPERTY( VisibleAnywhere, Category = "ACPR|Outlet" )
	TObjectPtr< class UFGColoredInstanceMeshProxy > mOutletBase = nullptr;

	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	TSoftObjectPtr< UStaticMesh > mBaseMeshBody = TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_OutletBaseBody.SM_ACPR_OutletBaseBody" ) ) );
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	TSoftObjectPtr< UStaticMesh > mBaseMeshRail = TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_OutletBaseRail.SM_ACPR_OutletBaseRail" ) ) );
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	TSoftObjectPtr< UStaticMesh > mBaseMeshJunction = TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
		TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_OutletBaseJunction.SM_ACPR_OutletBaseJunction" ) ) );

	/** Pad widths per seat: RAIL_FLAT, RAIL_TERMINAL_RECESS_SIZE, JUNCTION_INDENT. [ACPR-FIT] catches drift. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mBaseWidthBody = 47.172f;
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mBaseWidthRail = 44.0f;
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mBaseWidthJunction = 50.0f;
	/** OUTLET_BASE_HEIGHT = ACCENT_WIDTH. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mBaseDepth = 2.0f;

	/** Body seat: the pad's top a millimetre below the Rail's flat — sunk in, never proud, and no z-fighting. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mBodyInset = 0.1f;
	/** Rail terminal seat: the pad's top exactly on the terminal plane, §9 and §11. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Outlet" )
	float mRailTerminalInset = 0.0f;

	/** Which of the three seats an Outlet is in — the pad mesh, its width and its inset all follow. */
	enum class ESeat : uint8 { Body, RailTerminal, JunctionTerminal };
	static ESeat SeatFor( EACPROutletHostMode mode, const IACPRTerminalHost* host );

	/**
	 * Both meshes, one call, two renderers — the cylinder and the pad, placed for a seat. Same
	 * reason FitOutletMesh is static and public: the hologram must draw what the buildable will.
	 */
	static void FitOutletMeshes( class UStaticMeshComponent* cylinder, class UStaticMeshComponent* base,
	                             ESeat seat, const AACPROutlet* cdo );

	/**
	 * The whole Outlet fit, in one place, because two renderers draw it. Same shape, and the same
	 * reason, as AACPRCap::FitCapMesh — see that comment for the argument in full.
	 *
	 * The short version: B.2 says a hologram's preview is duplicated from the buildable CDO's
	 * component templates, and nothing on that path calls the buildable's BeginPlay. So a fit that
	 * lives only in FitMesh() is a fit the preview never gets: the player would aim an unfitted
	 * placeholder at a face and a correctly-sized Outlet would appear.
	 *
	 * Static and public so the hologram can call it without becoming an AACPROutlet to borrow a
	 * formula, the same argument as AACPRJunction::FitMeshComponentToCube.
	 */
	static FVector FitOutletMesh( class UStaticMeshComponent* comp,
	                              float visualExtent, float standoff );

protected:
	/**
	 * Terminal mode: puts the interface on the host terminal's cell so §6.1 couples it. From the host's
	 * SAVED geometry, so it is right whether or not the host's BeginPlay has placed its components.
	 */
	void PlaceMountInterface();

	/** Body mode: one hidden edge from the socket to the host's connection. §4's "bonded to its host". */
	void BondToBody();

	void RegisterTerminals();

	/** The power delegate's handler: the network's power changed on this host. */
	void HandleHasPowerChanged( bool hasPower );

	UFUNCTION()
	void OnRep_TerminalState();

	void CaptureReplicatedState();

	/** Attach and fit — the whole of the mesh setup, from BeginPlay and from the blueprint world alike. */
	void SetUpMeshes();
	void FitMesh();

	void LogOutlet( const TCHAR* stage ) const;

private:
	/**
	 * The Power Connection. VISIBLE, with a budget of four — see the header for why this cannot be
	 * the same component as the mounting interface.
	 *
	 * A CDO subobject like every connection component in this mod, for two reasons: the Hoverpack
	 * cannot see a connection that is not on the CDO, and neither can a saved Power Line looking for
	 * the component it ends on. This one carries player-attached wires, so the second reason is not
	 * theoretical here — it is the whole feature.
	 */
	UPROPERTY()
	TObjectPtr< UACPRPowerConnectionComponent > mPowerConnection = nullptr;

	/**
	 * §4's private one-sided mounting interface. Hidden, no wire budget, marked private so
	 * FACPRTerminalPicker skips it for every hologram at once.
	 */
	UPROPERTY()
	TObjectPtr< UACPRTerminalComponent > mMountInterface = nullptr;

	/** See AACPRCap's equivalent: in C++ so an unfinished setup is visible rather than silent. */
	UPROPERTY( VisibleAnywhere, Category = "ACPR|Outlet" )
	// UFGColoredInstanceMeshProxy for the indicators — see AACPRRail::mRailMeshComponent.
	TObjectPtr< class UFGColoredInstanceMeshProxy > mOutletMesh = nullptr;
	/** Pushes the cue material from the current state. Events only. */
	void RefreshIndicators();

	/** A client's view — see AACPRRail::mTerminalStateRep. Untested in multiplayer. */
	UPROPERTY( ReplicatedUsing = OnRep_TerminalState )
	TArray< uint8 > mTerminalStateRep;

	UPROPERTY( ReplicatedUsing = OnRep_TerminalState )
	bool mHasPowerRep = false;


	// SaveGame AND Replicated: a client fits its seat from these. Vanilla's own pattern for
	// fields a client needs to draw a buildable (FGBuildable.h:1013-1018). OnRep re-fits the meshes.
	UPROPERTY( SaveGame, ReplicatedUsing = OnRep_Host )
	EACPROutletHostMode mHostMode = EACPROutletHostMode::Terminal;

	UPROPERTY( SaveGame, ReplicatedUsing = OnRep_Host )
	TObjectPtr< AActor > mHostActor = nullptr;

	UPROPERTY( SaveGame, ReplicatedUsing = OnRep_Host )
	uint8 mHostTerminalIndex = 0;

	UFUNCTION()
	void OnRep_Host();

	UPROPERTY( SaveGame )
	TArray< FACPRSavedCoupling > mSavedCouplings;

	/** Set by PostLoadGame. Invariant 6's switch, with mSavedOnce. */
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
};
