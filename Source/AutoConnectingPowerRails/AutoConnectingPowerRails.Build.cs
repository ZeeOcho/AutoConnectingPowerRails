using UnrealBuildTool;
using System.IO;
using System;

public class AutoConnectingPowerRails : ModuleRules
{
	public AutoConnectingPowerRails(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// FactoryGame transitive dependencies
		// Not all of these are required, but including the extra ones saves you from having to add them later.
		// Some entries are commented out to avoid compile-time warnings about depending on a module that you don't explicitly depend on.
		// You can uncomment these as necessary when your code actually needs to use them.
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject",
			"Engine",
			"DeveloperSettings",
			"PhysicsCore",
			"InputCore",
			//"OnlineSubsystem", "OnlineSubsystemUtils", "OnlineSubsystemNull",
			//"SignificanceManager",
			"GeometryCollectionEngine",
			//"ChaosVehiclesCore", "ChaosVehicles", "ChaosSolverEngine",
			"AnimGraphRuntime",
			//"AkAudio",
			"AssetRegistry",
			"NavigationSystem",
			//"ReplicationGraph",
			"AIModule",
			"GameplayTasks",
			"SlateCore", "Slate", "UMG",
			//"InstancedSplines",
			"RenderCore",
			"CinematicCamera",
			"Foliage",
			//"Niagara",
			//"EnhancedInput",
			//"GameplayCameras",
			//"TemplateSequence",
			"NetCore",
			"GameplayTags",
			"Json", "JsonUtilities"
		});

		// FactoryGame plugins
		PublicDependencyModuleNames.AddRange(new string[] {
			// AACPRRail post-scales the FInstanceData it hands back, so the module needs
			// InstanceData.h and the symbols behind it.
			"AbstractInstance",
			//"InstancedSplinesComponent",
			//"SignificanceISPC"
		});

		// Header stubs
		PublicDependencyModuleNames.AddRange(new string[] {
			"DummyHeaders",
		});

		if (Target.Type == TargetRules.TargetType.Editor) {
			PublicDependencyModuleNames.AddRange(new string[] {/*"OnlineBlueprintSupport",*/ "AnimGraph"});
		}
		PublicDependencyModuleNames.AddRange(new string[] {"FactoryGame", "SML"});

		// The game changelist the FactoryGame headers in this Starter Project were taken from — the
		// file SML's own build reads for FACTORYGAME_VERSION. The binary is laid out by these headers
		// (every vtable of every class we derive from), so the module logs this number beside the
		// running game's at startup and warns when they differ. 0 when the file is missing.
		string headersVersionFile = Path.Combine(Target.ProjectFile!.Directory.FullName, "Source", "FactoryGame", "currentVersion.txt");
		string headersVersion = File.Exists(headersVersionFile) ? File.ReadAllText(headersVersionFile).Trim() : "";
		int headersChangelist;
		if (!int.TryParse(headersVersion, out headersChangelist)) {
			headersChangelist = 0;
		}
		PrivateDefinitions.Add("ACPR_HEADERS_CL=" + headersChangelist);

		PublicIncludePaths.AddRange(new string[] {
			// ... add public include paths required here ...
		});
		
		PrivateIncludePaths.AddRange(new string[] {
			// ... add private include paths required here ...
		});
		
		PublicDependencyModuleNames.AddRange(new string[] {
			// ... add public dependencies that you statically link with here ...
		});
		
		PrivateDependencyModuleNames.AddRange(new string[] {
			// ... add private dependencies that you statically link with here ...	
		});
		
		DynamicallyLoadedModuleNames.AddRange(new string[] {
			// ... add any modules that your module loads dynamically here ...
		});
	}
}
