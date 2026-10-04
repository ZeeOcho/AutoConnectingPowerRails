// Auto-Connecting Power Rails — our terminals as vanilla attachment points.

#include "ACPRAttachment.h"

#include "ACPRTerminalComponent.h"
#include "ACPRTerminalHost.h"
#include "AutoConnectingPowerRails.h"
#include "FGAttachmentPointComponent.h"
#include "UObject/UnrealType.h"

bool UACPRTerminalAttachmentType::CanAttach_Implementation( const FFGAttachmentPoint& point, const FFGAttachmentPoint& targetPoint ) const
{
	// Our type mates with our type; vanilla has already matched the classes before asking. The
	// question left is the terminal's state behind the target — the picker's own two tests (§4).
	const UACPRTerminalComponent* target = FACPRAttachment::TerminalFor( targetPoint );
	return target && target->IsOpen() && !target->IsPrivateInterface();
}

void FACPRAttachment::ConfigureComponent( UFGAttachmentPointComponent* component, bool hologramToo )
{
	if( !component )
	{
		return;
	}

	// mType: TSubclassOf< UFGAttachmentPointType > — an FClassProperty. mUsage: EAttachmentPointUsage,
	// an enum class on uint8 — an FEnumProperty (or FByteProperty on older headers; both handled).
	// Both private, EditDefaultsOnly (FGAttachmentPointComponent.h:33-40); reflection is the one
	// route from C++ that is not a Friend entry for a two-field write.
	UClass* componentClass = UFGAttachmentPointComponent::StaticClass();

	if( FClassProperty* typeProperty = CastField< FClassProperty >( componentClass->FindPropertyByName( TEXT( "mType" ) ) ) )
	{
		typeProperty->SetObjectPropertyValue_InContainer( component, UACPRTerminalAttachmentType::StaticClass() );
	}
	else
	{
		ACPR_LOG( Warning, MODULE, TEXT( "UFGAttachmentPointComponent::mType not found by reflection — terminal snapping is off" ) );
	}

	// EAPU_Default = 0 (buildable and hologram), EAPU_BuildableOnly = 1 (FGAttachmentPointComponent.h:11-16).
	const uint8 usage = hologramToo ? 0 : 1;
	if( FEnumProperty* enumProperty = CastField< FEnumProperty >( componentClass->FindPropertyByName( TEXT( "mUsage" ) ) ) )
	{
		void* value = enumProperty->ContainerPtrToValuePtr< void >( component );
		enumProperty->GetUnderlyingProperty()->SetIntPropertyValue( value, static_cast< int64 >( usage ) );
	}
	else if( FByteProperty* byteProperty = CastField< FByteProperty >( componentClass->FindPropertyByName( TEXT( "mUsage" ) ) ) )
	{
		byteProperty->SetPropertyValue_InContainer( component, usage );
	}
	else
	{
		ACPR_LOG( Warning, MODULE, TEXT( "UFGAttachmentPointComponent::mUsage not found by reflection — terminal snapping is off" ) );
	}
}

bool FACPRAttachment::MakeTerminalPoint( AActor* owner, const IACPRTerminalHost* host, uint8 index, FFGAttachmentPoint& out_point )
{
	FTransform frame;
	if( !owner || !host || !host->GetTerminalLocalFrame( index, frame ) )
	{
		return false;
	}
	out_point.RelativeTransform = frame;
	out_point.Type = UACPRTerminalAttachmentType::StaticClass();
	out_point.Owner = owner;
	return true;
}

UACPRTerminalComponent* FACPRAttachment::TerminalFor( const FFGAttachmentPoint& point )
{
	AActor* owner = point.Owner.Get();
	const IACPRTerminalHost* host = owner ? Cast< IACPRTerminalHost >( owner ) : nullptr;
	if( !host )
	{
		return nullptr;
	}

	const int32 count = host->GetTerminalCount();
	for( int32 i = 0; i < count; ++i )
	{
		FTransform frame;
		if( host->GetTerminalLocalFrame( static_cast< uint8 >( i ), frame ) &&
			FVector::DistSquared( frame.GetLocation(), point.RelativeTransform.GetLocation() )
				< UACPRTerminalComponent::LatticeQuantum * UACPRTerminalComponent::LatticeQuantum )
		{
			return host->GetTerminalAtIndex( static_cast< uint8 >( i ) );
		}
	}
	return nullptr;
}
