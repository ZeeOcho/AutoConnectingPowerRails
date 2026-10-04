// Auto-Connecting Power Rails — §10's blueprint open-connection manager: generated bridge Rails.
//
// §10 is implemented on vanilla's blueprint open-connection machinery. The manager is registered on every
// AFGBlueprintHologram, finds the blueprint's boundary terminals and predicts §10's verdicts. Vanilla drives
// it as follows:
//
//   * Default / Blueprint modes drive nothing here — no update, no snap query, no Construct. Flush
//     boundaries couple anyway, through §6.1 at each placed host's Tick+1.
//   * The Auto-Connect modes call UpdateAutomaticConnections every frame, AttemptConnectionStateSnap once
//     per click, HandleBuildableConnectionRemapping once per placed buildable (index = Initialize index),
//     and Construct once, after every spawn and remap and before any placed buildable's BeginPlay.
//
// Per boundary terminal (§10.3's "BP candidate"):
//
//   1. DISCOVERY (§10.4). The coincident test first, then the ray: every world terminal and every other
//      blueprint terminal occludes a disc of `face width - 0.1 m` about its snap point; the smallest t at
//      which the ray enters a disc is the first hit; a tie is ambiguous; a first hit that is not an open,
//      opposing world terminal in the placement's space blocks; nothing ever falls through.
//   2. VALIDATION. A separated pair must be buildable by hand: the far end must land on the target's own
//      cell, both ends must satisfy §6.1's integer opposition, the length must be inside the Rail's 1-40 m,
//      and the child Rail hologram's own CheckValidPlacement must pass (invariant 10).
//   3. PREVIEW. A valid proposal is shown as a real Rail hologram, a child of the blueprint hologram
//      (AFGHologram::SpawnChildHologramFromRecipe, FGHologram.h:98), priced from its exact length. An
//      invalid proposal before the first click is hidden, exactly as vanilla's template hides its own.
//   4. LATCH (§10.2). The first click latches every valid proposal's OB terminal. From then on the
//      bridge follows the blueprint's geometry and never retargets; if it stops being buildable it stays
//      visible, turns red and blocks the whole placement.
//   5. CONSTRUCT (§10.6). The second click builds each latched bridge through its own hologram's Construct,
//      from the PLACED buildables' transforms, and hands it to vanilla in out_ConstructedBridgeBuildables.
//      §6.1 couples both ends a tick later, exactly as it couples a hand-built Rail.
//
// Coincident (flush) pairs are reported and NOT latched: coincidence couples in every mode by §6.1, so a
// click spent latching it would be a click that visibly does nothing.
//
// Derived from the base, not the template, for Vertical Conveyor Auto-Connect's reason: the template's
// UpdateAutomaticConnections is `override final` and expects UFGConnectionComponent-keyed candidates from
// the overlap broadphase, while §10.4 is a ray over the whole terminal registry.

#pragma once

#include "CoreMinimal.h"
#include "ACPRSpec.h"
#include "ACPRSpatialGrid.h"
#include "ACPRTerminalRegistry.h"
#include "FGFactoryColoringTypes.h"
#include "Hologram/FGBlueprintOpenConnectionManager.h"

class AFGBuildable;
class AFGBlueprintHologram;
class AFGHologram;
class AFGBuildableBlueprintDesigner;
class AACPRRail;
class AACPRRailHologram;
class UACPRTerminalComponent;
class UFGRecipe;
class UFGConnectionComponent;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

class FACPRBlueprintTerminalManager final : public FGBlueprintOpenConnectionManagerBase
{
public:
	explicit FACPRBlueprintTerminalManager( AFGBlueprintHologram* blueprintHologram );
	virtual ~FACPRBlueprintTerminalManager() override;

	// Begin FGBlueprintOpenConnectionManagerBase — every pure virtual, FGBlueprintOpenConnectionManager.h:34-54
	virtual void RegisterNearbyActor( AActor* actor ) override;
	virtual void UnregisterNearbyActor( AActor* actor ) override;
	virtual void UpdateAutomaticConnections( const FHitResult& hitResult, bool& out_PlaySnapEffects ) override;
	virtual void HandleBuildableConnectionRemapping( AFGBuildable* buildable, int32 blueprintBuildableIndex ) override;
	virtual void Initialize( TArray< AFGBuildable* > buildables ) override;

