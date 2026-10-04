// Auto-Connecting Power Rails — what it means to own terminals.
//
// §6.1's Couplings are between TERMINALS, not between Rails. Anything that owns terminals
// implements IACPRTerminalHost, and FACPRCoupling does the resolving for all of them:
//
//   * a Rail owns two, at its ends (§4);
//   * a Junction owns six, on the faces of its 1 m cube, all electrically common (§7);
//   * an Outlet owns a private one-sided mounting interface (§4);
//   * a Cap occupies one rather than owning any (§9).
//
// The interface deliberately asks for very little: how many terminals, which one is at index N,
// which index a given terminal is, and somewhere to record a Coupling. Everything else — the
// coincidence test, the opposing test, invariant 8's ambiguity rule, the saved replay — belongs to
// the resolver, so there is one implementation of §6.1 rather than one per buildable.

#pragma once

#include "CoreMinimal.h"
// FHitResult, for FACPRTerminalPicker below. Engine/HitResult.h rather than Engine/EngineTypes.h:
// it is where UE 5.x actually declares the type, and FactoryGame's own headers use that exact path
// (FGUseableInterface.h, FGHealthComponent.h), so it is verified present rather than assumed.
#include "Engine/HitResult.h"
#include "UObject/Interface.h"
#include "ACPRTerminalHost.generated.h"

class UACPRTerminalComponent;
class UACPRPowerConnectionComponent;
class AFGBuildableBlueprintDesigner;
struct FACPRSpaceFilter;
struct FACPRHighlight;

/**
 * One end of one Coupling, as saved state on the host that owns that end.
 *
 * Terminals are identified by (actor, index) rather than by component pointer. That is partly
 * because it is what survives a save cleanly, and partly because it is the form §10.6's server
 * revalidation and B.3's "explicit actor remapping" want.
 *
 * Both participants record their own half, which makes the record symmetric: a load can restore the
 * edge from either side with no ordering rule, and never by inferring it from geometry (invariant 6).
 */
USTRUCT()
struct FACPRSavedCoupling
{
	GENERATED_BODY()

	/** The other buildable. Any terminal host — Rail, Junction, Outlet. */
	UPROPERTY( SaveGame )
	TObjectPtr< AActor > OtherActor = nullptr;

	/** Which of OUR terminals, in our own indexing. */
	UPROPERTY( SaveGame )
	uint8 LocalTerminalIndex = 0;

	/** Which of THEIRS, in theirs. */
	UPROPERTY( SaveGame )
	uint8 OtherTerminalIndex = 0;
};

UINTERFACE( MinimalAPI, NotBlueprintable )
class UACPRTerminalHost : public UInterface
{
	GENERATED_BODY()
};

class AUTOCONNECTINGPOWERRAILS_API IACPRTerminalHost
{
	GENERATED_BODY()

public:
	/** Two for a Rail, six for a Junction. */
	virtual int32 GetTerminalCount() const = 0;

	/** Null for an out-of-range index rather than an assert — indices come from saved data. */
	virtual UACPRTerminalComponent* GetTerminalAtIndex( uint8 index ) const = 0;

	/** INDEX_NONE if the terminal is not ours. */
	virtual int32 GetIndexOfTerminal( const UACPRTerminalComponent* terminal ) const;

	/** Adds a record if an identical one is not already present. */
	virtual void AddSavedCoupling( const FACPRSavedCoupling& coupling ) = 0;

	/** Read access for the replay on load. */
	virtual const TArray< FACPRSavedCoupling >& GetSavedCouplings() const = 0;

	/**
	 * Write access, for §6.3's "conservative save repair may remove an invalid edge before it is
	 * player-visible".
	 *
	 * Without it a record naming a dismantled buildable is logged as unrestorable on every single
	 * load, forever — a permanent warning about something already handled, which is the kind of
	 * noise that trains people to ignore warnings.
	 */
	virtual TArray< FACPRSavedCoupling >& GetMutableSavedCouplings() = 0;

	/**
	 * The host's one power connection. A Coupling is a hidden edge between two hosts'
	 * connections; the Outlet's is its wire socket. Never null on a host that implements this.
	 */
	virtual UACPRPowerConnectionComponent* GetPowerConnection() const = 0;

	/**
	 * Whether the network this host is on carries power — the Powered cue. On the authority it is the
	 * connection's own HasPower(); on a client that bit is not replicated by vanilla
	 * (FGPowerConnectionComponent.h:79), so the host replicates it itself, as the pole does
	 * (FGBuildablePowerPole.h:110).
	 */
	virtual bool HasNetworkPower() const = 0;

