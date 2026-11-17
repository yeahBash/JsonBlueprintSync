#include "JsonBlueprintSync.h"

#include "JsonBlueprintSyncModule.h"
#include "JsonBlueprintSyncSettings.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BaseWidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ActorComponent.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataAsset.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Misc/StringOutputDevice.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/UnrealType.h"

namespace JsonBlueprintSync
{
namespace
{
	/** Properties that are never synced, wherever they are. */
	constexpr EPropertyFlags SkippedPropertyFlags =
		CPF_Transient | CPF_DuplicateTransient | CPF_EditConst | CPF_Deprecated
		| CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance;

	/**
	 * Templates (class defaults, component templates, Designer widgets) are edited like defaults; a data asset is edited like an
	 * instance. The two hide different properties in the Details panel.
	 */
	enum class EObjectRole : uint8
	{
		Template,
		Instance,
	};

	bool IsSyncable(const FProperty* Property, EObjectRole Role)
	{
		const EPropertyFlags Hidden = Role == EObjectRole::Template ? CPF_DisableEditOnTemplate : CPF_DisableEditOnInstance;
		if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(SkippedPropertyFlags | Hidden) || Property->ArrayDim != 1)
		{
			return false;
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			// References to components wire sub-objects together; they are not data. Components are synced under "components".
			if (ObjectProperty->PropertyClass && ObjectProperty->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
			{
				return false;
			}
		}
		return true;
	}

	bool EqualsExact(const FString& A, const FString& B)
	{
		return A.Equals(B, ESearchCase::CaseSensitive);
	}

	void SortExact(TArray<FString>& Strings)
	{
		Strings.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	}

	template <typename ValueType>
	TArray<FString> SortedKeys(const TMap<FString, ValueType>& Map)
	{
		TArray<FString> Keys;
		Map.GetKeys(Keys);
		SortExact(Keys);
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
	 * The value a property is diffed against: its value in the baseline object, or a default-initialized value
	 * if the baseline does not have the property (a variable added by this Blueprint).
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

	struct FComponentTarget
	{
		/** Current template, or null if inherited from a parent Blueprint and not overridden here yet. */
		UActorComponent* Template = nullptr;
		/** What the template's values are diffed against. */
		const UObject* Baseline = nullptr;
		UClass* Class = nullptr;
		/** Set for components added by a parent Blueprint; this Blueprint overrides them through its inheritable component handler. */
		TOptional<FComponentKey> InheritedKey;
	};

	/** Closest template above this Blueprint for an inherited component: a parent's override, else the original SCS template. */
	const UObject* FindInheritedArchetype(const UBlueprint* Blueprint, const FComponentKey& Key)
	{
		for (UBlueprintGeneratedClass* Class = Cast<UBlueprintGeneratedClass>(Blueprint->ParentClass.Get()); Class;
			 Class = Cast<UBlueprintGeneratedClass>(Class->GetSuperClass()))
		{
			if (Class->InheritableComponentHandler)
			{
				if (UActorComponent* Override = Class->InheritableComponentHandler->GetOverridenComponentTemplate(Key))
				{
					return Override;
				}
			}
		}
		return Key.GetOriginalTemplate();
	}

	/** Calls Func(Name, Target) for every component template the Blueprint's actors get: own SCS, inherited SCS, native. */
	void ForEachComponent(UBlueprint* Blueprint, TFunctionRef<void(const FString&, const FComponentTarget&)> Func)
	{
		if (Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentTemplate)
				{
					FComponentTarget Target;
					Target.Template = Node->ComponentTemplate;
					Target.Class = Node->ComponentTemplate->GetClass();
					Target.Baseline = Target.Class->GetDefaultObject();
					Func(Node->GetVariableName().ToString(), Target);
				}
			}
		}

		UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(false);
		for (UBlueprintGeneratedClass* Class = Cast<UBlueprintGeneratedClass>(Blueprint->ParentClass.Get()); Class;
			 Class = Cast<UBlueprintGeneratedClass>(Class->GetSuperClass()))
		{
			if (!Class->SimpleConstructionScript)
			{
				continue;
			}
			for (USCS_Node* Node : Class->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentClass)
				{
					const FComponentKey Key(Node);
					FComponentTarget Target;
					Target.Template = Handler ? Handler->GetOverridenComponentTemplate(Key) : nullptr;
					Target.Class = Node->ComponentClass;
					Target.Baseline = FindInheritedArchetype(Blueprint, Key);
					Target.InheritedKey = Key;
					Func(Node->GetVariableName().ToString(), Target);
				}
			}
		}

		if (Blueprint->GeneratedClass)
		{
			TArray<UObject*> Subobjects;
			Blueprint->GeneratedClass->GetDefaultObject()->GetDefaultSubobjects(Subobjects);
			for (UObject* Subobject : Subobjects)
			{
				if (UActorComponent* Component = Cast<UActorComponent>(Subobject))
				{
					FComponentTarget Target;
					Target.Template = Component;
					Target.Class = Component->GetClass();
					Target.Baseline = Component->GetArchetype();
					Func(Component->GetName(), Target);
				}
			}
		}
	}

	TOptional<FComponentTarget> FindComponent(UBlueprint* Blueprint, const FString& Name)
	{
		TOptional<FComponentTarget> Found;
		ForEachComponent(Blueprint, [&Found, &Name](const FString& ComponentName, const FComponentTarget& Target)
		{
			if (!Found.IsSet() && EqualsExact(ComponentName, Name))
			{
				Found = Target;
			}
		});
		return Found;
	}

	/** Suffix of the entry that holds a widget's layout in its parent panel, e.g. "Title.Slot". */
	const TCHAR* const SlotSuffix = TEXT(".Slot");

	/**
	 * Calls Func(Name, Object) for every widget in a Widget Blueprint's Designer tree (including named-slot content), and for its
	 * layout slot as "<Widget>.Slot". These are the design-time templates the Designer edits; compiling copies them into the class.
	 * Object names cannot contain '.', so names never clash with the slot entries.
	 */
	void ForEachWidget(UBlueprint* Blueprint, TFunctionRef<void(const FString&, UObject*)> Func)
	{
		const UBaseWidgetBlueprint* WidgetBlueprint = Cast<UBaseWidgetBlueprint>(Blueprint);
		UWidgetTree* Tree = WidgetBlueprint ? WidgetBlueprint->WidgetTree.Get() : nullptr;
		if (!Tree)
		{
			return;
		}
		Tree->ForEachWidget([&Func](UWidget* Widget)
		{
			if (!Widget)
			{
				return;
			}
			const FString Name = Widget->GetName();
			Func(Name, Widget);
			if (Widget->Slot)
			{
				Func(Name + SlotSuffix, Widget->Slot);
			}
		});
	}

	UObject* FindWidget(UBlueprint* Blueprint, const FString& Name)
	{
		UObject* Found = nullptr;
		ForEachWidget(Blueprint, [&Found, &Name](const FString& WidgetName, UObject* Object)
		{
			if (!Found && EqualsExact(WidgetName, Name))
			{
				Found = Object;
			}
		});
		return Found;
	}

	/** Finds a property by exact name. Sets OutError if it is missing or not synced. */
	const FProperty* FindSyncedProperty(const UClass* Class, const FString& Name, EObjectRole Role, FString* OutError = nullptr)
	{
		const FProperty* Property = Class ? FindFProperty<FProperty>(Class, FName(*Name)) : nullptr;
		if (!Property)
		{
			if (OutError) { *OutError = TEXT("unknown property"); }
			return nullptr;
		}
		if (!EqualsExact(Property->GetName(), Name))
		{
			if (OutError) { *OutError = FString::Printf(TEXT("unknown property (did you mean '%s'?)"), *Property->GetName()); }
			return nullptr;
		}
		if (!IsSyncable(Property, Role))
		{
			if (OutError) { *OutError = TEXT("property is not synced (not editable here, or transient, instanced or a component reference)"); }
			return nullptr;
		}
		return Property;
	}

	void CaptureObject(UObject* Object, const UObject* Baseline, EObjectRole Role, TMap<FString, FString>& Out)
	{
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			if (!IsSyncable(Property, Role))
			{
				continue;
			}
			const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
			const FBaselineValue Base(Property, Baseline);
			if (Property->Identical(Value, Base.Get(), PPF_None))
			{
				continue;
			}
			FString Text;
			Property->ExportTextItem_Direct(Text, Value, Base.Get(), Object, PPF_None);
			Out.Add(Property->GetName(), MoveTemp(Text));
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

	struct FWriteTarget
	{
		UClass* Class = nullptr;
		/** Null for an inherited component that is not overridden yet; the override is created on write. */
		UObject* Object = nullptr;
		const UObject* Baseline = nullptr;
		TOptional<FComponentKey> InheritedKey;
		UObject* ImportOwner = nullptr;
		EObjectRole Role = EObjectRole::Template;
	};

	struct FPendingWrite
	{
		FString Key;
		const FProperty* Property = nullptr;
		FWriteTarget Target;
		TUniquePtr<FValueBuffer> Value;
	};

	/** Validates one key and parses its value onto a copy of the baseline value. Text is null to reset the key to the baseline. */
	void PrepareWrite(const FString& Key, const FString& PropertyName, const FString* Text, const FWriteTarget& Target,
		FApplyResult& Result, TArray<FPendingWrite>& Writes)
	{
		FString Error;
		const FProperty* Property = FindSyncedProperty(Target.Class, PropertyName, Target.Role, &Error);
		if (!Property)
		{
			Result.Errors.Add(FString::Printf(TEXT("%s: %s"), *Key, *Error));
			return;
		}

		FPendingWrite Write;
		Write.Key = Key;
		Write.Property = Property;
		Write.Target = Target;
		Write.Value = MakeUnique<FValueBuffer>(Property);
		{
			const FBaselineValue Base(Property, Target.Baseline);
			Property->CopyCompleteValue(Write.Value->Get(), Base.Get());
		}
		if (Text && !ImportValue(Property, *Text, Write.Value->Get(), Target.ImportOwner, Error))
		{
			Result.Errors.Add(FString::Printf(TEXT("%s: %s"), *Key, *Error));
			return;
		}
		Writes.Add(MoveTemp(Write));
	}

	/** Prepares the writes for one object: every value in Wanted, and a reset for every value in Current that Wanted does not list. */
	void PrepareObject(const FString& Prefix, const TMap<FString, FString>* Wanted, const TMap<FString, FString>* Current,
		const FWriteTarget& Target, FApplyResult& Result, TArray<FPendingWrite>& Writes)
	{
		if (Wanted)
		{
			for (const FString& Property : SortedKeys(*Wanted))
			{
				PrepareWrite(Prefix + Property, Property, &Wanted->FindChecked(Property), Target, Result, Writes);
			}
		}
		if (Current)
		{
			for (const FString& Property : SortedKeys(*Current))
			{
				if (!Wanted || !Wanted->Contains(Property))
				{
					PrepareWrite(Prefix + Property, Property, nullptr, Target, Result, Writes);
				}
			}
		}
	}

	/** Writes the value to Target and to every archetype instance that was inheriting Target's value (child CDOs, loaded instances). */
	void WriteWithPropagation(UObject* Target, const FProperty* Property, const void* NewValue)
	{
		// Collect before writing: the helper finds instances whose value still equals Target's current value.
		TArray<UObject*> Inheritors;
		PropertyAccessUtil::GetArchetypeInstancesInheritingPropertyValue(Property, Target, Inheritors);

		FProperty* MutableProperty = const_cast<FProperty*>(Property);
		auto WriteOne = [MutableProperty, NewValue](UObject* Object)
		{
			Object->Modify();
			Object->PreEditChange(MutableProperty);
			MutableProperty->CopyCompleteValue(MutableProperty->ContainerPtrToValuePtr<void>(Object), NewValue);
			FPropertyChangedEvent Event(MutableProperty, EPropertyChangeType::ValueSet);
			Object->PostEditChangeProperty(Event);
		};

		WriteOne(Target);
		for (UObject* Inheritor : Inheritors)
		{
			WriteOne(Inheritor);
		}
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

	/** Object name -> property map, written at the top level of the document (indent level 1). */
	void AppendObjectMaps(FString& Out, const TMap<FString, TMap<FString, FString>>& Maps)
	{
		if (Maps.Num() == 0)
		{
			Out += TEXT("{}");
			return;
		}
		Out += TEXT("{\n");
		const TArray<FString> Names = SortedKeys(Maps);
		for (int32 i = 0; i < Names.Num(); ++i)
		{
			AppendIndent(Out, 2);
			AppendQuoted(Out, Names[i]);
			Out += TEXT(": ");
			AppendStringMap(Out, Maps.FindChecked(Names[i]), 2);
			Out += i + 1 < Names.Num() ? TEXT(",\n") : TEXT("\n");
		}
		Out += TEXT("  }");
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

	bool GetObjectField(const FJsonObject& Root, const TCHAR* Name, TSharedPtr<FJsonObject>& Out, FString& OutError)
	{
		const TSharedPtr<FJsonValue>* Value = Root.Values.Find(Name);
		if (!Value)
		{
			return true;
		}
		if (!Value->IsValid() || (*Value)->Type != EJson::Object)
		{
			OutError = FString::Printf(TEXT("'%s' must be an object"), Name);
			return false;
		}
		Out = (*Value)->AsObject();
		return true;
	}

	/** Reads an optional top-level field of object name -> property map, e.g. "components". Empty objects are dropped. */
	bool ReadObjectMaps(const FJsonObject& Root, const TCHAR* Name, TMap<FString, TMap<FString, FString>>& Out, FString& OutError)
	{
		TSharedPtr<FJsonObject> Object;
		if (!GetObjectField(Root, Name, Object, OutError))
		{
			return false;
		}
		if (!Object.IsValid())
		{
			return true;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			const FString Context = FString::Printf(TEXT("%s.%s"), Name, *Pair.Key);
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
			{
				OutError = Context + TEXT(" must be an object");
				return false;
			}
			TMap<FString, FString> Values;
			if (!ReadStringMap(*Pair.Value->AsObject(), Context, Values, OutError))
			{
				return false;
			}
			if (Values.Num() > 0)
			{
				Out.Add(Pair.Key, MoveTemp(Values));
			}
		}
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
}

EAssetKind GetAssetKind(const UObject* Asset)
{
	if (!Asset)
	{
		return EAssetKind::None;
	}
	if (Asset->IsA<UBaseWidgetBlueprint>())
	{
		return EAssetKind::WidgetBlueprint;
	}
	if (Asset->IsA<UBlueprint>())
	{
		return EAssetKind::Blueprint;
	}
	if (Asset->IsA<UDataAsset>())
	{
		return EAssetKind::DataAsset;
	}
	return EAssetKind::None;
}

EAssetKind GetAssetKind(const FAssetData& Asset)
{
	const FTopLevelAssetPath ClassPath = Asset.AssetClassPath;
	if (IsChildOfClass(ClassPath, UBaseWidgetBlueprint::StaticClass()))
	{
		return EAssetKind::WidgetBlueprint;
	}
	if (IsChildOfClass(ClassPath, UBlueprint::StaticClass()))
	{
		return EAssetKind::Blueprint;
	}
	if (IsChildOfClass(ClassPath, UDataAsset::StaticClass()))
	{
		return EAssetKind::DataAsset;
	}
	return EAssetKind::None;
}

const TCHAR* LexToString(EAssetKind Kind)
{
	switch (Kind)
	{
	case EAssetKind::Blueprint: return TEXT("Blueprint");
	case EAssetKind::WidgetBlueprint: return TEXT("Widget Blueprint");
	case EAssetKind::DataAsset: return TEXT("Data Asset");
	default: return TEXT("None");
	}
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

	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	if (!Blueprint)
	{
		// A data asset: its own values against its archetype, the class defaults.
		Doc.Parent = Asset->GetClass()->GetPathName();
		CaptureObject(Asset, Asset->GetArchetype(), EObjectRole::Instance, Doc.Defaults);
		return Doc;
	}

	Doc.Parent = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString();
	UClass* GeneratedClass = Blueprint->GeneratedClass;
	if (!GeneratedClass)
	{
		return Doc;
	}
	UClass* SuperClass = GeneratedClass->GetSuperClass();
	CaptureObject(GeneratedClass->GetDefaultObject(), SuperClass ? SuperClass->GetDefaultObject() : nullptr, EObjectRole::Template, Doc.Defaults);

	ForEachComponent(Blueprint, [&Doc](const FString& Name, const FComponentTarget& Target)
	{
		if (Target.Template)
		{
			TMap<FString, FString> Values;
			CaptureObject(Target.Template, Target.Baseline, EObjectRole::Template, Values);
			if (Values.Num() > 0)
			{
				Doc.Components.Add(Name, MoveTemp(Values));
			}
		}
	});

	ForEachWidget(Blueprint, [&Doc](const FString& Name, UObject* Widget)
	{
		TMap<FString, FString> Values;
		CaptureObject(Widget, Widget->GetClass()->GetDefaultObject(), EObjectRole::Template, Values);
		if (Values.Num() > 0)
		{
			Doc.Widgets.Add(Name, MoveTemp(Values));
		}
	});
	return Doc;
}

FString ToJson(const FAssetDefaults& Doc)
{
	FString Out;
	Out += TEXT("{\n");
	Out += FString::Printf(TEXT("  \"schema\": %d,\n"), FAssetDefaults::SchemaVersion);
	Out += TEXT("  \"scope\": \"defaults\",\n");
	Out += TEXT("  \"asset\": ");
	AppendQuoted(Out, Doc.Asset);
	Out += TEXT(",\n  \"parent\": ");
	AppendQuoted(Out, Doc.Parent);
	Out += TEXT(",\n  \"defaults\": ");
	AppendStringMap(Out, Doc.Defaults, 1);
	// Only Blueprints have components and only Widget Blueprints have widgets; leave out what is empty.
	if (Doc.Components.Num() > 0)
	{
		Out += TEXT(",\n  \"components\": ");
		AppendObjectMaps(Out, Doc.Components);
	}
	if (Doc.Widgets.Num() > 0)
	{
		Out += TEXT(",\n  \"widgets\": ");
		AppendObjectMaps(Out, Doc.Widgets);
	}
	Out += TEXT("\n}\n");
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

	static const TSet<FString> KnownFields = { TEXT("schema"), TEXT("scope"), TEXT("asset"), TEXT("parent"), TEXT("defaults"), TEXT("components"), TEXT("widgets") };
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
	{
		if (!KnownFields.Contains(Pair.Key))
		{
			OutError = FString::Printf(TEXT("unknown field '%s'"), *Pair.Key);
			return false;
		}
	}

	FString Scope;
	if (!Root->TryGetStringField(TEXT("scope"), Scope) || !EqualsExact(Scope, TEXT("defaults")))
	{
		OutError = TEXT("'scope' must be \"defaults\"");
		return false;
	}
	if (!Root->TryGetStringField(TEXT("asset"), OutDoc.Asset) || OutDoc.Asset.IsEmpty())
	{
		OutError = TEXT("'asset' must be the asset's package name");
		return false;
	}
	Root->TryGetStringField(TEXT("parent"), OutDoc.Parent);

	TSharedPtr<FJsonObject> Defaults;
	if (!GetObjectField(*Root, TEXT("defaults"), Defaults, OutError)
		|| (Defaults.IsValid() && !ReadStringMap(*Defaults, TEXT("defaults"), OutDoc.Defaults, OutError)))
	{
		return false;
	}
	return ReadObjectMaps(*Root, TEXT("components"), OutDoc.Components, OutError)
		&& ReadObjectMaps(*Root, TEXT("widgets"), OutDoc.Widgets, OutError);
}

TMap<FString, FString> Flatten(const FAssetDefaults& Doc)
{
	TMap<FString, FString> Result;
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
	return Result;
}

bool ValuesEqual(const FAssetDefaults& A, const FAssetDefaults& B)
{
	const TMap<FString, FString> FlatA = Flatten(A);
	const TMap<FString, FString> FlatB = Flatten(B);
	if (FlatA.Num() != FlatB.Num())
	{
		return false;
	}
	for (const TPair<FString, FString>& Pair : FlatA)
	{
		const FString* Other = FlatB.Find(Pair.Key);
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

	const EAssetKind Kind = GetAssetKind(Asset);
	if (Kind == EAssetKind::None)
	{
		Result.Errors.Add(TEXT("not a Blueprint or a data asset"));
		return Result;
	}
	const FString PackageName = Asset->GetPackage()->GetName();
	if (!EqualsExact(Doc.Asset, PackageName))
	{
		Result.Errors.Add(FString::Printf(TEXT("the JSON is for %s, not %s"), *Doc.Asset, *PackageName));
		return Result;
	}

	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	FWriteTarget DefaultsTarget;
	if (Blueprint)
	{
		UClass* GeneratedClass = Blueprint->GeneratedClass;
		if (!GeneratedClass)
		{
			Result.Errors.Add(TEXT("the Blueprint has no generated class; compile it first"));
			return Result;
		}
		UClass* SuperClass = GeneratedClass->GetSuperClass();
		DefaultsTarget.Class = GeneratedClass;
		DefaultsTarget.Object = GeneratedClass->GetDefaultObject();
		DefaultsTarget.Baseline = SuperClass ? SuperClass->GetDefaultObject() : nullptr;
		DefaultsTarget.ImportOwner = DefaultsTarget.Object;
	}
	else
	{
		DefaultsTarget.Class = Asset->GetClass();
		DefaultsTarget.Object = Asset;
		DefaultsTarget.Baseline = Asset->GetArchetype();
		DefaultsTarget.ImportOwner = Asset;
		DefaultsTarget.Role = EObjectRole::Instance;
	}

	// The document is a full snapshot: what it does not list goes back to the archetype. Current says what the asset overrides now.
	const FAssetDefaults Current = Capture(Asset);
	if (!Doc.Parent.IsEmpty() && !EqualsExact(Doc.Parent, Current.Parent))
	{
		Result.Warnings.Add(FString::Printf(TEXT("parent class differs: JSON has %s, the asset has %s"), *Doc.Parent, *Current.Parent));
	}

	auto GetComponentTarget = [&](const FString& Name) -> TOptional<FWriteTarget>
	{
		const TOptional<FComponentTarget> Component = Blueprint ? FindComponent(Blueprint, Name) : TOptional<FComponentTarget>();
		if (!Component)
		{
			return {};
		}
		FWriteTarget Target;
		Target.Class = Component->Class;
		Target.Object = Component->Template;
		Target.Baseline = Component->Baseline;
		if (!Component->Template)
		{
			Target.InheritedKey = Component->InheritedKey;
		}
		Target.ImportOwner = Component->Template ? static_cast<UObject*>(Component->Template) : DefaultsTarget.Object;
		return Target;
	};

	auto GetWidgetTarget = [&](const FString& Name) -> TOptional<FWriteTarget>
	{
		UObject* Widget = Blueprint ? FindWidget(Blueprint, Name) : nullptr;
		if (!Widget)
		{
			return {};
		}
		FWriteTarget Target;
		Target.Class = Widget->GetClass();
		Target.Object = Widget;
		Target.Baseline = Target.Class->GetDefaultObject();
		Target.ImportOwner = Widget;
		return Target;
	};

	auto PrepareObjects = [&](const TCHAR* Section, const TCHAR* What, const TMap<FString, TMap<FString, FString>>& Wanted,
		const TMap<FString, TMap<FString, FString>>& Have, TFunctionRef<TOptional<FWriteTarget>(const FString&)> GetTarget,
		TArray<FPendingWrite>& Writes)
	{
		TSet<FString> Names;
		for (const TPair<FString, TMap<FString, FString>>& Pair : Wanted) { Names.Add(Pair.Key); }
		for (const TPair<FString, TMap<FString, FString>>& Pair : Have) { Names.Add(Pair.Key); }
		TArray<FString> Sorted = Names.Array();
		SortExact(Sorted);
		for (const FString& Name : Sorted)
		{
			const TOptional<FWriteTarget> Target = GetTarget(Name);
			if (!Target)
			{
				Result.Errors.Add(FString::Printf(TEXT("%s.%s: unknown %s"), Section, *Name, What));
				continue;
			}
			PrepareObject(Name + TEXT("."), Wanted.Find(Name), Have.Find(Name), *Target, Result, Writes);
		}
	};

	// Pass 1: validate everything and parse the values. Nothing is written if anything is wrong.
	TArray<FPendingWrite> Writes;
	PrepareObject(FString(), &Doc.Defaults, &Current.Defaults, DefaultsTarget, Result, Writes);
	PrepareObjects(TEXT("components"), TEXT("component"), Doc.Components, Current.Components, GetComponentTarget, Writes);
	PrepareObjects(TEXT("widgets"), TEXT("widget"), Doc.Widgets, Current.Widgets, GetWidgetTarget, Writes);
	if (Result.HasErrors())
	{
		return Result;
	}

	// Pass 2: write the values that differ.
	for (FPendingWrite& Write : Writes)
	{
		UObject* Target = Write.Target.Object;
		if (!Target && Write.Target.InheritedKey.IsSet())
		{
			// Inherited component: an earlier write in this pass may have created the override already.
			UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(false);
			Target = Handler ? Handler->GetOverridenComponentTemplate(*Write.Target.InheritedKey) : nullptr;
		}

		const FBaselineValue Base(Write.Property, Write.Target.Baseline);
		const void* CurrentValue = Target ? Write.Property->ContainerPtrToValuePtr<void>(Target) : Base.Get();
		if (Write.Property->Identical(CurrentValue, Write.Value->Get(), PPF_None))
		{
			continue;
		}

		if (!Target)
		{
			UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(true);
			Target = Handler ? Handler->CreateOverridenComponentTemplate(*Write.Target.InheritedKey) : nullptr;
			if (!Target)
			{
				Result.Errors.Add(FString::Printf(TEXT("%s: cannot create an override for the inherited component"), *Write.Key));
				continue;
			}
		}

		WriteWithPropagation(Target, Write.Property, Write.Value->Get());
		Result.Changed.Add(Write.Key);
	}
	return Result;
}

void MarkModified(UObject* Asset)
{
	if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
	else
	{
		Asset->MarkPackageDirty();
	}
}

FApplyResult Import(UObject* Asset, const FAssetDefaults& Doc)
{
	FApplyResult Result = Apply(Asset, Doc);
	if (Result.Changed.Num() > 0)
	{
		MarkModified(Asset);
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		}
	}
	return Result;
}

int32 GetApplyOrder(const UObject* Asset)
{
	const UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	if (!Blueprint)
	{
		// Data assets last: their class may be a Blueprint whose defaults change in the same batch.
		return MAX_int32;
	}
	int32 Depth = 0;
	for (const UClass* Class = Blueprint->ParentClass.Get(); Class; Class = Class->GetSuperClass())
	{
		if (Cast<UBlueprintGeneratedClass>(Class))
		{
			++Depth;
		}
	}
	return Depth;
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
