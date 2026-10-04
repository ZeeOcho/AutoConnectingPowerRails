// Auto-Connecting Power Rails — §12's coupling cue. See the header for how the cue is chosen and played.

#include "ACPRCouplingCue.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AutoConnectingPowerRails.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FGFactorySettings.h"
#include "FGGlobalSettings.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Components/ActorComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Modules/ModuleManager.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "TimerManager.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

// -----------------------------------------------------------------------------------------------------
// Settings. Console variables so a choice can be auditioned and changed in game with no rebuild; they can
// also be set in Engine.ini under [ConsoleVariables] if the console is not to hand.
// -----------------------------------------------------------------------------------------------------

static TAutoConsoleVariable< int32 > CVarACPRCouplingCue(
	TEXT( "acpr.CouplingCue" ), 1,
	TEXT( "Power Rails: play the coupling cue (spark + sound) when a Coupling forms. 1 on, 0 off." ),
	ECVF_Default );

/**
 * Vanilla's own pair, as the cooked game names them (acpr.ListCueCandidates lists both):
 *
 *   /Game/WwiseAudio/Events/Buildable/_Shared/Play_F_PowerConnection   the power-connection event, in the
 *                                                                      folder shared by every power building
 *   /Game/FactoryGame/VFX/Misc/BuildEffect/P_PowerlineSparks_01        the power line's own spark effect
 *
 * §12 asks for "the vanilla electrical-connection cue — spark VFX plus the electrical sound", and these are
 * those two assets by name. They are defaults, not constants: either console variable overrides, "none"
 * silences, and "auto" falls back to the keyword search.
 */
static TAutoConsoleVariable< FString > CVarACPRCouplingSound(
	TEXT( "acpr.CouplingSound" ),
	TEXT( "/Game/WwiseAudio/Events/Buildable/_Shared/Play_F_PowerConnection.Play_F_PowerConnection" ),
	TEXT( "Power Rails: Wwise event for the coupling sound, as an asset path. 'auto' = best power/connect-like "
	      "event in the game, 'none' = silent." ),
	ECVF_Default );

static TAutoConsoleVariable< FString > CVarACPRCouplingSpark(
	TEXT( "acpr.CouplingSpark" ),
	TEXT( "/Game/FactoryGame/VFX/Misc/BuildEffect/P_PowerlineSparks_01.P_PowerlineSparks_01" ),
	TEXT( "Power Rails: Niagara or Cascade system for the coupling spark, as an asset path. 'auto' = best "
	      "spark-like system in the game, 'none' = no spark." ),
	ECVF_Default );

static TAutoConsoleVariable< float > CVarACPRCouplingSparkLifetime(
	TEXT( "acpr.CouplingSparkLifetime" ), 3.0f,
	TEXT( "Power Rails: seconds after which a coupling spark is destroyed if it has not finished by itself "
	      "(guards against a looping system). 0 = never." ),
	ECVF_Default );

static TAutoConsoleVariable< float > CVarACPRCouplingSparkScale(
	TEXT( "acpr.CouplingSparkScale" ), 1.0f,
	TEXT( "Power Rails: uniform scale of the coupling spark." ),
	ECVF_Default );

// -----------------------------------------------------------------------------------------------------
// State. Game thread only (every caller is: Form runs from a host's tick, the commands from the console).
// -----------------------------------------------------------------------------------------------------

namespace ACPRCue
{
	struct FBatch
	{
		TWeakObjectPtr< UWorld > World;
		TArray< FVector > Points;
		TArray< FVector > Outwards;
		TArray< FString > Contexts;
		bool Scheduled = false;
	};

	struct FCandidate
	{
		FString Path;
		FString Name;
		FString Class;
		int32 Score = 0;
	};

	/** One resolved asset choice, re-resolved only when its console variable changes. */
	struct FResolved
	{
		FString Key;
		bool Done = false;
		TWeakObjectPtr< UObject > Object;
		bool RootedByUs = false;
		bool Found = false;
		FString Description;
	};

	static FBatch& Batch() { static FBatch batch; return batch; }
	static int32& SuppressDepth() { static int32 depth = 0; return depth; }
	static int32& SuppressedCount() { static int32 count = 0; return count; }
	static FResolved& Sound() { static FResolved resolved; return resolved; }
	static FResolved& Spark() { static FResolved resolved; return resolved; }
	static TArray< FCandidate >& LastSounds() { static TArray< FCandidate > list; return list; }
	static TArray< FCandidate >& LastEffects() { static TArray< FCandidate > list; return list; }

	static constexpr int32 MaxSparksPerBatch = 32;
	static constexpr int32 MaxListed = 40;

	/** The screen holds far fewer lines than the log; the rest are in the log with their paths. */
	static constexpr int32 MaxOnScreen = 14;

