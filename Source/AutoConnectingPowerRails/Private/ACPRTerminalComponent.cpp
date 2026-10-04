// Auto-Connecting Power Rails — the terminal.

#include "ACPRTerminalComponent.h"

#include "ACPRDesignerSpace.h"
#include "ACPRTerminalHost.h"
#include "ACPRTerminalRegistry.h"
#include "AutoConnectingPowerRails.h"

UACPRTerminalComponent::UACPRTerminalComponent()
{
	// A terminal is a scene component with no visual. It never ticks.
	PrimaryComponentTick.bCanEverTick = false;
}

void UACPRTerminalComponent::EndPlay( const EEndPlayReason::Type endPlayReason )
{
	if( UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this ) )
	{
		registry->UnregisterTerminal( this );
	}

	Super::EndPlay( endPlayReason );
}

void UACPRTerminalComponent::OnComponentDestroyed( bool destroyingHierarchy )
{
	// Every destruction path, not only the player's dismantle. A terminal that goes must reopen its
	// partner and drop the partner's mirror record, or the survivor reads Coupled to a dead component
	// until GC and carries a record naming a dead actor into the next save. FACPRCoupling::ReleaseAll
	// runs at dismantle time (seconds earlier, when the player is waiting); this is the guarantee
	// behind it, idempotent with it.
	if( mCoupledTo )
	{
		AActor* owner = GetOwner();
		IACPRTerminalHost* host = Cast< IACPRTerminalHost >( owner );
		const int32 index = host ? host->GetIndexOfTerminal( this ) : INDEX_NONE;
		if( host && index != INDEX_NONE )
		{
			FACPRCoupling::Release( owner, host, static_cast< uint8 >( index ), TEXT( "destroyed" ) );
		}
		else
		{
			// No host to name us: clear what can be cleared by hand.
			if( IsValid( mCoupledTo ) && mCoupledTo->GetCoupledTo() == this )
			{
				mCoupledTo->SetCoupledTo( nullptr );
			}
			mCoupledTo = nullptr;
		}
	}

	Super::OnComponentDestroyed( destroyingHierarchy );
}

EACPRTerminalState UACPRTerminalComponent::GetTerminalState() const
{
	// A client never sees a coupling form (invariant 13), so it reads what the host replicated.
	const AActor* owner = GetOwner();
	if( owner && !owner->HasAuthority() )
	{
		return mReplicatedState;
	}

	// Order matters, and it encodes invariant 4 rather than merely respecting it. A capped terminal
	// cannot also be coupled, so if both are somehow set the cap wins and the disagreement is
	// visible in Describe() below rather than silently resolved.
	if( mIsCapped )
	{
		return EACPRTerminalState::Capped;
	}

	if( mCoupledTo )
	{
		return EACPRTerminalState::Coupled;
	}

	return EACPRTerminalState::Open;
}

void UACPRTerminalComponent::SetCoupledTo( UACPRTerminalComponent* other )
{
	if( mCoupledTo == other )
	{
		return;
	}
	mCoupledTo = other;
	NotifyOwner();
}

void UACPRTerminalComponent::SetCapped( bool capped )
{
	if( mIsCapped == capped )
	{
		return;
	}
	mIsCapped = capped;
	NotifyOwner();
}

void UACPRTerminalComponent::SetReplicatedState( EACPRTerminalState state )
{
	if( mReplicatedState == state )
	{
		return;
	}
	mReplicatedState = state;
	NotifyOwner();
}

void UACPRTerminalComponent::NotifyOwner()
{
	// The event the indicators live on. The owner decides what to do with it; a host
	// with no cue surfaces ignores it.
	if( IACPRTerminalHost* host = Cast< IACPRTerminalHost >( GetOwner() ) )
	{
		const int32 index = host->GetIndexOfTerminal( this );
		host->OnTerminalStateChanged( index == INDEX_NONE ? INDEX_NONE : index );
	}
}

FIntVector UACPRTerminalComponent::QuantizeLocation( const FVector& worldLocation )
{
	// 1 uu cells. FVector is double-precision in UE5, so world coordinates in the 250,000 uu range
	// still resolve far below this quantum — the rounding here is removing placement noise, not
	// floating-point error.
	return FIntVector(
		FMath::RoundToInt( worldLocation.X ),
		FMath::RoundToInt( worldLocation.Y ),
		FMath::RoundToInt( worldLocation.Z ) );
}

