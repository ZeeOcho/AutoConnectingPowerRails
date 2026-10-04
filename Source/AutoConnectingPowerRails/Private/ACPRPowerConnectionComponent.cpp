// Auto-Connecting Power Rails — the one power connection a host owns.

#include "ACPRPowerConnectionComponent.h"

#include "AutoConnectingPowerRails.h"
#include "FGPowerCircuit.h"

UACPRPowerConnectionComponent::UACPRPowerConnectionComponent()
{
	// Set next to the base class it promises: a member of a power circuit IS a UFGPowerConnectionComponent.
	mCircuitType = UFGPowerCircuit::StaticClass();

	// The default is a Rail's or a Junction's: hidden, no Power Line can take it. The Outlet reconfigures
	// its socket in its constructor.
	mIsHiddenConnection = true;
	mMaxNumConnectionLinks = 0;
	mAllowDaisyChaining = false;

	PrimaryComponentTick.bCanEverTick = false;
}

void UACPRPowerConnectionComponent::ConfigureAsWireSocket( int32 maxWires )
{
	// A budget of zero means "not a wire socket at all", which also has to mean hidden — a visible
	// connection that refuses every wire is a thing the player can aim at and get nothing from.
	mMaxNumConnectionLinks = FMath::Max( 0, maxWires );
	mIsHiddenConnection = ( mMaxNumConnectionLinks == 0 );
}

void UACPRPowerConnectionComponent::MakePreviewCopyInert()
{
	const int32 circuit = mCircuitID;
	const int32 hidden = mHiddenConnections.Num();
	const int32 wires = mWires.Num();

	mWires.Empty();
	mNumWiresConnected = 0;
	mHiddenConnections.Empty();
	mCircuitID = INDEX_NONE;

	ACPR_LOG( Verbose, POWER,
		TEXT( "%s is a preview copy | cleared circuit=%d hidden=%d wires=%d" ),
		*GetName(), circuit, hidden, wires );
}

FString UACPRPowerConnectionComponent::Describe() const
{
	// circuit= is NOT the acceptance test: UFGPowerCircuit overrides IsTrivial (FGPowerCircuit.h:272) and
	// the subsystem discards a circuit with nothing to simulate, so an unpowered chain correctly reports
	// -1. hidden= (the Couplings) and wires= (actual Power Lines) mean something without a generator.
	return FString::Printf( TEXT( "%s circuit=%d pwr=%d hidden=%d wires=%d/%d%s" ),
		*GetName(), GetCircuitID(), HasPower() ? 1 : 0,
		GetNumHiddenConnections(), GetNumConnections(), GetMaxNumConnections(),
		IsHidden() ? TEXT( "" ) : TEXT( " socket" ) );
}