	/**
	 * Diagnostic (Verbose only): what vanilla's DuplicateMeshComponentsFromBuildableArray made for our
	 * Caps and one Rail — mesh, rotation, scale under each buildable's new root — logged at Initialize
	 * and again at the first update, so "copied from the fitted instance", "copied from the CDO" and
	 * "made by something else" can be told apart from the log alone.
	 */
	void DumpDuplicatedMeshes( const TCHAR* when ) const;

	/**
	 * Vanilla's duplicates are template copies — identity transform, unit scale, and the blueprint's
	 * authored mesh — not the blueprint-world instance after its fit. This walks every ACPR buildable's
	 * duplicates by component name and gives each the original's mesh, relative transform and
	 * visibility, keeping the hologram material vanilla put on it. Runs once, after Initialize's dump,
	 * before the first frame is drawn.
	 */
	void FixUpDuplicatedMeshes();
	virtual bool AttemptConnectionStateSnap() override;
	virtual bool CanSnapConnectionStates() const override;
	virtual void Construct( TArray< AFGBuildable* >& out_ConstructedBridgeBuildables, FNetConstructionID netConstructionID ) override;
	virtual void SerializeConstructMessage( FArchive& ar, FNetConstructionID id ) override;
	virtual void PostConstructMessageDeserialization() override;
	virtual void ResetAutomaticConnections() override;
	// End FGBlueprintOpenConnectionManagerBase

	/**
	 * One second after a bridge is built: did both ends couple, did vanilla put it in the placed
	 * blueprint's dismantle group, did it inherit the BP side's customization. Static and value-captured,
	 * because the manager is destroyed with its hologram right after Construct.
	 */
	struct FBridgeAudit
	{
		FString Hologram;
		int32 State = INDEX_NONE;
		TWeakObjectPtr< AACPRRail > Rail;
		TWeakObjectPtr< AFGBuildable > BPHost;
		uint8 BPTerminalIndex = 0;
		TWeakObjectPtr< UACPRTerminalComponent > Target;
		double Length = 0.0;
		FFactoryCustomizationData Customization;
	};

	static void RunBridgeAudit( const FBridgeAudit& audit );

	/**
	 * The blueprint hologram's build mode changed — called from the module's after-hook on
	 * AFGBlueprintHologram::OnBuildModeChanged, for whichever manager is registered on that hologram.
	 * Vanilla tells its managers nothing here, so bridges shown on leaving auto-connect would otherwise
	 * stay on screen. The manager's answer is mode-agnostic:
	 * every mode change resets the picture — bridges down, latches released, icons cleared, movement
	 * gate opened — and the next UpdateAutomaticConnections, which vanilla sends only in an
	 * auto-connect mode, rebuilds it from scratch on the very next frame.
	 */
	static void OnBuildModeChanged( const class AFGBlueprintHologram* hologram,
	                                TSubclassOf< class UFGHologramBuildModeDescriptor > buildMode );

private:
	/**
	 * Every terminal of every terminal host in the blueprint. §10.3: "terminals inside the hologram occlude
	 * other BP rays but can never be targets", so the occluder set is all of them, open or not.
	 */
	struct FBPTerminal
	{
		/** The blueprint-world buildable. Weak: the blueprint world owns it. */
		TWeakObjectPtr< AFGBuildable > Buildable;
		int32 BuildableIndex = INDEX_NONE;
		uint8 TerminalIndex = 0;

		/** Relative to the buildable, from IACPRTerminalHost::GetTerminalLocalFrame. */
		FTransform LocalFrame;

		/**
		 * The point §6.1 derives this terminal's quantized outward from, in the buildable's frame: the
		 * Rail's OTHER end (ACPRRail.cpp: Reduce( cellA - cellB )), or the Junction's centre
		 * (ACPRJunction.cpp: Reduce( face - centre )). Predicting the outward the placed host WILL get,
		 * by the placed host's own rule, is what makes §10.4's opposition an exact integer test.
		 */
		FVector ReferenceLocal = FVector::ZeroVector;
		bool HasReference = false;

		/** §10.4 / §11: "face width - 0.1 m" as a radius — 20 uu on a Rail end, 45 on a Junction face. */
		double DiscRadius = 20.0;

		/** Index into mStates when this terminal is a boundary candidate; INDEX_NONE for occluder-only. */
		int32 State = INDEX_NONE;
	};

