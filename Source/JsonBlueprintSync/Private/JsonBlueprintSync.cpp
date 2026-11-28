#include "JsonBlueprintSync.h"

#include "JsonBlueprintSyncModule.h"
#include "JsonBlueprintSyncSettings.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataAsset.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Misc/StringOutputDevice.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace JsonBlueprintSync
{
namespace
{
	/**
	 * Properties that are never synced. Instanced subobjects are left out: their values are objects of their own, which the format
	 * does not hold, so they stay in the .uasset.
	 */
	constexpr EPropertyFlags SkippedPropertyFlags =
		CPF_Transient | CPF_DuplicateTransient | CPF_EditConst | CPF_Deprecated | CPF_DisableEditOnInstance
		| CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance;

	/** What the Details panel of a data asset lets you edit. */
	bool IsSyncable(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(SkippedPropertyFlags) && Property->ArrayDim == 1;
	}

	bool EqualsExact(const FString& A, const FString& B)
	{
		return A.Equals(B, ESearchCase::CaseSensitive);
	}

	template <typename ValueType>
	TArray<FString> SortedKeys(const TMap<FString, ValueType>& Map)
	{
		TArray<FString> Keys;
		Map.GetKeys(Keys);
		Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
		return Keys;
	}

	/** One property value in its own memory. */
	class FValueBuffer
	{
	public:
		explicit FValueBuffer(const FProperty* InProperty)
			: Property(InProperty)
			, Memory(InProperty->AllocateAndInitializeValue())
		{
		}
		~FValueBuffer() { Property->DestroyAndFreeValue(Memory); }
		FValueBuffer(const FValueBuffer&) = delete;
		FValueBuffer& operator=(const FValueBuffer&) = delete;

		void* Get() const { return Memory; }

	private:
		const FProperty* Property;
		void* Memory;
	};

	/**
	 * The value a property is diffed against: its value in the archetype, or a default-initialized value if the archetype does not
	 * have the property.
	 */
	class FBaselineValue
	{
	public:
		FBaselineValue(const FProperty* Property, const UObject* Baseline)
		{
			const UClass* Owner = Property->GetOwnerClass();
			if (Baseline && Owner && Baseline->GetClass()->IsChildOf(Owner))
			{
				Ptr = Property->ContainerPtrToValuePtr<void>(Baseline);
			}
			else
			{
				Fallback = MakeUnique<FValueBuffer>(Property);
				Ptr = Fallback->Get();
			}
		}

		const void* Get() const { return Ptr; }

	private:
		const void* Ptr = nullptr;
		TUniquePtr<FValueBuffer> Fallback;
	};

	/** Finds a property by exact name. Sets OutError if it is missing or not synced. */
	const FProperty* FindSyncedProperty(const UClass* Class, const FString& Name, FString& OutError)
	{
		const FProperty* Property = Class ? FindFProperty<FProperty>(Class, FName(*Name)) : nullptr;
		if (!Property)
		{
			OutError = TEXT("unknown property");
			return nullptr;
		}
		if (!EqualsExact(Property->GetName(), Name))
		{
			OutError = FString::Printf(TEXT("unknown property (did you mean '%s'?)"), *Property->GetName());
			return nullptr;
		}
		if (!IsSyncable(Property))
		{
			OutError = TEXT("property is not synced (not editable on the asset, or transient or instanced)");
			return nullptr;
		}
		return Property;
	}
}

	bool ImportValue(const FProperty* Property, const FString& Text, void* Dest, UObject* Owner, FString& OutError)
	{
		// Struct import reports unknown fields only while LogExec is at Verbose (FProperty::ImportSingleProperty).
		// Raise it, so that misspelled fields reach the error device instead of being dropped silently.
		const ELogVerbosity::Type OldVerbosity = LogExec.GetVerbosity();
		LogExec.SetVerbosity(ELogVerbosity::Verbose);
		ON_SCOPE_EXIT { LogExec.SetVerbosity(OldVerbosity); };

		FStringOutputDevice Errors;
		const TCHAR* End = Property->ImportText_Direct(*Text, Dest, Owner, PPF_None, &Errors);
		if (!End)
		{
			OutError = Errors.IsEmpty() ? FString::Printf(TEXT("cannot parse '%s'"), *Text) : Errors.TrimStartAndEnd();
			return false;
		}
		while (FChar::IsWhitespace(*End))
		{
			++End;
		}
		if (*End != TEXT('\0'))
		{
			OutError = FString::Printf(TEXT("unexpected text after the value: '%s'"), End);
			return false;
		}
		if (!Errors.IsEmpty())
		{
			OutError = Errors.TrimStartAndEnd();
			return false;
		}
		return true;
	}

namespace
{
	struct FPendingWrite
	{
		FString Key;
		const FProperty* Property = nullptr;
		TUniquePtr<FValueBuffer> Value;
	};

	/** Validates one key and parses its value onto a copy of the class default. Text is null to reset the key to the class default. */
	void PrepareWrite(const FString& Key, const FString* Text, UObject* Asset, const UObject* Baseline, FApplyResult& Result,
		TArray<FPendingWrite>& Writes)
	{
		FString Error;
		const FProperty* Property = FindSyncedProperty(Asset->GetClass(), Key, Error);
		if (!Property)
		{
			Result.Errors.Add(FString::Printf(TEXT("%s: %s"), *Key, *Error));
			return;
		}

		FPendingWrite Write;
		Write.Key = Key;
		Write.Property = Property;
		Write.Value = MakeUnique<FValueBuffer>(Property);
		{
			const FBaselineValue Base(Property, Baseline);
			Property->CopyCompleteValue(Write.Value->Get(), Base.Get());
		}
		if (Text && !ImportValue(Property, *Text, Write.Value->Get(), Asset, Error))
		{
			Result.Errors.Add(FString::Printf(TEXT("%s: %s"), *Key, *Error));
			return;
		}
		Writes.Add(MoveTemp(Write));
	}

	void AppendIndent(FString& Out, int32 Level)
	{
		for (int32 i = 0; i < Level; ++i)
		{
			Out += TEXT("  ");
		}
	}

	void AppendQuoted(FString& Out, const FString& Value)
	{
		Out.AppendChar(TEXT('"'));
		for (const TCHAR C : Value)
		{
			switch (C)
			{
			case TEXT('"'): Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			case TEXT('\b'): Out += TEXT("\\b"); break;
			case TEXT('\f'): Out += TEXT("\\f"); break;
			default:
				if (C < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(C));
				}
				else
				{
					Out.AppendChar(C);
				}
			}
		}
		Out.AppendChar(TEXT('"'));
	}

	void AppendStringMap(FString& Out, const TMap<FString, FString>& Map, int32 Level)
	{
		if (Map.Num() == 0)
		{
			Out += TEXT("{}");
			return;
		}
		Out += TEXT("{\n");
		const TArray<FString> Keys = SortedKeys(Map);
		for (int32 i = 0; i < Keys.Num(); ++i)
		{
			AppendIndent(Out, Level + 1);
			AppendQuoted(Out, Keys[i]);
			Out += TEXT(": ");
			AppendQuoted(Out, Map.FindChecked(Keys[i]));
			Out += i + 1 < Keys.Num() ? TEXT(",\n") : TEXT("\n");
		}
		AppendIndent(Out, Level);
		Out += TEXT("}");
	}

	/** Path of a field for error messages, e.g. "overrides.self". */
	FString FieldPath(const FString& Parent, const FString& Name)
	{
		return Parent.IsEmpty() ? Name : Parent + TEXT(".") + Name;
	}

	/** Fails with an error naming the first field of Object that is not in Known. */
	bool CheckKnownFields(const FJsonObject& Object, const FString& Path, const TSet<FString>& Known, FString& OutError)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
		{
			if (!Known.Contains(Pair.Key))
			{
				OutError = FString::Printf(TEXT("unknown field '%s'"), *FieldPath(Path, Pair.Key));
				return false;
			}
		}
		return true;
	}

	bool ReadStringMap(const FJsonObject& Object, const FString& Context, TMap<FString, FString>& Out, FString& OutError)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
			{
				OutError = FString::Printf(TEXT("%s.%s: value must be a string of Unreal property text, e.g. \"2.5\""), *Context, *Pair.Key);
				return false;
			}
			Out.Add(Pair.Key, Pair.Value->AsString());
		}
		return true;
	}

	/** Reads an optional object field of Parent (whose path is ParentPath). Out stays null if the field is missing. */
	bool GetObjectField(const FJsonObject& Parent, const FString& ParentPath, const TCHAR* Name, TSharedPtr<FJsonObject>& Out, FString& OutError)
	{
		const TSharedPtr<FJsonValue>* Value = Parent.Values.Find(Name);
		if (!Value)
		{
			return true;
		}
		if (!Value->IsValid() || (*Value)->Type != EJson::Object)
		{
			OutError = FString::Printf(TEXT("'%s' must be an object"), *FieldPath(ParentPath, Name));
			return false;
		}
		Out = (*Value)->AsObject();
		return true;
	}

	bool IsChildOfClass(const FTopLevelAssetPath& ClassPath, const UClass* Base)
	{
		if (const UClass* Class = FindObject<UClass>(ClassPath))
		{
			return Class->IsChildOf(Base);
		}
		// Not loaded, e.g. the Blueprint class of a data asset: ask the asset registry, which knows Blueprint class hierarchies.
		TArray<FTopLevelAssetPath> Ancestors;
		if (IAssetRegistry* Registry = IAssetRegistry::Get())
		{
			Registry->GetAncestorClassNames(ClassPath, Ancestors);
		}
		return Ancestors.Contains(Base->GetClassPathName());
	}

	/** "/Game/A/" or "/Game/A" -> "/Game/A". */
	FString TrimFolder(FString Folder)
	{
		Folder.TrimStartAndEndInline();
		while (Folder.Len() > 1 && Folder.EndsWith(TEXT("/")))
		{
			Folder.LeftChopInline(1);
		}
		return Folder;
	}

	bool IsUnderFolder(const FString& PackageName, const FString& Folder)
	{
		return !Folder.IsEmpty() && PackageName.StartsWith(Folder + TEXT("/"), ESearchCase::IgnoreCase);
	}
}