	/**
	 * The indicator event. Called by a terminal whose state just changed
	 * (SetCoupledTo, SetCapped, a client's replicated state), by the host's own power delegate handler
	 * and by the highlight. `index` is the terminal, or INDEX_NONE for "the host as a whole" (power).
	 * The host pushes its indicators and, on the authority, updates its replicated state.
	 */
	virtual void OnTerminalStateChanged( int32 index ) {}

	/**
	 * True when this host was loaded from a save rather than just built — invariant 6's switch.
	 * Answered from DATA, not from call order. A host that has ever been saved carries a
	 * SaveGame flag written in PreSaveGame; PostLoadGame's flag is the other witness. Either is
	 * enough, so BeginPlay can couple at once without knowing whether PostLoadGame ran before it.
	 */
	virtual bool WasLoadedFromSave() const = 0;

	/**
	 * True when this host was placed as part of a blueprint — and then WasLoadedFromSave is true too.
	 *
	 * Every placed blueprint buildable arrives with PostLoadGame already run, because a blueprint IS a
	 * small save file. Its internal records are remapped onto the placed copies, so the replay restores
	 * the inside of the blueprint correctly — and without this flag the host, being "loaded", would
	 * never ask §6.1 about its boundary terminals.
	 *
	 * Placing a blueprint is construction, not a load: "coincidence couples however it is achieved"
	 * applies to its boundary exactly as to a hand-placed Rail. So a placed host replays its
	 * records AND then asks §6.1 for every terminal still open. Invariant 6 is untouched for real save
	 * loads, which never call PostSerializedFromBlueprint.
	 *
	 * A default rather than pure, because only hosts that can be blueprinted need to answer.
	 */
	virtual bool WasPlacedFromBlueprint() const { return false; }

	/**
	 * §12's highlight, written by whichever hologram is aiming at this host and read
	 * by FACPRIndicators::Push. Defaults do nothing, so a host without cue surfaces can ignore it;
	 * the Rail and the Junction store it and re-push their indicators at once when it changes.
	 */
	virtual void SetHighlight( const FACPRHighlight& highlight ) {}
	virtual const FACPRHighlight* GetHighlight() const { return nullptr; }

	/**
	 * What would be dismantled along with this host — §8.3's and §9's dependent-dismantle rule.
	 *
	 * §9: "Dismantling the owning Rail or Junction dismantles the Cap with a full refund and visible
	 * dependency preview." §8.3 says the same for Outlets. Vanilla's hook for that is
	 * AFGBuildable::GetChildDismantleActors_Implementation (FGBuildable.h:303), and it needs a list.
	 *
	 * Runtime state, rebuilt by the attachments themselves, and that direction is the point. The Cap
	 * and the Outlet already save which host they are on, because that is what they need in order to
	 * function; a second saved list on the host would be the same fact written twice, and invariant
	 * 6's argument against inferring Couplings from geometry applies just as well to inferring them
	 * from a list that can disagree with its counterpart. So each attachment registers itself at
	 * BeginPlay and unregisters at EndPlay, and the host merely holds the bag.
	 *
	 * Null for a host that can carry no attachments — §8's Outlet owns a terminal but nothing mounts
	 * on it — which is why this hands back a pointer rather than a reference.
	 */
	// Const, because AFGBuildable::GetChildDismantleActors_Implementation is const and that is the
	// only caller that matters. The list it hands back is mutable on the implementor, so Collect can
	// prune dead entries while walking it.
	virtual TArray< TWeakObjectPtr< AActor > >* GetAttachmentList() const { return nullptr; }

	/**
	 * §10.1's build space: the Blueprint Designer this host was built in, or null for the world.
	 *
	 * Pure virtual, and every implementation is one line returning AFGBuildable::mBlueprintDesigner
	 * (FGBuildable.h:1014, protected). Each host reads its OWN protected field, which a free function
	 * could not; the vanilla getter AFGBuildable::GetBlueprintDesigner (FGBuildable.h:530) would work
	 * too, but it is a plain export and this is read in every coupling test. See ACPRDesignerSpace.h.
	 */
	virtual class AFGBuildableBlueprintDesigner* GetHostDesigner() const = 0;

	/**
	 * Where terminal `index` sits relative to the actor, computed from saved state alone.
	 *
	 * §10's blueprint manager needs terminal positions for actors that live in the blueprint's own
	 * world, where nothing guarantees BeginPlay has placed the components (the blueprint keeps live
	 * buildables in a separate world and the hologram only copies their meshes). So this
	 * restates each host's placement rule from the fields it saves — the Rail's length, the Junction's
	 * half extent — rather than reading the component transform.
	 *
	 * False for a terminal that is never a blueprint boundary candidate: §4's private mounting
	 * interface is "not a blueprint candidate", so the Outlet answers false.
	 */
	virtual bool GetTerminalLocalFrame( uint8 index, FTransform& out_frame ) const = 0;
};

