// Auto-Connecting Power Rails — the Power Rail Junction.

#include "ACPRJunction.h"

#include "ACPRAttachment.h"
#include "FGAttachmentPointComponent.h"

#include "FGColoredInstanceMeshProxy.h"
#include "FGSwatchGroup.h"

#include "ACPRDesignerSpace.h"
#include "ACPRPowerConnectionComponent.h"
#include "ACPRTerminalComponent.h"
#include "ACPRTerminalRegistry.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

/**
 * Index -> (name, local rotation), in the order the save format fixes.
 *
 * The rotation is what points the terminal's own +X outward along that face's axis, because
 * UACPRTerminalComponent::GetOutwardAxis is local +X by the Rail's convention. Keeping one
 * convention across both hosts is what lets §6.1's opposing test be the same integer comparison
 * for a Rail-to-Rail join and a Rail-to-Junction one.
 */
static const struct { const TCHAR* Name; FVector Axis; FRotator Rotation; } ACPRJunctionFaces[] =
{
	{ TEXT( "ACPRTerminalPosX" ), FVector(  1,  0,  0 ), FRotator(   0.0f,    0.0f, 0.0f ) },
	{ TEXT( "ACPRTerminalNegX" ), FVector( -1,  0,  0 ), FRotator(   0.0f,  180.0f, 0.0f ) },
	{ TEXT( "ACPRTerminalPosY" ), FVector(  0,  1,  0 ), FRotator(   0.0f,   90.0f, 0.0f ) },
	{ TEXT( "ACPRTerminalNegY" ), FVector(  0, -1,  0 ), FRotator(   0.0f,  -90.0f, 0.0f ) },
	{ TEXT( "ACPRTerminalPosZ" ), FVector(  0,  0,  1 ), FRotator(  90.0f,    0.0f, 0.0f ) },
	{ TEXT( "ACPRTerminalNegZ" ), FVector(  0,  0, -1 ), FRotator( -90.0f,    0.0f, 0.0f ) },
};

static_assert( UE_ARRAY_COUNT( ACPRJunctionFaces ) == 6, "Six faces, and the order is save format." );