	static const TCHAR* const SoundKeywords[] = {
		TEXT( "wire" ), TEXT( "cable" ), TEXT( "power" ), TEXT( "pole" ), TEXT( "connect" ), TEXT( "snap" ),
		TEXT( "plug" ), TEXT( "spark" ), TEXT( "zap" ), TEXT( "electric" ), TEXT( "attach" ) };

	static const TCHAR* const EffectRequired[] = {
		TEXT( "spark" ), TEXT( "zap" ), TEXT( "electric" ), TEXT( "lightning" ), TEXT( "shock" ) };

	static const TCHAR* const EffectBonus[] = {
		TEXT( "wire" ), TEXT( "power" ), TEXT( "pole" ), TEXT( "connect" ), TEXT( "cable" ), TEXT( "small" ),
		TEXT( "hit" ), TEXT( "impact" ) };

	static const TCHAR* const EffectPenalty[] = {
		TEXT( "explosion" ), TEXT( "large" ), TEXT( "big" ), TEXT( "fire" ), TEXT( "smoke" ), TEXT( "rain" ),
		TEXT( "weather" ), TEXT( "loop" ), TEXT( "storm" ) };

	/** One candidate source: a name, a class name and a path, from the registry or from memory. */
	struct FRawAsset
	{
		FString Name;
		FString Class;
		FString Path;
	};

	/**
	 * Every asset of one class, appended to `out`.
	 *
	 * The registry first, then what is loaded. The cooked asset registry can answer nothing for a class
	 * (Wwise events, for one), which a cooked build is allowed to do — the registry it ships is whatever was
	 * cooked into it. So when the registry has nothing for a class, the objects already in memory are asked
	 * instead: fewer than exist on disk, but they are the ones the game has actually used, which for "what
	 * does vanilla play" is the interesting set anyway.
	 */
	static void AssetsOfClass( const TCHAR* package, const TCHAR* className, TArray< FRawAsset >& out, FString& out_note )
	{
		IAssetRegistry& registry =
			FModuleManager::LoadModuleChecked< FAssetRegistryModule >( TEXT( "AssetRegistry" ) ).Get();

		TArray< FAssetData > found;
		registry.GetAssetsByClass( FTopLevelAssetPath( FName( package ), FName( className ) ), found, /*bSearchSubClasses*/ true );

		for( const FAssetData& asset : found )
		{
			FRawAsset raw;
			raw.Name = asset.AssetName.ToString();
			raw.Class = asset.AssetClassPath.GetAssetName().ToString();
			raw.Path = asset.GetObjectPathString();
			out.Add( raw );
		}

		out_note += FString::Printf( TEXT( "%s: registry=%d" ), className, found.Num() );

		if( found.Num() > 0 )
		{
			out_note += TEXT( "  " );
			return;
		}

		const FString classPath = FString::Printf( TEXT( "%s.%s" ), package, className );
		UClass* type = FindObject< UClass >( nullptr, *classPath );
		int32 loaded = 0;

		if( type )
		{
			// Class defaults and garbage excluded by the iterator itself.
			for( TObjectIterator< UObject > it( RF_ClassDefaultObject, true, EInternalObjectFlags::Garbage ); it; ++it )
			{
				UObject* object = *it;
				if( object && object->IsA( type ) )
				{
					FRawAsset raw;
					raw.Name = object->GetName();
					raw.Class = object->GetClass()->GetName();
					raw.Path = object->GetPathName();
					out.Add( raw );
					++loaded;
				}
			}
		}

		out_note += FString::Printf( TEXT( " loadedInMemory=%d%s  " ), loaded, type ? TEXT( "" ) : TEXT( " (class not loaded)" ) );
	}

	static FString& LastGatherNote() { static FString note; return note; }

	/**
	 * The raw lists, gathered once per session per kind. The registry query is cheap; the in-memory fallback
	 * walks every UObject, and Gather is called by the discovery log, by the auto-pick and by every listing.
	 */
	static const TArray< FRawAsset >& RawAssets( bool sounds )
	{
		static TArray< FRawAsset > soundAssets;
		static TArray< FRawAsset > effectAssets;
		static FString soundNote;
		static FString effectNote;
		static bool haveSounds = false;
		static bool haveEffects = false;

		if( sounds && !haveSounds )
		{
			haveSounds = true;
			AssetsOfClass( TEXT( "/Script/AkAudio" ), TEXT( "AkAudioEvent" ), soundAssets, soundNote );
		}
		else if( !sounds && !haveEffects )
		{
			haveEffects = true;
			AssetsOfClass( TEXT( "/Script/Niagara" ), TEXT( "NiagaraSystem" ), effectAssets, effectNote );
			AssetsOfClass( TEXT( "/Script/Engine" ), TEXT( "ParticleSystem" ), effectAssets, effectNote );
		}

		LastGatherNote() = sounds ? soundNote : effectNote;
		return sounds ? soundAssets : effectAssets;
	}

