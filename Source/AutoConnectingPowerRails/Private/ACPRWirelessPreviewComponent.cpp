// Auto-Connecting Power Rails — §16.5's placement preview.

#include "ACPRWirelessPreviewComponent.h"

#include "ACPRDesignerSpace.h"
#include "ACPRWirelessLinkSubsystem.h"
#include "ACPRWirelessOutlet.h"
#include "ACPRWirelessOutletHologram.h"
#include "AutoConnectingPowerRails.h"
#include "Buildables/FGBuildableFactory.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "FGPowerConnectionComponent.h"
#include "GameFramework/Pawn.h"
#include "Hologram/FGBuildableHologram.h"
#include "Materials/MaterialInterface.h"

UACPRWirelessPreviewComponent::UACPRWirelessPreviewComponent()
{
	// After the hologram and the build gun have moved it this frame, so the answer is for where it is.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	SetIsReplicatedByDefault( false );
}

UACPRWirelessPreviewComponent* UACPRWirelessPreviewComponent::AttachToMachineHologram( AFGBuildableHologram* hologram )
{
	if( !IsValid( hologram ) || hologram->IsA< AACPRWirelessOutletHologram >() )
	{
		return nullptr;
	}
	const UClass* buildClass = hologram->GetBuildClass();
	if( !buildClass || !UACPRWirelessLinkSubsystem::IsMachine( buildClass->GetDefaultObject< AActor >() ) )
	{
		return nullptr;
	}
	return Attach( hologram, EMode::Machine );
}

UACPRWirelessPreviewComponent* UACPRWirelessPreviewComponent::AttachToOutletHologram( AFGBuildableHologram* hologram )
{
	return Attach( hologram, EMode::Outlet );
}

UACPRWirelessPreviewComponent* UACPRWirelessPreviewComponent::Attach( AFGBuildableHologram* hologram, EMode mode )
{
	if( !IsValid( hologram ) )
	{
		return nullptr;
	}
	// A readout for the player holding the hologram: a remote player's hologram on the host, or any
	// hologram on a dedicated server, draws nothing here.
	const APawn* instigator = hologram->GetConstructionInstigator();
	if( !instigator || !instigator->IsLocallyControlled() )
	{
		return nullptr;
	}
	if( hologram->FindComponentByClass< UACPRWirelessPreviewComponent >() )
	{
		return nullptr;
	}

	UACPRWirelessPreviewComponent* preview = NewObject< UACPRWirelessPreviewComponent >( hologram );
	if( !preview )
	{
		return nullptr;
	}
	preview->mMode = mode;
	const AACPRWirelessOutlet* rules = mode == EMode::Outlet
		? Cast< AACPRWirelessOutlet >( hologram->GetBuildClass() ? hologram->GetBuildClass()->GetDefaultObject() : nullptr )
		: AACPRWirelessOutlet::StaticClass()->GetDefaultObject< AACPRWirelessOutlet >();
	preview->mRules = rules;
	preview->RegisterComponent();

	ACPR_LOG( Verbose, WIRELESS_HOLO, TEXT( "%s: %s preview attached | rules=%s" ),
		*hologram->GetName(), mode == EMode::Outlet ? TEXT( "outlet" ) : TEXT( "machine" ), *GetNameSafe( rules ) );
	return preview;
}

const AACPRWirelessOutlet* UACPRWirelessPreviewComponent::Rules() const
{
	return mRules.Get();
}