AACPRJunction::AACPRJunction()
{
	// Every connection component is a CDO subobject. That is the difference between working and
	// silently broken — the Hoverpack cannot see a runtime-created connection, and neither can a
	// saved Power Line looking for the component it ends on.
	for( int32 i = 0; i < TerminalCount; ++i )
	{
		if( UACPRTerminalComponent* terminal =
				CreateDefaultSubobject< UACPRTerminalComponent >( FName( ACPRJunctionFaces[ i ].Name ) ) )
		{
			mTerminals.Add( terminal );
		}

		// The face as a vanilla attachment point, at the CDO's half extent (BeginPlay and the blueprint
		// path re-place it from the live value through RefreshAttachmentPoints).
		if( UFGAttachmentPointComponent* attachment = CreateDefaultSubobject< UFGAttachmentPointComponent >(
				FName( *FString::Printf( TEXT( "ACPRAttach_%s" ), ACPRJunctionFaces[ i ].Name + 12 ) ) ) )
		{
			attachment->SetRelativeLocationAndRotation( ACPRJunctionFaces[ i ].Axis * mHalfExtent, ACPRJunctionFaces[ i ].Rotation );
			// All six are targets on the built Junction; only PosX is a LOCAL point on the hologram, so
			// vanilla always mates that face — the face §7.2's roll turns about (PlaceOnTerminal), which is
			// what keeps vanilla's placement and our roll writing the same transform.
			FACPRAttachment::ConfigureComponent( attachment, i == 0 );
			mFaceAttachments.Add( attachment );
		}
	}

	// The one power connection, at the cube centre (§5.4's node, and the end of every Coupling's
	// hidden edge). Hidden, no wire budget. Named as the Wall Outlet's, for §15's redirect.
	mPower = CreateDefaultSubobject< UACPRPowerConnectionComponent >( ACPR_POWER_CONNECTION_NAME );

	// The visible cube. Its mesh is assigned by the editor script, and its transform is deliberately
	// not set here: a constant scale and offset is correct for exactly one mesh and silently wrong
	// for any other. FitMeshComponentToCube measures the chosen mesh's bounds and writes the scale
	// and offset that fit it to 2 * mHalfExtent, so the visible cube is the cube the terminals sit
	// on the faces of, whatever mesh is chosen. Identity here means an unfitted mesh is visibly the
	// wrong size rather than plausibly the right one, and BeginPlay prints the fitted extent either
	// way. The terminals sit at +/-mHalfExtent whatever the mesh does, so a wrong fit is cosmetic.
	//
	// The default swatch is the Foundation's. AFGBuildable::mSwatchGroup decides which of the
	// player's per-category default swatches a fresh build wears; the Standard group's is FICSIT
	// orange, and that orange accent fights the amber and white of the indicators, so a player
	// cannot tell which colours mean what. The Foundation group's default is the dark grey every
	// foundation wears, so an unpainted ACPR part reads as dark infrastructure and the accent only
	// appears when someone paints it. Any swatch still applies. UFGSwatchGroup_FicsitFoundation:
	// FGSwatchGroup.h:65. A blueprint that overrides mSwatchGroup wins, and the push line's swatch=
	// field is what says which one took.
	mSwatchGroup = UFGSwatchGroup_FicsitFoundation::StaticClass();

	mJunctionMesh = CreateDefaultSubobject< UFGColoredInstanceMeshProxy >( TEXT( "JunctionMesh" ) );
	FACPRIndicators::ConfigureProxy( mJunctionMesh );

	// RootComponent is normally null in a buildable's C++ constructor — the root is authored on the
	// Blueprint leaf — so this is best effort and BeginPlay finishes whatever is left unattached.
	if( USceneComponent* root = GetRootComponent() )
	{
		for( UACPRTerminalComponent* terminal : mTerminals )
		{
			if( terminal ) { terminal->SetupAttachment( root ); }
		}
		if( mPower )         { mPower->SetupAttachment( root ); }
		if( mJunctionMesh )  { mJunctionMesh->SetupAttachment( root ); }
		// BuildingMesh: the profile AACPRRail sets on its three components, and the one vanilla's
		// instance data carries. An explicit, matching profile is what the build gun's trace sees,
		// so every ACPR part can be aimed at the same way.
		if( mJunctionMesh ) { mJunctionMesh->SetCollisionProfileName( TEXT( "BuildingMesh" ) ); }
	}
}

void AACPRJunction::GetChildDismantleActors_Implementation( TArray< AActor* >& out_ChildDismantleActors ) const
{
	Super::GetChildDismantleActors_Implementation( out_ChildDismantleActors );

	// Appended rather than replacing, so whatever vanilla already considers a child of this buildable
	// survives. The mod's own children are the Caps and Outlets that registered themselves.
	FACPRAttachments::Collect( const_cast< AACPRJunction* >( this ), out_ChildDismantleActors );
}

bool AACPRJunction::ShouldSave_Implementation() const
{
	return true;
}

void AACPRJunction::Dismantle_Implementation()
{
	FACPRCostScope cost( EACPRCost::Dismantle );
	// Vanilla's mass dismantle calls this once per actor in the selection for every child it
	// collects, and a blueprint proxy makes every member a child of every other — one Outlet can be
	// dismantled hundreds of times in one frame, once per selected actor. Our release runs on the
	// first call only: nothing re-couples a dismantling actor, so the repeats have nothing to do but
	// reach Super. The count is logged once so the pattern stays visible.
	if( ++mDismantleCalls > 1 )
	{
		if( mDismantleCalls == 2 )
		{
			ACPR_LOG( Verbose, JUNC,
				TEXT( "%s Dismantle called again in the same dismantle — vanilla repeats it for every selected actor sharing a blueprint proxy; once was enough" ),
				*GetName() );
		}
		Super::Dismantle_Implementation();
		return;
	}
	// See AACPRRail::Dismantle_Implementation. A Junction has six terminals rather than two and is
	// the more visible case: dismantling one in a manifold would otherwise leave up to six neighbours
	// reading Coupled for the length of the effect.
	FACPRCoupling::ReleaseAll( this, this );

	Super::Dismantle_Implementation();
}