bool IsDataAsset(const UObject* Asset)
{
	return Asset && Asset->IsA<UDataAsset>() && !Asset->HasAnyFlags(RF_ClassDefaultObject);
}

bool IsDataAsset(const FAssetData& Asset)
{
	return IsChildOfClass(Asset.AssetClassPath, UDataAsset::StaticClass());
}

TArray<FString> GetSyncedRoots()
{
	TArray<FString> Roots = { TEXT("/Game") };
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPluginsWithContent())
	{
		if (Plugin->GetLoadedFrom() != EPluginLoadedFrom::Project)
		{
			continue;
		}
		const FString Root = TrimFolder(Plugin->GetMountedAssetPath());
		if (Root.Len() > 1 && FPackageName::MountPointExists(Root + TEXT("/")))
		{
			Roots.AddUnique(Root);
		}
	}
	Roots.Sort();
	return Roots;
}

bool IsSyncedPackage(const FString& PackageName)
{
	if (FPackageName::IsTempPackage(PackageName))
	{
		return false;
	}
	if (!GetSyncedRoots().ContainsByPredicate([&PackageName](const FString& Root) { return IsUnderFolder(PackageName, Root); }))
	{
		return false;
	}
	for (const FDirectoryPath& Excluded : GetDefault<UJsonBlueprintSyncSettings>()->ExcludedFolders)
	{
		if (IsUnderFolder(PackageName, TrimFolder(Excluded.Path)))
		{
			return false;
		}
	}
	return true;
}

