#include "JsonDataAssetSyncSubsystem.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "JsonDataAssetSyncModule.h"
#include "JsonDataAssetSyncSettings.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "FileHelpers.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IO/IoHash.h"
#include "Misc/FileHelper.h"
#include "Misc/ITransaction.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(JsonDataAssetSyncSubsystem)

#define LOCTEXT_NAMESPACE "JsonDataAssetSync"

using namespace JsonDataAssetSync;

namespace
{
	/** A data asset in a synced folder. */
	bool IsSynced(const FAssetData& Asset)
	{
		return IsSyncedPackage(Asset.PackageName.ToString()) && IsDataAsset(Asset);
	}

	bool IsSynced(const UObject* Asset)
	{
		return Asset && IsSyncedPackage(Asset->GetPackage()->GetName()) && IsDataAsset(Asset);
	}

	bool IsDirty(const UObject* Asset)
	{
		return Asset->GetPackage()->IsDirty();
	}

	FString GetAssetName(const FString& PackageName)
	{
		return FPackageName::GetShortName(PackageName);
	}

	/** Reads the JSON file and returns its hash, or an empty string if it does not exist or cannot be read. */
	FString ReadJsonHash(const FString& Path, FString& OutText)
	{
		OutText.Reset();
		return FPaths::FileExists(Path) && FFileHelper::LoadFileToString(OutText, *Path) ? HashText(OutText) : FString();
	}

	/** At most Max items, then "and N more". */
	FString JoinShort(const TArray<FString>& Items, int32 Max = 8)
	{
		if (Items.Num() <= Max)
		{
			return FString::Join(Items, TEXT(", "));
		}
		const TArray<FString> Head(Items.GetData(), Max);
		return FString::Printf(TEXT("%s and %d more"), *FString::Join(Head, TEXT(", ")), Items.Num() - Max);
	}

	void CloseNotification(const TWeakPtr<SNotificationItem>& Notification)
	{
		if (const TSharedPtr<SNotificationItem> Item = Notification.Pin())
		{
			Item->SetCompletionState(SNotificationItem::CS_None);
			Item->ExpireAndFadeout();
		}
	}

	/** The registry entry of the package's main asset. */
	TOptional<FAssetData> FindMainAsset(const FString& PackageName)
	{
		TArray<FAssetData> Assets;
		IAssetRegistry::GetChecked().GetAssetsByPackageName(FName(*PackageName), Assets);
		const FName ShortName(*FPackageName::GetShortName(PackageName));
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName == ShortName)
			{
				return Asset;
			}
		}
		return {};
	}

	/**
	 * The package's saved hash: of the loaded package if it is in memory (that is what Detect compares), else from the asset
	 * registry, which reads it from the file without loading it. Empty if unknown, e.g. never saved.
	 */
	FString GetSavedHash(const FString& PackageName)
	{
		FIoHash Hash;
		if (const UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Hash = Package->GetSavedHash();
		}
		if (Hash.IsZero())
		{
			if (const TOptional<FAssetPackageData> Data = IAssetRegistry::GetChecked().GetAssetPackageDataCopy(FName(*PackageName)))
			{
				Hash = Data->GetPackageSavedHash();
			}
		}
		return Hash.IsZero() ? FString() : LexToString(Hash);
	}

	/** The JSON names a class and it is not the asset's. An empty parent field in the JSON makes no claim. */
	bool ParentDiffers(const FAssetDefaults& File, const FAssetDefaults& Current)
	{
		return !File.Parent.IsEmpty() && !File.Parent.Equals(Current.Parent, ESearchCase::CaseSensitive);
	}
}

bool UJsonDataAssetSyncSubsystem::FPendingView::IsJsonOutOfDate() const
{
	return Error.IsEmpty() && bHasBase && Keys.Num() > 0
		&& !Keys.ContainsByPredicate([](const FKeyDiff& Diff) { return Diff.Change != EKeyChange::Asset; });
}

bool UJsonDataAssetSyncSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return !IsRunningCommandlet() && Super::ShouldCreateSubsystem(Outer);
}

void UJsonDataAssetSyncSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (!Cache.Load(FSyncCache::GetDefaultDir()))
	{
		// Too early for a toast; the first sync shows one.
		UE_LOG(LogJsonDataAssetSync, Warning, TEXT("Cannot read the sync cache in %s; starting with an empty one"), *FSyncCache::GetDefaultDir());
		bCacheLoadFailed = true;
	}

	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddUObject(this, &UJsonDataAssetSyncSubsystem::OnPackageSaved);

	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	AssetRenamedHandle = Registry.OnAssetRenamed().AddUObject(this, &UJsonDataAssetSyncSubsystem::OnAssetRenamed);
	AssetRemovedHandle = Registry.OnAssetRemoved().AddUObject(this, &UJsonDataAssetSyncSubsystem::OnAssetRemoved);
	if (Registry.IsLoadingAssets())
	{
		FilesLoadedHandle = Registry.OnFilesLoaded().AddUObject(this, &UJsonDataAssetSyncSubsystem::OnFilesLoaded);
	}
	else
	{
		// Wait one tick, so that the editor has finished initializing.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float)
		{
			OnFilesLoaded();
			return false;
		}));
	}

	if (GEditor)
	{
		GEditor->RegisterForUndo(this);
	}

	SyncCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("JsonDataAssetSync.Sync"),
		TEXT("Checks for JSON files that changed on disk since the last sync (e.g. after a git pull) and lists them in the JSON Sync tab."),
		FConsoleCommandDelegate::CreateUObject(this, &UJsonDataAssetSyncSubsystem::SyncFromJson));

	UE_LOG(LogJsonDataAssetSync, Log, TEXT("Syncing data assets under %s with %s"), *FString::Join(GetSyncedRoots(), TEXT(", ")), *GetJsonRoot());
}

void UJsonDataAssetSyncSubsystem::Deinitialize()
{
	UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
	if (IAssetRegistry* Registry = IAssetRegistry::Get())
	{
		Registry->OnFilesLoaded().Remove(FilesLoadedHandle);
		Registry->OnAssetRenamed().Remove(AssetRenamedHandle);
		Registry->OnAssetRemoved().Remove(AssetRemovedHandle);
	}
	FTSTicker::GetCoreTicker().RemoveTicker(RemovalTicker);
	if (SyncCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(SyncCommand);
		SyncCommand = nullptr;
	}
	if (GEditor)
	{
		GEditor->UnregisterForUndo(this);
	}
	CloseNotification(PendingNotification);
	Super::Deinitialize();
}

