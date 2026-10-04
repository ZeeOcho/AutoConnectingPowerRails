// Auto-Connecting Power Rails — the Insulated Terminal Cap.
//
// §9: "A Cap snaps only to an open terminal and caps it. It is not itself a snap target."
//
// The Cap is the saved record, and that is the whole design. UACPRTerminalComponent::mIsCapped is
// Transient by deliberate choice — the terminal derives its state from its occupant rather than
// storing it twice (§4: "never two at once"), and the authoritative occupant here is this actor.
// So a Cap saves WHICH terminal it occupies, and re-applies the capped state to it on load. Nothing
// scans, nothing infers, and a Cap that failed to save simply leaves an open terminal rather than a
// terminal that believes it is capped with nothing capping it.
//
// That also makes invariant 5 free. "A capped terminal is not a geometric snap target" needs no
// separate rule anywhere: every snap path in this mod already filters on UACPRTerminalComponent::
// IsOpen(), and IsOpen() is false while an occupant exists. The Cap does not have to be excluded
// from the Rail hologram, the Junction hologram, the far-end search or §6.1's coincident pair,
// because it is not a terminal and its host is no longer open.
//
// What it is not. A Cap owns no terminals, so it is not an IACPRTerminalHost. It occupies one
// (ACPRTerminalHost.h's own summary says exactly this). It therefore cannot be coupled to, cannot
// be a blueprint candidate, and cannot continue a Rail — all by absence rather than by a rule.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"
#include "Buildables/FGBuildable.h"
#include "ACPRTerminalHost.h"
#include "ACPRCap.generated.h"