	/** Scored, filtered and sorted candidates. `filter` non-empty replaces the keyword requirement. */
	static void Gather( bool sounds, const FString& filter, TArray< FCandidate >& out )
	{
		out.Reset();

		const TArray< FRawAsset >& assets = RawAssets( sounds );

		for( const FRawAsset& asset : assets )
		{
			const FString name = asset.Name;
			int32 score = 0;

			if( !filter.IsEmpty() )
			{
				if( !name.Contains( filter ) )
				{
					continue;
				}
				score = 1;
			}

			if( sounds )
			{
				int32 matched = 0;
				for( const TCHAR* keyword : SoundKeywords )
				{
					matched += name.Contains( keyword ) ? 1 : 0;
				}
				if( filter.IsEmpty() && matched == 0 )
				{
					continue;
				}
				score += matched * 10;
				if( name.Contains( TEXT( "wire" ) ) &&
					( name.Contains( TEXT( "connect" ) ) || name.Contains( TEXT( "snap" ) ) || name.Contains( TEXT( "attach" ) ) ) )
				{
					score += 20;
				}
			}
			else
			{
				bool required = false;
				for( const TCHAR* keyword : EffectRequired )
				{
					required |= name.Contains( keyword );
				}
				if( filter.IsEmpty() && !required )
				{
					continue;
				}
				score += required ? 10 : 0;
				for( const TCHAR* keyword : EffectBonus )
				{
					score += name.Contains( keyword ) ? 10 : 0;
				}
				for( const TCHAR* keyword : EffectPenalty )
				{
					score -= name.Contains( keyword ) ? 15 : 0;
				}
				score += ( asset.Class == TEXT( "NiagaraSystem" ) ) ? 3 : 0;
			}

			FCandidate candidate;
			candidate.Path = asset.Path;
			candidate.Name = name;
			candidate.Class = asset.Class;
			candidate.Score = score;
			out.Add( candidate );
		}

		out.Sort( []( const FCandidate& a, const FCandidate& b )
		{
			return a.Score != b.Score ? a.Score > b.Score : a.Name.Len() < b.Name.Len();
		} );
	}

	/** On screen only — which a shipping build may not draw at all, hence the output device below. */
	static void Screen( const FString& line, const FColor& colour, float seconds )
	{
		if( GEngine )
		{
			GEngine->AddOnScreenDebugMessage( static_cast< uint64 >( INDEX_NONE ), seconds, colour, line );
		}
	}

	/**
	 * To the console that ran the command, to the log, and to the screen.
	 *
	 * The console is the one that matters: a console command registered through IConsoleManager prints
	 * nothing by itself, and a shipping build draws no on-screen debug messages, so without it
	 * `acpr.ListCueCandidates` would look like it did nothing. The commands are registered as
	 * FAutoConsoleCommandWithWorldArgsAndOutputDevice, which hands them the console's own FOutputDevice — the
	 * same route every vanilla console command's output takes.
	 */
	static void Say( FOutputDevice* ar, const FString& line, const FColor& colour = FColor::White, float seconds = 30.0f )
	{
		ACPR_LOG( Display, CUE, TEXT( "%s" ), *line );

		if( ar )
		{
			ar->Log( line );
		}

		Screen( line, colour, seconds );
	}

	static void LogCandidates( FOutputDevice* ar, const TCHAR* prefix, const TArray< FCandidate >& list, int32 limit )
	{
		const int32 shown = FMath::Min( list.Num(), limit );

		// Every line to the log with its path, since that is where a choice gets copied from.
		for( int32 i = 0; i < shown; ++i )
		{
			ACPR_LOG( Display, CUE, TEXT( "  %s%d  %s  (%s, score %d)  %s" ),
				prefix, i, *list[ i ].Name, *list[ i ].Class, list[ i ].Score, *list[ i ].Path );
		}

		if( !ar )
		{
			return;
		}

		// To the console, names only: the paths would wrap several times over in that window.
		for( int32 i = 0; i < shown; ++i )
		{
			ar->Logf( TEXT( "  %s%d  %s  (%s, score %d)" ),
				prefix, i, *list[ i ].Name, *list[ i ].Class, list[ i ].Score );
		}

		// And the first few on screen too, newest at the top, for when the console is in the way.
		for( int32 i = FMath::Min( shown, MaxOnScreen ) - 1; i >= 0; --i )
		{
			Screen( FString::Printf( TEXT( "  %s%d  %s  (%s, score %d)" ),
				prefix, i, *list[ i ].Name, *list[ i ].Class, list[ i ].Score ), FColor::Silver, 60.0f );
		}
	}

	/** Vanilla's hologram snap sound, off the factory settings CDO by reflection (no AkAudio header needed). */
	static UObject* PlaceholderSound()
	{
		// UFGGlobalSettings::GetFactorySettingsCDO: static UFUNCTION (FGGlobalSettings.h:19).
		const UFGFactorySettings* settings = UFGGlobalSettings::GetFactorySettingsCDO();
		if( !settings )
		{
			return nullptr;
		}

		const FObjectPropertyBase* property =
			CastField< FObjectPropertyBase >( settings->GetClass()->FindPropertyByName( TEXT( "mHologramSnapSound" ) ) );

		return property ? property->GetObjectPropertyValue_InContainer( settings ) : nullptr;
	}

