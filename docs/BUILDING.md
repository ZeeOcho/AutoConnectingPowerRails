# Building

The mod is a Game Feature plugin for the SML Starter Project (Satisfactory 1.2, SML 3.12,
Unreal Engine 5.6.1-CSS, Visual Studio 2022). It lives at
`<project>\Mods\GameFeatures\AutoConnectingPowerRails\`.

Every piece of content is produced by a script in `Scripts\` and committed, so a checkout
packages without running any script; the scripts are how the assets are reproduced after a
change. Each content folder's `GENERATED.md` says which of its files come from which script.
The only assets not made by a script are the two Alpakit's Create Mod wizard puts at the
Content root: the `FGGameFeatureData` asset and `RootGameWorld_AutoConnectingPowerRails`.
There is no manual editor step anywhere in the pipeline.

## Packaging a checkout

1. Clone the Starter Project and put this repository at
   `Mods\GameFeatures\AutoConnectingPowerRails\`.
2. Extract the base game's assets into the project: `docs/EXTRACTING_GAME_FILES.md`. The
   mod's materials are instances of vanilla masters and its holograms read vanilla assets, so
   the Starter Project's placeholders are not enough.
3. Build **Development Editor** in Visual Studio (or let Alpakit build).
4. Open the project and run **Alpakit Release** on the mod. The result is the multi-target
   zip ficsit.app takes.

## Regenerating the content

Run the scripts inside the editor: Window > Developer Tools > Output Log, switch the dropdown
from "Cmd" to "Python", then `exec(open(r"<plugin>\Scripts\<script>.py").read())`.
`acpr_build_all.py` runs steps 2–8 below in order and stops at the first one that reports a
failure; the steps can also be run one at a time. Each one writes its log to
`Saved\ACPR\ACPR_*_log.txt` (the icon renders go to `Saved\ACPR_Icons\`), reads back what it
wrote, and ends with a problem count. The order:

| # | Script | Makes | Needs |
| --- | --- | --- | --- |
| 1 | the C++ build, then an **editor restart** | the classes the blueprints are parented to | — |
| 2 | `acpr_buildables.py` | `Build_`/`Holo_`/`Desc_`/`Recipe_` for all four buildables and `Schematic_PowerRails` with its unlock and dependency | 1 |
| 3 | `acpr_meshes.py` | `Content\Meshes\SM_ACPR_*` (Geometry Script) | 1 |
| 4 | `acpr_materials.py` | `Content\Materials\M_ACPR_*`, `MI_ACPR_*`; assigns the meshes' slots | 3 |
| 5 | `acpr_bind_meshes.py` | points the four buildables' mesh components at the meshes | 2, 3 |
| 6 | `acpr_icons.py` | `Content\Icons\T_ACPR_*`, `Resources\Icon128.png`, `Resources\ModIcon512.png`; writes them into the descriptors and the schematic | 2, 4, 5 |
| 7 | `acpr_validate.py` | nothing — checks the unlock, recipes, descriptors and meshes before a cook | 2, 5 |
| 8 | `acpr_fingerprint.py` | `Saved\ACPR_Fingerprint\<label>.json`: what every generated asset contains, as data; run on its own it writes `before.json`, run by `acpr_build_all.py` it writes `after.json` and `diff.txt` against the `before.json` beside it — the check that a regeneration changed only what the script change meant to | 2–6 |

## The editor restart rule

A C++ change that adds or removes a `UPROPERTY`, a component created in a constructor, or a
`UCLASS`/`USTRUCT` member changes the class layout, which Live Coding cannot apply: close and
reopen the editor after the Visual Studio build, or the running editor keeps the old class
defaults and the scripts write to them. A change to function bodies only can go in through
Live Coding.

Two binaries exist: Visual Studio builds the editor DLL, Alpakit builds the one the game
loads. A build log with zero compile actions for the shipping target means the sources are
older than the objects (a zip extracted from another machine does this): touch the sources.
Every buildable logs its compile-time build stamp at `BeginPlay`, which is the check.

## Logging

All runtime logging is one category, `LogAutoConnectingPowerRails`, with a `[ACPR-…]` tag per
subsystem. Settled instrumentation is Verbose and hidden by default; raise it with the launch
argument `-LogCmds="LogAutoConnectingPowerRails Verbose"`. Timing and cost meters run only under
the console variable `acpr.Trace`.
