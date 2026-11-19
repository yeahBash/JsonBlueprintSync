#include "SyncState.h"

#include "JsonBlueprintSync.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace JsonBlueprintSync
{
namespace
{
	constexpr int32 CacheVersion = 2;

	bool SameValue(const FString* A, const FString* B)
	{
		if (!A || !B)
		{
			return A == B;
		}
		return A->Equals(*B, ESearchCase::CaseSensitive);
	}

	TOptional<FString> ToOptional(const FString* Value)
	{
		return Value ? TOptional<FString>(*Value) : TOptional<FString>();
	}
}

TArray<FKeyDiff> DiffKeys(const FAssetDefaults* Base, const FAssetDefaults& File, const FAssetDefaults& Current)
{
	const TMap<FString, FString> FlatFile = Flatten(File);
	const TMap<FString, FString> FlatCurrent = Flatten(Current);
	const TMap<FString, FString> FlatBase = Base ? Flatten(*Base) : TMap<FString, FString>();

	TSet<FString> Keys;
	for (const TPair<FString, FString>& Pair : FlatFile) { Keys.Add(Pair.Key); }
	for (const TPair<FString, FString>& Pair : FlatCurrent) { Keys.Add(Pair.Key); }

	TArray<FKeyDiff> Diffs;
	for (const FString& Key : Keys)
	{
		const FString* FileValue = FlatFile.Find(Key);
		const FString* CurrentValue = FlatCurrent.Find(Key);
		if (SameValue(FileValue, CurrentValue))
		{
			continue;
		}
		FKeyDiff& Diff = Diffs.AddDefaulted_GetRef();
		Diff.Key = Key;
		Diff.JsonValue = ToOptional(FileValue);
		Diff.AssetValue = ToOptional(CurrentValue);
		if (Base)
		{
			const FString* BaseValue = FlatBase.Find(Key);
			if (SameValue(BaseValue, CurrentValue))
			{
				Diff.Change = EKeyChange::Json;
			}
			else if (SameValue(BaseValue, FileValue))
			{
				Diff.Change = EKeyChange::Asset;
			}
		}
	}
	Diffs.Sort([](const FKeyDiff& A, const FKeyDiff& B) { return A.Key.Compare(B.Key, ESearchCase::CaseSensitive) < 0; });
	return Diffs;
}

EBaseSource ChooseBase(const FBaseInputs& In)
{
	if (In.bPending)
	{
		return EBaseSource::Stored;
	}
	if (!In.bHasEntry)
	{
		return EBaseSource::None;
	}
	if (!In.bJsonChanged)
	{
		// The file is what was synced last, so whatever differs was changed in the asset, even if it is dirty or reparented.
		return EBaseSource::File;
	}
	if (In.bParentDiffers || In.bAssetChanged || In.bAssetDirty)
	{
		// Both sides moved since the last sync, or the JSON's values are relative to another parent.
		return EBaseSource::None;
	}
	return EBaseSource::Current;
}

bool CanAutoApply(bool bHasBase, bool bParentDiffers, const TArray<FKeyDiff>& Keys)
{
	if (!bHasBase || bParentDiffers)
	{
		return false;
	}
	bool bAnyJson = false;
	for (const FKeyDiff& Diff : Keys)
	{
		if (Diff.Change == EKeyChange::Both)
		{
			return false;
		}
		bAnyJson |= Diff.Change == EKeyChange::Json;
	}
	return bAnyJson;
}

FAssetDefaults TakeKeys(const FAssetDefaults& Current, const FAssetDefaults& Source, const TArray<FString>& Keys)
{
	FAssetDefaults Result = Current;
	for (const FString& Key : Keys)
	{
		FString Object, Property;
		if (!Key.Split(TEXT("."), &Object, &Property, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			if (const FString* Value = Source.Defaults.Find(Key))
			{
				Result.Defaults.Add(Key, *Value);
			}
			else
			{
				Result.Defaults.Remove(Key);
			}
			continue;
		}
		// A Blueprint has components or widgets, never both, so an object name belongs to one section.
		const bool bWidget = Source.Widgets.Contains(Object) || Current.Widgets.Contains(Object);
		TMap<FString, TMap<FString, FString>>& ResultMaps = bWidget ? Result.Widgets : Result.Components;
		const TMap<FString, TMap<FString, FString>>& SourceMaps = bWidget ? Source.Widgets : Source.Components;

		const TMap<FString, FString>* SourceValues = SourceMaps.Find(Object);
		const FString* Value = SourceValues ? SourceValues->Find(Property) : nullptr;
		if (Value)
		{
			ResultMaps.FindOrAdd(Object).Add(Property, *Value);
		}
		else if (TMap<FString, FString>* Values = ResultMaps.Find(Object))
		{
			Values->Remove(Property);
			if (Values->Num() == 0)
			{
				ResultMaps.Remove(Object);
			}
		}
	}
	return Result;
}

FString FSyncCache::GetDefaultDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("JsonBlueprintSync")));
}

