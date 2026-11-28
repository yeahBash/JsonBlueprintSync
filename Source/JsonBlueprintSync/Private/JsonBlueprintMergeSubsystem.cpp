#include "JsonBlueprintMergeSubsystem.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "JsonBlueprintSync.h"
#include "JsonBlueprintSyncModule.h"
#include "JsonBlueprintSyncSubsystem.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/App.h"
#include "Misc/ITransaction.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(JsonBlueprintMergeSubsystem)

#define LOCTEXT_NAMESPACE "JsonBlueprintSync"

using namespace JsonBlueprintSync;
using namespace JsonBlueprintSync::AssetMerge;

namespace
{
	using EMergeNotice = UJsonBlueprintSyncSubsystem::ENotice;

	FString MergeAssetName(const FString& PackageName)
	{
		return FPackageName::GetShortName(PackageName);
	}

	/** At most Max names, then "and N more". */
	FString JoinNames(const TArray<FString>& Names, int32 Max = 8)
	{
		if (Names.Num() <= Max)
		{
			return FString::Join(Names, TEXT(", "));
		}
		const TArray<FString> Head(Names.GetData(), Max);
		return FString::Printf(TEXT("%s and %d more"), *FString::Join(Head, TEXT(", ")), Names.Num() - Max);
	}

	FString DescribeValue(const TOptional<FString>& Value)
	{
		return Value.IsSet() ? *Value : FString(TEXT("(inherited)"));
	}

	bool IsPackageDirty(const FString& PackageName)
	{
		const UPackage* Package = FindPackage(nullptr, *PackageName);
		return Package && Package->IsDirty();
	}

	FString NormalizedDir(const FString& Dir)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Dir);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	void CloseMergeNotification(const TWeakPtr<SNotificationItem>& Notification)
	{
		if (const TSharedPtr<SNotificationItem> Item = Notification.Pin())
		{
			Item->SetCompletionState(SNotificationItem::CS_None);
			Item->ExpireAndFadeout();
		}
	}
}

bool UJsonBlueprintMergeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return !IsRunningCommandlet() && Super::ShouldCreateSubsystem(Outer);
}

void UJsonBlueprintMergeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The first check comes from UJsonBlueprintSyncSubsystem::SyncFromJson, once the asset registry has loaded.
	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddUObject(this, &UJsonBlueprintMergeSubsystem::OnPackageSaved);
	if (GEditor)
	{
		GEditor->RegisterForUndo(this);
	}

	MergesCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("JsonBlueprintSync.Merges"),
		TEXT("Checks the asset merges saved by the merge tool (Saved/JsonBlueprintSync/Merge) again and logs what each would change in its asset."),
		FConsoleCommandDelegate::CreateWeakLambda(this, [this]()
		{
			Refresh(false);
			LogEntries();
		}));
	ApplyMergeCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("JsonBlueprintSync.ApplyMerge"),
		TEXT("JsonBlueprintSync.ApplyMerge <package> [save]: applies the rows of a Ready merge to its asset, as Apply in the Asset Merges tab; with save, as Apply and Save."),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UJsonBlueprintMergeSubsystem::ApplyCommand));
}

void UJsonBlueprintMergeSubsystem::Deinitialize()
{
	UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
	FTSTicker::GetCoreTicker().RemoveTicker(SavedTicker);
	for (IConsoleObject** Command : { &MergesCommand, &ApplyMergeCommand })
	{
		if (*Command)
		{
			IConsoleManager::Get().UnregisterConsoleObject(*Command);
			*Command = nullptr;
		}
	}
	if (GEditor)
	{
		GEditor->UnregisterForUndo(this);
	}
	CloseMergeNotification(Notification);
	Super::Deinitialize();
}

bool UJsonBlueprintMergeSubsystem::MatchesContext(const FTransactionContext& InContext, const TArray<TPair<UObject*, FTransactionObjectEvent>>& TransactionObjectContexts) const
{
	// Called just before PostUndo/PostRedo, with the transaction being undone or redone.
	UndoRedoTransactionId = InContext.TransactionId;
	return true;
}