/**
 * Register/unregister helpers, so an attachment does not have to know what kind of host it is on.
 *
 * Free functions for the same reason FACPRCoupling's are: they need the AActor as well as the
 * interface, and a C++ interface in Unreal cannot reach its own actor without a cast the caller
 * already has.
 */
struct AUTOCONNECTINGPOWERRAILS_API FACPRAttachments
{
	/** No-op when the host is null, is not a terminal host, or carries no attachment list. */
	static void Register( AActor* hostActor, AActor* attachment );
	static void Unregister( AActor* hostActor, AActor* attachment );

	/** Appends every live attachment, pruning dead weak pointers as it goes. */
	static void Collect( AActor* hostActor, TArray< AActor* >& out_actors );
};

/**
 * The indicators — one material per state, swapped per slot. No custom data of ours is written.
 *
 * Custom data is the wrong channel for a readout. Index 6 (CDI_HasPower) is owned by vanilla's
 * ApplyHasPowerCustomData and reads 1.0 whatever is written to it; with bUsePrimitiveCustomData on,
 * the array master colours the cue from the PRIMARY PAINT floats, so every strip takes the swatch
 * and none of them moves with state; and re-applying resolved data on a timer wipes vanilla's
 * swatch PREVIEW off the aimed buildable.
 *
 * So the cue slots carry one of four fixed instances — Open (amber), Coupled (dim power colour),
 * Powered (power colour), Capped (unlit) — and Push does nothing but SetMaterial on the slot whose
 * terminal changed. The Junction's six face slots make per-face state a slot index; the Rail's
 * body groove is a network summary; each collar is its own terminal. The accent keeps the proxy's
 * custom data — that IS the swatch channel and it works.
 *
 * ConfigureProxy: what every ACPR proxy needs — instancing BLOCKED, so it keeps rendering as the
 * plain component it replaces, and 20 custom data floats (BUILDABLE_CUSTOM_DATA_NUM).
 */
enum class EACPRIndicator : uint8 { Open, Coupled, Powered, Capped, Selected };
static constexpr int32 ACPR_INDICATOR_STATES = 5;

/**
 * §12's highlight, as one more state of the same cue.
 *
 * A hologram aiming at a host writes this onto the host: which terminal is the exact target. Push
 * then lights that OPEN terminal's own cue Selected instead of Open — the collar's groove, the
 * Junction's face strips, the surfaces the player already reads for state — and nothing is added
 * to the scene. Only the target: lighting every open terminal of the aimed host the moment the
 * crosshair touches it reads as "this will couple", and it will not. It is written every frame the
 * aim holds and cleared by the hologram that wrote it — in its Destroyed() as well as on aim-away,
 * so no time-to-live is needed to cover a hologram vanishing.
 */
struct FACPRHighlight
{
	/**
	 * Bit i = terminal i is the exact target. A MASK, not an index, because a Rail in its second step
	 * has two: the anchor it is growing from and the far end it is aiming at. Six terminals
	 * is the most any host has.
	 */
	uint8 Selected = 0;

	bool IsSet() const { return Selected != 0; }
	bool operator==( const FACPRHighlight& other ) const { return Selected == other.Selected; }
	bool operator!=( const FACPRHighlight& other ) const { return !( *this == other ); }
};

/**
 * Material slots by name. acpr_meshes.py compacts every mesh's material IDs to the
 * ones it actually uses and NAMES the slots: "Body", "Cue" (the Junction: "Cue_+X" … "Cue_-Z"),
 * "Accent", "Power" (the Junction: "Power_+X" … "Power_-Z"). acpr_materials.py fills them by name;
 * the C++ addresses them by name through UStaticMesh::GetMaterialIndex. Nobody restates a number,
 * so nothing can drift: a Rail collar has four slots, a Junction fourteen, and a name that a mesh
 * does not carry is a Warning naming the mesh, once (the script was not re-run).
 */
namespace ACPRSlot
{
	// Function-local statics rather than namespace-scope FNames: the name table must exist first.
	inline const FName& Body()   { static const FName n( TEXT( "Body" ) );   return n; }
	inline const FName& Cue()    { static const FName n( TEXT( "Cue" ) );    return n; }
	inline const FName& Accent() { static const FName n( TEXT( "Accent" ) ); return n; }
	inline const FName& Power()  { static const FName n( TEXT( "Power" ) );  return n; }
	/** The Junction's per-face cue and floor, in kTerminalLayout order: +X, -X, +Y, -Y, +Z, -Z. */
	inline const FName& CueFace( int32 face )
	{
		static const FName n[ 6 ] = {
			FName( TEXT( "Cue_+X" ) ), FName( TEXT( "Cue_-X" ) ), FName( TEXT( "Cue_+Y" ) ),
			FName( TEXT( "Cue_-Y" ) ), FName( TEXT( "Cue_+Z" ) ), FName( TEXT( "Cue_-Z" ) ) };
		return n[ FMath::Clamp( face, 0, 5 ) ];
	}
	inline const FName& PowerFace( int32 face )
	{
		static const FName n[ 6 ] = {
			FName( TEXT( "Power_+X" ) ), FName( TEXT( "Power_-X" ) ), FName( TEXT( "Power_+Y" ) ),
			FName( TEXT( "Power_-Y" ) ), FName( TEXT( "Power_+Z" ) ), FName( TEXT( "Power_-Z" ) ) };
		return n[ FMath::Clamp( face, 0, 5 ) ];
	}

