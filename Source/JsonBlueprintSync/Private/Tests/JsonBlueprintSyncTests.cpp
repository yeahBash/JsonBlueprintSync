#include "JsonBlueprintSync.h"
#include "JsonBlueprintSyncSettings.h"
#include "../SyncState.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetData.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PointLightComponent.h"
#include "Components/TextBlock.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/Engine.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace JsonBlueprintSync::Tests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	UPackage* CreateTestPackage(const FString& Name)
	{
		UPackage* Package = CreatePackage(*(TEXT("/Temp/JsonBlueprintSyncTests/") + Name));
		Package->SetPackageFlags(PKG_InMemoryOnly);
		return Package;
	}

	FEdGraphPinType DoubleType()
	{
		return FEdGraphPinType(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
	}

	/**
	 * Parent Blueprint (Actor) with variables FlickerSpeed (double), Tint (LinearColor), Steps (int array) and a point light
	 * component "Light", plus a child Blueprint of it. Both live in transient /Temp packages.
	 */
	struct FTestBlueprints
	{
		UBlueprint* Parent = nullptr;
		UBlueprint* Child = nullptr;

		FTestBlueprints()
		{
			const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Parent = Create(AActor::StaticClass(), TEXT("BP_Parent_") + Suffix);

			FEdGraphPinType ColorType(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FLinearColor>::Get(), EPinContainerType::None, false, FEdGraphTerminalType());
			FEdGraphPinType IntArrayType(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, EPinContainerType::Array, false, FEdGraphTerminalType());
			FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("FlickerSpeed"), DoubleType());
			FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("Tint"), ColorType);
			FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("Steps"), IntArrayType);
			// Instance-editable, so placed instances can override it. Non-instance-editable variables are reset from the CDO
			// whenever the construction script reruns (AActor::ResetPropertiesForConstruction).
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Parent, TEXT("FlickerSpeed"), false);

			USCS_Node* Light = Parent->SimpleConstructionScript->CreateNode(UPointLightComponent::StaticClass(), TEXT("Light"));
			Parent->SimpleConstructionScript->AddNode(Light);
			FKismetEditorUtilities::CompileBlueprint(Parent, EBlueprintCompileOptions::SkipGarbageCollection);

			Child = Create(Parent->GeneratedClass, TEXT("BP_Child_") + Suffix);
		}

		static UBlueprint* Create(UClass* ParentClass, const FString& Name)
		{
			return FKismetEditorUtilities::CreateBlueprint(ParentClass, CreateTestPackage(Name), FName(*Name), BPTYPE_Normal);
		}
	};

	/** Editor world for spawning instances; destroyed with the scope. */
	struct FTestWorld
	{
		UWorld* World = nullptr;

		FTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}
		~FTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		AActor* Spawn(UBlueprint* Blueprint, const TCHAR* Name) const
		{
			FActorSpawnParameters Params;
			Params.Name = Name;
			return World->SpawnActor<AActor>(Blueprint->GeneratedClass, Params);
		}

		/** Compiling reinstances actors; find the current one by name. */
		AActor* Find(const TCHAR* Name) const
		{
			return FindObject<AActor>(World->PersistentLevel, Name);
		}
	};

	FAssetDefaults MakeDoc(const UObject* Asset)
	{
		FAssetDefaults Doc;
		Doc.Asset = Asset->GetPackage()->GetName();
		return Doc;
	}

	double GetDouble(const UObject* Object, const TCHAR* Name)
	{
		const FDoubleProperty* Property = FindFProperty<FDoubleProperty>(Object->GetClass(), Name);
		return Property ? Property->GetPropertyValue_InContainer(Object) : -1.0;
	}

	void SetDouble(UObject* Object, const TCHAR* Name, double Value)
	{
		if (const FDoubleProperty* Property = FindFProperty<FDoubleProperty>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, Value);
		}
	}

	FLinearColor GetTint(const UObject* Object)
	{
		const FStructProperty* Property = FindFProperty<FStructProperty>(Object->GetClass(), TEXT("Tint"));
		return Property ? *Property->ContainerPtrToValuePtr<FLinearColor>(Object) : FLinearColor::Black;
	}

	void SetTint(UObject* Object, const FLinearColor& Value)
	{
		if (const FStructProperty* Property = FindFProperty<FStructProperty>(Object->GetClass(), TEXT("Tint")))
		{
			*Property->ContainerPtrToValuePtr<FLinearColor>(Object) = Value;
		}
	}

	UPointLightComponent* GetTemplateLight(UBlueprint* Blueprint)
	{
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (Node->GetVariableName() == TEXT("Light"))
			{
				return Cast<UPointLightComponent>(Node->ComponentTemplate);
			}
		}
		return nullptr;
	}

	bool ReportErrors(FAutomationTestBase& Test, const FApplyResult& Result)
	{
		for (const FString& Error : Result.Errors)
		{
			Test.AddError(FString::Printf(TEXT("Apply error: %s"), *Error));
		}
		return !Result.HasErrors();
	}

	bool ImportOk(FAutomationTestBase& Test, UObject* Asset, const FAssetDefaults& Doc)
	{
		return ReportErrors(Test, Import(Asset, Doc));
	}

	FString JoinKeys(const TArray<FKeyDiff>& Diffs)
	{
		TArray<FString> Keys;
		for (const FKeyDiff& Diff : Diffs)
		{
			Keys.Add(Diff.Key);
		}
		return FString::Join(Keys, TEXT(","));
	}

	const FKeyDiff* FindDiff(const TArray<FKeyDiff>& Diffs, const TCHAR* Key)
	{
		return Diffs.FindByPredicate([Key](const FKeyDiff& Diff) { return Diff.Key == Key; });
	}
}