bool UJsonDataAssetSyncSubsystem::MatchesContext(const FTransactionContext& InContext, const TArray<TPair<UObject*, FTransactionObjectEvent>>& TransactionObjectContexts) const
{
	// Called just before PostUndo/PostRedo, with the transaction being undone or redone.
	UndoRedoTransactionId = InContext.TransactionId;
	return true;
}

void UJsonDataAssetSyncSubsystem::PostUndo(bool bSuccess)
{
	const FGuid TransactionId = UndoRedoTransactionId;
	UndoRedoTransactionId.Invalidate();
	if (!bSuccess || !TransactionId.IsValid())
	{
		return;
	}
	TArray<FString> Packages;
	for (const TPair<FString, FResolved>& Pair : Resolved)
	{
		if (Pair.Value.TransactionId == TransactionId && Pair.Value.Before)
		{
			Packages.Add(Pair.Key);
		}
	}
	if (Packages.Num() == 0)
	{
		return;
	}
	// The asset is back where it was before the apply, so its pending state is too. Without this, the next save would write the
	// asset's old values over the JSON change the user meant to take.
	TArray<FString> Names;
	for (const FString& PackageName : Packages)
	{
		FResolved Res = Resolved.FindAndRemoveChecked(PackageName);
		FPending State = *Res.Before;
		MarkPending(PackageName, MoveTemp(State));
		Undone.Add(PackageName, MoveTemp(Res));
		Names.Add(GetAssetName(PackageName));
	}
	Notify(FText::Format(LOCTEXT("ApplyUndone", "JSON Sync: undo reverted the JSON applied to {0}. It is back in the JSON Sync tab; saving it does not write its JSON."),
		FText::FromString(JoinShort(Names))), ENotice::Warning);
	ShowPendingNotification();
	OnPendingChanged.Broadcast();
}

void UJsonDataAssetSyncSubsystem::PostRedo(bool bSuccess)
{
	const FGuid TransactionId = UndoRedoTransactionId;
	UndoRedoTransactionId.Invalidate();
	if (!bSuccess || !TransactionId.IsValid())
	{
		return;
	}
	bool bChanged = false;
	for (auto It = Undone.CreateIterator(); It; ++It)
	{
		if (It->Value.TransactionId != TransactionId)
		{
			continue;
		}
		const FPending* State = Pending.Find(It->Key);
		if (State && State->FileHash == It->Value.FileHash)
		{
			// The apply is back, and the JSON is still the one it applied.
			Pending.Remove(It->Key);
			Cache.DeleteBase(It->Key);
			if (const FSyncEntry* Entry = Cache.Find(It->Key))
			{
				FSyncEntry NotPending = *Entry;
				NotPending.bPending = false;
				Cache.Set(It->Key, NotPending);
			}
			UE_LOG(LogJsonDataAssetSync, Display, TEXT("%s: redo applied the JSON again"), *It->Key);
			Resolved.Add(It->Key, MoveTemp(It->Value));
			bChanged = true;
		}
		It.RemoveCurrent();
	}
	if (bChanged)
	{
		SaveCache();
		ShowPendingNotification();
		OnPendingChanged.Broadcast();
	}
}

void UJsonDataAssetSyncSubsystem::OnFilesLoaded()
{
	if (IAssetRegistry* Registry = IAssetRegistry::Get())
	{
		Registry->OnFilesLoaded().Remove(FilesLoadedHandle);
	}
	FilesLoadedHandle.Reset();
	if (bCacheLoadFailed)
	{
		bCacheLoadFailed = false;
		Notify(FText::Format(LOCTEXT("CacheLoadFailed", "JSON Sync: cannot read {0}, so the last sync is unknown. Assets whose JSON differs are listed with every key as a conflict."),
			FText::FromString(FPaths::Combine(FSyncCache::GetDefaultDir(), TEXT("cache.json")))), ENotice::Error);
	}
	SyncFromJson();
}

FString UJsonDataAssetSyncSubsystem::GetJsonRoot() const
{
	// Read each time, so that a change in Project Settings takes effect without a restart.
	return JsonDataAssetSync::GetDefaultJsonRoot();
}

FString UJsonDataAssetSyncSubsystem::GetJsonPath(const FString& PackageName) const
{
	return JsonDataAssetSync::GetJsonPath(GetJsonRoot(), PackageName);
}

void UJsonDataAssetSyncSubsystem::SaveCache() const
{
	if (Cache.Save())
	{
		bCacheSaveFailed = false;
		return;
	}
	const FString Path = FPaths::Combine(FSyncCache::GetDefaultDir(), TEXT("cache.json"));
	if (bCacheSaveFailed)
	{
		// Already reported; a toast on every save would be noise.
		UE_LOG(LogJsonDataAssetSync, Warning, TEXT("Cannot write %s"), *Path);
		return;
	}
	bCacheSaveFailed = true;
	Notify(FText::Format(LOCTEXT("CacheSaveFailed", "JSON Sync: cannot write {0}. Is it read-only? What was synced in this session will be compared again at the next start."),
		FText::FromString(Path)), ENotice::Error);
}

bool UJsonDataAssetSyncSubsystem::DeleteJsonFile(const FString& Path)
{
	if (!FPaths::FileExists(Path))
	{
		return true;
	}
	if (!IFileManager::Get().Delete(*Path, false, true, true))
	{
		Notify(FText::Format(LOCTEXT("DeleteFailed", "JSON Sync: cannot delete {0}. Is it open in another program? Delete it by hand."),
			FText::FromString(Path)), ENotice::Error);
		return false;
	}
	UE_LOG(LogJsonDataAssetSync, Log, TEXT("Deleted %s"), *Path);
	return true;
}

