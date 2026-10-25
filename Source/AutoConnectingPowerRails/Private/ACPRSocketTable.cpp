// Auto-Connecting Power Rails — §16.5's socket table.

#include "ACPRSocketTable.h"

#include "AutoConnectingPowerRails.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	const TCHAR* TABLE_ASSET = TEXT( "/AutoConnectingPowerRails/Data/DA_ACPR_SocketTable.DA_ACPR_SocketTable" );
	const TCHAR* OVERRIDE_FILE = TEXT( "AutoConnectingPowerRails_Sockets.json" );

	bool ReadVector( const TSharedPtr< FJsonObject >& object, const TCHAR* field, FVector& out )
	{
		const TArray< TSharedPtr< FJsonValue > >* values = nullptr;
		if( !object->TryGetArrayField( field, values ) || !values || values->Num() != 3 )
		{
			return false;
		}
		double v[ 3 ];
		for( int32 i = 0; i < 3; ++i )
		{
			if( !( *values )[ i ]->TryGetNumber( v[ i ] ) )
			{
				return false;
			}
		}
		out = FVector( v[ 0 ], v[ 1 ], v[ 2 ] );
		return true;
	}

	/** One fit from its object; fields it lacks keep `base`'s values. */
	bool ReadFit( const TSharedPtr< FJsonObject >& object, const FACPRSocketFit& base, FACPRSocketFit& out )
	{
		if( !object.IsValid() )
		{
			return false;
		}
		out = base;
		FVector axis;
		if( ReadVector( object, TEXT( "axis" ), axis ) )
		{
			out.Axis = axis.GetSafeNormal( 0.01, base.Axis );
		}
		ReadVector( object, TEXT( "offset" ), out.Offset );
		double number;
		if( object->TryGetNumberField( TEXT( "radius" ), number ) ) { out.Radius = static_cast< float >( number ); out.Core = out.Radius; }
		if( object->TryGetNumberField( TEXT( "core" ), number ) ) { out.Core = static_cast< float >( number ); }
		if( object->TryGetNumberField( TEXT( "bottom" ), number ) ) { out.Bottom = static_cast< float >( number ); }
		if( object->TryGetNumberField( TEXT( "top" ), number ) ) { out.Top = static_cast< float >( number ); }
		if( out.Radius <= 0.0f || out.Top <= out.Bottom )
		{
			out = base;
			return false;
		}
		return true;
	}
}

namespace
{
	TUniquePtr< FACPRSocketTable >& TableSlot()
	{
		static TUniquePtr< FACPRSocketTable > slot;
		return slot;
	}
}

const FACPRSocketTable& FACPRSocketTable::Get()
{
	TUniquePtr< FACPRSocketTable >& slot = TableSlot();
	if( !slot )
	{
		slot.Reset( new FACPRSocketTable() );
	}
	return *slot;
}

FString FACPRSocketTable::Reload()
{
	TableSlot().Reset( new FACPRSocketTable() );
	const FACPRSocketTable& table = Get();
	return FString::Printf( TEXT( "socket table reloaded: %d classes, default r=%.1f core=%.1f %.1f..%.1f" ),
		table.ClassCount(), table.Default().Radius, table.Default().Core, table.Default().Bottom, table.Default().Top );
}

FACPRSocketTable::FACPRSocketTable()
{
	int32 shippedClasses = -1;
	if( const UACPRSocketTableAsset* asset = LoadObject< UACPRSocketTableAsset >( nullptr, TABLE_ASSET ) )
	{
		shippedClasses = Load( asset->Json, TABLE_ASSET, TEXT( "table" ) );
	}
	else
	{
		ACPR_LOG( Warning, WIRELESS, TEXT( "socket table asset %s is missing — link ends use the default fit; run acpr_sockets.py" ), TABLE_ASSET );
	}
	const FString override = FPaths::Combine( FPaths::ProjectDir(), TEXT( "Configs" ), OVERRIDE_FILE );
	int32 overrideClasses = 0;
	FString overrideText;
	if( FPaths::FileExists( override ) )
	{
		overrideClasses = FFileHelper::LoadFileToString( overrideText, *override ) ? Load( overrideText, override, TEXT( "override" ) ) : -1;
	}
	ACPR_LOG( Display, WIRELESS,
		TEXT( "socket table | %d classes from %s (game changelist %s) | override %s: %s | default r=%.1f %.1f..%.1f" ),
		shippedClasses, TABLE_ASSET, mGameChangelist.IsEmpty() ? TEXT( "?" ) : *mGameChangelist, *override,
		overrideClasses > 0 ? *FString::Printf( TEXT( "%d classes" ), overrideClasses )
		                    : overrideClasses < 0 ? TEXT( "unreadable" ) : TEXT( "none" ),
		mDefault.Radius, mDefault.Bottom, mDefault.Top );
}

