#pragma once

#include "CoreMinimal.h"

namespace JsonBlueprintSync
{
	struct FBlueprintDefaults;

	/** Hashes (HashText) recorded the last time a Blueprint and its JSON file were known to match. */
	struct FSyncEntry
	{
		/** The JSON file's text. */
		FString JsonHash;

		/** ToJson(Capture(Blueprint)) without a previous document, so it does not depend on the file's reset list. */
		FString AssetHash;
	};

	enum class ESyncAction : uint8
	{
		/** The file has not changed since the last sync. */
		None,
		/** The file matches the asset (a teammate committed both). Record the new hashes. */
		UpdateCache,
		/** Only the file changed: the JSON wins. */
		Import,
		/** Both changed and they disagree, or they disagree and there is no record. The user picks. */
		Conflict,
	};

	/**
	 * Which side wins for one JSON file.
	 * Entry: the last sync, or null. FileHash: the file on disk. CurrentHash: ToJson(Capture(Blueprint, &FileDoc)).
	 * AssetHash: as in FSyncEntry, for the asset now.
	 */
	ESyncAction Classify(const FSyncEntry* Entry, const FString& FileHash, const FString& CurrentHash, const FString& AssetHash);

	/** Keys that differ between two documents: "Property", "Component.Property" or "parent". Sorted. */
	TArray<FString> DiffKeys(const FBlueprintDefaults& A, const FBlueprintDefaults& B);

	/** Local record of the last sync per Blueprint package. Lives in Saved/, never committed. */
	class FSyncCache
	{
	public:
		/** <ProjectSavedDir>/JsonBlueprintSync/cache.json */
		static FString GetDefaultPath();

		/** A missing file gives an empty cache. Returns false if the file exists but cannot be read; the cache is then empty. */
		bool Load(const FString& Path);
		bool Save(const FString& Path) const;

		const FSyncEntry* Find(const FString& PackageName) const { return Entries.Find(PackageName); }
		void Set(const FString& PackageName, const FSyncEntry& Entry) { Entries.Add(PackageName, Entry); }
		void Remove(const FString& PackageName) { Entries.Remove(PackageName); }
		int32 Num() const { return Entries.Num(); }

	private:
		TMap<FString, FSyncEntry> Entries;
	};
}
