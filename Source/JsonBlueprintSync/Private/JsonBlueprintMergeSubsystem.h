#pragma once

#include "AssetMerge.h"
#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "EditorUndoClient.h"
#include "Misc/Guid.h"

#include "JsonBlueprintMergeSubsystem.generated.h"

class SNotificationItem;
class UPackage;
class FObjectPostSaveContext;

/**
 * The asset merges saved by the merge tool (see AssetMerge.h) while the editor runs: lists them in the Asset Merges tab, applies the
 * rows the user picks, and works a merge out again when its asset is saved.
 *
 * The merge files are checked with the JSON files: when the editor starts and on JsonBlueprintSync.Sync. Each check runs the reader
 * on each asset. Nothing is recorded on disk; a merge file stays until the user deletes it.
 *
 * Console commands: JsonBlueprintSync.Merges (check and log), JsonBlueprintSync.ApplyMerge <package> [save].
 */
UCLASS()
class UJsonBlueprintMergeSubsystem : public UEditorSubsystem, public FEditorUndoClient
{
	GENERATED_BODY()

public:
	/** A merge file as the last check saw it, and what this session did with it since its asset was last saved. */
	struct FEntry
	{
		JsonBlueprintSync::AssetMerge::FMergeFile File;
		/** Rows applied by Apply since the asset was last saved, by row key, with the transaction that applied them. */
		TMap<FString, FGuid> Applied;
		/** Rows that failed at the last Apply, by row key, with the reason. */
		TMap<FString, FString> Failures;
		/** Apply changed the asset since it was last saved, so its unsaved changes are (at least partly) the merge's. */
		bool bChangedByApply = false;
	};

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FEditorUndoClient: undoing an Apply makes its rows applicable again; redoing it marks them applied again.
	virtual bool MatchesContext(const FTransactionContext& InContext, const TArray<TPair<UObject*, FTransactionObjectEvent>>& TransactionObjectContexts) const override;
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

	/** Checks every merge file again (runs the reader on each asset). bNotify: shows the "N merge(s) to apply" notification. */
	void Refresh(bool bNotify);

	/** The merge files, sorted by package. */
	const TArray<FEntry>& GetEntries() const { return Entries; }
	const FEntry* FindEntry(const FString& PackageName) const;

	/**
	 * The status now: the checked one, except that an asset that has unsaved changes Apply didn't make is UnsavedChanges. The reader
	 * reads the saved file, so its rows don't show such changes.
	 */
	JsonBlueprintSync::AssetMerge::EMergeStatus GetStatus(const FEntry& Entry) const;
	FString GetStatusMessage(const FEntry& Entry) const;

	/** Whether rows of the file may be applied now. */
	bool CanApply(const FEntry& Entry) const { return JsonBlueprintSync::AssetMerge::CanApply(GetStatus(Entry)); }

	/** The row was applied in this session (and not undone), but the asset hasn't been saved since. */
	bool IsAppliedNotSaved(const FEntry& Entry, const FString& RowKey) const;

	/** How many merge files are not Done. */
	int32 GetToApplyNum() const;

	/** Identifies a row of a file: its object and path. */
	static FString GetRowKey(const JsonBlueprintSync::AssetMerge::FMergeRow& Row);

	/** The rows to apply, by package, as row keys. */
	using FApplyRequest = TPair<FString, TArray<FString>>;

	/**
	 * Applies the given rows of each file whose status allows it: all of them in one transaction (one undo step), parents before
	 * children, skipping rows that are not applicable or already applied. bSave: then compiles the changed Blueprints and saves the
	 * changed assets; saving works the merges out again. Returns how many rows were applied.
	 */
	int32 Apply(const TArray<FApplyRequest>& Requests, bool bSave);

	/** Deletes the merge file of the package and its folders that are left empty. Returns false (with a toast) if it can't. */
	bool DeleteMergeFile(const FString& PackageName);

	/** Broadcast when the list or a status changes. */
	FSimpleMulticastDelegate OnChanged;

	/** Opens the Asset Merges tab. */
	static void OpenMergeTab();

private:
	FEntry* FindMutableEntry(const FString& PackageName);

	/** Replaces the entries of these packages (all entries if Packages is unset) with a new check of their files. */
	void Check(const TOptional<TSet<FString>>& Packages);

	void ShowNotification();
	void OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext Context);
	bool ProcessSaved(float DeltaTime);

	void LogEntries() const;
	void ApplyCommand(const TArray<FString>& Args);

	TArray<FEntry> Entries;

	/** Apply transactions that are undone now. */
	TSet<FGuid> UndoneTransactions;

	/** The transaction being undone or redone, from MatchesContext. */
	mutable FGuid UndoRedoTransactionId;

	/** Packages with a merge file that were saved; checked again on the next tick, once the save has finished. */
	TSet<FString> SavedPackages;
	FTSTicker::FDelegateHandle SavedTicker;

	TWeakPtr<SNotificationItem> Notification;

	FDelegateHandle PackageSavedHandle;
	IConsoleObject* MergesCommand = nullptr;
	IConsoleObject* ApplyMergeCommand = nullptr;
};