	/** The slot index of `name` on the proxy's current mesh, or INDEX_NONE (with one Warning per mesh). */
	AUTOCONNECTINGPOWERRAILS_API int32 IndexOf( const class UStaticMeshComponent* component, const FName& name );
}

/**
 * One cue slot on one proxy, and which terminal it reports.
 *   TerminalIndex >= 0   that terminal's §4 state: open / coupled / powered / capped
 *   TerminalIndex == -1  the whole host: powered if any terminal is, else coupled if any is, else open
 *   PowerFloor           the socket floor of TerminalIndex: while that terminal is OPEN, the
 *                        network's power — white or dim; once it is coupled or capped, DARK. A
 *                        coupled Rail's end sits on the terminal plane, 2 cm proud of the seat floor
 *                        the socket opens in, so a lit floor would show through that slot all round
 *                        the Rail.
 */
struct FACPRIndicatorSlot
{
	class UFGColoredInstanceMeshProxy* Proxy = nullptr;
	FName Slot;
	int32 TerminalIndex = -1;
	bool PowerFloor = false;
};
static constexpr int32 ACPR_INDICATOR_HOST = -1;

/**
 * The cost meter. Where our own time goes at construction and dismantle, per phase, accumulated
 * and printed as one [ACPR-COST] line per 30 s window of wall clock, from the scope that closes the
 * window (no timer of its own). Cycles64 around the phase; the line says how much of a bulk
 * construction's or dismantle's seconds is ours and in which function.
 *
 * Under `acpr.Trace` only: with it off a scope reads no clock and keeps no count.
 */
enum class EACPRCost : uint8
{
	BeginPlayRail, BeginPlayJunction, BeginPlayOutlet, BeginPlayCap,
	Dismantle, EndPlay,
	ManagerInitialize, ManagerRemap, ManagerConstruct, ManagerAudit, ManagerFixUp,
	/**
	 * The bridge hologram's life, split so a slow spawn or re-show cannot hide.
	 * bridge.spawn is vanilla's SpawnChildHologramFromRecipe and CONTAINS bridge.beginPlay
	 * (our own BeginPlay on it); the other four are one UpdateBridge each: spec, vanilla's placement
	 * check, SetDisabled(false), SetPlacementMaterialState. Nested scopes count twice in "ours total".
	 */
	BridgeSpawn, BridgeBeginPlay, BridgeApply, BridgeValidate, BridgeShow, BridgeMaterial,
	/** Inside bridge.validate, vanilla's CheckValidPlacement (clearance); inside bridge.show, vanilla's hide toggle. */
	BridgeValidateVanilla, BridgeHidden,
	/** Vanilla's SetDisabled( true ) — the other half of the enable/disable pair. */
	BridgeDisable,
	Count
};

struct AUTOCONNECTINGPOWERRAILS_API FACPRCostScope
{
	explicit FACPRCostScope( EACPRCost phase );
	~FACPRCostScope();
private:
	EACPRCost Phase;
	/** 0 while `acpr.Trace` is off: the scope then measures nothing. */
	uint64 StartCycles;
};

struct AUTOCONNECTINGPOWERRAILS_API FACPRIndicators
{
	static void ConfigureProxy( class UFGColoredInstanceMeshProxy* proxy );

	/** Applies the state materials; returns true when any slot changed, which is when it logs. */
	static bool Push( class AFGBuildable* buildable, const IACPRTerminalHost* host,
	                  TArrayView< const FACPRIndicatorSlot > slots, const TCHAR* tag );

