#include "JsonBlueprintSyncSubsystem.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "JsonBlueprintSyncModule.h"
#include "JsonBlueprintSyncSettings.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(JsonBlueprintSyncSubsystem)

#define LOCTEXT_NAMESPACE "JsonBlueprintSync"

using namespace JsonBlueprintSync;

namespace
{
	/** The subsystem only syncs project content. The commandlet's -dir= covers other mount points. */
	const TCHAR* SyncedRoot = TEXT("/Game");

	bool IsSyncedPackage(const FString& PackageName)
	{
		return PackageName.StartsWith(FString(SyncedRoot) + TEXT("/")) && !FPackageName::IsTempPackage(PackageName);
	}

	bool IsDirty(const UObject* Asset)
	{
		return Asset->GetPackage()->IsDirty();
	}

	FString GetAssetName(const FString& PackageName)
	{
		return FPackageName::GetShortName(PackageName);
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
}

bool UJsonBlueprintSyncSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return !IsRunningCommandlet() && Super::ShouldCreateSubsystem(Outer);
}

void UJsonBlueprintSyncSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (!Cache.Load(FSyncCache::GetDefaultDir()))
	{
		UE_LOG(LogJsonBlueprintSync, Warning, TEXT("Cannot read the sync cache in %s; starting with an empty one"), *FSyncCache::GetDefaultDir());
	}

	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddUObject(this, &UJsonBlueprintSyncSubsystem::OnPackageSaved);

	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	AssetRenamedHandle = Registry.OnAssetRenamed().AddUObject(this, &UJsonBlueprintSyncSubsystem::OnAssetRenamed);
	AssetRemovedHandle = Registry.OnAssetRemoved().AddUObject(this, &UJsonBlueprintSyncSubsystem::OnAssetRemoved);
	if (Registry.IsLoadingAssets())
	{
		FilesLoadedHandle = Registry.OnFilesLoaded().AddUObject(this, &UJsonBlueprintSyncSubsystem::OnFilesLoaded);
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

	SyncCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("JsonBlueprintSync.Sync"),
		TEXT("Checks for JSON files that changed on disk since the last sync (e.g. after a git pull) and lists them in the JSON Sync tab."),
		FConsoleCommandDelegate::CreateUObject(this, &UJsonBlueprintSyncSubsystem::SyncFromJson));

	UE_LOG(LogJsonBlueprintSync, Log, TEXT("Syncing assets under %s with %s"), SyncedRoot, *GetJsonRoot());
}

void UJsonBlueprintSyncSubsystem::Deinitialize()
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
	CloseNotification(PendingNotification);
	Super::Deinitialize();
}

void UJsonBlueprintSyncSubsystem::OnFilesLoaded()
{
	if (IAssetRegistry* Registry = IAssetRegistry::Get())
	{
		Registry->OnFilesLoaded().Remove(FilesLoadedHandle);
	}
	FilesLoadedHandle.Reset();
	SyncFromJson();
}

FString UJsonBlueprintSyncSubsystem::GetJsonRoot() const
{
	// Read each time, so that a change in Project Settings takes effect without a restart.
	return JsonBlueprintSync::GetDefaultJsonRoot();
}

FString UJsonBlueprintSyncSubsystem::GetJsonPath(const FString& PackageName) const
{
	return JsonBlueprintSync::GetJsonPath(GetJsonRoot(), PackageName);
}

void UJsonBlueprintSyncSubsystem::SaveCache() const
{
	if (!Cache.Save())
	{
		UE_LOG(LogJsonBlueprintSync, Warning, TEXT("Cannot write the sync cache in %s"), *FSyncCache::GetDefaultDir());
	}
}

EAssetKind UJsonBlueprintSyncSubsystem::GetSyncedKind(const FAssetData& Asset) const
{
	const EAssetKind Kind = GetAssetKind(Asset);
	return GetDefault<UJsonBlueprintSyncSettings>()->GetMode(Kind) == EJsonSyncMode::None ? EAssetKind::None : Kind;
}

EAssetKind UJsonBlueprintSyncSubsystem::GetSyncedKind(const UObject* Asset) const
{
	const EAssetKind Kind = GetAssetKind(Asset);
	return GetDefault<UJsonBlueprintSyncSettings>()->GetMode(Kind) == EJsonSyncMode::None ? EAssetKind::None : Kind;
}