void AACPRJunction::BeginPlay()
{
	FACPRCostScope cost( EACPRCost::BeginPlayJunction );
	Super::BeginPlay();

	ACPR_LOG( Verbose, JUNC,
		TEXT( "%s BeginPlay | BUILD=%s | halfExtent=%.1f | authority=%d" ),
		*GetName(), ACPR_BUILD_STAMP(), mHalfExtent, HasAuthority() ? 1 : 0 );

	// Fit first, then report. The script assigns the mesh; the size is decided here, in the process
	// that renders it, so the number the fit is computed from and the number the renderer uses are
	// the same number.
	FitMeshComponentToCube( mJunctionMesh, mHalfExtent );
	LogMeshFit();

	// Terminals, power, register, resolve — coupling at registration (see AACPRRail::BeginPlay).
	PlaceTerminals();
	RefreshAttachmentPoints();
	RegisterTerminals();
	FACPRCoupling::Resolve( this, this );
	LogTopology( TEXT( "BeginPlay" ) );

	// The one Display line per Junction; LogTopology's seven are Verbose.
	FACPRCoupling::LogHostSummary( this, this, ACPR_TAG_TEXT( JUNC ), TEXT( "BeginPlay" ), mBlueprintState );

	// Indicators: once here, then only on events; power changes have a delegate.
	CaptureReplicatedState();
	RefreshIndicators();
}

void AACPRJunction::GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );
	DOREPLIFETIME( AACPRJunction, mTerminalStateRep );
	DOREPLIFETIME( AACPRJunction, mHasPowerRep );
}

bool AACPRJunction::HasNetworkPower() const
{
	if( !HasAuthority() )
	{
		return mHasPowerRep;
	}
	return mPower && mPower->HasPower();
}

void AACPRJunction::CaptureReplicatedState()
{
	if( !HasAuthority() )
	{
		return;
	}
	mTerminalStateRep.SetNum( mTerminals.Num() );
	for( int32 i = 0; i < mTerminals.Num(); ++i )
	{
		mTerminalStateRep[ i ] = static_cast< uint8 >( mTerminals[ i ] ? mTerminals[ i ]->GetTerminalState() : EACPRTerminalState::Open );
	}
	mHasPowerRep = mPower && mPower->HasPower();
}

void AACPRJunction::OnRep_TerminalState()
{
	for( int32 i = 0; i < mTerminals.Num() && i < mTerminalStateRep.Num(); ++i )
	{
		if( mTerminals[ i ] )
		{
			mTerminals[ i ]->SetReplicatedState( static_cast< EACPRTerminalState >( mTerminalStateRep[ i ] ) );
		}
	}
	RefreshIndicators();
}

void AACPRJunction::OnTerminalStateChanged( int32 /*index*/ )
{
	if( HasActorBegunPlay() )
	{
		CaptureReplicatedState();
		RefreshIndicators();
	}
}

void AACPRJunction::HandleHasPowerChanged( bool /*hasPower*/ )
{
	OnTerminalStateChanged( INDEX_NONE );
}

void AACPRJunction::PreSaveGame_Implementation( int32 saveVersion, int32 gameVersion )
{
	Super::PreSaveGame_Implementation( saveVersion, gameVersion );
	mSavedOnce = true;  // See AACPRRail::PreSaveGame_Implementation.
}

void AACPRJunction::SetHighlight( const FACPRHighlight& highlight )
{
	if( highlight != mHighlight )
	{
		mHighlight = highlight;
		RefreshIndicators();
	}
}

void AACPRJunction::RefreshIndicators()
{
	// Six face cues, one per terminal, named for the face in kTerminalLayout order (PosX, NegX, PosY,
	// NegY, PosZ, NegZ — acpr_meshes.py names them the same way): per-face state, as a named slot.
	// Plus the six socket floors, each dark once its own terminal is occupied.
	FACPRIndicatorSlot slots[ 12 ];
	for( int32 i = 0; i < 6; ++i )
	{
		slots[ i ]     = { mJunctionMesh.Get(), ACPRSlot::CueFace( i ),   i, false };
		slots[ 6 + i ] = { mJunctionMesh.Get(), ACPRSlot::PowerFace( i ), i, true };
	}
	FACPRIndicators::Push( this, this, TArrayView< const FACPRIndicatorSlot >( slots, 12 ), ACPR_TAG_TEXT( JUNC ) );
}

