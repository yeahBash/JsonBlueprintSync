#pragma once

#include "CoreMinimal.h"

/**
 * Merges of assets made outside the editor: the TortoiseGit merge tool (Tools/uasset-merge.js) saves the merged UnrealAssetJson
 * merge text of an asset to <Project>/Saved/JsonBlueprintSync/Merge/<package path>/merged.txt. This compares it with the
 * reader's text of the saved asset, key by key, and lists what to change.
 */
namespace JsonBlueprintSync::AssetMerge
{
	/** One "<path> = <value>" line. */
	struct FMergeLine
	{
		FString Path;
		FString Value;
		/** 1-based line in the file. */
		int32 LineNumber = 0;
	};

	/** "[<object path>] <class>" and its lines. */
	struct FMergeSection
	{
		FString Object;
		FString Class;
		TArray<FMergeLine> Lines;
		int32 LineNumber = 0;

		const FMergeLine* FindLine(const FString& Path) const;
	};

	/** The section of hashes for parts of the file the merge can't change (package tables, thumbnails). */
	inline const TCHAR* IgnoredSection = TEXT("!ignored");

	struct FMergeText
	{
		/** From the first line, "# <format>", e.g. "UnrealAssetJson merge text 3". */
		FString Format;
		/** From "# Asset: <package>". */
		FString Asset;
		TArray<FMergeSection> Sections;
		/** Lines of unresolved conflicts ("<<<<<<<", "|||||||", "=======", ">>>>>>>"). */
		TArray<int32> ConflictLines;
		/** "Line <n>: <problem>". */
		TArray<FString> Errors;

		const FMergeSection* FindSection(const FString& Object) const;
	};

	/** Parses merge text. Returns false if it has errors or conflict markers; what could be read is still filled in. */
	bool ParseMergeText(const FString& Text, FMergeText& Out);

	enum class EMergeAction : uint8
	{
		/** The merged value differs from the asset's. */
		Set,
		/** A map or set line whose merged value is "(added)". */
		Add,
		/** A map or set line whose merged value is "(removed)". */
		Remove,
		/** The line is only in the asset's text: the merged value is the archetype's. */
		Reset,
		/** The plugin can't make this change; Reason says why. */
		NotApplicable,
	};

	const TCHAR* LexToString(EMergeAction Action);

	struct FMergeRow
	{
		/** Object path of the section. */
		FString Object;
		/** Empty for a row about the whole object (added, removed, class changed). */
		FString Path;
		/** Unset if the asset's text has no such line (inherited, or the object doesn't exist). */
		TOptional<FString> Current;
		/** Unset if merged.txt has no such line. */
		TOptional<FString> Merged;
		EMergeAction Action = EMergeAction::Set;
		/** Why the row is not applicable. */
		FString Reason;
	};

	/**
	 * What to change in the asset (Current: the reader's text of the saved asset) to get Merged. Sections in Merged's order, then
	 * the ones only in Current; in a section, lines in Merged's order, then the ones only in Current. The ignored section is
	 * skipped.
	 */
	TArray<FMergeRow> DiffMergeText(const FMergeText& Current, const FMergeText& Merged);

	/** <ProjectSavedDir>/JsonBlueprintSync/Merge */
	FString GetMergeDir();

	struct FMergeFileLocation
	{
		/** "/Game/Features/UI/WBP_HUD", or "/<Plugin>/..." for a plugin's asset. */
		FString PackageName;
		/** Absolute path of merged.txt. */
		FString MergedFile;
	};

	/** Every merged.txt below MergeDir, sorted by package. The folder below MergeDir is the package path. */
	TArray<FMergeFileLocation> FindMergeFiles(const FString& MergeDir);

	/** The reader's path from git config unrealassetjson.path, read in the project folder. False with a message if not set. */
	bool FindReader(FString& OutReader, FString& OutError);

	/** Runs the reader with -merge on an asset file and returns its text. False with the reader's output if it failed. */
	bool RunReader(const FString& Reader, const FString& AssetFile, FString& OutText, FString& OutError);

	enum class EMergeStatus : uint8
	{
		/** There are rows to apply. */
		Ready,
		/** The saved asset already matches merged.txt, apart from rows that aren't applicable. */
		Done,
		HasConflictMarkers,
		HasErrors,
		/** merged.txt has another merge text format than the reader writes now. */
		FormatDiffers,
		AssetNotFound,
		ReaderNotSetUp,
		ReaderFailed,
		/** The asset is loaded with unsaved changes; the reader reads the saved file, so the rows may be wrong. */
		UnsavedChanges,
	};

	const TCHAR* LexToString(EMergeStatus Status);

	/** Whether rows of a file with this status may be applied. */
	inline bool CanApply(EMergeStatus Status) { return Status == EMergeStatus::Ready; }

	struct FMergeFile
	{
		FMergeFileLocation Location;
		/** Absolute path of the .uasset or .umap; empty if not found. */
		FString AssetFile;
		EMergeStatus Status = EMergeStatus::Ready;
		/** Details for a status other than Ready and Done. */
		FString Message;
		TArray<FMergeRow> Rows;
	};

	/** Gets the reader's text of an asset file. Returns false with a message if it couldn't. */
	using FReadAsset = TFunctionRef<bool(const FString& AssetFile, FString& OutText, FString& OutError)>;

	/**
	 * Status and rows of a merge file. ReadAsset gives the current text (RunReader in the editor). bAssetDirty: the asset is
	 * loaded with unsaved changes.
	 */
	FMergeFile CheckMergeFile(const FMergeFileLocation& Location, const FString& AssetFile, bool bAssetDirty, FReadAsset ReadAsset);

	/** CheckMergeFile for every merge file of the project, with the reader from git config. */
	TArray<FMergeFile> CheckMergeFiles();

	enum class ERowResult : uint8
	{
		Applied,
		/** The asset already has the merged value; nothing was written. */
		Unchanged,
		/** Not written; Message says why. */
		Failed,
	};

	const TCHAR* LexToString(ERowResult Result);

	struct FRowResult
	{
		ERowResult Result = ERowResult::Failed;
		FString Message;
	};

	/**
	 * Writes Set and Reset rows to the loaded objects of Asset's package, each value by its path (struct members one by one), the
	 * way the Details panel does: archetype instances that still have the old value at that path (child Blueprints, data assets of
	 * the class, placed actors) get the new value too, and overrides are kept. A reset takes the value at the same path in the
	 * object's archetype, which is what the saved file is compared with. Only values the Details panel can edit are written.
	 *
	 * Each row on its own: a row that fails is reported and skipped, the others are still written. Arrays, maps, sets and instanced
	 * structs fail for now. Marks the asset modified if anything was written. No transaction, compile or save: the caller does those.
	 * Returns one result per row.
	 */
	TArray<FRowResult> ApplyRows(UObject* Asset, TConstArrayView<FMergeRow> Rows);
}