	static UObject* LoadByPath( const FString& path )
	{
		FSoftObjectPath soft( path );
		return soft.IsValid() ? soft.TryLoad() : nullptr;
	}

	static void Adopt( FResolved& resolved, UObject* object, bool rootIt, const FString& description )
	{
		if( resolved.RootedByUs )
		{
			if( UObject* previous = resolved.Object.Get() )
			{
				previous->RemoveFromRoot();
			}
		}

		// Loaded by us and referenced by nothing else: rooted, or the next GC would collect it and every
		// cue would reload it from disk. The placeholder is held by the settings CDO and is not rooted.
		// Never claims an object something else had already rooted, so replacing it never un-roots it.
		resolved.RootedByUs = rootIt && object != nullptr && !object->IsRooted();
		if( resolved.RootedByUs )
		{
			object->AddToRoot();
		}

		resolved.Found = object != nullptr;
		resolved.Object = object;
		resolved.Description = description;
		resolved.Done = true;
	}

	static UObject* ResolveSound( FString& out_description )
	{
		FResolved& resolved = Sound();
		const FString key = CVarACPRCouplingSound.GetValueOnGameThread().TrimStartAndEnd();

		// Re-resolved when the setting changes, or when an asset that WAS found has since gone away. A path
		// that was not found is not retried on every coupling (a synchronous load each time); changing the
		// console variable retries it.
		if( !resolved.Done || key != resolved.Key || ( resolved.Found && !resolved.Object.IsValid() ) )
		{
			resolved.Key = key;

			if( key.Equals( TEXT( "none" ), ESearchCase::IgnoreCase ) )
			{
				Adopt( resolved, nullptr, false, TEXT( "none (acpr.CouplingSound)" ) );
			}
			else if( key.IsEmpty() || key.Equals( TEXT( "auto" ), ESearchCase::IgnoreCase ) )
			{
				// The keyword search (which ranks Play_F_PowerConnection first in the cooked game), then
				// vanilla's hologram snap sound as the last resort.
				TArray< FCandidate > candidates;
				Gather( true, FString(), candidates );

				UObject* picked = nullptr;
				FString pickedPath;
				for( int32 i = 0; i < candidates.Num() && i < 5 && !picked; ++i )
				{
					picked = LoadByPath( candidates[ i ].Path );
					pickedPath = candidates[ i ].Path;
				}

				if( picked )
				{
					Adopt( resolved, picked, true,
						FString::Printf( TEXT( "auto-picked %s (of %d)" ), *pickedPath, candidates.Num() ) );
				}
				else
				{
					UObject* placeholder = PlaceholderSound();
					Adopt( resolved, placeholder, false,
						FString::Printf( TEXT( "placeholder mHologramSnapSound = %s" ), *GetPathNameSafe( placeholder ) ) );
				}
			}
			else
			{
				UObject* loaded = LoadByPath( key );
				Adopt( resolved, loaded, true,
					FString::Printf( TEXT( "acpr.CouplingSound = %s%s" ), *key, loaded ? TEXT( "" ) : TEXT( " (NOT FOUND)" ) ) );
			}
		}

		out_description = resolved.Description;
		return resolved.Object.Get();
	}

	static UObject* ResolveSpark( FString& out_description )
	{
		FResolved& resolved = Spark();
		const FString key = CVarACPRCouplingSpark.GetValueOnGameThread().TrimStartAndEnd();

		if( !resolved.Done || key != resolved.Key || ( resolved.Found && !resolved.Object.IsValid() ) )
		{
			resolved.Key = key;

			if( key.Equals( TEXT( "none" ), ESearchCase::IgnoreCase ) )
			{
				Adopt( resolved, nullptr, false, TEXT( "none (acpr.CouplingSpark)" ) );
			}
			else if( key.IsEmpty() || key.Equals( TEXT( "auto" ), ESearchCase::IgnoreCase ) )
			{
				TArray< FCandidate > candidates;
				Gather( false, FString(), candidates );

				UObject* picked = nullptr;
				FString pickedPath;
				for( int32 i = 0; i < candidates.Num() && i < 5 && !picked; ++i )
				{
					picked = LoadByPath( candidates[ i ].Path );
					pickedPath = candidates[ i ].Path;
				}

				Adopt( resolved, picked, true, picked
					? FString::Printf( TEXT( "auto-picked %s (of %d spark-like systems)" ), *pickedPath, candidates.Num() )
					: FString::Printf( TEXT( "auto-pick found nothing loadable (%d candidates)" ), candidates.Num() ) );
			}
			else
			{
				UObject* loaded = LoadByPath( key );
				Adopt( resolved, loaded, true,
					FString::Printf( TEXT( "acpr.CouplingSpark = %s%s" ), *key, loaded ? TEXT( "" ) : TEXT( " (NOT FOUND)" ) ) );
			}
		}

		out_description = resolved.Description;
		return resolved.Object.Get();
	}