void UJsonDataAssetSyncSubsystem::Notify(const FText& Text, ENotice Level)
{
	FNotificationInfo Info(Text);
	switch (Level)
	{
	case ENotice::Success:
		UE_LOG(LogJsonDataAssetSync, Display, TEXT("%s"), *Text.ToString());
		Info.ExpireDuration = 6.0f;
		Info.bUseSuccessFailIcons = true;
		break;
	case ENotice::Warning:
		UE_LOG(LogJsonDataAssetSync, Warning, TEXT("%s"), *Text.ToString());
		Info.ExpireDuration = 8.0f;
		Info.Image = FAppStyle::GetBrush(TEXT("Icons.WarningWithColor"));
		break;
	case ENotice::Error:
		UE_LOG(LogJsonDataAssetSync, Error, TEXT("%s"), *Text.ToString());
		Info.ExpireDuration = 10.0f;
		Info.bUseSuccessFailIcons = true;
		break;
	}
	if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		if (Level != ENotice::Warning)
		{
			Item->SetCompletionState(Level == ENotice::Error ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
		}
	}
}

void UJsonDataAssetSyncSubsystem::OpenSyncTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(JsonDataAssetSync::GetSyncTabName()));
}

void UJsonDataAssetSyncSubsystem::SyncFromJson()
{
	if (GEditor && GEditor->PlayWorld)
	{
		Notify(LOCTEXT("SyncDuringPie", "JSON Sync: JSON files are not checked during Play In Editor. Press Refresh in the JSON Sync tab when it has ended."),
			ENotice::Warning);
		return;
	}

	const UJsonDataAssetSyncSettings* Settings = GetDefault<UJsonDataAssetSyncSettings>();
	const FString JsonRoot = GetJsonRoot();
	TArray<FString> Errors;
	const bool bAutomatic = Settings->Mode == EJsonSyncMode::Automatic;
	TArray<FString> ToAutoApply;
	TSet<FString> Seen;
	int32 Checked = 0;
	TArray<FString> Files;
	for (const FString& Root : GetSyncedRoots())
	{
		Files.Append(FindJsonFiles(JsonRoot, Root));
	}
	for (const FString& File : Files)
	{
		FString PackageName;
		if (!JsonDataAssetSync::GetPackageName(JsonRoot, File, PackageName))
		{
			// Reported by ReportMissingPairs.
			continue;
		}
		if (!IsSyncedPackage(PackageName))
		{
			// In an excluded folder: not synced at all.
			continue;
		}
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			Errors.Add(FString::Printf(TEXT("%s: cannot read the file"), *File));
			continue;
		}
		Seen.Add(PackageName);
		const FString FileHash = HashText(Text);
		const FSyncEntry* Entry = Cache.Find(PackageName);
		const FString SavedHash = GetSavedHash(PackageName);
		const bool bAssetChanged = Entry && !Entry->AssetHash.IsEmpty() && !SavedHash.IsEmpty() && Entry->AssetHash != SavedHash;
		const FResolved* Res = Resolved.Find(PackageName);
		const bool bResolved = Res != nullptr;
		if (Res && Res->FileHash == FileHash)
		{
			// Reviewed and not saved yet: the next save writes the JSON.
			continue;
		}
		if (!bResolved && Entry && !Entry->bPending && Entry->JsonHash == FileHash && !bAssetChanged)
		{
			// Neither side changed since the last sync: skip without loading the asset.
			if (Entry->AssetHash.IsEmpty() && !SavedHash.IsEmpty())
			{
				// Recorded by an older plugin version: take the asset as it is now as the synced one, as that version did.
				FSyncEntry Adopted = *Entry;
				Adopted.AssetHash = SavedHash;
				Cache.Set(PackageName, Adopted);
			}
			continue;
		}

		// A JSON file without its data asset is reported by ReportMissingPairs.
		const TOptional<FAssetData> Asset = FindMainAsset(PackageName);
		if (!Asset || !IsDataAsset(*Asset))
		{
			if (Pending.Remove(PackageName) > 0 || Entry)
			{
				Cache.Remove(PackageName);
			}
			Resolved.Remove(PackageName);
			Undone.Remove(PackageName);
			continue;
		}
		++Checked;
		bool bPending = false;
		if (bResolved)
		{
			// Reviewed, then the JSON changed again before the save (e.g. a git pull in between).
			MarkChangedAfterReview(PackageName, Text);
			bPending = true;
		}
		else
		{
			bPending = Detect(PackageName, Text, FileHash, bAssetChanged, Errors);
		}
		if (!bPending)
		{
			Undone.Remove(PackageName);
		}
		else if (bAutomatic)
		{
			// An undone Apply JSON is still in the undo history (as a redo step, or under the edits made since), so the auto-apply
			// below leaves such an asset for the user.
			ToAutoApply.Add(PackageName);
		}
	}

	// Pending assets whose JSON file is gone have nothing left to review.
	for (const FString& PackageName : Cache.GetPendingPackages())
	{
		if (!Seen.Contains(PackageName))
		{
			Cache.Remove(PackageName);
			Pending.Remove(PackageName);
		}
	}
	SaveCache();
	UE_LOG(LogJsonDataAssetSync, Log, TEXT("Checked %d changed JSON files, %d differ from their asset"), Checked, Pending.Num());

	for (const FString& Error : Errors)
	{
		UE_LOG(LogJsonDataAssetSync, Error, TEXT("%s"), *Error);
	}
	if (Errors.Num() > 0)
	{
		Notify(FText::Format(LOCTEXT("SyncErrors", "JSON Sync: {0} JSON files could not be checked. See the Output Log (LogJsonDataAssetSync)."),
			FText::AsNumber(Errors.Num())), ENotice::Error);
	}
	ReportMissingPairs();

	// Automatic: apply what only the JSON changed. Conflicts, a different class and a JSON that is only out of date stay in the
	// list for the user.
	TArray<TPair<FString, TArray<FString>>> Requests;
	for (const FString& PackageName : ToAutoApply)
	{
		FPendingView View;
		if (!GetPendingView(PackageName, View) || !View.Error.IsEmpty() || !CanAutoApply(View.bHasBase, View.ParentDiffers(), View.Keys))
		{
			continue;
		}
		const UPackage* Package = FindPackage(nullptr, *PackageName);
		if (Package && Package->IsDirty())
		{
			// The automatic apply saves the asset, which would save the user's unsaved edits with it. Usually such an asset has no base
			// and is not applied anyway, but one that was already pending keeps its stored base.
			UE_LOG(LogJsonDataAssetSync, Display, TEXT("%s: not applied automatically, because it has unsaved changes; review it in the JSON Sync tab"),
				*PackageName);
			continue;
		}
		if (IsInUndoHistory(PackageName))
		{
			// The automatic apply is not undoable, which is safe only if no undo step holds the asset. Rare: at start-up the history is
			// empty, so this happens only for a Refresh after the asset was edited in this session.
			UE_LOG(LogJsonDataAssetSync, Display, TEXT("%s: not applied automatically, because the undo history has changes to it; review it in the JSON Sync tab"),
				*PackageName);
			continue;
		}
		TArray<FString> Keys;
		for (const FKeyDiff& Diff : View.Keys)
		{
			if (Diff.Change == EKeyChange::Json)
			{
				Keys.Add(Diff.Key);
			}
		}
		Requests.Add({ PackageName, MoveTemp(Keys) });
	}
	if (Requests.Num() > 0)
	{
		ApplyBatch(Requests, true, true);
	}

	ShowPendingNotification();
	OnPendingChanged.Broadcast();
}

