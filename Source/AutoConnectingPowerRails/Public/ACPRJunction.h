// Auto-Connecting Power Rails — the Power Rail Junction.
//
// §7: "A restrained 1 m cubic node with three opposing terminal pairs, all six electrically common
// and independently open, coupled or capped."
//
// What it inherits, and what it does not. §7 is explicit that "the Beam Connector is the placement
// and visual basis, not a class-inheritance requirement" — which is just as well, because
// Build_Beam_Connector is not an AFGBuildableBeam subclass at all (it returns None for every beam
// field). So the Junction starts from AFGBuildable and borrows nothing from the Rail's chain. It
// has a fixed size, no length, and no instance-data rendering: its mesh is an ordinary component on
// the Blueprint leaf.
//
// What it shares with the Rail is the part that matters: IACPRTerminalHost. Every rule in §6.1 —
// coincidence, opposition, invariant 8's ambiguity refusal, invariant 6's saved replay — is
// FACPRCoupling's, not the Rail's and not the Junction's. A Junction couples to a Rail because both
// are hosts, not because either knows about the other.
//
// Terminal indices are save format. 0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z, in the actor's
// own frame. A saved Coupling names a terminal by index, so reordering this list silently reconnects
// every stored Coupling to a different face.

#pragma once

#include "CoreMinimal.h"
#include "ACPRSpec.h"
#include "ACPRTerminalHost.h"
#include "Buildables/FGBuildable.h"
#include "Engine/EngineTypes.h"
#include "ACPRJunction.generated.h"

class UACPRTerminalComponent;
class UACPRPowerConnectionComponent;

UCLASS()
class AUTOCONNECTINGPOWERRAILS_API AACPRJunction : public AFGBuildable, public IACPRTerminalHost
{
	GENERATED_BODY()

public:
	AACPRJunction();

	virtual void BeginPlay() override;

	/**
	 * §14's "dismantling any element immediately removes graph edges", and immediately is the
	 * operative word. See FACPRCoupling::ReleaseAll: the dismantle effect runs for seconds before
	 * the actor is destroyed, so a release that waited for EndPlay would leave a terminal Coupled or
	 * Capped for the whole animation.
	 */
	virtual void Dismantle_Implementation() override;


	/** §14. Without this the Junction does not survive a reload. */
	virtual bool ShouldSave_Implementation() const override;

	virtual void PostLoadGame_Implementation( int32 saveVersion, int32 gameVersion ) override;

	// Begin IACPRTerminalHost
	virtual int32 GetTerminalCount() const override { return TerminalCount; }
	virtual UACPRTerminalComponent* GetTerminalAtIndex( uint8 index ) const override;
	virtual void AddSavedCoupling( const FACPRSavedCoupling& coupling ) override;
	virtual const TArray< FACPRSavedCoupling >& GetSavedCouplings() const override { return mSavedCouplings; }
	virtual TArray< FACPRSavedCoupling >& GetMutableSavedCouplings() override { return mSavedCouplings; }
	virtual UACPRPowerConnectionComponent* GetPowerConnection() const override { return mPower; }
	virtual bool HasNetworkPower() const override;
	virtual void OnTerminalStateChanged( int32 index ) override;
	virtual bool WasLoadedFromSave() const override { return mCameFromSave || mSavedOnce; }
	/** The data-side witness that this host has been through a save (see WasLoadedFromSave). */
	virtual void PreSaveGame_Implementation( int32 saveVersion, int32 gameVersion ) override;
	virtual void GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const override;

	/** The six faces as vanilla attachment points (FACPRAttachment). See AACPRRail's. */
	virtual void GetAttachmentPoints( TArray< const FFGAttachmentPoint* >& out_points ) const override;	virtual bool WasPlacedFromBlueprint() const override { return mPlacedFromBlueprint; }
	/** §12's highlight: stored, and pushed at once when it changes. */
	virtual void SetHighlight( const FACPRHighlight& highlight ) override;
	virtual const FACPRHighlight* GetHighlight() const override { return &mHighlight; }
	virtual TArray< TWeakObjectPtr< AActor > >* GetAttachmentList() const override { return &mAttachments; }

	/** §10.1. AFGBuildable::mBlueprintDesigner, our own protected field (FGBuildable.h:1014). */
	virtual class AFGBuildableBlueprintDesigner* GetHostDesigner() const override;

	/** Face `index` at mHalfExtent along its axis — PlaceTerminals' rule, from saved state alone. */
	virtual bool GetTerminalLocalFrame( uint8 index, FTransform& out_frame ) const override;

	/** §10's blueprint path, instrumented. See AACPRRail for why these add no new import. */
	virtual void PreSerializedToBlueprint() override;
	virtual void PostSerializedFromBlueprint( bool isBlueprintWorld ) override;

	/**
	 * §8.3 and §9: dismantling a host takes its Caps and Outlets with it, "with a full refund and
	 * visible dependency preview". This is vanilla's hook for exactly that (FGBuildable.h:303); the
	 * list it reports is the one the attachments registered themselves into.
	 */
	virtual void GetChildDismantleActors_Implementation( TArray< AActor* >& out_ChildDismantleActors ) const override;
	// End IACPRTerminalHost

	/** Three opposing pairs. Fixed, and part of the save format — see the header. */
	static constexpr int32 TerminalCount = 6;

