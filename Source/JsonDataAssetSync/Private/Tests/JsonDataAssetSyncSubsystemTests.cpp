#include "JsonDataAssetSync.h"
#include "JsonDataAssetSyncSettings.h"
#include "JsonDataAssetSyncTestAsset.h"
#include "../JsonDataAssetSyncSubsystem.h"
#include "../SyncState.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

/**
 * Tests of the editor subsystem: what a save and a sync do to the asset, its JSON and the cache. They run on the editor's own
 * subsystem, with its state swapped for an empty one, a JSON folder and cache under Saved/JsonDataAssetSyncTests, and data assets
 * saved under /Game/JsonDataAssetSyncTests; all of it is deleted and restored afterwards. A manual Apply JSON leaves an undo step
 * for a deleted test asset, so run these headless (see Docs/ManualTests.md, section 0).
 */
namespace JsonDataAssetSync::Tests
{
	constexpr EAutomationTestFlags SubsystemTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FSubsystemScope
	{
		UJsonDataAssetSyncSubsystem* Subsystem = nullptr;
		FString JsonRoot;

		FSubsystemScope(FAutomationTestBase& InTest, const TCHAR* Name, EJsonSyncMode Mode)
			: Test(InTest)
		{
			Subsystem = GEditor ? GEditor->GetEditorSubsystem<UJsonDataAssetSyncSubsystem>() : nullptr;
			if (!Subsystem)
			{
				Test.AddError(TEXT("No JsonDataAssetSync subsystem"));
				return;
			}
			// Every sync reports the project's data assets, which have no JSON in the test folder.
			Test.AddExpectedMessagePlain(TEXT("no JSON file"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);

			Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("JsonDataAssetSyncTests") / Name);
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
			JsonRoot = Dir / TEXT("Json");

			UJsonDataAssetSyncSettings* Settings = GetMutableDefault<UJsonDataAssetSyncSettings>();
			SavedJsonDir = Settings->JsonDir;
			SavedMode = Settings->Mode;
			SavedExcluded = Settings->ExcludedFolders;
			Settings->JsonDir = JsonRoot;
			Settings->Mode = Mode;
			Settings->ExcludedFolders.Reset();

			SavedCache = MoveTemp(Subsystem->Cache);
			SavedPending = MoveTemp(Subsystem->Pending);
			SavedResolved = MoveTemp(Subsystem->Resolved);
			SavedUndone = MoveTemp(Subsystem->Undone);
			Subsystem->Cache = FSyncCache();
			Subsystem->Cache.Load(Dir / TEXT("Cache"));
			Subsystem->Pending.Reset();
			Subsystem->Resolved.Reset();
			Subsystem->Undone.Reset();

			// With -unattended, FEditorFileUtils::PromptForCheckoutAndSave cancels instead of saving; an unattended script saves directly,
			// as the editor's own Python scripts do. The automatic apply and Apply and Save depend on it.
			SavedUnattendedScript = GIsRunningUnattendedScript;
			GIsRunningUnattendedScript = true;
		}

		~FSubsystemScope()
		{
			if (!Subsystem)
			{
				return;
			}
			for (UJsonDataAssetSyncTestAsset* Asset : Assets)
			{
				UPackage* Package = Asset->GetPackage();
				ResetLoaders(Package);
				// The asset registry learns of the deletion from its folder watcher, as it learned of the save; telling it first makes the
				// watcher's late report of the save warn that a deleted package changed on disk.
				IFileManager::Get().Delete(*GetFilename(Asset), false, true, true);
				Package->SetDirtyFlag(false);
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->MarkAsGarbage();
			}
			IFileManager::Get().DeleteDirectory(*FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("JsonDataAssetSyncTests")), false, true);

			GIsRunningUnattendedScript = SavedUnattendedScript;
			Subsystem->Cache = MoveTemp(SavedCache);
			Subsystem->Pending = MoveTemp(SavedPending);
			Subsystem->Resolved = MoveTemp(SavedResolved);
			Subsystem->Undone = MoveTemp(SavedUndone);

			UJsonDataAssetSyncSettings* Settings = GetMutableDefault<UJsonDataAssetSyncSettings>();
			Settings->JsonDir = SavedJsonDir;
			Settings->Mode = SavedMode;
			Settings->ExcludedFolders = SavedExcluded;
			IFileManager::Get().DeleteDirectory(*Dir, false, true);

			Subsystem->ShowPendingNotification();
			Subsystem->OnPendingChanged.Broadcast();
		}

