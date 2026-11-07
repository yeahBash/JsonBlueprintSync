#include "JsonBlueprintSync.h"
#include "../SyncState.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/PointLightComponent.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"

namespace JsonBlueprintSync::Tests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

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

			FEdGraphPinType DoubleType(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
			FEdGraphPinType ColorType(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FLinearColor>::Get(), EPinContainerType::None, false, FEdGraphTerminalType());
			FEdGraphPinType IntArrayType(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, EPinContainerType::Array, false, FEdGraphTerminalType());
			FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("FlickerSpeed"), DoubleType);
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
			UPackage* Package = CreatePackage(*(TEXT("/Temp/JsonBlueprintSyncTests/") + Name));
			Package->SetPackageFlags(PKG_InMemoryOnly);
			return FKismetEditorUtilities::CreateBlueprint(ParentClass, Package, FName(*Name), BPTYPE_Normal);
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

	FBlueprintDefaults MakeDoc(UBlueprint* Blueprint)
	{
		FBlueprintDefaults Doc;
		Doc.Blueprint = Blueprint->GetPackage()->GetName();
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

	bool ImportOk(FAutomationTestBase& Test, UBlueprint* Blueprint, const FBlueprintDefaults& Doc)
	{
		const FApplyResult Result = Import(Blueprint, Doc);
		for (const FString& Error : Result.Errors)
		{
			Test.AddError(FString::Printf(TEXT("Import error: %s"), *Error));
		}
		return !Result.HasErrors();
	}
}

using namespace JsonBlueprintSync;
using namespace JsonBlueprintSync::Tests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncFormatTest, "JsonBlueprintSync.Format", TestFlags)
bool FJsonBlueprintSyncFormatTest::RunTest(const FString&)
{
	FBlueprintDefaults Doc;
	Doc.Blueprint = TEXT("/Game/A/BP_B");
	Doc.Parent = TEXT("/Script/Engine.Actor");
	Doc.Defaults.Add(TEXT("b"), TEXT("Quote \" back \\ tab \t"));
	Doc.Defaults.Add(TEXT("Z"), TEXT("(1,2)"));
	Doc.Defaults.Add(TEXT("a"), TEXT("1.0"));
	Doc.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("5000.0"));
	Doc.Reset = { TEXT("Y"), TEXT("Light.Radius") };

	const FString Json = ToJson(Doc);
	TestFalse(TEXT("No CR"), Json.Contains(TEXT("\r")));
	TestTrue(TEXT("Trailing newline"), Json.EndsWith(TEXT("}\n")));
	TestTrue(TEXT("Keys sorted case-sensitively"), Json.Find(TEXT("\"Z\"")) < Json.Find(TEXT("\"a\"")) && Json.Find(TEXT("\"a\"")) < Json.Find(TEXT("\"b\"")));
	TestTrue(TEXT("Fixed top-level order"), Json.Find(TEXT("\"schema\"")) < Json.Find(TEXT("\"blueprint\"")) && Json.Find(TEXT("\"components\"")) < Json.Find(TEXT("\"reset\"")));

	FBlueprintDefaults Parsed;
	FString Error;
	TestTrue(TEXT("Parses its own output"), FromJson(Json, Parsed, Error));
	TestEqual(TEXT("Round trip is byte-identical"), ToJson(Parsed), Json);
	TestEqual(TEXT("Escapes survive"), Parsed.Defaults.FindRef(TEXT("b")), FString(TEXT("Quote \" back \\ tab \t")));

	const FString Empty = ToJson(FBlueprintDefaults());
	TestTrue(TEXT("Empty maps"), Empty.Contains(TEXT("\"defaults\": {},")) && Empty.Contains(TEXT("\"reset\": []")));

	auto Rejects = [this](const TCHAR* What, const FString& Text)
	{
		FBlueprintDefaults Ignored;
		FString Message;
		TestFalse(What, FromJson(Text, Ignored, Message));
		TestFalse(FString(What) + TEXT(" has a message"), Message.IsEmpty());
	};
	Rejects(TEXT("Not JSON"), TEXT("{"));
	Rejects(TEXT("Wrong schema"), TEXT("{\"schema\": 2, \"scope\": \"defaults\", \"blueprint\": \"/Game/X\"}"));
	Rejects(TEXT("Wrong scope"), TEXT("{\"schema\": 1, \"scope\": \"instances\", \"blueprint\": \"/Game/X\"}"));
	Rejects(TEXT("Missing blueprint"), TEXT("{\"schema\": 1, \"scope\": \"defaults\"}"));
	Rejects(TEXT("Unknown field"), TEXT("{\"schema\": 1, \"scope\": \"defaults\", \"blueprint\": \"/Game/X\", \"extra\": 1}"));
	Rejects(TEXT("Number value"), TEXT("{\"schema\": 1, \"scope\": \"defaults\", \"blueprint\": \"/Game/X\", \"defaults\": {\"A\": 2.5}}"));
	Rejects(TEXT("Reset not strings"), TEXT("{\"schema\": 1, \"scope\": \"defaults\", \"blueprint\": \"/Game/X\", \"reset\": [1]}"));

	const FString Root = TEXT("C:/Project/JsonBlueprintSync");
	const FString Path = GetJsonPath(Root, TEXT("/Game/Features/Light/BP_Lamp"));
	TestEqual(TEXT("JSON path"), Path, FString(TEXT("C:/Project/JsonBlueprintSync/Game/Features/Light/BP_Lamp.json")));
	FString PackageName;
	TestTrue(TEXT("Package name from path"), GetPackageName(Root, Path, PackageName));
	TestEqual(TEXT("Package name"), PackageName, FString(TEXT("/Game/Features/Light/BP_Lamp")));
	TestFalse(TEXT("Outside the root"), GetPackageName(Root, TEXT("C:/Other/Game/BP.json"), PackageName));
	TestFalse(TEXT("Not JSON"), GetPackageName(Root, TEXT("C:/Project/JsonBlueprintSync/Game/BP.txt"), PackageName));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncRoundTripTest, "JsonBlueprintSync.RoundTrip", TestFlags)