	/**
	 * Scales and centres a mesh component so its mesh renders as a 2 * halfExtent cube. Returns the
	 * size it predicts, or zero if there was nothing to fit.
	 *
	 * The fit lives in C++ because two measurements of one mesh can disagree: the editor script
	 * measures SM_ConnectionCube_01 through Python at 256 uu, while the game measures the same asset
	 * through UStaticMesh::GetBounds() at 110.692. A scale computed from 256 and rendered against
	 * 110.692 makes a 0.43 m Junction instead of a 1 m one — small enough to sit inside a Rail.
	 *
	 * Which of the two numbers is "right" is not the interesting question. The fit and the thing
	 * being fitted must come from the same measurement, taken in the process that does the
	 * rendering. So the script assigns the mesh and nothing else, and this — called by the buildable
	 * and by the hologram, so the preview and the built actor cannot drift apart either — owns the
	 * transform.
	 *
	 * Static because the hologram is not an AACPRJunction and has no business becoming one to borrow
	 * a formula.
	 */
	static FVector FitMeshComponentToCube( class UStaticMeshComponent* comp, float halfExtent );

	/**
	 * §11 sets the Junction cube at 1 m, and B.2 requires "the Junction actor origin at the exact
	 * centre of its 1 m cube" — which is what makes the terminals land on the face centres at half
	 * the cube from the origin, and what makes the 1 m lane pitch of §11 come out of the geometry
	 * rather than out of a constant somewhere else.
	 *
	 * Editable so the cube can be re-measured against the eventual art without a rebuild.
	 */
	UPROPERTY( EditDefaultsOnly, Category = "ACPR|Junction" )
	float mHalfExtent = ACPRSpec::JunctionHalfExtent;

	/** One attachment-point component per face, on the CDO (hologram-cached and buildable-offered). */
	UPROPERTY( VisibleAnywhere, Category = "ACPR|Junction" )
	TArray< TObjectPtr< class UFGAttachmentPointComponent > > mFaceAttachments;

	void RefreshAttachmentPoints();
	TArray< FFGAttachmentPoint > mTerminalPoints;

protected:
	/** Attaches and positions the six terminals, and the one power connection at the centre. */
	void PlaceTerminals();

	/** Files all six under their quantized cells once the geometry is final. */
	void RegisterTerminals();

	/** The power delegate's handler: the network's power changed on this host. */
	void HandleHasPowerChanged( bool hasPower );

	/** A client received the host's replicated terminal states / power. */
	UFUNCTION()
	void OnRep_TerminalState();

	/** Writes the per-terminal replicated state from the authoritative one (server). */
	void CaptureReplicatedState();

	void LogTopology( const TCHAR* stage );

	/**
	 * Prints the mesh's raw bounds and centre, the transform the editor script wrote, and the two
	 * numbers that say whether the fit landed: `fitted` and `residual`.
	 *
	 * The script chooses the mesh and FitMeshComponentToCube fits it; this is the check that the fit
	 * landed. Both halves are checked on purpose. `fitted` should be 2 * mHalfExtent on
	 * every axis, in the same units as the terminal cells logged beside it. `residual` should be
	 * zero — a mesh authored from a pivot at one end, like the painted beam segment, can be exactly
	 * the right SIZE and still sit half a metre off the terminals, and size alone would call that a
	 * pass.
	 */
	void LogMeshFit() const;

private:
	/**
	 * Caps and Outlets currently mounted on this host. Runtime state, rebuilt by the attachments
	 * themselves at their own BeginPlay — see IACPRTerminalHost::GetAttachmentList for why the
	 * ownership points that way rather than being saved here.
	 *
	 * Mutable so the const dismantle hook can prune dead entries while reading it.
	 */
	mutable TArray< TWeakObjectPtr< AActor > > mAttachments;

	/**
	 * The six faces. CDO subobjects, like every connection component in this mod: the Hoverpack
	 * cannot see a runtime-created connection, and neither can a saved Power Line.
	 */
	UPROPERTY()
	TArray< TObjectPtr< UACPRTerminalComponent > > mTerminals;

	/**
	 * The one power connection, at the cube centre: §5.4's Hoverpack node and the end of every
	 * Coupling's hidden edge. §4's "all six Junction terminals are common" is a fact about there
	 * being one connection, not six linked ones.
	 */
	UPROPERTY()
	TObjectPtr< UACPRPowerConnectionComponent > mPower = nullptr;

	/**
	 * The visible cube. In C++ rather than added to the Blueprint by hand, because the Junction is
	 * invisible without it — a setup step that silently produces nothing is the wrong kind of step.
	 *
	 * A mesh component on the buildable earns an empty zoop ISM in the hologram, and on the Rail
	 * that ISM could land ahead of AFGBeamHologram's first-match mBeamMesh bind and be scaled
	 * invisibly forever. A Junction's hologram is a plain AFGBuildableHologram with no such bind, so
	 * there is nothing to shadow — and AACPRJunctionHologram::BeginPlay logs the mesh count.
	 *
	 * The mesh asset itself is assigned by the editor script, which finds it rather than guessing a
	 * path. Empty here is a visible, fixable state; a wrong hard-coded path is a silent one.
	 */
	UPROPERTY( VisibleAnywhere, Category = "ACPR|Junction" )
	// UFGColoredInstanceMeshProxy for the indicators — see AACPRRail::mRailMeshComponent.
	TObjectPtr< class UFGColoredInstanceMeshProxy > mJunctionMesh = nullptr;
	/** Pushes the cue materials from the current state. Called on events only. */
	void RefreshIndicators();
	/** The aiming hologram's highlight; read by FACPRIndicators::Push. */
	FACPRHighlight mHighlight;

	/** A client's view; see AACPRRail::mTerminalStateRep. Untested in multiplayer. */
	UPROPERTY( ReplicatedUsing = OnRep_TerminalState )
	TArray< uint8 > mTerminalStateRep;

	UPROPERTY( ReplicatedUsing = OnRep_TerminalState )
	bool mHasPowerRep = false;


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
