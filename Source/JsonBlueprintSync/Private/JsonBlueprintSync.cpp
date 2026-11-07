#include "JsonBlueprintSync.h"

#include "JsonBlueprintSyncModule.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Misc/StringOutputDevice.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/UnrealType.h"

namespace JsonBlueprintSync
{
namespace
{
	constexpr EPropertyFlags SkippedPropertyFlags =
		CPF_Transient | CPF_DuplicateTransient | CPF_DisableEditOnTemplate | CPF_EditConst | CPF_Deprecated
		| CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance;

	bool IsSyncable(const FProperty* Property)
	{
		if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(SkippedPropertyFlags) || Property->ArrayDim != 1)
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

	/** Finds a property by exact name. Sets OutError if it is missing or not synced. */
	const FProperty* FindSyncedProperty(const UClass* Class, const FString& Name, FString* OutError = nullptr)
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
		if (!IsSyncable(Property))
		{
			if (OutError) { *OutError = TEXT("property is not synced (not editable on defaults, or transient, instanced or a component reference)"); }
			return nullptr;
		}
		return Property;
	}

	void CaptureObject(UObject* Object, const UObject* Baseline, TMap<FString, FString>& Out)
	{
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			const FProperty* Property = *It;
			if (!IsSyncable(Property))
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

	bool IsCaptured(const FBlueprintDefaults& Doc, const FString& Key)
	{
		FString Component, Property;
		if (Key.Split(TEXT("."), &Component, &Property))
		{
			const TMap<FString, FString>* Values = Doc.Components.Find(Component);
			return Values && Values->Contains(Property);
		}
		return Doc.Defaults.Contains(Key);
	}

	bool KeyExists(UBlueprint* Blueprint, const FString& Key)
	{
		FString Component, Property;
		if (Key.Split(TEXT("."), &Component, &Property))
		{
			const TOptional<FComponentTarget> Target = FindComponent(Blueprint, Component);
			return Target.IsSet() && FindSyncedProperty(Target->Class, Property);
		}
		return FindSyncedProperty(Blueprint->GeneratedClass, Key) != nullptr;
	}

	bool ImportValue(const FProperty* Property, const FString& Text, void* Dest, UObject* Owner, FString& OutError)
	{
		// Struct import reports unknown fields only while LogExec is at Verbose (FProperty::ImportSingleProperty).
		// Raise it, so that misspelled fields reach the error device instead of being dropped silently.
		const ELogVerbosity::Type OldVerbosity = LogExec.GetVerbosity();
		LogExec.SetVerbosity(ELogVerbosity::Verbose);
		ON_SCOPE_EXIT { LogExec.SetVerbosity(OldVerbosity); };

		FStringOutputDevice Errors;
		const TCHAR* End = *Text;
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		if (StructProperty && StructProperty->Struct == FInstancedStruct::StaticStruct())
		{
			// FInstancedStruct's parser fails on an unknown struct type after reading the type name, and the engine then retries
			// with the generic struct parser, which accepts the "(...)" that is left and keeps the old value. Call it directly so
			// that the failure stands.
			if (!static_cast<FInstancedStruct*>(Dest)->ImportTextItem(End, PPF_None, Owner, &Errors))
			{
				End = nullptr;
			}
		}
		else
		{
			End = Property->ImportText_Direct(*Text, Dest, Owner, PPF_None, &Errors);
		}
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
	};

	struct FPendingWrite
	{
		FString Key;
		const FProperty* Property = nullptr;
		FWriteTarget Target;
		TUniquePtr<FValueBuffer> Value;
	};

	/** Validates one key and parses its value onto a copy of the baseline value. Text is null for a reset. */
	void PrepareWrite(const FString& Key, const FString& PropertyName, const FString* Text, const FWriteTarget& Target,
		FApplyResult& Result, TArray<FPendingWrite>& Writes)
	{
		FString Error;
		const FProperty* Property = FindSyncedProperty(Target.Class, PropertyName, &Error);
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
}

FString GetDefaultJsonRoot()
{
	FString Dir = TEXT("JsonBlueprintSync");
	if (GConfig)
	{
		GConfig->GetString(TEXT("JsonBlueprintSync"), TEXT("JsonDir"), Dir, GEditorIni);
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

FBlueprintDefaults Capture(UBlueprint* Blueprint, const FBlueprintDefaults* Previous)
{
	FBlueprintDefaults Doc;
	Doc.Blueprint = Blueprint->GetPackage()->GetName();
	Doc.Parent = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString();

	UClass* GeneratedClass = Blueprint->GeneratedClass;
	if (!GeneratedClass)
	{
		return Doc;
	}
	UClass* SuperClass = GeneratedClass->GetSuperClass();
	CaptureObject(GeneratedClass->GetDefaultObject(), SuperClass ? SuperClass->GetDefaultObject() : nullptr, Doc.Defaults);

	ForEachComponent(Blueprint, [&Doc](const FString& Name, const FComponentTarget& Target)
	{
		if (Target.Template)
		{
			TMap<FString, FString> Values;
			CaptureObject(Target.Template, Target.Baseline, Values);
			if (Values.Num() > 0)
			{
				Doc.Components.Add(Name, MoveTemp(Values));
			}
		}
	});

	if (Previous)
	{
		// A key that was listed before but no longer differs was reset locally. Keep it in "reset" so the reset reaches other machines.
		TSet<FString> Candidates(Previous->Reset);
		for (const TPair<FString, FString>& Pair : Previous->Defaults)
		{
			Candidates.Add(Pair.Key);
		}
		for (const TPair<FString, TMap<FString, FString>>& Component : Previous->Components)
		{
			for (const TPair<FString, FString>& Pair : Component.Value)
			{
				Candidates.Add(Component.Key + TEXT(".") + Pair.Key);
			}
		}
		for (const FString& Key : Candidates)
		{
			if (!IsCaptured(Doc, Key) && KeyExists(Blueprint, Key))
			{
				Doc.Reset.Add(Key);
			}
		}
	}
	SortExact(Doc.Reset);
	return Doc;
}

FString ToJson(const FBlueprintDefaults& Doc)
{
	FString Out;
	Out += TEXT("{\n");
	Out += FString::Printf(TEXT("  \"schema\": %d,\n"), FBlueprintDefaults::SchemaVersion);
	Out += TEXT("  \"scope\": \"defaults\",\n");
	Out += TEXT("  \"blueprint\": ");
	AppendQuoted(Out, Doc.Blueprint);
	Out += TEXT(",\n  \"parent\": ");
	AppendQuoted(Out, Doc.Parent);
	Out += TEXT(",\n  \"defaults\": ");
	AppendStringMap(Out, Doc.Defaults, 1);
	Out += TEXT(",\n  \"components\": ");
	if (Doc.Components.Num() == 0)
	{
		Out += TEXT("{}");
	}
	else
	{
		Out += TEXT("{\n");
		const TArray<FString> Names = SortedKeys(Doc.Components);
		for (int32 i = 0; i < Names.Num(); ++i)
		{
			AppendIndent(Out, 2);
			AppendQuoted(Out, Names[i]);
			Out += TEXT(": ");
			AppendStringMap(Out, Doc.Components.FindChecked(Names[i]), 2);
			Out += i + 1 < Names.Num() ? TEXT(",\n") : TEXT("\n");
		}
		Out += TEXT("  }");
	}
	Out += TEXT(",\n  \"reset\": ");
	if (Doc.Reset.Num() == 0)
	{
		Out += TEXT("[]");
	}
	else
	{
		TArray<FString> Reset = Doc.Reset;
		SortExact(Reset);
		Out += TEXT("[\n");
		for (int32 i = 0; i < Reset.Num(); ++i)
		{
			AppendIndent(Out, 2);
			AppendQuoted(Out, Reset[i]);
			Out += i + 1 < Reset.Num() ? TEXT(",\n") : TEXT("\n");
		}
		Out += TEXT("  ]");
	}
	Out += TEXT("\n}\n");
	return Out;
}

bool FromJson(const FString& Json, FBlueprintDefaults& OutDoc, FString& OutError)
{
	OutDoc = FBlueprintDefaults();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("not valid JSON: %s"), *Reader->GetErrorMessage());
		return false;
	}

	static const TSet<FString> KnownFields = { TEXT("schema"), TEXT("scope"), TEXT("blueprint"), TEXT("parent"), TEXT("defaults"), TEXT("components"), TEXT("reset") };
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
	{
		if (!KnownFields.Contains(Pair.Key))
		{
			OutError = FString::Printf(TEXT("unknown field '%s'"), *Pair.Key);
			return false;
		}
	}

	int32 Schema = 0;
	if (!Root->TryGetNumberField(TEXT("schema"), Schema) || Schema != FBlueprintDefaults::SchemaVersion)
	{
		OutError = FString::Printf(TEXT("'schema' must be %d"), FBlueprintDefaults::SchemaVersion);
		return false;
	}
	FString Scope;
	if (!Root->TryGetStringField(TEXT("scope"), Scope) || !EqualsExact(Scope, TEXT("defaults")))
	{
		OutError = TEXT("'scope' must be \"defaults\"");
		return false;
	}
	if (!Root->TryGetStringField(TEXT("blueprint"), OutDoc.Blueprint) || OutDoc.Blueprint.IsEmpty())
	{
		OutError = TEXT("'blueprint' must be the Blueprint's package name");
		return false;
	}
	Root->TryGetStringField(TEXT("parent"), OutDoc.Parent);

	TSharedPtr<FJsonObject> Defaults;
	if (!GetObjectField(*Root, TEXT("defaults"), Defaults, OutError)
		|| (Defaults.IsValid() && !ReadStringMap(*Defaults, TEXT("defaults"), OutDoc.Defaults, OutError)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Components;
	if (!GetObjectField(*Root, TEXT("components"), Components, OutError))
	{
		return false;
	}
	if (Components.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Components->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
			{
				OutError = FString::Printf(TEXT("components.%s must be an object"), *Pair.Key);
				return false;
			}
			TMap<FString, FString>& Values = OutDoc.Components.Add(Pair.Key);
			if (!ReadStringMap(*Pair.Value->AsObject(), TEXT("components.") + Pair.Key, Values, OutError))
			{
				return false;
			}
		}
	}

	if (const TSharedPtr<FJsonValue>* Reset = Root->Values.Find(TEXT("reset")))
	{
		if (!Reset->IsValid() || (*Reset)->Type != EJson::Array)
		{
			OutError = TEXT("'reset' must be an array of strings");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Item : (*Reset)->AsArray())
		{
			if (!Item.IsValid() || Item->Type != EJson::String)
			{
				OutError = TEXT("'reset' must be an array of strings");
				return false;
			}
			OutDoc.Reset.Add(Item->AsString());
		}
		SortExact(OutDoc.Reset);
	}
	return true;
}

FApplyResult Apply(UBlueprint* Blueprint, const FBlueprintDefaults& Doc)
{
	FApplyResult Result;

	UClass* GeneratedClass = Blueprint->GeneratedClass;
	if (!GeneratedClass)
	{
		Result.Errors.Add(TEXT("the Blueprint has no generated class; compile it first"));
		return Result;
	}
	const FString PackageName = Blueprint->GetPackage()->GetName();
	if (!EqualsExact(Doc.Blueprint, PackageName))
	{
		Result.Errors.Add(FString::Printf(TEXT("the JSON is for %s, not %s"), *Doc.Blueprint, *PackageName));
		return Result;
	}
	const FString ParentPath = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString();
	if (!Doc.Parent.IsEmpty() && !EqualsExact(Doc.Parent, ParentPath))
	{
		Result.Warnings.Add(FString::Printf(TEXT("parent class differs: JSON has %s, the Blueprint has %s"), *Doc.Parent, *ParentPath));
	}

	UObject* ClassDefaults = GeneratedClass->GetDefaultObject();
	UClass* SuperClass = GeneratedClass->GetSuperClass();

	FWriteTarget DefaultsTarget;
	DefaultsTarget.Class = GeneratedClass;
	DefaultsTarget.Object = ClassDefaults;
	DefaultsTarget.Baseline = SuperClass ? SuperClass->GetDefaultObject() : nullptr;
	DefaultsTarget.ImportOwner = ClassDefaults;

	TMap<FString, TOptional<FWriteTarget>> ComponentTargets;
	auto GetComponentTarget = [&](const FString& Name) -> const FWriteTarget*
	{
		if (const TOptional<FWriteTarget>* Cached = ComponentTargets.Find(Name))
		{
			return Cached->GetPtrOrNull();
		}
		TOptional<FWriteTarget> Entry;
		if (const TOptional<FComponentTarget> Component = FindComponent(Blueprint, Name))
		{
			FWriteTarget Target;
			Target.Class = Component->Class;
			Target.Object = Component->Template;
			Target.Baseline = Component->Baseline;
			if (!Component->Template)
			{
				Target.InheritedKey = Component->InheritedKey;
			}
			Target.ImportOwner = Component->Template ? static_cast<UObject*>(Component->Template) : ClassDefaults;
			Entry = MoveTemp(Target);
		}
		return ComponentTargets.Add(Name, MoveTemp(Entry)).GetPtrOrNull();
	};

	// Pass 1: validate everything and parse the values. Nothing is written if anything is wrong.
	TArray<FPendingWrite> Writes;
	for (const FString& Key : SortedKeys(Doc.Defaults))
	{
		PrepareWrite(Key, Key, &Doc.Defaults.FindChecked(Key), DefaultsTarget, Result, Writes);
	}
	for (const FString& Name : SortedKeys(Doc.Components))
	{
		const FWriteTarget* Target = GetComponentTarget(Name);
		if (!Target)
		{
			Result.Errors.Add(FString::Printf(TEXT("components.%s: unknown component"), *Name));
			continue;
		}
		const FWriteTarget TargetCopy = *Target;
		const TMap<FString, FString>& Values = Doc.Components.FindChecked(Name);
		for (const FString& Property : SortedKeys(Values))
		{
			PrepareWrite(Name + TEXT(".") + Property, Property, &Values.FindChecked(Property), TargetCopy, Result, Writes);
		}
	}
	for (const FString& Key : Doc.Reset)
	{
		if (IsCaptured(Doc, Key))
		{
			Result.Errors.Add(FString::Printf(TEXT("%s: listed in both a value and 'reset'"), *Key));
			continue;
		}
		FString Component, Property;
		if (Key.Split(TEXT("."), &Component, &Property))
		{
			const FWriteTarget* Target = GetComponentTarget(Component);
			if (!Target)
			{
				Result.Errors.Add(FString::Printf(TEXT("reset %s: unknown component"), *Key));
				continue;
			}
			const FWriteTarget TargetCopy = *Target;
			PrepareWrite(Key, Property, nullptr, TargetCopy, Result, Writes);
		}
		else
		{
			PrepareWrite(Key, Key, nullptr, DefaultsTarget, Result, Writes);
		}
	}
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
		const void* Current = Target ? Write.Property->ContainerPtrToValuePtr<void>(Target) : Base.Get();
		if (Write.Property->Identical(Current, Write.Value->Get(), PPF_None))
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

FApplyResult Import(UBlueprint* Blueprint, const FBlueprintDefaults& Doc)
{
	FApplyResult Result = Apply(Blueprint, Doc);
	if (Result.Changed.Num() > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	}
	return Result;
}

int32 GetBlueprintDepth(const UBlueprint* Blueprint)
{
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

bool LoadFile(const FString& Path, FBlueprintDefaults& OutDoc, FString& OutError)
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

bool SaveFileIfChanged(const FString& Path, const FBlueprintDefaults& Doc, bool& bOutWritten)
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
}

namespace JsonBlueprintSync
{
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

UBlueprint* LoadBlueprint(const FString& PackageName)
{
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	return LoadObject<UBlueprint>(nullptr, *ObjectPath);
}

FString HashText(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*NormalizeLineEndings(Text));
	return FMD5::HashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}
}
