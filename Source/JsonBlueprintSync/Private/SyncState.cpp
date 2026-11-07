#include "SyncState.h"

#include "JsonBlueprintSync.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace JsonBlueprintSync
{
namespace
{
	constexpr int32 CacheVersion = 1;

	/** Flattens a document to key -> value; reset keys get a marker that no property text can equal. */
	TMap<FString, FString> Flatten(const FBlueprintDefaults& Doc)
	{
		TMap<FString, FString> Result;
		Result.Add(TEXT("parent"), Doc.Parent);
		for (const TPair<FString, FString>& Pair : Doc.Defaults)
		{
			Result.Add(Pair.Key, Pair.Value);
		}
		for (const TMap<FString, TMap<FString, FString>>* Maps : { &Doc.Components, &Doc.Widgets })
		{
			for (const TPair<FString, TMap<FString, FString>>& Object : *Maps)
			{
				for (const TPair<FString, FString>& Pair : Object.Value)
				{
					Result.Add(Object.Key + TEXT(".") + Pair.Key, Pair.Value);
				}
			}
		}
		for (const FString& Key : Doc.Reset)
		{
			Result.Add(Key, TEXT("\n<reset>"));
		}
		return Result;
	}
}

ESyncAction Classify(const FSyncEntry* Entry, const FString& FileHash, const FString& CurrentHash, const FString& AssetHash)
{
	if (Entry && Entry->JsonHash == FileHash)
	{
		return ESyncAction::None;
	}
	if (CurrentHash == FileHash)
	{
		return ESyncAction::UpdateCache;
	}
	if (Entry && Entry->AssetHash == AssetHash)
	{
		return ESyncAction::Import;
	}
	return ESyncAction::Conflict;
}

TArray<FString> DiffKeys(const FBlueprintDefaults& A, const FBlueprintDefaults& B)
{
	const TMap<FString, FString> FlatA = Flatten(A);
	const TMap<FString, FString> FlatB = Flatten(B);
	TArray<FString> Keys;
	for (const TPair<FString, FString>& Pair : FlatA)
	{
		const FString* Other = FlatB.Find(Pair.Key);
		if (!Other || !Other->Equals(Pair.Value, ESearchCase::CaseSensitive))
		{
			Keys.Add(Pair.Key);
		}
	}
	for (const TPair<FString, FString>& Pair : FlatB)
	{
		if (!FlatA.Contains(Pair.Key))
		{
			Keys.Add(Pair.Key);
		}
	}
	Keys.Sort([](const FString& L, const FString& R) { return L.Compare(R, ESearchCase::CaseSensitive) < 0; });
	return Keys;
}

FString FSyncCache::GetDefaultPath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("JsonBlueprintSync"), TEXT("cache.json")));
}

bool FSyncCache::Load(const FString& Path)
{
	Entries.Reset();
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
	if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != CacheVersion || !Root->TryGetObjectField(TEXT("entries"), Items))
	{
		return false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Items)->Values)
	{
		const TSharedPtr<FJsonObject>* Item = nullptr;
		FSyncEntry Entry;
		if (Pair.Value->TryGetObject(Item) && (*Item)->TryGetStringField(TEXT("json"), Entry.JsonHash) && (*Item)->TryGetStringField(TEXT("asset"), Entry.AssetHash))
		{
			Entries.Add(Pair.Key, MoveTemp(Entry));
		}
	}
	return true;
}

bool FSyncCache::Save(const FString& Path) const
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
		Item->SetStringField(TEXT("asset"), Entry.AssetHash);
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
	return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}