	/**
	 * Calls a static UFUNCTION through reflection: parameters zero-initialised, then filled by type and name —
	 * WorldContextObject gets the world, the one object parameter whose class the asset satisfies gets the
	 * asset, an FVector named *Scale* gets `scale` and any other FVector the location, an FRotator the
	 * rotation, every bool true (auto-destroy, auto-activate, pre-cull check). Everything else stays at its
	 * zero default (pooling None, empty strings, unbound delegates).
	 */
	/**
	 * Calls `function` on `receiver` through reflection.
	 *
	 * `asset` is the thing being played when the function takes it as a parameter (a static library function);
	 * pass nullptr when the RECEIVER is the asset (a member function such as UAkAudioEvent::PostAtLocation),
	 * and no object parameter is filled with it.
	 */
	static bool CallReflected( UObject* receiver, UFunction* function, UObject* asset, UWorld* world,
	                           const FVector& location, const FRotator& rotation, const FVector& scale,
	                           FString& out_result, UObject** out_returned = nullptr )
	{
		if( out_returned )
		{
			*out_returned = nullptr;
		}

		if( !receiver || !function )
		{
			out_result = TEXT( "no function" );
			return false;
		}

		const int32 size = FMath::Max< int32 >( function->ParmsSize, 1 );
		uint8* parameters = static_cast< uint8* >( FMemory::Malloc( size, function->GetMinAlignment() ) );
		FMemory::Memzero( parameters, size );

		int32 assetSet = 0;
		int32 worldSet = 0;
		int32 locationSet = 0;
		FString signature;

		for( TFieldIterator< FProperty > it( function ); it && it->HasAnyPropertyFlags( CPF_Parm ); ++it )
		{
			FProperty* property = *it;
			property->InitializeValue_InContainer( parameters );

			if( property->HasAnyPropertyFlags( CPF_ReturnParm ) )
			{
				continue;
			}

			const FString name = property->GetName();
			signature += name + TEXT( " " );

			if( FObjectPropertyBase* object = CastField< FObjectPropertyBase >( property ) )
			{
				if( name == TEXT( "WorldContextObject" ) )
				{
					object->SetObjectPropertyValue_InContainer( parameters, world );
					++worldSet;
				}
				else if( asset && object->PropertyClass && asset->IsA( object->PropertyClass ) && assetSet == 0 )
				{
					object->SetObjectPropertyValue_InContainer( parameters, asset );
					++assetSet;
				}
			}
			else if( FStructProperty* structure = CastField< FStructProperty >( property ) )
			{
				if( structure->Struct == TBaseStructure< FVector >::Get() )
				{
					const bool isScale = name.Contains( TEXT( "Scale" ) );
					*structure->ContainerPtrToValuePtr< FVector >( parameters ) = isScale ? scale : location;
					locationSet += isScale ? 0 : 1;
				}
				else if( structure->Struct == TBaseStructure< FRotator >::Get() )
				{
					*structure->ContainerPtrToValuePtr< FRotator >( parameters ) = rotation;
				}
			}
			else if( FBoolProperty* flag = CastField< FBoolProperty >( property ) )
			{
				flag->SetPropertyValue_InContainer( parameters, true );
			}
		}

		// The asset has to have landed somewhere when it is a parameter, and the function has to take a
		// location; a world-context parameter is optional (a member function on the asset may not have one).
		const bool callable = ( asset ? assetSet == 1 : true ) && locationSet >= 1;
		if( callable )
		{
			receiver->ProcessEvent( function, parameters );

			// The return value, when it is an object (SpawnSystemAtLocation's UNiagaraComponent), so the caller
			// can bound the effect's lifetime.
			if( out_returned )
			{
				if( FProperty* returned = function->GetReturnProperty() )
				{
					if( FObjectPropertyBase* object = CastField< FObjectPropertyBase >( returned ) )
					{
						*out_returned = object->GetObjectPropertyValue_InContainer( parameters );
					}
				}
			}
		}

		for( TFieldIterator< FProperty > it( function ); it && it->HasAnyPropertyFlags( CPF_Parm ); ++it )
		{
			it->DestroyValue_InContainer( parameters );
		}
		FMemory::Free( parameters );

		out_result = callable
			? FString::Printf( TEXT( "%s::%s ok" ), *GetNameSafe( function->GetOuterUClass() ), *function->GetName() )
			: FString::Printf( TEXT( "%s::%s NOT CALLED (asset=%d world=%d location=%d | params: %s)" ),
				*GetNameSafe( function->GetOuterUClass() ), *function->GetName(), assetSet, worldSet, locationSet,
				*signature );
		return callable;
	}

