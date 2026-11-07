#include "JsonBlueprintSyncSubsystem.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "JsonBlueprintSyncModule.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(JsonBlueprintSyncSubsystem)

#define LOCTEXT_NAMESPACE "JsonBlueprintSync"

namespace
{
	/** The subsystem only syncs project content. The commandlet's -dir= covers other mount points. */
	const TCHAR* SyncedRoot = TEXT("/Game");

	bool IsSyncedPackage(const FString& PackageName)
	{
		return PackageName.StartsWith(FString(SyncedRoot) + TEXT("/"));
	}

	bool IsBlueprintAsset(const FAssetData& Asset)
	{
		return Asset.IsInstanceOf(UBlueprint::StaticClass());
	}

	bool IsDirty(const UBlueprint* Blueprint)
	{
		return Blueprint->GetPackage()->IsDirty();
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
}

bool UJsonBlueprintSyncSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (IsRunningCommandlet() || !Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	bool bAutoSync = true;
	GConfig->GetBool(TEXT("JsonBlueprintSync"), TEXT("bAutoSync"), bAutoSync, GEditorIni);
	return bAutoSync;
}

void UJsonBlueprintSyncSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	JsonRoot = JsonBlueprintSync::GetDefaultJsonRoot();
	if (!Cache.Load(JsonBlueprintSync::FSyncCache::GetDefaultPath()))
	{
		UE_LOG(LogJsonBlueprintSync, Warning, TEXT("Cannot read %s; starting with an empty sync cache"), *JsonBlueprintSync::FSyncCache::GetDefaultPath());
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
		TEXT("Imports Blueprint defaults JSON files that changed on disk since the last sync (e.g. after a git pull)."),
		FConsoleCommandDelegate::CreateUObject(this, &UJsonBlueprintSyncSubsystem::SyncFromJson));