bool UJsonDataAssetSyncSubsystem::Detect(const FString& PackageName, const FString& Text, const FString& FileHash, bool bAssetChanged,
	TArray<FString>& OutErrors)
{
	const FSyncEntry* Entry = Cache.Find(PackageName);
	const TOptional<FSyncEntry> OldEntry = Entry ? TOptional<FSyncEntry>(*Entry) : TOptional<FSyncEntry>();

	FPending State;
	State.FileHash = FileHash;
	FAssetDefaults FileDoc;
	if (FromJson(Text, FileDoc, State.Error))
	{
		State.File = MoveTemp(FileDoc);
	}

	UObject* Asset = FPackageName::DoesPackageExist(PackageName) ? LoadAsset(PackageName) : nullptr;
	if (!Asset)
	{
		// Reported with the other files that could not be checked. Nothing is recorded, so the next sync tries again.
		OutErrors.Add(FString::Printf(TEXT("%s: cannot load the asset"), *PackageName));
		return false;
	}
	const FAssetDefaults Current = Capture(Asset);
	const bool bParentDiffers = State.File && ParentDiffers(*State.File, Current);
	if (State.File && !bParentDiffers && ValuesEqual(*State.File, Current))
	{
		// They match, e.g. a teammate committed both, or only values the JSON does not hold changed. Record saved state only: recording an
		// unsaved match would hide the difference after an editor restart.
		Pending.Remove(PackageName);
		if (!IsDirty(Asset))
		{
			Cache.DeleteBase(PackageName);
			Cache.Set(PackageName, { FileHash, GetSavedHash(PackageName), false });
		}
		else if (OldEntry && OldEntry->bPending)
		{
			Cache.DeleteBase(PackageName);
			Cache.Set(PackageName, { OldEntry->JsonHash, OldEntry->AssetHash, false });
		}
		return false;
	}

	// They differ. The base (the values both sides had at the last sync) tells which side changed each key.
	FBaseInputs Inputs;
	Inputs.bPending = OldEntry && OldEntry->bPending;
	Inputs.bHasEntry = OldEntry.IsSet();
	Inputs.bJsonChanged = !OldEntry || OldEntry->JsonHash != FileHash;
	Inputs.bAssetChanged = bAssetChanged;
	Inputs.bAssetDirty = IsDirty(Asset);
	Inputs.bParentDiffers = bParentDiffers;
	switch (ChooseBase(Inputs))
	{
	case EBaseSource::Stored:
	{
		FAssetDefaults Base;
		if (Cache.LoadBase(PackageName, Base))
		{
			State.Base = MoveTemp(Base);
		}
		break;
	}
	case EBaseSource::File:
		// The JSON is the one last synced, so the asset changed every key that differs: the JSON is out of date.
		if (State.File)
		{
			State.Base = *State.File;
			Cache.SaveBase(PackageName, *State.File);
		}
		break;
	case EBaseSource::Current:
		// The asset is the one last synced, so the JSON changed every key that differs.
		State.Base = Current;
		Cache.SaveBase(PackageName, Current);
		break;
	case EBaseSource::None:
		// Never synced on this machine, or both sides changed: unknown, every key is a conflict.
		break;
	}
	if (bParentDiffers)
	{
		UE_LOG(LogJsonDataAssetSync, Warning, TEXT("%s: the JSON was written for class %s, the asset's class is %s"), *PackageName,
			*State.File->Parent, *Current.Parent);
	}

	if (State.File)
	{
		UE_LOG(LogJsonDataAssetSync, Display, TEXT("%s differs from its JSON: %s"), *PackageName,
			*JoinShort([&] { TArray<FString> Keys; for (const FKeyDiff& Diff : DiffKeys(State.Base.GetPtrOrNull(), *State.File, Current)) { Keys.Add(Diff.Key); } return Keys; }()));
	}
	else
	{
		UE_LOG(LogJsonDataAssetSync, Warning, TEXT("%s: %s"), *GetJsonPath(PackageName), *State.Error);
	}
	// Keep the hashes of the last sync: they tell whether the file still has no changes of its own.
	Cache.Set(PackageName, { OldEntry ? OldEntry->JsonHash : FString(), OldEntry ? OldEntry->AssetHash : FString(), true });
	Pending.Add(PackageName, MoveTemp(State));
	Resolved.Remove(PackageName);
	return true;
}

TArray<FString> UJsonDataAssetSyncSubsystem::GetPendingPackages() const
{
	TArray<FString> Packages;
	Pending.GetKeys(Packages);
	Packages.Sort();
	return Packages;
}

