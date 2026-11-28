#include "JsonDataAssetSync.h"
#include "JsonDataAssetSyncSettings.h"
#include "../SyncState.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetData.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

namespace JsonDataAssetSync::Tests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	UPackage* CreateTestPackage(const FString& Name)
	{
		UPackage* Package = CreatePackage(*(TEXT("/Temp/JsonDataAssetSyncTests/") + Name));
		Package->SetPackageFlags(PKG_InMemoryOnly);
		return Package;
	}

	FEdGraphPinType DoubleType()
	{
		return FEdGraphPinType(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
	}

	/**
	 * A Blueprint subclass of UPrimaryDataAsset with the instance-editable variables FlickerSpeed (double), Tint (LinearColor) and
	 * Steps (int array) and the class-only Hidden (double), plus one data asset of it. Both live in transient /Temp packages.
	 */
	struct FTestData
	{
		UBlueprint* ClassBlueprint = nullptr;
		UDataAsset* Asset = nullptr;

		/** DefaultSpeed: the class default of FlickerSpeed, which a key missing from the JSON resets to. */
		explicit FTestData(double DefaultSpeed = 0.0)
		{
			const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString ClassName = TEXT("BP_Data_") + Suffix;
			ClassBlueprint = FKismetEditorUtilities::CreateBlueprint(UPrimaryDataAsset::StaticClass(), CreateTestPackage(ClassName), FName(*ClassName),
				BPTYPE_Normal);

			const FEdGraphPinType ColorType(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FLinearColor>::Get(), EPinContainerType::None, false,
				FEdGraphTerminalType());
			const FEdGraphPinType IntArrayType(UEdGraphSchema_K2::PC_Int, NAME_None, nullptr, EPinContainerType::Array, false, FEdGraphTerminalType());
			FBlueprintEditorUtils::AddMemberVariable(ClassBlueprint, TEXT("FlickerSpeed"), DoubleType());
			FBlueprintEditorUtils::AddMemberVariable(ClassBlueprint, TEXT("Tint"), ColorType);
			FBlueprintEditorUtils::AddMemberVariable(ClassBlueprint, TEXT("Steps"), IntArrayType);
			FBlueprintEditorUtils::AddMemberVariable(ClassBlueprint, TEXT("Hidden"), DoubleType());
			// New variables are class-only (not editable on an asset); a data asset can only set the instance-editable ones.
			for (const TCHAR* Name : { TEXT("FlickerSpeed"), TEXT("Tint"), TEXT("Steps") })
			{
				FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(ClassBlueprint, Name, false);
			}
			FKismetEditorUtilities::CompileBlueprint(ClassBlueprint, EBlueprintCompileOptions::SkipGarbageCollection);
			SetDouble(GetClassDefault(), TEXT("FlickerSpeed"), DefaultSpeed);

			const FString Name = TEXT("DA_Test_") + Suffix;
			Asset = NewObject<UDataAsset>(CreateTestPackage(Name), ClassBlueprint->GeneratedClass, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		}

		UObject* GetClassDefault() const
		{
			return ClassBlueprint->GeneratedClass->GetDefaultObject();
		}

		static void SetDouble(UObject* Object, const TCHAR* Name, double Value)
		{
			if (const FDoubleProperty* Property = FindFProperty<FDoubleProperty>(Object->GetClass(), Name))
			{
				Property->SetPropertyValue_InContainer(Object, Value);
			}
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

	FString JoinCapturedKeys(UObject* Asset)
	{
		TArray<FString> Keys;
		Flatten(Capture(Asset)).GetKeys(Keys);
		Keys.Sort();
		return FString::Join(Keys, TEXT(","));
	}

	const FKeyDiff* FindDiff(const TArray<FKeyDiff>& Diffs, const TCHAR* Key)
	{
		return Diffs.FindByPredicate([Key](const FKeyDiff& Diff) { return Diff.Key == Key; });
	}
}

using namespace JsonDataAssetSync;
using namespace JsonDataAssetSync::Tests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncFormatTest, "JsonDataAssetSync.Format", TestFlags)
bool FJsonDataAssetSyncFormatTest::RunTest(const FString&)
{
	FAssetDefaults Doc;
	Doc.Asset = TEXT("/Game/A/DA_B");
	Doc.Parent = TEXT("/Script/Engine.PrimaryDataAsset");
	Doc.Self.Add(TEXT("b"), TEXT("Quote \" back \\ tab \t"));
	Doc.Self.Add(TEXT("Z"), TEXT("(1,2)"));
	Doc.Self.Add(TEXT("a"), TEXT("1.0"));

	const FString Json = ToJson(Doc);
	TestFalse(TEXT("No CR"), Json.Contains(TEXT("\r")));
	TestTrue(TEXT("Trailing newline"), Json.EndsWith(TEXT("}\n")));
	TestTrue(TEXT("Schema 2"), Json.Contains(TEXT("\"schema\": 2,")));
	TestTrue(TEXT("Keys sorted case-sensitively"), Json.Find(TEXT("\"Z\"")) < Json.Find(TEXT("\"a\"")) && Json.Find(TEXT("\"a\"")) < Json.Find(TEXT("\"b\"")));
	{
		// Field names with their colon, so that a value cannot match.
		TArray<int32> Positions;
		for (const TCHAR* Field : { TEXT("\"schema\":"), TEXT("\"asset\":"), TEXT("\"parent\":"), TEXT("\"overrides\":"), TEXT("\"self\":") })
		{
			Positions.Add(Json.Find(Field, ESearchCase::CaseSensitive));
		}
		bool bOrdered = Positions[0] >= 0;
		for (int32 Index = 1; Index < Positions.Num(); ++Index)
		{
			bOrdered &= Positions[Index - 1] < Positions[Index];
		}
		TestTrue(TEXT("Fixed field order"), bOrdered);
	}
	TestTrue(TEXT("Self is the only section"), Json.EndsWith(TEXT("\n      \"b\": \"Quote \\\" back \\\\ tab \\t\"\n    }\n  }\n}\n")));

	FAssetDefaults Parsed;
	FString Error;
	TestTrue(TEXT("Parses its own output"), FromJson(Json, Parsed, Error));
	TestEqual(TEXT("Round trip is byte-identical"), ToJson(Parsed), Json);
	TestEqual(TEXT("Escapes survive"), Parsed.Self.FindRef(TEXT("b")), FString(TEXT("Quote \" back \\ tab \t")));

	const FString Empty = ToJson(FAssetDefaults());
	TestTrue(TEXT("Empty self"), Empty.Contains(TEXT("\"overrides\": {\n    \"self\": {}\n  }")));

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
	Rejects(TEXT("Future schema"), TEXT("{\"schema\": 3, \"asset\": \"/Game/X\"}"));
	Rejects(TEXT("Old layout"), TEXT("{\"schema\": 2, \"scope\": \"defaults\", \"asset\": \"/Game/X\", \"defaults\": {}}"), TEXT("unknown field 'scope'"));
	Rejects(TEXT("Missing asset"), TEXT("{\"schema\": 2, \"overrides\": {}}"));
	Rejects(TEXT("Reset is no longer a field"), TEXT("{\"schema\": 2, \"asset\": \"/Game/X\", \"reset\": []}"));
	Rejects(TEXT("Unknown field in overrides"), TEXT("{\"schema\": 2, \"asset\": \"/Game/X\", \"overrides\": {\"defaults\": {}}}"), TEXT("unknown field 'overrides.defaults'"));
	// Blueprint files of older plugin versions: components and widgets are no longer synced.
	Rejects(TEXT("Components"), TEXT("{\"schema\": 2, \"asset\": \"/Game/X\", \"overrides\": {\"components\": {}}}"), TEXT("unknown field 'overrides.components'"));
	Rejects(TEXT("Widgets"), TEXT("{\"schema\": 2, \"asset\": \"/Game/X\", \"overrides\": {\"widgets\": {}}}"), TEXT("unknown field 'overrides.widgets'"));
	Rejects(TEXT("Overrides not an object"), TEXT("{\"schema\": 2, \"asset\": \"/Game/X\", \"overrides\": []}"), TEXT("'overrides' must be an object"));
	Rejects(TEXT("Self not an object"), TEXT("{\"schema\": 2, \"asset\": \"/Game/X\", \"overrides\": {\"self\": 1}}"), TEXT("'overrides.self' must be an object"));
	Rejects(TEXT("Number value"), TEXT("{\"schema\": 2, \"asset\": \"/Game/X\", \"overrides\": {\"self\": {\"A\": 2.5}}}"), TEXT("overrides.self.A"));
	{
		FAssetDefaults Minimal;
		FString MinimalError;
		TestTrue(TEXT("No overrides means nothing overridden"), FromJson(TEXT("{\"schema\": 2, \"asset\": \"/Game/X\"}"), Minimal, MinimalError)
			&& Minimal.Self.Num() == 0);
	}

	const FString Root = TEXT("C:/Project/JsonDataAssetSync");
	const FString Path = GetJsonPath(Root, TEXT("/Game/Features/Light/DA_Lamp"));
	TestEqual(TEXT("JSON path"), Path, FString(TEXT("C:/Project/JsonDataAssetSync/Game/Features/Light/DA_Lamp.json")));
	FString PackageName;
	TestTrue(TEXT("Package name from path"), GetPackageName(Root, Path, PackageName));
	TestEqual(TEXT("Package name"), PackageName, FString(TEXT("/Game/Features/Light/DA_Lamp")));
	TestFalse(TEXT("Outside the root"), GetPackageName(Root, TEXT("C:/Other/Game/DA.json"), PackageName));
	TestFalse(TEXT("Not JSON"), GetPackageName(Root, TEXT("C:/Project/JsonDataAssetSync/Game/DA.txt"), PackageName));

	TestEqual(TEXT("Hash ignores line endings"), HashText(TEXT("a\r\nb\n")), HashText(TEXT("a\nb\n")));
	TestNotEqual(TEXT("Hash sees content"), HashText(TEXT("a\n")), HashText(TEXT("b\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncRoundTripTest, "JsonDataAssetSync.RoundTrip", TestFlags)
bool FJsonDataAssetSyncRoundTripTest::RunTest(const FString&)
{
	FTestData Data;
	UDataAsset* Asset = Data.Asset;

	const FAssetDefaults Initial = Capture(Asset);
	TestEqual(TEXT("New data asset has no values"), Initial.Self.Num(), 0);
	TestEqual(TEXT("Asset package"), Initial.Asset, Asset->GetPackage()->GetName());
	TestEqual(TEXT("Parent is the asset's class"), Initial.Parent, Data.ClassBlueprint->GeneratedClass->GetPathName());

	FAssetDefaults Doc = MakeDoc(Asset);
	Doc.Self.Add(TEXT("FlickerSpeed"), TEXT("2.5"));
	Doc.Self.Add(TEXT("Tint"), TEXT("(R=1.0,G=0.5,B=0.25,A=1.0)"));
	Doc.Self.Add(TEXT("Steps"), TEXT("(1,2,3)"));
	const FApplyResult Result = Import(Asset, Doc);
	if (!ReportErrors(*this, Result))
	{
		return false;
	}
	TestEqual(TEXT("Three values changed"), Result.Changed.Num(), 3);
	TestTrue(TEXT("Package marked dirty"), Asset->GetPackage()->IsDirty());

	TestEqual(TEXT("FlickerSpeed"), GetDouble(Asset, TEXT("FlickerSpeed")), 2.5);
	TestEqual(TEXT("Tint"), GetTint(Asset), FLinearColor(1.0f, 0.5f, 0.25f, 1.0f));
	TestEqual(TEXT("Class default untouched"), GetDouble(Data.GetClassDefault(), TEXT("FlickerSpeed")), 0.0);

	const FAssetDefaults Captured = Capture(Asset);
	TestEqual(TEXT("Captured keys"), JoinCapturedKeys(Asset), FString(TEXT("FlickerSpeed,Steps,Tint")));

	const FString Json = ToJson(Captured);
	FAssetDefaults Parsed;
	FString Error;
	TestTrue(TEXT("Parses"), FromJson(Json, Parsed, Error));
	TestTrue(TEXT("Values equal"), ValuesEqual(Parsed, Captured));
	const FApplyResult Again = Import(Asset, Parsed);
	TestFalse(TEXT("Re-import succeeded"), Again.HasErrors());
	TestEqual(TEXT("Re-importing the export changes nothing"), Again.Changed.Num(), 0);
	TestEqual(TEXT("Export is stable"), ToJson(Capture(Asset)), Json);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncRejectsBadValuesTest, "JsonDataAssetSync.RejectsBadValues", TestFlags)
bool FJsonDataAssetSyncRejectsBadValuesTest::RunTest(const FString&)
{
	FTestData Data;
	UDataAsset* Asset = Data.Asset;

	auto Rejects = [this, Asset](const TCHAR* What, TFunctionRef<void(FAssetDefaults&)> Edit)
	{
		FAssetDefaults Doc = MakeDoc(Asset);
		Doc.Self.Add(TEXT("FlickerSpeed"), TEXT("2.5"));
		Edit(Doc);
		const FApplyResult Result = Import(Asset, Doc);
		TestTrue(FString(What) + TEXT(": rejected"), Result.HasErrors());
		TestEqual(FString(What) + TEXT(": nothing written"), Result.Changed.Num(), 0);
		TestEqual(FString(What) + TEXT(": valid value not applied either"), GetDouble(Asset, TEXT("FlickerSpeed")), 0.0);
	};

	Rejects(TEXT("Unknown property"), [](FAssetDefaults& Doc) { Doc.Self.Add(TEXT("NoSuchProperty"), TEXT("1")); });
	Rejects(TEXT("Wrong case"), [](FAssetDefaults& Doc) { Doc.Self.Add(TEXT("tint"), TEXT("(R=1.0,G=1.0,B=1.0,A=1.0)")); });
	Rejects(TEXT("Bad array text"), [](FAssetDefaults& Doc) { Doc.Self.Add(TEXT("Steps"), TEXT("(1,2")); });
	Rejects(TEXT("Unknown struct field"), [](FAssetDefaults& Doc) { Doc.Self.Add(TEXT("Tint"), TEXT("(X=1)")); });
	Rejects(TEXT("Trailing text"), [](FAssetDefaults& Doc) { Doc.Self.Add(TEXT("Steps"), TEXT("(1,2) junk")); });
	Rejects(TEXT("Class-only variable"), [](FAssetDefaults& Doc) { Doc.Self.Add(TEXT("Hidden"), TEXT("1.0")); });
	Rejects(TEXT("Wrong asset"), [](FAssetDefaults& Doc) { Doc.Asset = TEXT("/Game/Other/DA_Other"); });

	TestTrue(TEXT("The class Blueprint is not a data asset"), Apply(Data.ClassBlueprint, MakeDoc(Data.ClassBlueprint)).HasErrors());
	TestTrue(TEXT("The class default is not a data asset"), Apply(Data.GetClassDefault(), MakeDoc(Data.GetClassDefault())).HasErrors());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncMissingKeyResetsTest, "JsonDataAssetSync.MissingKeyResets", TestFlags)
bool FJsonDataAssetSyncMissingKeyResetsTest::RunTest(const FString&)
{
	// The class default of FlickerSpeed is 7, so a reset is visible as more than a zeroed value.
	FTestData Data(7.0);
	UDataAsset* Asset = Data.Asset;
	TestEqual(TEXT("The asset starts at the class default"), GetDouble(Asset, TEXT("FlickerSpeed")), 7.0);
	TestEqual(TEXT("A class default is not captured"), JoinCapturedKeys(Asset), FString());

	FAssetDefaults Doc = MakeDoc(Asset);
	Doc.Self.Add(TEXT("FlickerSpeed"), TEXT("2.5"));
	Doc.Self.Add(TEXT("Tint"), TEXT("(R=1.0,G=0.0,B=0.0,A=1.0)"));
	if (!ImportOk(*this, Asset, Doc))
	{
		return false;
	}

	// A document without FlickerSpeed: it goes back to the class default, Tint stays.
	FAssetDefaults Partial = MakeDoc(Asset);
	Partial.Self.Add(TEXT("Tint"), TEXT("(R=1.0,G=0.0,B=0.0,A=1.0)"));
	const FApplyResult Result = Import(Asset, Partial);
	if (!ReportErrors(*this, Result))
	{
		return false;
	}
	TestEqual(TEXT("One key changed"), FString::Join(Result.Changed, TEXT(",")), FString(TEXT("FlickerSpeed")));
	TestEqual(TEXT("FlickerSpeed reset to the class default"), GetDouble(Asset, TEXT("FlickerSpeed")), 7.0);
	TestEqual(TEXT("Tint kept"), GetTint(Asset), FLinearColor(1.0f, 0.0f, 0.0f, 1.0f));
	TestEqual(TEXT("Only Tint captured"), JoinCapturedKeys(Asset), FString(TEXT("Tint")));

	// An empty document resets everything.
	if (!ReportErrors(*this, Apply(Asset, MakeDoc(Asset))))
	{
		return false;
	}
	TestEqual(TEXT("Nothing captured after the reset"), JoinCapturedKeys(Asset), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncMergeTest, "JsonDataAssetSync.Merge", TestFlags)
bool FJsonDataAssetSyncMergeTest::RunTest(const FString&)
{
	// What the JSON Sync tab does: base = the asset when the JSON change was seen; the user changed Tint locally since; the JSON
	// changed FlickerSpeed and dropped Steps. Applying the JSON-side keys keeps the local Tint.
	FTestData Data;
	UDataAsset* Asset = Data.Asset;
	FAssetDefaults Start = MakeDoc(Asset);
	Start.Self.Add(TEXT("FlickerSpeed"), TEXT("2.5"));
	Start.Self.Add(TEXT("Steps"), TEXT("(1,2)"));
	if (!ImportOk(*this, Asset, Start))
	{
		return false;
	}
	const FAssetDefaults Base = Capture(Asset);

	FAssetDefaults File = Base;
	File.Self.Add(TEXT("FlickerSpeed"), TEXT("3.000000"));
	File.Self.Remove(TEXT("Steps"));

	SetTint(Asset, FLinearColor(0.0f, 0.0f, 1.0f, 1.0f));
	const FAssetDefaults Current = Capture(Asset);

	const TArray<FKeyDiff> Diffs = DiffKeys(&Base, File, Current);
	TestEqual(TEXT("Differing keys"), JoinKeys(Diffs), FString(TEXT("FlickerSpeed,Steps,Tint")));
	const FKeyDiff* Speed = FindDiff(Diffs, TEXT("FlickerSpeed"));
	const FKeyDiff* Steps = FindDiff(Diffs, TEXT("Steps"));
	const FKeyDiff* Tint = FindDiff(Diffs, TEXT("Tint"));
	TestTrue(TEXT("FlickerSpeed changed in the JSON"), Speed && Speed->Change == EKeyChange::Json && Speed->TakeJsonByDefault());
	TestTrue(TEXT("Steps removed in the JSON"), Steps && Steps->Change == EKeyChange::Json && !Steps->JsonValue.IsSet());
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
	if (!ReportErrors(*this, Apply(Asset, Merged)))
	{
		return false;
	}
	TestEqual(TEXT("FlickerSpeed from the JSON"), GetDouble(Asset, TEXT("FlickerSpeed")), 3.0);
	TestEqual(TEXT("Steps reset from the JSON"), JoinCapturedKeys(Asset), FString(TEXT("FlickerSpeed,Tint")));
	TestEqual(TEXT("Local Tint kept"), GetTint(Asset), FLinearColor(0.0f, 0.0f, 1.0f, 1.0f));

	// After the apply only the asset-side key differs from the file: saving writes it to the JSON.
	TestEqual(TEXT("Only Tint left"), JoinKeys(DiffKeys(&Base, File, Capture(Asset))), FString(TEXT("Tint")));

	// Without a base, every difference is a conflict and nothing is ticked.
	for (const FKeyDiff& Diff : DiffKeys(nullptr, File, Current))
	{
		TestTrue(FString::Printf(TEXT("%s is a conflict without a base"), *Diff.Key), Diff.Change == EKeyChange::Both && !Diff.TakeJsonByDefault());
	}
	// Changed on both sides to different values: a conflict.
	FAssetDefaults Both = File;
	Both.Self.Add(TEXT("Tint"), TEXT("(R=0.0,G=1.0,B=0.0,A=1.0)"));
	const FKeyDiff* TintBoth = FindDiff(DiffKeys(&Base, Both, Current), TEXT("Tint"));
	TestTrue(TEXT("Both sides changed Tint"), TintBoth && TintBoth->Change == EKeyChange::Both);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncTakeKeysTest, "JsonDataAssetSync.TakeKeys", TestFlags)
bool FJsonDataAssetSyncTakeKeysTest::RunTest(const FString&)
{
	FAssetDefaults Current;
	Current.Asset = TEXT("/Game/X");
	Current.Self.Add(TEXT("Keep"), TEXT("1"));
	Current.Self.Add(TEXT("Remove"), TEXT("1"));

	FAssetDefaults Source;
	Source.Self.Add(TEXT("Keep"), TEXT("2"));
	Source.Self.Add(TEXT("Add"), TEXT("3"));

	const FAssetDefaults Result = TakeKeys(Current, Source, { TEXT("Add"), TEXT("Remove") });
	TestEqual(TEXT("Untaken key keeps the current value"), Result.Self.FindRef(TEXT("Keep")), FString(TEXT("1")));
	TestEqual(TEXT("Taken key added"), Result.Self.FindRef(TEXT("Add")), FString(TEXT("3")));
	TestFalse(TEXT("Taken key missing from the source is removed"), Result.Self.Contains(TEXT("Remove")));
	TestEqual(TEXT("Asset field kept"), Result.Asset, Current.Asset);

	FAssetDefaults Renamed = Current;
	Renamed.Asset = TEXT("/Game/Y");
	Renamed.Parent = TEXT("/Script/Engine.PrimaryDataAsset");
	TestTrue(TEXT("ValuesEqual ignores asset and parent"), ValuesEqual(Current, Renamed));
	TestFalse(TEXT("ValuesEqual sees values"), ValuesEqual(Current, Result));
	TestEqual(TEXT("Flatten keys"), FString::Join([&] { TArray<FString> Keys; Flatten(Current).GetKeys(Keys); Keys.Sort(); return Keys; }(), TEXT(",")),
		FString(TEXT("Keep,Remove")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncChangedAfterReviewTest, "JsonDataAssetSync.ChangedAfterReview", TestFlags)
bool FJsonDataAssetSyncChangedAfterReviewTest::RunTest(const FString&)
{
	// The subsystem's case: the user reviewed file Reviewed, took Speed from it and kept the asset's Tint, then the file changed
	// again (Updated) before the save. With the reviewed file as the base, the new change is a JSON change and the kept value an
	// asset change, so the user's decision stands and Automatic mode may apply the new change.
	FAssetDefaults Reviewed;
	Reviewed.Asset = TEXT("/Game/X");
	Reviewed.Self.Add(TEXT("Speed"), TEXT("2"));
	Reviewed.Self.Add(TEXT("Tint"), TEXT("Red"));
	Reviewed.Self.Add(TEXT("Size"), TEXT("1"));

	FAssetDefaults Asset = Reviewed;
	Asset.Self.Add(TEXT("Tint"), TEXT("Blue"));

	FAssetDefaults Updated = Reviewed;
	Updated.Self.Add(TEXT("Size"), TEXT("5"));

	const TArray<FKeyDiff> Diffs = DiffKeys(&Reviewed, Updated, Asset);
	TestEqual(TEXT("Differing keys"), JoinKeys(Diffs), FString(TEXT("Size,Tint")));
	const FKeyDiff* Size = FindDiff(Diffs, TEXT("Size"));
	const FKeyDiff* Tint = FindDiff(Diffs, TEXT("Tint"));
	TestTrue(TEXT("New file change is a JSON change"), Size && Size->Change == EKeyChange::Json);
	TestTrue(TEXT("Kept asset value is an asset change"), Tint && Tint->Change == EKeyChange::Asset && !Tint->TakeJsonByDefault());
	TestTrue(TEXT("Automatic mode may apply it"), CanAutoApply(true, false, Diffs));

	// The new file also changed the kept key: a conflict, left for the user.
	FAssetDefaults Clash = Updated;
	Clash.Self.Add(TEXT("Tint"), TEXT("Green"));
	const TArray<FKeyDiff> ClashDiffs = DiffKeys(&Reviewed, Clash, Asset);
	const FKeyDiff* TintClash = FindDiff(ClashDiffs, TEXT("Tint"));
	TestTrue(TEXT("Both changed Tint"), TintClash && TintClash->Change == EKeyChange::Both);
	TestFalse(TEXT("Automatic mode leaves a conflict"), CanAutoApply(true, false, ClashDiffs));

	// The new file changed a key the user took from the reviewed file (e.g. a teammate reverted it): a JSON change, ticked.
	FAssetDefaults Reverted = Reviewed;
	Reverted.Self.Add(TEXT("Speed"), TEXT("1"));
	const FKeyDiff* Speed = FindDiff(DiffKeys(&Reviewed, Reverted, Asset), TEXT("Speed"));
	TestTrue(TEXT("Reverted key is a JSON change"), Speed && Speed->Change == EKeyChange::Json);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncCacheTest, "JsonDataAssetSync.Cache", TestFlags)
bool FJsonDataAssetSyncCacheTest::RunTest(const FString&)
{
	const FString Dir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("JsonDataAssetSync"), FGuid::NewGuid().ToString());
	const FString Path = FPaths::Combine(Dir, TEXT("cache.json"));

	FSyncCache Cache;
	TestTrue(TEXT("Missing cache loads empty"), Cache.Load(Dir) && Cache.Num() == 0);
	Cache.Set(TEXT("/Game/B"), { TEXT("j2"), FString(), true });
	Cache.Set(TEXT("/Game/A"), { TEXT("j1"), TEXT("a1"), false });
	FAssetDefaults Base;
	Base.Asset = TEXT("/Game/B");
	Base.Self.Add(TEXT("Speed"), TEXT("1.0"));
	TestTrue(TEXT("Base saves"), Cache.SaveBase(TEXT("/Game/B"), Base));
	TestTrue(TEXT("Cache saves"), Cache.Save());

	FSyncCache Loaded;
	TestTrue(TEXT("Cache loads"), Loaded.Load(Dir));
	const FSyncEntry* A = Loaded.Find(TEXT("/Game/A"));
	const FSyncEntry* B = Loaded.Find(TEXT("/Game/B"));
	TestTrue(TEXT("Cache round trip"), Loaded.Num() == 2 && A && A->JsonHash == TEXT("j1") && A->AssetHash == TEXT("a1") && !A->bPending
		&& B && B->bPending && B->AssetHash.IsEmpty());
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncBaseChoiceTest, "JsonDataAssetSync.BaseChoice", TestFlags)
bool FJsonDataAssetSyncBaseChoiceTest::RunTest(const FString&)
{
	// Which side changed what, from the hashes recorded at the last sync.
	FBaseInputs JsonOnly;
	JsonOnly.bHasEntry = true;
	TestTrue(TEXT("JSON changed, asset not: base is the asset"), ChooseBase(JsonOnly) == EBaseSource::Current);

	FBaseInputs Pending = JsonOnly;
	Pending.bPending = true;
	Pending.bAssetChanged = true;
	TestTrue(TEXT("Already pending: stored base"), ChooseBase(Pending) == EBaseSource::Stored);

	FBaseInputs Unknown;
	TestTrue(TEXT("Never synced: no base"), ChooseBase(Unknown) == EBaseSource::None);

	FBaseInputs AssetOnly = JsonOnly;
	AssetOnly.bJsonChanged = false;
	AssetOnly.bAssetChanged = true;
	TestTrue(TEXT("Asset changed, JSON not: base is the file (JSON out of date)"), ChooseBase(AssetOnly) == EBaseSource::File);
	AssetOnly.bAssetDirty = true;
	AssetOnly.bParentDiffers = true;
	TestTrue(TEXT("Unchanged JSON is the base even if the asset is dirty or its class changed"), ChooseBase(AssetOnly) == EBaseSource::File);

	FBaseInputs BothChanged = JsonOnly;
	BothChanged.bAssetChanged = true;
	TestTrue(TEXT("Both changed: no base"), ChooseBase(BothChanged) == EBaseSource::None);
	FBaseInputs Dirty = JsonOnly;
	Dirty.bAssetDirty = true;
	TestTrue(TEXT("JSON changed, unsaved edits: no base"), ChooseBase(Dirty) == EBaseSource::None);
	FBaseInputs OtherClass = JsonOnly;
	OtherClass.bParentDiffers = true;
	TestTrue(TEXT("JSON changed, other class: no base"), ChooseBase(OtherClass) == EBaseSource::None);

	// Automatic mode applies on its own only when the JSON changed something and nothing is in doubt.
	auto MakeDiff = [](const TCHAR* Key, EKeyChange Change)
	{
		FKeyDiff Diff;
		Diff.Key = Key;
		Diff.Change = Change;
		return Diff;
	};
	const TArray<FKeyDiff> JsonKeys = { MakeDiff(TEXT("A"), EKeyChange::Json), MakeDiff(TEXT("B"), EKeyChange::Asset) };
	const TArray<FKeyDiff> AssetKeys = { MakeDiff(TEXT("B"), EKeyChange::Asset) };
	const TArray<FKeyDiff> Conflict = { MakeDiff(TEXT("A"), EKeyChange::Json), MakeDiff(TEXT("C"), EKeyChange::Both) };
	TestTrue(TEXT("Auto-apply JSON keys"), CanAutoApply(true, false, JsonKeys));
	TestFalse(TEXT("No auto-apply without a base"), CanAutoApply(false, false, JsonKeys));
	TestFalse(TEXT("No auto-apply with another class"), CanAutoApply(true, true, JsonKeys));
	TestFalse(TEXT("No auto-apply with a conflict"), CanAutoApply(true, false, Conflict));
	TestFalse(TEXT("No auto-apply of an out-of-date JSON"), CanAutoApply(true, false, AssetKeys));
	TestFalse(TEXT("No auto-apply without keys"), CanAutoApply(true, false, {}));

	// A JSON that is only out of date: with the file as base, every difference is an asset change and nothing is ticked.
	FAssetDefaults File;
	File.Self.Add(TEXT("Speed"), TEXT("1.0"));
	File.Self.Add(TEXT("Removed"), TEXT("2"));
	FAssetDefaults Current;
	Current.Self.Add(TEXT("Speed"), TEXT("5.0"));
	Current.Self.Add(TEXT("Added"), TEXT("3"));
	const TArray<FKeyDiff> Diffs = DiffKeys(&File, File, Current);
	TestEqual(TEXT("Out-of-date keys"), JoinKeys(Diffs), FString(TEXT("Added,Removed,Speed")));
	TestFalse(TEXT("Every key changed in the asset"), Diffs.ContainsByPredicate([](const FKeyDiff& Diff) { return Diff.Change != EKeyChange::Asset || Diff.TakeJsonByDefault(); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncSettingsTest, "JsonDataAssetSync.Settings", TestFlags)
bool FJsonDataAssetSyncSettingsTest::RunTest(const FString&)
{
	const UJsonDataAssetSyncSettings* Fresh = NewObject<UJsonDataAssetSyncSettings>(GetTransientPackage());
	TestTrue(TEXT("Automatic by default"), Fresh->Mode == EJsonSyncMode::Automatic);
	TestEqual(TEXT("No excluded folders by default"), Fresh->ExcludedFolders.Num(), 0);

	const TArray<FString> Roots = GetSyncedRoots();
	TestTrue(TEXT("/Game is synced"), Roots.Contains(TEXT("/Game")));
	TestFalse(TEXT("/Engine is not synced"), Roots.Contains(TEXT("/Engine")));
	TestTrue(TEXT("Under /Game"), IsSyncedPackage(TEXT("/Game/Features/DA_Lamp")));
	TestFalse(TEXT("Engine content"), IsSyncedPackage(TEXT("/Engine/EngineMaterials/DA_X")));
	TestFalse(TEXT("Temp package"), IsSyncedPackage(TEXT("/Temp/JsonDataAssetSyncTests/DA_X")));
	TestFalse(TEXT("A root is matched as a whole folder"), IsSyncedPackage(TEXT("/GameX/DA_X")));

	// The excluded folders of the real settings, restored afterwards (not saved to the ini).
	UJsonDataAssetSyncSettings* Settings = GetMutableDefault<UJsonDataAssetSyncSettings>();
	const TArray<FDirectoryPath> Saved = Settings->ExcludedFolders;
	ON_SCOPE_EXIT { Settings->ExcludedFolders = Saved; };
	FDirectoryPath ThirdParty;
	ThirdParty.Path = TEXT("/Game/ThirdParty/");
	Settings->ExcludedFolders = { ThirdParty };
	TestFalse(TEXT("Excluded folder"), IsSyncedPackage(TEXT("/Game/ThirdParty/Pack/DA_X")));
	TestFalse(TEXT("Excluded folder, other case"), IsSyncedPackage(TEXT("/Game/thirdparty/DA_X")));
	TestTrue(TEXT("A folder is excluded as a whole"), IsSyncedPackage(TEXT("/Game/ThirdPartyTools/DA_X")));
	TestTrue(TEXT("Other folders stay synced"), IsSyncedPackage(TEXT("/Game/Features/DA_Lamp")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncDataAssetTest, "JsonDataAssetSync.DataAsset", TestFlags)
bool FJsonDataAssetSyncDataAssetTest::RunTest(const FString&)
{
	FTestData Data;
	TestTrue(TEXT("Data asset"), IsDataAsset(Data.Asset));
	TestTrue(TEXT("Data asset from asset data"), IsDataAsset(FAssetData(Data.Asset)));
	TestFalse(TEXT("The class Blueprint is not a data asset"), IsDataAsset(Data.ClassBlueprint));
	TestFalse(TEXT("The class Blueprint's asset data is not a data asset"), IsDataAsset(FAssetData(Data.ClassBlueprint)));
	TestFalse(TEXT("A class default is not a data asset"), IsDataAsset(Data.GetClassDefault()));
	TestFalse(TEXT("Null is not a data asset"), IsDataAsset(static_cast<const UObject*>(nullptr)));

	// Other assets are not synced.
	UPackage* Package = Data.Asset->GetPackage();
	TestFalse(TEXT("A package is not a data asset"), IsDataAsset(Package));
	TestTrue(TEXT("Apply refuses other assets"), Apply(Package, MakeDoc(Package)).HasErrors());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonDataAssetSyncMissingPairsTest, "JsonDataAssetSync.MissingPairs", TestFlags)

bool FJsonDataAssetSyncMissingPairsTest::RunTest(const FString&)
{
	// A JSON root of its own, so the project's JSON files do not matter.
	const FString JsonRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("JsonDataAssetSyncTests/MissingPairs"));
	IFileManager::Get().DeleteDirectory(*JsonRoot, false, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*JsonRoot, false, true); };

	UJsonDataAssetSyncSettings* Settings = GetMutableDefault<UJsonDataAssetSyncSettings>();
	const TArray<FDirectoryPath> Saved = Settings->ExcludedFolders;
	ON_SCOPE_EXIT { Settings->ExcludedFolders = Saved; };
	FDirectoryPath Excluded;
	Excluded.Path = TEXT("/Game/JsonDataAssetSyncTestsExcluded");
	Settings->ExcludedFolders = { Excluded };

	const FString Gone = GetJsonPath(JsonRoot, TEXT("/Game/JsonDataAssetSyncTestsGone/DA_Gone"));
	const FString Skipped = GetJsonPath(JsonRoot, TEXT("/Game/JsonDataAssetSyncTestsExcluded/DA_Gone"));
	TestTrue(TEXT("Write a JSON without its asset"), FFileHelper::SaveStringToFile(TEXT("{}\n"), *Gone));
	TestTrue(TEXT("Write a JSON in an excluded folder"), FFileHelper::SaveStringToFile(TEXT("{}\n"), *Skipped));

	FMissingPairs Missing = FindMissingPairs(JsonRoot);
	TestTrue(TEXT("A JSON without its asset is reported"), Missing.JsonWithoutAsset.Contains(FPaths::ConvertRelativePathToFull(Gone)));
	TestFalse(TEXT("A JSON in an excluded folder is not"), Missing.JsonWithoutAsset.Contains(FPaths::ConvertRelativePathToFull(Skipped)));
	for (const FString& PackageName : Missing.AssetsWithoutJson)
	{
		TestTrue(FString::Printf(TEXT("%s is synced"), *PackageName), IsSyncedPackage(PackageName));
	}
	TArray<FString> Sorted = Missing.AssetsWithoutJson;
	Sorted.Sort();
	TestEqual(TEXT("Assets are sorted"), Missing.AssetsWithoutJson, Sorted);

	// A data asset of the project, if it has any: with its JSON it is in neither list.
	if (Missing.AssetsWithoutJson.Num() > 0)
	{
		const FString PackageName = Missing.AssetsWithoutJson[0];
		const FString Path = GetJsonPath(JsonRoot, PackageName);
		TestTrue(TEXT("Write the asset's JSON"), FFileHelper::SaveStringToFile(TEXT("{}\n"), *Path));
		Missing = FindMissingPairs(JsonRoot);
		TestFalse(TEXT("An asset with its JSON is not reported"), Missing.AssetsWithoutJson.Contains(PackageName));
		TestFalse(TEXT("Its JSON is not reported"), Missing.JsonWithoutAsset.Contains(FPaths::ConvertRelativePathToFull(Path)));
	}
	return true;
}

#endif