	UE_LOG(LogJsonBlueprintSync, Log, TEXT("Syncing Blueprint defaults under %s with %s"), SyncedRoot, *JsonRoot);
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
	for (const TPair<FString, TWeakPtr<SNotificationItem>>& Pair : OpenConflicts)
	{
		CloseNotification(Pair.Value);
	}
	OpenConflicts.Reset();
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

FString UJsonBlueprintSyncSubsystem::GetJsonPath(const FString& PackageName) const
{
	return JsonBlueprintSync::GetJsonPath(JsonRoot, PackageName);
}

void UJsonBlueprintSyncSubsystem::SaveCache() const
{
	if (!Cache.Save(JsonBlueprintSync::FSyncCache::GetDefaultPath()))
	{
		UE_LOG(LogJsonBlueprintSync, Warning, TEXT("Cannot write %s"), *JsonBlueprintSync::FSyncCache::GetDefaultPath());
	}
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

void UJsonBlueprintSyncSubsystem::SyncFromJson()
{
	if (GEditor && GEditor->PlayWorld)
	{
		UE_LOG(LogJsonBlueprintSync, Warning, TEXT("Not syncing Blueprint defaults during Play In Editor; run JsonBlueprintSync.Sync afterwards"));
		return;
	}

	TArray<FString> Errors;
	TArray<FPendingImport> Imports;
	int32 Checked = 0;
	for (const FString& File : JsonBlueprintSync::FindJsonFiles(JsonRoot, SyncedRoot))
	{
		FString PackageName;
		FString Text;
		if (!JsonBlueprintSync::GetPackageName(JsonRoot, File, PackageName) || !FFileHelper::LoadFileToString(Text, *File))
		{
			Errors.Add(FString::Printf(TEXT("%s: cannot read the file"), *File));
			continue;
		}
		const FString FileHash = JsonBlueprintSync::HashText(Text);
		const JsonBlueprintSync::FSyncEntry* Entry = Cache.Find(PackageName);
		if ((Entry && Entry->JsonHash == FileHash) || OpenConflicts.Contains(PackageName))
		{
			// Unchanged since the last sync, or already waiting for the user: skip without loading the Blueprint.
			continue;
		}

		JsonBlueprintSync::FBlueprintDefaults Doc;
		FString Error;
		if (!JsonBlueprintSync::FromJson(Text, Doc, Error))
		{
			Errors.Add(FString::Printf(TEXT("%s: %s"), *File, *Error));
			continue;
		}
		UBlueprint* Blueprint = FPackageName::DoesPackageExist(PackageName) ? JsonBlueprintSync::LoadBlueprint(PackageName) : nullptr;
		if (!Blueprint)
		{
			Errors.Add(FString::Printf(TEXT("%s: no Blueprint %s"), *File, *PackageName));
			continue;
		}
		++Checked;

		const JsonBlueprintSync::FBlueprintDefaults Current = JsonBlueprintSync::Capture(Blueprint, &Doc);
		const FString CurrentHash = JsonBlueprintSync::HashText(JsonBlueprintSync::ToJson(Current));
		const FString AssetHash = JsonBlueprintSync::HashText(JsonBlueprintSync::ToJson(JsonBlueprintSync::Capture(Blueprint)));
		switch (JsonBlueprintSync::Classify(Entry, FileHash, CurrentHash, AssetHash))
		{
		case JsonBlueprintSync::ESyncAction::None:
			break;
		case JsonBlueprintSync::ESyncAction::UpdateCache:
			// Record saved state only: recording an unsaved match would hide the difference after an editor restart.
			if (!IsDirty(Blueprint))
			{
				Cache.Set(PackageName, { FileHash, AssetHash });
			}
			break;
		case JsonBlueprintSync::ESyncAction::Import:
			Imports.Add({ Blueprint, MoveTemp(Doc) });
			break;
		case JsonBlueprintSync::ESyncAction::Conflict:
			ShowConflict(Blueprint, JsonBlueprintSync::DiffKeys(Current, Doc));
			break;
		}
	}
	SaveCache();
	UE_LOG(LogJsonBlueprintSync, Log, TEXT("Checked %d changed JSON files, %d to import"), Checked, Imports.Num());

	for (const FString& Error : Errors)
	{
		UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s"), *Error);
	}
	if (Errors.Num() > 0)
	{
		Notify(FText::Format(LOCTEXT("SyncErrors", "Blueprint defaults: {0} JSON files could not be imported. See the Output Log (LogJsonBlueprintSync)."),
			FText::AsNumber(Errors.Num())), true);
	}
	ImportDocuments(MoveTemp(Imports));
}

void UJsonBlueprintSyncSubsystem::ImportDocuments(TArray<FPendingImport> Imports)
{
	Imports.RemoveAll([](const FPendingImport& Import) { return !Import.Blueprint.IsValid(); });
	if (Imports.Num() == 0)
	{
		return;
	}
	// A child's JSON is a delta on its parent, so the parent's values must propagate first.
	Imports.StableSort([](const FPendingImport& A, const FPendingImport& B)
	{
		return JsonBlueprintSync::GetBlueprintDepth(A.Blueprint.Get()) < JsonBlueprintSync::GetBlueprintDepth(B.Blueprint.Get());
	});

	TArray<FString> Changed;
	TArray<FString> Failed;
	{
		// One transaction, so a single Ctrl+Z reverts the whole import. No compile: compiling replaces the CDO, which would make the
		// change impossible to undo. Values on the CDO and the templates survive the next compile, as with Details-panel edits.
		FScopedTransaction Transaction(LOCTEXT("ImportTransaction", "Import Blueprint Defaults from JSON"));
		for (const FPendingImport& Import : Imports)
		{
			UBlueprint* Blueprint = Import.Blueprint.Get();
			const FString PackageName = Blueprint->GetPackage()->GetName();
			const JsonBlueprintSync::FApplyResult Result = JsonBlueprintSync::Apply(Blueprint, Import.Doc);
			for (const FString& Warning : Result.Warnings)
			{
				UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: %s"), *PackageName, *Warning);
			}
			for (const FString& Error : Result.Errors)
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: %s"), *PackageName, *Error);
			}
			if (Result.HasErrors())
			{
				Failed.Add(GetAssetName(PackageName));
				continue;
			}
			if (Result.Changed.Num() > 0)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				Changed.Add(GetAssetName(PackageName));
				UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s: imported %s"), *PackageName, *FString::Join(Result.Changed, TEXT(", ")));
			}
			// The cache is not updated: it records saved state only. Saving the Blueprint exports and records it.
		}
		if (Changed.Num() == 0)
		{
			Transaction.Cancel();
		}
	}