void UJsonBlueprintSyncSubsystem::Notify(const FText& Text, bool bError)
{
	if (bError)
	{
		UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s"), *Text.ToString());
	}
	else
	{
		UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s"), *Text.ToString());
	}
	FNotificationInfo Info(Text);
	Info.ExpireDuration = bError ? 10.0f : 6.0f;
	Info.bUseSuccessFailIcons = true;
	if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(bError ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
	}
}

void UJsonBlueprintSyncSubsystem::OpenSyncTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(JsonBlueprintSync::GetSyncTabName()));
}

void UJsonBlueprintSyncSubsystem::SyncFromJson()
{
	if (GEditor && GEditor->PlayWorld)
	{
		UE_LOG(LogJsonBlueprintSync, Warning, TEXT("Not checking JSON files during Play In Editor; run JsonBlueprintSync.Sync afterwards"));
		return;
	}

	const UJsonBlueprintSyncSettings* Settings = GetDefault<UJsonBlueprintSyncSettings>();
	const FString JsonRoot = GetJsonRoot();
	TArray<FString> Errors;
	TArray<FString> TwoWay;
	TSet<FString> Seen;
	int32 Checked = 0;
	for (const FString& File : FindJsonFiles(JsonRoot, SyncedRoot))
	{
		FString PackageName;
		FString Text;
		if (!JsonBlueprintSync::GetPackageName(JsonRoot, File, PackageName) || !FFileHelper::LoadFileToString(Text, *File))
		{
			Errors.Add(FString::Printf(TEXT("%s: cannot read the file"), *File));
			continue;
		}
		Seen.Add(PackageName);
		const FString FileHash = HashText(Text);
		const FSyncEntry* Entry = Cache.Find(PackageName);
		if (Entry && !Entry->bPending && Entry->JsonHash == FileHash)
		{
			// Unchanged since the last sync: skip without loading the asset.
			continue;
		}

		const TOptional<FAssetData> Asset = FindMainAsset(PackageName);
		if (!Asset)
		{
			Errors.Add(FString::Printf(TEXT("%s: no asset %s"), *File, *PackageName));
			continue;
		}
		const EAssetKind Kind = GetSyncedKind(*Asset);
		if (Kind == EAssetKind::None)
		{
			if (Pending.Remove(PackageName) > 0 || (Entry && Entry->bPending))
			{
				Cache.Remove(PackageName);
			}
			continue;
		}
		++Checked;
		if (Detect(PackageName, Text, FileHash, Kind) && Settings->GetMode(Kind) == EJsonSyncMode::TwoWay)
		{
			TwoWay.Add(PackageName);
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
	UE_LOG(LogJsonBlueprintSync, Log, TEXT("Checked %d changed JSON files, %d differ from their asset"), Checked, Pending.Num());

	for (const FString& Error : Errors)
	{
		UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s"), *Error);
	}
	if (Errors.Num() > 0)
	{
		Notify(FText::Format(LOCTEXT("SyncErrors", "JSON Sync: {0} JSON files could not be checked. See the Output Log (LogJsonBlueprintSync)."),
			FText::AsNumber(Errors.Num())), true);
	}

	// Two Way: apply what only the JSON changed. Anything that conflicts stays in the list for the user.
	TArray<TPair<FString, TArray<FString>>> Requests;
	for (const FString& PackageName : TwoWay)
	{
		FPendingView View;
		if (!GetPendingView(PackageName, View) || !View.Error.IsEmpty() || !View.bHasBase)
		{
			continue;
		}
		if (View.Keys.ContainsByPredicate([](const FKeyDiff& Diff) { return Diff.Change == EKeyChange::Both; }))
		{
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
		ApplyBatch(Requests, true);
	}

	ShowPendingNotification();
	OnPendingChanged.Broadcast();
}

bool UJsonBlueprintSyncSubsystem::Detect(const FString& PackageName, const FString& Text, const FString& FileHash, EAssetKind Kind)
{
	const FSyncEntry* Entry = Cache.Find(PackageName);
	const TOptional<FSyncEntry> OldEntry = Entry ? TOptional<FSyncEntry>(*Entry) : TOptional<FSyncEntry>();

	FPending State;
	State.Kind = Kind;
	FAssetDefaults FileDoc;
	if (FromJson(Text, FileDoc, State.Error))
	{
		State.File = MoveTemp(FileDoc);
	}

	UObject* Asset = FPackageName::DoesPackageExist(PackageName) ? LoadAsset(PackageName) : nullptr;
	if (!Asset)
	{
		UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: cannot load the asset"), *PackageName);
		return false;
	}
	const FAssetDefaults Current = Capture(Asset);
	if (State.File && ValuesEqual(*State.File, Current))
	{
		// They match, e.g. a teammate committed both. Record saved state only: recording an unsaved match would hide the
		// difference after an editor restart.
		Pending.Remove(PackageName);
		if (!IsDirty(Asset))
		{
			Cache.DeleteBase(PackageName);
			Cache.Set(PackageName, { FileHash, false });
		}
		else if (OldEntry && OldEntry->bPending)
		{
			Cache.DeleteBase(PackageName);
			Cache.Set(PackageName, { OldEntry->JsonHash, false });
		}
		return false;
	}

	// They differ. The base tells which side changed each key: the asset as it was when the JSON change was first seen.
	if (OldEntry && OldEntry->bPending)
	{
		FAssetDefaults Base;
		if (Cache.LoadBase(PackageName, Base))
		{
			State.Base = MoveTemp(Base);
		}
	}
	else if (OldEntry && !IsDirty(Asset))
	{
		// Known asset, saved state: everything that differs now was changed in the JSON.
		State.Base = Current;
		Cache.SaveBase(PackageName, Current);
	}
	// Otherwise (never synced on this machine, or unsaved edits): unknown, every key is a conflict.

	if (State.File)
	{
		UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s differs from its JSON: %s"), *PackageName,
			*JoinShort([&] { TArray<FString> Keys; for (const FKeyDiff& Diff : DiffKeys(State.Base.GetPtrOrNull(), *State.File, Current)) { Keys.Add(Diff.Key); } return Keys; }()));
	}
	else
	{
		UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: %s"), *GetJsonPath(PackageName), *State.Error);
	}
	Cache.Set(PackageName, { OldEntry ? OldEntry->JsonHash : FString(), true });
	Pending.Add(PackageName, MoveTemp(State));
	Resolved.Remove(PackageName);
	return true;
}

TArray<FString> UJsonBlueprintSyncSubsystem::GetPendingPackages() const
{
	TArray<FString> Packages;
	Pending.GetKeys(Packages);
	Packages.Sort();
	return Packages;
}

bool UJsonBlueprintSyncSubsystem::GetPendingView(const FString& PackageName, FPendingView& OutView) const
{
	const FPending* State = Pending.Find(PackageName);
	if (!State)
	{
		return false;
	}
	OutView = FPendingView();
	OutView.PackageName = PackageName;
	OutView.Kind = State->Kind;
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
	OutView.Keys = DiffKeys(State->Base.GetPtrOrNull(), *State->File, Capture(Asset));
	return true;
}

bool UJsonBlueprintSyncSubsystem::ApplyJson(const FString& PackageName, const TArray<FString>& Keys)
{
	const bool bResolved = ApplyBatch({ { PackageName, Keys } }, true).Contains(PackageName);
	OnPendingChanged.Broadcast();
	return bResolved;
}

TArray<FString> UJsonBlueprintSyncSubsystem::ApplyBatch(const TArray<TPair<FString, TArray<FString>>>& Requests, bool bNotify)
{
	struct FItem
	{
		FString PackageName;
		UObject* Asset = nullptr;
		FAssetDefaults Doc;
		int32 Order = 0;
	};
	TArray<FItem> Items;
	TArray<FString> Failed;
	for (const TPair<FString, TArray<FString>>& Request : Requests)
	{
		const FPending* State = Pending.Find(Request.Key);
		UObject* Asset = State && State->File && FPackageName::DoesPackageExist(Request.Key) ? LoadAsset(Request.Key) : nullptr;
		if (!Asset)
		{
			UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: not pending, or the JSON or the asset cannot be loaded"), *Request.Key);
			Failed.Add(GetAssetName(Request.Key));
			continue;
		}
		FItem& Item = Items.AddDefaulted_GetRef();
		Item.PackageName = Request.Key;
		Item.Asset = Asset;
		Item.Doc = TakeKeys(Capture(Asset), *State->File, Request.Value);
		Item.Order = GetApplyOrder(Asset);
	}
	// A child's values are a delta on its parent, and a data asset's on its class, so parents go first.
	Items.StableSort([](const FItem& A, const FItem& B) { return A.Order < B.Order; });

	TArray<FString> Changed;
	TArray<FString> Succeeded;
	{
		// One transaction, so a single Ctrl+Z reverts the whole apply. No compile: compiling replaces the CDO, which would make the
		// change impossible to undo. Values on the CDO and the templates survive the next compile, as with Details-panel edits.
		FScopedTransaction Transaction(LOCTEXT("ApplyTransaction", "Apply JSON"));
		for (const FItem& Item : Items)
		{
			const FApplyResult Result = Apply(Item.Asset, Item.Doc);
			for (const FString& Warning : Result.Warnings)
			{
				UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: %s"), *Item.PackageName, *Warning);
			}
			for (const FString& Error : Result.Errors)
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: %s"), *Item.PackageName, *Error);
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
				UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s: applied %s"), *Item.PackageName, *FString::Join(Result.Changed, TEXT(", ")));
			}
			Succeeded.Add(Item.PackageName);
		}
		if (Changed.Num() == 0)
		{
			Transaction.Cancel();
		}
	}
	for (const FItem& Item : Items)
	{
		if (Succeeded.Contains(Item.PackageName))
		{
			Resolve(Item.PackageName, Item.Asset);
		}
	}
	SaveCache();

	if (bNotify && Changed.Num() > 0)
	{
		Notify(FText::Format(LOCTEXT("Applied", "Applied JSON to {0}. Not saved; Ctrl+Z reverts."), FText::FromString(JoinShort(Changed))), false);
	}
	if (Failed.Num() > 0)
	{
		Notify(FText::Format(LOCTEXT("ApplyFailed", "Cannot apply the JSON of {0}; nothing was changed for them. See the Output Log (LogJsonBlueprintSync)."),
			FText::FromString(JoinShort(Failed))), true);
	}
	return Succeeded;
}

void UJsonBlueprintSyncSubsystem::Resolve(const FString& PackageName, UObject* Asset)
{
	Pending.Remove(PackageName);
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
		// The next save writes the JSON, including the keys where the user kept the asset's value.
		Resolved.Add(PackageName);
	}
	else
	{
		// Nothing changed (the user kept the asset's values): write the JSON now, or no save would ever do it.
		WriteJson(Asset);
	}
}

