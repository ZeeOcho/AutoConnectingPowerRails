# Extracting the game's assets into the project

The SML Starter Project ships placeholders for the base game's assets. The mod's materials are
instances of vanilla masters and its holograms, icons and recipes read vanilla assets, so the
editor only shows something true once the real assets are in `Content\`. This is the
procedure, assembled from the modding docs: [Extracting Game Files](https://docs.ficsit.app/satisfactory-modding/latest/Development/ExtractGameFiles.html),
[Asset Toolkit](https://docs.ficsit.app/satisfactory-modding/latest/CommunityResources/AssetToolkit.html),
[Launch Script](https://docs.ficsit.app/satisfactory-modding/latest/Development/TestingResources.html#LaunchScript).

Three paths recur below; substitute your own everywhere:

| | |
| --- | --- |
| `<game>` | the Satisfactory install, e.g. `D:\Steam\steamapps\common\Satisfactory` |
| `<project>` | the SML Starter Project, e.g. `G:\Satisfactory\SML` |
| `<dump>` | a scratch folder for the dump, e.g. `G:\Satisfactory\dump` |

What it costs: about 8.5 GB of dump (deleted at the end) and 7 GB of generated `Content\`;
one to two hours of dumping and several hours of generating, mostly unattended. A game update
means doing it again. The toolkit cannot recreate every material asset; the ones it cannot are
restored from the Starter Project in step 8.

Only the project-root `Content\` (`/Game/...`) is touched. The mod's own `Content\`
(`/AutoConnectingPowerRails/...`) under `Mods\GameFeatures\` is never involved.

All commands are PowerShell.

## 1. Install the toolkit

Clone the two plugins into `Mods\`, beside `GameFeatures\`:

```powershell
Set-Location '<project>\Mods'
git clone --depth 1 --branch dev https://github.com/satisfactorymodding/UEAssetToolkit.git _uetk
Move-Item _uetk\AssetDumper .
Move-Item _uetk\AssetGenerator .
Remove-Item _uetk -Recurse -Force
```

Regenerate the Visual Studio project files (right-click the `.uproject`) and build
**Development Editor**. Add `/Mods/AssetDumper/` and `/Mods/AssetGenerator/` to the project's
`.gitignore`.

## 2. Package the AssetDumper into the game

The dump runs inside the game, so the AssetDumper mod must be installed there. In the editor,
open Alpakit: enable **Copy Mods to Game**, set **Game Path** to `<game>`, make sure no server
target is enabled (the toolkit does not support dedicated servers), and run **Alpakit Dev** on
**AssetDumper**. AssetGenerator is editor-side and needs no packaging.

## 3. Point the dump at the scratch folder

A directory junction keeps the game install clean; the link path must not exist yet:

```powershell
New-Item -ItemType Directory -Path '<dump>' -Force
New-Item -ItemType Junction -Path '<game>\FactoryGame\AssetDump' -Target '<dump>'
```

## 4. Launch the game once the documented way

Steam needs files that only exist after a launch through the modding docs' launch script.
Copy the script from the [Launch Script](https://docs.ficsit.app/satisfactory-modding/latest/Development/TestingResources.html#LaunchScript)
page to `SFLaunch_Advanced.ps1`, set `$GameDirs.steam.client.steam.path` to `<game>` (the
other entries stay `"UNSET"`), then:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\SFLaunch_Advanced.ps1
```

Close the game once it is up.

## 5. Dump

From the game install:

```powershell
Set-Location '<game>'
.\FactoryGameSteam.exe -EpicPortal -NoSteamClient -DumpAllGameAssets -RootAssetPath=/Game -ExcludePackagePaths=/Game/WwiseAudio -ExcludePackageNames=/Game/Geometry/Meshes/1M_Cube_Chamfer -PackagesPerTick=32 -ExitOnFinish -log -NewConsole -RenderOffscreen -norhithread
```