bool UJsonDataAssetSyncSubsystem::GetPendingView(const FString& PackageName, FPendingView& OutView) const
{
	const FPending* State = Pending.Find(PackageName);
	if (!State)
	{
		return false;
	}
	OutView = FPendingView();
	OutView.PackageName = PackageName;
	OutView.bHasBase = State->Base.IsSet();
	if (!State->File)
	{
		OutView.Error = State->Error;
		return true;
	}
	UObject* Asset = FPackageName::DoesPackageExist(PackageName) ? LoadAsset(PackageName) : nullptr;
	if (!Asset)
	{
		OutView.Error = TEXT("cannot load the asset");
		return true;
	}
	const FAssetDefaults Current = Capture(Asset);
	OutView.Keys = DiffKeys(State->Base.GetPtrOrNull(), *State->File, Current);
	if (ParentDiffers(*State->File, Current))
	{
		OutView.JsonParent = State->File->Parent;
		OutView.AssetParent = Current.Parent;
	}
	return true;
}

bool UJsonDataAssetSyncSubsystem::ApplyJson(const FString& PackageName, const TArray<FString>& Keys, bool bSave)
{
	bool bResolved = ApplyBatch({ { PackageName, Keys } }, !bSave, false).Contains(PackageName);
	if (bResolved && bSave)
	{
		UObject* Asset = LoadAsset(PackageName);
		const FText AssetName = FText::FromString(GetAssetName(PackageName));
		if (Asset && IsDirty(Asset))
		{
			// Saving writes the JSON (OnPackageSaved, as for any resolved asset). Checks the file out first if source control needs it.
			UPackage* Package = Asset->GetPackage();
			const FEditorFileUtils::EPromptReturnCode Result = FEditorFileUtils::PromptForCheckoutAndSave({ Package }, true, false);
			if (Result == FEditorFileUtils::PR_Success && !Package->IsDirty())
			{
				Notify(FText::Format(LOCTEXT("AppliedSaved", "Applied the JSON to {0} and saved it."), AssetName), ENotice::Success);
			}
			else
			{
				bResolved = false;
				Notify(FText::Format(LOCTEXT("AppliedNotSaved", "Applied the JSON to {0}, but it was not saved. Save it to write its JSON; Ctrl+Z reverts the apply."), AssetName),
					ENotice::Warning);
			}
		}
		else
		{
			// Nothing in the asset changed: Resolve has already written the JSON, so there is nothing to save.
			Notify(FText::Format(LOCTEXT("AppliedNothingToSave", "Wrote the JSON of {0}; the asset did not change."), AssetName), ENotice::Success);
		}
	}
	OnPendingChanged.Broadcast();
	return bResolved;
}

bool UJsonDataAssetSyncSubsystem::IsInUndoHistory(const FString& PackageName)
{
	const UPackage* Package = FindPackage(nullptr, *PackageName);
	if (!Package || !GEditor || !GEditor->Trans)
	{
		// A package that is not loaded has no objects in any transaction.
		return false;
	}
	// The whole queue: the steps that can be undone and those that can be redone.
	for (int32 Index = 0; Index < GEditor->Trans->GetQueueLength(); ++Index)
	{
		const FTransaction* Transaction = GEditor->Trans->GetTransaction(Index);
		if (!Transaction)
		{
			continue;
		}
		TArray<UObject*> Objects;
		Transaction->GetTransactionObjects(Objects);
		for (const UObject* Object : Objects)
		{
			if (Object && Object->GetPackage() == Package)
			{
				return true;
			}
		}
	}
	return false;
}

TArray<FString> UJsonDataAssetSyncSubsystem::ApplyBatch(const TArray<TPair<FString, TArray<FString>>>& Requests, bool bNotify, bool bAutomatic)
{
	struct FItem
	{
		FString PackageName;
		UObject* Asset = nullptr;
		FAssetDefaults Doc;
	};
	TArray<FItem> Items;
	TArray<FString> Failed;
	for (const TPair<FString, TArray<FString>>& Request : Requests)
	{
		const FPending* State = Pending.Find(Request.Key);
		UObject* Asset = State && State->File && FPackageName::DoesPackageExist(Request.Key) ? LoadAsset(Request.Key) : nullptr;
		if (!Asset)
		{
			UE_LOG(LogJsonDataAssetSync, Error, TEXT("%s: not pending, or the JSON or the asset cannot be loaded"), *Request.Key);
			Failed.Add(GetAssetName(Request.Key));
			continue;
		}
		FItem& Item = Items.AddDefaulted_GetRef();
		Item.PackageName = Request.Key;
		Item.Asset = Asset;
		Item.Doc = TakeKeys(Capture(Asset), *State->File, Request.Value);
	}

	TArray<FString> Changed;
	TArray<FString> Succeeded;
	TSet<FString> ChangedPackages;
	FGuid TransactionId;
	{
		// One transaction, so a single Ctrl+Z reverts the whole apply.
		// An Automatic mode apply has no transaction: the JSON is the source of truth, and Ctrl+Z must not revert a change
		// that came with it (e.g. from a teammate). Modify() without a transaction only marks the package dirty.
		TUniquePtr<FScopedTransaction> Transaction;
		if (!bAutomatic)
		{
			Transaction = MakeUnique<FScopedTransaction>(LOCTEXT("ApplyTransaction", "Apply JSON"));
			if (GUndo)
			{
				// Undoing this transaction makes the assets it changed pending again (see PostUndo).
				TransactionId = GUndo->GetContext().TransactionId;
			}
		}
		for (const FItem& Item : Items)
		{
			const FApplyResult Result = Apply(Item.Asset, Item.Doc);
			for (const FString& Warning : Result.Warnings)
			{
				UE_LOG(LogJsonDataAssetSync, Warning, TEXT("%s: %s"), *Item.PackageName, *Warning);
			}
			for (const FString& Error : Result.Errors)
			{
				UE_LOG(LogJsonDataAssetSync, Error, TEXT("%s: %s"), *Item.PackageName, *Error);
			}
			if (Result.HasErrors())
			{
				Failed.Add(GetAssetName(Item.PackageName));
				continue;
			}
			if (Result.Changed.Num() > 0)
			{
				MarkModified(Item.Asset);
				Changed.Add(GetAssetName(Item.PackageName));
				ChangedPackages.Add(Item.PackageName);
				UE_LOG(LogJsonDataAssetSync, Display, TEXT("%s: applied %s"), *Item.PackageName, *FString::Join(Result.Changed, TEXT(", ")));
			}
			Succeeded.Add(Item.PackageName);
		}
		if (Transaction && Changed.Num() == 0)
		{
			Transaction->Cancel();
		}
	}
	for (const FItem& Item : Items)
	{
		if (Succeeded.Contains(Item.PackageName))
		{
			Resolve(Item.PackageName, Item.Asset, ChangedPackages.Contains(Item.PackageName) ? TransactionId : FGuid());
		}
	}
	SaveCache();

	if (bAutomatic)
	{
		// Save at once, so the .uasset matches the JSON it came from. Left unsaved, a Reload or a source control revert of the asset
		// would bring back its old values while it still counts as resolved, and the next save would write them over the JSON. Only
		// assets without unsaved changes are applied automatically (SyncFromJson skips dirty ones), so this saves nothing else.
		TArray<UPackage*> ToSave;
		for (const FItem& Item : Items)
		{
			if (ChangedPackages.Contains(Item.PackageName) && IsDirty(Item.Asset))
			{
				ToSave.Add(Item.Asset->GetPackage());
			}
		}
		if (ToSave.Num() > 0)
		{
			// Saving writes the JSON (OnPackageSaved, as for any resolved asset). Checks the files out first if source control needs it.
			FEditorFileUtils::PromptForCheckoutAndSave(ToSave, true, false);
		}
		TArray<FString> Saved;
		TArray<FString> NotSaved;
		for (const UPackage* Package : ToSave)
		{
			(Package->IsDirty() ? NotSaved : Saved).Add(GetAssetName(Package->GetName()));
		}
		if (bNotify && Saved.Num() > 0)
		{
			Notify(FText::Format(LOCTEXT("AppliedAutomatically", "Applied JSON to {0} automatically and saved it."),
				FText::FromString(JoinShort(Saved))), ENotice::Success);
		}
		if (NotSaved.Num() > 0)
		{
			Notify(FText::Format(LOCTEXT("AppliedAutomaticallyNotSaved", "Applied JSON to {0} automatically, but it was not saved. Save it to keep the change; Ctrl+Z does not revert it."),
				FText::FromString(JoinShort(NotSaved))), ENotice::Warning);
		}
	}
	else if (bNotify && Changed.Num() > 0)
	{
		Notify(FText::Format(LOCTEXT("Applied", "Applied JSON to {0}. Not saved; Ctrl+Z reverts."), FText::FromString(JoinShort(Changed))), ENotice::Success);
	}
	if (Failed.Num() > 0)
	{
		Notify(FText::Format(LOCTEXT("ApplyFailed", "Cannot apply the JSON of {0}; nothing was changed for them. See the Output Log (LogJsonDataAssetSync)."),
			FText::FromString(JoinShort(Failed))), ENotice::Error);
	}
	return Succeeded;
}