	/**
	 * Swatch inheritance, tied to coupling rather than to snapping: a hologram that copied the host's
	 * data at snap-accept would carry a custom swatch away when aimed off and placed unsnapped. So
	 * nothing happens in the hologram. A freshly BUILT host adopts its first coupled partner's
	 * customization the moment §6.1 couples them (FACPRCoupling::Resolve, hand-built path); a Cap or
	 * Outlet adopts its host's at BeginPlay(built). Loaded and blueprint-placed actors keep what they
	 * carry.
	 *
	 * Server-side, through IFGColorInterface::SetCustomizationData_Native (FGColorInterface.h:22, which
	 * applies as well); mCustomizationData is ReplicatedUsing, so clients follow. Read back in the log
	 * line rather than assumed.
	 */
	static void AdoptSwatch( class AFGBuildable* built, const AActor* partnerActor, const TCHAR* tag );

	/** What a proxy holds, logged: the 20 custom data floats and the material in each slot. */
	static void ReadBack( const class UFGColoredInstanceMeshProxy* proxy, const TCHAR* tag, const TCHAR* label );

};

/**
 * What an aimed-terminal search found, and every number that decided it.
 *
 * The counts are not decoration. "It will not snap" has several distinct causes that are
 * indistinguishable from outside the game, and a picker that returns only a pointer forces every
 * caller to invent its own diagnostic, and four different refusals would read alike in a log.
 */
struct FACPRTerminalPick
{
	/** Null when nothing qualified OR when the result was ambiguous. `Tied > 1` tells them apart. */
	class UACPRTerminalComponent* Terminal = nullptr;

	/** Distance from the aim point to the chosen terminal. */
	double Distance = 0.0;

	/** Open terminals on the aimed host within range, before the face filter. */
	int32 OpenInRange = 0;

	/** How many survived the face filter. */
	int32 Candidates = 0;

	/** How many share the winner's distance within the tolerance. Invariant 8 refuses above one. */
	int32 Tied = 0;

	/**
	 * The actor the pick came from.
	 *
	 * Non-const deliberately: `const AActor*` does not compile at the call sites.
	 * TWeakObjectPtr<AActor>'s converting assignment is constrained on convertibility, `const AActor*`
	 * to `AActor*` loses a qualifier, the overload drops out, and `mTargetHost = pick.Host;` has no
	 * viable operator=. It is written from an already-non-const `hitResult.GetActor()`, so the
	 * const would be a decoration rather than a guarantee that buys anything.
	 */
	AActor* Host = nullptr;
};

/**
 * "Which terminal is being aimed at" — one implementation, for every hologram that asks.
 *
 * The Rail hologram asks it for its near end. The Junction asks it for terminal-snapped placement.
 * §9's Cap and §8's Outlet both ask it, and so does §10's blueprint resolver. That is five, and the
 * question is identical every time: of the terminals on the buildable under the crosshair, which one
 * is the player pointing at, and is the answer unambiguous?
 *
 * Three rules travel with it:
 *
 *   The face filter — a hit on a face returns that face's outward as the impact normal, so the
 *   normal answers "which face" directly and distance alone does not. Ranking by distance alone
 *   produces ambiguity refusals whenever the aim crosses a Junction's edges.
 *
 *   The two-pass tie count — a selection and a tie check are two queries over one set, and
 *   fusing them makes the answer depend on terminal index order, which is save format.
 *
 *   Invariant 8 — ambiguity produces no coupling, so the pick refuses rather than choosing.
 *
 * A fifth copy of those would be a fifth chance to get one of them subtly wrong, which is the
 * argument for keeping §6.1 off the Rail applied to the question one step earlier.
 */
/**
 * What is really being aimed at, once, for everything that asks.
 *
 * An attachment stands in front of the terminal it occupies. A Cap is its own actor with its own
 * collision, standing 6 uu PROUD of the terminal plane (AACPRCap::mStandoff), so the build gun's
 * ray hits `Build_PowerRailCap_C` and never reaches the Rail or Junction behind it. A consumer
 * that asked the Cap for its terminals would get nothing — a Cap owns none, §9 says so outright —
 * and would treat the frame as "not aiming at anything of ours":
 *
 *   §7.3's occupied-end refusal   would never fire; the Junction would place itself on the Cap's
 *                                 face, 6 uu off the lattice
 *   §12's highlight               would show nothing at all on a capped host
 *   the Rail's anchor snap        would fall through to free placement with no indication
 *
 * All three are the same step. Resolving the aimed actor through its attachment gives every caller
 * the host that is actually there, plus the terminal the attachment is sitting on — which is
 * precisely the terminal each of them needs to name.
 *
 * Deliberately NOT an interface on the attachment. A Cap is not an IACPRTerminalHost and must not
 * become one (§9: it occupies a terminal, it does not own any), and one more UINTERFACE to express
 * "I am attached to a terminal" would be reflection for a two-line question. The concrete cases are
 * named in the .cpp, where the concrete headers are already available.
 */
struct AUTOCONNECTINGPOWERRAILS_API FACPRAimedHost
{
	/** The terminal host being aimed at, through an attachment if there was one. Null if none. */
	AActor* HostActor = nullptr;

