// Auto-Connecting Power Rails — the Insulated Terminal Cap.

#include "ACPRCap.h"

#include "FGColoredInstanceMeshProxy.h"
#include "FGSwatchGroup.h"
#include "TimerManager.h"

#include "ACPRTerminalComponent.h"
#include "ACPRTerminalHost.h"
#include "AutoConnectingPowerRails.h"
#include "Net/UnrealNetwork.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

AACPRCap::AACPRCap()
{
	// The default swatch is the Foundation's. AFGBuildable::mSwatchGroup decides which of the
	// player's per-category default swatches a fresh build wears; the Standard group's is FICSIT
	// orange, and on an unpainted Rail that orange accent fights the amber and white of the
	// indicators, so the colours stop meaning anything. The Foundation group's default is the dark
	// grey every foundation wears, so an unpainted ACPR part reads as dark infrastructure and the
	// accent only appears when someone paints it. Any swatch still applies.
	// UFGSwatchGroup_FicsitFoundation: FGSwatchGroup.h:65. A blueprint that overrides mSwatchGroup
	// wins, and the push line's swatch= field is what says which one took.
	mSwatchGroup = UFGSwatchGroup_FicsitFoundation::StaticClass();

	mCapMesh = CreateDefaultSubobject< UFGColoredInstanceMeshProxy >( TEXT( "CapMesh" ) );
	FACPRIndicators::ConfigureProxy( mCapMesh );

	// RootComponent is normally null in a buildable's C++ constructor — the root is authored on the
	// Blueprint leaf — so this is best effort and BeginPlay finishes what is left unattached.
	if( USceneComponent* root = GetRootComponent() )
	{
		if( mCapMesh ) { mCapMesh->SetupAttachment( root ); }
	// BuildingMesh, the same profile AACPRRail sets on its three components — and vanilla's own,
	// read off the instance data our blueprint carries (CollisionProfileName="BuildingMesh"). An
	// explicit, matching profile is what the aim trace sees, independent of where the mesh is.
		if( mCapMesh ) { mCapMesh->SetCollisionProfileName( TEXT( "BuildingMesh" ) ); }
	}
}

bool AACPRCap::ShouldSave_Implementation() const
{
	return true;
}

void AACPRCap::PostLoadGame_Implementation( int32 saveVersion, int32 gameVersion )
{
	Super::PostLoadGame_Implementation( saveVersion, gameVersion );

	// PostLoadGame runs BEFORE BeginPlay, so this flag is always set before ApplyCap reads it.
	mCameFromSave = true;
}

void AACPRCap::PostSerializedFromBlueprint( bool isBlueprintWorld )
{
	Super::PostSerializedFromBlueprint( isBlueprintWorld );

	// Fitted here for the blueprint world. AFGBlueprintHologram duplicates each blueprint-world
	// buildable's mesh components as they are at duplication time, and BeginPlay never runs in that
	// world. Everything this actor does to its meshes at BeginPlay — pick, scale, rotate — therefore
	// has to have happened by the end of this call, or the hologram shows the CDO's meshes:
	// unrotated, unscaled, and the Rail-sized Cap on a Junction face. Idempotent, so the placed
	// copy's BeginPlay doing it again costs nothing.
	if( isBlueprintWorld )
	{
		SetUpMeshes();
	}

	ACPR_LOG( Verbose, BP,
		TEXT( "%s PostSerializedFromBlueprint(isBlueprintWorld=%d) | begunPlay=%d fromSave=%d "
		      "| host=%s[%d] hostWorld=%s | capMesh=%s scale=%s rot=%s" ),
		*GetName(), isBlueprintWorld ? 1 : 0,
		HasActorBegunPlay() ? 1 : 0, mCameFromSave ? 1 : 0,
		IsValid( mHostActor ) ? *mHostActor->GetName() : TEXT( "<null>" ),
		static_cast< int32 >( mHostTerminalIndex ),
		!IsValid( mHostActor ) ? TEXT( "null" )
			: ( mHostActor->GetWorld() == GetWorld() ? TEXT( "same" ) : TEXT( "OTHER-WORLD" ) ),
		( mCapMesh && mCapMesh->GetStaticMesh() ) ? *mCapMesh->GetStaticMesh()->GetName() : TEXT( "<none>" ),
		mCapMesh ? *mCapMesh->GetRelativeScale3D().ToString() : TEXT( "-" ),
		mCapMesh ? *mCapMesh->GetRelativeRotation().ToString() : TEXT( "-" ) );
}

