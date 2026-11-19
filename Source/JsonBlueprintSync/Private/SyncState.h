#pragma once

#include "CoreMinimal.h"

namespace JsonBlueprintSync
{
	struct FAssetDefaults;

	/** What the editor knows about one asset's JSON file. */
	struct FSyncEntry
	{
		/** HashText of the JSON file the last time it matched the saved asset. Empty if that never happened on this machine. */
		FString JsonHash;

		/**
		 * The package's saved hash (UPackage::GetSavedHash) at that time, so that a .uasset that changed without its JSON (e.g.
		 * committed alone) is noticed without loading every asset. Empty if unknown, e.g. in a cache from an older plugin version.
		 */
		FString AssetHash;

		/** The file has changes waiting in the JSON Sync tab. Saving the asset does not write its JSON until they are resolved. */
		bool bPending = false;
	};

	/** Which side changed a key that differs between the JSON file and the asset. */
	enum class EKeyChange : uint8
	{
		/** The asset still has the base value: the JSON changed it. Applied by default. */
		Json,
		/** The JSON still has the base value: the asset changed it. Kept by default; saving the asset writes it to the JSON. */
		Asset,
		/** Both changed it, or there is no base to tell. Kept by default and highlighted. */
		Both,
	};

	struct FKeyDiff
	{
		/** "Property", "Component.Property", "Widget.Property" or "Widget.Slot.Property". */
		FString Key;
		/** Unset if the key is not in the JSON: the archetype value. */
		TOptional<FString> JsonValue;
		/** Unset if the asset does not override the key: the archetype value. */
		TOptional<FString> AssetValue;
		EKeyChange Change = EKeyChange::Both;

		/** Whether the key is ticked (taken from the JSON) by default. */
		bool TakeJsonByDefault() const { return Change == EKeyChange::Json; }
	};

	/**
	 * Keys whose value differs between the JSON file and the asset, sorted, each with the side that changed it.
	 * Base is the asset as it was when the JSON change was detected; null if that is unknown.
	 */
	TArray<FKeyDiff> DiffKeys(const FAssetDefaults* Base, const FAssetDefaults& File, const FAssetDefaults& Current);

	/** Where the base of a newly detected difference comes from. */
	enum class EBaseSource : uint8
	{
		/** Unknown which side changed what: every key is a conflict. */
		None,
		/** Already pending: keep the base stored when the difference was first seen. */
		Stored,
		/** The JSON file, which is unchanged since the last sync: the asset made every change. */
		File,
		/** The asset as it is now, which is unchanged since the last sync: the JSON made every change. */
		Current,
	};

	struct FBaseInputs
	{
		bool bPending = false;
		/** The asset was synced on this machine before. */
		bool bHasEntry = false;
		/** The JSON's hash differs from the one recorded at the last sync. */
		bool bJsonChanged = true;
		/** The package's saved hash differs from the one recorded at the last sync (false if unknown). */
		bool bAssetChanged = false;
		/** The asset has unsaved edits. */
		bool bAssetDirty = false;
		/** The JSON names a different parent than the asset has, so its values are relative to another archetype. */
		bool bParentDiffers = false;
	};

	EBaseSource ChooseBase(const FBaseInputs& In);

	/**
	 * Whether Two Way may apply the JSON without asking: the base is known, the parents agree, no key conflicts, and the JSON
	 * changed at least one key. A JSON that is only out of date (every key changed in the asset) is left for the user.
	 */
	bool CanAutoApply(bool bHasBase, bool bParentDiffers, const TArray<FKeyDiff>& Keys);

	/** Current with the given keys taken from Source. A key that Source does not list is removed, i.e. reset to the archetype. */
	FAssetDefaults TakeKeys(const FAssetDefaults& Current, const FAssetDefaults& Source, const TArray<FString>& Keys);

	/**
	 * Local sync state per package, plus the base documents of pending assets. Lives in Saved/, never committed.
	 *   <Dir>/cache.json             package -> FSyncEntry
	 *   <Dir>/Base/<Package>.json    the asset as it was when its pending JSON change was detected
	 */
	class FSyncCache
	{
	public:
		/** <ProjectSavedDir>/JsonBlueprintSync */
		static FString GetDefaultDir();

		/**
		 * Reads <Dir>/cache.json. A missing file, or one from an older version of the plugin, gives an empty cache.
		 * Returns false if the file exists but cannot be read; the cache is then empty.
		 */
		bool Load(const FString& InDir);
		bool Save() const;

		const FSyncEntry* Find(const FString& PackageName) const { return Entries.Find(PackageName); }
		void Set(const FString& PackageName, const FSyncEntry& Entry) { Entries.Add(PackageName, Entry); }
		/** Removes the entry and its base document. */
		void Remove(const FString& PackageName);
		int32 Num() const { return Entries.Num(); }
		TArray<FString> GetPendingPackages() const;

		bool SaveBase(const FString& PackageName, const FAssetDefaults& Base) const;
		bool LoadBase(const FString& PackageName, FAssetDefaults& OutBase) const;
		void DeleteBase(const FString& PackageName) const;

	private:
		FString GetBasePath(const FString& PackageName) const;

		FString Dir;
		TMap<FString, FSyncEntry> Entries;
	};
}