	/** The attachment the ray actually hit, or null when the host was hit directly. */
	AActor* Attachment = nullptr;

	/**
	 * The terminal that attachment occupies, or null.
	 *
	 * Almost always an occupied one, and callers must still ask. FACPRCoupling::ReleaseAll reopens
	 * the host terminal at DISMANTLE rather than at destruction, so between a Cap being dismantled
	 * and its effect finishing there is a live attachment standing in front of a terminal that is
	 * already Open. Anything refusing on the strength of this pointer has to test IsOpen() too.
	 */
	class UACPRTerminalComponent* OccupiedTerminal = nullptr;

	/** Resolves a hit actor. Returns the actor itself when it is already a host. */
	static FACPRAimedHost Resolve( AActor* hitActor );
};

struct AUTOCONNECTINGPOWERRAILS_API FACPRTerminalPicker
{
	/**
	 * @param hitResult      the build gun's hit; its actor supplies the candidates and its normal the face filter
	 * @param snapRange      how near the aim point a terminal must be to count as aimed at
	 * @param ambiguityTol   the tie tolerance, applied in the second pass only
	 * @param faceAlignMin   dot( outward, hit normal ) above which a terminal counts as the aimed face; <= 0 disables
	 * @param exclude        a terminal never to return — the Rail's own anchor, for instance
	 */
	static FACPRTerminalPick FindAimed( const FHitResult& hitResult,
	                                    double snapRange,
	                                    double ambiguityTol,
	                                    double faceAlignMin,
	                                    const class UACPRTerminalComponent* exclude = nullptr );
};

/**
 * §12's terminal feedback, once, for every hologram that targets terminals.
 *
 * §12: "Eligible terminals and the exact selected target are highlighted during construction.
 * Capped, coupled and Outlet-occupied terminals never display as valid."
 *
 * Why this is not cosmetic, and why it is worth a shared helper rather than four ad-hoc versions.
 * Without it the four buildables appear to disagree about whether a capped terminal is refused —
 * and they do not. Each obeys its own section; what differs is what each one's FALLBACK does when
 * the terminal question says no:
 *
 *   Rail     -> free beam placement        -> nothing visibly changes
 *   Junction -> §7.1 standalone placement  -> the cube shifts off the face by mSurfaceOffset
 *   Outlet   -> none (§8 gives only body and terminal) -> red
 *   Cap      -> none (§9 gives only a terminal)        -> red
 *
 * So the Junction's apparent "slipping off" is an accident of the surface offset and the Outlet's
 * red is the absence of anywhere else to go. Neither is a deliberate statement about the terminal.
 * A single shared cue is the only thing that makes the four consistent, because it does not depend
 * on what any of them falls back to.
 *
 * Scoped to the aimed host, deliberately, matching FACPRTerminalPicker: candidates come from the
 * actor under the crosshair, never from a registry sweep, so the highlight answers "what can I do
 * with THIS" rather than lighting up the factory.
 *
 * No markers: a box at the snap point is buried by the hologram standing on it, and a frame mesh
 * of its own is one more thing in the scene. The highlight is a cue state, lit on the host's own
 * surfaces; this struct only decides WHICH terminals get it and hands that to the host.
 */
struct AUTOCONNECTINGPOWERRAILS_API FACPRTerminalHighlight
{
	/**
	 * Lights the SELECTED terminal of the aimed host (and nothing else) by writing an
	 * FACPRHighlight onto the host. `lastHost` is the hologram's memory of where it wrote last, so
	 * the previous host is cleared the moment the aim moves to another.
	 *
	 * `hitActor` is the RAW aimed actor and is resolved through FACPRAimedHost, so a ray that lands
	 * on a Cap still lights up the open faces of the Rail or Junction behind it.
	 *
	 * Only an OPEN, non-private terminal can be selected — the picker's own test, in the same file, so
	 * the cue cannot drift from the rule it is reporting.
	 *
	 * `space` adds §10.1: with Enforce set, a host in a different build space than the placement shows
	 * nothing at all, because the hologram will refuse it with vanilla's Designer message. Required
	 * rather than defaulted, so every hologram states its answer instead of inheriting "don't care".
	 */
	static void Show( TWeakObjectPtr< AActor >& lastHost,
	                  AActor* hitActor,
	                  const class UACPRTerminalComponent* selected,
	                  const struct FACPRSpaceFilter& space,
	                  const class UACPRTerminalComponent* pinned = nullptr,
	                  const class UACPRTerminalComponent* pinned2 = nullptr );