	if (Changed.Num() > 0)
	{
		Notify(FText::Format(LOCTEXT("Imported", "Imported Blueprint defaults from JSON into {0}. Not saved; Ctrl+Z reverts."),
			FText::FromString(JoinShort(Changed))), false);
	}
	if (Failed.Num() > 0)
	{
		Notify(FText::Format(LOCTEXT("ImportFailed", "Blueprint defaults JSON has invalid values for {0}; nothing was imported for them. See the Output Log."),
			FText::FromString(JoinShort(Failed))), true);
	}
}

void UJsonBlueprintSyncSubsystem::ShowConflict(UBlueprint* Blueprint, const TArray<FString>& Keys)
{
	const FString PackageName = Blueprint->GetPackage()->GetName();
	UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: the Blueprint and %s both changed and disagree on %s"),
		*PackageName, *GetJsonPath(PackageName), *FString::Join(Keys, TEXT(", ")));

	FNotificationInfo Info(FText::Format(LOCTEXT("Conflict", "{0}: the Blueprint and its defaults JSON both changed"), FText::FromString(GetAssetName(PackageName))));
	Info.SubText = FText::Format(LOCTEXT("ConflictKeys", "Different: {0}"), FText::FromString(JoinShort(Keys)));
	Info.bFireAndForget = false;
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("UseJson", "Use JSON"),
		LOCTEXT("UseJsonTip", "Import the JSON file into the Blueprint (not saved; Ctrl+Z reverts)"),
		FSimpleDelegate::CreateWeakLambda(this, [this, PackageName]() { ResolveConflict(PackageName, true); })));
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("KeepBlueprint", "Keep Blueprint"),
		LOCTEXT("KeepBlueprintTip", "Overwrite the JSON file with the current Blueprint defaults"),
		FSimpleDelegate::CreateWeakLambda(this, [this, PackageName]() { ResolveConflict(PackageName, false); })));

	if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(SNotificationItem::CS_Pending);
		OpenConflicts.Add(PackageName, Item);
	}
}

void UJsonBlueprintSyncSubsystem::ResolveConflict(const FString& PackageName, bool bUseJson)
{
	TWeakPtr<SNotificationItem> Notification;
	OpenConflicts.RemoveAndCopyValue(PackageName, Notification);
	CloseNotification(Notification);

	UBlueprint* Blueprint = FPackageName::DoesPackageExist(PackageName) ? JsonBlueprintSync::LoadBlueprint(PackageName) : nullptr;
	if (!Blueprint)
	{
		Notify(FText::Format(LOCTEXT("ConflictGone", "{0} no longer exists"), FText::FromString(PackageName)), true);
		return;
	}
	if (!bUseJson)
	{
		ExportBlueprint(Blueprint);
		return;
	}
	JsonBlueprintSync::FBlueprintDefaults Doc;
	FString Error;
	if (!JsonBlueprintSync::LoadFile(GetJsonPath(PackageName), Doc, Error))
	{
		Notify(FText::Format(LOCTEXT("ConflictBadFile", "Cannot read {0}: {1}"), FText::FromString(GetJsonPath(PackageName)),
			FText::FromString(Error.IsEmpty() ? TEXT("missing") : Error)), true);
		return;
	}
	TArray<FPendingImport> Imports;
	Imports.Add({ Blueprint, MoveTemp(Doc) });
	ImportDocuments(MoveTemp(Imports));
}