bool FJsonBlueprintSyncRoundTripTest::RunTest(const FString&)
{
	FTestBlueprints BPs;
	UBlueprint* BP = BPs.Parent;

	const FBlueprintDefaults Initial = Capture(BP);
	TestEqual(TEXT("New Blueprint has no overrides"), Initial.Defaults.Num() + Initial.Components.Num(), 0);
	TestEqual(TEXT("Blueprint package"), Initial.Blueprint, BP->GetPackage()->GetName());
	TestEqual(TEXT("Parent class"), Initial.Parent, AActor::StaticClass()->GetPathName());

	FBlueprintDefaults Doc = MakeDoc(BP);
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

	const FBlueprintDefaults Captured = Capture(BP);
	TestEqual(TEXT("Captured defaults"), Captured.Defaults.Num(), 3);
	TestTrue(TEXT("Captured component"), Captured.Components.Contains(TEXT("Light")) && Captured.Components[TEXT("Light")].Contains(TEXT("Intensity")));

	const FString Json = ToJson(Captured);
	FBlueprintDefaults Parsed;
	FString Error;
	TestTrue(TEXT("Parses"), FromJson(Json, Parsed, Error));
	const FApplyResult Again = Import(BP, Parsed);
	TestFalse(TEXT("Re-import succeeded"), Again.HasErrors());
	TestEqual(TEXT("Re-importing the export changes nothing"), Again.Changed.Num(), 0);
	TestEqual(TEXT("Export is stable"), ToJson(Capture(BP, &Parsed)), Json);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncPropagationTest, "JsonBlueprintSync.Propagation", TestFlags)