bool FSyncCache::Load(const FString& InDir)
{
	Dir = InDir;
	Entries.Reset();
	const FString Path = FPaths::Combine(Dir, TEXT("cache.json"));
	if (!FPaths::FileExists(Path))
	{
		return true;
	}
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		return false;
	}
	int32 Version = 0;
	const TSharedPtr<FJsonObject>* Items = nullptr;
	if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != CacheVersion)
	{
		// An older plugin's cache: its hashes are of schema 1 files, which no longer load. Start over.
		return true;
	}
	if (!Root->TryGetObjectField(TEXT("entries"), Items))
	{
		return false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Items)->Values)
	{
		const TSharedPtr<FJsonObject>* Item = nullptr;
		if (!Pair.Value->TryGetObject(Item))
		{
			continue;
		}
		FSyncEntry Entry;
		(*Item)->TryGetStringField(TEXT("json"), Entry.JsonHash);
		(*Item)->TryGetStringField(TEXT("asset"), Entry.AssetHash);
		(*Item)->TryGetBoolField(TEXT("pending"), Entry.bPending);
		Entries.Add(Pair.Key, MoveTemp(Entry));
	}
	return true;
}

bool FSyncCache::Save() const
{
	TArray<FString> Names;
	Entries.GetKeys(Names);
	Names.Sort();

	const TSharedRef<FJsonObject> Items = MakeShared<FJsonObject>();
	for (const FString& Name : Names)
	{
		const FSyncEntry& Entry = Entries[Name];
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("json"), Entry.JsonHash);
		if (!Entry.AssetHash.IsEmpty())
		{
			Item->SetStringField(TEXT("asset"), Entry.AssetHash);
		}
		if (Entry.bPending)
		{
			Item->SetBoolField(TEXT("pending"), true);
		}
		Items->SetObjectField(Name, Item);
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), CacheVersion);
	Root->SetObjectField(TEXT("entries"), Items);

	FString Text;
	if (!FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text)))
	{
		return false;
	}
	return FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Dir, TEXT("cache.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FSyncCache::Remove(const FString& PackageName)
{
	Entries.Remove(PackageName);
	DeleteBase(PackageName);
}

TArray<FString> FSyncCache::GetPendingPackages() const
{
	TArray<FString> Packages;
	for (const TPair<FString, FSyncEntry>& Pair : Entries)
	{
		if (Pair.Value.bPending)
		{
			Packages.Add(Pair.Key);
		}
	}
	Packages.Sort();
	return Packages;
}

FString FSyncCache::GetBasePath(const FString& PackageName) const
{
	return GetJsonPath(FPaths::Combine(Dir, TEXT("Base")), PackageName);
}

bool FSyncCache::SaveBase(const FString& PackageName, const FAssetDefaults& Base) const
{
	return FFileHelper::SaveStringToFile(ToJson(Base), *GetBasePath(PackageName), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool FSyncCache::LoadBase(const FString& PackageName, FAssetDefaults& OutBase) const
{
	FString Error;
	return LoadFile(GetBasePath(PackageName), OutBase, Error);
}

void FSyncCache::DeleteBase(const FString& PackageName) const
{
	const FString Path = GetBasePath(PackageName);
	if (FPaths::FileExists(Path))
	{
		IFileManager::Get().Delete(*Path, false, true, true);
	}
}
}
