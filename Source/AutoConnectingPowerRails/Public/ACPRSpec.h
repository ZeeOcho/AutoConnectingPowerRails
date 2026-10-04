// Auto-Connecting Power Rails — the spec's numbers, spelled once.
//
// Each figure stays an EditDefaultsOnly field on the class that uses it — the editor may still tune
// one hologram — but the default is the spec's figure from here, so a change to the spec is one line
// and the headers say which section the number comes from.

#pragma once

#include "CoreMinimal.h"

/**
 * §15: the name of every host's power connection component is the Wall Outlet's
 * (Build_PowerPoleWall's "PowerConnection"). A save references a connection by actor and component
 * name — the circuit's member list, every Power Line's endpoint, every Coupling's hidden edge — so
 * when the mod is removed and the player's Core Redirects turn each ACPR buildable into a Wall
 * Outlet, the saved connection data lands on the Wall Outlet's own component and the network
 * survives. A macro rather than a constexpr because CreateDefaultSubobject wants a literal-shaped
 * name and the three constructors are the only users.
 */
#define ACPR_POWER_CONNECTION_NAME TEXT( "PowerConnection" )

namespace ACPRSpec
{
	/** The build grid: one metre, and the half cell every lattice phase is measured in (§7.1, §11). */
	constexpr float CellSize = 100.0f;
	constexpr float HalfCell = CellSize * 0.5f;

	/** §7: the Junction is a 1 m cube. */
	constexpr float JunctionHalfExtent = HalfCell;

	/** §5: the Rail's profile is 50 uu wide; half of it is how far a body reaches off its axis. */
	constexpr float RailHalfWidth = 25.0f;

	/** §5: the longest Rail (40 m). Build_PowerRail's mMaxLength must agree; the hologram reads the CDO for its own use. */
	constexpr float RailMaxLength = 4000.0f;

	/** §7.2 / §7.3: within 1.5 m of a terminal the hologram is in terminal-snapped mode; also vanilla's attachment threshold. */
	constexpr float TerminalSnapRange = 150.0f;

	/** §7.4: "a clear metre" between a Junction's cell and another Junction or Outlet. */
	constexpr float NeighbourClearance = CellSize;

	/** §12: the nudge steps — a whole cell, and a quarter of one with the fine modifier. */
	constexpr float NudgeCoarse = CellSize;
	constexpr float NudgeFine = 25.0f;

	/**
	 * The reach a search needs to find every Rail whose body can touch a Junction cell: the body may
	 * pass within half a cell plus its own half width of the centre, and its terminals — what the
	 * registry indexes — are at most one full Rail further.
	 */
	constexpr float CrossingSearchRadius = RailMaxLength + JunctionHalfExtent + RailHalfWidth;
}
