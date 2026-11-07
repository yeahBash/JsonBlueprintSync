#pragma once

#include "JsonBlueprintSync.h"
#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "SyncState.h"

#include "JsonBlueprintSyncSubsystem.generated.h"

class SNotificationItem;
class UBlueprint;
class UPackage;
class FObjectPostSaveContext;
struct FAssetData;

/**
 * Keeps the Blueprint defaults JSON files in sync while the editor runs (see docs/plans/json-blueprint-sync.md):
 * - On save of a Blueprint under /Game, exports its defaults (the asset wins).
 * - When the asset registry has finished loading, imports JSON files that changed since the last sync (the JSON wins),
 *   unless the Blueprint changed too, in which case a notification lets the user pick. Imported Blueprints are left dirty, never saved.
 * - Moves the JSON file when a Blueprint is renamed and deletes it when the Blueprint is deleted.
 *
 * Not created in commandlets (use -run=JsonBlueprintSync there) or when [JsonBlueprintSync] bAutoSync=False in the editor ini.
 * Console command JsonBlueprintSync.Sync reruns the import check, e.g. after a git pull with the editor open.
 */
UCLASS()
class UJsonBlueprintSyncSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Imports JSON files that changed since the last sync; reports matches, imports and conflicts. */
	void SyncFromJson();

	/** Writes the Blueprint's JSON file and records the sync. Returns false if the file cannot be written. */
	bool ExportBlueprint(UBlueprint* Blueprint);

	/** Shows a toast and logs it. */
	static void Notify(const FText& Text, bool bError);

private:
	struct FPendingImport
	{
		TWeakObjectPtr<UBlueprint> Blueprint;
		JsonBlueprintSync::FBlueprintDefaults Doc;
	};

	/** Applies all documents in one transaction (parents first), then compiles the changed Blueprints. Never saves. */
	void ImportDocuments(TArray<FPendingImport> Imports);

	void ShowConflict(UBlueprint* Blueprint, const TArray<FString>& Keys);
	void ResolveConflict(const FString& PackageName, bool bUseJson);

	void OnFilesLoaded();
	void OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext Context);
	void OnAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath);
	void OnAssetRemoved(const FAssetData& Asset);
	bool ProcessRemovals(float DeltaTime);

	FString GetJsonPath(const FString& PackageName) const;
	void SaveCache() const;

	FString JsonRoot;
	JsonBlueprintSync::FSyncCache Cache;

	/** Blueprint packages reported removed; checked on the next tick, once the delete or rename has finished. */
	TSet<FString> PendingRemovals;
	FTSTicker::FDelegateHandle RemovalTicker;

	/** Packages with a conflict notification on screen. */
	TMap<FString, TWeakPtr<SNotificationItem>> OpenConflicts;

	FDelegateHandle FilesLoadedHandle;
	FDelegateHandle PackageSavedHandle;
	FDelegateHandle AssetRenamedHandle;
	FDelegateHandle AssetRemovedHandle;
	IConsoleObject* SyncCommand = nullptr;
};