bool UJsonBlueprintSyncSubsystem::OverwriteJson(const FString& PackageName)
{
	UObject* Asset = FPackageName::DoesPackageExist(PackageName) ? LoadAsset(PackageName) : nullptr;
	if (!Asset)
	{
		Notify(FText::Format(LOCTEXT("OverwriteNoAsset", "Cannot load {0}"), FText::FromString(PackageName)), true);
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
	if (IsDirty(Asset))
	{
		Resolved.Add(PackageName);
	}
	const bool bWritten = WriteJson(Asset);
	SaveCache();
	OnPendingChanged.Broadcast();
	return bWritten;
}

bool UJsonBlueprintSyncSubsystem::WriteJson(UObject* Asset)
{
	const FString PackageName = Asset->GetPackage()->GetName();
	const FString Path = GetJsonPath(PackageName);
	const FAssetDefaults Doc = Capture(Asset);
	bool bWritten = false;
	if (!SaveFileIfChanged(Path, Doc, bWritten))
	{
		Notify(FText::Format(LOCTEXT("WriteFailed", "Cannot write {0}. Is it read-only?"), FText::FromString(Path)), true);
		return false;
	}
	if (bWritten)
	{
		UE_LOG(LogJsonBlueprintSync, Log, TEXT("Wrote %s"), *Path);
	}
	// Record saved state only (see Detect). The save event fires after the dirty flag is cleared.
	if (!IsDirty(Asset))
	{
		Cache.Set(PackageName, { HashText(ToJson(Doc)), false });
		Resolved.Remove(PackageName);
		SaveCache();
	}
	return true;
}

void UJsonBlueprintSyncSubsystem::MarkPending(const FString& PackageName, FPending&& State)
{
	const FSyncEntry* Entry = Cache.Find(PackageName);
	Cache.Set(PackageName, { Entry ? Entry->JsonHash : FString(), true });
	Cache.DeleteBase(PackageName);
	Pending.Add(PackageName, MoveTemp(State));
	Resolved.Remove(PackageName);
	SaveCache();
}

void UJsonBlueprintSyncSubsystem::ShowPendingNotification()
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

void UJsonBlueprintSyncSubsystem::NotifySaveSkipped(const FString& PackageName)
{
	UE_LOG(LogJsonBlueprintSync, Warning, TEXT("Saved %s but did not write %s: the JSON has changes that are not reviewed"),
		*PackageName, *GetJsonPath(PackageName));

	FNotificationInfo Info(FText::Format(LOCTEXT("SaveSkipped", "Saved {0}, but did not write its JSON: the JSON has changes you have not reviewed"),
		FText::FromString(GetAssetName(PackageName))));
	Info.ExpireDuration = 10.0f;
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("Review", "Review"),
		LOCTEXT("ReviewTip", "Open the JSON Sync tab to apply or discard the JSON changes"),
		FSimpleDelegate::CreateStatic(&UJsonBlueprintSyncSubsystem::OpenSyncTab),
		SNotificationItem::CS_None));
	FSlateNotificationManager::Get().AddNotification(Info);
}

