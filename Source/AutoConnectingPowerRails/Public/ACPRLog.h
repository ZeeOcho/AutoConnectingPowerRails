// Auto-Connecting Power Rails — the one place the log category, its tags and its budgets are spelled.
//
// A line is written as
//
//     ACPR_LOG( Verbose, RAIL_HOLO, TEXT( "step complete | ..." ), args... );
//
// and the category, the "[ACPR-RAIL-HOLO] " prefix and the launch-line name live here only, so no
// call site spells the category name. Every runtime line starts "[ACPR", so one filter captures all
// of it. Settled instrumentation is Verbose: compiled in, hidden unless the category is raised
// (-LogCmds="LogAutoConnectingPowerRails Verbose").

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

// The category's name is written out here and in the module's DEFINE_LOG_CATEGORY — nowhere else. It is
// not aliased behind a macro: the engine's log macros paste the name into an identifier
// (FLogCategory##Name) before any expansion, so an alias would not resolve there.
DECLARE_LOG_CATEGORY_EXTERN( LogAutoConnectingPowerRails, Log, All );

/** The tags, as TCHAR literals so they concatenate with TEXT() formats. One per subsystem; the tag is what the log filter keys on. */
#define ACPR_TAG_MODULE       TEXT( "ACPR" )
#define ACPR_TAG_RAIL         TEXT( "ACPR-RAIL" )
#define ACPR_TAG_RAIL_LANE    TEXT( "ACPR-RAIL-LANE" )
#define ACPR_TAG_JUNC         TEXT( "ACPR-JUNC" )
#define ACPR_TAG_CAP          TEXT( "ACPR-CAP" )
#define ACPR_TAG_OUTLET       TEXT( "ACPR-OUTLET" )
#define ACPR_TAG_TERM         TEXT( "ACPR-TERM" )
#define ACPR_TAG_POWER        TEXT( "ACPR-POWER" )
#define ACPR_TAG_SLOT         TEXT( "ACPR-SLOT" )
#define ACPR_TAG_FIT          TEXT( "ACPR-FIT" )
#define ACPR_TAG_COST         TEXT( "ACPR-COST" )
#define ACPR_TAG_REPAIR       TEXT( "ACPR-REPAIR" )
#define ACPR_TAG_SPACE        TEXT( "ACPR-SPACE" )
#define ACPR_TAG_AIM          TEXT( "ACPR-AIM" )
#define ACPR_TAG_CUE          TEXT( "ACPR-CUE" )
#define ACPR_TAG_RAIL_HOLO    TEXT( "ACPR-RAIL-HOLO" )
#define ACPR_TAG_JUNC_HOLO    TEXT( "ACPR-JUNC-HOLO" )
#define ACPR_TAG_CAP_HOLO     TEXT( "ACPR-CAP-HOLO" )
#define ACPR_TAG_OUTLET_HOLO  TEXT( "ACPR-OUTLET-HOLO" )
#define ACPR_TAG_BP           TEXT( "ACPR-BP" )
#define ACPR_TAG_BP_DUP       TEXT( "ACPR-BP-DUP" )
#define ACPR_TAG_BP_TIME      TEXT( "ACPR-BP-TIME" )
#define ACPR_TAG_BP_ICON      TEXT( "ACPR-BP-ICON" )
#define ACPR_TAG_BRIDGE       TEXT( "ACPR-BRIDGE" )
#define ACPR_TAG_BRIDGE_SCALE TEXT( "ACPR-BRIDGE-SCALE" )

/** A line with a compile-time tag: ACPR_LOG( Verbosity, TAG, TEXT( format ), ... ). */
#define ACPR_LOG( Verbosity, Tag, Format, ... ) \
	UE_LOG( LogAutoConnectingPowerRails, Verbosity, TEXT( "[" ) ACPR_TAG_##Tag TEXT( "] " ) Format, ##__VA_ARGS__ )

/** A line whose tag is a runtime string (the shared host helpers, which log for whichever class called). */
#define ACPR_LOG_TAGGED( Verbosity, TagString, Format, ... ) \
	UE_LOG( LogAutoConnectingPowerRails, Verbosity, TEXT( "[%s] " ) Format, TagString, ##__VA_ARGS__ )

/** Is the category raised to this verbosity? The cheap test that guards every string a diagnostic builds. */
#define ACPR_LOG_ACTIVE( Verbosity ) UE_LOG_ACTIVE( LogAutoConnectingPowerRails, Verbosity )

/** The runtime tag strings, for the shared helpers. */
#define ACPR_TAG_TEXT( Tag ) ACPR_TAG_##Tag

/**
 * The compiler's stamp for the translation unit this is written in: a BeginPlay line that prints it
 * says whether that file was in the last build (a stale DLL is otherwise invisible). Expanded per
 * use, so each file carries its own; TEXT() cannot wrap __DATE__ (it pastes rather than expands),
 * hence ANSI_TO_TCHAR.
 */
#define ACPR_BUILD_STAMP() \
	( []() -> const TCHAR* { static const FString Stamp( ANSI_TO_TCHAR( __DATE__ " " __TIME__ ) ); return *Stamp; }() )

/**
 * A budgeted, de-duplicated line. Placement hooks run every frame; a gate keeps a diagnostic to one
 * line per real change (the key) and to a fixed number of lines per hologram (the budget), and the
 * key is only built once the cheap tests have passed:
 *
 *     if( !mSnapLog.IsOpen( ACPR_LOG_ACTIVE( Verbose ) ) ) { return; }
 *     const FString key = ...;
 *     if( !mSnapLog.Admit( key ) ) { return; }
 *     ACPR_LOG( ... );
 */
struct FACPRLogGate
{
	explicit FACPRLogGate( int32 budget ) : Budget( budget ) {}

	/** Lines left and the verbosity raised. */
	bool IsOpen( bool verbosityActive ) const { return Budget > 0 && verbosityActive; }

	/** True when `key` differs from the last admitted one; spends one line of the budget. */
	bool Admit( const FString& key )
	{
		if( key == LastKey )
		{
			return false;
		}
		LastKey = key;
		--Budget;
		return true;
	}

	int32 Budget;
	FString LastKey;
};