void UJsonDataAssetSyncSubsystem::Resolve(const FString& PackageName, UObject* Asset, const FGuid& TransactionId)
{
	TOptional<FPending> Before;
	if (FPending* State = Pending.Find(PackageName))
	{
		Before = MoveTemp(*State);
		Pending.Remove(PackageName);
	}
	Undone.Remove(PackageName);
	Cache.DeleteBase(PackageName);
	FSyncEntry Entry;
	if (const FSyncEntry* Old = Cache.Find(PackageName))
	{
		Entry = *Old;
	}
	Entry.bPending = false;
	Cache.Set(PackageName, Entry);

	if (IsDirty(Asset))
	{
		// The next save writes the JSON, including the keys where the user kept the asset's value, unless the JSON changes again.
		FResolved& Res = Resolved.Add(PackageName);
		if (Before && !Before->FileHash.IsEmpty())
		{
			Res.FileHash = Before->FileHash;
			Res.ReviewedFile = Before->File;
		}
		else
		{
			FString Text;
			Res.FileHash = ReadJsonHash(GetJsonPath(PackageName), Text);
			FAssetDefaults Doc;
			FString Error;
			if (FromJson(Text, Doc, Error))
			{
				Res.ReviewedFile = MoveTemp(Doc);
			}
		}
		Res.Before = MoveTemp(Before);
		Res.TransactionId = TransactionId;
	}
	else
	{
		// Nothing changed (the user kept the asset's values): write the JSON now, or no save would ever do it.
		WriteJson(Asset);
	}
}

void UJsonDataAssetSyncSubsystem::MarkChangedAfterReview(const FString& PackageName, const FString& Text)
{
	FResolved Res;
	Resolved.RemoveAndCopyValue(PackageName, Res);
	FPending State;
	State.FileHash = HashText(Text);
	FAssetDefaults FileDoc;
	if (FromJson(Text, FileDoc, State.Error))
	{
		State.File = MoveTemp(FileDoc);
	}
	// What the user reviewed is the base: keys the new file changed are JSON changes. The asset's differences from it (values the
	// user kept, or edited since) are asset changes, which keep the user's decision unless the new file changed the same key.
	State.Base = MoveTemp(Res.ReviewedFile);
	UE_LOG(LogJsonDataAssetSync, Warning, TEXT("%s: the JSON changed after it was reviewed"), *PackageName);
	MarkPending(PackageName, MoveTemp(State));
}

bool UJsonDataAssetSyncSubsystem::OverwriteJson(const FString& PackageName)
{
	UObject* Asset = FPackageName::DoesPackageExist(PackageName) ? LoadAsset(PackageName) : nullptr;
	if (!Asset)
	{
		Notify(FText::Format(LOCTEXT("OverwriteNoAsset", "Cannot load {0}"), FText::FromString(PackageName)), ENotice::Error);
		return false;
	}
	Pending.Remove(PackageName);
	Cache.DeleteBase(PackageName);
	FSyncEntry Entry;
	if (const FSyncEntry* Old = Cache.Find(PackageName))
	{
		Entry = *Old;
	}
	Entry.bPending = false;
	Cache.Set(PackageName, Entry);
	Undone.Remove(PackageName);
	const bool bWritten = WriteJson(Asset);
	if (IsDirty(Asset))
	{
		// The file on disk (the one just written, or the old one if the write failed) counts as reviewed: the next save writes the
		// JSON, unless the file changes again before that.
		FResolved& Res = Resolved.Add(PackageName);
		FString Text;
		Res.FileHash = ReadJsonHash(GetJsonPath(PackageName), Text);
		FAssetDefaults Doc;
		FString Error;
		if (FromJson(Text, Doc, Error))
		{
			Res.ReviewedFile = MoveTemp(Doc);
		}
	}
	SaveCache();
	OnPendingChanged.Broadcast();
	return bWritten;
}