bool UJsonBlueprintSyncSubsystem::ExportBlueprint(UBlueprint* Blueprint)
{
	const FString PackageName = Blueprint->GetPackage()->GetName();
	const FString Path = GetJsonPath(PackageName);

	JsonBlueprintSync::FBlueprintDefaults Previous;
	FString Error;
	FString OldText;
	const bool bHasPrevious = JsonBlueprintSync::LoadFile(Path, Previous, Error) && FFileHelper::LoadFileToString(OldText, *Path);
	if (!Error.IsEmpty())
	{
		UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: ignoring the existing file, %s"), *Path, *Error);
	}

	const JsonBlueprintSync::FBlueprintDefaults Doc = JsonBlueprintSync::Capture(Blueprint, bHasPrevious ? &Previous : nullptr);
	bool bWritten = false;
	if (!JsonBlueprintSync::SaveFileIfChanged(Path, Doc, bWritten))
	{
		Notify(FText::Format(LOCTEXT("ExportFailed", "Cannot write {0}. Is it read-only?"), FText::FromString(Path)), true);
		return false;
	}

	// The asset wins on save. If the file had changed outside the editor since the last sync, say what was overwritten.
	TWeakPtr<SNotificationItem> Conflict;
	const bool bHadConflict = OpenConflicts.RemoveAndCopyValue(PackageName, Conflict);
	const JsonBlueprintSync::FSyncEntry* Entry = Cache.Find(PackageName);
	if (bWritten && bHasPrevious && Entry && Entry->JsonHash != JsonBlueprintSync::HashText(OldText) && !bHadConflict)
	{
		Notify(FText::Format(LOCTEXT("Overwrote", "Saving {0} overwrote changes made to its JSON file outside the editor: {1}. Restore them with git if needed."),
			FText::FromString(GetAssetName(PackageName)), FText::FromString(JoinShort(JsonBlueprintSync::DiffKeys(Previous, Doc)))), true);
	}
	if (bWritten)
	{
		UE_LOG(LogJsonBlueprintSync, Log, TEXT("Wrote %s"), *Path);
	}
	// Saving the Blueprint or picking a side settles an open conflict.
	CloseNotification(Conflict);

	// Record saved state only (see SyncFromJson). The save event fires after the dirty flag is cleared.
	if (!IsDirty(Blueprint))
	{
		Cache.Set(PackageName, { JsonBlueprintSync::HashText(JsonBlueprintSync::ToJson(Doc)),
			JsonBlueprintSync::HashText(JsonBlueprintSync::ToJson(JsonBlueprintSync::Capture(Blueprint))) });
		SaveCache();
	}
	return true;
}

void UJsonBlueprintSyncSubsystem::OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext Context)
{
	if (!Package || Context.IsFromAutoSave() || Context.IsProceduralSave() || Context.IsCooking())
	{
		return;
	}
	if (!IsSyncedPackage(Package->GetName()) || FPackageName::IsTempPackage(Package->GetName()))
	{
		return;
	}
	if (UBlueprint* Blueprint = Cast<UBlueprint>(Package->FindAssetInPackage()))
	{
		ExportBlueprint(Blueprint);
	}
}

void UJsonBlueprintSyncSubsystem::OnAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath)
{
	if (!IsBlueprintAsset(Asset))
	{
		return;
	}
	const FString OldPackage = FPackageName::ObjectPathToPackageName(OldObjectPath);
	const FString NewPackage = Asset.PackageName.ToString();
	if (!IsSyncedPackage(OldPackage) || OldPackage == NewPackage)
	{
		return;
	}
	const FString OldPath = GetJsonPath(OldPackage);
	JsonBlueprintSync::FBlueprintDefaults Doc;
	FString Error;
	if (!JsonBlueprintSync::LoadFile(OldPath, Doc, Error))
	{
		return;
	}
	// If the renamed package was already saved, its export is newer than the old file: keep it.
	if (IsSyncedPackage(NewPackage) && !FPaths::FileExists(GetJsonPath(NewPackage)))
	{
		// Move the file, keeping its reset list. The cache entry is written when the renamed Blueprint is saved.
		Doc.Blueprint = NewPackage;
		bool bWritten = false;
		if (!JsonBlueprintSync::SaveFileIfChanged(GetJsonPath(NewPackage), Doc, bWritten))
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
	if (!IsBlueprintAsset(Asset) || !IsSyncedPackage(Asset.PackageName.ToString()))
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
		// Checked a tick later, once the delete has finished. Only a deleted package file means the Blueprint is gone.
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
		bChanged = true;
	}
	PendingRemovals.Reset();
	if (bChanged)
	{
		SaveCache();
	}
	return false;
}

#undef LOCTEXT_NAMESPACE