void AACPRCap::BeginPlay()
{
	FACPRCostScope cost( EACPRCost::BeginPlayCap );
	Super::BeginPlay();

	SetUpMeshes();

	// Applied at BeginPlay. The terminal is a CDO subobject, so it exists from construction, and its
	// position is irrelevant to whether it is occupied. A host that begins play after this Cap finds
	// the terminal already capped, so §6.1 cannot couple it (invariant 5); a host that began play
	// first restored its records — RestoreSaved refuses a capped end — and ApplyCap refuses a coupled
	// one. Both orders keep invariant 4 without a rule that mentions Caps.
	ApplyCap();

	// §9: "Dismantling the owning Rail or Junction dismantles the Cap with a full refund and visible
	// dependency preview." The host cannot know about us until we say so, and we are the side that
	// already had to save which host it is on — see IACPRTerminalHost::GetAttachmentList for why the
	// ownership points this way rather than the host keeping a saved list of its own.
	FACPRAttachments::Register( mHostActor, this );

	// A freshly built Cap wears its host's swatch. A loaded one keeps its own.
	if( !mCameFromSave )
	{
		FACPRIndicators::AdoptSwatch( this, mHostActor, ACPR_TAG_TEXT( CAP ) );
	}

	LogCap( mCameFromSave ? TEXT( "BeginPlay(loaded)" ) : TEXT( "BeginPlay(built)" ) );

	// What the Cap's proxy holds once vanilla has applied the buildable's customization — the swatch
	// path, read back rather than assumed. A Cap has no terminals, so no indicator push.
	FACPRIndicators::ReadBack( mCapMesh, ACPR_TAG_TEXT( CAP ), TEXT( "CapMesh" ) );
}

void AACPRCap::GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );
	DOREPLIFETIME( AACPRCap, mHostActor );
	DOREPLIFETIME( AACPRCap, mHostTerminalIndex );
}

void AACPRCap::OnRep_Host()
{
	// A client's Cap: the host arrived (possibly after BeginPlay ran with it empty) — fit the mesh to
	// the seat it names. Idempotent. The terminal's Capped state comes from the host's replication.
	if( HasActorBegunPlay() )
	{
		SetUpMeshes();
	}
}

void AACPRCap::Dismantle_Implementation()
{
	FACPRCostScope cost( EACPRCost::Dismantle );
	// §9: "Dismantling the Cap reopens the terminal, initiating no search." ReleaseCap is idempotent
	// and EndPlay calls it too; releasing here is what makes the host's terminal read Open at once,
	// rather than when the dismantle effect finishes.
	ReleaseCap();
	FACPRAttachments::Unregister( mHostActor, this );

	Super::Dismantle_Implementation();
}


void AACPRCap::Remap( AActor* newHostActor, uint8 newTerminalIndex )
{
	AActor* oldHost = mHostActor;

	// Order matters, and it is release-then-apply. The reverse would leave a window in which this Cap
	// claims two terminals, and invariant 4 — "every terminal accepts exactly one occupant" — is the
	// one rule in this mod that nothing is allowed to break even transiently.
	ReleaseCap();
	FACPRAttachments::Unregister( oldHost, this );

	SetHostTerminal( newHostActor, newTerminalIndex );

	ApplyCap();
	FACPRAttachments::Register( mHostActor, this );

	LogCap( TEXT( "Remap" ) );
}

void AACPRCap::EndPlay( const EEndPlayReason::Type endPlayReason )
{
	FACPRCostScope cost( EACPRCost::EndPlay );
	// §9: "Dismantling the Cap reopens the terminal, initiating no search." The second half is the
	// load-bearing one and it is satisfied by doing nothing: reopening is a state write on one
	// component, and no coupling pass is scheduled, so §6.3's "removing a Cap ... never initiates a
	// coupling search" holds by construction rather than by a guard.
	//
	// This also runs when the HOST is dismantled and takes the Cap with it as a child. The terminal
	// may already be torn down by then, which ResolveHostTerminal reports as null, so the release is
	// a no-op rather than a crash.
	ReleaseCap();
	FACPRAttachments::Unregister( mHostActor, this );

	Super::EndPlay( endPlayReason );
}

