#pragma once

#include "JsonBlueprintSync.h"
#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "EditorUndoClient.h"
#include "Misc/Guid.h"
#include "SyncState.h"

#include "JsonBlueprintSyncSubsystem.generated.h"

class SNotificationItem;
class UPackage;
class FObjectPostSaveContext;
struct FAssetData;

/**
 * Keeps the JSON files in sync with the assets while the editor runs (see the plugin README.md):
 * - Saving an asset writes its JSON, unless the JSON has changes that are not reviewed yet (then the asset is "pending").
 * - When the asset registry has finished loading, finds JSON files that changed since the last sync and differ from their asset.
 *   They are listed in the JSON Sync tab. Data assets in Two Way mode get the JSON applied automatically when nothing conflicts.
 * - Moves the JSON file when an asset is renamed and deletes it when the asset is deleted.
 * - Undoing an Apply JSON puts the asset back in the JSON Sync tab; a JSON that changes again after it was reviewed is not overwritten.
 *
 * Not created in commandlets (use -run=JsonBlueprintSync there). Console command JsonBlueprintSync.Sync reruns the check,
 * e.g. after a git pull with the editor open.
 */
UCLASS()
class UJsonBlueprintSyncSubsystem : public UEditorSubsystem, public FEditorUndoClient
{
	GENERATED_BODY()

public:
	/** A pending asset as the JSON Sync tab shows it. */
	struct FPendingView
	{
		FString PackageName;
		JsonBlueprintSync::EAssetKind Kind = JsonBlueprintSync::EAssetKind::None;
		/** Set if the JSON cannot be read or the asset cannot be loaded; then only Overwrite JSON is possible. */
		FString Error;
		/** Keys that differ between the JSON and the asset now. */
		TArray<JsonBlueprintSync::FKeyDiff> Keys;
		/** False if it is unknown which side changed what (every key is then a conflict). */
		bool bHasBase = false;
		/**
		 * Set if the JSON names a different parent (Blueprints) or class (data assets) than the asset has, e.g. after a reparent.
		 * The JSON's values are then relative to another archetype, so nothing is applied automatically.
		 */
		FString JsonParent;
		FString AssetParent;

		bool ParentDiffers() const { return !JsonParent.IsEmpty(); }

		/** The JSON has nothing of its own to apply: every key was changed in the asset (e.g. the .uasset was committed alone). */
		bool IsJsonOutOfDate() const;
	};

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FEditorUndoClient: undoing an Apply JSON makes the asset pending again; redoing it resolves it again.
	virtual bool MatchesContext(const FTransactionContext& InContext, const TArray<TPair<UObject*, FTransactionObjectEvent>>& TransactionObjectContexts) const override;
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

	/** Finds JSON files that changed since the last sync and differ from their asset; applies Two Way ones that do not conflict. */
	void SyncFromJson();

	/** Pending package names, sorted. */
	TArray<FString> GetPendingPackages() const;

	/** Compares the pending asset with its JSON now. Returns false if the package is not pending. */
	bool GetPendingView(const FString& PackageName, FPendingView& OutView) const;

	/**
	 * Sets the given keys to their JSON values (one undo step). Keys that are not given keep the asset's value, and saving the asset
	 * writes them to the JSON. Resolves the pending state. bSave: then saves the asset if it is dirty (which writes the JSON); if it
	 * is not, the JSON is written without a save.
	 */
	bool ApplyJson(const FString& PackageName, const TArray<FString>& Keys, bool bSave = false);

	/** Writes the asset's current values to its JSON file, discarding the JSON's changes. Resolves the pending state. */
	bool OverwriteJson(const FString& PackageName);

	/** Broadcast when the pending list changes. */
	FSimpleMulticastDelegate OnPendingChanged;

	/** How a toast looks and at which verbosity it is logged. */
	enum class ENotice : uint8
	{
		/** Something was done (check mark, 6 s, logged as Display). */
		Success,
		/** Something was not done or needs your attention, but nothing failed (warning icon, 8 s, logged as Warning). */
		Warning,
		/** Something failed (red cross, 10 s, logged as Error). */
		Error,
	};

	/** Shows a toast and logs it. */
	static void Notify(const FText& Text, ENotice Level);

	/** Opens the JSON Sync tab. */
	static void OpenSyncTab();

private:
	struct FPending
	{
		JsonBlueprintSync::EAssetKind Kind = JsonBlueprintSync::EAssetKind::None;
		/** Unset if the file cannot be parsed. */
		TOptional<JsonBlueprintSync::FAssetDefaults> File;
		FString Error;
		/** The asset as it was when the JSON change was detected. Unset if unknown. */
		TOptional<JsonBlueprintSync::FAssetDefaults> Base;
		/** Hash of the file that was compared, i.e. the one the user reviews. */
		FString FileHash;
	};

	/** An asset whose pending state was resolved (Apply JSON, Two Way, Overwrite JSON) but that has not been saved since. */
	struct FResolved
	{
		JsonBlueprintSync::EAssetKind Kind = JsonBlueprintSync::EAssetKind::None;
		/** Hash of the JSON as reviewed. If the file has another hash at the next save or sync, it changed after the review. */
		FString FileHash;
		/** The JSON as reviewed: the base if the file changes again before the save. Unset if unknown. */
		TOptional<JsonBlueprintSync::FAssetDefaults> ReviewedFile;
		/** The pending state before Apply JSON, restored if the apply is undone. Unset for Overwrite JSON. */
		TOptional<FPending> Before;
		/** The Apply JSON transaction that changed this asset. Invalid if it changed nothing in this asset, and for Overwrite JSON. */
		FGuid TransactionId;
	};