using namespace JsonBlueprintSync;
using namespace JsonBlueprintSync::Tests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncFormatTest, "JsonBlueprintSync.Format", TestFlags)
bool FJsonBlueprintSyncFormatTest::RunTest(const FString&)
{
	FAssetDefaults Doc;
	Doc.Asset = TEXT("/Game/A/BP_B");
	Doc.Parent = TEXT("/Script/Engine.Actor");
	Doc.Defaults.Add(TEXT("b"), TEXT("Quote \" back \\ tab \t"));
	Doc.Defaults.Add(TEXT("Z"), TEXT("(1,2)"));
	Doc.Defaults.Add(TEXT("a"), TEXT("1.0"));
	Doc.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("5000.0"));

	const FString Json = ToJson(Doc);
	TestFalse(TEXT("No CR"), Json.Contains(TEXT("\r")));
	TestTrue(TEXT("Trailing newline"), Json.EndsWith(TEXT("}\n")));
	TestTrue(TEXT("Schema 2"), Json.Contains(TEXT("\"schema\": 2,")));
	TestTrue(TEXT("Keys sorted case-sensitively"), Json.Find(TEXT("\"Z\"")) < Json.Find(TEXT("\"a\"")) && Json.Find(TEXT("\"a\"")) < Json.Find(TEXT("\"b\"")));
	{
		// Field names with their colon, so that "scope": "defaults" does not match "defaults".
		TArray<int32> Positions;
		for (const TCHAR* Field : { TEXT("\"schema\":"), TEXT("\"scope\":"), TEXT("\"asset\":"), TEXT("\"parent\":"), TEXT("\"defaults\":"), TEXT("\"components\":") })
		{
			Positions.Add(Json.Find(Field, ESearchCase::CaseSensitive));
		}
		bool bOrdered = Positions[0] >= 0;
		for (int32 Index = 1; Index < Positions.Num(); ++Index)
		{
			bOrdered &= Positions[Index - 1] < Positions[Index];
		}
		TestTrue(TEXT("Fixed top-level order"), bOrdered);
	}
	TestFalse(TEXT("No reset field"), Json.Contains(TEXT("\"reset\"")));

	FAssetDefaults Parsed;
	FString Error;
	TestTrue(TEXT("Parses its own output"), FromJson(Json, Parsed, Error));
	TestEqual(TEXT("Round trip is byte-identical"), ToJson(Parsed), Json);
	TestEqual(TEXT("Escapes survive"), Parsed.Defaults.FindRef(TEXT("b")), FString(TEXT("Quote \" back \\ tab \t")));

	const FString Empty = ToJson(FAssetDefaults());
	TestTrue(TEXT("Empty defaults"), Empty.Contains(TEXT("\"defaults\": {}")));
	TestFalse(TEXT("No components section without components"), Empty.Contains(TEXT("\"components\"")));
	TestFalse(TEXT("No widgets section without widgets"), Empty.Contains(TEXT("\"widgets\"")));

	auto Rejects = [this](const TCHAR* What, const FString& Text, const TCHAR* ExpectedMessage = nullptr)
	{
		FAssetDefaults Ignored;
		FString Message;
		TestFalse(What, FromJson(Text, Ignored, Message));
		TestFalse(FString(What) + TEXT(" has a message"), Message.IsEmpty());
		if (ExpectedMessage)
		{
			TestTrue(FString(What) + TEXT(" message: ") + Message, Message.Contains(ExpectedMessage));
		}
	};
	Rejects(TEXT("Not JSON"), TEXT("{"));
	Rejects(TEXT("Schema 1"), TEXT("{\"schema\": 1, \"scope\": \"defaults\", \"blueprint\": \"/Game/X\", \"reset\": []}"), TEXT("re-export"));
	Rejects(TEXT("Future schema"), TEXT("{\"schema\": 3, \"scope\": \"defaults\", \"asset\": \"/Game/X\"}"));
	Rejects(TEXT("Wrong scope"), TEXT("{\"schema\": 2, \"scope\": \"instances\", \"asset\": \"/Game/X\"}"));
	Rejects(TEXT("Missing asset"), TEXT("{\"schema\": 2, \"scope\": \"defaults\"}"));
	Rejects(TEXT("Reset is no longer a field"), TEXT("{\"schema\": 2, \"scope\": \"defaults\", \"asset\": \"/Game/X\", \"reset\": []}"));
	Rejects(TEXT("Number value"), TEXT("{\"schema\": 2, \"scope\": \"defaults\", \"asset\": \"/Game/X\", \"defaults\": {\"A\": 2.5}}"));

	const FString Root = TEXT("C:/Project/JsonBlueprintSync");
	const FString Path = GetJsonPath(Root, TEXT("/Game/Features/Light/BP_Lamp"));
	TestEqual(TEXT("JSON path"), Path, FString(TEXT("C:/Project/JsonBlueprintSync/Game/Features/Light/BP_Lamp.json")));
	FString PackageName;
	TestTrue(TEXT("Package name from path"), GetPackageName(Root, Path, PackageName));
	TestEqual(TEXT("Package name"), PackageName, FString(TEXT("/Game/Features/Light/BP_Lamp")));
	TestFalse(TEXT("Outside the root"), GetPackageName(Root, TEXT("C:/Other/Game/BP.json"), PackageName));
	TestFalse(TEXT("Not JSON"), GetPackageName(Root, TEXT("C:/Project/JsonBlueprintSync/Game/BP.txt"), PackageName));

	TestEqual(TEXT("Hash ignores line endings"), HashText(TEXT("a\r\nb\n")), HashText(TEXT("a\nb\n")));
	TestNotEqual(TEXT("Hash sees content"), HashText(TEXT("a\n")), HashText(TEXT("b\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncRoundTripTest, "JsonBlueprintSync.RoundTrip", TestFlags)
bool FJsonBlueprintSyncRoundTripTest::RunTest(const FString&)
{
	FTestBlueprints BPs;
	UBlueprint* BP = BPs.Parent;
	TestTrue(TEXT("Blueprint kind"), GetAssetKind(BP) == EAssetKind::Blueprint);
	TestTrue(TEXT("Blueprint kind from asset data"), GetAssetKind(FAssetData(BP)) == EAssetKind::Blueprint);

	const FAssetDefaults Initial = Capture(BP);
	TestEqual(TEXT("New Blueprint has no overrides"), Initial.Defaults.Num() + Initial.Components.Num(), 0);
	TestEqual(TEXT("Asset package"), Initial.Asset, BP->GetPackage()->GetName());
	TestEqual(TEXT("Parent class"), Initial.Parent, AActor::StaticClass()->GetPathName());

	FAssetDefaults Doc = MakeDoc(BP);
	Doc.Defaults.Add(TEXT("FlickerSpeed"), TEXT("2.5"));
	Doc.Defaults.Add(TEXT("Tint"), TEXT("(R=1.0,G=0.5,B=0.25,A=1.0)"));
	Doc.Defaults.Add(TEXT("Steps"), TEXT("(1,2,3)"));
	Doc.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("1234.0"));
	const FApplyResult Result = Import(BP, Doc);
	TestFalse(TEXT("Import succeeded"), Result.HasErrors());
	TestEqual(TEXT("Four values changed"), Result.Changed.Num(), 4);

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	TestEqual(TEXT("FlickerSpeed"), GetDouble(CDO, TEXT("FlickerSpeed")), 2.5);
	TestEqual(TEXT("Tint"), GetTint(CDO), FLinearColor(1.0f, 0.5f, 0.25f, 1.0f));
	UPointLightComponent* Light = GetTemplateLight(BP);
	TestTrue(TEXT("Light template intensity"), Light && FMath::IsNearlyEqual(Light->Intensity, 1234.0f));

	const FAssetDefaults Captured = Capture(BP);
	TestEqual(TEXT("Captured defaults"), Captured.Defaults.Num(), 3);
	TestTrue(TEXT("Captured component"), Captured.Components.Contains(TEXT("Light")) && Captured.Components[TEXT("Light")].Contains(TEXT("Intensity")));

	const FString Json = ToJson(Captured);
	FAssetDefaults Parsed;
	FString Error;
	TestTrue(TEXT("Parses"), FromJson(Json, Parsed, Error));
	TestTrue(TEXT("Values equal"), ValuesEqual(Parsed, Captured));
	const FApplyResult Again = Import(BP, Parsed);
	TestFalse(TEXT("Re-import succeeded"), Again.HasErrors());
	TestEqual(TEXT("Re-importing the export changes nothing"), Again.Changed.Num(), 0);
	TestEqual(TEXT("Export is stable"), ToJson(Capture(BP)), Json);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncPropagationTest, "JsonBlueprintSync.Propagation", TestFlags)
bool FJsonBlueprintSyncPropagationTest::RunTest(const FString&)
{
	FTestBlueprints BPs;
	FTestWorld World;

	// The child overrides FlickerSpeed; instance B overrides it too.
	FAssetDefaults ChildDoc = MakeDoc(BPs.Child);
	ChildDoc.Defaults.Add(TEXT("FlickerSpeed"), TEXT("42.0"));
	if (!ImportOk(*this, BPs.Child, ChildDoc))
	{
		return false;
	}
	const FString ChildJsonBefore = ToJson(Capture(BPs.Child));

	World.Spawn(BPs.Parent, TEXT("InstanceA"));
	AActor* B = World.Spawn(BPs.Parent, TEXT("InstanceB"));
	SetDouble(B, TEXT("FlickerSpeed"), 99.0);

	FAssetDefaults Doc = MakeDoc(BPs.Parent);
	Doc.Defaults.Add(TEXT("FlickerSpeed"), TEXT("3.0"));
	Doc.Defaults.Add(TEXT("Tint"), TEXT("(R=0.0,G=1.0,B=0.0,A=1.0)"));
	Doc.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("777.0"));
	if (!ImportOk(*this, BPs.Parent, Doc))
	{
		return false;
	}
	TestTrue(TEXT("Parents apply before children"), GetApplyOrder(BPs.Parent) < GetApplyOrder(BPs.Child));

	UObject* ChildCDO = BPs.Child->GeneratedClass->GetDefaultObject();
	TestEqual(TEXT("Child keeps its own FlickerSpeed"), GetDouble(ChildCDO, TEXT("FlickerSpeed")), 42.0);
	TestEqual(TEXT("Child follows the parent's Tint"), GetTint(ChildCDO), FLinearColor(0.0f, 1.0f, 0.0f, 1.0f));
	TestEqual(TEXT("Child JSON is unchanged by the parent import"), ToJson(Capture(BPs.Child)), ChildJsonBefore);

	AActor* A = World.Find(TEXT("InstanceA"));
	B = World.Find(TEXT("InstanceB"));
	if (!TestNotNull(TEXT("Instance A"), A) || !TestNotNull(TEXT("Instance B"), B))
	{
		return false;
	}
	TestEqual(TEXT("Instance A follows the new default"), GetDouble(A, TEXT("FlickerSpeed")), 3.0);
	TestEqual(TEXT("Instance B keeps its override"), GetDouble(B, TEXT("FlickerSpeed")), 99.0);
	TestEqual(TEXT("Instance A gets the new Tint"), GetTint(A), FLinearColor(0.0f, 1.0f, 0.0f, 1.0f));

	UPointLightComponent* InstanceLight = A->FindComponentByClass<UPointLightComponent>();
	TestTrue(TEXT("Instance A's light follows the template"), InstanceLight && FMath::IsNearlyEqual(InstanceLight->Intensity, 777.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncRejectsBadValuesTest, "JsonBlueprintSync.RejectsBadValues", TestFlags)
bool FJsonBlueprintSyncRejectsBadValuesTest::RunTest(const FString&)
{
	FTestBlueprints BPs;
	UBlueprint* BP = BPs.Parent;
	UObject* CDO = BP->GeneratedClass->GetDefaultObject();

	auto Rejects = [this, BP, CDO](const TCHAR* What, TFunctionRef<void(FAssetDefaults&)> Edit)
	{
		FAssetDefaults Doc = MakeDoc(BP);
		Doc.Defaults.Add(TEXT("FlickerSpeed"), TEXT("2.5"));
		Edit(Doc);
		const FApplyResult Result = Import(BP, Doc);
		TestTrue(FString(What) + TEXT(": rejected"), Result.HasErrors());
		TestEqual(FString(What) + TEXT(": nothing written"), Result.Changed.Num(), 0);
		TestEqual(FString(What) + TEXT(": valid value not applied either"), GetDouble(CDO, TEXT("FlickerSpeed")), 0.0);
	};

	Rejects(TEXT("Unknown property"), [](FAssetDefaults& Doc) { Doc.Defaults.Add(TEXT("NoSuchProperty"), TEXT("1")); });
	Rejects(TEXT("Wrong case"), [](FAssetDefaults& Doc) { Doc.Defaults.Add(TEXT("tint"), TEXT("(R=1.0,G=1.0,B=1.0,A=1.0)")); });
	Rejects(TEXT("Bad array text"), [](FAssetDefaults& Doc) { Doc.Defaults.Add(TEXT("Steps"), TEXT("(1,2")); });
	Rejects(TEXT("Unknown struct field"), [](FAssetDefaults& Doc) { Doc.Defaults.Add(TEXT("Tint"), TEXT("(X=1)")); });
	Rejects(TEXT("Trailing text"), [](FAssetDefaults& Doc) { Doc.Defaults.Add(TEXT("Steps"), TEXT("(1,2) junk")); });
	Rejects(TEXT("Unknown component"), [](FAssetDefaults& Doc) { Doc.Components.Add(TEXT("NoSuchComponent")).Add(TEXT("Intensity"), TEXT("1.0")); });
	Rejects(TEXT("Unknown component property"), [](FAssetDefaults& Doc) { Doc.Components.Add(TEXT("Light")).Add(TEXT("NoSuchProperty"), TEXT("1.0")); });
	Rejects(TEXT("Wrong asset"), [](FAssetDefaults& Doc) { Doc.Asset = TEXT("/Game/Other/BP_Other"); });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncMissingKeyResetsTest, "JsonBlueprintSync.MissingKeyResets", TestFlags)
bool FJsonBlueprintSyncMissingKeyResetsTest::RunTest(const FString&)
{
	FTestBlueprints BPs;
	UBlueprint* BP = BPs.Parent;
	// Compiling replaces the CDO, so look it up after each import.
	auto CDO = [BP]() { return BP->GeneratedClass->GetDefaultObject(); };

	FAssetDefaults Doc = MakeDoc(BP);
	Doc.Defaults.Add(TEXT("FlickerSpeed"), TEXT("2.5"));
	Doc.Defaults.Add(TEXT("Tint"), TEXT("(R=1.0,G=0.0,B=0.0,A=1.0)"));
	Doc.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("1234.0"));
	if (!ImportOk(*this, BP, Doc))
	{
		return false;
	}

	// A document without FlickerSpeed and Light.Intensity: they go back to the parent's values, Tint stays.
	FAssetDefaults Partial = MakeDoc(BP);
	Partial.Defaults.Add(TEXT("Tint"), TEXT("(R=1.0,G=0.0,B=0.0,A=1.0)"));
	const FApplyResult Result = Import(BP, Partial);
	if (!ReportErrors(*this, Result))
	{
		return false;
	}
	TestEqual(TEXT("Two keys changed"), FString::Join(Result.Changed, TEXT(",")), FString(TEXT("FlickerSpeed,Light.Intensity")));
	TestEqual(TEXT("FlickerSpeed reset"), GetDouble(CDO(), TEXT("FlickerSpeed")), 0.0);
	TestTrue(TEXT("Intensity reset"), FMath::IsNearlyEqual(GetTemplateLight(BP)->Intensity, GetDefault<UPointLightComponent>()->Intensity));
	TestEqual(TEXT("Tint kept"), GetTint(CDO()), FLinearColor(1.0f, 0.0f, 0.0f, 1.0f));
	TestEqual(TEXT("Only Tint captured"), FString::Join([&] { TArray<FString> Keys; Flatten(Capture(BP)).GetKeys(Keys); return Keys; }(), TEXT(",")), FString(TEXT("Tint")));

	// A child that inherits a reset value follows the parent.
	TestEqual(TEXT("Child follows the reset"), GetDouble(BPs.Child->GeneratedClass->GetDefaultObject(), TEXT("FlickerSpeed")), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncMergeTest, "JsonBlueprintSync.Merge", TestFlags)
bool FJsonBlueprintSyncMergeTest::RunTest(const FString&)
{
	// What the JSON Sync tab does: base = the asset when the JSON change was seen; the user changed Tint locally since; the JSON
	// changed FlickerSpeed and dropped Light.Intensity. Applying the JSON-side keys keeps the local Tint.
	FTestBlueprints BPs;
	UBlueprint* BP = BPs.Parent;
	FAssetDefaults Start = MakeDoc(BP);
	Start.Defaults.Add(TEXT("FlickerSpeed"), TEXT("2.5"));
	Start.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("1234.0"));
	if (!ImportOk(*this, BP, Start))
	{
		return false;
	}
	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	const FAssetDefaults Base = Capture(BP);

	FAssetDefaults File = Base;
	File.Defaults.Add(TEXT("FlickerSpeed"), TEXT("3.000000"));
	File.Components.Reset();

	SetTint(CDO, FLinearColor(0.0f, 0.0f, 1.0f, 1.0f));
	const FAssetDefaults Current = Capture(BP);

	const TArray<FKeyDiff> Diffs = DiffKeys(&Base, File, Current);
	TestEqual(TEXT("Differing keys"), JoinKeys(Diffs), FString(TEXT("FlickerSpeed,Light.Intensity,Tint")));
	const FKeyDiff* Speed = FindDiff(Diffs, TEXT("FlickerSpeed"));
	const FKeyDiff* Intensity = FindDiff(Diffs, TEXT("Light.Intensity"));
	const FKeyDiff* Tint = FindDiff(Diffs, TEXT("Tint"));
	TestTrue(TEXT("FlickerSpeed changed in the JSON"), Speed && Speed->Change == EKeyChange::Json && Speed->TakeJsonByDefault());
	TestTrue(TEXT("Intensity removed in the JSON"), Intensity && Intensity->Change == EKeyChange::Json && !Intensity->JsonValue.IsSet());
	TestTrue(TEXT("Tint changed in the asset"), Tint && Tint->Change == EKeyChange::Asset && !Tint->TakeJsonByDefault() && !Tint->JsonValue.IsSet());

	TArray<FString> Keys;
	for (const FKeyDiff& Diff : Diffs)
	{
		if (Diff.TakeJsonByDefault())
		{
			Keys.Add(Diff.Key);
		}
	}
	const FAssetDefaults Merged = TakeKeys(Current, File, Keys);
	if (!ReportErrors(*this, Apply(BP, Merged)))
	{
		return false;
	}
	TestEqual(TEXT("FlickerSpeed from the JSON"), GetDouble(CDO, TEXT("FlickerSpeed")), 3.0);
	TestTrue(TEXT("Intensity reset from the JSON"), FMath::IsNearlyEqual(GetTemplateLight(BP)->Intensity, GetDefault<UPointLightComponent>()->Intensity));
	TestEqual(TEXT("Local Tint kept"), GetTint(CDO), FLinearColor(0.0f, 0.0f, 1.0f, 1.0f));

	// After the apply only the asset-side key differs from the file: saving writes it to the JSON.
	TestEqual(TEXT("Only Tint left"), JoinKeys(DiffKeys(&Base, File, Capture(BP))), FString(TEXT("Tint")));

	// Without a base, every difference is a conflict and nothing is ticked.
	for (const FKeyDiff& Diff : DiffKeys(nullptr, File, Current))
	{
		TestTrue(FString::Printf(TEXT("%s is a conflict without a base"), *Diff.Key), Diff.Change == EKeyChange::Both && !Diff.TakeJsonByDefault());
	}
	// Changed on both sides to different values: a conflict.
	FAssetDefaults Both = File;
	Both.Defaults.Add(TEXT("Tint"), TEXT("(R=0.0,G=1.0,B=0.0,A=1.0)"));
	const FKeyDiff* TintBoth = FindDiff(DiffKeys(&Base, Both, Current), TEXT("Tint"));
	TestTrue(TEXT("Both sides changed Tint"), TintBoth && TintBoth->Change == EKeyChange::Both);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncTakeKeysTest, "JsonBlueprintSync.TakeKeys", TestFlags)
bool FJsonBlueprintSyncTakeKeysTest::RunTest(const FString&)
{
	FAssetDefaults Current;
	Current.Asset = TEXT("/Game/X");
	Current.Defaults.Add(TEXT("Keep"), TEXT("1"));
	Current.Defaults.Add(TEXT("Remove"), TEXT("1"));
	Current.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("1.0"));
	Current.Widgets.Add(TEXT("Title")).Add(TEXT("Text"), TEXT("A"));

	FAssetDefaults Source;
	Source.Defaults.Add(TEXT("Keep"), TEXT("2"));
	Source.Defaults.Add(TEXT("Add"), TEXT("3"));
	Source.Widgets.Add(TEXT("Title.Slot")).Add(TEXT("ZOrder"), TEXT("7"));

	const FAssetDefaults Result = TakeKeys(Current, Source, { TEXT("Add"), TEXT("Remove"), TEXT("Light.Intensity"), TEXT("Title.Slot.ZOrder") });
	TestEqual(TEXT("Untaken key keeps the current value"), Result.Defaults.FindRef(TEXT("Keep")), FString(TEXT("1")));
	TestEqual(TEXT("Taken key added"), Result.Defaults.FindRef(TEXT("Add")), FString(TEXT("3")));
	TestFalse(TEXT("Taken key missing from the source is removed"), Result.Defaults.Contains(TEXT("Remove")));
	TestEqual(TEXT("Empty component dropped"), Result.Components.Num(), 0);
	const TMap<FString, FString>* Slot = Result.Widgets.Find(TEXT("Title.Slot"));
	TestTrue(TEXT("Slot key splits at the last dot"), Slot && Slot->FindRef(TEXT("ZOrder")) == TEXT("7"));
	TestEqual(TEXT("Other widget values kept"), Result.Widgets.FindRef(TEXT("Title")).FindRef(TEXT("Text")), FString(TEXT("A")));
	TestEqual(TEXT("Asset field kept"), Result.Asset, Current.Asset);

	FAssetDefaults Renamed = Current;
	Renamed.Asset = TEXT("/Game/Y");
	Renamed.Parent = TEXT("/Script/Engine.Pawn");
	TestTrue(TEXT("ValuesEqual ignores asset and parent"), ValuesEqual(Current, Renamed));
	TestFalse(TEXT("ValuesEqual sees values"), ValuesEqual(Current, Result));
	TestEqual(TEXT("Flatten keys"), FString::Join([&] { TArray<FString> Keys; Flatten(Current).GetKeys(Keys); Keys.Sort(); return Keys; }(), TEXT(",")),
		FString(TEXT("Keep,Light.Intensity,Remove,Title.Text")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncCacheTest, "JsonBlueprintSync.Cache", TestFlags)
bool FJsonBlueprintSyncCacheTest::RunTest(const FString&)
{
	const FString Dir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("JsonBlueprintSync"), FGuid::NewGuid().ToString());
	const FString Path = FPaths::Combine(Dir, TEXT("cache.json"));

	FSyncCache Cache;
	TestTrue(TEXT("Missing cache loads empty"), Cache.Load(Dir) && Cache.Num() == 0);
	Cache.Set(TEXT("/Game/B"), { TEXT("j2"), true });
	Cache.Set(TEXT("/Game/A"), { TEXT("j1"), false });
	FAssetDefaults Base;
	Base.Asset = TEXT("/Game/B");
	Base.Defaults.Add(TEXT("Speed"), TEXT("1.0"));
	TestTrue(TEXT("Base saves"), Cache.SaveBase(TEXT("/Game/B"), Base));
	TestTrue(TEXT("Cache saves"), Cache.Save());

	FSyncCache Loaded;
	TestTrue(TEXT("Cache loads"), Loaded.Load(Dir));
	const FSyncEntry* A = Loaded.Find(TEXT("/Game/A"));
	const FSyncEntry* B = Loaded.Find(TEXT("/Game/B"));
	TestTrue(TEXT("Cache round trip"), Loaded.Num() == 2 && A && A->JsonHash == TEXT("j1") && !A->bPending && B && B->bPending);
	TestEqual(TEXT("Pending packages"), FString::Join(Loaded.GetPendingPackages(), TEXT(",")), FString(TEXT("/Game/B")));
	FAssetDefaults LoadedBase;
	TestTrue(TEXT("Base loads"), Loaded.LoadBase(TEXT("/Game/B"), LoadedBase) && ValuesEqual(LoadedBase, Base));
	Loaded.Remove(TEXT("/Game/B"));
	TestFalse(TEXT("Remove deletes the base"), Loaded.LoadBase(TEXT("/Game/B"), LoadedBase));

	FFileHelper::SaveStringToFile(TEXT("{\"version\": 1, \"entries\": {\"/Game/A\": {\"json\": \"x\", \"asset\": \"y\"}}}"), *Path);
	TestTrue(TEXT("Old cache version loads"), Loaded.Load(Dir));
	TestEqual(TEXT("Old cache version is dropped"), Loaded.Num(), 0);
	FFileHelper::SaveStringToFile(TEXT("{ broken"), *Path);
	TestFalse(TEXT("Broken cache reports failure"), Loaded.Load(Dir));
	TestEqual(TEXT("Broken cache is empty"), Loaded.Num(), 0);
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncSettingsTest, "JsonBlueprintSync.Settings", TestFlags)
bool FJsonBlueprintSyncSettingsTest::RunTest(const FString&)
{
	UJsonBlueprintSyncSettings* Settings = NewObject<UJsonBlueprintSyncSettings>(GetTransientPackage());
	TestTrue(TEXT("Blueprints default to Write JSON Only"), Settings->GetMode(EAssetKind::Blueprint) == EJsonSyncMode::WriteJsonOnly);
	TestTrue(TEXT("Widget Blueprints default to Write JSON Only"), Settings->GetMode(EAssetKind::WidgetBlueprint) == EJsonSyncMode::WriteJsonOnly);
	TestTrue(TEXT("Data assets default to Two Way"), Settings->GetMode(EAssetKind::DataAsset) == EJsonSyncMode::TwoWay);
	TestTrue(TEXT("Other assets are not synced"), Settings->GetMode(EAssetKind::None) == EJsonSyncMode::None);

	Settings->Blueprints = EJsonSyncMode::TwoWay;
	Settings->WidgetBlueprints = EJsonSyncMode::TwoWay;
	TestTrue(TEXT("Two Way is not allowed for Blueprints"), Settings->GetMode(EAssetKind::Blueprint) == EJsonSyncMode::WriteJsonOnly);
	TestTrue(TEXT("Two Way is not allowed for Widget Blueprints"), Settings->GetMode(EAssetKind::WidgetBlueprint) == EJsonSyncMode::WriteJsonOnly);
	Settings->DataAssets = EJsonSyncMode::None;
	TestTrue(TEXT("None turns syncing off"), Settings->GetMode(EAssetKind::DataAsset) == EJsonSyncMode::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncDataAssetTest, "JsonBlueprintSync.DataAsset", TestFlags)
bool FJsonBlueprintSyncDataAssetTest::RunTest(const FString&)
{
	// A Blueprint subclass of UPrimaryDataAsset with an instance-editable Power and a class-only Hidden, and one data asset of it.
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprint* ClassBP = FTestBlueprints::Create(UPrimaryDataAsset::StaticClass(), TEXT("BP_Data_") + Suffix);
	FBlueprintEditorUtils::AddMemberVariable(ClassBP, TEXT("Power"), DoubleType());
	FBlueprintEditorUtils::AddMemberVariable(ClassBP, TEXT("Hidden"), DoubleType());
	FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(ClassBP, TEXT("Power"), false);
	FKismetEditorUtilities::CompileBlueprint(ClassBP, EBlueprintCompileOptions::SkipGarbageCollection);

	const FString Name = TEXT("DA_Test_") + Suffix;
	UDataAsset* Asset = NewObject<UDataAsset>(CreateTestPackage(Name), ClassBP->GeneratedClass, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
	TestTrue(TEXT("Data asset kind"), GetAssetKind(Asset) == EAssetKind::DataAsset);
	TestTrue(TEXT("Data asset kind from asset data"), GetAssetKind(FAssetData(Asset)) == EAssetKind::DataAsset);
	TestTrue(TEXT("Class Blueprint is a Blueprint"), GetAssetKind(ClassBP) == EAssetKind::Blueprint);
	TestTrue(TEXT("Data assets apply after Blueprints"), GetApplyOrder(ClassBP) < GetApplyOrder(Asset));

	const FAssetDefaults Initial = Capture(Asset);
	TestEqual(TEXT("New data asset has no values"), Initial.Defaults.Num(), 0);
	TestEqual(TEXT("Parent is the asset's class"), Initial.Parent, ClassBP->GeneratedClass->GetPathName());
	TestEqual(TEXT("Asset package"), Initial.Asset, Asset->GetPackage()->GetName());

	FAssetDefaults Doc = MakeDoc(Asset);
	Doc.Defaults.Add(TEXT("Power"), TEXT("5.0"));
	const FApplyResult Result = Import(Asset, Doc);
	if (!ReportErrors(*this, Result))
	{
		return false;
	}
	TestEqual(TEXT("One value changed"), Result.Changed.Num(), 1);
	TestEqual(TEXT("Power set on the asset"), GetDouble(Asset, TEXT("Power")), 5.0);
	TestEqual(TEXT("Class default untouched"), GetDouble(ClassBP->GeneratedClass->GetDefaultObject(), TEXT("Power")), 0.0);
	TestTrue(TEXT("Package marked dirty"), Asset->GetPackage()->IsDirty());

	const FAssetDefaults Captured = Capture(Asset);
	TestEqual(TEXT("Captured value"), Captured.Defaults.FindRef(TEXT("Power")).Left(1), FString(TEXT("5")));
	TestEqual(TEXT("Re-import changes nothing"), Import(Asset, Captured).Changed.Num(), 0);

	// Class-only variables are not part of a data asset's JSON.
	FAssetDefaults Hidden = MakeDoc(Asset);
	Hidden.Defaults.Add(TEXT("Hidden"), TEXT("1.0"));
	TestTrue(TEXT("Class-only variable rejected"), Apply(Asset, Hidden).HasErrors());
	FAssetDefaults Components = MakeDoc(Asset);
	Components.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("1.0"));
	TestTrue(TEXT("Components rejected"), Apply(Asset, Components).HasErrors());
	TestEqual(TEXT("Nothing applied"), GetDouble(Asset, TEXT("Power")), 5.0);

	// A missing key resets to the class default.
	if (!ReportErrors(*this, Apply(Asset, MakeDoc(Asset))))
	{
		return false;
	}
	TestEqual(TEXT("Power reset"), GetDouble(Asset, TEXT("Power")), 0.0);
	TestEqual(TEXT("Nothing captured after the reset"), Capture(Asset).Defaults.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncInheritedComponentTest, "JsonBlueprintSync.InheritedComponent", TestFlags)
bool FJsonBlueprintSyncInheritedComponentTest::RunTest(const FString&)
{
	FTestBlueprints BPs;

	FAssetDefaults ParentDoc = MakeDoc(BPs.Parent);
	ParentDoc.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("1000.0"));
	if (!ImportOk(*this, BPs.Parent, ParentDoc))
	{
		return false;
	}
	TestEqual(TEXT("Child without override exports no components"), Capture(BPs.Child).Components.Num(), 0);

	FAssetDefaults ChildDoc = MakeDoc(BPs.Child);
	ChildDoc.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("500.0"));
	if (!ImportOk(*this, BPs.Child, ChildDoc))
	{
		return false;
	}

	UInheritableComponentHandler* Handler = BPs.Child->GetInheritableComponentHandler(false);
	USCS_Node* ParentNode = BPs.Parent->SimpleConstructionScript->FindSCSNode(TEXT("Light"));
	UPointLightComponent* Override = Handler && ParentNode ? Cast<UPointLightComponent>(Handler->GetOverridenComponentTemplate(FComponentKey(ParentNode))) : nullptr;
	if (!TestNotNull(TEXT("Child override created"), Override))
	{
		return false;
	}
	TestTrue(TEXT("Child override value"), FMath::IsNearlyEqual(Override->Intensity, 500.0f));
	TestTrue(TEXT("Parent template unchanged"), FMath::IsNearlyEqual(GetTemplateLight(BPs.Parent)->Intensity, 1000.0f));

	const FAssetDefaults Captured = Capture(BPs.Child);
	const TMap<FString, FString>* Light = Captured.Components.Find(TEXT("Light"));
	TestTrue(TEXT("Child exports only the differing value"), Light && Light->Num() == 1 && Light->Contains(TEXT("Intensity")));

	// No key for the inherited component: the child's override goes back to the parent's value.
	if (!ImportOk(*this, BPs.Child, MakeDoc(BPs.Child)))
	{
		return false;
	}
	Override = Handler ? Cast<UPointLightComponent>(Handler->GetOverridenComponentTemplate(FComponentKey(ParentNode))) : nullptr;
	TestTrue(TEXT("Child override back to the parent's value"), !Override || FMath::IsNearlyEqual(Override->Intensity, 1000.0f));
	TestEqual(TEXT("Matching the parent is not an override"), Capture(BPs.Child).Components.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncWidgetTest, "JsonBlueprintSync.Widget", TestFlags)
bool FJsonBlueprintSyncWidgetTest::RunTest(const FString&)
{
	// Widget Blueprint with a canvas "Root" holding a text block "Title", built the way the Designer builds it.
	const FString Name = TEXT("WBP_Test_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UWidgetBlueprint* Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), CreateTestPackage(Name),
		FName(*Name), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Blueprint->WidgetTree->RootWidget = Root;
	UTextBlock* Title = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Title"));
	UCanvasPanelSlot* TitleSlot = Root->AddChildToCanvas(Title);
	Blueprint->OnVariableAdded(Root->GetFName());
	Blueprint->OnVariableAdded(Title->GetFName());
	Title->SetText(FText::FromString(TEXT("Hello")));
	TitleSlot->SetZOrder(3);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	TestTrue(TEXT("Widget Blueprint kind"), GetAssetKind(Blueprint) == EAssetKind::WidgetBlueprint);
	TestTrue(TEXT("Widget Blueprint kind from asset data"), GetAssetKind(FAssetData(Blueprint)) == EAssetKind::WidgetBlueprint);

	// Export: Designer values go under "widgets", the layout in the parent panel under "<Widget>.Slot".
	const FAssetDefaults Initial = Capture(Blueprint);
	const TMap<FString, FString>* TitleValues = Initial.Widgets.Find(TEXT("Title"));
	const TMap<FString, FString>* SlotValues = Initial.Widgets.Find(TEXT("Title.Slot"));
	TestTrue(TEXT("Text captured"), TitleValues && TitleValues->FindRef(TEXT("Text")).Contains(TEXT("Hello")));
	TestFalse(TEXT("Unchanged value not captured"), TitleValues && TitleValues->Contains(TEXT("RenderOpacity")));
	TestEqual(TEXT("Slot captured"), SlotValues ? SlotValues->FindRef(TEXT("ZOrder")) : FString(), FString(TEXT("3")));
	TestEqual(TEXT("Widgets are not components"), Initial.Components.Num(), 0);

	const FString Json = ToJson(Initial);
	TestTrue(TEXT("Widgets section written"), Json.Contains(TEXT("\"widgets\": {")));
	FAssetDefaults Parsed;
	FString Error;
	TestTrue(TEXT("Parses its own output"), FromJson(Json, Parsed, Error));
	TestEqual(TEXT("Round trip is byte-identical"), ToJson(Parsed), Json);

	// Import: values reach the Designer templates and, after the compile, the class's widget tree.
	FAssetDefaults Doc = Initial;
	Doc.Widgets.FindOrAdd(TEXT("Title")).Add(TEXT("RenderOpacity"), TEXT("0.500000"));
	Doc.Widgets.FindOrAdd(TEXT("Title.Slot")).Add(TEXT("ZOrder"), TEXT("7"));
	if (!ImportOk(*this, Blueprint, Doc))
	{
		return false;
	}
	TestEqual(TEXT("Template value"), Title->GetRenderOpacity(), 0.5f);
	TestEqual(TEXT("Slot value"), TitleSlot->GetZOrder(), 7);
	UWidgetTree* CompiledTree = CastChecked<UWidgetBlueprintGeneratedClass>(Blueprint->GeneratedClass)->GetWidgetTreeArchetype();
	const UTextBlock* Compiled = CompiledTree ? Cast<UTextBlock>(CompiledTree->FindWidget(TEXT("Title"))) : nullptr;
	TestTrue(TEXT("Compiled widget tree has the value"), Compiled && FMath::IsNearlyEqual(Compiled->GetRenderOpacity(), 0.5f));
	TestEqual(TEXT("Re-export is identical"), ToJson(Capture(Blueprint)), ToJson(Doc));
	TestTrue(TEXT("Diff keys include slot keys"), FindDiff(DiffKeys(nullptr, Doc, Initial), TEXT("Title.Slot.ZOrder")) != nullptr);

	// Keys missing from the document go back to the widget class defaults; keys split at the last '.', so "Title.Slot.ZOrder"
	// targets the slot.
	const FAssetDefaults Reset = TakeKeys(Doc, Initial, { TEXT("Title.RenderOpacity"), TEXT("Title.Slot.ZOrder") });
	if (!ImportOk(*this, Blueprint, Reset))
	{
		return false;
	}
	TestEqual(TEXT("Value reset"), Title->GetRenderOpacity(), 1.0f);
	TestEqual(TEXT("Slot value back to the initial value"), TitleSlot->GetZOrder(), 3);
	TestTrue(TEXT("Text kept"), Title->GetText().ToString() == TEXT("Hello"));
	TestEqual(TEXT("Capture matches"), ToJson(Capture(Blueprint)), ToJson(Initial));

	// An unknown widget is an error, and nothing else in the file is applied.
	FAssetDefaults Bad = Initial;
	Bad.Widgets.FindOrAdd(TEXT("Title")).Add(TEXT("RenderOpacity"), TEXT("0.25"));
	Bad.Widgets.Add(TEXT("Missing")).Add(TEXT("RenderOpacity"), TEXT("0.25"));
	TestTrue(TEXT("Unknown widget rejected"), Apply(Blueprint, Bad).HasErrors());
	TestEqual(TEXT("Nothing applied"), Title->GetRenderOpacity(), 1.0f);
	return true;
}

#endif