FMissingPairs FindMissingPairs(const FString& JsonRoot)
{
	FMissingPairs Result;
	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return Result;
	}
	const TArray<FString> Roots = GetSyncedRoots();

	TSet<FString> WithJson;
	for (const FString& Root : Roots)
	{
		for (const FString& File : FindJsonFiles(JsonRoot, Root))
		{
			FString PackageName;
			if (!GetPackageName(JsonRoot, File, PackageName))
			{
				Result.JsonWithoutAsset.Add(File);
				continue;
			}
			if (!IsSyncedPackage(PackageName))
			{
				continue;
			}
			WithJson.Add(PackageName);
			const FSoftObjectPath ObjectPath(PackageName + TEXT(".") + FPackageName::GetShortName(PackageName));
			const FAssetData Asset = Registry->GetAssetByObjectPath(ObjectPath);
			if (!Asset.IsValid() || !IsDataAsset(Asset))
			{
				Result.JsonWithoutAsset.Add(File);
			}
		}
	}

	FARFilter Filter;
	Filter.ClassPaths.Add(UDataAsset::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	for (const FString& Root : Roots)
	{
		Filter.PackagePaths.Add(FName(*Root));
	}
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	Registry->GetAssets(Filter, Assets);
	for (const FAssetData& Asset : Assets)
	{
		const FString PackageName = Asset.PackageName.ToString();
		// Only a package's main asset is synced.
		if (Asset.AssetName.ToString() == FPackageName::GetShortName(PackageName) && IsSyncedPackage(PackageName) && !WithJson.Contains(PackageName))
		{
			Result.AssetsWithoutJson.AddUnique(PackageName);
		}
	}
	Result.JsonWithoutAsset.Sort();
	Result.AssetsWithoutJson.Sort();
	return Result;
}