void AACPRCap::SetHostTerminal( AActor* hostActor, uint8 terminalIndex )
{
	mHostActor = hostActor;
	mHostTerminalIndex = terminalIndex;
}

UACPRTerminalComponent* AACPRCap::ResolveHostTerminal() const
{
	if( !IsValid( mHostActor ) )
	{
		return nullptr;
	}

	// Cast to the interface rather than to a concrete class, for ACPRTerminalHost.h's reason: a Cap
	// caps a terminal, and it must not care whether that terminal belongs to a Rail, a Junction or
	// something §8 and §10 have not built yet.
	const IACPRTerminalHost* host = Cast< IACPRTerminalHost >( mHostActor );
	return host ? host->GetTerminalAtIndex( mHostTerminalIndex ) : nullptr;
}

void AACPRCap::ApplyCap()
{
	UACPRTerminalComponent* terminal = ResolveHostTerminal();
	if( !terminal )
	{
		// A saved Cap whose host no longer exists. §6.3 calls this class of thing "conservative save
		// repair" and allows removing invalid state before it is player-visible — but a Cap is a
		// buildable the player paid for, so it is left standing and merely reported. It caps nothing,
		// which is visible, rather than silently deleting itself, which is not.
		ACPR_LOG( Warning, CAP,
			TEXT( "%s has no host terminal: actor=%s index=%d. The Cap is standing on "
			      "nothing — its host was dismantled without taking it, or the terminal index moved." ),
			*GetName(),
			IsValid( mHostActor ) ? *mHostActor->GetName() : TEXT( "<null>" ),
			static_cast< int32 >( mHostTerminalIndex ) );
		return;
	}

	// Coupled beats capped, and it is a refusal rather than an overwrite. §4 says a terminal is open,
	// coupled or capped and "never two at once", so a Cap arriving on a terminal that is already
	// coupled must not quietly become the second occupant — invariant 4 gives every terminal exactly
	// one. The hologram will not let this happen; a corrupt save could.
	if( terminal->GetCoupledTo() )
	{
		ACPR_LOG( Warning, CAP,
			TEXT( "%s cannot cap %s.%s: already coupled to %s. Invariant 4 — one occupant "
			      "per terminal. The Cap is left standing and caps nothing." ),
			*GetName(),
			*mHostActor->GetName(), *terminal->GetName(),
			*terminal->GetCoupledTo()->GetName() );
		return;
	}

	terminal->SetCapped( true );
}

void AACPRCap::ReleaseCap()
{
	if( UACPRTerminalComponent* terminal = ResolveHostTerminal() )
	{
		// Only if we are the reason it is capped. Two Caps can never share a terminal — ApplyCap
		// refuses an occupied one — but a host being torn down in the same frame can leave the
		// component alive and already reset, and clearing a state we did not set is how a future
		// second occupant type would silently reopen a terminal it does not own.
		if( terminal->GetTerminalState() == EACPRTerminalState::Capped )
		{
			terminal->SetCapped( false );
		}
	}
}