		bool IsValid() const { return Subsystem != nullptr; }

		/** A new data asset under /Game/JsonDataAssetSyncTests, in memory only until Save. */
		UJsonDataAssetSyncTestAsset* CreateAsset(const TCHAR* Name, double Speed)
		{
			const FString ShortName = FString::Printf(TEXT("%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			UPackage* Package = CreatePackage(*(TEXT("/Game/JsonDataAssetSyncTests/") + ShortName));
			UJsonDataAssetSyncTestAsset* Asset = NewObject<UJsonDataAssetSyncTestAsset>(Package, FName(*ShortName), RF_Public | RF_Standalone | RF_Transactional);
			Asset->Speed = Speed;
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			Assets.Add(Asset);
			return Asset;
		}

		/** Saves the asset as the editor does, which runs the subsystem's save handler. */
		bool Save(UJsonDataAssetSyncTestAsset* Asset)
		{
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			return UPackage::SavePackage(Asset->GetPackage(), Asset, *GetFilename(Asset), Args);
		}

		/** An unsaved edit, as in the Details panel without a save. */
		static void Edit(UJsonDataAssetSyncTestAsset* Asset, int32 Count)
		{
			Asset->Count = Count;
			Asset->GetPackage()->MarkPackageDirty();
		}

		static FString GetPackageName(const UObject* Asset) { return Asset->GetPackage()->GetName(); }

		FString GetJsonPath(const UObject* Asset) const { return JsonDataAssetSync::GetJsonPath(JsonRoot, GetPackageName(Asset)); }

		FString ReadJson(const UObject* Asset) const
		{
			FString Text;
			FFileHelper::LoadFileToString(Text, *GetJsonPath(Asset));
			return Text;
		}

		/** The JSON file's values, e.g. Speed -> 2.000000; empty if it cannot be read. */
		TMap<FString, FString> ReadValues(const UObject* Asset) const
		{
			FAssetDefaults Doc;
			FString Error;
			return LoadFile(GetJsonPath(Asset), Doc, Error) ? Doc.Self : TMap<FString, FString>();
		}

		/** Writes the asset's JSON with these values, as a git pull or a teammate's edit would. */
		bool WriteValues(UJsonDataAssetSyncTestAsset* Asset, const TMap<FString, FString>& Values) const
		{
			FAssetDefaults Doc = Capture(Asset);
			Doc.Self = Values;
			return WriteText(Asset, ToJson(Doc));
		}

		bool WriteText(const UObject* Asset, const FString& Text) const
		{
			return FFileHelper::SaveStringToFile(Text, *GetJsonPath(Asset), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		void Sync() const { Subsystem->SyncFromJson(); }

		bool IsPending(const UObject* Asset) const { return Subsystem->Pending.Contains(GetPackageName(Asset)); }

		const FSyncEntry* FindEntry(const UObject* Asset) const { return FindEntry(GetPackageName(Asset)); }
		const FSyncEntry* FindEntry(const FString& PackageName) const { return Subsystem->Cache.Find(PackageName); }
		void SetEntry(const FString& PackageName, const FSyncEntry& Entry) const { Subsystem->Cache.Set(PackageName, Entry); }

		/** The cache records the JSON on disk and the saved asset as the last sync. */
		bool IsSynced(const UObject* Asset) const
		{
			const FSyncEntry* Entry = FindEntry(Asset);
			const FString Text = ReadJson(Asset);
			return Entry && !Entry->bPending && !IsPending(Asset) && !Text.IsEmpty() && Entry->JsonHash == HashText(Text)
				&& Entry->AssetHash == LexToString(Asset->GetPackage()->GetSavedHash());
		}

		bool GetView(const UObject* Asset, UJsonDataAssetSyncSubsystem::FPendingView& OutView) const
		{
			return Subsystem->GetPendingView(GetPackageName(Asset), OutView);
		}

		/** Keys of the view, each as Key:Change with Change one of Json, Asset, Both. */
		static FString DescribeKeys(const UJsonDataAssetSyncSubsystem::FPendingView& View)
		{
			TArray<FString> Parts;
			for (const FKeyDiff& Diff : View.Keys)
			{
				const TCHAR* Change = Diff.Change == EKeyChange::Json ? TEXT("Json") : Diff.Change == EKeyChange::Asset ? TEXT("Asset") : TEXT("Both");
				Parts.Add(Diff.Key + TEXT(":") + Change);
			}
			Parts.Sort();
			return FString::Join(Parts, TEXT(","));
		}

		/** Pretends the given JSON and cache entry are the last sync, e.g. after a pull that brought only the .uasset. */
		void SetLastSync(const UObject* Asset, const FString& Text, const FSyncEntry& Entry) const
		{
			WriteText(Asset, Text);
			SetEntry(GetPackageName(Asset), Entry);
		}

	private:
		static FString GetFilename(const UObject* Asset)
		{
			return FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(GetPackageName(Asset), FPackageName::GetAssetPackageExtension()));
		}

		FAutomationTestBase& Test;
		FString Dir;
		TArray<UJsonDataAssetSyncTestAsset*> Assets;

		bool SavedUnattendedScript = false;
		FString SavedJsonDir;
		EJsonSyncMode SavedMode = EJsonSyncMode::Automatic;
		TArray<FDirectoryPath> SavedExcluded;
		FSyncCache SavedCache;
		TMap<FString, UJsonDataAssetSyncSubsystem::FPending> SavedPending;
		TMap<FString, UJsonDataAssetSyncSubsystem::FResolved> SavedResolved;
		TMap<FString, UJsonDataAssetSyncSubsystem::FResolved> SavedUndone;
	};

	const FString Speed1 = TEXT("1.000000");
	const FString Speed2 = TEXT("2.000000");
	const FString Speed3 = TEXT("3.000000");
}

using namespace JsonDataAssetSync;
using namespace JsonDataAssetSync::Tests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncSubsystemSaveTest, "JsonDataAssetSync.Subsystem.WriteOnSave", SubsystemTestFlags)
bool FJsonDataAssetSyncSubsystemSaveTest::RunTest(const FString&)
{
	FSubsystemScope Scope(*this, TEXT("WriteOnSave"), EJsonSyncMode::Automatic);
	if (!Scope.IsValid())
	{
		return false;
	}
	UJsonDataAssetSyncTestAsset* Asset = Scope.CreateAsset(TEXT("DA_Save"), 1.0);
	TestTrue(TEXT("Saved"), Scope.Save(Asset));
	TestEqual(TEXT("The save wrote the JSON"), Scope.ReadValues(Asset).FindRef(TEXT("Speed")), Speed1);
	TestFalse(TEXT("A class default is not written"), Scope.ReadValues(Asset).Contains(TEXT("Count")));
	TestTrue(TEXT("The save is the last sync"), Scope.IsSynced(Asset));

	Asset->Speed = 2.0;
	TestTrue(TEXT("Saved again"), Scope.Save(Asset));
	TestEqual(TEXT("The next save writes the new value"), Scope.ReadValues(Asset).FindRef(TEXT("Speed")), Speed2);
	TestTrue(TEXT("And records it"), Scope.IsSynced(Asset));

	Scope.Sync();
	TestFalse(TEXT("A sync finds nothing to do"), Scope.IsPending(Asset));
	TestEqual(TEXT("Nor changes the asset"), Asset->Speed, 2.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncSubsystemAutomaticTest, "JsonDataAssetSync.Subsystem.Automatic", SubsystemTestFlags)
bool FJsonDataAssetSyncSubsystemAutomaticTest::RunTest(const FString&)
{
	FSubsystemScope Scope(*this, TEXT("Automatic"), EJsonSyncMode::Automatic);
	if (!Scope.IsValid())
	{
		return false;
	}
	UJsonDataAssetSyncTestAsset* Asset = Scope.CreateAsset(TEXT("DA_Auto"), 1.0);
	TestTrue(TEXT("Saved"), Scope.Save(Asset));

	// Only the JSON changed: applied and saved.
	TestTrue(TEXT("JSON changed"), Scope.WriteValues(Asset, { { TEXT("Speed"), Speed2 } }));
	Scope.Sync();
	TestEqual(TEXT("Applied"), Asset->Speed, 2.0);
	TestFalse(TEXT("And saved"), Asset->GetPackage()->IsDirty());
	TestEqual(TEXT("The JSON keeps its value"), Scope.ReadValues(Asset).FindRef(TEXT("Speed")), Speed2);
	TestTrue(TEXT("Recorded as the last sync"), Scope.IsSynced(Asset));

	// The asset has unsaved edits: an automatic apply would save them, so it is listed instead, and without a base (anything may
	// have been edited) every differing key is a conflict.
	Scope.Edit(Asset, 5);
	TestTrue(TEXT("JSON changed again"), Scope.WriteValues(Asset, { { TEXT("Speed"), Speed3 } }));
	Scope.Sync();
	TestTrue(TEXT("Unsaved edits: listed"), Scope.IsPending(Asset));
	TestEqual(TEXT("Not applied"), Asset->Speed, 2.0);
	TestEqual(TEXT("The edit is kept"), Asset->Count, 5);
	UJsonDataAssetSyncSubsystem::FPendingView View;
	TestTrue(TEXT("View"), Scope.GetView(Asset, View));
	TestFalse(TEXT("No base"), View.bHasBase);
	TestEqual(TEXT("Every key a conflict"), FSubsystemScope::DescribeKeys(View), FString(TEXT("Count:Both,Speed:Both")));

	// Write JSON: the asset's values win.
	TestTrue(TEXT("Write JSON"), Scope.Subsystem->OverwriteJson(FSubsystemScope::GetPackageName(Asset)));
	TestFalse(TEXT("No longer listed"), Scope.IsPending(Asset));
	const TMap<FString, FString> Written = Scope.ReadValues(Asset);
	TestEqual(TEXT("The JSON has the asset's Speed"), Written.FindRef(TEXT("Speed")), Speed2);
	TestEqual(TEXT("And its Count"), Written.FindRef(TEXT("Count")), FString(TEXT("5")));
	TestTrue(TEXT("Saved"), Scope.Save(Asset));
	TestTrue(TEXT("The save is the last sync"), Scope.IsSynced(Asset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncSubsystemManualTest, "JsonDataAssetSync.Subsystem.Manual", SubsystemTestFlags)
bool FJsonDataAssetSyncSubsystemManualTest::RunTest(const FString&)
{
	FSubsystemScope Scope(*this, TEXT("Manual"), EJsonSyncMode::Manual);
	if (!Scope.IsValid())
	{
		return false;
	}
	UJsonDataAssetSyncTestAsset* Asset = Scope.CreateAsset(TEXT("DA_Manual"), 1.0);
	TestTrue(TEXT("Saved"), Scope.Save(Asset));

	TestTrue(TEXT("JSON changed"), Scope.WriteValues(Asset, { { TEXT("Speed"), Speed2 } }));
	Scope.Sync();
	TestTrue(TEXT("Listed"), Scope.IsPending(Asset));
	TestEqual(TEXT("Not applied"), Asset->Speed, 1.0);
	TestFalse(TEXT("Not dirtied"), Asset->GetPackage()->IsDirty());
	UJsonDataAssetSyncSubsystem::FPendingView View;
	TestTrue(TEXT("View"), Scope.GetView(Asset, View));
	TestTrue(TEXT("Base known"), View.bHasBase);
	TestEqual(TEXT("A JSON change"), FSubsystemScope::DescribeKeys(View), FString(TEXT("Speed:Json")));

	// Apply and Save.
	TestTrue(TEXT("Applied"), Scope.Subsystem->ApplyJson(FSubsystemScope::GetPackageName(Asset), { TEXT("Speed") }, true));
	TestEqual(TEXT("The JSON value"), Asset->Speed, 2.0);
	TestFalse(TEXT("Saved"), Asset->GetPackage()->IsDirty());
	TestFalse(TEXT("No longer listed"), Scope.IsPending(Asset));
	TestTrue(TEXT("Recorded as the last sync"), Scope.IsSynced(Asset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncSubsystemSaveGuardTest, "JsonDataAssetSync.Subsystem.SaveGuard", SubsystemTestFlags)
bool FJsonDataAssetSyncSubsystemSaveGuardTest::RunTest(const FString&)
{
	FSubsystemScope Scope(*this, TEXT("SaveGuard"), EJsonSyncMode::Automatic);
	if (!Scope.IsValid())
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("did not write"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	UJsonDataAssetSyncTestAsset* Asset = Scope.CreateAsset(TEXT("DA_Guard"), 1.0);
	TestTrue(TEXT("Saved"), Scope.Save(Asset));

	// The JSON changes with the editor open (a git pull, no sync since), then the asset is edited and saved.
	TestTrue(TEXT("JSON changed"), Scope.WriteValues(Asset, { { TEXT("Speed"), Speed3 } }));
	const FString Pulled = Scope.ReadJson(Asset);
	Scope.Edit(Asset, 7);
	TestTrue(TEXT("Saved with the edit"), Scope.Save(Asset));
	TestEqual(TEXT("The JSON is not overwritten"), Scope.ReadJson(Asset), Pulled);
	TestTrue(TEXT("Listed"), Scope.IsPending(Asset));
	UJsonDataAssetSyncSubsystem::FPendingView View;
	TestTrue(TEXT("View"), Scope.GetView(Asset, View));
	TestFalse(TEXT("No base: the asset was edited after the JSON changed"), View.bHasBase);
	TestEqual(TEXT("Every key a conflict"), FSubsystemScope::DescribeKeys(View), FString(TEXT("Count:Both,Speed:Both")));

	// Take Speed from the JSON, keep the edited Count; the next save writes both.
	TestTrue(TEXT("Applied"), Scope.Subsystem->ApplyJson(FSubsystemScope::GetPackageName(Asset), { TEXT("Speed") }));
	TestEqual(TEXT("Speed from the JSON"), Asset->Speed, 3.0);
	TestEqual(TEXT("Count kept"), Asset->Count, 7);
	TestTrue(TEXT("Not saved"), Asset->GetPackage()->IsDirty());
	TestFalse(TEXT("No longer listed"), Scope.IsPending(Asset));
	TestTrue(TEXT("Saved"), Scope.Save(Asset));
	const TMap<FString, FString> Written = Scope.ReadValues(Asset);
	TestEqual(TEXT("The JSON has Speed"), Written.FindRef(TEXT("Speed")), Speed3);
	TestEqual(TEXT("And the kept Count"), Written.FindRef(TEXT("Count")), FString(TEXT("7")));
	TestTrue(TEXT("Recorded as the last sync"), Scope.IsSynced(Asset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncSubsystemAssetChangedTest, "JsonDataAssetSync.Subsystem.AssetChanged", SubsystemTestFlags)
bool FJsonDataAssetSyncSubsystemAssetChangedTest::RunTest(const FString&)
{
	FSubsystemScope Scope(*this, TEXT("AssetChanged"), EJsonSyncMode::Automatic);
	if (!Scope.IsValid())
	{
		return false;
	}
	UJsonDataAssetSyncTestAsset* Asset = Scope.CreateAsset(TEXT("DA_Changed"), 1.0);
	TestTrue(TEXT("Saved"), Scope.Save(Asset));
	const FString OldText = Scope.ReadJson(Asset);
	const FSyncEntry* Entry = Scope.FindEntry(Asset);
	if (!TestNotNull(TEXT("Recorded"), Entry))
	{
		return false;
	}
	const FSyncEntry OldEntry = *Entry;

	// A pull that brought a new .uasset without its JSON.
	Asset->Speed = 2.0;
	TestTrue(TEXT("Saved the new value"), Scope.Save(Asset));
	Scope.SetLastSync(Asset, OldText, OldEntry);
	Scope.Sync();
	TestTrue(TEXT("Listed"), Scope.IsPending(Asset));
	TestEqual(TEXT("Not applied, even in Automatic mode"), Asset->Speed, 2.0);
	UJsonDataAssetSyncSubsystem::FPendingView View;
	TestTrue(TEXT("View"), Scope.GetView(Asset, View));
	TestTrue(TEXT("The JSON is only out of date"), View.IsJsonOutOfDate());
	TestEqual(TEXT("An asset change"), FSubsystemScope::DescribeKeys(View), FString(TEXT("Speed:Asset")));
	TestEqual(TEXT("The JSON is not written"), Scope.ReadJson(Asset), OldText);

	// Saving the asset is the natural fix: it writes the JSON.
	TestTrue(TEXT("Saved"), Scope.Save(Asset));
	TestFalse(TEXT("No longer listed"), Scope.IsPending(Asset));
	TestEqual(TEXT("The JSON has the asset's value"), Scope.ReadValues(Asset).FindRef(TEXT("Speed")), Speed2);
	TestTrue(TEXT("Recorded as the last sync"), Scope.IsSynced(Asset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncSubsystemMissingTest, "JsonDataAssetSync.Subsystem.MissingFiles", SubsystemTestFlags)
bool FJsonDataAssetSyncSubsystemMissingTest::RunTest(const FString&)
{
	FSubsystemScope Scope(*this, TEXT("MissingFiles"), EJsonSyncMode::Manual);
	if (!Scope.IsValid())
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("no data asset for this JSON file"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	// A listed asset whose JSON is deleted: nothing left to review.
	UJsonDataAssetSyncTestAsset* Asset = Scope.CreateAsset(TEXT("DA_NoJson"), 1.0);
	TestTrue(TEXT("Saved"), Scope.Save(Asset));
	TestTrue(TEXT("JSON changed"), Scope.WriteValues(Asset, { { TEXT("Speed"), Speed2 } }));
	Scope.Sync();
	TestTrue(TEXT("Listed"), Scope.IsPending(Asset));
	TestTrue(TEXT("JSON deleted"), IFileManager::Get().Delete(*Scope.GetJsonPath(Asset)));
	Scope.Sync();
	TestFalse(TEXT("No longer listed"), Scope.IsPending(Asset));
	TestNull(TEXT("Forgotten"), Scope.FindEntry(Asset));
	TestEqual(TEXT("The asset is unchanged"), Asset->Speed, 1.0);

	// A JSON whose asset is gone, listed before: forgotten, and the file is only reported.
	const FString Gone = TEXT("/Game/JsonDataAssetSyncTests/DA_Gone_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString GonePath = GetJsonPath(Scope.JsonRoot, Gone);
	TestTrue(TEXT("Write a JSON without its asset"), FFileHelper::SaveStringToFile(TEXT("{\"schema\": 2, \"asset\": \"") + Gone + TEXT("\"}\n"), *GonePath));
	Scope.SetEntry(Gone, { TEXT("old"), TEXT("old"), true });
	Scope.Sync();
	TestNull(TEXT("Its entry is removed"), Scope.FindEntry(Gone));
	TestFalse(TEXT("Not listed"), Scope.Subsystem->GetPendingPackages().Contains(Gone));
	TestTrue(TEXT("The file is kept"), FPaths::FileExists(GonePath));
	return true;
}

#endif