	/**
	 * A spark is a one-shot, but the auto-picked system is chosen by name and nothing guarantees it does not
	 * loop — spawned with auto-destroy, a looping system would never finish. So every spawned effect is
	 * destroyed after acpr.CouplingSparkLifetime seconds unless it has already finished (weak: a one-shot that
	 * auto-destroyed is simply gone by then).
	 */
	static void BoundLifetime( UWorld* world, UActorComponent* component )
	{
		const float lifetime = CVarACPRCouplingSparkLifetime.GetValueOnGameThread();
		if( !world || !component || lifetime <= 0.0f )
		{
			return;
		}

		TWeakObjectPtr< UActorComponent > weak( component );
		FTimerHandle handle;
		world->GetTimerManager().SetTimer( handle, FTimerDelegate::CreateLambda( [ weak ]()
		{
			if( UActorComponent* alive = weak.Get() )
			{
				alive->DestroyComponent();
			}
		} ), lifetime, false );
	}

	/** A static library function by class path and name, with the class default object as the receiver. */
	static UFunction* FindLibraryFunction( const TCHAR* classPath, const TCHAR* functionName, UObject*& out_receiver )
	{
		out_receiver = nullptr;

		UClass* library = FindObject< UClass >( nullptr, classPath );
		if( !library )
		{
			return nullptr;
		}

		UFunction* function = library->FindFunctionByName( FName( functionName ) );
		if( function )
		{
			out_receiver = library->GetDefaultObject();
		}
		return function;
	}

	/**
	 * The Wwise API of this build, listed once. The integration Satisfactory ships has event posting on the
	 * event object itself (AkGameplayStatics has no PostEventAtLocation), and a later integration may move
	 * it again. Rather than depend on one name, the candidates below are tried in order and this prints
	 * what the build actually offers, so a new name costs a log line to find.
	 */
	static void LogAudioApiOnce( const UObject* sound )
	{
		static bool logged = false;
		if( logged )
		{
			return;
		}
		logged = true;

		auto Dump = [] ( UClass* type, const TCHAR* label )
		{
			if( !type )
			{
				ACPR_LOG( Display, CUE, TEXT( "audio API: %s not loaded" ), label );
				return;
			}

			FString names;
			for( TFieldIterator< UFunction > it( type ); it; ++it )
			{
				const FString name = it->GetName();
				if( name.Contains( TEXT( "Post" ) ) || name.Contains( TEXT( "Spawn" ) ) || name.Contains( TEXT( "Play" ) ) )
				{
					names += name + TEXT( " " );
				}
			}

			ACPR_LOG( Display, CUE, TEXT( "audio API: %s (%s) offers: %s" ),
				label, *type->GetPathName(), names.IsEmpty() ? TEXT( "<nothing matching Post/Spawn/Play>" ) : *names );
		};

		Dump( FindObject< UClass >( nullptr, TEXT( "/Script/AkAudio.AkGameplayStatics" ) ), TEXT( "AkGameplayStatics" ) );
		Dump( sound ? sound->GetClass() : nullptr, TEXT( "the event's own class" ) );
	}

	static bool PlayCueSound( UWorld* world, UObject* sound, const FVector& location, FString& out_result )
	{
		if( !sound )
		{
			out_result = TEXT( "no sound" );
			return false;
		}

		LogAudioApiOnce( sound );

		// 1. The event's own member function — Wwise 2022.1 and later
		//    (UAkAudioEvent::PostAtLocation( Location, Orientation, Callback, CallbackMask, WorldContextObject )).
		static const TCHAR* const MemberFunctions[] = { TEXT( "PostAtLocation" ), TEXT( "PostAtLocationAsync" ) };
		for( const TCHAR* name : MemberFunctions )
		{
			if( UFunction* function = sound->FindFunction( FName( name ) ) )
			{
				if( CallReflected( sound, function, nullptr, world, location, FRotator::ZeroRotator,
					FVector::OneVector, out_result ) )
				{
					return true;
				}
			}
		}

		// 2. The static library, older integrations and the spawn-a-component route.
		static const TCHAR* const LibraryFunctions[] = {
			TEXT( "PostEventAtLocation" ), TEXT( "PostEventAtLocationAsync" ), TEXT( "SpawnAkComponentAtLocation" ) };
		for( const TCHAR* name : LibraryFunctions )
		{
			UObject* receiver = nullptr;
			if( UFunction* function = FindLibraryFunction( TEXT( "/Script/AkAudio.AkGameplayStatics" ), name, receiver ) )
			{
				if( CallReflected( receiver, function, sound, world, location, FRotator::ZeroRotator,
					FVector::OneVector, out_result ) )
				{
					return true;
				}
			}
		}

		out_result = FString::Printf( TEXT( "no usable Wwise posting function for %s (see the audio API line)" ),
			*GetNameSafe( sound ) );
		return false;
	}