class UACPRTerminalComponent;
class UStaticMesh;

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API AACPRCap : public AFGBuildable
{
	GENERATED_BODY()

public:
	AACPRCap();

	virtual void BeginPlay() override;
	virtual void EndPlay( const EEndPlayReason::Type endPlayReason ) override;

	/**
	 * §9: "Dismantling the Cap reopens the terminal." Immediately — see FACPRCoupling::ReleaseAll
	 * for why EndPlay would be seconds too late.
	 */
	virtual void Dismantle_Implementation() override;


	/** §14. Without this the Cap does not survive a reload and its terminal silently reopens. */
	virtual bool ShouldSave_Implementation() const override;
	virtual void GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const override;

	virtual void PostLoadGame_Implementation( int32 saveVersion, int32 gameVersion ) override;

	/**
	 * §10's blueprint path, instrumented: mHostActor is a SaveGame actor reference like a coupling
	 * record, and this logs whether a placed blueprint left it pointing at the new host or at the
	 * blueprint's private copy. An AFGBuildable virtual (FGBuildable.h:559), so no new import.
	 */
	virtual void PostSerializedFromBlueprint( bool isBlueprintWorld ) override;
	/**
	 * Which terminal this Cap occupies, written by the hologram at construction.
	 *
	 * (actor, index) rather than a component pointer, for the same reasons FACPRSavedCoupling gives:
	 * it is what survives a save cleanly, and it is the form §10.6's server revalidation and B.3's
	 * "explicit actor remapping" want for blueprints. The index is save format — reordering a
	 * host's terminals moves every Cap in every save onto a different face.
	 */
	void SetHostTerminal( AActor* hostActor, uint8 terminalIndex );

	/**
	 * Moves this Cap onto another host's terminal, releasing the old one first.
	 *
	 * §7.4: insertion preserves "terminal Caps and Outlets at the original endpoints". After a split
	 * the original endpoints belong to the two children, so the Cap has to follow — and SetHostTerminal
	 * alone would not do it: the old terminal would stay capped on an actor that is about to be
	 * destroyed (harmless) and the new one would stay OPEN (not harmless at all, because §6.1 would
	 * then couple it to whatever it is now facing).
	 *
	 * The Cap does not move in world space. Its transform is already on the plane the child's terminal
	 * lands on, because the child's terminal is the original's terminal at the same coordinates.
	 */
	void Remap( AActor* newHostActor, uint8 newTerminalIndex );

	AActor* GetHostActor() const { return mHostActor; }
	uint8 GetHostTerminalIndex() const { return mHostTerminalIndex; }

	/** Resolves the host reference to the live component, or null if the host is gone. */
	UACPRTerminalComponent* ResolveHostTerminal() const;

	/**
	 * Scales and positions a mesh component so it renders as a box of `extent` half-sizes with its
	 * OUTER face on the local YZ plane and its body extending along local -X.
	 *
	 * §9 is specific about this and it is not cosmetic: "the outer face lies at the terminal snap
	 * plane ... and the mesh extends inward into the owning actor and never beyond the plane. A Cap
	 * therefore changes no terminal position, Rail length, Junction cell, blueprint interface
	 * transform or stacking geometry." A Cap that stuck out by even a few centimetres would push the
	 * next lane's Junction off the 1 m pitch, and §6.2's back-to-back Caps ("zero added spacing")
	 * would overlap.
	 *
	 * Fitted in C++ and nowhere else: the editor and the game can measure the same asset differently
	 * (256 uu against 110.692 for one mesh), so a scale a script computes in the editor renders at
	 * the wrong size in game. The fit and the thing being fitted must come from the same
	 * measurement, taken in the process that renders.
	 *
	 * Static, and public, because the hologram is not an AACPRCap and has no business becoming one to
	 * borrow a formula — the same argument as AACPRJunction::FitMeshComponentToCube, which this
	 * generalises from a cube to a box.
	 */
	static FVector FitMeshComponentToBox( class UStaticMeshComponent* comp, const FVector& extent );

	/**
	 * The rotation, in one place, because every ACPR mesh needs the same one.
	 *
	 * Geometry Script builds along +Z and `acpr_meshes.py` says so in every docstring — the Outlet
	 * is "authored along +Z with its base on the mounting plane", the Rail body "authored 4 m along
	 * +Z with its base at the origin", the Cap's face is at z = 0 with the plate extending to -Z.
	 * Every ACPR actor, meanwhile, uses its LOCAL +X as the outward/mounting normal: §8.2 takes it
	 * from the terminal axis, §8.1 from the target face, and the beam axis is local X.
	 *
	 * A per-axis box fit without the rotation would produce roughly the right BOUNDING BOX from the
	 * wrong axes — a 40 x 40 x 26 cylinder scaled into a 90 cube, a 50 x 50 x 6 plate scaled into a
	 * 6 x 50 x 50 one at 8.3x on the axis that was 6 — parts the right size, in the wrong
	 * orientation, with their proportions destroyed: an Outlet pointing sideways and a Cap reading
	 * as a flat slab.
	 *
	 * -90 in pitch is the rotation: UE pitch turns about Y and a positive pitch carries +X up to +Z,
	 * so the negative one carries the mesh's +Z forward to the actor's +X. It maps mesh Y to actor Y
	 * and mesh X to actor -Z, which is why `targetSize` is given in ACTOR axes and unpicked here.
	 *
	 * No re-centring along X, deliberately. Both meshes author their mounting plane ON the origin —
	 * the Outlet's base, the Cap's outer face — so the only X translation is `standoff`, which keeps
	 * its existing signed meaning. The transverse axes are centred on the mesh's own bounds.
	 *
	 * `uniformScale` fits the TRANSVERSE size and applies one factor to all three axes, for a shape
	 * whose proportions carry meaning: a ribbed cylinder stretched to fill a cube stops reading as a
	 * socket. Per-axis is right where the numbers are stated independently, like the Cap's plate.
	 */
	static FVector FitZAuthoredMeshToLocalX( class UStaticMeshComponent* comp,
	                                         const FVector& targetSize,
	                                         float standoff,
	                                         bool uniformScale );

	/**
	 * The whole Cap fit, in one place, because two renderers draw it.
	 *
	 * The buildable fits its mesh at BeginPlay; the HOLOGRAM never runs that, because B.2 says a
	 * hologram's preview is duplicated from the buildable CDO's component templates and nothing on
	 * that path calls BeginPlay. A fit that lived only there would leave the preview showing the raw,
	 * unfitted mesh.
	 *
	 * Sharing one static is the same answer as fitting in C++ at all: the fit and the thing being
	 * fitted must come from the same formula, and the only way to guarantee that is to have one.
	 *
	 * `standoff` is signed, and zero or NEGATIVE — §9 puts the Cap's body inward, behind the
	 * terminal plane. See mStandoff and mJunctionCapInset.
	 */
	static FVector FitCapMesh( class UStaticMeshComponent* comp,
	                           float depth, float faceWidth, float standoff );

	/**
	 * One cap mesh per host face width, and the reason is the accent.
	 *
	 * The chamfer carries the circuit colour, and a fit scales the whole mesh, so a single
	 * authored 2 cm cut would come out 1.6 cm against a Rail's 2.0 and 3.6 cm against a
	 * Junction's. A Cap whose trim does not line up with the part it is sitting on is the one
	 * thing a Cap must not be. Each mesh is authored at its own face width instead, and both
	 * fits are then 1:1.
	 *
	 * Soft paths with defaults in source, the same as AACPRRail::mRailMeshAsset and for the
	 * same reason: a clean checkout builds a Cap that already has its mesh, and this game
	 * feature's content is not guaranteed mounted when the CDO is constructed.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	TSoftObjectPtr< UStaticMesh > mRailCapMeshAsset =
		TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
			TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_CapRail.SM_ACPR_CapRail" ) ) );

	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	TSoftObjectPtr< UStaticMesh > mJunctionCapMeshAsset =
		TSoftObjectPtr< UStaticMesh >( FSoftObjectPath(
			TEXT( "/AutoConnectingPowerRails/Meshes/SM_ACPR_CapJunction.SM_ACPR_CapJunction" ) ) );

	/**
	 * Picks the mesh for this kind of host, puts it on `comp`, and fits it — in that order and
	 * in one place, because the buildable and the hologram both draw a Cap and must do it from
	 * the same formula. Returns the fitted size in actor axes.
	 *
	 * `junctionLike` is the host's terminal COUNT being greater than two, which is how the rest
	 * of this class tells a Junction from a Rail without a class cast.
	 */
	static FVector ApplyCapMesh( class UStaticMeshComponent* comp,
	                             const AACPRCap* cdo,
	                             bool junctionLike );

	/**
	 * §11: "Cap external spacing | 0 m; outer face at or microscopically inside the terminal plane."
	 *
	 * Where the Rail Cap's outer face sits relative to the terminal plane. Signed, and positive
	 * means OUTWARD, which §9 forbids: the outer face lies at the plane and the mesh "extends inward
	 * into the owning actor and never beyond the plane". SM_ACPR_CapRail is authored that way and
	 * the Rail terminal carries RAIL_TERMINAL_RECESS for it to sit in, so with
	 * FitZAuthoredMeshToLocalX putting the mesh's own face on the plane, zero IS §11's "0 m" and
	 * §6.2's back-to-back pair. It moves the MESH only; the actor origin stays on the snap plane, so
	 * the terminal it occupies is untouched whatever this is set to. A blueprint that overrides it
	 * wins, and will float the Cap proud.
	 *
	 * Zero rather than a millimetre in: a sweep meets the FRONTMOST surface anywhere in its aim
	 * disc, so a Cap sunk even a millimetre hands the pocket's rim every aim instead of half of
	 * them and stops being targetable. The Rail pocket has nothing coplanar with the face to fight,
	 * so there is nothing to sink away from.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	float mStandoff = 0.0f;

	/**
	 * A tenth of a centimetre, sunk INWARD, to stop the Junction Cap z-fighting its host.
	 *
	 * The Junction Cap is buried in the cube with only its outer face showing, and that face is
	 * EXACTLY coplanar with the Junction's own — so the four indicator strips shimmer through it.
	 * Breaking a coplanar pair only needs the two surfaces separated; it does not care which way.
	 *
	 * Inset, not standoff, and the sign is the whole point. A millimetre proud would put a capped
	 * 1 m Junction at 100.2 cm, and "only a millimetre" is not a principle. The rule: a Cap never
	 * extends its host's footprint. When two surfaces fight, sink the Cap. There is no case where
	 * pushing it out is the answer.
	 *
	 * A millimetre recessed is also what §9 asks for in as many words — "at or microscopically
	 * inside the terminal plane" — so this is the spec's own tolerance rather than a new licence.
	 *
	 * The Rail Cap keeps 0: its face sits in a pocket with nothing coplanar to fight, because the
	 * body is inset between the collars.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	float mJunctionCapInset = 0.1f;

	/** §11's face width — 0.4 m on a Rail end, 0.9 m on a Junction face. Overridden per host below. */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	// 44 = RAIL_TERMINAL_RECESS_SIZE. A Cap fills its pocket, in width as in depth: narrower, it
	// would leave a ring of pocket wall showing and read as the bottom of the hole rather than as
	// a Cap.
	float mRailFaceWidth = 44.0f;

	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	// The Junction's cap SEAT — JUNCTION_CAP_SEAT_SIZE. The cube's flat face is 97.172
	// (100 - ACCENT_WIDTH * root2), which is the seat plus the rim it leaves standing; the seat
	// itself is that less JUNCTION_SEAT_MARGIN on each side, so 93.172. The Cap fills the seat, so
	// the indicator strips are covered out to the rim. [ACPR-FIT] names any drift between this and
	// the authored mesh.
	float mJunctionFaceWidth = 93.172f;

	/**
	 * How far the Cap reaches INTO its host — ONE PER HOST, because the pockets differ.
	 *
	 * A depth beyond the pocket would put centimetres of Cap inside solid host: two
	 * interpenetrating solids, and since the meshes collide complex-as-simple, two sets of
	 * coincident surfaces for the build gun's trace to choose between — a target flicking between
	 * Rail and Cap on a tiny mouse movement.
	 *
	 * A Cap fills its host's pocket exactly and stops. Its outer face then lands ON the terminal
	 * plane, which is §9's "the outer face sits AT the terminal plane" and §11's "Cap external
	 * spacing | 0 m" — so nothing extends past the whole-metre boundary. mStandoff stays 0; a
	 * standoff would push the Cap proud and lengthen the Rail, which is the wrong dial.
	 *
	 * acpr_meshes.py authors CAP_DEPTH_RAIL and CAP_DEPTH_JUNCTION from the pockets themselves;
	 * these must match, and [ACPR-FIT] is what says so when they stop matching.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	// = RAIL_TERMINAL_RECESS + RAIL_TERMINAL_SETBACK. The setback is 0, so this is the pocket: 3.
	// If this and CAP_DEPTH_RAIL ever disagree, [ACPR-FIT] says so on the next run.
	float mRailCapDepth = 3.0f;

	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Cap" )
	// The seat's depth, not the socket's: JUNCTION_CAP_SEAT_DEPTH. The Cap is a plate that fills
	// the seat flush, not a block buried in the cube where nothing could see it.
	float mJunctionCapDepth = 2.0f;

protected:
	/** Applies the capped state to the host terminal, and logs whether it took. */
	void ApplyCap();

	/** §9: "Dismantling the Cap reopens the terminal, initiating no search." */
	void ReleaseCap();

	/** Attach and fit — the whole of the mesh setup, from BeginPlay and from the blueprint world alike. */
	void SetUpMeshes();
	void FitMesh();

	void LogCap( const TCHAR* stage ) const;

private:
	/**
	 * The visible plate. In C++ rather than added to the Blueprint by hand: a setup step that
	 * silently produces nothing is the wrong kind of step, and a Cap with no mesh is invisible
	 * rather than obviously unfinished.
	 *
	 * Safe to add in C++ because the Cap's hologram is a plain AFGBuildableHologram, exactly as the
	 * Junction's is; a hologram that binds a mesh by first match — AFGBeamHologram's mBeamMesh —
	 * would pick up whichever component came first.
	 */
	UPROPERTY( VisibleAnywhere, Category = "ACPR|Cap" )
	// UFGColoredInstanceMeshProxy for the accent — see AACPRRail::mRailMeshComponent. A Cap owns no
	// terminals (§9), so it pushes no indicator data; the proxy is for the accent's swatch.
	TObjectPtr< class UFGColoredInstanceMeshProxy > mCapMesh = nullptr;

	// SaveGame AND Replicated: a client fits its Cap from these — a Rail cap or a Junction cap, on
	// which face. OnRep re-fits. UNTESTED in multiplayer.
	UPROPERTY( SaveGame, ReplicatedUsing = OnRep_Host )
	TObjectPtr< AActor > mHostActor = nullptr;

	UPROPERTY( SaveGame, ReplicatedUsing = OnRep_Host )
	uint8 mHostTerminalIndex = 0;

	UFUNCTION()
	void OnRep_Host();

	/** Set by PostLoadGame, read at BeginPlay. A loaded Cap re-applies; a built one applies fresh. */
	bool mCameFromSave = false;
};