void UJsonBlueprintMergeSubsystem::PostUndo(bool bSuccess)
{
	const FGuid TransactionId = UndoRedoTransactionId;
	UndoRedoTransactionId.Invalidate();
	if (!bSuccess || !TransactionId.IsValid())
	{
		return;
	}
	const bool bOurs = Entries.ContainsByPredicate([&TransactionId](const FEntry& Entry)
	{
		for (const TPair<FString, FGuid>& Pair : Entry.Applied)
		{
			if (Pair.Value == TransactionId)
			{
				return true;
			}
		}
		return false;
	});
	if (bOurs)
	{
		UndoneTransactions.Add(TransactionId);
		OnChanged.Broadcast();
	}
}

void UJsonBlueprintMergeSubsystem::PostRedo(bool bSuccess)
{
	const FGuid TransactionId = UndoRedoTransactionId;
	UndoRedoTransactionId.Invalidate();
	if (bSuccess && TransactionId.IsValid() && UndoneTransactions.Remove(TransactionId) > 0)
	{
		OnChanged.Broadcast();
	}
}

void UJsonBlueprintMergeSubsystem::Refresh(bool bNotify)
{
	Check({});
	if (bNotify)
	{
		ShowNotification();
	}
	OnChanged.Broadcast();
}

const UJsonBlueprintMergeSubsystem::FEntry* UJsonBlueprintMergeSubsystem::FindEntry(const FString& PackageName) const
{
	return Entries.FindByPredicate([&PackageName](const FEntry& Entry) { return Entry.File.Location.PackageName == PackageName; });
}

UJsonBlueprintMergeSubsystem::FEntry* UJsonBlueprintMergeSubsystem::FindMutableEntry(const FString& PackageName)
{
	return Entries.FindByPredicate([&PackageName](const FEntry& Entry) { return Entry.File.Location.PackageName == PackageName; });
}

EMergeStatus UJsonBlueprintMergeSubsystem::GetStatus(const FEntry& Entry) const
{
	const EMergeStatus Status = Entry.File.Status;
	if ((Status == EMergeStatus::Ready || Status == EMergeStatus::Done) && !Entry.bChangedByApply && IsPackageDirty(Entry.File.Location.PackageName))
	{
		return EMergeStatus::UnsavedChanges;
	}
	return Status;
}

FString UJsonBlueprintMergeSubsystem::GetStatusMessage(const FEntry& Entry) const
{
	if (GetStatus(Entry) == EMergeStatus::UnsavedChanges && Entry.File.Status != EMergeStatus::UnsavedChanges)
	{
		return TEXT("The list is for the saved file. Save or revert the asset first.");
	}
	return Entry.File.Message;
}

bool UJsonBlueprintMergeSubsystem::IsAppliedNotSaved(const FEntry& Entry, const FString& RowKey) const
{
	const FGuid* TransactionId = Entry.Applied.Find(RowKey);
	return TransactionId && !UndoneTransactions.Contains(*TransactionId);
}

int32 UJsonBlueprintMergeSubsystem::GetToApplyNum() const
{
	int32 Num = 0;
	for (const FEntry& Entry : Entries)
	{
		Num += GetStatus(Entry) != EMergeStatus::Done ? 1 : 0;
	}
	return Num;
}

FString UJsonBlueprintMergeSubsystem::GetRowKey(const FMergeRow& Row)
{
	// A path never holds a line break: the merge text has one line per path.
	return Row.Object + TEXT("\n") + Row.Path;
}

