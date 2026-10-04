# Auto-Connecting Power Rails

A Satisfactory mod (SML 3.12) that adds a power backbone for modular factories: conductive
rails and junctions that snap together, auto-connect across blueprint boundaries, and feed the
rest of the factory through outlets.

| Buildable | What it is |
| --- | --- |
| **Power Rail** | A conductive beam, 1–40 m, whose two ends are terminals. A rail aimed at a terminal snaps to it and couples, straight on. |
| **Power Rail Junction** | A 1 m cube with a terminal on each face: placed free, snapped to a terminal, or inserted into a rail, which it splits. |
| **Power Rail Outlet** | The interface to ordinary wiring: on a rail body or an open terminal, four power lines, like a Wall Outlet. |
| **Insulated Terminal Cap** | Closes a terminal so nothing snaps or auto-connects to it. |

Coupled rails, junctions and outlets are one vanilla power circuit. Rails and junctions power
the Hoverpack along their length, all four take Customizer swatches and patterns, and a
blueprint's open terminals auto-connect at its boundary in the Auto-Connect build modes.
Everything unlocks with one AWESOME Shop package after Basic Steel Production.

Single-player is tested; multiplayer is serialized but untested.

## Repository

| | |
| --- | --- |
| `DESIGN_SPEC.md` | The rules. Every behaviour of the mod is specified there, once; the appendices hold art direction, implementation notes and engine verification. |
| `Source/` | The C++ module: the four buildables, their holograms, the terminal/coupling model, the blueprint auto-connect manager. |
| `Content/` | The assets. Each folder's `GENERATED.md` says which files a script produces. |
| `Scripts/` | The editor Python scripts that generate meshes, materials, the buildable assets and icons, validate the result, and fingerprint it for comparison; `acpr_build_all.py` runs them in order (run acpr_fingerprint before changing scripts to create something for the fingerprint of acpr_build_all to compare against). |
| `Resources/` | The plugin icon and the ficsit.app image. |
| `Config/AccessTransformers.ini` | The SML access transformers the module needs. |
| `docs/BUILDING.md` | How to package a checkout and how to regenerate the content. |
| `docs/EXTRACTING_GAME_FILES.md` | Getting the base game's assets into the Starter Project. |

## Removing the mod from a save

Buildables of an uninstalled mod are dropped from a save, and dropping a power backbone cuts
the power to what hung off it. SML's [Core Redirects](https://docs.ficsit.app/satisfactory-modding/latest/ForUsers/CoreRedirectMigration.html)
can substitute vanilla buildables instead, from a file the player writes at
`<game>\FactoryGame\Mods\SML\Config\Engine.ini`:

```ini
[CoreRedirects]
+ClassRedirects=(OldName="/AutoConnectingPowerRails/Buildables/Build_PowerRail.Build_PowerRail_C",NewName="/Game/FactoryGame/Buildable/Factory/PowerPoleWall/Build_PowerPoleWall.Build_PowerPoleWall_C")
+ClassRedirects=(OldName="/AutoConnectingPowerRails/Buildables/Build_PowerRailJunction.Build_PowerRailJunction_C",NewName="/Game/FactoryGame/Buildable/Factory/PowerPoleWall/Build_PowerPoleWall.Build_PowerPoleWall_C")
+ClassRedirects=(OldName="/AutoConnectingPowerRails/Buildables/Build_PowerRailOutlet.Build_PowerRailOutlet_C",NewName="/Game/FactoryGame/Buildable/Factory/PowerPoleWall/Build_PowerPoleWall.Build_PowerPoleWall_C")
+ClassRedirects=(OldName="/Script/AutoConnectingPowerRails.ACPRPowerConnectionComponent",NewName="/Script/FactoryGame.FGPowerConnectionComponent")
```

Every rail, junction and outlet becomes a Wall Outlet standing where the buildable's origin was,
and the network stays powered: a save stores power as the circuit's member list, each power
line's endpoints and each hidden connection, all of them references to a connection component by
actor and component name — and the mod's buildables name their connection component as the Wall
Outlet names its own (`PowerConnection`), so every one of those references resolves on the
replacement. The rails' couplings become hidden connections between the wall outlets, outlets keep
their power lines, caps are dropped. What is lost is the look: the wall outlets float where the
rails were, and running visible cables between them is cosmetic. The mechanism is all-or-nothing
and cannot be tuned: try it on a copy of the save first.

## Author

ZeeOcho

## AI Disclaimer
Generative AI (Claude Fable) was extensively used to create this mod, especially:
- writing the desin spec and other docs
- create all code
- create all scripts, which in turn create the mods assets (everything in Content/ and Resources/, which includes icons)
- the buildables / assets of this mod partly reference vanilla Satisfactory textures / classes / behavior