bool FJsonBlueprintSyncPropagationTest::RunTest(const FString&)
{
	FTestBlueprints BPs;
	FTestWorld World;

	// The child overrides FlickerSpeed; instance B overrides it too.
	FBlueprintDefaults ChildDoc = MakeDoc(BPs.Child);
	ChildDoc.Defaults.Add(TEXT("FlickerSpeed"), TEXT("42.0"));
	if (!ImportOk(*this, BPs.Child, ChildDoc))
	{
		return false;
	}
	const FString ChildJsonBefore = ToJson(Capture(BPs.Child));

	World.Spawn(BPs.Parent, TEXT("InstanceA"));
	AActor* B = World.Spawn(BPs.Parent, TEXT("InstanceB"));
	SetDouble(B, TEXT("FlickerSpeed"), 99.0);

	FBlueprintDefaults Doc = MakeDoc(BPs.Parent);
	Doc.Defaults.Add(TEXT("FlickerSpeed"), TEXT("3.0"));
	Doc.Defaults.Add(TEXT("Tint"), TEXT("(R=0.0,G=1.0,B=0.0,A=1.0)"));
	Doc.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("777.0"));
	if (!ImportOk(*this, BPs.Parent, Doc))
	{
		return false;
	}

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

	auto Rejects = [this, BP, CDO](const TCHAR* What, TFunctionRef<void(FBlueprintDefaults&)> Edit)
	{
		FBlueprintDefaults Doc = MakeDoc(BP);
		Doc.Defaults.Add(TEXT("FlickerSpeed"), TEXT("2.5"));
		Edit(Doc);
		const FApplyResult Result = Import(BP, Doc);
		TestTrue(FString(What) + TEXT(": rejected"), Result.HasErrors());
		TestEqual(FString(What) + TEXT(": nothing written"), Result.Changed.Num(), 0);
		TestEqual(FString(What) + TEXT(": valid value not applied either"), GetDouble(CDO, TEXT("FlickerSpeed")), 0.0);
	};

	Rejects(TEXT("Unknown property"), [](FBlueprintDefaults& Doc) { Doc.Defaults.Add(TEXT("NoSuchProperty"), TEXT("1")); });
	Rejects(TEXT("Wrong case"), [](FBlueprintDefaults& Doc) { Doc.Defaults.Add(TEXT("tint"), TEXT("(R=1.0,G=1.0,B=1.0,A=1.0)")); });
	Rejects(TEXT("Bad array text"), [](FBlueprintDefaults& Doc) { Doc.Defaults.Add(TEXT("Steps"), TEXT("(1,2")); });
	Rejects(TEXT("Unknown struct field"), [](FBlueprintDefaults& Doc) { Doc.Defaults.Add(TEXT("Tint"), TEXT("(X=1)")); });
	Rejects(TEXT("Trailing text"), [](FBlueprintDefaults& Doc) { Doc.Defaults.Add(TEXT("Steps"), TEXT("(1,2) junk")); });
	Rejects(TEXT("Unknown component"), [](FBlueprintDefaults& Doc) { Doc.Components.Add(TEXT("NoSuchComponent")).Add(TEXT("Intensity"), TEXT("1.0")); });
	Rejects(TEXT("Unknown component property"), [](FBlueprintDefaults& Doc) { Doc.Components.Add(TEXT("Light")).Add(TEXT("NoSuchProperty"), TEXT("1.0")); });
	Rejects(TEXT("Value and reset"), [](FBlueprintDefaults& Doc) { Doc.Reset.Add(TEXT("FlickerSpeed")); });
	Rejects(TEXT("Wrong Blueprint"), [](FBlueprintDefaults& Doc) { Doc.Blueprint = TEXT("/Game/Other/BP_Other"); });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncResetTest, "JsonBlueprintSync.Reset", TestFlags)