void UJsonBlueprintSyncSubsystem::OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext Context)
{
	if (!Package || Context.IsFromAutoSave() || Context.IsProceduralSave() || Context.IsCooking())
	{
		return;
	}
	const FString PackageName = Package->GetName();
	if (!IsSyncedPackage(PackageName))
	{
		return;
	}
	UObject* Asset = Package->FindAssetInPackage();
	const EAssetKind Kind = GetSyncedKind(Asset);
	if (Kind == EAssetKind::None)
	{
		return;
	}

	const FSyncEntry* Entry = Cache.Find(PackageName);
	if (Pending.Contains(PackageName) || (Entry && Entry->bPending))
	{
		NotifySaveSkipped(PackageName);
		return;
	}

	// Save guard: if the file changed since the last sync (e.g. a git pull with the editor open), writing would discard that change.
	const FString Path = GetJsonPath(PackageName);
	FString Text;
	if (!Resolved.Contains(PackageName) && FPaths::FileExists(Path) && FFileHelper::LoadFileToString(Text, *Path)
		&& (!Entry || Entry->JsonHash != HashText(Text)))
	{
		FPending State;
		State.Kind = Kind;
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

void UJsonBlueprintSyncSubsystem::OnAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath)
{
	const FString OldPackage = FPackageName::ObjectPathToPackageName(OldObjectPath);
	const FString NewPackage = Asset.PackageName.ToString();
	if (!IsSyncedPackage(OldPackage) || OldPackage == NewPackage || GetSyncedKind(Asset) == EAssetKind::None)
	{
		return;
	}
	if (Pending.Remove(OldPackage) > 0)
	{
		OnPendingChanged.Broadcast();
	}
	Resolved.Remove(OldPackage);

	const FString OldPath = GetJsonPath(OldPackage);
	FAssetDefaults Doc;
	FString Error;
	if (!LoadFile(OldPath, Doc, Error))
	{
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
			Notify(FText::Format(LOCTEXT("MoveFailed", "Cannot write {0}"), FText::FromString(GetJsonPath(NewPackage))), true);
			return;
		}
	}
	IFileManager::Get().Delete(*OldPath, false, true, true);
	Cache.Remove(OldPackage);
	SaveCache();
	UE_LOG(LogJsonBlueprintSync, Log, TEXT("Moved %s to %s"), *OldPath, *GetJsonPath(NewPackage));
}

void UJsonBlueprintSyncSubsystem::OnAssetRemoved(const FAssetData& Asset)
{
	if (!IsSyncedPackage(Asset.PackageName.ToString()) || GetSyncedKind(Asset) == EAssetKind::None)
	{
		return;
	}
	PendingRemovals.Add(Asset.PackageName.ToString());
	if (!RemovalTicker.IsValid())
	{
		RemovalTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UJsonBlueprintSyncSubsystem::ProcessRemovals));
	}
}

bool UJsonBlueprintSyncSubsystem::ProcessRemovals(float DeltaTime)
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
		const FString Path = GetJsonPath(PackageName);
		if (FPaths::FileExists(Path))
		{
			IFileManager::Get().Delete(*Path, false, true, true);
			UE_LOG(LogJsonBlueprintSync, Log, TEXT("Deleted %s"), *Path);
		}
		Cache.Remove(PackageName);
		Pending.Remove(PackageName);
		Resolved.Remove(PackageName);
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