FVector AACPRCap::FitMeshComponentToBox( UStaticMeshComponent* comp, const FVector& extent )
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
	// methods — the one form that does not depend on a name that cannot be checked against a local
	// header, since the engine headers are not in the SML set. Same choice, and the same reason, as
	// AACPRJunction::FitMeshComponentToCube.
	const FBoxSphereBounds bounds = mesh->GetBounds();
	const FVector size = bounds.BoxExtent * 2.0;
	const FVector centre = bounds.Origin;

	const FVector target = extent * 2.0;

	// Per axis, and guarded: a degenerate axis keeps scale 1 rather than producing an infinity that
	// would take the whole component off-screen and look like the mesh had failed to load.
	FVector scale( 1.0, 1.0, 1.0 );
	if( FMath::Abs( size.X ) > 0.01 ) { scale.X = target.X / size.X; }
	if( FMath::Abs( size.Y ) > 0.01 ) { scale.Y = target.Y / size.Y; }
	if( FMath::Abs( size.Z ) > 0.01 ) { scale.Z = target.Z / size.Z; }

	comp->SetRelativeScale3D( scale );

	// Centring is the other half, and it is the half a hand-written constant does by accident. A mesh
	// authored from a pivot at one end is exactly the right size and still sits off the terminal;
	// -centre * scale puts the mesh's own bounds on the actor origin whatever its pivot.
	comp->SetRelativeLocation( FVector( -centre.X * scale.X,
	                                    -centre.Y * scale.Y,
	                                    -centre.Z * scale.Z ) );

	return FVector( size.X * scale.X, size.Y * scale.Y, size.Z * scale.Z );
}

void AACPRCap::SetUpMeshes()
{
	// One function for every door the saved state arrives through — construction (BeginPlay) and a
	// blueprint copy (PostSerializedFromBlueprint, where BeginPlay never runs). The fit needs
	// mHostActor and mHostTerminalIndex, so it cannot live in the constructor; it lives here and the
	// entry points only call it.
	if( mCapMesh && GetRootComponent() && !mCapMesh->IsAttachedTo( GetRootComponent() ) )
	{
		mCapMesh->AttachToComponent( GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform );
	}
	FitMesh();
}

void AACPRCap::FitMesh()
{
	if( !mCapMesh )
	{
		ACPR_LOG( Warning, CAP,
			TEXT( "no CapMesh component — Build_PowerRailCap's parent class is wrong, and "
			      "the Cap will be invisible." ) );
		return;
	}

	// No "no mesh assigned" bail, because assigning it is this function's job: ApplyCapMesh
	// resolves the right one of the two cap meshes from the CDO's soft paths. Bailing here would
	// make the C++ default unreachable on a clean checkout.

	// §11's face width, chosen by what this Cap is actually sitting on rather than by a single
	// constant: 0.4 m on a Rail end, 0.9 m on a Junction face. The host is asked through the terminal
	// interface, so a Rail and a Junction are told apart by their terminal COUNT rather than by a
	// class cast — the same reasoning that keeps the coupling resolver off AACPRRail, and it means
	// §8's Outlet interface gets a sensible width without being named here.
	const IACPRTerminalHost* host = IsValid( mHostActor ) ? Cast< IACPRTerminalHost >( mHostActor ) : nullptr;
	const bool junctionLike = host && host->GetTerminalCount() > 2;
	// ApplyCapMesh picks the mesh as well as fitting it — the Rail and the Junction have one each,
	// and which one this Cap wears is the same question as how wide it is.
	ApplyCapMesh( mCapMesh, this, junctionLike );
}