FString GetDefaultJsonRoot()
{
	FString Dir = GetDefault<UJsonBlueprintSyncSettings>()->JsonDir;
	if (Dir.IsEmpty())
	{
		Dir = TEXT("JsonBlueprintSync");
	}
	FString Full = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Dir);
	FPaths::NormalizeDirectoryName(Full);
	return Full;
}

FString GetJsonPath(const FString& JsonRoot, const FString& PackageName)
{
	FString Relative = PackageName;
	Relative.RemoveFromStart(TEXT("/"));
	return FPaths::Combine(JsonRoot, Relative) + TEXT(".json");
}

bool GetPackageName(const FString& JsonRoot, const FString& JsonPath, FString& OutPackageName)
{
	FString Full = FPaths::ConvertRelativePathToFull(JsonPath);
	FPaths::NormalizeFilename(Full);
	FString Root = FPaths::ConvertRelativePathToFull(JsonRoot);
	FPaths::NormalizeDirectoryName(Root);
	Root += TEXT("/");

	if (!Full.StartsWith(Root, ESearchCase::IgnoreCase) || !Full.EndsWith(TEXT(".json"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	OutPackageName = TEXT("/") + Full.Mid(Root.Len()).LeftChop(5);
	return FPackageName::IsValidLongPackageName(OutPackageName);
}

FAssetDefaults Capture(UObject* Asset)
{
	FAssetDefaults Doc;
	Doc.Asset = Asset->GetPackage()->GetName();
	Doc.Parent = Asset->GetClass()->GetPathName();
	const UObject* Baseline = Asset->GetArchetype();
	for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
	{
		const FProperty* Property = *It;
		if (!IsSyncable(Property))
		{
			continue;
		}
		const void* Value = Property->ContainerPtrToValuePtr<void>(Asset);
		const FBaselineValue Base(Property, Baseline);
		if (Property->Identical(Value, Base.Get(), PPF_None))
		{
			continue;
		}
		FString Text;
		Property->ExportTextItem_Direct(Text, Value, Base.Get(), Asset, PPF_None);
		Doc.Self.Add(Property->GetName(), MoveTemp(Text));
	}
	return Doc;
}

FString ToJson(const FAssetDefaults& Doc)
{
	FString Out;
	Out += TEXT("{\n");
	Out += FString::Printf(TEXT("  \"schema\": %d,\n"), FAssetDefaults::SchemaVersion);
	Out += TEXT("  \"asset\": ");
	AppendQuoted(Out, Doc.Asset);
	Out += TEXT(",\n  \"parent\": ");
	AppendQuoted(Out, Doc.Parent);
	Out += TEXT(",\n  \"overrides\": {\n    \"self\": ");
	AppendStringMap(Out, Doc.Self, 2);
	Out += TEXT("\n  }\n}\n");
	return Out;
}

bool FromJson(const FString& Json, FAssetDefaults& OutDoc, FString& OutError)
{
	OutDoc = FAssetDefaults();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("not valid JSON: %s"), *Reader->GetErrorMessage());
		return false;
	}

	// Schema first, so that an old file gets a useful message rather than "unknown field".
	int32 Schema = 0;
	if (!Root->TryGetNumberField(TEXT("schema"), Schema) || Schema != FAssetDefaults::SchemaVersion)
	{
		OutError = Schema > 0 && Schema < FAssetDefaults::SchemaVersion
			? FString::Printf(TEXT("schema %d is from an older version of the plugin; re-export the file (save the asset, or run -run=JsonBlueprintSync -export)"), Schema)
			: FString::Printf(TEXT("'schema' must be %d"), FAssetDefaults::SchemaVersion);
		return false;
	}

	static const TSet<FString> KnownFields = { TEXT("schema"), TEXT("asset"), TEXT("parent"), TEXT("overrides") };
	if (!CheckKnownFields(*Root, FString(), KnownFields, OutError))
	{
		return false;
	}
	if (!Root->TryGetStringField(TEXT("asset"), OutDoc.Asset) || OutDoc.Asset.IsEmpty())
	{
		OutError = TEXT("'asset' must be the asset's package name");
		return false;
	}
	Root->TryGetStringField(TEXT("parent"), OutDoc.Parent);

	// A missing "overrides" or "self" means that nothing differs from the class defaults.
	TSharedPtr<FJsonObject> Overrides;
	if (!GetObjectField(*Root, FString(), TEXT("overrides"), Overrides, OutError))
	{
		return false;
	}
	if (!Overrides.IsValid())
	{
		return true;
	}
	const FString OverridesPath = TEXT("overrides");
	static const TSet<FString> KnownOverrides = { TEXT("self") };
	if (!CheckKnownFields(*Overrides, OverridesPath, KnownOverrides, OutError))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Self;
	return GetObjectField(*Overrides, OverridesPath, TEXT("self"), Self, OutError)
		&& (!Self.IsValid() || ReadStringMap(*Self, FieldPath(OverridesPath, TEXT("self")), OutDoc.Self, OutError));
}

TMap<FString, FString> Flatten(const FAssetDefaults& Doc)
{
	return Doc.Self;
}

bool ValuesEqual(const FAssetDefaults& A, const FAssetDefaults& B)
{
	if (A.Self.Num() != B.Self.Num())
	{
		return false;
	}
	for (const TPair<FString, FString>& Pair : A.Self)
	{
		const FString* Other = B.Self.Find(Pair.Key);
		if (!Other || !EqualsExact(*Other, Pair.Value))
		{
			return false;
		}
	}
	return true;
}

FApplyResult Apply(UObject* Asset, const FAssetDefaults& Doc)
{
	FApplyResult Result;

	if (!IsDataAsset(Asset))
	{
		Result.Errors.Add(TEXT("not a data asset"));
		return Result;
	}
	const FString PackageName = Asset->GetPackage()->GetName();
	if (!EqualsExact(Doc.Asset, PackageName))
	{
		Result.Errors.Add(FString::Printf(TEXT("the JSON is for %s, not %s"), *Doc.Asset, *PackageName));
		return Result;
	}

	// The document is a full snapshot: what it does not list goes back to the class default. Current says what the asset overrides now.
	const FAssetDefaults Current = Capture(Asset);
	if (!Doc.Parent.IsEmpty() && !EqualsExact(Doc.Parent, Current.Parent))
	{
		Result.Warnings.Add(FString::Printf(TEXT("class differs: JSON has %s, the asset has %s"), *Doc.Parent, *Current.Parent));
	}

	// Pass 1: validate everything and parse the values. Nothing is written if anything is wrong.
	const UObject* Baseline = Asset->GetArchetype();
	TArray<FPendingWrite> Writes;
	for (const FString& Key : SortedKeys(Doc.Self))
	{
		PrepareWrite(Key, &Doc.Self.FindChecked(Key), Asset, Baseline, Result, Writes);
	}
	for (const FString& Key : SortedKeys(Current.Self))
	{
		if (!Doc.Self.Contains(Key))
		{
			PrepareWrite(Key, nullptr, Asset, Baseline, Result, Writes);
		}
	}
	if (Result.HasErrors())
	{
		return Result;
	}

	// Pass 2: write the values that differ.
	for (FPendingWrite& Write : Writes)
	{
		FProperty* Property = const_cast<FProperty*>(Write.Property);
		void* Value = Property->ContainerPtrToValuePtr<void>(Asset);
		if (Property->Identical(Value, Write.Value->Get(), PPF_None))
		{
			continue;
		}
		Asset->Modify();
		Asset->PreEditChange(Property);
		Property->CopyCompleteValue(Value, Write.Value->Get());
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Asset->PostEditChangeProperty(Event);
		Result.Changed.Add(Write.Key);
	}
	return Result;
}

void MarkModified(UObject* Asset)
{
	Asset->MarkPackageDirty();
}

FApplyResult Import(UObject* Asset, const FAssetDefaults& Doc)
{
	FApplyResult Result = Apply(Asset, Doc);
	if (Result.Changed.Num() > 0)
	{
		MarkModified(Asset);
	}
	return Result;
}

FString NormalizeLineEndings(const FString& Text)
{
	return Text.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\r"), TEXT("\n"));
}

bool LoadFile(const FString& Path, FAssetDefaults& OutDoc, FString& OutError)
{
	OutError.Reset();
	if (!FPaths::FileExists(Path))
	{
		return false;
	}
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = TEXT("cannot read the file");
		return false;
	}
	return FromJson(Text, OutDoc, OutError);
}

bool SaveFileIfChanged(const FString& Path, const FAssetDefaults& Doc, bool& bOutWritten)
{
	bOutWritten = false;
	const FString Json = ToJson(Doc);
	FString Existing;
	if (FPaths::FileExists(Path) && FFileHelper::LoadFileToString(Existing, *Path) && EqualsExact(NormalizeLineEndings(Existing), Json))
	{
		return true;
	}
	if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}
	bOutWritten = true;
	return true;
}

TArray<FString> FindJsonFiles(const FString& JsonRoot, const FString& Dir)
{
	const FString Folder = FPaths::Combine(JsonRoot, Dir.RightChop(1));
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Folder, TEXT("*.json"), true, false);
	for (FString& File : Files)
	{
		FPaths::NormalizeFilename(File);
	}
	Files.Sort();
	return Files;
}

UObject* LoadAsset(const FString& PackageName)
{
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	return LoadObject<UObject>(nullptr, *ObjectPath);
}

FString HashText(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*NormalizeLineEndings(Text));
	return FMD5::HashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}
}