A console window shows a stream of `LogAssetDumper` lines; it is done when the console and
the game close. No `LogAssetDumper` line after a minute means the mod did not reach
`<game>\FactoryGame\Mods\` (step 2). The game renders while dumping; `-ResX=128 -ResY=128`
reduces that work without touching the frame rate, which must not be capped (the dumper does
its packages per tick). Progress:

```powershell
(Get-ChildItem '<dump>' -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1GB
```

## 6. Move the placeholder Content aside

Close the editor and rename `<project>\Content\` to `Content_PreGenerate`.

## 7. Generate

With the editor closed, save the generation script from the
[Asset Toolkit](https://docs.ficsit.app/satisfactory-modding/latest/CommunityResources/AssetToolkit.html)
page as `generate_assets.ps1` with its three paths filled in (`$UECmdPath` is the
`UnrealEditor-Cmd.exe` of the Unreal Engine - CSS build, `$UProjectPath` is
`<project>\FactoryGame.uproject`, `$AssetDumpDirectory` is `<dump>`), and run it from a
writable directory (it writes its temp files beside itself). Several hours.

Do not add `-PublicProject` (it nulls out the assets this is for), `-NoRefresh` (nothing
exists to refresh) or `-AssetClassWhitelist` (the mod references vanilla recipes and
descriptors as well as meshes, and anything outside the whitelist is not generated at all).

## 8. Restore the Starter Project's custom assets

`CustomAssets.txt` in the modding organisation's `UnrealProjectUpdater` repository lists the
assets the generator cannot recreate — among them the master materials
(`MM_FactoryBaked`, `MM_Factory_Array`, the `MF_*` material functions). Until they are back
every material in the project is a stub. Copy each listed file from `Content_PreGenerate\`
into the new `Content\`, skipping `TX_*` (textures; the generated ones are the real thing):

```powershell
Set-Location '<project>\..'
git clone --depth 1 https://github.com/satisfactorymodding/UnrealProjectUpdater.git
$src  = '<project>\Content_PreGenerate'
$dst  = '<project>\Content'
$list = '<project>\..\UnrealProjectUpdater\CustomAssets.txt'
$copied = 0; $skipped = 0; $missing = @()
Get-Content $list | Where-Object { $_.Trim() -and $_ -notmatch '^\s*#' } | ForEach-Object {
    $rel  = $_.Trim() -replace '/', '\'
    if ((Split-Path $rel -Leaf) -like 'TX_*') { $skipped++; return }
    $from = Join-Path $src $rel; $to = Join-Path $dst $rel
    if (Test-Path $from) {
        New-Item -ItemType Directory -Path (Split-Path $to -Parent) -Force | Out-Null
        Copy-Item $from $to -Force; $copied++
    } else { $missing += $rel }
}
"copied $copied, skipped $skipped TX_ files, missing $($missing.Count)"; $missing
New-Item -ItemType Directory -Path "$dst\Localization" -Force | Out-Null
Copy-Item "$src\Localization\StringTables" "$dst\Localization\" -Recurse -Force
```

A few `missing` entries are expected (the list spans game versions); `MM_FactoryBaked` or
`MM_Factory_Array` missing is not. Keep `Content_PreGenerate\` until an Alpakit package
succeeds.

## 9. Repair the generated material parents

The generator writes no parent links, so nearly every base-game material instance lands on
`WorldGridMaterial`. Run `Scripts/acpr_fix_material_parents.py` in the editor (it backs up
every `.uasset` it touches and reads each write back).

## 10. Clean up

Run an Alpakit package of the mod and delete whatever broken assets it reports. Remove the
AssetDumper mod from `<game>\FactoryGame\Mods\`. Delete `<dump>`, then remove the junction
with `cmd /c rmdir "<game>\FactoryGame\AssetDump"` (not `Remove-Item`, which would follow
it). Keep the generated Content out of git: `git update-index --assume-unchanged Content/*`.
