using UnrealBuildTool;

// Editor-only module: syncs Blueprint class-default overrides with JSON files.
// See README.md in the plugin folder.
public class JsonBlueprintSync : ModuleRules
{
	public JsonBlueprintSync(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"BlueprintGraph",
			"Kismet",
			"KismetCompiler",
			"AssetRegistry",
			"Json",
			"JsonUtilities",
			"DirectoryWatcher",
			"EditorSubsystem",
			"Slate",
			"SlateCore",
		});
	}
}
