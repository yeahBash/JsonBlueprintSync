#include "JsonBlueprintSyncCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/DataAsset.h"
#include "FileHelpers.h"
#include "JsonBlueprintSync.h"
#include "JsonBlueprintSyncModule.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(JsonBlueprintSyncCommandlet)

namespace
{
	using namespace JsonBlueprintSync;

	/** Package names of the synced data assets under Dir (not those in an excluded folder), sorted. */
	TArray<FString> FindSyncedPackages(const FString& Dir)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.SearchAllAssets(true);
		Registry.ScanPathsSynchronous({ Dir }, true);

		FARFilter Filter;
		Filter.ClassPaths.Add(UDataAsset::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(FName(*Dir));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);

		TArray<FString> Packages;
		for (const FAssetData& Asset : Assets)
		{
			// Only a package's main asset is synced.
			if (Asset.AssetName.ToString() != FPackageName::GetShortName(Asset.PackageName))
			{
				continue;
			}
			if (IsSyncedPackage(Asset.PackageName.ToString()))
			{
				Packages.AddUnique(Asset.PackageName.ToString());
			}
		}
		Packages.Sort();
		return Packages;
	}

	int32 RunExport(const FString& JsonRoot, const FString& Dir)
	{
		int32 Failures = 0;
		int32 Written = 0;
		const TArray<FString> Packages = FindSyncedPackages(Dir);
		TSet<FString> Expected;
		for (const FString& PackageName : Packages)
		{
			UObject* Asset = LoadAsset(PackageName);
			if (!Asset)
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: cannot load the asset"), *PackageName);
				++Failures;
				continue;
			}
			const FString Path = GetJsonPath(JsonRoot, PackageName);
			Expected.Add(Path);

			bool bWritten = false;
			if (!SaveFileIfChanged(Path, Capture(Asset), bWritten))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: cannot write the file"), *Path);
				++Failures;
				continue;
			}
			if (bWritten)
			{
				UE_LOG(LogJsonBlueprintSync, Display, TEXT("Wrote %s"), *Path);
				++Written;
			}
		}
		for (const FString& File : FindJsonFiles(JsonRoot, Dir))
		{
			if (!Expected.Contains(File))
			{
				UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: no synced asset for this file"), *File);
			}
		}
		UE_LOG(LogJsonBlueprintSync, Display, TEXT("Export: %d assets, %d files written, %d failures"), Packages.Num(), Written, Failures);
		return Failures > 0 ? 1 : 0;
	}

	int32 RunVerify(const FString& JsonRoot, const FString& Dir)
	{
		int32 Failures = 0;
		const TArray<FString> Packages = FindSyncedPackages(Dir);
		TSet<FString> Expected;
		for (const FString& PackageName : Packages)
		{
			const FString Path = GetJsonPath(JsonRoot, PackageName);
			Expected.Add(Path);

			UObject* Asset = LoadAsset(PackageName);
			if (!Asset)
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: cannot load the asset"), *PackageName);
				++Failures;
				continue;
			}
			FAssetDefaults OnDisk;
			FString Error;
			if (!LoadFile(Path, OnDisk, Error))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: %s"), *Path, Error.IsEmpty() ? TEXT("missing; run -export") : *Error);
				++Failures;
				continue;
			}
			FString Text;
			FFileHelper::LoadFileToString(Text, *Path);
			if (!NormalizeLineEndings(Text).Equals(ToJson(Capture(Asset)), ESearchCase::CaseSensitive))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: differs from %s; run -export or -import"), *Path, *PackageName);
				++Failures;
			}
		}
		for (const FString& File : FindJsonFiles(JsonRoot, Dir))
		{
			if (!Expected.Contains(File))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: no synced asset for this file"), *File);
				++Failures;
			}
		}
		UE_LOG(LogJsonBlueprintSync, Display, TEXT("Verify: %d assets, %d failures"), Packages.Num(), Failures);
		return Failures > 0 ? 1 : 0;
	}

	int32 RunImport(const FString& JsonRoot, const FString& Dir, bool bSave)
	{
		// Also makes sure the asset registry knows the assets, so that loading resolves Blueprint classes.
		const TSet<FString> Synced(FindSyncedPackages(Dir));

		int32 Failures = 0;
		struct FEntry
		{
			UObject* Asset;
			FAssetDefaults Doc;
		};
		TArray<FEntry> Entries;
		for (const FString& File : FindJsonFiles(JsonRoot, Dir))
		{
			FString PackageName;
			if (!GetPackageName(JsonRoot, File, PackageName))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: not a valid asset path"), *File);
				++Failures;
				continue;
			}
			FAssetDefaults Doc;
			FString Error;
			if (!LoadFile(File, Doc, Error))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: %s"), *File, *Error);
				++Failures;
				continue;
			}
			UObject* Asset = FPackageName::DoesPackageExist(PackageName) ? LoadAsset(PackageName) : nullptr;
			if (!Asset)
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: no asset %s"), *File, *PackageName);
				++Failures;
				continue;
			}
			if (!Synced.Contains(PackageName))
			{
				UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s: skipped; %s is not synced (not a data asset, or in an excluded folder)"), *File, *PackageName);
				continue;
			}
			Entries.Add({ Asset, MoveTemp(Doc) });
		}

		int32 Changed = 0;
		TArray<UPackage*> Dirty;
		for (const FEntry& Entry : Entries)
		{
			const FString PackageName = Entry.Asset->GetPackage()->GetName();
			const FApplyResult Result = Import(Entry.Asset, Entry.Doc);
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
				++Failures;
				continue;
			}
			if (Result.Changed.Num() > 0)
			{
				++Changed;
				Dirty.AddUnique(Entry.Asset->GetPackage());
				UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s: changed %s"), *PackageName, *FString::Join(Result.Changed, TEXT(", ")));
			}
		}

		if (bSave)
		{
			if (Dirty.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(Dirty, true))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("Saving the changed packages failed"));
				++Failures;
			}
			UE_LOG(LogJsonBlueprintSync, Display, TEXT("Saved %d packages"), Dirty.Num());
		}
		UE_LOG(LogJsonBlueprintSync, Display, TEXT("Import: %d files, %d assets changed, %d failures"), Entries.Num(), Changed, Failures);
		return Failures > 0 ? 1 : 0;
	}
}