void UJsonBlueprintMergeSubsystem::Check(const TOptional<TSet<FString>>& Packages)
{
	TArray<FMergeFileLocation> Locations = FindMergeFiles(GetMergeDir());
	if (Packages)
	{
		Locations.RemoveAll([&Packages](const FMergeFileLocation& Location) { return !Packages->Contains(Location.PackageName); });
	}
	// Unsaved changes are checked live (GetStatus), since Apply makes some of them.
	TArray<FMergeFile> Files = CheckMergeFiles(Locations, false);

	TMap<FString, FEntry> Old;
	for (FEntry& Entry : Entries)
	{
		const FString PackageName = Entry.File.Location.PackageName;
		Old.Add(PackageName, MoveTemp(Entry));
	}
	TArray<FEntry> New;
	if (Packages)
	{
		for (TPair<FString, FEntry>& Pair : Old)
		{
			if (!Packages->Contains(Pair.Key))
			{
				New.Add(MoveTemp(Pair.Value));
			}
		}
	}
	for (FMergeFile& File : Files)
	{
		FEntry& Entry = New.AddDefaulted_GetRef();
		// What this session applied stays until the asset is saved: the saved file doesn't have it yet.
		if (FEntry* Previous = Old.Find(File.Location.PackageName))
		{
			Entry.Applied = MoveTemp(Previous->Applied);
			Entry.Failures = MoveTemp(Previous->Failures);
			Entry.bChangedByApply = Previous->bChangedByApply;
		}
		Entry.File = MoveTemp(File);
	}
	New.Sort([](const FEntry& A, const FEntry& B) { return A.File.Location.PackageName < B.File.Location.PackageName; });
	Entries = MoveTemp(New);
	UE_LOG(LogJsonBlueprintSync, Log, TEXT("Checked %d merge file(s) in %s"), Files.Num(), *GetMergeDir());
}

void UJsonBlueprintMergeSubsystem::ShowNotification()
{
	CloseMergeNotification(Notification);
	Notification.Reset();
	TArray<FString> Names;
	for (const FEntry& Entry : Entries)
	{
		if (GetStatus(Entry) != EMergeStatus::Done)
		{
			Names.Add(MergeAssetName(Entry.File.Location.PackageName));
		}
	}
	if (Names.Num() == 0)
	{
		return;
	}
	FNotificationInfo Info(FText::Format(LOCTEXT("MergesToApply", "JSON Sync: {0} merge(s) to apply"), FText::AsNumber(Names.Num())));
	Info.SubText = FText::FromString(JoinNames(Names));
	Info.bFireAndForget = false;
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("OpenMerges", "Open"),
		LOCTEXT("OpenMergesTip", "Open the Asset Merges tab to apply the merges"),
		FSimpleDelegate::CreateWeakLambda(this, [this]()
		{
			CloseMergeNotification(Notification);
			OpenMergeTab();
		}),
		SNotificationItem::CS_Pending));
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("LaterMerges", "Later"),
		LOCTEXT("LaterMergesTip", "Close this notification. The merges stay in Window > Asset Merges."),
		FSimpleDelegate::CreateWeakLambda(this, [this]() { CloseMergeNotification(Notification); }),
		SNotificationItem::CS_Pending));
	if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(SNotificationItem::CS_Pending);
		Notification = Item;
	}
}

void UJsonBlueprintMergeSubsystem::OpenMergeTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(JsonBlueprintSync::GetMergeTabName()));
}