int32 FACPRSocketTable::Load( const FString& text, const FString& path, const TCHAR* what )
{
	TSharedPtr< FJsonObject > root;
	const TSharedRef< TJsonReader<> > reader = TJsonReaderFactory<>::Create( text );
	if( !FJsonSerializer::Deserialize( reader, root ) || !root.IsValid() )
	{
		ACPR_LOG( Warning, WIRELESS, TEXT( "socket %s %s is not valid JSON: %s" ), what, *path, *reader->GetErrorMessage() );
		return -1;
	}
	FString changelist;
	if( root->TryGetStringField( TEXT( "game_changelist" ), changelist ) && FCString::Strcmp( what, TEXT( "table" ) ) == 0 )
	{
		mGameChangelist = changelist;
	}
	const TSharedPtr< FJsonObject >* defaultObject = nullptr;
	if( root->TryGetObjectField( TEXT( "default" ), defaultObject ) && defaultObject )
	{
		FACPRSocketFit fit;
		if( ReadFit( *defaultObject, mDefault, fit ) )
		{
			mDefault = fit;
		}
		else
		{
			ACPR_LOG( Warning, WIRELESS, TEXT( "socket %s %s: \"default\" is not a fit (needs radius > 0 and top > bottom); kept" ), what, *path );
		}
	}
	int32 classes = 0;
	const TSharedPtr< FJsonObject >* sockets = nullptr;
	if( root->TryGetObjectField( TEXT( "sockets" ), sockets ) && sockets )
	{
		for( const TPair< FString, TSharedPtr< FJsonValue > >& classEntry : ( *sockets )->Values )
		{
			const TSharedPtr< FJsonObject >* connections = nullptr;
			if( !classEntry.Value.IsValid() || !classEntry.Value->TryGetObject( connections ) || !connections )
			{
				ACPR_LOG( Warning, WIRELESS, TEXT( "socket %s %s: %s is not an object of connections; skipped" ), what, *path, *classEntry.Key );
				continue;
			}
			TMap< FName, FACPRSocketFit >& fits = mSockets.FindOrAdd( classEntry.Key );
			for( const TPair< FString, TSharedPtr< FJsonValue > >& connectionEntry : ( *connections )->Values )
			{
				const TSharedPtr< FJsonObject >* object = nullptr;
				FACPRSocketFit fit;
				if( connectionEntry.Value.IsValid() && connectionEntry.Value->TryGetObject( object ) && object
					&& ReadFit( *object, mDefault, fit ) )
				{
					fits.Add( FName( *connectionEntry.Key ), fit );
				}
				else
				{
					ACPR_LOG( Warning, WIRELESS, TEXT( "socket %s %s: %s / %s is not a fit; skipped" ), what, *path, *classEntry.Key, *connectionEntry.Key );
				}
			}
			++classes;
		}
	}
	return classes;
}

const FACPRSocketFit& FACPRSocketTable::Find( const UClass* buildableClass, FName connection, bool* known ) const
{
	for( const UClass* cls = buildableClass; cls; cls = cls->GetSuperClass() )
	{
		if( const TMap< FName, FACPRSocketFit >* fits = mSockets.Find( cls->GetPathName() ) )
		{
			if( const FACPRSocketFit* fit = fits->Find( connection ) )
			{
				if( known ) { *known = true; }
				return *fit;
			}
		}
	}
	if( known ) { *known = false; }
	const FString key = FString::Printf( TEXT( "%s / %s" ), buildableClass ? *buildableClass->GetPathName() : TEXT( "?" ), *connection.ToString() );
	if( !mReportedUnknown.Contains( key ) )
	{
		mReportedUnknown.Add( key );
		ACPR_LOG( Verbose, WIRELESS, TEXT( "socket table has no entry for %s — default fit; an entry in Configs/%s would replace it" ), *key, OVERRIDE_FILE );
	}
	return mDefault;
}
