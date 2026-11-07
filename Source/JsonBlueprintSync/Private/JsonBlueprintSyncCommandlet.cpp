#include "JsonBlueprintSyncCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "JsonBlueprintSync.h"
#include "JsonBlueprintSyncModule.h"
#include "Engine/Blueprint.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(JsonBlueprintSyncCommandlet)

namespace
{
	/** Blueprint package names under Dir, sorted. */
	TArray<FString> FindBlueprintPackages(const FString& Dir)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.SearchAllAssets(true);
		Registry.ScanPathsSynchronous({ Dir }, true);

		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.PackagePaths.Add(FName(*Dir));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);

		TArray<FString> Packages;
		for (const FAssetData& Asset : Assets)
		{
			Packages.AddUnique(Asset.PackageName.ToString());
		}
		Packages.Sort();
		return Packages;
	}

	using JsonBlueprintSync::FindJsonFiles;
	using JsonBlueprintSync::LoadBlueprint;

	int32 RunExport(const FString& JsonRoot, const FString& Dir)
	{
		int32 Failures = 0;
		int32 Written = 0;
		const TArray<FString> Packages = FindBlueprintPackages(Dir);
		TSet<FString> Expected;
		for (const FString& PackageName : Packages)
		{
			UBlueprint* Blueprint = LoadBlueprint(PackageName);
			if (!Blueprint)
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: cannot load the Blueprint"), *PackageName);
				++Failures;
				continue;
			}
			const FString Path = JsonBlueprintSync::GetJsonPath(JsonRoot, PackageName);
			Expected.Add(Path);

			JsonBlueprintSync::FBlueprintDefaults Previous;
			FString Error;
			const bool bHasPrevious = JsonBlueprintSync::LoadFile(Path, Previous, Error);
			if (!Error.IsEmpty())
			{
				UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: ignoring the existing file, %s"), *Path, *Error);
			}

			bool bWritten = false;
			if (!JsonBlueprintSync::SaveFileIfChanged(Path, JsonBlueprintSync::Capture(Blueprint, bHasPrevious ? &Previous : nullptr), bWritten))
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
				UE_LOG(LogJsonBlueprintSync, Warning, TEXT("%s: no Blueprint for this file"), *File);
			}
		}
		UE_LOG(LogJsonBlueprintSync, Display, TEXT("Export: %d Blueprints, %d files written, %d failures"), Packages.Num(), Written, Failures);
		return Failures > 0 ? 1 : 0;
	}

	int32 RunVerify(const FString& JsonRoot, const FString& Dir)
	{
		int32 Failures = 0;
		const TArray<FString> Packages = FindBlueprintPackages(Dir);
		TSet<FString> Expected;
		for (const FString& PackageName : Packages)
		{
			const FString Path = JsonBlueprintSync::GetJsonPath(JsonRoot, PackageName);
			Expected.Add(Path);

			UBlueprint* Blueprint = LoadBlueprint(PackageName);
			if (!Blueprint)
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: cannot load the Blueprint"), *PackageName);
				++Failures;
				continue;
			}
			JsonBlueprintSync::FBlueprintDefaults OnDisk;
			FString Error;
			if (!JsonBlueprintSync::LoadFile(Path, OnDisk, Error))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: %s"), *Path, Error.IsEmpty() ? TEXT("missing; run -export") : *Error);
				++Failures;
				continue;
			}
			FString Text;
			FFileHelper::LoadFileToString(Text, *Path);
			const FString Current = JsonBlueprintSync::ToJson(JsonBlueprintSync::Capture(Blueprint, &OnDisk));
			if (!JsonBlueprintSync::NormalizeLineEndings(Text).Equals(Current, ESearchCase::CaseSensitive))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: differs from %s; run -export or -import"), *Path, *PackageName);
				++Failures;
			}
		}
		for (const FString& File : FindJsonFiles(JsonRoot, Dir))
		{
			if (!Expected.Contains(File))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: no Blueprint for this file"), *File);
				++Failures;
			}
		}
		UE_LOG(LogJsonBlueprintSync, Display, TEXT("Verify: %d Blueprints, %d failures"), Packages.Num(), Failures);
		return Failures > 0 ? 1 : 0;
	}

	int32 RunImport(const FString& JsonRoot, const FString& Dir, bool bSave)
	{
		// Make sure the asset registry knows the Blueprints, so that loading resolves parents.
		FindBlueprintPackages(Dir);

		int32 Failures = 0;
		struct FEntry
		{
			UBlueprint* Blueprint;
			JsonBlueprintSync::FBlueprintDefaults Doc;
			int32 Depth;
		};
		TArray<FEntry> Entries;
		for (const FString& File : FindJsonFiles(JsonRoot, Dir))
		{
			FString PackageName;
			if (!JsonBlueprintSync::GetPackageName(JsonRoot, File, PackageName))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: not a valid Blueprint path"), *File);
				++Failures;
				continue;
			}
			JsonBlueprintSync::FBlueprintDefaults Doc;
			FString Error;
			if (!JsonBlueprintSync::LoadFile(File, Doc, Error))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: %s"), *File, *Error);
				++Failures;
				continue;
			}
			UBlueprint* Blueprint = FPackageName::DoesPackageExist(PackageName) ? LoadBlueprint(PackageName) : nullptr;
			if (!Blueprint)
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("%s: no Blueprint %s"), *File, *PackageName);
				++Failures;
				continue;
			}
			Entries.Add({ Blueprint, MoveTemp(Doc), JsonBlueprintSync::GetBlueprintDepth(Blueprint) });
		}
		Entries.StableSort([](const FEntry& A, const FEntry& B) { return A.Depth < B.Depth; });

		int32 Changed = 0;
		for (const FEntry& Entry : Entries)
		{
			const FString PackageName = Entry.Blueprint->GetPackage()->GetName();
			const JsonBlueprintSync::FApplyResult Result = JsonBlueprintSync::Import(Entry.Blueprint, Entry.Doc);
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
				UE_LOG(LogJsonBlueprintSync, Display, TEXT("%s: changed %s"), *PackageName, *FString::Join(Result.Changed, TEXT(", ")));
			}
		}

		if (bSave)
		{
			// Save every dirty Blueprint package: the imported ones and the child Blueprints the values propagated to.
			TArray<UPackage*> Dirty;
			for (TObjectIterator<UBlueprint> It; It; ++It)
			{
				UPackage* Package = It->GetPackage();
				if (Package && Package->IsDirty() && !Package->HasAnyPackageFlags(PKG_CompiledIn | PKG_InMemoryOnly)
					&& !FPackageName::IsTempPackage(Package->GetName()))
				{
					Dirty.AddUnique(Package);
				}
			}
			if (Dirty.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(Dirty, true))
			{
				UE_LOG(LogJsonBlueprintSync, Error, TEXT("Saving the changed packages failed"));
				++Failures;
			}
			UE_LOG(LogJsonBlueprintSync, Display, TEXT("Saved %d packages"), Dirty.Num());
		}
		UE_LOG(LogJsonBlueprintSync, Display, TEXT("Import: %d files, %d Blueprints changed, %d failures"), Entries.Num(), Changed, Failures);
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
	HelpDescription = TEXT("Syncs Blueprint class defaults with JSON files.");
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

	FString JsonRoot = JsonBlueprintSync::GetDefaultJsonRoot();
	FString JsonDirOverride;
	if (FParse::Value(*Params, TEXT("jsondir="), JsonDirOverride))
	{
		JsonRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), JsonDirOverride);
		FPaths::NormalizeDirectoryName(JsonRoot);
	}
	UE_LOG(LogJsonBlueprintSync, Display, TEXT("Blueprints under %s, JSON under %s"), *Dir, *JsonRoot);

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