FVector AACPRCap::FitZAuthoredMeshToLocalX( UStaticMeshComponent* comp,
                                            const FVector& targetSize,
                                            float standoff,
                                            bool uniformScale )
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
	// methods — the same choice, and the same reason, as FitMeshComponentToBox above.
	const FBoxSphereBounds bounds = mesh->GetBounds();
	const FVector meshSize = bounds.BoxExtent * 2.0;

	// The axis map, written out once: actor X <- mesh Z, actor Y <- mesh Y, actor Z <- mesh X.
	FVector scale( 1.0, 1.0, 1.0 );
	if( FMath::Abs( meshSize.Z ) > 0.01 ) { scale.Z = targetSize.X / meshSize.Z; }
	if( FMath::Abs( meshSize.Y ) > 0.01 ) { scale.Y = targetSize.Y / meshSize.Y; }
	if( FMath::Abs( meshSize.X ) > 0.01 ) { scale.X = targetSize.Z / meshSize.X; }

	if( uniformScale )
	{
		// The TRANSVERSE fit decides it — actor Y and Z, which are mesh Y and X — so the part fills
		// the width it was given and keeps its own proportions along the normal. The smaller of the
		// two, so neither transverse axis overflows the target.
		const double transverse = FMath::Min( scale.Y, scale.X );
		scale = FVector( transverse, transverse, transverse );
	}

	const FRotator rotation( -90.0, 0.0, 0.0 );
	comp->SetRelativeScale3D( scale );
	comp->SetRelativeRotation( rotation );

	// X carries ONLY the standoff: the mesh's mounting plane is already on its own origin. The
	// transverse axes are centred on the mesh's own bounds, computed in the mesh frame and then
	// rotated, so this stays correct if the axis map above ever changes.
	const FVector centre = rotation.Quaternion().RotateVector( bounds.Origin * scale );
	comp->SetRelativeLocation( FVector( standoff, -centre.Y, -centre.Z ) );

	// The fit is an assertion, not a scaler. Every ACPR mesh is authored at its final size, so
	// every scale here should be 1. A scaler that silently resizes is how a 2 cm accent strip
	// comes out 1.6 cm on one part and 3.6 cm on another while every bounding box is exactly what
	// the caller asked for. Anything else is drift between acpr_meshes.py and these numbers, and it
	// says so rather than absorbing it.
	const double drift = FMath::Max3( FMath::Abs( scale.X - 1.0 ),
	                                  FMath::Abs( scale.Y - 1.0 ),
	                                  FMath::Abs( scale.Z - 1.0 ) );
	if( drift > 0.02 )
	{
		ACPR_LOG( Warning, FIT,
			TEXT( "%s is authored %s but is being fitted to %s — scale %s. Every ACPR "
			      "mesh should fit 1:1; this one will wear an accent strip %.0f%% of the right "
			      "width. Reconcile acpr_meshes.py with the field that set the target." ),
			*mesh->GetName(), *meshSize.ToString(), *targetSize.ToString(), *scale.ToString(),
			100.0 * FMath::Min3( scale.X, scale.Y, scale.Z ) );
	}

	// Returned in ACTOR axes, which is what every caller logs and compares against a spec number.
	return FVector( meshSize.Z * scale.Z, meshSize.Y * scale.Y, meshSize.X * scale.X );
}

FVector AACPRCap::FitCapMesh( UStaticMeshComponent* comp,
                              float depth, float faceWidth, float standoff )
{
	// §9: the outer face sits AT the terminal plane and the plate extends INWARD. The mesh is
	// authored exactly that way — a CAP_FACE x CAP_FACE x CAP_DEPTH box from z = 0 down to
	// -CAP_DEPTH — so once it is rotated so mesh +Z is actor +X, the face lands on the plane and
	// the body extends to -X with no shift at all. Fitting it unrotated into a centred box would
	// need a half-depth shift to put the face back on the plane, and would stretch the plate's
	// depth onto the axis that carries its face: right bounding box, wrong shape.
	//
	// Per-axis rather than uniform: `depth` and `faceWidth` are stated independently by §9 and §11,
	// and a plate has no proportion to preserve.
	//
	// The mesh moves; the actor does not. So the terminal this Cap occupies is untouched by either
	// number, and §9's "changes no terminal position, Rail length, Junction cell or blueprint
	// interface transform" holds however this is tuned.
	return FitZAuthoredMeshToLocalX( comp, FVector( depth, faceWidth, faceWidth ),
	                                 standoff, false );
}