bool UACPRWirelessPreviewComponent::EnsureLines()
{
	if( mPoweredLines && mFaintLines )
	{
		return true;
	}
	if( mLinesMissing )
	{
		return false;
	}
	const AACPRWirelessOutlet* rules = Rules();
	AActor* owner = GetOwner();
	USceneComponent* root = owner ? owner->GetRootComponent() : nullptr;
	UStaticMesh* mesh = rules ? rules->mLinkLineMesh.LoadSynchronous() : nullptr;
	UMaterialInterface* powered = rules ? rules->mLinkMaterialPowered.LoadSynchronous() : nullptr;
	UMaterialInterface* faint = rules ? rules->mLinkMaterial.LoadSynchronous() : nullptr;
	if( !root || !mesh || !powered || !faint )
	{
		mLinesMissing = true;
		ACPR_LOG( Warning, WIRELESS_HOLO,
			TEXT( "%s: preview lines need the link mesh and both link materials (mesh=%d powered=%d faint=%d) — run acpr_meshes.py and acpr_materials.py." ),
			*GetNameSafe( owner ), mesh ? 1 : 0, powered ? 1 : 0, faint ? 1 : 0 );
		return false;
	}

	auto make = [ & ]( const TCHAR* name, UMaterialInterface* material ) -> UInstancedStaticMeshComponent*
	{
		UInstancedStaticMeshComponent* lines = NewObject< UInstancedStaticMeshComponent >( owner, name );
		if( !lines )
		{
			return nullptr;
		}
		// World-space instances, rebuilt on every answer: the component itself must not move with
		// the hologram between answers, so its transform is absolute.
		lines->SetMobility( EComponentMobility::Movable );
		lines->SetAbsolute( true, true, true );
		lines->SetCollisionEnabled( ECollisionEnabled::NoCollision );
		lines->SetCastShadow( false );
		lines->SetCanEverAffectNavigation( false );
		lines->SetRenderCustomDepth( false );
		lines->SetupAttachment( root );
		lines->RegisterComponent();
		lines->SetStaticMesh( mesh );
		lines->SetMaterial( 0, material );
		return lines;
	};
	mPoweredLines = make( TEXT( "ACPRPreviewPoweredLines" ), powered );
	mFaintLines = make( TEXT( "ACPRPreviewFaintLines" ), faint );
	if( !mPoweredLines || !mFaintLines )
	{
		mLinesMissing = true;
		return false;
	}
	return true;
}

void UACPRWirelessPreviewComponent::TickComponent( float deltaTime, ELevelTick tickType, FActorComponentTickFunction* thisTickFunction )
{
	Super::TickComponent( deltaTime, tickType, thisTickFunction );
	AActor* owner = GetOwner();
	UACPRWirelessLinkSubsystem* links = UACPRWirelessLinkSubsystem::Get( this );
	if( !IsValid( owner ) || !links )
	{
		return;
	}
	// Same place, same world: the last answer stands.
	const FTransform now = owner->GetActorTransform();
	if( mAnswered && links->GetWorldVersion() == mLastWorldVersion && now.Equals( mLastTransform, 0.01 ) )
	{
		return;
	}
	mLastTransform = now;
	mLastWorldVersion = links->GetWorldVersion();
	mAnswered = true;
	Refresh();
}

void UACPRWirelessPreviewComponent::Refresh()
{
	AFGBuildableHologram* hologram = Cast< AFGBuildableHologram >( GetOwner() );
	UACPRWirelessLinkSubsystem* links = UACPRWirelessLinkSubsystem::Get( this );
	if( !hologram || !links )
	{
		return;
	}
	if( mPoweredLines )
	{
		mPoweredLines->ClearInstances();
	}
	if( mFaintLines )
	{
		mFaintLines->ClearInstances();
	}
	if( mMode == EMode::Machine )
	{
		RefreshMachine( links, hologram );
	}
	else
	{
		RefreshOutlet( links, hologram );
	}
}