	static bool PlayCueSpark( UWorld* world, UObject* effect, const FVector& location, const FVector& outward, FString& out_result )
	{
		if( !effect )
		{
			out_result = TEXT( "no spark" );
			return false;
		}

		const FRotator rotation = outward.IsNearlyZero()
			? FRotator::ZeroRotator
			: FRotationMatrix::MakeFromX( outward.GetSafeNormal() ).Rotator();
		const FVector scale( FMath::Max( 0.01, static_cast< double >( CVarACPRCouplingSparkScale.GetValueOnGameThread() ) ) );

		if( UParticleSystem* cascade = Cast< UParticleSystem >( effect ) )
		{
			UActorComponent* component = UGameplayStatics::SpawnEmitterAtLocation( world, cascade, location, rotation, scale, true );
			out_result = component ? TEXT( "GameplayStatics.SpawnEmitterAtLocation ok" ) : TEXT( "SpawnEmitterAtLocation returned null" );
			BoundLifetime( world, component );
			return component != nullptr;
		}

		if( effect->GetClass()->GetFName() == FName( TEXT( "NiagaraSystem" ) ) )
		{
			UObject* receiver = nullptr;
			UFunction* function = FindLibraryFunction( TEXT( "/Script/Niagara.NiagaraFunctionLibrary" ),
				TEXT( "SpawnSystemAtLocation" ), receiver );

			UObject* returned = nullptr;
			const bool called = CallReflected( receiver, function, effect, world, location, rotation, scale,
				out_result, &returned );
			BoundLifetime( world, Cast< UActorComponent >( returned ) );
			return called;
		}

		out_result = FString::Printf( TEXT( "unsupported effect class %s" ), *effect->GetClass()->GetName() );
		return false;
	}

	static void Flush()
	{
		FBatch batch = MoveTemp( Batch() );
		Batch() = FBatch();

		UWorld* world = batch.World.Get();
		if( !world || batch.Points.Num() == 0 )
		{
			return;
		}

		// The candidate discovery (a walk of every UObject when the cooked registry is empty for a class) is
		// reachable only from acpr.ListCueCandidates / acpr.CouplingCueTest, where somebody asked for it.

		FString soundDescription;
		FString sparkDescription;
		UObject* sound = ResolveSound( soundDescription );
		UObject* spark = ResolveSpark( sparkDescription );

		// §12: the sound once per transaction, the spark at each coupling point.
		FString soundResult;
		PlayCueSound( world, sound, batch.Points[ 0 ], soundResult );

		int32 sparks = 0;
		FString sparkResult = TEXT( "-" );
		for( int32 i = 0; i < batch.Points.Num() && i < MaxSparksPerBatch; ++i )
		{
			sparks += PlayCueSpark( world, spark, batch.Points[ i ], batch.Outwards[ i ], sparkResult ) ? 1 : 0;
		}

		ACPR_LOG( Display, CUE,
			TEXT( "%d coupling(s) -> sound x1 [%s: %s] + sparks x%d [%s: %s] | first at %s | %s%s" ),
			batch.Points.Num(),
			*soundDescription, *soundResult,
			sparks, *sparkDescription, *sparkResult,
			*batch.Points[ 0 ].ToString(),
			*FString::Join( batch.Contexts, TEXT( ", " ) ),
			batch.Points.Num() > batch.Contexts.Num() ? TEXT( ", ..." ) : TEXT( "" ) );
	}

	// ---- console commands ------------------------------------------------------------------------

	static void ListCommand( const TArray< FString >& args, UWorld* /*world*/, FOutputDevice& ar )
	{
		const FString filter = args.Num() > 0 ? args[ 0 ] : FString();

		Gather( true, filter, LastSounds() );
		const FString soundNote = LastGatherNote();
		Gather( false, filter, LastEffects() );
		const FString effectNote = LastGatherNote();

		Say( &ar, FString::Printf( TEXT( "candidates%s%s | sounds=%d sparks=%d (showing up to %d of each) | %s%s" ),
			filter.IsEmpty() ? TEXT( "" ) : TEXT( " containing " ), *filter,
			LastSounds().Num(), LastEffects().Num(), MaxListed, *soundNote, *effectNote ),
			FColor::Yellow, 60.0f );

		LogCandidates( &ar, TEXT( "S" ), LastSounds(), MaxListed );
		LogCandidates( &ar, TEXT( "E" ), LastEffects(), MaxListed );
	}

