#pragma once

#include "CoreMinimal.h"

class UBlueprint;

namespace JsonBlueprintSync
{
	/**
	 * In-memory form of one Blueprint's defaults JSON file.
	 *
	 * Values are Unreal property text (the Details-panel copy/paste format), diffed against the archetype:
	 * the parent class CDO for the Blueprint's own defaults, and the parent template for components.
	 */
	struct JSONBLUEPRINTSYNC_API FBlueprintDefaults
	{
		static constexpr int32 SchemaVersion = 1;

		/** Package name, e.g. /Game/Features/Light/BP_Lamp. */
		FString Blueprint;

		/** Path of the parent class, e.g. /Script/Engine.Actor. */
		FString Parent;

		/** Property name -> value text, for the class default object. */
		TMap<FString, FString> Defaults;

		/** Component name -> property name -> value text, for component templates. */
		TMap<FString, TMap<FString, FString>> Components;

		/**
		 * Widget Blueprints only: widget name -> property name -> value text, for the widgets in the Designer's widget tree,
		 * diffed against the widget class defaults. "<Widget>.Slot" holds the layout of the widget in its parent panel.
		 */
		TMap<FString, TMap<FString, FString>> Widgets;

		/** Keys to reset to the archetype value: "Property", "Component.Property", "Widget.Property" or "Widget.Slot.Property". Sorted. */
		TArray<FString> Reset;
	};

	struct JSONBLUEPRINTSYNC_API FApplyResult
	{
		/** Keys whose value changed. */
		TArray<FString> Changed;
		TArray<FString> Warnings;
		/** If non-empty, nothing was applied. */
		TArray<FString> Errors;

		bool HasErrors() const { return Errors.Num() > 0; }
	};

	/** JSON root folder: [JsonBlueprintSync] JsonDir in the editor ini, relative to the project dir. Defaults to JsonBlueprintSync. */
	JSONBLUEPRINTSYNC_API FString GetDefaultJsonRoot();

	/** /Game/A/BP_B -> <JsonRoot>/Game/A/BP_B.json */
	JSONBLUEPRINTSYNC_API FString GetJsonPath(const FString& JsonRoot, const FString& PackageName);

	/** Inverse of GetJsonPath. Returns false if the file is not under JsonRoot or is not a .json file. */
	JSONBLUEPRINTSYNC_API bool GetPackageName(const FString& JsonRoot, const FString& JsonPath, FString& OutPackageName);

	/**
	 * Reads the Blueprint's current defaults.
	 * Previous is the JSON as last written. Keys it listed that no longer differ go into Reset, so that the reset reaches other machines.
	 */
	JSONBLUEPRINTSYNC_API FBlueprintDefaults Capture(UBlueprint* Blueprint, const FBlueprintDefaults* Previous = nullptr);

	/** Deterministic JSON: fixed top-level order, sorted keys, two-space indent, LF line endings, trailing newline. */
	JSONBLUEPRINTSYNC_API FString ToJson(const FBlueprintDefaults& Doc);

	JSONBLUEPRINTSYNC_API bool FromJson(const FString& Json, FBlueprintDefaults& OutDoc, FString& OutError);

	/**
	 * Writes the document's values into the Blueprint's CDO and component templates. Keys missing from the document are left untouched.
	 * All values are validated first; on any error nothing is written. Each changed value is also copied to archetype instances that
	 * were inheriting it (child Blueprint CDOs and loaded instances). Does not compile or save. Wrap in an FScopedTransaction in the editor.
	 */
	JSONBLUEPRINTSYNC_API FApplyResult Apply(UBlueprint* Blueprint, const FBlueprintDefaults& Doc);

	/** Apply, then mark the Blueprint modified and compile it if anything changed. Never saves. */
	JSONBLUEPRINTSYNC_API FApplyResult Import(UBlueprint* Blueprint, const FBlueprintDefaults& Doc);

	/** Number of Blueprint classes above this one. Import parents (lower depth) before children. */
	JSONBLUEPRINTSYNC_API int32 GetBlueprintDepth(const UBlueprint* Blueprint);

	JSONBLUEPRINTSYNC_API FString NormalizeLineEndings(const FString& Text);

	/** Loads and parses a JSON file. Returns false if it is missing or invalid (OutError is empty if it is missing). */
	JSONBLUEPRINTSYNC_API bool LoadFile(const FString& Path, FBlueprintDefaults& OutDoc, FString& OutError);

	/** Writes ToJson(Doc) as UTF-8 without BOM, only if it differs from the file (ignoring line endings). Returns false on write failure. */
	JSONBLUEPRINTSYNC_API bool SaveFileIfChanged(const FString& Path, const FBlueprintDefaults& Doc, bool& bOutWritten);

	/** JSON files under the folder that mirrors Dir (e.g. /Game), as normalized paths, sorted. */
	JSONBLUEPRINTSYNC_API TArray<FString> FindJsonFiles(const FString& JsonRoot, const FString& Dir);

	/** Loads the Blueprint asset of a package such as /Game/A/BP_B. */
	JSONBLUEPRINTSYNC_API UBlueprint* LoadBlueprint(const FString& PackageName);

	/** MD5 of the UTF-8 text with normalized line endings. */
	JSONBLUEPRINTSYNC_API FString HashText(const FString& Text);
}