	/**
	 * Compares a JSON file with its asset and records it as matching or pending. Returns true if it became pending.
	 * bAssetChanged: the package's saved hash differs from the one recorded at the last sync.
	 */
	bool Detect(const FString& PackageName, const FString& Text, const FString& FileHash, JsonBlueprintSync::EAssetKind Kind, bool bAssetChanged,
		TArray<FString>& OutErrors);

	/** Whether the JSON file on disk is the one recorded at the last sync, i.e. it has no changes of its own. */
	bool IsJsonUnchanged(const FString& PackageName) const;

	/** Which success toast an apply shows. Failures are always shown. */
	enum class EApplyNotice : uint8
	{
		/** None: the caller shows its own. */
		None,
		/** The user pressed Apply: a toast that fades out. */
		Manual,
		/** Two Way applied it on its own: a toast that stays until dismissed. */
		Automatic,
	};

	/** Applies the keys to each asset (Blueprints first, parents before children) in one transaction. Returns the resolved packages. */
	TArray<FString> ApplyBatch(const TArray<TPair<FString, TArray<FString>>>& Requests, EApplyNotice Notice);

	/** The success toast of an apply: the changed keys of each asset (asset name, keys), and an Undo button if TransactionId is valid. */
	void NotifyApplied(const TArray<TPair<FString, TArray<FString>>>& ChangedKeys, const FGuid& TransactionId, EApplyNotice Notice);

	/** The toast's Undo button: undoes the apply if it is still the last transaction, and opens the JSON Sync tab. */
	void UndoApply(const FGuid& TransactionId);

	/** Clears the pending state after the user (or Two Way) picked the values. TransactionId: the apply's, if it changed the asset. */
	void Resolve(const FString& PackageName, UObject* Asset, const FGuid& TransactionId);

	/**
	 * The JSON of a resolved asset changed after the review (Text is the new file). Makes it pending again, with the reviewed file
	 * as the base: keys the new file changed are JSON changes, the values the user kept or edited since are asset changes.
	 */
	void MarkChangedAfterReview(const FString& PackageName, const FString& Text);

	/** Writes the asset's JSON file and, if the asset is saved, records the sync. Returns false if the file cannot be written. */
	bool WriteJson(UObject* Asset);

	/** Records the asset as pending, with State.Base as its stored base (none if unset). */
	void MarkPending(const FString& PackageName, FPending&& State);
	void ShowPendingNotification();
	void NotifySaveSkipped(const FString& PackageName, bool bChangedAfterReview = false);

	/** The synced kind of the asset, or None if its kind is not synced or its mode is None. */
	JsonBlueprintSync::EAssetKind GetSyncedKind(const FAssetData& Asset) const;
	JsonBlueprintSync::EAssetKind GetSyncedKind(const UObject* Asset) const;

	void OnFilesLoaded();
	void OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext Context);
	void OnAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath);
	void OnAssetRemoved(const FAssetData& Asset);
	bool ProcessRemovals(float DeltaTime);

	FString GetJsonRoot() const;
	FString GetJsonPath(const FString& PackageName) const;
	/** Saves the cache. The first failure shows a toast; the next one only after a save has worked again. */
	void SaveCache() const;

	/** Deletes a JSON file. Shows a toast and returns false if it cannot be deleted (e.g. it is read-only). */
	static bool DeleteJsonFile(const FString& Path);

	JsonBlueprintSync::FSyncCache Cache;

	/** The cache could not be read at start-up. Reported by the first sync, once the editor can show toasts. */
	bool bCacheLoadFailed = false;

	/** The last cache save failed, and the toast for it was shown. */
	mutable bool bCacheSaveFailed = false;

	/** Assets whose JSON has changes waiting for the user. Mirrors the pending entries of the cache. */
	TMap<FString, FPending> Pending;

	/**
	 * Assets resolved in this session but not saved since. Their next save writes the JSON even though the file differs from the last
	 * sync, as long as it is still the reviewed file.
	 */
	TMap<FString, FResolved> Resolved;

	/**
	 * Assets whose Apply JSON was undone, so that a redo can resolve them again. A Two Way asset in here is not applied again
	 * automatically while its file is the one that was undone.
	 */
	TMap<FString, FResolved> Undone;

	/** The transaction being undone or redone, from MatchesContext. */
	mutable FGuid UndoRedoTransactionId;

	/** Packages reported removed; checked on the next tick, once the delete or rename has finished. */
	TSet<FString> PendingRemovals;
	FTSTicker::FDelegateHandle RemovalTicker;

	TWeakPtr<SNotificationItem> PendingNotification;
	TWeakPtr<SNotificationItem> AppliedNotification;

	FDelegateHandle FilesLoadedHandle;
	FDelegateHandle PackageSavedHandle;
	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle AssetRemovedHandle;
	IConsoleObject* SyncCommand = nullptr;
};