	static void TestCommand( const TArray< FString >& args, UWorld* world, FOutputDevice& ar )
	{
		APlayerController* controller = world ? world->GetFirstPlayerController() : nullptr;
		if( !controller )
		{
			Say( &ar, FString::Printf( TEXT( "test: no player controller in %s" ), *GetNameSafe( world ) ), FColor::Orange );
			return;
		}

		FVector eye;
		FRotator view;
		controller->GetPlayerViewPoint( eye, view );
		const FVector at = eye + view.Vector() * 300.0;
		const FVector outward = -view.Vector();

		if( args.Num() > 0 && args[ 0 ].Len() >= 2 )
		{
			const TCHAR kind = FChar::ToUpper( args[ 0 ][ 0 ] );
			const int32 index = FCString::Atoi( *args[ 0 ].RightChop( 1 ) );

			if( kind == TEXT( 'S' ) && LastSounds().Num() == 0 ) { Gather( true, FString(), LastSounds() ); }
			if( kind == TEXT( 'E' ) && LastEffects().Num() == 0 ) { Gather( false, FString(), LastEffects() ); }

			const TArray< FCandidate >& list = ( kind == TEXT( 'S' ) ) ? LastSounds() : LastEffects();
			if( !list.IsValidIndex( index ) )
			{
				Say( &ar, FString::Printf( TEXT( "test %s: no such candidate — %d sounds, %d sparks (run acpr.ListCueCandidates)" ),
					*args[ 0 ], LastSounds().Num(), LastEffects().Num() ), FColor::Orange );
				return;
			}

			UObject* asset = LoadByPath( list[ index ].Path );
			FString result;
			if( kind == TEXT( 'S' ) )
			{
				PlayCueSound( world, asset, at, result );
			}
			else
			{
				PlayCueSpark( world, asset, at, outward, result );
			}

			Say( &ar, FString::Printf( TEXT( "test %s = %s (%s) | loaded=%d | %s" ),
				*args[ 0 ], *list[ index ].Name, *list[ index ].Class, asset ? 1 : 0, *result ),
				asset ? FColor::Green : FColor::Orange );
			ACPR_LOG( Display, CUE, TEXT( "test %s path=%s" ), *args[ 0 ], *list[ index ].Path );
			return;
		}

		FString soundDescription;
		FString sparkDescription;
		UObject* sound = ResolveSound( soundDescription );
		UObject* spark = ResolveSpark( sparkDescription );

		FString soundResult;
		FString sparkResult;
		PlayCueSound( world, sound, at, soundResult );
		PlayCueSpark( world, spark, at, outward, sparkResult );

		Say( &ar, FString::Printf( TEXT( "test (configured cue) | sound [%s: %s] | spark [%s: %s]" ),
			*soundDescription, *soundResult, *sparkDescription, *sparkResult ), FColor::Green );
	}

	// WithOutputDevice, so the lines land in the console that ran the command (see Say).
	static FAutoConsoleCommandWithWorldArgsAndOutputDevice ListCueCandidatesCommand(
		TEXT( "acpr.ListCueCandidates" ),
		TEXT( "Power Rails: list numbered sound (S) and spark (E) candidates. Optional argument: only names "
		      "containing this text." ),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic( &ListCommand ) );

	static FAutoConsoleCommandWithWorldArgsAndOutputDevice CouplingCueTestCommand(
		TEXT( "acpr.CouplingCueTest" ),
		TEXT( "Power Rails: play the coupling cue 3 m in front of the camera. With S<n> or E<n>, play that "
		      "candidate from acpr.ListCueCandidates instead." ),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic( &TestCommand ) );
}

// -----------------------------------------------------------------------------------------------------

FACPRCouplingCue::FSuppressScope::FSuppressScope()
{
	++ACPRCue::SuppressDepth();
}

FACPRCouplingCue::FSuppressScope::~FSuppressScope()
{
	--ACPRCue::SuppressDepth();
}

void FACPRCouplingCue::Request( UWorld* world, const FVector& location, const FVector& outward, const FString& context )
{
	if( ACPRCue::SuppressDepth() > 0 )
	{
		++ACPRCue::SuppressedCount();
		ACPR_LOG( Verbose, CUE, TEXT( "suppressed (load path) | %s | total suppressed=%d" ),
			*context, ACPRCue::SuppressedCount() );
		return;
	}

	// A game world with a player: the blueprint preview world has none, and a cue there would play into
	// a world nobody is standing in.
	if( !world || !world->IsGameWorld() || !world->GetFirstPlayerController() )
	{
		return;
	}

	if( CVarACPRCouplingCue.GetValueOnGameThread() == 0 )
	{
		return;
	}

	ACPRCue::FBatch& batch = ACPRCue::Batch();
	if( batch.World.Get() != world )
	{
		batch = ACPRCue::FBatch();
		batch.World = world;
	}

	batch.Points.Add( location );
	batch.Outwards.Add( outward );
	if( batch.Contexts.Num() < 6 )
	{
		batch.Contexts.Add( context );
	}

	// One transaction = one frame. Every host couples at its own registration, and a placement's hosts — a
	// blueprint's buildables, an inserted Junction's two children, a bridge's ends — all register in the
	// frame that builds them. So the batch closes at the end of this frame (vanilla's SetTimerForNextTick),
	// not after a wall-clock window.
	if( !batch.Scheduled )
	{
		batch.Scheduled = true;
		world->GetTimerManager().SetTimerForNextTick( FTimerDelegate::CreateLambda( []() { ACPRCue::Flush(); } ) );
	}
}