FIntVector UACPRTerminalComponent::ReduceDirection( const FIntVector& delta )
{
	// Euclid, on the absolute values.
	auto gcd = []( int32 a, int32 b )
	{
		a = FMath::Abs( a );
		b = FMath::Abs( b );
		while( b != 0 )
		{
			const int32 t = b;
			b = a % b;
			a = t;
		}
		return a;
	};

	const int32 g = gcd( gcd( delta.X, delta.Y ), delta.Z );
	return g > 0 ? FIntVector( delta.X / g, delta.Y / g, delta.Z / g ) : FIntVector( 0, 0, 0 );
}

bool UACPRTerminalComponent::CanCoupleWith( const UACPRTerminalComponent* other, bool checkSpace ) const
{
	if( !other || other == this || other->GetOwner() == GetOwner() )
	{
		return false;
	}

	// §10.1, FIRST, because it is the cheapest question with the strongest answer: "Designer-to-world
	// and cross-Designer Couplings are rejected." Without it every other rule below would let a
	// Designer Rail couple to a world Rail.
	if( checkSpace && FACPRSpace::OfActor( GetOwner() ) != FACPRSpace::OfActor( other->GetOwner() ) )
	{
		return false;
	}

	// §4: "Only an open terminal accepts anything." Invariant 4 gives each terminal exactly one
	// occupant, so a terminal that already has one is not a candidate for a second.
	if( !IsOpen() || !other->IsOpen() )
	{
		return false;
	}

	// §6.1: "two exactly coincident, opposing, open terminals". Both halves are integer equalities.
	//
	// Coincident: the same cell. Not "within a tolerance of" the same cell — invariant 2 says
	// proximity never couples on its own, and an epsilon here is exactly how a coincidence rule
	// turns into a proximity rule by degrees.
	if( GetQuantizedLocation() != other->GetQuantizedLocation() )
	{
		return false;
	}

	// Opposing: the two reduced directions are exact negatives. Two Rails meeting end to end point
	// INTO each other, so (0,-1,0) meets (0,1,0). Two Rails meeting at a bend do not, and §5.1 is
	// explicit that "a bend requires a Junction".
	const FIntVector ours = GetQuantizedOutward();
	const FIntVector theirs = other->GetQuantizedOutward();

	if( ours == FIntVector( 0, 0, 0 ) || theirs == FIntVector( 0, 0, 0 ) )
	{
		return false;
	}

	return ours == FIntVector( -theirs.X, -theirs.Y, -theirs.Z );
}

FString UACPRTerminalComponent::Describe() const
{
	const TCHAR* state = TEXT( "?" );
	switch( GetTerminalState() )
	{
		case EACPRTerminalState::Open:    state = TEXT( "open" );    break;
		case EACPRTerminalState::Coupled: state = TEXT( "coupled" ); break;
		case EACPRTerminalState::Capped:  state = TEXT( "capped" );  break;
	}

	const FIntVector cell = GetQuantizedLocation();

	// The electrical fields live on the host's one connection (IACPRTerminalHost::GetPowerConnection);
	// this line is the terminal's own facts: cell, reduced outward, occupant.
	return FString::Printf(
		TEXT( "%s[%s] cell=(%d,%d,%d) out=(%d,%d,%d) to=%s%s%s" ),
		*GetName(), state,
		cell.X, cell.Y, cell.Z,
		mQuantizedOutward.X, mQuantizedOutward.Y, mQuantizedOutward.Z,
		mCoupledTo ? *FString::Printf( TEXT( "%s.%s" ),
			mCoupledTo->GetOwner() ? *mCoupledTo->GetOwner()->GetName() : TEXT( "?" ),
			*mCoupledTo->GetName() ) : TEXT( "-" ),
		mIsPrivateInterface ? TEXT( " private" ) : TEXT( "" ),
		( mIsCapped && mCoupledTo ) ? TEXT( "  *** CAPPED AND COUPLED — invariant 4 violated ***" )
		                            : TEXT( "" ) );
}