void UACPRWirelessPreviewComponent::RefreshMachine( UACPRWirelessLinkSubsystem* links, AFGBuildableHologram* hologram )
{
	const AFGBuildableBlueprintDesigner* space = UACPRWirelessLinkSubsystem::SpaceOf( hologram );
	const AActor* const ends[] = { hologram };
	TSet< TWeakObjectPtr< AACPRWirelessOutlet > > reaching;

	for( const UFGPowerConnectionComponent* connection : hologram->GetCachedPowerConnectionComponents() )
	{
		if( !IsValid( connection ) || connection->IsHidden() )
		{
			continue;
		}
		const FVector point = connection->GetComponentLocation();
		TArray< FACPRLinkCandidate > candidates;
		links->ClassifyOutletsFor( point, space, MakeArrayView( ends ), candidates );
		bool chosen = false;
		for( const FACPRLinkCandidate& candidate : candidates )
		{
			reaching.Add( candidate.Outlet );
			if( candidate.Clear )
			{
				DrawLine( candidate.Outlet->GetLinkOrigin(), point, !chosen );
				chosen = true;
			}
			else
			{
				DrawLine( candidate.Outlet->GetLinkOrigin(), candidate.BlockedAt, false );
			}
		}
	}
	ShowSpheres( reaching );
}

void UACPRWirelessPreviewComponent::RefreshOutlet( UACPRWirelessLinkSubsystem* links, AFGBuildableHologram* hologram )
{
	const AACPRWirelessOutlet* rules = Rules();
	if( !rules )
	{
		return;
	}
	// The same point the built outlet measures from: mLinkOriginHeight along its mounting normal,
	// which is the hologram's own +X once it is snapped to a host (AACPROutletHologram).
	const FVector origin = hologram->GetActorLocation() + hologram->GetActorForwardVector() * rules->mLinkOriginHeight;
	TArray< FACPRPreviewLine > lines;
	links->PreviewOutletAt( rules, origin, UACPRWirelessLinkSubsystem::SpaceOf( hologram ), hologram, lines );
	for( const FACPRPreviewLine& line : lines )
	{
		DrawLine( line.From, line.To, line.Chosen );
	}
}

void UACPRWirelessPreviewComponent::DrawLine( const FVector& from, const FVector& to, bool powered )
{
	if( !EnsureLines() )
	{
		return;
	}
	const AACPRWirelessOutlet* rules = Rules();
	const FVector delta = to - from;
	const double length = delta.Size();
	if( length < 1.0 )
	{
		return;
	}
	// The outlet's own geometry (AACPRWirelessOutlet::RebuildLines): 100 uu along +X, 2 uu across.
	const double across = FMath::Max( 0.1, static_cast< double >( rules ? rules->mLinkLineWidth : 1.5f ) * 0.5 );
	const FTransform instance( FRotationMatrix::MakeFromX( delta / length ).ToQuat(), from,
	                           FVector( length / 100.0, across, across ) );
	UInstancedStaticMeshComponent* lines = powered ? mPoweredLines : mFaintLines;
	lines->AddInstance( instance, /*bWorldSpace*/ true );
}

void UACPRWirelessPreviewComponent::ShowSpheres( const TSet< TWeakObjectPtr< AACPRWirelessOutlet > >& outlets )
{
	for( const TWeakObjectPtr< AACPRWirelessOutlet >& weak : mShowing )
	{
		if( AACPRWirelessOutlet* outlet = weak.Get(); outlet && !outlets.Contains( weak ) )
		{
			outlet->SetRangeShownBy( this, false );
		}
	}
	for( const TWeakObjectPtr< AACPRWirelessOutlet >& weak : outlets )
	{
		if( AACPRWirelessOutlet* outlet = weak.Get(); outlet && !mShowing.Contains( weak ) )
		{
			outlet->SetRangeShownBy( this, true );
		}
	}
	mShowing = outlets;
}

void UACPRWirelessPreviewComponent::Clear()
{
	ShowSpheres( TSet< TWeakObjectPtr< AACPRWirelessOutlet > >() );
	if( mPoweredLines )
	{
		mPoweredLines->ClearInstances();
	}
	if( mFaintLines )
	{
		mFaintLines->ClearInstances();
	}
}

void UACPRWirelessPreviewComponent::EndPlay( const EEndPlayReason::Type endPlayReason )
{
	Clear();
	Super::EndPlay( endPlayReason );
}