UJsonBlueprintSyncCommandlet::UJsonBlueprintSyncCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
	HelpDescription = TEXT("Syncs data asset values with JSON files. -import applies every JSON file whatever the mode; data assets in "
		"excluded folders are skipped.");
	HelpUsage = TEXT("-run=JsonBlueprintSync -export|-import|-verify [-dir=/Game/...] [-jsondir=<path>] [-nosave]");
}

int32 UJsonBlueprintSyncCommandlet::Main(const FString& Params)
{
	const bool bExport = FParse::Param(*Params, TEXT("export"));
	const bool bImport = FParse::Param(*Params, TEXT("import"));
	const bool bVerify = FParse::Param(*Params, TEXT("verify"));
	if (int32(bExport) + int32(bImport) + int32(bVerify) != 1)
	{
		UE_LOG(LogJsonBlueprintSync, Error, TEXT("Pass exactly one of -export, -import, -verify. Usage: %s"), *HelpUsage);
		return 1;
	}

	FString Dir = TEXT("/Game");
	FParse::Value(*Params, TEXT("dir="), Dir);
	Dir.RemoveFromEnd(TEXT("/"));
	if (!FPackageName::IsValidPath(Dir))
	{
		UE_LOG(LogJsonBlueprintSync, Error, TEXT("-dir=%s is not a content path such as /Game/Features"), *Dir);
		return 1;
	}

	FString JsonRoot = GetDefaultJsonRoot();
	FString JsonDirOverride;
	if (FParse::Value(*Params, TEXT("jsondir="), JsonDirOverride))
	{
		JsonRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), JsonDirOverride);
		FPaths::NormalizeDirectoryName(JsonRoot);
	}
	UE_LOG(LogJsonBlueprintSync, Display, TEXT("Assets under %s, JSON under %s"), *Dir, *JsonRoot);

	if (bExport)
	{
		return RunExport(JsonRoot, Dir);
	}
	if (bVerify)
	{
		return RunVerify(JsonRoot, Dir);
	}
	return RunImport(JsonRoot, Dir, !FParse::Param(*Params, TEXT("nosave")));
}
