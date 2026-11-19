#pragma once

#include "JsonBlueprintSync.h"
#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
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
 *
 * Not created in commandlets (use -run=JsonBlueprintSync there). Console command JsonBlueprintSync.Sync reruns the check,
 * e.g. after a git pull with the editor open.
 */
UCLASS()
class UJsonBlueprintSyncSubsystem : public UEditorSubsystem
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

	/** Finds JSON files that changed since the last sync and differ from their asset; applies Two Way ones that do not conflict. */
	void SyncFromJson();

	/** Pending package names, sorted. */
	TArray<FString> GetPendingPackages() const;

	/** Compares the pending asset with its JSON now. Returns false if the package is not pending. */
	bool GetPendingView(const FString& PackageName, FPendingView& OutView) const;

	/**
	 * Sets the given keys to their JSON values (one undo step; not saved). Keys that are not given keep the asset's value, and saving
	 * the asset writes them to the JSON. Resolves the pending state.
	 */
	bool ApplyJson(const FString& PackageName, const TArray<FString>& Keys);

	/** Writes the asset's current values to its JSON file, discarding the JSON's changes. Resolves the pending state. */
	bool OverwriteJson(const FString& PackageName);

	/** Broadcast when the pending list changes. */
	FSimpleMulticastDelegate OnPendingChanged;

	/** Shows a toast and logs it. */
	static void Notify(const FText& Text, bool bError);

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
	};

	/**
	 * Compares a JSON file with its asset and records it as matching or pending. Returns true if it became pending.
	 * bAssetChanged: the package's saved hash differs from the one recorded at the last sync.
	 */
	bool Detect(const FString& PackageName, const FString& Text, const FString& FileHash, JsonBlueprintSync::EAssetKind Kind, bool bAssetChanged);

	/** Whether the JSON file on disk is the one recorded at the last sync, i.e. it has no changes of its own. */
	bool IsJsonUnchanged(const FString& PackageName) const;

	/** Applies the keys to each asset (Blueprints first, parents before children) in one transaction. Returns the resolved packages. */
	TArray<FString> ApplyBatch(const TArray<TPair<FString, TArray<FString>>>& Requests, bool bNotify);

	/** Clears the pending state after the user (or Two Way) picked the values. */
	void Resolve(const FString& PackageName, UObject* Asset);

	/** Writes the asset's JSON file and, if the asset is saved, records the sync. Returns false if the file cannot be written. */
	bool WriteJson(UObject* Asset);

	void MarkPending(const FString& PackageName, FPending&& State);
	void ShowPendingNotification();
	void NotifySaveSkipped(const FString& PackageName);

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
	void SaveCache() const;

	JsonBlueprintSync::FSyncCache Cache;

	/** Assets whose JSON has changes waiting for the user. Mirrors the pending entries of the cache. */
	TMap<FString, FPending> Pending;

	/** Assets resolved in this session but not saved since. Their next save writes the JSON even though the file changed. */
	TSet<FString> Resolved;

	/** Packages reported removed; checked on the next tick, once the delete or rename has finished. */
	TSet<FString> PendingRemovals;
	FTSTicker::FDelegateHandle RemovalTicker;

	TWeakPtr<SNotificationItem> PendingNotification;

	FDelegateHandle FilesLoadedHandle;
	FDelegateHandle PackageSavedHandle;
	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle AssetRemovedHandle;
	IConsoleObject* SyncCommand = nullptr;
};