FVector AACPRJunction::FitMeshComponentToCube( UStaticMeshComponent* comp, float halfExtent )
{
	if( !comp )
	{
		return FVector::ZeroVector;
	}

	const UStaticMesh* mesh = comp->GetStaticMesh();
	if( !mesh )
	{
		return FVector::ZeroVector;
	}

	// GetBounds().Origin / .BoxExtent are plain public members of FBoxSphereBounds rather than
	// methods, which is the one form that cannot fail on a name that cannot be checked against a
	// local header — the engine headers are not in the SML set.
	const FBoxSphereBounds bounds = mesh->GetBounds();
	const FVector size = bounds.BoxExtent * 2.0;
	const FVector centre = bounds.Origin;

	const double target = 2.0 * static_cast< double >( halfExtent );

	// Per axis, and guarded: a degenerate axis keeps scale 1 rather than producing an infinity that
	// would take the whole component off-screen and look like the mesh had failed to load.
	FVector scale( 1.0, 1.0, 1.0 );
	if( FMath::Abs( size.X ) > 0.01 ) { scale.X = target / size.X; }
	if( FMath::Abs( size.Y ) > 0.01 ) { scale.Y = target / size.Y; }
	if( FMath::Abs( size.Z ) > 0.01 ) { scale.Z = target / size.Z; }

	comp->SetRelativeScale3D( scale );

	// Centring is the other half. A mesh authored from a pivot at one end can be exactly the right
	// size and still sit off the terminals; -centre * scale puts the mesh's own bounds on the actor
	// origin whatever its pivot.
	comp->SetRelativeLocation( FVector( -centre.X * scale.X,
	                                    -centre.Y * scale.Y,
	                                    -centre.Z * scale.Z ) );

	return FVector( size.X * scale.X, size.Y * scale.Y, size.Z * scale.Z );
}

