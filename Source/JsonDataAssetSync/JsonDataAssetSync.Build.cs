using UnrealBuildTool;

// Editor-only module: syncs data asset values with JSON files.
// See README.md in the plugin folder.
public class JsonDataAssetSync : ModuleRules
{
	public JsonDataAssetSync(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// DeveloperSettings: the public settings header derives from UDeveloperSettings.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "DeveloperSettings" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			// Only the tests use it, for the pin types of the Blueprint data asset classes they build.
			"BlueprintGraph",
			"AssetRegistry",
			// IPluginManager, for the content roots of the project's plugins.
			"Projects",
			"Json",
			"JsonUtilities",
			"EditorSubsystem",
			"Slate",
			"SlateCore",
			"InputCore",
			"ToolMenus",
			"WorkspaceMenuStructure",
		});
	}
}