	/** One terminal's frame in the world, now: the preview's, or the placed buildable's at Construct. */
	struct FTerminalFrame
	{
		FVector P = FVector::ZeroVector;
		FVector D = FVector::ForwardVector;
		FVector Up = FVector::UpVector;
		FIntVector PQ = FIntVector( 0, 0, 0 );
		FIntVector OutQ = FIntVector( 0, 0, 0 );
	};

	enum class EVerdict : uint8 { None, Direct, Bridge, Blocked };

	/**
	 * The verdict is data, not a string: the reason is an enum with the few numbers a line needs, and
	 * change detection compares those. The string exists only in DescribeVerdict, called only when a
	 * line is actually printed, so discovery builds no FStrings per candidate per evaluation.
	 */
	enum class EReason : uint8
	{
		None,                 // not evaluated yet
		NoPreviewRoot,
		NoRegistry,
		Direct,               // §6.1 coincident pair in the cell (Target)
		AmbiguousCoincident,  // Aux = how many
		CoincidentRefused,    // Blocker = the refusing terminal
		NoneInRange,          // Aux = examined
		AmbiguousRay,         // Aux = how many at T
		BlockedByBPTerminal,  // Aux = BP terminal index, T
		BlockedSpace,         // Blocker, T
		BlockedNotOpen,       // Blocker, T
		PairFault,            // Blocker, T, Fault
		Bridge,               // Target, Length
		Conflict,             // Aux = winning state (§10.5)
		NoBridgeHologram,
		FailsValidation,      // BridgeDisqualifiers
		LatchedCoincident,    // Target
		LatchedValid,         // Target, Length
		LatchedInvalid,       // Target, Fault
	};

	/** EvaluatePair's answer and FollowLatched's extra checks. */
	enum class EPairFault : uint8
	{
		None,
		TargetGone,
		NotAhead,             // AuxValue = along
		FarEndMisses,         // AuxCell = far end's cell, AuxValue = off-axis distance
		TargetNotOpposing,    // AuxCell = target outward
		BPNotOpposing,        // AuxCell = BP outward
		TooShort,             // AuxValue = length
		TooLong,
		TargetNotOpen,
		OtherSpace,
		RayLost,
		Occluded,             // Blocker or Aux (BP terminal), AuxValue = T
	};

	/** One BP candidate: its discovery, its latch and its bridge hologram. */
	struct FState
	{
		int32 Terminal = INDEX_NONE;

		EVerdict Kind = EVerdict::None;
		EReason Reason = EReason::None;
		EPairFault Fault = EPairFault::None;
		int32 Aux = 0;
		double T = 0.0;
		double AuxValue = 0.0;
		FIntVector AuxCell = FIntVector( 0, 0, 0 );
		TWeakObjectPtr< const UACPRTerminalComponent > Blocker;

		/** The discovered (or latched) OB terminal, and the exact length a bridge to it would have. */
		TWeakObjectPtr< UACPRTerminalComponent > Target;
		double Length = 0.0;

		/** §10.2 step 2. */
		bool Latched = false;
		TWeakObjectPtr< UACPRTerminalComponent > LatchedTarget;

		TWeakObjectPtr< AACPRRailHologram > Bridge;
		bool BridgeEnabled = false;
		bool BridgeValid = false;
		FString BridgeDisqualifiers;

		/** Change detection for the log: the last verdict printed, as data. */
		EReason LoggedReason = EReason::None;
		EPairFault LoggedFault = EPairFault::None;
		TWeakObjectPtr< UACPRTerminalComponent > LoggedTarget;
		int32 LoggedAux = -1;
		bool LoggedBridgeSet = false;
		bool LoggedLatched = false, LoggedEnabled = false, LoggedValid = false;

		/** §10.6's icon through vanilla: what was last broadcast, so a broadcast happens only on a change. */
		bool BroadcastValid = false;
		TWeakObjectPtr< UACPRTerminalComponent > BroadcastTarget;

		bool IsFaulted() const { return Fault != EPairFault::None; }
	};

	/** The verdict as one line, for the log only. */
	FString DescribeVerdict( const FState& state ) const;
	static const TCHAR* DescribePairFault( EPairFault fault );

	/** Where terminal `t` is right now. `placed` reads the constructed buildable instead of the preview. */
	bool ComputeFrame( const FBPTerminal& t, bool placed, FTerminalFrame& out_frame ) const;

	/** The world transform of `t`'s buildable: preview root, or the placed actor. */
	bool GetBuildableTransform( const FBPTerminal& t, bool placed, FTransform& out_transform ) const;