void AACPRJunction::LogMeshFit() const
{
	if( !mJunctionMesh )
	{
		ACPR_LOG( Warning, JUNC,
			TEXT( "no JunctionMesh component — Build_PowerRailJunction's parent class is "
			      "wrong, and the Junction will be invisible." ) );
		return;
	}

	const UStaticMesh* mesh = mJunctionMesh->GetStaticMesh();
	if( !mesh )
	{
		ACPR_LOG( Warning, JUNC,
			TEXT( "JunctionMesh has no mesh assigned — run acpr_bind_meshes.py." ) );
		return;
	}

	// The one line that catches a bad mesh swap in game rather than by eye.
	//
	// This reports what the fit actually produced, in the same units as the terminal cells above
	// it. fitted should read 100 x 100 x 100 for a 0.5 m half extent — anything else and the visible
	// cube is not the cube the faces belong to, which is a thing you would otherwise only notice by
	// looking at it and doubting yourself.
	// GetBounds().Origin / .BoxExtent rather than GetBoundingBox(): these two are plain public
	// members of FBoxSphereBounds rather than methods, so this is the one form that cannot fail on
	// a name that cannot be checked against a local header.
	const FBoxSphereBounds bounds = mesh->GetBounds();
	const FVector size = bounds.BoxExtent * 2.0;
	const FVector centre = bounds.Origin;

	const FVector scale = mJunctionMesh->GetRelativeScale3D();
	const FVector offset = mJunctionMesh->GetRelativeLocation();

	const FVector fitted( size.X * scale.X, size.Y * scale.Y, size.Z * scale.Z );

	// Both halves of the fit, because size alone is only half a test. A mesh authored from a pivot
	// at one end can be exactly the right size and still sit visibly off the terminals. A centred
	// cube satisfies offset == -centre * scale, so the residual is what should read zero.
	const FVector residual( offset.X + centre.X * scale.X,
	                        offset.Y + centre.Y * scale.Y,
	                        offset.Z + centre.Z * scale.Z );

	// A second, independent measurement.
	//
	// `fitted` is arithmetic on GetBounds(): it says what the fit intended. `world` is the component's
	// own render bound after the transform, which is what is actually drawn. A scale computed from
	// one measurement against a mesh the game measures differently passes every check that consults
	// the same wrong number (an editor-side measurement of 256 uu against the game's 110.692 for the
	// same asset, for instance). Two sources that can disagree are the only way that shows up in a
	// log rather than by eye.
	//
	// Both should read 2 * mHalfExtent on every axis. If `fitted` is right and `world` is not, then
	// GetBounds() is not the bound the renderer uses and the fit needs to be driven from `world`.
	// CalcBounds with rotation removed, not the cached world Bounds. The component's `Bounds` is a
	// world-space AABB, so a correctly fitted cube at 45° yaw re-encloses to 141.4 x 141.4 x 100 and
	// this line would shout "the visible cube is NOT 100 uu" about a cube that is exactly right —
	// the worst possible failure for a check whose only job is catching a real size error. Feeding
	// CalcBounds a scale-only transform keeps the independence (it is still the renderer's own
	// bounds code rather than a second copy of the arithmetic above) and drops the rotation.
	const FVector world = mJunctionMesh->CalcBounds(
			FTransform( FQuat::Identity, FVector::ZeroVector, mJunctionMesh->GetRelativeScale3D() ) )
		.BoxExtent * 2.0;

	const double target = 2.0 * static_cast< double >( mHalfExtent );
	const bool fitsFitted = FMath::IsNearlyEqual( fitted.X, target, 1.0 )
	                     && FMath::IsNearlyEqual( fitted.Y, target, 1.0 )
	                     && FMath::IsNearlyEqual( fitted.Z, target, 1.0 );
	const bool fitsWorld = FMath::IsNearlyEqual( world.X, target, 1.0 )
	                    && FMath::IsNearlyEqual( world.Y, target, 1.0 )
	                    && FMath::IsNearlyEqual( world.Z, target, 1.0 );

	// Verbosity is a compile-time token in UE_LOG, so the state goes in the line and the alarm is a
	// separate one. Two lines is the cost of a warning that a log filter can actually find.
	ACPR_LOG( Verbose, JUNC,
		TEXT( "mesh=%s | raw=%s centre=%s | scale=%s offset=%s "
		      "-> fitted=%s residual=%s | world=%s | want=%.0f cube | ok(fitted=%d world=%d)" ),
		*mesh->GetName(),
		*size.ToString(),
		*centre.ToString(),
		*scale.ToString(),
		*offset.ToString(),
		*fitted.ToString(),
		*residual.ToString(),
		*world.ToString(),
		2.0f * mHalfExtent,
		fitsFitted ? 1 : 0,
		fitsWorld ? 1 : 0 );

	if( !fitsFitted || !fitsWorld )
	{
		ACPR_LOG( Warning, JUNC,
			TEXT( "the visible cube is NOT %.0f uu. fitted=%s world=%s. A Junction that "
			      "is not the size of its terminal spacing cannot meet a Rail — the faces are at "
			      "+/-%.0f whatever the mesh does." ),
			2.0f * mHalfExtent, *fitted.ToString(), *world.ToString(), mHalfExtent );
	}
}

void AACPRJunction::PostLoadGame_Implementation( int32 saveVersion, int32 gameVersion )
{
	Super::PostLoadGame_Implementation( saveVersion, gameVersion );

	// Invariant 6's switch: a loaded Junction replays its saved Couplings and never searches for
	// new ones. Two capped faces meeting are coincident and opposing, and §6.2 requires they stay
	// uncoupled — which re-deriving topology from geometry on every load would quietly undo.
	mCameFromSave = true;

	LogTopology( TEXT( "PostLoadGame" ) );
}

AFGBuildableBlueprintDesigner* AACPRJunction::GetHostDesigner() const
{
	return mBlueprintDesigner.Get();
}