bool UJsonDataAssetSyncSubsystem::WriteJson(UObject* Asset)
{
	const FString PackageName = Asset->GetPackage()->GetName();
	const FString Path = GetJsonPath(PackageName);
	const FAssetDefaults Doc = Capture(Asset);
	bool bWritten = false;
	if (!SaveFileIfChanged(Path, Doc, bWritten))
	{
		Notify(FText::Format(LOCTEXT("WriteFailed", "Cannot write {0}. Is it read-only?"), FText::FromString(Path)), ENotice::Error);
		return false;
	}
	if (bWritten)
	{
		UE_LOG(LogJsonDataAssetSync, Log, TEXT("Wrote %s"), *Path);
	}
	// Record saved state only (see Detect). The save event fires after the dirty flag is cleared.
	if (!IsDirty(Asset))
	{
		// The saved hash is already the new one when the save event fires.
		Cache.Set(PackageName, { HashText(ToJson(Doc)), GetSavedHash(PackageName), false });
		Resolved.Remove(PackageName);
		Undone.Remove(PackageName);
		SaveCache();
	}
	return true;
}

bool UJsonDataAssetSyncSubsystem::IsJsonUnchanged(const FString& PackageName) const
{
	const FSyncEntry* Entry = Cache.Find(PackageName);
	FString Text;
	return Entry && !Entry->JsonHash.IsEmpty() && FFileHelper::LoadFileToString(Text, *GetJsonPath(PackageName))
		&& Entry->JsonHash == HashText(Text);
}

void UJsonDataAssetSyncSubsystem::MarkPending(const FString& PackageName, FPending&& State)
{
	const FSyncEntry* Entry = Cache.Find(PackageName);
	Cache.Set(PackageName, { Entry ? Entry->JsonHash : FString(), Entry ? Entry->AssetHash : FString(), true });
	if (State.Base)
	{
		Cache.SaveBase(PackageName, *State.Base);
	}
	else
	{
		Cache.DeleteBase(PackageName);
	}
	Pending.Add(PackageName, MoveTemp(State));
	Resolved.Remove(PackageName);
	SaveCache();
}

void UJsonDataAssetSyncSubsystem::ShowPendingNotification()
{
	CloseNotification(PendingNotification);
	PendingNotification.Reset();
	if (Pending.Num() == 0)
	{
		return;
	}
	TArray<FString> Names;
	for (const FString& PackageName : GetPendingPackages())
	{
		Names.Add(GetAssetName(PackageName));
	}
	FNotificationInfo Info(FText::Format(LOCTEXT("PendingCount", "JSON Sync: {0} asset(s) differ from their JSON"),
		FText::AsNumber(Pending.Num())));
	Info.SubText = FText::FromString(JoinShort(Names));
	Info.bFireAndForget = false;
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("Review", "Review"),
		LOCTEXT("ReviewTip", "Open the JSON Sync tab to apply or discard the JSON changes"),
		FSimpleDelegate::CreateWeakLambda(this, [this]()
		{
			CloseNotification(PendingNotification);
			OpenSyncTab();
		}),
		SNotificationItem::CS_Pending));
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("Later", "Later"),
		LOCTEXT("LaterTip", "Close this notification. The assets stay in Window > JSON Sync."),
		FSimpleDelegate::CreateWeakLambda(this, [this]() { CloseNotification(PendingNotification); }),
		SNotificationItem::CS_Pending));
	if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(SNotificationItem::CS_Pending);
		PendingNotification = Item;
	}
}

void UJsonDataAssetSyncSubsystem::ReportMissingPairs()
{
	const FMissingPairs Missing = FindMissingPairs(GetJsonRoot());
	if (Missing.IsEmpty())
	{
		return;
	}
	for (const FString& File : Missing.JsonWithoutAsset)
	{
		UE_LOG(LogJsonDataAssetSync, Warning, TEXT("%s: no data asset for this JSON file. Delete the file if the asset was deleted on purpose, or restore the asset."),
			*File);
	}
	for (const FString& PackageName : Missing.AssetsWithoutJson)
	{
		UE_LOG(LogJsonDataAssetSync, Warning, TEXT("%s: the data asset has no JSON file. Save the asset or run -run=JsonDataAssetSync -export, or add its folder to Excluded Folders."),
			*PackageName);
	}

	TArray<FString> Lines;
	if (Missing.JsonWithoutAsset.Num() > 0)
	{
		Lines.Add(FString::Printf(TEXT("%d JSON files have no data asset"), Missing.JsonWithoutAsset.Num()));
	}
	if (Missing.AssetsWithoutJson.Num() > 0)
	{
		Lines.Add(FString::Printf(TEXT("%d data assets have no JSON file"), Missing.AssetsWithoutJson.Num()));
	}
	Notify(FText::Format(LOCTEXT("MissingPairs", "JSON Sync: {0}. See the Output Log (LogJsonDataAssetSync) for how to fix each one."),
		FText::FromString(FString::Join(Lines, TEXT(", ")))), ENotice::Warning);
}

void UJsonDataAssetSyncSubsystem::NotifySaveSkipped(const FString& PackageName, bool bChangedAfterReview)
{
	UE_LOG(LogJsonDataAssetSync, Warning, TEXT("Saved %s but did not write %s: the JSON has changes that are not reviewed"),
		*PackageName, *GetJsonPath(PackageName));

	const FText AssetName = FText::FromString(GetAssetName(PackageName));
	FNotificationInfo Info(bChangedAfterReview
		? FText::Format(LOCTEXT("SaveSkippedChanged", "Saved {0}, but did not write its JSON: the JSON changed again after you reviewed it"), AssetName)
		: FText::Format(LOCTEXT("SaveSkipped", "Saved {0}, but did not write its JSON: the JSON has changes you have not reviewed"), AssetName));
	Info.ExpireDuration = 10.0f;
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("Review", "Review"),
		LOCTEXT("ReviewTip", "Open the JSON Sync tab to apply or discard the JSON changes"),
		FSimpleDelegate::CreateStatic(&UJsonDataAssetSyncSubsystem::OpenSyncTab),
		SNotificationItem::CS_None));
	FSlateNotificationManager::Get().AddNotification(Info);
}