bool FJsonBlueprintSyncResetTest::RunTest(const FString&)
{
	FTestBlueprints BPs;
	UBlueprint* BP = BPs.Parent;
	// Compiling replaces the CDO, so look it up after each import.
	auto CDO = [BP]() { return BP->GeneratedClass->GetDefaultObject(); };

	FBlueprintDefaults Doc = MakeDoc(BP);
	Doc.Defaults.Add(TEXT("FlickerSpeed"), TEXT("2.5"));
	Doc.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("1234.0"));
	if (!ImportOk(*this, BP, Doc))
	{
		return false;
	}
	const FBlueprintDefaults Before = Capture(BP);

	// Local reset (as if done in the Details panel): the next capture lists the keys in "reset".
	SetDouble(CDO(), TEXT("FlickerSpeed"), 0.0);
	GetTemplateLight(BP)->Intensity = GetDefault<UPointLightComponent>()->Intensity;
	const FBlueprintDefaults AfterLocalReset = Capture(BP, &Before);
	TestEqual(TEXT("Reset keys"), FString::Join(AfterLocalReset.Reset, TEXT(",")), FString(TEXT("FlickerSpeed,Light.Intensity")));
	TestEqual(TEXT("Nothing captured after the local reset"), ToJson(AfterLocalReset).Contains(TEXT("\"defaults\": {},")) && AfterLocalReset.Components.Num() == 0, true);
	if (AfterLocalReset.Components.Num() > 0 || AfterLocalReset.Defaults.Num() > 0)
	{
		AddInfo(ToJson(AfterLocalReset));
	}
	TestEqual(TEXT("Reset is sticky"), ToJson(Capture(BP, &AfterLocalReset)), ToJson(AfterLocalReset));

	// Another machine still has the overrides; importing the reset file restores the baseline.
	if (!ImportOk(*this, BP, Doc))
	{
		return false;
	}
	if (!ImportOk(*this, BP, AfterLocalReset))
	{
		return false;
	}
	TestEqual(TEXT("FlickerSpeed reset"), GetDouble(CDO(), TEXT("FlickerSpeed")), 0.0);
	TestTrue(TEXT("Intensity reset"), FMath::IsNearlyEqual(GetTemplateLight(BP)->Intensity, GetDefault<UPointLightComponent>()->Intensity));

	// A reset key whose property no longer exists is dropped on capture.
	FBlueprintDefaults Stale = AfterLocalReset;
	Stale.Reset.Add(TEXT("Removed"));
	TestFalse(TEXT("Stale reset key dropped"), Capture(BP, &Stale).Reset.Contains(TEXT("Removed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncInheritedComponentTest, "JsonBlueprintSync.InheritedComponent", TestFlags)
bool FJsonBlueprintSyncInheritedComponentTest::RunTest(const FString&)
{
	FTestBlueprints BPs;

	FBlueprintDefaults ParentDoc = MakeDoc(BPs.Parent);
	ParentDoc.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("1000.0"));
	if (!ImportOk(*this, BPs.Parent, ParentDoc))
	{
		return false;
	}
	TestEqual(TEXT("Child without override exports no components"), Capture(BPs.Child).Components.Num(), 0);

	FBlueprintDefaults ChildDoc = MakeDoc(BPs.Child);
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

	const FBlueprintDefaults Captured = Capture(BPs.Child);
	const TMap<FString, FString>* Light = Captured.Components.Find(TEXT("Light"));
	TestTrue(TEXT("Child exports only the differing value"), Light && Light->Num() == 1 && Light->Contains(TEXT("Intensity")));

	// Same value as the parent: diffed against the parent's template, so it is not exported.
	ChildDoc.Components[TEXT("Light")][TEXT("Intensity")] = TEXT("1000.0");
	if (!ImportOk(*this, BPs.Child, ChildDoc))
	{
		return false;
	}
	TestEqual(TEXT("Matching the parent is not an override"), Capture(BPs.Child).Components.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncSyncRulesTest, "JsonBlueprintSync.SyncRules", TestFlags)
bool FJsonBlueprintSyncSyncRulesTest::RunTest(const FString&)
{
	// Hashes: "J" = JSON at the last sync, "A" = asset at the last sync.
	const FSyncEntry Entry{ TEXT("J"), TEXT("A") };
	TestTrue(TEXT("File unchanged"), Classify(&Entry, TEXT("J"), TEXT("X"), TEXT("Y")) == ESyncAction::None);
	TestTrue(TEXT("File changed, matches the asset"), Classify(&Entry, TEXT("J2"), TEXT("J2"), TEXT("A2")) == ESyncAction::UpdateCache);
	TestTrue(TEXT("Only the file changed"), Classify(&Entry, TEXT("J2"), TEXT("X"), TEXT("A")) == ESyncAction::Import);
	TestTrue(TEXT("Both changed and disagree"), Classify(&Entry, TEXT("J2"), TEXT("X"), TEXT("A2")) == ESyncAction::Conflict);
	TestTrue(TEXT("No record, file matches"), Classify(nullptr, TEXT("J"), TEXT("J"), TEXT("A")) == ESyncAction::UpdateCache);
	TestTrue(TEXT("No record, file differs"), Classify(nullptr, TEXT("J"), TEXT("X"), TEXT("A")) == ESyncAction::Conflict);

	TestEqual(TEXT("Hash ignores line endings"), HashText(TEXT("a\r\nb\n")), HashText(TEXT("a\nb\n")));
	TestNotEqual(TEXT("Hash sees content"), HashText(TEXT("a\n")), HashText(TEXT("b\n")));

	FBlueprintDefaults A;
	A.Parent = TEXT("/Script/Engine.Actor");
	A.Defaults.Add(TEXT("Same"), TEXT("1"));
	A.Defaults.Add(TEXT("Changed"), TEXT("1"));
	A.Defaults.Add(TEXT("OnlyA"), TEXT("1"));
	A.Components.Add(TEXT("Light")).Add(TEXT("Intensity"), TEXT("1.0"));
	FBlueprintDefaults B = A;
	B.Defaults.Remove(TEXT("OnlyA"));
	B.Defaults.Add(TEXT("Changed"), TEXT("2"));
	B.Components[TEXT("Light")].Add(TEXT("Intensity"), TEXT("2.0"));
	B.Reset.Add(TEXT("Same"));
	TestEqual(TEXT("Diff keys"), FString::Join(DiffKeys(A, B), TEXT(",")), FString(TEXT("Changed,Light.Intensity,OnlyA,Same")));
	B.Parent = TEXT("/Script/Engine.Pawn");
	TestTrue(TEXT("Parent difference"), DiffKeys(A, B).Contains(TEXT("parent")));
	TestEqual(TEXT("No difference"), DiffKeys(A, A).Num(), 0);

	const FString Path = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("JsonBlueprintSync"), FGuid::NewGuid().ToString() + TEXT(".json"));
	FSyncCache Cache;
	TestTrue(TEXT("Missing cache loads empty"), Cache.Load(Path) && Cache.Num() == 0);
	Cache.Set(TEXT("/Game/B"), { TEXT("j2"), TEXT("a2") });
	Cache.Set(TEXT("/Game/A"), { TEXT("j1"), TEXT("a1") });
	TestTrue(TEXT("Cache saves"), Cache.Save(Path));
	FSyncCache Loaded;
	TestTrue(TEXT("Cache loads"), Loaded.Load(Path));
	const FSyncEntry* Found = Loaded.Find(TEXT("/Game/A"));
	TestTrue(TEXT("Cache round trip"), Loaded.Num() == 2 && Found && Found->JsonHash == TEXT("j1") && Found->AssetHash == TEXT("a1"));
	FFileHelper::SaveStringToFile(TEXT("{ broken"), *Path);
	TestFalse(TEXT("Broken cache reports failure"), Loaded.Load(Path));
	TestEqual(TEXT("Broken cache is empty"), Loaded.Num(), 0);
	IFileManager::Get().Delete(*Path);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncInstancedStructTest, "JsonBlueprintSync.InstancedStruct", TestFlags)
bool FJsonBlueprintSyncInstancedStructTest::RunTest(const FString&)
{
	// Actor Blueprint with an FInstancedStruct variable "Payload".
	UBlueprint* Blueprint = FTestBlueprints::Create(AActor::StaticClass(), TEXT("BP_Instanced_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FEdGraphPinType PayloadType(UEdGraphSchema_K2::PC_Struct, NAME_None, FInstancedStruct::StaticStruct(), EPinContainerType::None, false, FEdGraphTerminalType());
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Payload"), PayloadType);
	FEdGraphPinType PayloadArrayType = PayloadType;
	PayloadArrayType.ContainerType = EPinContainerType::Array;
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Payloads"), PayloadArrayType);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	auto Payload = [Blueprint]() -> FInstancedStruct&
	{
		const FStructProperty* Property = FindFProperty<FStructProperty>(Blueprint->GeneratedClass, TEXT("Payload"));
		return *Property->ContainerPtrToValuePtr<FInstancedStruct>(Blueprint->GeneratedClass->GetDefaultObject());
	};
	TestFalse(TEXT("Empty payload not captured"), Capture(Blueprint).Defaults.Contains(TEXT("Payload")));

	// Export: the struct type path plus all of its fields.
	Payload() = FInstancedStruct::Make(FVector(1.0, 2.0, 3.0));
	const FBlueprintDefaults Initial = Capture(Blueprint);
	const FString Text = Initial.Defaults.FindRef(TEXT("Payload"));
	AddInfo(FString::Printf(TEXT("Payload exported as: %s"), *Text));
	TestTrue(TEXT("Type and value exported"), Text.StartsWith(TEXT("/Script/CoreUObject.Vector(")) && Text.Contains(TEXT("Z=3.")));
	FBlueprintDefaults Parsed;
	FString Error;
	TestTrue(TEXT("Parses its own output"), FromJson(ToJson(Initial), Parsed, Error));
	TestEqual(TEXT("Round trip is byte-identical"), ToJson(Parsed), ToJson(Initial));

	// Import can change the struct type, not just the values.
	FBlueprintDefaults Doc = MakeDoc(Blueprint);
	Doc.Parent = Initial.Parent;
	Doc.Defaults.Add(TEXT("Payload"), TEXT("/Script/CoreUObject.LinearColor(R=1.000000,G=0.500000,B=0.000000,A=1.000000)"));
	if (!ImportOk(*this, Blueprint, Doc))
	{
		return false;
	}
	const FLinearColor* Color = Payload().GetPtr<FLinearColor>();
	TestTrue(TEXT("Imported type and value"), Color && FMath::IsNearlyEqual(Color->G, 0.5f));
	TestEqual(TEXT("Re-export is identical"), ToJson(Capture(Blueprint, &Doc)), ToJson(Doc));

	// Reset empties it again.
	FBlueprintDefaults ResetDoc = MakeDoc(Blueprint);
	ResetDoc.Reset = { TEXT("Payload") };
	if (!ImportOk(*this, Blueprint, ResetDoc))
	{
		return false;
	}
	TestFalse(TEXT("Reset to empty"), Payload().IsValid());

	// A struct type that does not exist is an error, not a silent reset.
	Payload() = FInstancedStruct::Make(FVector(1.0, 2.0, 3.0));
	FBlueprintDefaults Bad = MakeDoc(Blueprint);
	Bad.Defaults.Add(TEXT("Payload"), TEXT("/Script/CoreUObject.NoSuchStruct()"));
	TestTrue(TEXT("Unknown struct type rejected"), Apply(Blueprint, Bad).HasErrors());
	TestNotNull(TEXT("Nothing applied"), Payload().GetPtr<FVector>());

	// Arrays of instanced structs: each element keeps its own type.
	const FString ArrayText = TEXT("(/Script/CoreUObject.Vector(X=1.000000,Y=2.000000,Z=3.000000),/Script/CoreUObject.LinearColor(R=1.000000,G=0.500000,B=0.000000,A=1.000000))");
	FBlueprintDefaults ArrayDoc = MakeDoc(Blueprint);
	ArrayDoc.Defaults.Add(TEXT("Payloads"), ArrayText);
	if (!ImportOk(*this, Blueprint, ArrayDoc))
	{
		return false;
	}
	TestEqual(TEXT("Array round trip"), Capture(Blueprint).Defaults.FindRef(TEXT("Payloads")), ArrayText);
	return true;
}

#endif
