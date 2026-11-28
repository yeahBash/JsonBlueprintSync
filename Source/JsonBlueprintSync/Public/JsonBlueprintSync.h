#pragma once

#include "CoreMinimal.h"

class FProperty;
class UObject;
struct FAssetData;

namespace JsonBlueprintSync
{
	/**
	 * In-memory form of one data asset's JSON file.
	 *
	 * Values are Unreal property text (the Details-panel copy/paste format), diffed against the asset's archetype, the class defaults.
	 * The document is complete: a key that is missing means the class default.
	 */
	struct JSONBLUEPRINTSYNC_API FAssetDefaults
	{
		static constexpr int32 SchemaVersion = 2;

		/** Package name, e.g. /Game/Features/Light/DA_Lamp. */
		FString Asset;

		/** Path of the asset's class, e.g. /Script/MyGame.LampData or /Game/Features/Light/BP_LampData.BP_LampData_C. */
		FString Parent;

		/** "overrides.self": property name -> value text. */
		TMap<FString, FString> Self;
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

	/** Whether the object is a data asset (an instance of a UDataAsset subclass, native or Blueprint), the only kind that is synced. */
	JSONBLUEPRINTSYNC_API bool IsDataAsset(const UObject* Asset);

	/** Works without loading the asset, also for data assets whose class is a Blueprint. */
	JSONBLUEPRINTSYNC_API bool IsDataAsset(const FAssetData& Asset);

	/**
	 * The content roots whose data assets are synced: /Game and the mounted content of the project's own plugins, e.g. /MyPlugin.
	 * Engine and engine plugin content is not synced. A plugin that is disabled is not mounted, so its JSON files are not checked.
	 */
	JSONBLUEPRINTSYNC_API TArray<FString> GetSyncedRoots();

	/** Whether the package is under a synced root and not in a folder the ExcludedFolders setting leaves out. Temp packages never are. */
	JSONBLUEPRINTSYNC_API bool IsSyncedPackage(const FString& PackageName);

	/** Files and assets that lack their other half. */
	struct JSONBLUEPRINTSYNC_API FMissingPairs
	{
		/** JSON files (full paths) under a synced root whose data asset does not exist, or whose asset is not a data asset. */
		TArray<FString> JsonWithoutAsset;
		/** Package names of synced data assets that have no JSON file. */
		TArray<FString> AssetsWithoutJson;

		bool IsEmpty() const { return JsonWithoutAsset.Num() == 0 && AssetsWithoutJson.Num() == 0; }
	};

	/** Compares the JSON files under the synced roots with the synced data assets the asset registry knows. Both lists are sorted. */
	JSONBLUEPRINTSYNC_API FMissingPairs FindMissingPairs(const FString& JsonRoot);

	/** JSON root folder: the JsonDir project setting, relative to the project dir. */
	JSONBLUEPRINTSYNC_API FString GetDefaultJsonRoot();

	/** /Game/A/DA_B -> <JsonRoot>/Game/A/DA_B.json */
	JSONBLUEPRINTSYNC_API FString GetJsonPath(const FString& JsonRoot, const FString& PackageName);

	/** Inverse of GetJsonPath. Returns false if the file is not under JsonRoot or is not a .json file. */
	JSONBLUEPRINTSYNC_API bool GetPackageName(const FString& JsonRoot, const FString& JsonPath, FString& OutPackageName);

	/** Reads the data asset's current values. */
	JSONBLUEPRINTSYNC_API FAssetDefaults Capture(UObject* Asset);

	/** Deterministic JSON: fixed top-level order, sorted keys, two-space indent, LF line endings, trailing newline. */
	JSONBLUEPRINTSYNC_API FString ToJson(const FAssetDefaults& Doc);

	JSONBLUEPRINTSYNC_API bool FromJson(const FString& Json, FAssetDefaults& OutDoc, FString& OutError);

	/** Key -> value for every value in the document. */
	JSONBLUEPRINTSYNC_API TMap<FString, FString> Flatten(const FAssetDefaults& Doc);

	/** True if both documents hold the same values (the Asset and Parent fields are not compared). */
	JSONBLUEPRINTSYNC_API bool ValuesEqual(const FAssetDefaults& A, const FAssetDefaults& B);

	/**
	 * Makes the data asset's values match the document: keys in the document are set, values the asset overrides that the document
	 * does not list are reset to the class default. All values are validated first; on any error nothing is written.
	 * Does not mark modified or save. Wrap in an FScopedTransaction in the editor.
	 */
	JSONBLUEPRINTSYNC_API FApplyResult Apply(UObject* Asset, const FAssetDefaults& Doc);

	/** Apply, then MarkModified if anything changed. Never saves. */
	JSONBLUEPRINTSYNC_API FApplyResult Import(UObject* Asset, const FAssetDefaults& Doc);

	/** Marks the asset's package dirty. */
	JSONBLUEPRINTSYNC_API void MarkModified(UObject* Asset);

	/**
	 * Parses property text (the Details-panel copy/paste format) into Dest, strictly: the whole text must be read, and nothing may be
	 * reported, including unknown struct fields, which Unreal reports only at Verbose. Struct text that lists only some fields leaves
	 * the others as they are in Dest.
	 */
	JSONBLUEPRINTSYNC_API bool ImportValue(const FProperty* Property, const FString& Text, void* Dest, UObject* Owner, FString& OutError);

	JSONBLUEPRINTSYNC_API FString NormalizeLineEndings(const FString& Text);

	/** Loads and parses a JSON file. Returns false if it is missing or invalid (OutError is empty if it is missing). */
	JSONBLUEPRINTSYNC_API bool LoadFile(const FString& Path, FAssetDefaults& OutDoc, FString& OutError);

	/** Writes ToJson(Doc) as UTF-8 without BOM, only if it differs from the file (ignoring line endings). Returns false on write failure. */
	JSONBLUEPRINTSYNC_API bool SaveFileIfChanged(const FString& Path, const FAssetDefaults& Doc, bool& bOutWritten);

	/** JSON files under the folder that mirrors Dir (e.g. /Game), as normalized paths, sorted. */
	JSONBLUEPRINTSYNC_API TArray<FString> FindJsonFiles(const FString& JsonRoot, const FString& Dir);

	/** Loads the main asset of a package such as /Game/A/DA_B. */
	JSONBLUEPRINTSYNC_API UObject* LoadAsset(const FString& PackageName);

	/** MD5 of the UTF-8 text with normalized line endings. */
	JSONBLUEPRINTSYNC_API FString HashText(const FString& Text);
}