void UJsonDataAssetSyncSubsystem::OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext Context)
{
	if (!Package || Context.IsFromAutoSave() || Context.IsProceduralSave() || Context.IsCooking())
	{
		return;
	}
	const FString PackageName = Package->GetName();
	UObject* Asset = Package->FindAssetInPackage();
	if (!IsSynced(Asset))
	{
		return;
	}

	const FSyncEntry* Entry = Cache.Find(PackageName);
	if (Pending.Contains(PackageName) || (Entry && Entry->bPending))
	{
		if (IsJsonUnchanged(PackageName))
		{
			// The JSON has no changes of its own, it only missed the asset's (e.g. the .uasset was committed alone). Saving the
			// asset is the natural fix, so write it.
			OverwriteJson(PackageName);
			return;
		}
		NotifySaveSkipped(PackageName);
		return;
	}

	const FString Path = GetJsonPath(PackageName);
	FString Text;
	if (const FResolved* Res = Resolved.Find(PackageName))
	{
		// Reviewed since the last sync: the JSON may differ from the last sync, but it must still be the reviewed file.
		const FString FileHash = ReadJsonHash(Path, Text);
		if (!FileHash.IsEmpty() && FileHash != Res->FileHash)
		{
			MarkChangedAfterReview(PackageName, Text);
			NotifySaveSkipped(PackageName, true);
			OnPendingChanged.Broadcast();
			return;
		}
		WriteJson(Asset);
		return;
	}

	// Save guard: if the file changed since the last sync (e.g. a git pull with the editor open), writing would discard that change.
	if (FPaths::FileExists(Path) && FFileHelper::LoadFileToString(Text, *Path) && (!Entry || Entry->JsonHash != HashText(Text)))
	{
		FPending State;
		State.FileHash = HashText(Text);
		FAssetDefaults FileDoc;
		if (FromJson(Text, FileDoc, State.Error))
		{
			if (ValuesEqual(FileDoc, Capture(Asset)))
			{
				WriteJson(Asset);
				return;
			}
			State.File = MoveTemp(FileDoc);
		}
		// No base: the asset was edited after the JSON changed, so either side may have changed any key.
		MarkPending(PackageName, MoveTemp(State));
		NotifySaveSkipped(PackageName);
		OnPendingChanged.Broadcast();
		return;
	}
	WriteJson(Asset);
}

void UJsonDataAssetSyncSubsystem::OnAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath)
{
	const FString OldPackage = FPackageName::ObjectPathToPackageName(OldObjectPath);
	const FString NewPackage = Asset.PackageName.ToString();
	if (!IsSyncedPackage(OldPackage) || OldPackage == NewPackage || !IsDataAsset(Asset))
	{
		return;
	}
	if (Pending.Remove(OldPackage) > 0)
	{
		OnPendingChanged.Broadcast();
	}
	Resolved.Remove(OldPackage);
	Undone.Remove(OldPackage);

	const FString OldPath = GetJsonPath(OldPackage);
	FAssetDefaults Doc;
	FString Error;
	if (!LoadFile(OldPath, Doc, Error))
	{
		if (!Error.IsEmpty())
		{
			// Left where it is: it may hold something worth keeping, such as a merge conflict.
			Notify(FText::Format(LOCTEXT("MoveUnreadable", "JSON Sync: renamed {0} to {1}, but its JSON cannot be read ({2}), so it was not moved. Fix or delete {3}; saving {1} writes a new JSON."),
				FText::FromString(GetAssetName(OldPackage)), FText::FromString(GetAssetName(NewPackage)), FText::FromString(Error),
				FText::FromString(OldPath)), ENotice::Warning);
		}
		Cache.Remove(OldPackage);
		SaveCache();
		return;
	}
	// If the renamed package was already saved, its JSON is newer than the old file: keep it.
	if (IsSyncedPackage(NewPackage) && !FPaths::FileExists(GetJsonPath(NewPackage)))
	{
		// The cache entry is written when the renamed asset is saved.
		Doc.Asset = NewPackage;
		bool bWritten = false;
		if (!SaveFileIfChanged(GetJsonPath(NewPackage), Doc, bWritten))
		{
			Notify(FText::Format(LOCTEXT("MoveFailed", "Cannot write {0}"), FText::FromString(GetJsonPath(NewPackage))), ENotice::Error);
			return;
		}
	}
	DeleteJsonFile(OldPath);
	Cache.Remove(OldPackage);
	SaveCache();
	UE_LOG(LogJsonDataAssetSync, Log, TEXT("Moved %s to %s"), *OldPath, *GetJsonPath(NewPackage));
}

void UJsonDataAssetSyncSubsystem::OnAssetRemoved(const FAssetData& Asset)
{
	if (!IsSyncedPackage(Asset.PackageName.ToString()) || !IsDataAsset(Asset))
	{
		return;
	}
	PendingRemovals.Add(Asset.PackageName.ToString());
	if (!RemovalTicker.IsValid())
	{
		RemovalTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UJsonDataAssetSyncSubsystem::ProcessRemovals));
	}
}

bool UJsonDataAssetSyncSubsystem::ProcessRemovals(float DeltaTime)
{
	RemovalTicker.Reset();
	bool bChanged = false;
	for (const FString& PackageName : PendingRemovals)
	{
		// Checked a tick later, once the delete has finished. Only a deleted package file means the asset is gone.
		if (FPackageName::DoesPackageExist(PackageName))
		{
			continue;
		}
		DeleteJsonFile(GetJsonPath(PackageName));
		Cache.Remove(PackageName);
		Pending.Remove(PackageName);
		Resolved.Remove(PackageName);
		Undone.Remove(PackageName);
		bChanged = true;
	}
	PendingRemovals.Reset();
	if (bChanged)
	{
		SaveCache();
		OnPendingChanged.Broadcast();
	}
	return false;
}

#undef LOCTEXT_NAMESPACE
