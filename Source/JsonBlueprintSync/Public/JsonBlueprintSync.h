#pragma once

#include "CoreMinimal.h"

class UObject;
struct FAssetData;

namespace JsonBlueprintSync
{
	/** The kinds of asset that are synced. Each has its own mode in the project settings. */
	enum class EAssetKind : uint8
	{
		/** Not synced. */
		None,
		/** Any Blueprint except Widget Blueprints: class defaults and component templates. */
		Blueprint,
		/** Widget Blueprint: class defaults and the Designer's widgets. */
		WidgetBlueprint,
		/** Instance of a UDataAsset subclass (native or Blueprint class): the asset's own properties. */
		DataAsset,
	};

	/**
	 * In-memory form of one asset's JSON file.
	 *
	 * Values are Unreal property text (the Details-panel copy/paste format), diffed against the archetype: the parent class CDO for a
	 * Blueprint's own defaults, the parent template for components, the widget class CDO for widgets, and the asset's class CDO for a
	 * data asset. The document is complete: a key that is missing means the archetype's value.
	 */
	struct JSONBLUEPRINTSYNC_API FAssetDefaults
	{
		static constexpr int32 SchemaVersion = 2;

		/** Package name, e.g. /Game/Features/Light/BP_Lamp. */
		FString Asset;

		/** Blueprints: path of the parent class, e.g. /Script/Engine.Actor. Data assets: path of the asset's class. */
		FString Parent;

		/** Property name -> value text: the class default object for a Blueprint, the asset itself for a data asset. */
		TMap<FString, FString> Defaults;

		/** Component name -> property name -> value text, for component templates. */
		TMap<FString, TMap<FString, FString>> Components;

		/**
		 * Widget Blueprints only: widget name -> property name -> value text, for the widgets in the Designer's widget tree,
		 * diffed against the widget class defaults. "<Widget>.Slot" holds the layout of the widget in its parent panel.
		 */
		TMap<FString, TMap<FString, FString>> Widgets;
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

	JSONBLUEPRINTSYNC_API EAssetKind GetAssetKind(const UObject* Asset);

	/** Works without loading the asset, also for data assets whose class is a Blueprint. */
	JSONBLUEPRINTSYNC_API EAssetKind GetAssetKind(const FAssetData& Asset);

	JSONBLUEPRINTSYNC_API const TCHAR* LexToString(EAssetKind Kind);

	/** JSON root folder: the JsonDir project setting, relative to the project dir. */
	JSONBLUEPRINTSYNC_API FString GetDefaultJsonRoot();

	/** /Game/A/BP_B -> <JsonRoot>/Game/A/BP_B.json */
	JSONBLUEPRINTSYNC_API FString GetJsonPath(const FString& JsonRoot, const FString& PackageName);

	/** Inverse of GetJsonPath. Returns false if the file is not under JsonRoot or is not a .json file. */
	JSONBLUEPRINTSYNC_API bool GetPackageName(const FString& JsonRoot, const FString& JsonPath, FString& OutPackageName);

	/** Reads the asset's current values. The asset must be of a synced kind. */
	JSONBLUEPRINTSYNC_API FAssetDefaults Capture(UObject* Asset);

	/** Deterministic JSON: fixed top-level order, sorted keys, two-space indent, LF line endings, trailing newline. */
	JSONBLUEPRINTSYNC_API FString ToJson(const FAssetDefaults& Doc);

	JSONBLUEPRINTSYNC_API bool FromJson(const FString& Json, FAssetDefaults& OutDoc, FString& OutError);

	/** Key -> value for every value in the document: "Property", "Component.Property", "Widget.Property", "Widget.Slot.Property". */
	JSONBLUEPRINTSYNC_API TMap<FString, FString> Flatten(const FAssetDefaults& Doc);

	/** True if both documents hold the same values (the Asset and Parent fields are not compared). */
	JSONBLUEPRINTSYNC_API bool ValuesEqual(const FAssetDefaults& A, const FAssetDefaults& B);

	/**
	 * Makes the asset's values match the document: keys in the document are set, values the asset overrides that the document does not
	 * list are reset to the archetype. All values are validated first; on any error nothing is written. Each changed value is also copied
	 * to archetype instances that were inheriting it (child Blueprint CDOs, data assets of a Blueprint class, loaded instances).
	 * Does not compile, mark modified or save. Wrap in an FScopedTransaction in the editor.
	 */
	JSONBLUEPRINTSYNC_API FApplyResult Apply(UObject* Asset, const FAssetDefaults& Doc);

	/** Apply, then MarkModified and, for Blueprints, compile if anything changed. Never saves. */
	JSONBLUEPRINTSYNC_API FApplyResult Import(UObject* Asset, const FAssetDefaults& Doc);

	/** Blueprints: MarkBlueprintAsModified. Data assets: marks the package dirty. */
	JSONBLUEPRINTSYNC_API void MarkModified(UObject* Asset);

	/** Apply in ascending order: parent Blueprints before children, Blueprints before data assets (whose class may be one of them). */
	JSONBLUEPRINTSYNC_API int32 GetApplyOrder(const UObject* Asset);

	JSONBLUEPRINTSYNC_API FString NormalizeLineEndings(const FString& Text);

	/** Loads and parses a JSON file. Returns false if it is missing or invalid (OutError is empty if it is missing). */
	JSONBLUEPRINTSYNC_API bool LoadFile(const FString& Path, FAssetDefaults& OutDoc, FString& OutError);

	/** Writes ToJson(Doc) as UTF-8 without BOM, only if it differs from the file (ignoring line endings). Returns false on write failure. */
	JSONBLUEPRINTSYNC_API bool SaveFileIfChanged(const FString& Path, const FAssetDefaults& Doc, bool& bOutWritten);

	/** JSON files under the folder that mirrors Dir (e.g. /Game), as normalized paths, sorted. */
	JSONBLUEPRINTSYNC_API TArray<FString> FindJsonFiles(const FString& JsonRoot, const FString& Dir);

	/** Loads the main asset of a package such as /Game/A/BP_B. */
	JSONBLUEPRINTSYNC_API UObject* LoadAsset(const FString& PackageName);

	/** MD5 of the UTF-8 text with normalized line endings. */
	JSONBLUEPRINTSYNC_API FString HashText(const FString& Text);
}