void AACPRJunction::RefreshAttachmentPoints()
{
	mTerminalPoints.Reset();
	for( int32 i = 0; i < TerminalCount; ++i )
	{
		FFGAttachmentPoint point;
		if( FACPRAttachment::MakeTerminalPoint( this, this, static_cast< uint8 >( i ), point ) )
		{
			mTerminalPoints.Add( point );
		}
		FTransform frame;
		if( mFaceAttachments.IsValidIndex( i ) && mFaceAttachments[ i ] && GetTerminalLocalFrame( static_cast< uint8 >( i ), frame ) )
		{
			if( USceneComponent* root = GetRootComponent() )
			{
				if( mFaceAttachments[ i ]->GetAttachParent() != root )
				{
					mFaceAttachments[ i ]->AttachToComponent( root, FAttachmentTransformRules::KeepRelativeTransform );
				}
			}
			mFaceAttachments[ i ]->SetRelativeTransform( frame );
		}
	}
}

void AACPRJunction::GetAttachmentPoints( TArray< const FFGAttachmentPoint* >& out_points ) const
{
	for( const FFGAttachmentPoint& point : mTerminalPoints )
	{
		out_points.Add( &point );
	}
}

bool AACPRJunction::GetTerminalLocalFrame( uint8 index, FTransform& out_frame ) const
{
	// PlaceTerminals' placement, from mHalfExtent and the fixed face table, with no component read.
	if( index >= UE_ARRAY_COUNT( ACPRJunctionFaces ) )
	{
		return false;
	}

	out_frame = FTransform( ACPRJunctionFaces[ index ].Rotation.Quaternion(),
	                        ACPRJunctionFaces[ index ].Axis * mHalfExtent );
	return true;
}

void AACPRJunction::PreSerializedToBlueprint()
{
	Super::PreSerializedToBlueprint();

	ACPR_LOG( Verbose, BP,
		TEXT( "%s PreSerializedToBlueprint | space=%s | records=%s" ),
		*GetName(), *FACPRSpace::Describe( GetHostDesigner() ),
		*FACPRCoupling::DescribeRecords( this, this ) );
}

void AACPRJunction::PostSerializedFromBlueprint( bool isBlueprintWorld )
{
	Super::PostSerializedFromBlueprint( isBlueprintWorld );
	RefreshAttachmentPoints();

	mBlueprintState = isBlueprintWorld ? TEXT( "bpworld" ) : TEXT( "placed" );
	mPlacedFromBlueprint = !isBlueprintWorld;

	// Fitted here for the blueprint world, like the Rail, Cap and Outlet: BeginPlay never runs
	// there, and the hologram copies what the components hold. Idempotent.
	if( isBlueprintWorld )
	{
		FitMeshComponentToCube( mJunctionMesh, mHalfExtent );
	}

	ACPR_LOG( Verbose, BP,
		TEXT( "%s PostSerializedFromBlueprint(isBlueprintWorld=%d) | begunPlay=%d fromSave=%d "
		      "| space=%s | records=%s" ),
		*GetName(), isBlueprintWorld ? 1 : 0,
		HasActorBegunPlay() ? 1 : 0, mCameFromSave ? 1 : 0,
		*FACPRSpace::Describe( GetHostDesigner() ),
		*FACPRCoupling::DescribeRecords( this, this ) );
}

UACPRTerminalComponent* AACPRJunction::GetTerminalAtIndex( uint8 index ) const
{
	return mTerminals.IsValidIndex( index ) ? mTerminals[ index ] : nullptr;
}

void AACPRJunction::AddSavedCoupling( const FACPRSavedCoupling& coupling )
{
	for( const FACPRSavedCoupling& existing : mSavedCouplings )
	{
		if( existing.OtherActor == coupling.OtherActor &&
		    existing.LocalTerminalIndex == coupling.LocalTerminalIndex &&
		    existing.OtherTerminalIndex == coupling.OtherTerminalIndex )
		{
			return;
		}
	}

	mSavedCouplings.Add( coupling );
}