int32 UJsonBlueprintMergeSubsystem::Apply(const TArray<FApplyRequest>& Requests, bool bSave)
{
	struct FItem
	{
		FString PackageName;
		UObject* Asset = nullptr;
		TArray<FMergeRow> Rows;
		TArray<FString> Keys;
		int32 Order = 0;
	};
	TArray<FItem> Items;
	TArray<FString> Skipped;
	for (const FApplyRequest& Request : Requests)
	{
		const FEntry* Entry = FindEntry(Request.Key);
		if (!Entry || !CanApply(*Entry))
		{
			const FString Status = Entry ? LexToString(GetStatus(*Entry)) : TEXT("no merge file");
			UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: not applied (%s)"), *Request.Key, *Status);
			Skipped.Add(FString::Printf(TEXT("%s (%s)"), *MergeAssetName(Request.Key), *Status));
			continue;
		}
		const TSet<FString> Wanted(Request.Value);
		FItem Item;
		Item.PackageName = Request.Key;
		for (const FMergeRow& Row : Entry->File.Rows)
		{
			const FString Key = GetRowKey(Row);
			if (Row.Action != EMergeAction::NotApplicable && Wanted.Contains(Key) && !IsAppliedNotSaved(*Entry, Key))
			{
				Item.Rows.Add(Row);
				Item.Keys.Add(Key);
			}
		}
		if (Item.Rows.IsEmpty())
		{
			continue;
		}
		Item.Asset = LoadAsset(Request.Key);
		if (!Item.Asset)
		{
			UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: can't load the asset"), *Request.Key);
			Skipped.Add(FString::Printf(TEXT("%s (can't load it)"), *MergeAssetName(Request.Key)));
			continue;
		}
		Item.Order = GetApplyOrder(Item.Asset);
		Items.Add(MoveTemp(Item));
	}
	// A child's values are a delta on its parent, and a data asset's on its class, so parents go first.
	Items.StableSort([](const FItem& A, const FItem& B) { return A.Order < B.Order; });

	// Assets that Apply makes dirty, also by propagation to a child with a merge of its own, count as changed by the merge.
	TSet<FString> CleanBefore;
	for (const FEntry& Entry : Entries)
	{
		if (!IsPackageDirty(Entry.File.Location.PackageName))
		{
			CleanBefore.Add(Entry.File.Location.PackageName);
		}
	}

	TArray<TArray<FRowResult>> Results;
	FGuid TransactionId;
	int32 NumApplied = 0;
	int32 NumFailed = 0;
	{
		// One transaction for every asset, so a single Ctrl+Z reverts the whole apply.
		FScopedTransaction Transaction(LOCTEXT("ApplyMergeTransaction", "Apply Merge"));
		if (GUndo)
		{
			TransactionId = GUndo->GetContext().TransactionId;
		}
		for (const FItem& Item : Items)
		{
			const TArray<FRowResult>& ItemResults = Results.Add_GetRef(ApplyRows(Item.Asset, Item.Rows));
			for (const FRowResult& Result : ItemResults)
			{
				NumApplied += Result.Result == ERowResult::Applied ? 1 : 0;
				NumFailed += Result.Result == ERowResult::Failed ? 1 : 0;
			}
		}
		if (NumApplied == 0)
		{
			Transaction.Cancel();
			TransactionId.Invalidate();
		}
	}

	TArray<FString> ChangedNames;
	TArray<UObject*> ChangedAssets;
	for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
	{
		const FItem& Item = Items[ItemIndex];
		FEntry* Entry = FindMutableEntry(Item.PackageName);
		int32 ItemApplied = 0;
		for (int32 Index = 0; Index < Item.Rows.Num(); ++Index)
		{
			const FMergeRow& Row = Item.Rows[Index];
			const FRowResult& Result = Results[ItemIndex][Index];
			UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s  %s: %s [%s] %s%s%s"), *Item.PackageName, LexToString(Result.Result), LexToString(Row.Action),
				*Row.Object, *Row.Path, Result.Message.IsEmpty() ? TEXT("") : TEXT(". "), *Result.Message);
			if (Result.Result == ERowResult::Failed)
			{
				Entry->Failures.Add(Item.Keys[Index], Result.Message);
				continue;
			}
			// Unchanged: the loaded asset already has the merged value (e.g. from a parent in this apply), but the saved file doesn't.
			Entry->Failures.Remove(Item.Keys[Index]);
			Entry->Applied.Add(Item.Keys[Index], TransactionId);
			ItemApplied += Result.Result == ERowResult::Applied ? 1 : 0;
		}
		if (ItemApplied > 0)
		{
			ChangedNames.Add(MergeAssetName(Item.PackageName));
			ChangedAssets.Add(Item.Asset);
		}
	}
	for (FEntry& Entry : Entries)
	{
		if (CleanBefore.Contains(Entry.File.Location.PackageName) && IsPackageDirty(Entry.File.Location.PackageName))
		{
			Entry.bChangedByApply = true;
		}
	}

	const FText AssetNames = FText::FromString(JoinNames(ChangedNames));
	if (bSave && ChangedAssets.Num() > 0)
	{
		TArray<UPackage*> Packages;
		for (UObject* Asset : ChangedAssets)
		{
			if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
			{
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
			}
			Packages.Add(Asset->GetPackage());
		}
		// Checks the files out first if source control needs it. Saving works the merges out again (OnPackageSaved).
		FEditorFileUtils::PromptForCheckoutAndSave(Packages, true, false);
		TArray<FString> NotSaved;
		for (const UPackage* Package : Packages)
		{
			if (Package->IsDirty())
			{
				NotSaved.Add(MergeAssetName(Package->GetName()));
			}
		}
		if (NotSaved.IsEmpty())
		{
			UJsonBlueprintSyncSubsystem::Notify(FText::Format(LOCTEXT("MergeAppliedSaved", "Applied {0} merge row(s) to {1} and saved."),
				FText::AsNumber(NumApplied), AssetNames), EMergeNotice::Success);
		}
		else
		{
			// With -unattended the editor cancels every save that may need a source-control checkout.
			UJsonBlueprintSyncSubsystem::Notify(FText::Format(LOCTEXT("MergeAppliedNotSaved", "Applied {0} merge row(s) to {1}, but {2} was not saved{3}. Save it to finish the merge; Ctrl+Z reverts the apply."),
				FText::AsNumber(NumApplied), AssetNames, FText::FromString(JoinNames(NotSaved)),
				FApp::IsUnattended() ? LOCTEXT("UnattendedNoSave", " (the editor doesn't save from commands in -unattended runs)") : FText::GetEmpty()), EMergeNotice::Warning);
		}
	}
	else if (NumApplied > 0)
	{
		UJsonBlueprintSyncSubsystem::Notify(FText::Format(LOCTEXT("MergeApplied", "Applied {0} merge row(s) to {1}. Not saved; Ctrl+Z reverts."),
			FText::AsNumber(NumApplied), AssetNames), EMergeNotice::Success);
	}
	else if (NumFailed == 0 && Items.Num() > 0)
	{
		UJsonBlueprintSyncSubsystem::Notify(LOCTEXT("MergeNothingChanged", "Nothing to change: the loaded assets already have the merged values. Save them to finish the merge."),
			EMergeNotice::Success);
	}
	if (NumFailed > 0)
	{
		UJsonBlueprintSyncSubsystem::Notify(FText::Format(LOCTEXT("MergeRowsFailed", "{0} merge row(s) could not be applied; the others were. The Asset Merges tab shows why."),
			FText::AsNumber(NumFailed)), EMergeNotice::Warning);
	}
	if (Skipped.Num() > 0)
	{
		UJsonBlueprintSyncSubsystem::Notify(FText::Format(LOCTEXT("MergeSkipped", "Not applied: {0}."), FText::FromString(JoinNames(Skipped))), EMergeNotice::Warning);
	}
	OnChanged.Broadcast();
	return NumApplied;
}

