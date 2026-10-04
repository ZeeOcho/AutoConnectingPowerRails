// Auto-Connecting Power Rails — our terminals as vanilla attachment points.
//
// Vanilla snaps buildables to each other through attachment points: a buildable offers
// FFGAttachmentPoints (AFGBuildable::GetAttachmentPoints, FGBuildable.h:451), a hologram caches the
// ones made from UFGAttachmentPointComponents on its buildable as its own LOCAL points
// (FGBuildableHologram.h:526), and on every placement frame the hologram filters the target's points,
// selects a candidate for one of its local points, and mates the two with its own transform
// (FilterAttachmentPoints :217, SelectCandidateForAttachment :407, CreateAttachmentPointTransform :413).
// Whether two points may mate is the TYPE's decision (UFGAttachmentPointType::CanAttach,
// FGAttachmentPoint.h:44).
//
// This file gives a terminal that shape: a point type that mates only with itself and only when the
// terminal behind the target is open (§4, invariant 5), a way to make a point from a terminal's local
// frame, and the reverse lookup from a point to the terminal it stands for. The hosts offer their
// terminals through it; the Junction hologram and the Rail hologram aimed at a Junction let vanilla
// pick, place and record the snap — its mating transform is identical to §6.1's on every face. One
// exception: AFGBeamHologram takes a private path for a beam target and never reaches the generic
// code, so a Rail hologram aimed at a Rail end keeps its own picker for that one target class — the
// same points, the same openness test, a second entry.

#pragma once

#include "CoreMinimal.h"
#include "FGAttachmentPoint.h"
#include "FGBuildableBeam.h"
#include "ACPRAttachment.generated.h"

class UACPRTerminalComponent;
class UFGAttachmentPointComponent;
class IACPRTerminalHost;

/**
 * The type every ACPR terminal point carries. Mates with its own type only (vanilla's default).
 *
 * Derived from the beam's point type rather than the base: the beam hologram's own chain code looks
 * for a UFGBeamAttachmentPoint by class, and a Rail is a beam. CanAttach gates on the terminal's
 * state either way.
 */
UCLASS()
class AUTOCONNECTINGPOWERRAILS_API UACPRTerminalAttachmentType : public UFGBeamAttachmentPoint
{
	GENERATED_BODY()

public:
	/**
	 * §4 / invariant 5: a point mates only with a target whose terminal is open and not the private
	 * mounting interface. The terminal is found from the point (FACPRAttachment::TerminalFor); a point
	 * no terminal answers to is refused.
	 */
	virtual bool CanAttach_Implementation( const FFGAttachmentPoint& point, const FFGAttachmentPoint& targetPoint ) const override;
};

struct AUTOCONNECTINGPOWERRAILS_API FACPRAttachment
{
	/**
	 * Sets the two private fields of a UFGAttachmentPointComponent — its type (ours) and its usage —
	 * by reflection, since both are EditDefaultsOnly with no setter. Called from the hosts' constructors
	 * on their CDO subobjects, so a clean checkout needs no blueprint edit.
	 */
	static void ConfigureComponent( UFGAttachmentPointComponent* component, bool hologramToo );

	/** A point for terminal `index` of `host`: the terminal's local frame, our type, `owner`. */
	static bool MakeTerminalPoint( AActor* owner, const IACPRTerminalHost* host, uint8 index, FFGAttachmentPoint& out_point );

	/**
	 * The terminal a point stands for: the owner's terminal whose local frame (GetTerminalLocalFrame)
	 * coincides with the point's relative transform within 1 uu. Null for a point that is not ours.
	 */
	static UACPRTerminalComponent* TerminalFor( const FFGAttachmentPoint& point );
};