	/** §10.4 steps 1-7: the ray, returning every terminal at the first-hit distance. */
	struct FRayHit
	{
		UACPRTerminalComponent* World = nullptr;
		int32 BPTerminal = INDEX_NONE;
		double T = 0.0;
	};
	void CastRay( const FTerminalFrame& frame, int32 sourceTerminal, bool placed,
	              TArray< FRayHit >& out_first, int32& out_examined ) const;

	// -------------------------------------------------------------------------------------------------
	// The occluders are a rigid body. A blueprint's buildables never move relative to each other — not
	// in the preview (every root is a child of the hologram), not once placed. So every blueprint
	// terminal's frame is fixed in the blueprint's own space, and the only thing that changes per
	// evaluation is where that space stands in the world. The local frames and a grid over them are
	// built once at Initialize, and each ray is carried into the body's space (one transform) and asks
	// the grid for the few occluders along it, instead of every evaluation recomputing every occluder's
	// world frame and every ray testing all of them (thousands of disc tests per ray on a large blueprint).
	//
	// "Body space" is the space of the first buildable's preview root at Initialize; nothing depends on
	// which. RefreshBody derives where the body stands now from any one buildable — the preview root,
	// or at Construct the placed actor — and, once per mode, checks every other buildable against that
	// (a Warning names any that disagree by more than 1 uu, which would mean the body is not rigid).
	// -------------------------------------------------------------------------------------------------

	/** One blueprint terminal in body space: position and outward axis. Indexed like mBPTerminals. */
	struct FOccluderLocal
	{
		FVector P = FVector::ZeroVector;
		FVector D = FVector::ForwardVector;
	};
	TArray< FOccluderLocal > mOccluderLocal;

	/** Blueprint index -> the buildable's transform in body space, for every buildable that had a preview root. */
	TMap< int32, FTransform > mBuildableLocal;

	/** Occluder indices by body-space position. */
	TACPRSpatialGrid< int32, ACPR_REGISTRY_BUCKET > mOccluderGrid;

	/** Builds the three above from the preview roots. Initialize, after mBPTerminals. */
	void BuildOccluderIndex();

	/**
	 * Where the body stands now. `placed` reads the constructed actors (Construct) instead of the
	 * preview. False, with nothing to cast against, when no buildable's transform is available.
	 */
	bool RefreshBody( bool placed ) const;
	mutable FTransform mBodyToWorld;
	mutable bool mBodyValid = false;
	mutable bool mBodyPlaced = false;

	/** Which modes the rigidity check has run for: bit 1 preview, bit 2 placed. */
	mutable uint8 mBodyChecked = 0;

	/**
	 * §10.4 steps 9-10 for one (source, target) pair: where the far end lands, whether it lands on the
	 * target's cell, whether both ends oppose by §6.1's integer rule, whether the length is a Rail's.
	 * Returns the fault, or an empty string when the pair is buildable.
	 */
	EPairFault EvaluatePair( const FTerminalFrame& frame, const UACPRTerminalComponent* target, double& out_length,
	                         FIntVector& out_cell, double& out_value ) const;

	/** The whole of §10.4 for an unlatched state. */
	void Discover( FState& state, const FTerminalFrame& frame, bool placed );

	/** §10.2 step 3 for a latched state: same target, new geometry, red if it no longer holds. */
	void FollowLatched( FState& state, const FTerminalFrame& frame, bool placed );

	/** Discovery for every state, §10.5's conflicts, then every bridge hologram. Returns tallies. */
	void EvaluateAll( const TCHAR* why, bool force, bool& out_playSnap );

	/** Spawns (once) and returns state `index`'s bridge hologram. */
	AACPRRailHologram* EnsureBridge( int32 index, FState& state, const FTerminalFrame& frame );

	/** Configures, validates and enables/disables one state's bridge. */
	void UpdateBridge( int32 index, FState& state, const FTerminalFrame& frame, bool& out_playSnap );

	void DisableBridge( FState& state );

	/**
	 * Vanilla's SetDisabled, called only when the answer changes, timed and sampled against the
	 * parent's child count. A bridge is active (an enabled child: priced, constructed, validated by vanilla)
	 * only while latched; an unlatched proposal is shown with the hide toggle and stays disabled.
	 */
	void SetBridgeActive( FState& state, AACPRRailHologram* bridge, bool active );