bool UJsonBlueprintMergeSubsystem::DeleteMergeFile(const FString& PackageName)
{
	const FEntry* Entry = FindEntry(PackageName);
	if (!Entry)
	{
		return false;
	}
	const FString MergedFile = Entry->File.Location.MergedFile;
	if (FPaths::FileExists(MergedFile) && !IFileManager::Get().Delete(*MergedFile, false, true, true))
	{
		UJsonBlueprintSyncSubsystem::Notify(FText::Format(LOCTEXT("MergeDeleteFailed", "JSON Sync: can't delete {0}. Is it open in another program?"),
			FText::FromString(MergedFile)), EMergeNotice::Error);
		return false;
	}
	UE_LOG(LogJsonBlueprintSync, Display, TEXT("Deleted %s"), *MergedFile);

	// The folders left empty, up to the merge folder. Only empty ones: the folder of /Game/A may also hold the merge of /Game/A/B.
	const FString Root = NormalizedDir(GetMergeDir());
	for (FString Folder = NormalizedDir(FPaths::GetPath(MergedFile)); Folder.StartsWith(Root + TEXT("/")); Folder = FPaths::GetPath(Folder))
	{
		bool bEmpty = true;
		IFileManager::Get().IterateDirectory(*Folder, [&bEmpty](const TCHAR*, bool)
		{
			bEmpty = false;
			return false;
		});
		if (!bEmpty || !IFileManager::Get().DeleteDirectory(*Folder, false, false))
		{
			break;
		}
	}

	Entries.RemoveAll([&PackageName](const FEntry& Each) { return Each.File.Location.PackageName == PackageName; });
	OnChanged.Broadcast();
	return true;
}