	/**
	 * Keeps one terminal lit Selected on its own host whatever the crosshair is on — the Rail's anchor
	 * during its second step, so the snap does not look lost. `skipHost` is the host Show()
	 * wrote this frame; when the pinned terminal lives there, Show()'s `pinned` argument has already
	 * covered it and this writes nothing, so the two never fight over one host. A null terminal clears.
	 */
	static void Pin( TWeakObjectPtr< AActor >& lastHost,
	                 const class UACPRTerminalComponent* terminal,
	                 const AActor* skipHost );

	/** Clears the highlight on the last host, if any. Called on any frame with no host under the crosshair. */
	static void HideAll( TWeakObjectPtr< AActor >& lastHost );
};

/**
 * §6.1, once, for every terminal host.
 *
 * Free functions rather than default interface implementations, because every one of them needs
 * both the interface AND the AActor — the resolver has to log actor names, compare owners and read
 * the world's registry — and a C++ interface in Unreal cannot get to its own actor without a cast
 * that the caller already has for free.
 */
struct AUTOCONNECTINGPOWERRAILS_API FACPRCoupling
{
	/**
	 * The whole of a host's coupling work for one construction or one load.
	 *
	 * Does one of two things and never both:
	 *   loaded from a save -> replay the saved records, and NOTHING else. Invariant 6.
	 *   newly built        -> §6.1's coincident-pair rule against the registry.
	 *
	 * Coupling at registration: called at the end of BeginPlay, right after the host has registered
	 * its terminals, not a tick later. The circuit subsystem assigns circuit IDs on its own tick either
	 * way, and RestoreSaved measures the saved geometry (GetTerminalLocalFrame, which exists for
	 * exactly this) rather than component transforms the partner's BeginPlay may not have placed, and
	 * forms the pair on both sides itself, so whichever host begins play first completes it and the
	 * second finds it done. No ordering, no timer.
	 */
	static void Resolve( AActor* actor, IACPRTerminalHost* host );

	/**
	 * Releases one terminal's coupling, reciprocally — the single implementation behind ReleaseAll,
	 * the terminal's own destruction hook, the Outlet's Remap and RestoreSaved's drop.
	 * Clears both ends, drops both hosts' records for the pair, and removes the hidden edge between the
	 * two hosts' connections when this was the last coupling between them. Idempotent. `why` is for the log.
	 */
	static void Release( AActor* actor, IACPRTerminalHost* host, uint8 localIndex, const TCHAR* why );

	/** How many couplings currently join `actor` to `other` — one hidden edge serves all of them. */
	static int32 CountCouplingsBetween( const IACPRTerminalHost* host, const AActor* other );

	/** Find and make the coincident pair for one terminal, if there is exactly one candidate. */
	static void TryCoupleTerminal( AActor* actor, IACPRTerminalHost* host,
	                               UACPRTerminalComponent* terminal, uint8 localIndex );

	/** Joins two terminals and records the Coupling on both owners. */
	static void Form( AActor* actor, IACPRTerminalHost* host,
	                  UACPRTerminalComponent* ours, uint8 localIndex,
	                  UACPRTerminalComponent* theirs );

	/**
	 * Replays the saved records — and repairs them, as §6.3 allows and §10 needs.
	 *
	 * §6.3: "conservative save repair may remove an invalid edge before it is player-visible".
	 * Restoring a record whenever both ends still exist, whatever they are, would permit a Designer
	 * Rail coupled to a world Rail, saved and restored verbatim on every load. A record is restored
	 * only when all of these hold, and each failure is dropped with its own [ACPR-REPAIR] line:
	 *
	 *   same world         a blueprint can carry references into its own private world
	 *   same build space   §10.1
	 *   coincident         within RepairCoincidenceTolerance — §6.1 formed it exactly; this only has to
	 *                      survive save-file float noise, so it is a tolerance for KEEPING, never for making
	 *   opposing           outward axes antiparallel
	 *   reciprocal         the partner holds the mirror record (invariant 6: both participants record)
	 *   exclusive          neither terminal is capped or already coupled elsewhere (invariant 4)
	 *
	 * A dropped record also loses its hidden power connection. That half matters most: the component's
	 * own mHiddenConnections is SaveGame (FGCircuitConnectionComponent.h:192), so vanilla restores the
	 * electrical edge on its own — dropping only our record would leave a Designer Rail powered from the
	 * world with nothing in the topology to say so.
	 *
	 * The ONE record that is re-resolved rather than dropped is a reference into another world. That is
	 * a blueprint reference that was not remapped, and the coupling it describes is still true in
	 * geometry — so §6.1's coincident rule is asked instead, for that terminal only. Invariant 6 is
	 * intact: no terminal with a resolvable record ever searches.
	 */
	static void RestoreSaved( AActor* actor, IACPRTerminalHost* host );