	/** One [ACPR-BRIDGE-SCALE] line per evaluation that spawned, enabled or disabled anything. */
	void LogBridgeScaling();
	TArray< TPair< int32, double > > mSpawnSamples;
	TArray< TPair< int32, double > > mShowSamples;
	TArray< TPair< int32, double > > mDisableSamples;
	int32 mRevealsReasserted = 0;

	/** Resolves the Rail recipe and its build-class limits once. */
	void ResolveRailRecipe( const TArray< AFGBuildable* >& buildables );

	/** The placement's build space (§10.1): the blueprint hologram's own mBlueprintDesigner. */
	AFGBuildableBlueprintDesigner* GetPlacementSpace() const;

	/** "2xDesc_SteelBeam 2xDesc_Cable". */
	static FString DescribeCost( const TArray< struct FItemAmount >& cost );

	/** The blueprint hologram's disqualifier list, read through the AccessTransformers Friend. */
	FString DescribeParentDisqualifiers() const;

	FString DescribeTerminal( int32 terminal ) const;
	static FString DescribeWorld( const UACPRTerminalComponent* terminal );
	static FString DescribeCell( const FIntVector& cell );

	void LogStateLine( int32 index, const FState& state, const TCHAR* why, bool display, bool proposal = false );

	// -------------------------------------------------------------------------------------------------
	// §10.6 "Every accepted pair shows the vanilla blue two-link icon."
	//
	// Vanilla draws it with FHologramHelpers::CreateAutomaticBlueprintConnectionRepresentation
	// (HologramHelpers.h:33) — a plain export on a struct, which is a load risk — so the icon is built
	// here from the same two assets instead: UFGFactorySettings::mBlueprintAutoConnectionMesh
	// (FGFactorySettings.h:158) and ::mDefaultAutomaticBlueprintConnectionMaterial (:102), public
	// UPROPERTY fields on the settings CDO that UFGGlobalSettings::GetFactorySettingsCDO (FGGlobalSettings.h:19,
	// static UFUNCTION) returns. One plain UStaticMeshComponent per BP candidate, on the blueprint hologram,
	// created as runtime components on the hologram and never destroyed — hidden instead.
	// -------------------------------------------------------------------------------------------------

	/**
	 * §10.6's icon — `acpr.LinkIconMode`: off, vanilla's own icon, or our own mesh.
	 *
	 * Vanilla's icon: registering a manager subscribes the blueprint hologram to that manager's
	 * `mOnConnectionStateChanged` (FGBlueprintHologram.h:186), and vanilla's manager template fires it whenever a
	 * pair's target or validity changes (FGBlueprintOpenConnectionManager.h:492, :637); the hologram's handler
	 * then draws and hides the blue two-link icon. This manager derives from the base rather than that template,
	 * so it fires the delegate itself: no new import (a delegate the base already owns), and vanilla's exact visual.
	 *
	 * Which component the handler wants is answered by Vertical Conveyor Auto-Connect, the shipping mod that
	 * shares this seam (its DEVELOPMENT.md, "Automatic-connection representation"): the handler replaces the
	 * ordinary connection-direction indicator with the automatic-link one, so the component to broadcast is the
	 * hologram's own duplicate from `mDuplicateConnectionToOriginalMap` — preferring one that
	 * `mConnectionRepresentationMeshes` has an indicator for — and "do not create a separate mod-owned icon".
	 *
	 * One difference: the hologram duplicates the connection types it knows, and a Power Rail terminal is a
	 * circuit connection, which it duplicates none of. Broadcasting the blueprint-world component makes vanilla
	 * draw the icon in FactoryBlueprintWorld, where nobody can see it. So where no duplicate exists,
	 * GetBroadcastConnection makes one with vanilla's own component duplication — the function vanilla runs
	 * for its own types — and broadcasts that. Mode 2 keeps our own mesh as a fallback.
	 */
	enum class EIconMode : uint8 { Off = 0, Vanilla = 1, Own = 2 };
	static EIconMode CurrentIconMode();

	/** The component to broadcast for state `index`: the hologram's own stand-in, else the blueprint-world connection. */
	UFGConnectionComponent* GetBroadcastConnection( int32 index, const FState& state );

	/** Fires mOnConnectionStateChanged exactly as vanilla's template does, on change only. */
	void BroadcastConnectionState( int32 index, FState& state );