FVector AACPRCap::ApplyCapMesh( UStaticMeshComponent* comp, const AACPRCap* cdo,
                                bool junctionLike )
{
	if( !comp )
	{
		return FVector::ZeroVector;
	}

	// The soft path resolves on first use — never in a constructor, because this game feature's
	// content is not guaranteed mounted when the CDO is built.
	// A VALUE, not a const reference. The ternary below mixes a member lvalue with a default
	// temporary, so its result is a prvalue; binding a reference to that works only through
	// lifetime extension, and a copy of a soft pointer costs nothing.
	const TSoftObjectPtr< UStaticMesh > wanted = junctionLike
		? ( cdo ? cdo->mJunctionCapMeshAsset : TSoftObjectPtr< UStaticMesh >() )
		: ( cdo ? cdo->mRailCapMeshAsset : TSoftObjectPtr< UStaticMesh >() );

	if( UStaticMesh* const mesh = wanted.LoadSynchronous() )
	{
		if( comp->GetStaticMesh() != mesh )
		{
			comp->SetStaticMesh( mesh );
		}
	}
	else if( !comp->GetStaticMesh() )
	{
		ACPR_LOG( Warning, CAP,
			TEXT( "no %s cap mesh could be resolved and the component has none — the "
			      "Cap will be invisible. Run acpr_meshes.py." ),
			junctionLike ? TEXT( "Junction" ) : TEXT( "Rail" ) );
	}

	// The no-CDO fallbacks match the header's defaults. A stale fallback would fit a Cap to the
	// wrong pocket, silently, on the one path where [ACPR-FIT] has no authored value to compare
	// against.
	const float width = junctionLike
		? ( cdo ? cdo->mJunctionFaceWidth : 93.172f )
		: ( cdo ? cdo->mRailFaceWidth : 44.0f );

	// Depth follows the host the same way width does: each Cap fills its own pocket and stops.
	const float depth = junctionLike
		? ( cdo ? cdo->mJunctionCapDepth : 2.0f )
		: ( cdo ? cdo->mRailCapDepth : 3.0f );

	// NEGATED: mJunctionCapInset is how far the Cap is sunk, so it reaches the standoff slot as a
	// negative. Named for the direction it is allowed to go in, because the sign is the mistake.
	const float standoff = junctionLike
		? -( cdo ? cdo->mJunctionCapInset : 0.1f )
		: ( cdo ? cdo->mStandoff : 0.0f );

	return FitCapMesh( comp, depth, width, standoff );
}

void AACPRCap::LogCap( const TCHAR* stage ) const
{
	const UACPRTerminalComponent* terminal = ResolveHostTerminal();

	// Which pocket this Cap was sized for. Same test as ApplyCapMesh and FitMesh use — terminal
	// COUNT, not a class cast — so the log cannot report a depth the fit did not use.
	const IACPRTerminalHost* logHost = IsValid( mHostActor )
		? Cast< IACPRTerminalHost >( mHostActor ) : nullptr;
	const bool hostIsJunctionLike = logHost && logHost->GetTerminalCount() > 2;

	// The mesh's own transform is on this line, as on [ACPR-OUTLET]: which way a part points cannot
	// be settled from a screenshot. A Rail Cap that is neither visible nor targetable is either
	// somewhere other than where it is meant to be, or has no mesh — and those need different fixes.
	//
	// rot should read P=-90 (mesh authored along +Z, actor's +X outward), scale 1,1,1 since every
	// Cap mesh is authored at its host's true size, and loc.X the inset — 0 on a Rail.
	const FString capMesh = mCapMesh
		? FString::Printf( TEXT( "%s rot=%s scale=%s loc=%s" ),
			mCapMesh->GetStaticMesh() ? *mCapMesh->GetStaticMesh()->GetName() : TEXT( "<none>" ),
			*mCapMesh->GetRelativeRotation().ToString(),
			*mCapMesh->GetRelativeScale3D().ToString(),
			*mCapMesh->GetRelativeLocation().ToString() )
		: FString( TEXT( "<no component>" ) );

	// host= and index= are the save format, and state= is what the Cap is for. A line reading
	// state=Open after a Cap's own BeginPlay means ApplyCap refused, and the Warning above says why.
	ACPR_LOG( Verbose, CAP,
		TEXT( "%s %s | BUILD=%s | host=%s index=%d | terminal=%s state=%s | loc=%s "
		      "| depth=%.1f standoff=%.1f | actorRot=%s | mesh: %s" ),
		*GetName(), stage, ACPR_BUILD_STAMP(),
		IsValid( mHostActor ) ? *mHostActor->GetName() : TEXT( "<null>" ),
		static_cast< int32 >( mHostTerminalIndex ),
		terminal ? *terminal->GetName() : TEXT( "<unresolved>" ),
		terminal ? *terminal->Describe() : TEXT( "-" ),
		*GetActorLocation().ToString(),
		hostIsJunctionLike ? mJunctionCapDepth : mRailCapDepth,
		hostIsJunctionLike ? -mJunctionCapInset : mStandoff,
		*GetActorRotation().ToString(),
		*capMesh );
}