void UJsonBlueprintMergeSubsystem::OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext Context)
{
	if (!Package || Context.IsFromAutoSave() || Context.IsProceduralSave() || Context.IsCooking())
	{
		return;
	}
	const FString PackageName = Package->GetName();
	// Also a package whose merge file isn't listed yet, e.g. one that was not found before it was saved for the first time.
	if (!FindEntry(PackageName) && !FPaths::FileExists(GetMergeDir() / PackageName / TEXT("merged.txt")))
	{
		return;
	}
	SavedPackages.Add(PackageName);
	if (!SavedTicker.IsValid())
	{
		SavedTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UJsonBlueprintMergeSubsystem::ProcessSaved));
	}
}

bool UJsonBlueprintMergeSubsystem::ProcessSaved(float DeltaTime)
{
	SavedTicker.Reset();
	const TSet<FString> Packages = MoveTemp(SavedPackages);
	SavedPackages.Reset();
	for (const FString& PackageName : Packages)
	{
		// The saved file has what was applied now, so the reader sees it.
		if (FEntry* Entry = FindMutableEntry(PackageName))
		{
			Entry->Applied.Reset();
			Entry->Failures.Reset();
			Entry->bChangedByApply = false;
		}
	}
	Check(Packages);
	OnChanged.Broadcast();
	return false;
}

void UJsonBlueprintMergeSubsystem::LogEntries() const
{
	UE_LOG(LogJsonBlueprintSync, Display, TEXT("%d merge file(s) in %s"), Entries.Num(), *GetMergeDir());
	for (const FEntry& Entry : Entries)
	{
		const FString Message = GetStatusMessage(Entry);
		UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s: %s%s%s"), *Entry.File.Location.PackageName, LexToString(GetStatus(Entry)),
			Message.IsEmpty() ? TEXT("") : TEXT(". "), *Message);
		for (const FMergeRow& Row : Entry.File.Rows)
		{
			const FString Key = GetRowKey(Row);
			const FString* Failure = Entry.Failures.Find(Key);
			const FString Note = IsAppliedNotSaved(Entry, Key) ? TEXT(" (applied, not saved)")
				: Failure ? FString::Printf(TEXT(" (failed: %s)"), **Failure)
				: FString();
			UE_LOG(LogJsonBlueprintSync, Display, TEXT("  %s [%s] %s: %s -> %s%s%s%s"), LexToString(Row.Action), *Row.Object, *Row.Path,
				*DescribeValue(Row.Current), *DescribeValue(Row.Merged), Row.Reason.IsEmpty() ? TEXT("") : TEXT(". "), *Row.Reason, *Note);
		}
	}
}

void UJsonBlueprintMergeSubsystem::ApplyCommand(const TArray<FString>& Args)
{
	if (Args.IsEmpty())
	{
		UE_LOG(LogJsonBlueprintSync, Warning, TEXT("Usage: JsonBlueprintSync.ApplyMerge <package, e.g. /Game/A/BP_B> [save]"));
		return;
	}
	const FString& PackageName = Args[0];
	const bool bSave = Args.ContainsByPredicate([](const FString& Arg) { return Arg.Equals(TEXT("save"), ESearchCase::IgnoreCase); });

	Check(TSet<FString>{ PackageName });
	const FEntry* Entry = FindEntry(PackageName);
	if (!Entry)
	{
		UE_LOG(LogJsonBlueprintSync, Warning, TEXT("There's no merge for %s in %s."), *PackageName, *GetMergeDir());
		OnChanged.Broadcast();
		return;
	}
	TArray<FString> Keys;
	for (const FMergeRow& Row : Entry->File.Rows)
	{
		Keys.Add(GetRowKey(Row));
	}
	const int32 NumRows = Keys.Num();
	const int32 NumApplied = Apply({ { PackageName, MoveTemp(Keys) } }, bSave);
	UE_LOG(LogJsonBlueprintSync, Display, TEXT("Applied %d of %d row(s) to %s."), NumApplied, NumRows, *PackageName);
}

#undef LOCTEXT_NAMESPACE
