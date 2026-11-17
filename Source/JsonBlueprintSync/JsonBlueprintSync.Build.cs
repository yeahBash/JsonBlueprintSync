using UnrealBuildTool;

// Editor-only module: syncs Blueprint class defaults and data asset values with JSON files.
// See README.md in the plugin folder.
public class JsonBlueprintSync : ModuleRules
{
	public JsonBlueprintSync(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// DeveloperSettings: the public settings header derives from UDeveloperSettings.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "DeveloperSettings" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"BlueprintGraph",
			"Kismet",
			"KismetCompiler",
			"AssetRegistry",
			"Json",
			"JsonUtilities",
			"EditorSubsystem",
			"Slate",
			"SlateCore",
			"InputCore",
			"WorkspaceMenuStructure",
			"UMG",
			// Only the tests use it, to build Widget Blueprints; the library reads them through UnrealEd's UBaseWidgetBlueprint.
			"UMGEditor",
		});
	}
}