	/**
	 * Removes hidden power edges between terminals that are not a Coupling.
	 *
	 * Runs at the end of Resolve on BOTH paths. On a hand-built host it finds nothing. It exists for
	 * edges that come from outside the mod's own records: vanilla's save of mHiddenConnections (see
	 * RestoreSaved), and a blueprint's serialized copy of the same, which can name components outside
	 * the blueprint.
	 *
	 * Deliberately narrow. An edge is removed only when BOTH ends are numbered terminals of hosts
	 * (GetIndexOfTerminal != INDEX_NONE), the two hosts differ, and the far end is not this terminal's
	 * coupling partner. Internal edges (same owner), a Rail's Hoverpack nodes and an Outlet's body bond
	 * (its socket is not a numbered terminal) are never touched.
	 */
	static void ScrubStrayHiddenEdges( AActor* actor, IACPRTerminalHost* host );

	/** "open", "capped", or "coupled>Owner[index]" — the compact per-terminal state for one-line logs. */
	static FString DescribeState( const UACPRTerminalComponent* terminal );

	/**
	 * "[ local->Other[index] same|OTHER-WORLD|null ... ]" — every saved record, with whether its actor
	 * reference lands in this actor's own world (a blueprint can leave one pointing into its private
	 * world; see RestoreSaved).
	 */
	static FString DescribeRecords( const AActor* actor, const IACPRTerminalHost* host );

	/**
	 * The one line per host, at Display.
	 *
	 * The per-terminal dumps are Verbose. What stays visible is every terminal's state in one field,
	 * whether the host came from a save or a blueprint, which build space it is in, and which
	 * blueprint group it belongs to.
	 *
	 * @param blueprintState  "-" for an ordinary host, "placed" or "bpworld" from PostSerializedFromBlueprint
	 */
	static void LogHostSummary( AActor* actor, const IACPRTerminalHost* host, const TCHAR* tag,
	                            const TCHAR* stage, const TCHAR* blueprintState );

	/**
	 * How far apart two coupled terminals may drift in a save file and still be restored.
	 *
	 * Formation is exact — §6.1 compares 1 uu cells for equality and nothing here changes that. This
	 * tolerance only decides whether a coupling that WAS exact is still recognisably the same pair after
	 * a save round-trip, so it errs large: 2 uu against a 100 uu lane pitch cannot confuse two pairs.
	 */
	static constexpr double RepairCoincidenceTolerance = 2.0;

	/**
	 * The guard that turns a fatal cast into a log line.
	 *
	 * UFGPowerCircuit::OnCircuitChanged CastChecks every member of a power circuit to
	 * UFGPowerConnectionComponent on the subsystem's tick — a frame or more after the
	 * AddHiddenConnection that put it there — so a type mistake would surface as a crash with no mod
	 * code on the stack.
	 */
	static bool VerifyPowerCircuitSafe( const AActor* actor,
	                                    class UFGCircuitConnectionComponent* connection,
	                                    const TCHAR* what );

	/**
	 * Where terminal `index` of `host` is and faces, from saved state (GetTerminalLocalFrame) composed
	 * with the actor's transform — true whether or not that actor's BeginPlay has placed its components.
	 * What RestoreSaved measures with, so a load has no ordering dependency.
	 */
	static bool SavedTerminalFrame( const AActor* actor, const IACPRTerminalHost* host, uint8 index,
	                                FVector& out_location, FVector& out_outward );

	/**
	 * Releases every terminal this host owns, at dismantle time rather than at destruction.
	 *
	 * The two are seconds apart, and the gap would be visible in game as a delay between dismantling
	 * something connected to a terminal and that terminal becoming a valid open target again.
	 * Satisfactory plays the dismantle build effect and destroys the actor when it finishes, so a
	 * release in EndPlay would run at the END of an animation, and until then the surviving partner's
	 * mCoupledTo would still point at a component that is on its way out. A terminal derives its
	 * state from its occupant (§4), so it reads Coupled for as long as that pointer stands.
	 *
	 * IFGDismantleInterface::Dismantle is the commit, not a preview — the player has paid and the
	 * refund is issued — so acting on it cannot undo a placement that is still being considered.
	 *
	 * It does two things and both are needed. It clears the coupling from BOTH sides, which is what
	 * reopens the survivor; and it unregisters our terminals, so the dying host cannot be found by
	 * §6.1's coincident-pair rule during the seconds it is still standing. Idempotent, because the
	 * terminal component's own OnComponentDestroyed runs the same release on every other path — a
	 * destruction that is not a dismantle — and a dismantle reaches it twice.
	 *
	 * The hidden edge between the two hosts' connections goes with the last coupling between them
	 * (Release). The circuit subsystem rebuilds on its own tick either way.
	 */
	static void ReleaseAll( AActor* actor, IACPRTerminalHost* host );
};