void AACPRJunction::PlaceTerminals()
{
	USceneComponent* root = GetRootComponent();
	if( !root )
	{
		ACPR_LOG( Error, JUNC,
			TEXT( "%s has no RootComponent at BeginPlay — terminals not placed, so "
			      "every quantized cell below would be world-origin nonsense." ),
			*GetName() );
		return;
	}

	// B.2: "Keep the Junction actor origin at the exact centre of its 1 m cube." So a face centre
	// is exactly mHalfExtent out along its axis, and §11's 1 m lane pitch falls out of the cube
	// rather than being asserted somewhere else.
	const FIntVector centreCell = UACPRTerminalComponent::QuantizeLocation( GetActorLocation() );

	for( int32 i = 0; i < mTerminals.Num() && i < (int32)UE_ARRAY_COUNT( ACPRJunctionFaces ); ++i )
	{
		UACPRTerminalComponent* terminal = mTerminals[ i ];
		if( !terminal )
		{
			continue;
		}

		if( terminal->GetAttachParent() != root )
		{
			terminal->AttachToComponent( root, FAttachmentTransformRules::KeepRelativeTransform );
		}

		terminal->SetRelativeLocationAndRotation(
			ACPRJunctionFaces[ i ].Axis * mHalfExtent,
			ACPRJunctionFaces[ i ].Rotation );
	}

	// The outward direction is DERIVED from quantized positions, exactly as B.3 requires for the
	// Rail — "derive a Rail's outward axis from its two quantized endpoints rather than storing a
	// separately quantized angle". Here the two points are the face centre and the cube centre.
	//
	// Doing it this way rather than rotating the authored axis is what keeps a Junction's outward
	// directions in the same number system as a Rail's, so §6.1's opposing test stays an integer
	// equality across the two classes. It also survives a rotated Junction: a 45° roll gives a
	// delta like (35,35,0), which reduces to (1,1,0) — still exact, still comparable.
	for( int32 i = 0; i < mTerminals.Num(); ++i )
	{
		if( UACPRTerminalComponent* terminal = mTerminals[ i ] )
		{
			terminal->SetQuantizedOutward(
				UACPRTerminalComponent::ReduceDirection( terminal->GetQuantizedLocation() - centreCell ) );
		}
	}

	// The one power connection, at the cube centre, its power delegate bound (see AACPRRail::PlacePowerConnection).
	if( mPower )
	{
		if( mPower->GetAttachParent() != root )
		{
			mPower->AttachToComponent( root, FAttachmentTransformRules::KeepRelativeTransform );
		}
		mPower->SetRelativeLocation( FVector::ZeroVector );
		mPower->OnHasPowerChanged.BindUObject( this, &AACPRJunction::HandleHasPowerChanged );
	}
}

void AACPRJunction::RegisterTerminals()
{
	UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this );
	if( !registry )
	{
		ACPR_LOG( Warning, JUNC,
			TEXT( "%s found no terminal registry — it will couple to nothing." ),
			*GetName() );
		return;
	}

	for( UACPRTerminalComponent* terminal : mTerminals )
	{
		if( terminal ) { registry->RegisterTerminal( terminal ); }
	}
}

void AACPRJunction::LogTopology( const TCHAR* stage )
{
	// One line per face, because six terminals on one line is unreadable and the whole point of
	// this log is that a person can see at a glance which faces are open and which are coupled.
	for( int32 i = 0; i < mTerminals.Num(); ++i )
	{
		ACPR_LOG( Verbose, JUNC,
			TEXT( "%s %s | [%d] %s" ),
			*GetName(), stage, i,
			mTerminals[ i ] ? *mTerminals[ i ]->Describe() : TEXT( "<null>" ) );
	}

	int32 registeredTerminals = 0;
	int32 occupiedCells       = 0;
	if( UACPRTerminalRegistry* registry = UACPRTerminalRegistry::Get( this ) )
	{
		registry->GetCounts( registeredTerminals, occupiedCells );
	}

	ACPR_LOG( Verbose, JUNC,
		TEXT( "%s %s | root=%s | couplings=%d fromSave=%d savedOnce=%d | power: %s | "
		      "registry: %d terminals in %d cells | authority=%d" ),
		*GetName(), stage,
		GetRootComponent() ? *GetRootComponent()->GetName() : TEXT( "<null>" ),
		mSavedCouplings.Num(), mCameFromSave ? 1 : 0, mSavedOnce ? 1 : 0,
		mPower ? *mPower->Describe() : TEXT( "<null>" ),
		registeredTerminals, occupiedCells,
		HasAuthority() ? 1 : 0 );
}