	/** What vanilla made of it: its icon component for this connection, where it is and whether it is visible. */
	void LogVanillaIcon( int32 index, const UFGConnectionComponent* connection, bool accepted );

	/** The preview stand-ins (copies of the host's power connection at each terminal), indexed like mStates. */
	TArray< TWeakObjectPtr< class UACPRPowerConnectionComponent > > mBroadcastDuplicates;

	int32 mBroadcastLogBudget = 40;
	int32 mBroadcasts = 0;
	int32 mDuplicatesMade = 0;

	/** Reads the two assets once. False (and one Warning) when either is missing: the icon is then off. */
	bool ResolveIconAssets();

	/** State `index`'s icon component, created on first use. */
	UStaticMeshComponent* EnsureIcon( int32 index );

	/** Shows the icon at the BP terminal for an accepted pair (Direct, or a shown valid bridge); hides it otherwise. */
	void UpdateIcon( int32 index, const FState& state, const FTerminalFrame* frame );

	void HideAllIcons();

	/** Indexed like mStates. Weak: the hologram owns the components (attached to its root). */
	TArray< TWeakObjectPtr< UStaticMeshComponent > > mIcons;

	TWeakObjectPtr< UStaticMesh > mIconMesh;
	TWeakObjectPtr< UMaterialInterface > mIconMaterial;
	bool mIconAssetsResolved = false;
	bool mIconAssetsOk = false;

	int32 mIconsCreated = 0;
	int32 mIconShows = 0;
	int32 mIconMaterialReapplied = 0;
	int32 mIconLogBudget = 24;

	TArray< FBPTerminal > mBPTerminals;
	TArray< FState > mStates;
	TArray< TWeakObjectPtr< AActor > > mNearby;

	/** HandleBuildableConnectionRemapping's answer: blueprint index -> placed buildable. */
	TMap< int32, TWeakObjectPtr< AFGBuildable > > mPlaced;

	/** What a client's construct message carried, applied in PostConstructMessageDeserialization. */
	struct FDeserializedLatch
	{
		int32 Terminal = INDEX_NONE;
		TWeakObjectPtr< UACPRTerminalComponent > Target;
		double Length = 0.0;
	};
	TArray< FDeserializedLatch > mDeserializedLatches;

	/** The Rail recipe a bridge is spawned from, and the build class's §11 limits. */
	TSubclassOf< UFGRecipe > mRailRecipe;
	FString mRailRecipeSource;
	double mRailMin = 100.0;
	double mRailMax = ACPRSpec::RailMaxLength;
	double mRailPerCost = 1000.0;

	FVector mLastEvaluatedLocation = FVector::ZeroVector;
	FQuat mLastEvaluatedRotation = FQuat::Identity;
	bool mHasEvaluated = false;

	/**
	 * Whether vanilla is currently driving this manager. Vanilla calls UpdateAutomaticConnections every
	 * frame in an auto-connect build mode and ResetAutomaticConnections when it leaves one, and nothing
	 * else: true on every update, false on every reset. The record of vanilla's drive, for the log.
	 */
	bool mDriven = false;

	FString mHologramName;

	int32 mStateLogBudget = 60;
	int32 mProposalLogBudget = 30;
	int32 mConstructLogBudget = 40;
	int32 mNearbyLogBudget = 4;
	int32 mInitLogBudget = 40;
	int32 mRemapLogBudget = 40;
	int32 mConflictLogBudget = 10;
	int32 mSpawnFailLogBudget = 4;

	/** Entry-point counts, printed by the destructor. */
	int32 mRemapCount = 0;
	int32 mUpdateCount = 0;
	int32 mEvaluationCount = 0;
	int32 mConstructCount = 0;
	int32 mResetCount = 0;
	int32 mSnapQueryCount = 0;
	int32 mBridgesSpawned = 0;
	int32 mLatchCount = 0;
	int32 mBlockedClicks = 0;
	int32 mBridgesBuilt = 0;

	/** Which caller actually caused each evaluation ("update", "click"), for the destructor line. */
	TMap< FString, int32 > mEvaluationsBy;

	/** Vanilla UpdateAutomaticConnections calls that arrived while the hologram was locked. */
	int32 mUpdatesWhileLocked = 0;

	/** Evaluations while locked are the nudge case; one Display line each, this many. */
	int32 mLockedLogBudget = 40;

	/** The registry key, kept so the destructor removes exactly what the constructor added. */
	const AFGHologram* mRegistryKey = nullptr;
};
