#include "../AssetMerge.h"

#if WITH_DEV_AUTOMATION_TESTS

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
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Layout/Margin.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace JsonBlueprintSync::AssetMergeTests
{
	using namespace JsonBlueprintSync::AssetMerge;

	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString Lines(std::initializer_list<const TCHAR*> InLines)
	{
		FString Text;
		for (const TCHAR* Line : InLines)
		{
			Text += Line;
			Text += TEXT("\n");
		}
		return Text;
	}

	const FMergeRow* FindRow(const TArray<FMergeRow>& Rows, const FString& Object, const FString& Path)
	{
		return Rows.FindByPredicate([&](const FMergeRow& Row) { return Row.Object == Object && Row.Path == Path; });
	}

	bool TestStatus(FAutomationTestBase& Test, const TCHAR* What, EMergeStatus Actual, EMergeStatus Expected)
	{
		return Test.TestEqual(What, FString(LexToString(Actual)), FString(LexToString(Expected)));
	}

	/** Checks a row's result; the message is in the test's name, so a failure shows why the row failed. */
	bool TestResult(FAutomationTestBase& Test, const TCHAR* What, const TArray<FRowResult>& Results, int32 Index, ERowResult Expected)
	{
		if (!Results.IsValidIndex(Index))
		{
			return Test.TestTrue(FString::Printf(TEXT("%s: no result"), What), false);
		}
		return Test.TestEqual(FString::Printf(TEXT("%s (%s)"), What, *Results[Index].Message), FString(LexToString(Results[Index].Result)),
			FString(LexToString(Expected)));
	}

	UPackage* NewTestPackage(const FString& Name)
	{
		UPackage* Package = CreatePackage(*(TEXT("/Temp/JsonBlueprintSyncTests/") + Name));
		Package->SetPackageFlags(PKG_InMemoryOnly);
		return Package;
	}

	UBlueprint* NewBlueprint(UClass* ParentClass, const FString& Name)
	{
		return FKismetEditorUtilities::CreateBlueprint(ParentClass, NewTestPackage(Name), FName(*Name), BPTYPE_Normal);
	}

	FEdGraphPinType DoublePin()
	{
		return FEdGraphPinType(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
	}

	FEdGraphPinType StructPin(UScriptStruct* Struct)
	{
		return FEdGraphPinType(UEdGraphSchema_K2::PC_Struct, NAME_None, Struct, EPinContainerType::None, false, FEdGraphTerminalType());
	}

	FEdGraphPinType ContainerPin(FName Category, FName SubCategory, UObject* SubCategoryObject, EPinContainerType Container,
		const FEdGraphTerminalType& Value = FEdGraphTerminalType())
	{
		return FEdGraphPinType(Category, SubCategory, SubCategoryObject, Container, false, Value);
	}

	FString MarginText(const FMargin& Margin)
	{
		return FString::Printf(TEXT("(%g, %g, %g, %g)"), Margin.Left, Margin.Top, Margin.Right, Margin.Bottom);
	}

	/** A map's keys and values as one line, sorted by key. */
	FString BoxesText(const TMap<FName, FMargin>* Boxes)
	{
		if (!Boxes)
		{
			return TEXT("none");
		}
		TArray<FName> Keys;
		Boxes->GetKeys(Keys);
		Keys.Sort(FNameLexicalLess());
		FString Text;
		for (const FName& Key : Keys)
		{
			Text += FString::Printf(TEXT("%s%s=%s"), Text.IsEmpty() ? TEXT("") : TEXT(" "), *Key.ToString(), *MarginText(Boxes->FindChecked(Key)));
		}
		return Text;
	}

	FString LabelsText(const TSet<FName>* Labels)
	{
		if (!Labels)
		{
			return TEXT("none");
		}
		TArray<FString> Names;
		for (const FName& Name : *Labels)
		{
			Names.Add(Name.ToString());
		}
		Names.Sort();
		return FString::Join(Names, TEXT(" "));
	}

	FString NumbersText(const TArray<double>* Numbers)
	{
		if (!Numbers)
		{
			return TEXT("none");
		}
		TArray<FString> Texts;
		for (double Number : *Numbers)
		{
			Texts.Add(FString::Printf(TEXT("%g"), Number));
		}
		return FString::Join(Texts, TEXT(" "));
	}

	FString InstanceText(const FInstancedStruct* Instance)
	{
		if (!Instance || !Instance->IsValid())
		{
			return TEXT("none");
		}
		if (const FMargin* Margin = Instance->GetPtr<FMargin>())
		{
			return TEXT("Margin") + MarginText(*Margin);
		}
		if (const FVector2D* Vector = Instance->GetPtr<FVector2D>())
		{
			return FString::Printf(TEXT("Vector2D(%g, %g)"), Vector->X, Vector->Y);
		}
		return Instance->GetScriptStruct()->GetName();
	}

	/** A row for an object of a test asset; a null Object gives an object path that doesn't exist. */
	FMergeRow MakeRow(const UObject* Object, const TCHAR* Path, EMergeAction Action, const TCHAR* Merged = nullptr)
	{
		FMergeRow Row;
		Row.Object = Object ? Object->GetPathName() : FString(TEXT("/Temp/JsonBlueprintSyncTests/Nothing.Nothing"));
		Row.Path = Path;
		Row.Action = Action;
		if (Merged)
		{
			Row.Merged = FString(Merged);
		}
		return Row;
	}

	template <typename ValueType>
	ValueType* FindValue(UObject* Object, const TCHAR* Name)
	{
		const FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), Name);
		return Property ? Property->ContainerPtrToValuePtr<ValueType>(Object) : nullptr;
	}

	double ReadDouble(UObject* Object, const TCHAR* Name)
	{
		const double* Value = FindValue<double>(Object, Name);
		return Value ? *Value : -1.0;
	}

	FMargin ReadMargin(UObject* Object, const TCHAR* Name)
	{
		const FMargin* Value = FindValue<FMargin>(Object, Name);
		return Value ? *Value : FMargin(-1.f);
	}

	bool TestMargin(FAutomationTestBase& Test, const TCHAR* What, const FMargin& Actual, const FMargin& Expected)
	{
		auto Text = [](const FMargin& Margin)
		{
			return FString::Printf(TEXT("(%g, %g, %g, %g)"), Margin.Left, Margin.Top, Margin.Right, Margin.Bottom);
		};
		return Test.TestEqual(What, Text(Actual), Text(Expected));
	}

	/** The reader's text of the test asset /Game/Test/DA. */
	const TCHAR* CurrentText()
	{
		static const FString Text = Lines({
			TEXT("# UAssReader merge text 3"),
			TEXT("# Asset: /Game/Test/DA"),
			TEXT("[/Game/Test/DA.DA] /Game/Test/BP_Data.BP_Data_C"),
			TEXT("Pair.B = 4"),
			TEXT("Name = \"x\""),
			TEXT("Map[\"D\"] = (removed)"),
			TEXT("Map[\"a\"] = 5"),
			TEXT("Set[\"C\"] = (added)"),
			TEXT("Blob = !notdecoded SomeStruct 16 0123456789abcdef"),
			TEXT("[/Game/Test/DA.DA:EventGraph] /Script/Engine.EdGraph"),
			TEXT("!graph = 1111111111111111"),
			TEXT("[/Game/Test/DA.DA:Old] /Script/Engine.SceneComponent"),
			TEXT("[/Game/Test/DA.DA:Light] /Script/Engine.PointLightComponent"),
			TEXT("Intensity = 5"),
			TEXT("[!ignored]"),
			TEXT("!package NameMap = 607 6a413e34558be5a0"),
		});
		return *Text;
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncAssetMergeParseTest, "JsonBlueprintSync.AssetMerge.Parse", JsonBlueprintSync::AssetMergeTests::TestFlags)
bool FJsonBlueprintSyncAssetMergeParseTest::RunTest(const FString& Parameters)
{
	using namespace JsonBlueprintSync::AssetMerge;
	using namespace JsonBlueprintSync::AssetMergeTests;
	FMergeText Text;
	TestTrue(TEXT("Parses"), ParseMergeText(CurrentText(), Text));
	TestEqual(TEXT("Format"), Text.Format, FString(TEXT("UAssReader merge text 3")));
	TestEqual(TEXT("Asset"), Text.Asset, FString(TEXT("/Game/Test/DA")));
	TestEqual(TEXT("Sections"), Text.Sections.Num(), 5);
	const FMergeSection* Section = Text.FindSection(TEXT("/Game/Test/DA.DA"));
	if (!TestNotNull(TEXT("Section"), Section))
	{
		return false;
	}
	TestEqual(TEXT("Class"), Section->Class, FString(TEXT("/Game/Test/BP_Data.BP_Data_C")));
	TestEqual(TEXT("Lines"), Section->Lines.Num(), 6);
	const FMergeLine* Map = Section->FindLine(TEXT("Map[\"a\"]"));
	TestTrue(TEXT("Map line"), Map && Map->Value == TEXT("5") && Map->LineNumber == 7);
	const FMergeSection* Empty = Text.FindSection(TEXT("/Game/Test/DA.DA:Old"));
	TestTrue(TEXT("Section without lines"), Empty && Empty->Lines.IsEmpty() && Empty->Class == TEXT("/Script/Engine.SceneComponent"));
	const FMergeSection* Ignored = Text.FindSection(IgnoredSection);
	TestTrue(TEXT("Ignored section has no class"), Ignored && Ignored->Class.IsEmpty() && Ignored->Lines.Num() == 1);

	// The separator is the first " = " outside quotes and brackets; CRLF line ends are fine.
	FMergeText Tricky;
	TestTrue(TEXT("Tricky parses"), ParseMergeText(TEXT("# F\r\n[/A.A] /C.C\r\nMap[\"k = v\"] = \"a = b\"\r\nS = \"q\\\" = \"\r\n"), Tricky));
	const FMergeLine* Key = Tricky.Sections.Num() == 1 ? Tricky.Sections[0].FindLine(TEXT("Map[\"k = v\"]")) : nullptr;
	TestTrue(TEXT("Key with separator"), Key && Key->Value == TEXT("\"a = b\""));
	TestEqual(TEXT("Class without CR"), Tricky.Sections.Num() == 1 ? Tricky.Sections[0].Class : FString(), FString(TEXT("/C.C")));
	const FMergeLine* Escaped = Tricky.Sections.Num() == 1 ? Tricky.Sections[0].FindLine(TEXT("S")) : nullptr;
	TestTrue(TEXT("Value with an escaped quote"), Escaped && Escaped->Value == TEXT("\"q\\\" = \""));

	FMergeText Conflicts;
	TestFalse(TEXT("Conflict markers fail"), ParseMergeText(Lines({
		TEXT("# F"),
		TEXT("[/A.A] /C.C"),
		TEXT("<<<<<<< .mine"),
		TEXT("X = 1"),
		TEXT("======="),
		TEXT("X = 2"),
		TEXT(">>>>>>> .theirs"),
	}), Conflicts));
	TestEqual(TEXT("Conflict lines"), Conflicts.ConflictLines, TArray<int32>({ 3, 5, 7 }));

	FMergeText Bad;
	TestFalse(TEXT("Errors fail"), ParseMergeText(Lines({
		TEXT("# F"),
		TEXT("X = 1"),
		TEXT("[/A.A] /C.C"),
		TEXT("Y"),
		TEXT("Z = 1"),
		TEXT("Z = 2"),
		TEXT("[/A.A] /C.C"),
		TEXT("[]"),
	}), Bad));
	TestEqual(TEXT("Error count"), Bad.Errors.Num(), 5);
	TestTrue(TEXT("Outside a section"), Bad.Errors.Num() > 0 && Bad.Errors[0].StartsWith(TEXT("Line 2:")));
	TestTrue(TEXT("No separator"), Bad.Errors.Num() > 1 && Bad.Errors[1].StartsWith(TEXT("Line 4:")));
	TestTrue(TEXT("Path twice"), Bad.Errors.Num() > 2 && Bad.Errors[2].StartsWith(TEXT("Line 6:")));
	TestTrue(TEXT("Section twice"), Bad.Errors.Num() > 3 && Bad.Errors[3].StartsWith(TEXT("Line 7:")));
	TestTrue(TEXT("Empty section"), Bad.Errors.Num() > 4 && Bad.Errors[4].StartsWith(TEXT("Line 8:")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncAssetMergeDiffTest, "JsonBlueprintSync.AssetMerge.Diff", JsonBlueprintSync::AssetMergeTests::TestFlags)
bool FJsonBlueprintSyncAssetMergeDiffTest::RunTest(const FString& Parameters)
{
	using namespace JsonBlueprintSync::AssetMerge;
	using namespace JsonBlueprintSync::AssetMergeTests;
	FMergeText Current;
	FMergeText Merged;
	TestTrue(TEXT("Current parses"), ParseMergeText(CurrentText(), Current));
	TestTrue(TEXT("Merged parses"), ParseMergeText(Lines({
		TEXT("# UAssReader merge text 3"),
		TEXT("# Asset: /Game/Test/DA"),
		TEXT("[/Game/Test/DA.DA] /Game/Test/BP_Data.BP_Data_C"),
		TEXT("Pair.B = 7"),                              // set
		TEXT("Map[\"D\"] = (removed)"),                  // same: no row
		TEXT("Map[\"a\"] = (removed)"),                  // remove
		TEXT("Set[\"C\"] = (added)"),                    // same
		TEXT("Set[\"E\"] = (added)"),                    // add
		TEXT("Blob = !notdecoded SomeStruct 16 fedcba9876543210"),
		// Name is missing: reset
		TEXT("[/Game/Test/DA.DA:EventGraph] /Script/Engine.EdGraph"),
		TEXT("!graph = 2222222222222222"),
		TEXT("[/Game/Test/DA.DA:New] /Script/Engine.SceneComponent"),
		TEXT("[/Game/Test/DA.DA:Light] /Script/Engine.SpotLightComponent"),
		TEXT("Intensity = 6"),
		TEXT("[!ignored]"),
		TEXT("!package NameMap = 999 0000000000000000"),
		// :Old is missing: removed
	}), Merged));

	const TArray<FMergeRow> Rows = DiffMergeText(Current, Merged);
	const FString Main = TEXT("/Game/Test/DA.DA");
	TestEqual(TEXT("Row count"), Rows.Num(), 9);

	const FMergeRow* Set = FindRow(Rows, Main, TEXT("Pair.B"));
	TestTrue(TEXT("Set"), Set && Set->Action == EMergeAction::Set && Set->Current == FString(TEXT("4")) && Set->Merged == FString(TEXT("7")));
	TestNull(TEXT("Same value: no row"), FindRow(Rows, Main, TEXT("Map[\"D\"]")));
	TestNull(TEXT("Same set line: no row"), FindRow(Rows, Main, TEXT("Set[\"C\"]")));
	const FMergeRow* Remove = FindRow(Rows, Main, TEXT("Map[\"a\"]"));
	TestTrue(TEXT("Remove"), Remove && Remove->Action == EMergeAction::Remove && Remove->Current == FString(TEXT("5")));
	const FMergeRow* Add = FindRow(Rows, Main, TEXT("Set[\"E\"]"));
	TestTrue(TEXT("Add"), Add && Add->Action == EMergeAction::Add && !Add->Current.IsSet());
	const FMergeRow* Reset = FindRow(Rows, Main, TEXT("Name"));
	TestTrue(TEXT("Reset"), Reset && Reset->Action == EMergeAction::Reset && Reset->Current == FString(TEXT("\"x\"")) && !Reset->Merged.IsSet());
	const FMergeRow* Blob = FindRow(Rows, Main, TEXT("Blob"));
	TestTrue(TEXT("Not decoded"), Blob && Blob->Action == EMergeAction::NotApplicable && !Blob->Reason.IsEmpty());
	const FMergeRow* Graph = FindRow(Rows, TEXT("/Game/Test/DA.DA:EventGraph"), TEXT("!graph"));
	TestTrue(TEXT("Graph"), Graph && Graph->Action == EMergeAction::NotApplicable);
	const FMergeRow* New = FindRow(Rows, TEXT("/Game/Test/DA.DA:New"), FString());
	TestTrue(TEXT("Added object"), New && New->Action == EMergeAction::NotApplicable && New->Merged == FString(TEXT("/Script/Engine.SceneComponent")));
	const FMergeRow* Light = FindRow(Rows, TEXT("/Game/Test/DA.DA:Light"), FString());
	TestTrue(TEXT("Changed class"), Light && Light->Action == EMergeAction::NotApplicable && Light->Current == FString(TEXT("/Script/Engine.PointLightComponent")));
	TestNull(TEXT("No rows inside an object whose class changed"), FindRow(Rows, TEXT("/Game/Test/DA.DA:Light"), TEXT("Intensity")));
	const FMergeRow* Old = FindRow(Rows, TEXT("/Game/Test/DA.DA:Old"), FString());
	TestTrue(TEXT("Removed object"), Old && Old->Action == EMergeAction::NotApplicable && !Old->Merged.IsSet());
	TestFalse(TEXT("Ignored section skipped"), Rows.ContainsByPredicate([](const FMergeRow& Row) { return Row.Object == IgnoredSection; }));

	// Order: merged.txt's lines first, then lines only in the asset; objects only in the asset last.
	TestTrue(TEXT("Order"), Rows.Num() == 9 && Rows[0].Path == TEXT("Pair.B") && Rows[4].Path == TEXT("Name") && Rows[8].Object == TEXT("/Game/Test/DA.DA:Old"));

	TestEqual(TEXT("Same text: no rows"), DiffMergeText(Current, Current).Num(), 0);

	// An instanced struct whose type changes: its member lines become rows even where they read the same, since the new
	// type starts from other values. Members of other values that read the same give no rows.
	FMergeText OldType;
	FMergeText NewType;
	TestTrue(TEXT("Old type parses"), ParseMergeText(Lines({
		TEXT("# F"),
		TEXT("[/A.A] /C.C"),
		TEXT("Inst.(type) = /Script/M.S1"),
		TEXT("Inst.B = 1"),
		TEXT("Inst.C = 2"),
		TEXT("Other.B = 1"),
	}), OldType));
	TestTrue(TEXT("New type parses"), ParseMergeText(Lines({
		TEXT("# F"),
		TEXT("[/A.A] /C.C"),
		TEXT("Inst.(type) = /Script/M.S2"),
		TEXT("Inst.B = 1"),
		TEXT("Other.B = 1"),
	}), NewType));
	const TArray<FMergeRow> Retyped = DiffMergeText(OldType, NewType);
	TestEqual(TEXT("Retyped rows"), Retyped.Num(), 3);
	TestTrue(TEXT("Type row first"), Retyped.Num() == 3 && Retyped[0].Path == TEXT("Inst.(type)") && Retyped[0].Action == EMergeAction::Set);
	const FMergeRow* SameMember = FindRow(Retyped, TEXT("/A.A"), TEXT("Inst.B"));
	TestTrue(TEXT("Same member of a retyped struct is a row"), SameMember && SameMember->Action == EMergeAction::Set && SameMember->Merged == FString(TEXT("1")));
	const FMergeRow* OldMember = FindRow(Retyped, TEXT("/A.A"), TEXT("Inst.C"));
	TestTrue(TEXT("Old type's member resets"), OldMember && OldMember->Action == EMergeAction::Reset);
	TestNull(TEXT("Other values stay quiet"), FindRow(Retyped, TEXT("/A.A"), TEXT("Other.B")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncAssetMergeFilesTest, "JsonBlueprintSync.AssetMerge.Files", JsonBlueprintSync::AssetMergeTests::TestFlags)
bool FJsonBlueprintSyncAssetMergeFilesTest::RunTest(const FString& Parameters)
{
	using namespace JsonBlueprintSync::AssetMerge;
	using namespace JsonBlueprintSync::AssetMergeTests;
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("JsonBlueprintSync"), FGuid::NewGuid().ToString(), TEXT("Merge")));
	FFileHelper::SaveStringToFile(TEXT("x"), *(Dir / TEXT("Game/Features/UI/WBP_HUD/merged.txt")));
	FFileHelper::SaveStringToFile(TEXT("x"), *(Dir / TEXT("MyPlugin/Sub/DA_Thing/merged.txt")));
	FFileHelper::SaveStringToFile(TEXT("x"), *(Dir / TEXT("Game/Other/notes.txt")));
	FFileHelper::SaveStringToFile(TEXT("x"), *(Dir / TEXT("merged.txt")));

	const TArray<FMergeFileLocation> Files = FindMergeFiles(Dir);
	TestEqual(TEXT("Count"), Files.Num(), 2);
	if (Files.Num() == 2)
	{
		TestEqual(TEXT("Project asset"), Files[0].PackageName, FString(TEXT("/Game/Features/UI/WBP_HUD")));
		TestEqual(TEXT("Plugin asset"), Files[1].PackageName, FString(TEXT("/MyPlugin/Sub/DA_Thing")));
		TestTrue(TEXT("Absolute file"), FPaths::IsSamePath(Files[0].MergedFile, Dir / TEXT("Game/Features/UI/WBP_HUD/merged.txt")));
	}
	TestEqual(TEXT("Missing folder"), FindMergeFiles(Dir / TEXT("Nothing")).Num(), 0);

	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Dir), false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncAssetMergeCheckTest, "JsonBlueprintSync.AssetMerge.Check", JsonBlueprintSync::AssetMergeTests::TestFlags)
bool FJsonBlueprintSyncAssetMergeCheckTest::RunTest(const FString& Parameters)
{
	using namespace JsonBlueprintSync::AssetMerge;
	using namespace JsonBlueprintSync::AssetMergeTests;
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("JsonBlueprintSync"), FGuid::NewGuid().ToString()));
	FMergeFileLocation Location;
	Location.PackageName = TEXT("/Game/Test/DA");
	Location.MergedFile = Dir / TEXT("merged.txt");
	const FString AssetFile = TEXT("C:/Fake/DA.uasset");

	FString Current = CurrentText();
	bool bReaderWorks = true;
	auto Check = [&](const FString& MergedText, const FString& InAssetFile = TEXT("C:/Fake/DA.uasset"), bool bDirty = false)
	{
		FFileHelper::SaveStringToFile(MergedText, *Location.MergedFile);
		return CheckMergeFile(Location, InAssetFile, bDirty, [&](const FString& File, FString& OutText, FString& OutError)
		{
			if (!bReaderWorks)
			{
				OutError = TEXT("reader broke");
				return false;
			}
			OutText = Current;
			return true;
		});
	};

	FString Changed = CurrentText();
	Changed.ReplaceInline(TEXT("Pair.B = 4"), TEXT("Pair.B = 8"));
	FString GraphOnly = CurrentText();
	GraphOnly.ReplaceInline(TEXT("!graph = 1111111111111111"), TEXT("!graph = 3333333333333333"));

	TestStatus(*this, TEXT("Ready"), Check(Changed).Status, EMergeStatus::Ready);
	TestEqual(TEXT("Ready rows"), Check(Changed).Rows.Num(), 1);
	TestStatus(*this, TEXT("Done when equal"), Check(CurrentText()).Status, EMergeStatus::Done);
	TestStatus(*this, TEXT("Done with only rows that don't apply"), Check(GraphOnly).Status, EMergeStatus::Done);
	TestStatus(*this, TEXT("Unsaved changes"), Check(Changed, AssetFile, true).Status, EMergeStatus::UnsavedChanges);
	TestEqual(TEXT("Unsaved changes keeps rows"), Check(Changed, AssetFile, true).Rows.Num(), 1);
	TestStatus(*this, TEXT("Asset not found"), Check(Changed, FString()).Status, EMergeStatus::AssetNotFound);

	FString Conflicted = Changed;
	Conflicted.ReplaceInline(TEXT("Pair.B = 8\n"), TEXT("<<<<<<< mine\nPair.B = 8\n=======\nPair.B = 9\n>>>>>>> theirs\n"));
	TestStatus(*this, TEXT("Conflict markers"), Check(Conflicted).Status, EMergeStatus::HasConflictMarkers);
	TestStatus(*this, TEXT("Errors"), Check(Changed + TEXT("no separator\n")).Status, EMergeStatus::HasErrors);
	FString OtherAsset = Changed;
	OtherAsset.ReplaceInline(TEXT("# Asset: /Game/Test/DA"), TEXT("# Asset: /Game/Test/Other"));
	TestStatus(*this, TEXT("Other asset"), Check(OtherAsset).Status, EMergeStatus::HasErrors);
	FString OtherFormat = Changed;
	OtherFormat.ReplaceInline(TEXT("merge text 3"), TEXT("merge text 4"));
	TestStatus(*this, TEXT("Format differs"), Check(OtherFormat).Status, EMergeStatus::FormatDiffers);

	bReaderWorks = false;
	const FMergeFile Failed = Check(Changed);
	TestTrue(TEXT("Reader failed"), Failed.Status == EMergeStatus::ReaderFailed && Failed.Message == TEXT("reader broke"));
	bReaderWorks = true;
	Current = TEXT("garbage\n");
	TestStatus(*this, TEXT("Reader text has errors"), Check(Changed).Status, EMergeStatus::ReaderFailed);

	TestFalse(TEXT("Missing merged.txt"), CheckMergeFile({ TEXT("/Game/X"), Dir / TEXT("none.txt") }, AssetFile, false,
		[](const FString&, FString&, FString&) { return true; }).Status == EMergeStatus::Ready);

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncAssetMergeApplyTest, "JsonBlueprintSync.AssetMerge.Apply", JsonBlueprintSync::AssetMergeTests::TestFlags)
bool FJsonBlueprintSyncAssetMergeApplyTest::RunTest(const FString& Parameters)
{
	using namespace JsonBlueprintSync::AssetMerge;
	using namespace JsonBlueprintSync::AssetMergeTests;

	// A parent Blueprint (Actor) with Speed (double), Pad (FMargin, saved member by member) and a point light "Light".
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprint* Parent = NewBlueprint(AActor::StaticClass(), TEXT("BP_MergeParent_") + Suffix);
	FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("Speed"), DoublePin());
	FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("Pad"), StructPin(FMargin::StaticStruct()));
	USCS_Node* LightNode = Parent->SimpleConstructionScript->CreateNode(UPointLightComponent::StaticClass(), TEXT("Light"));
	Parent->SimpleConstructionScript->AddNode(LightNode);
	FKismetEditorUtilities::CompileBlueprint(Parent, EBlueprintCompileOptions::SkipGarbageCollection);
	UObject* ParentDefaults = Parent->GeneratedClass->GetDefaultObject();
	UPointLightComponent* ParentLight = Cast<UPointLightComponent>(LightNode->ComponentTemplate);
	FMargin* ParentPad = FindValue<FMargin>(ParentDefaults, TEXT("Pad"));
	if (!TestNotNull(TEXT("Parent light template"), ParentLight) || !TestNotNull(TEXT("Parent Pad"), ParentPad))
	{
		return false;
	}
	*ParentPad = FMargin(1.f, 2.f, 3.f, 4.f);

	// A child that overrides Pad.Left and, in its own light template, AttenuationRadius.
	UBlueprint* Child = NewBlueprint(Parent->GeneratedClass, TEXT("BP_MergeChild_") + Suffix);
	UObject* ChildDefaults = Child->GeneratedClass->GetDefaultObject();
	FMargin* ChildPad = FindValue<FMargin>(ChildDefaults, TEXT("Pad"));
	UPointLightComponent* ChildLight = Cast<UPointLightComponent>(
		Child->GetInheritableComponentHandler(true)->CreateOverridenComponentTemplate(FComponentKey(LightNode)));
	if (!TestNotNull(TEXT("Child Pad"), ChildPad) || !TestNotNull(TEXT("Child light template"), ChildLight)
		|| !TestMargin(*this, TEXT("Child starts with the parent's Pad"), *ChildPad, FMargin(1.f, 2.f, 3.f, 4.f)))
	{
		return false;
	}
	ChildPad->Left = 9.f;
	ChildLight->AttenuationRadius = 123.f;

	FMergeRow NotApplicable = MakeRow(ParentDefaults, TEXT("!graph"), EMergeAction::NotApplicable, TEXT("2222222222222222"));
	NotApplicable.Reason = TEXT("A graph changed.");
	const TArray<FMergeRow> Rows = {
		MakeRow(ParentDefaults, TEXT("Speed"), EMergeAction::Set, TEXT("2.5")),
		MakeRow(ParentDefaults, TEXT("Pad.Top"), EMergeAction::Set, TEXT("20")),
		MakeRow(ParentDefaults, TEXT("Pad.Right"), EMergeAction::Reset),
		MakeRow(ParentLight, TEXT("Intensity"), EMergeAction::Set, TEXT("777")),
		MakeRow(ParentLight, TEXT("RelativeLocation"), EMergeAction::Set, TEXT("(X=1,Y=2,Z=3)")),
		MakeRow(ParentDefaults, TEXT("Speed"), EMergeAction::Set, TEXT("fast")),
		MakeRow(ParentDefaults, TEXT("Nope"), EMergeAction::Set, TEXT("1")),
		MakeRow(ParentDefaults, TEXT("Pad.Nope"), EMergeAction::Set, TEXT("1")),
		MakeRow(ParentDefaults, TEXT("Speed.X"), EMergeAction::Set, TEXT("1")),
		MakeRow(ParentDefaults, TEXT("Tags[0]"), EMergeAction::Set, TEXT("A")),
		MakeRow(ParentDefaults, TEXT("Tags.Num"), EMergeAction::Set, TEXT("1")),
		MakeRow(ParentDefaults, TEXT("RootComponent"), EMergeAction::Set, TEXT("None")),
		MakeRow(nullptr, TEXT("Speed"), EMergeAction::Set, TEXT("1")),
		MakeRow(ParentDefaults, TEXT("Map[\"a\"]"), EMergeAction::Add, TEXT("(added)")),
		NotApplicable,
		MakeRow(ParentDefaults, TEXT("Tags[0]"), EMergeAction::Set, TEXT("\"A \\\"b\\\"\"")),
	};
	const TArray<FRowResult> Results = ApplyRows(Parent, Rows);
	TestEqual(TEXT("One result per row"), Results.Num(), Rows.Num());
	TestResult(*this, TEXT("Set a value"), Results, 0, ERowResult::Applied);
	TestResult(*this, TEXT("Set a struct member"), Results, 1, ERowResult::Applied);
	TestResult(*this, TEXT("Reset a struct member"), Results, 2, ERowResult::Applied);
	TestResult(*this, TEXT("Set on a component template"), Results, 3, ERowResult::Applied);
	TestResult(*this, TEXT("Set a whole struct"), Results, 4, ERowResult::Applied);
	TestResult(*this, TEXT("Bad value"), Results, 5, ERowResult::Failed);
	TestResult(*this, TEXT("Unknown property"), Results, 6, ERowResult::Failed);
	TestResult(*this, TEXT("Unknown struct member"), Results, 7, ERowResult::Failed);
	TestResult(*this, TEXT("Member of a value that isn't a struct"), Results, 8, ERowResult::Failed);
	TestResult(*this, TEXT("Array element past the end"), Results, 9, ERowResult::Failed);
	TestResult(*this, TEXT("Array size"), Results, 10, ERowResult::Applied);
	TestResult(*this, TEXT("Not editable"), Results, 11, ERowResult::Failed);
	TestResult(*this, TEXT("Unknown object"), Results, 12, ERowResult::Failed);
	TestResult(*this, TEXT("Unknown map"), Results, 13, ERowResult::Failed);
	TestResult(*this, TEXT("Not applicable"), Results, 14, ERowResult::Failed);
	TestResult(*this, TEXT("Quoted name in a new element"), Results, 15, ERowResult::Applied);
	if (Results.Num() == Rows.Num())
	{
		TestTrue(TEXT("Unknown property says so"), Results[6].Message.Contains(TEXT("renamed or removed")));
		TestTrue(TEXT("An element past the end asks for the size first"), Results[9].Message.Contains(TEXT("Num")));
		TestTrue(TEXT("Not editable says so"), Results[11].Message.Contains(TEXT("Details panel")));
		TestEqual(TEXT("Not applicable gives the reason"), Results[14].Message, NotApplicable.Reason);
	}
	TestEqual(TEXT("Parent Tags resized"), CastChecked<AActor>(ParentDefaults)->Tags.Num(), 1);
	TestEqual(TEXT("Child Tags follow"), CastChecked<AActor>(ChildDefaults)->Tags.Num(), 1);
	TestEqual(TEXT("Parent tag unquoted"), CastChecked<AActor>(ParentDefaults)->Tags.IsEmpty() ? FString() : CastChecked<AActor>(ParentDefaults)->Tags[0].ToString(),
		FString(TEXT("A \"b\"")));

	TestEqual(TEXT("Parent Speed"), ReadDouble(ParentDefaults, TEXT("Speed")), 2.5);
	TestEqual(TEXT("Child inherits Speed"), ReadDouble(ChildDefaults, TEXT("Speed")), 2.5);
	TestMargin(*this, TEXT("Parent Pad: Top set, Right reset to the struct default"), *ParentPad, FMargin(1.f, 20.f, 0.f, 4.f));
	TestMargin(*this, TEXT("Child Pad: inherited members follow, the overridden Left stays"), *ChildPad, FMargin(9.f, 20.f, 0.f, 4.f));
	TestEqual(TEXT("Parent light Intensity"), ParentLight->Intensity, 777.f);
	TestEqual(TEXT("Child light inherits Intensity"), ChildLight->Intensity, 777.f);
	TestEqual(TEXT("Child light keeps its AttenuationRadius"), ChildLight->AttenuationRadius, 123.f);
	TestEqual(TEXT("Parent light location"), ParentLight->GetRelativeLocation(), FVector(1, 2, 3));
	TestEqual(TEXT("Child light inherits the location"), ChildLight->GetRelativeLocation(), FVector(1, 2, 3));
	TestTrue(TEXT("Parent marked modified"), Parent->GetPackage()->IsDirty());

	// Again: nothing left to write.
	const TArray<FRowResult> Again = ApplyRows(Parent, MakeArrayView(Rows).Left(5));
	for (int32 Index = 0; Index < Again.Num(); ++Index)
	{
		TestResult(*this, TEXT("Second apply"), Again, Index, ERowResult::Unchanged);
	}

	// Reset where the archetype doesn't have the variable: the type's default. Reset in the child: the parent's value.
	const TArray<FRowResult> Resets = ApplyRows(Parent, { MakeRow(ParentDefaults, TEXT("Speed"), EMergeAction::Reset) });
	TestResult(*this, TEXT("Reset a variable the parent class doesn't have"), Resets, 0, ERowResult::Applied);
	TestEqual(TEXT("Speed back to 0"), ReadDouble(ParentDefaults, TEXT("Speed")), 0.0);
	TestEqual(TEXT("Child follows"), ReadDouble(ChildDefaults, TEXT("Speed")), 0.0);
	const TArray<FRowResult> ChildResets = ApplyRows(Child, {
		MakeRow(ChildDefaults, TEXT("Pad.Left"), EMergeAction::Reset),
		MakeRow(ChildLight, TEXT("AttenuationRadius"), EMergeAction::Reset) });
	TestResult(*this, TEXT("Reset a child's member"), ChildResets, 0, ERowResult::Applied);
	TestResult(*this, TEXT("Reset a child's component value"), ChildResets, 1, ERowResult::Applied);
	TestEqual(TEXT("Child Pad.Left from the parent"), ChildPad->Left, 1.f);
	TestEqual(TEXT("Child AttenuationRadius from the parent's template"), ChildLight->AttenuationRadius, ParentLight->AttenuationRadius);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncAssetMergeApplyContainersTest, "JsonBlueprintSync.AssetMerge.ApplyContainers", JsonBlueprintSync::AssetMergeTests::TestFlags)
bool FJsonBlueprintSyncAssetMergeApplyContainersTest::RunTest(const FString& Parameters)
{
	using namespace JsonBlueprintSync::AssetMerge;
	using namespace JsonBlueprintSync::AssetMergeTests;

	// A parent Blueprint with an array of doubles, an array of margins, a map of name to margin, a set of names and an
	// instanced struct.
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprint* Parent = NewBlueprint(AActor::StaticClass(), TEXT("BP_MergeContainers_") + Suffix);
	FEdGraphTerminalType MarginValue;
	MarginValue.TerminalCategory = UEdGraphSchema_K2::PC_Struct;
	MarginValue.TerminalSubCategoryObject = FMargin::StaticStruct();
	FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("Numbers"), ContainerPin(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, nullptr, EPinContainerType::Array));
	FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("Pads"), ContainerPin(UEdGraphSchema_K2::PC_Struct, NAME_None, FMargin::StaticStruct(), EPinContainerType::Array));
	FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("Boxes"), ContainerPin(UEdGraphSchema_K2::PC_Name, NAME_None, nullptr, EPinContainerType::Map, MarginValue));
	FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("Labels"), ContainerPin(UEdGraphSchema_K2::PC_Name, NAME_None, nullptr, EPinContainerType::Set));
	FBlueprintEditorUtils::AddMemberVariable(Parent, TEXT("Inst"), StructPin(FInstancedStruct::StaticStruct()));
	FKismetEditorUtilities::CompileBlueprint(Parent, EBlueprintCompileOptions::SkipGarbageCollection);
	UObject* P = Parent->GeneratedClass->GetDefaultObject();
	TArray<double>* PNumbers = FindValue<TArray<double>>(P, TEXT("Numbers"));
	TArray<FMargin>* PPads = FindValue<TArray<FMargin>>(P, TEXT("Pads"));
	TMap<FName, FMargin>* PBoxes = FindValue<TMap<FName, FMargin>>(P, TEXT("Boxes"));
	TSet<FName>* PLabels = FindValue<TSet<FName>>(P, TEXT("Labels"));
	FInstancedStruct* PInst = FindValue<FInstancedStruct>(P, TEXT("Inst"));
	if (!TestTrue(TEXT("Parent variables"), PNumbers && PPads && PBoxes && PLabels && PInst))
	{
		return false;
	}
	*PNumbers = { 1.0, 2.0 };
	*PPads = { FMargin(1.f) };
	PBoxes->Add(TEXT("a"), FMargin(1.f, 2.f, 3.f, 4.f));
	PBoxes->Add(TEXT("b"), FMargin(5.f));
	*PLabels = { TEXT("X"), TEXT("Y") };
	PInst->InitializeAs<FMargin>(FMargin(1.f, 2.f, 3.f, 4.f));

	// A child that overrides one member of one map entry.
	UBlueprint* Child = NewBlueprint(Parent->GeneratedClass, TEXT("BP_MergeContainersChild_") + Suffix);
	UObject* C = Child->GeneratedClass->GetDefaultObject();
	TArray<double>* CNumbers = FindValue<TArray<double>>(C, TEXT("Numbers"));
	TArray<FMargin>* CPads = FindValue<TArray<FMargin>>(C, TEXT("Pads"));
	TMap<FName, FMargin>* CBoxes = FindValue<TMap<FName, FMargin>>(C, TEXT("Boxes"));
	TSet<FName>* CLabels = FindValue<TSet<FName>>(C, TEXT("Labels"));
	FInstancedStruct* CInst = FindValue<FInstancedStruct>(C, TEXT("Inst"));
	if (!TestTrue(TEXT("Child variables"), CNumbers && CPads && CBoxes && CLabels && CInst)
		|| !TestEqual(TEXT("Child starts with the parent's map"), BoxesText(CBoxes), BoxesText(PBoxes)))
	{
		return false;
	}
	CBoxes->FindChecked(TEXT("b")).Left = 9.f;

	const TArray<FMergeRow> Rows = {
		MakeRow(P, TEXT("Numbers.Num"), EMergeAction::Set, TEXT("3")),
		MakeRow(P, TEXT("Numbers[2]"), EMergeAction::Set, TEXT("7")),
		MakeRow(P, TEXT("Numbers[5]"), EMergeAction::Set, TEXT("1")),
		MakeRow(P, TEXT("Pads[0].Top"), EMergeAction::Set, TEXT("8")),
		MakeRow(P, TEXT("Boxes[\"a\"].Left"), EMergeAction::Set, TEXT("6")),
		MakeRow(P, TEXT("Boxes[\"b\"].Top"), EMergeAction::Set, TEXT("7")),
		MakeRow(P, TEXT("Boxes[\"c\"].Right"), EMergeAction::Set, TEXT("3")),
		MakeRow(P, TEXT("Boxes[\"e\"]"), EMergeAction::Set, TEXT("()")),
		MakeRow(P, TEXT("Labels[\"W\"]"), EMergeAction::Add, TEXT("(added)")),
		MakeRow(P, TEXT("Labels[\"X\"]"), EMergeAction::Remove, TEXT("(removed)")),
		MakeRow(P, TEXT("Inst.Top"), EMergeAction::Set, TEXT("9")),
		MakeRow(P, TEXT("Inst.Nope"), EMergeAction::Set, TEXT("1")),
		MakeRow(P, TEXT("Numbers.Nope"), EMergeAction::Set, TEXT("1")),
		MakeRow(P, TEXT("Boxes[0]"), EMergeAction::Set, TEXT("1")),
		MakeRow(P, TEXT("Labels[\"Q\"]"), EMergeAction::Set, TEXT("1")),
		MakeRow(P, TEXT("Numbers[0]"), EMergeAction::Remove, TEXT("(removed)")),
	};
	const TArray<FRowResult> Results = ApplyRows(Parent, Rows);
	TestEqual(TEXT("One result per row"), Results.Num(), Rows.Num());
	TestResult(*this, TEXT("Grow an array"), Results, 0, ERowResult::Applied);
	TestResult(*this, TEXT("Set a new element"), Results, 1, ERowResult::Applied);
	TestResult(*this, TEXT("Element past the end"), Results, 2, ERowResult::Failed);
	TestResult(*this, TEXT("Member of an array element"), Results, 3, ERowResult::Applied);
	TestResult(*this, TEXT("Member of a map entry"), Results, 4, ERowResult::Applied);
	TestResult(*this, TEXT("Member of an entry the child overrides elsewhere"), Results, 5, ERowResult::Applied);
	TestResult(*this, TEXT("Member of a new entry"), Results, 6, ERowResult::Applied);
	TestResult(*this, TEXT("New entry with default members"), Results, 7, ERowResult::Applied);
	TestResult(*this, TEXT("Add to a set"), Results, 8, ERowResult::Applied);
	TestResult(*this, TEXT("Remove from a set"), Results, 9, ERowResult::Applied);
	TestResult(*this, TEXT("Member of an instanced struct"), Results, 10, ERowResult::Applied);
	TestResult(*this, TEXT("Member the instanced struct's type doesn't have"), Results, 11, ERowResult::Failed);
	TestResult(*this, TEXT("Array followed by a name"), Results, 12, ERowResult::Failed);
	TestResult(*this, TEXT("Map followed by an index"), Results, 13, ERowResult::Failed);
	TestResult(*this, TEXT("Set element with a value"), Results, 14, ERowResult::Failed);
	TestResult(*this, TEXT("Remove an array element"), Results, 15, ERowResult::Failed);

	TestEqual(TEXT("Parent Numbers"), NumbersText(PNumbers), FString(TEXT("1 2 7")));
	TestEqual(TEXT("Child Numbers follow"), NumbersText(CNumbers), FString(TEXT("1 2 7")));
	TestEqual(TEXT("Parent Pads"), PPads->Num() == 1 ? MarginText((*PPads)[0]) : FString(), MarginText(FMargin(1.f, 8.f, 1.f, 1.f)));
	TestEqual(TEXT("Child Pads follow"), CPads->Num() == 1 ? MarginText((*CPads)[0]) : FString(), MarginText(FMargin(1.f, 8.f, 1.f, 1.f)));
	TestEqual(TEXT("Parent Boxes"), BoxesText(PBoxes),
		FString(TEXT("a=(6, 2, 3, 4) b=(5, 7, 5, 5) c=(0, 0, 3, 0) e=(0, 0, 0, 0)")));
	TestEqual(TEXT("Child Boxes follow and keep their own b.Left"), BoxesText(CBoxes),
		FString(TEXT("a=(6, 2, 3, 4) b=(9, 7, 5, 5) c=(0, 0, 3, 0) e=(0, 0, 0, 0)")));
	TestEqual(TEXT("Parent Labels"), LabelsText(PLabels), FString(TEXT("W Y")));
	TestEqual(TEXT("Child Labels follow"), LabelsText(CLabels), FString(TEXT("W Y")));
	TestEqual(TEXT("Parent Inst"), InstanceText(PInst), FString(TEXT("Margin(1, 9, 3, 4)")));
	TestEqual(TEXT("Child Inst follows"), InstanceText(CInst), FString(TEXT("Margin(1, 9, 3, 4)")));

	const TArray<FRowResult> Again = ApplyRows(Parent, Rows);
	for (int32 Index : { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10 })
	{
		TestResult(*this, TEXT("Second apply"), Again, Index, ERowResult::Unchanged);
	}

	// A type change: the members become the new type's defaults, then the member rows that follow set them.
	const TArray<FRowResult> Retype = ApplyRows(Parent, {
		MakeRow(P, TEXT("Inst.(type)"), EMergeAction::Set, TEXT("/Script/CoreUObject.Vector2D")),
		MakeRow(P, TEXT("Inst.X"), EMergeAction::Set, TEXT("4")),
		MakeRow(P, TEXT("Inst.Top"), EMergeAction::Reset),
		MakeRow(P, TEXT("Inst.(type)"), EMergeAction::Set, TEXT("/Script/Nope.Nope")),
		MakeRow(P, TEXT("Boxes[\"a\"]"), EMergeAction::Remove, TEXT("(removed)")),
		MakeRow(P, TEXT("Numbers.Num"), EMergeAction::Set, TEXT("1")),
		MakeRow(P, TEXT("Numbers[2]"), EMergeAction::Reset) });
	TestResult(*this, TEXT("Change the type"), Retype, 0, ERowResult::Applied);
	TestResult(*this, TEXT("Member of the new type"), Retype, 1, ERowResult::Applied);
	TestResult(*this, TEXT("Reset a member of the old type"), Retype, 2, ERowResult::Unchanged);
	TestResult(*this, TEXT("Unknown type"), Retype, 3, ERowResult::Failed);
	TestResult(*this, TEXT("Remove a map entry"), Retype, 4, ERowResult::Applied);
	TestResult(*this, TEXT("Shrink an array"), Retype, 5, ERowResult::Applied);
	TestResult(*this, TEXT("Reset an element that is gone"), Retype, 6, ERowResult::Unchanged);
	TestEqual(TEXT("Parent Inst retyped"), InstanceText(PInst), FString(TEXT("Vector2D(4, 0)")));
	TestEqual(TEXT("Child Inst follows the type"), InstanceText(CInst), FString(TEXT("Vector2D(4, 0)")));
	TestEqual(TEXT("Parent Boxes without a"), BoxesText(PBoxes), FString(TEXT("b=(5, 7, 5, 5) c=(0, 0, 3, 0) e=(0, 0, 0, 0)")));
	TestEqual(TEXT("Child Boxes without a"), BoxesText(CBoxes), FString(TEXT("b=(9, 7, 5, 5) c=(0, 0, 3, 0) e=(0, 0, 0, 0)")));
	TestEqual(TEXT("Parent Numbers shrunk"), NumbersText(PNumbers), FString(TEXT("1")));
	TestEqual(TEXT("Child Numbers follow the size"), NumbersText(CNumbers), FString(TEXT("1")));

	// Resets in the child take the parent's value at the same place, and drop what the parent doesn't have.
	CBoxes->Add(TEXT("own"), FMargin(2.f));
	CLabels->Add(TEXT("Z"));
	*CNumbers = { 5.0, 6.0 };
	CInst->InitializeAs<FMargin>(FMargin(3.f));
	const TArray<FRowResult> Resets = ApplyRows(Child, {
		MakeRow(C, TEXT("Boxes[\"b\"].Left"), EMergeAction::Reset),
		MakeRow(C, TEXT("Boxes[\"zz\"].Left"), EMergeAction::Reset),
		MakeRow(C, TEXT("Boxes[\"own\"].Top"), EMergeAction::Reset),
		MakeRow(C, TEXT("Labels[\"Z\"]"), EMergeAction::Reset),
		MakeRow(C, TEXT("Labels[\"X\"]"), EMergeAction::Reset),
		MakeRow(C, TEXT("Numbers.Num"), EMergeAction::Reset),
		MakeRow(C, TEXT("Numbers[0]"), EMergeAction::Reset),
		MakeRow(C, TEXT("Inst.(type)"), EMergeAction::Reset) });
	TestResult(*this, TEXT("Reset an entry member to the parent's"), Resets, 0, ERowResult::Applied);
	TestResult(*this, TEXT("Reset an entry neither has"), Resets, 1, ERowResult::Unchanged);
	TestResult(*this, TEXT("Reset an entry the parent doesn't have"), Resets, 2, ERowResult::Applied);
	TestResult(*this, TEXT("Reset a set element the parent doesn't have"), Resets, 3, ERowResult::Applied);
	TestResult(*this, TEXT("Reset a set element neither has"), Resets, 4, ERowResult::Unchanged);
	TestResult(*this, TEXT("Reset an array's size"), Resets, 5, ERowResult::Applied);
	TestResult(*this, TEXT("Reset an element that the size reset brought back"), Resets, 6, ERowResult::Unchanged);
	TestResult(*this, TEXT("Reset an instanced struct's type"), Resets, 7, ERowResult::Applied);
	TestEqual(TEXT("Child Boxes from the parent"), BoxesText(CBoxes), BoxesText(PBoxes));
	TestEqual(TEXT("Child Labels from the parent"), LabelsText(CLabels), LabelsText(PLabels));
	TestEqual(TEXT("Child Numbers from the parent"), NumbersText(CNumbers), NumbersText(PNumbers));
	TestEqual(TEXT("Child Inst from the parent"), InstanceText(CInst), FString(TEXT("Vector2D(4, 0)")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncAssetMergeApplyDataAssetTest, "JsonBlueprintSync.AssetMerge.ApplyDataAsset", JsonBlueprintSync::AssetMergeTests::TestFlags)
bool FJsonBlueprintSyncAssetMergeApplyDataAssetTest::RunTest(const FString& Parameters)
{
	using namespace JsonBlueprintSync::AssetMerge;
	using namespace JsonBlueprintSync::AssetMergeTests;

	// A data asset class (Blueprint) with an instance-editable Power and a class-only Hidden, and one data asset of it.
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprint* ClassBP = NewBlueprint(UPrimaryDataAsset::StaticClass(), TEXT("BP_MergeData_") + Suffix);
	FBlueprintEditorUtils::AddMemberVariable(ClassBP, TEXT("Power"), DoublePin());
	FBlueprintEditorUtils::AddMemberVariable(ClassBP, TEXT("Hidden"), DoublePin());
	FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(ClassBP, TEXT("Power"), false);
	FKismetEditorUtilities::CompileBlueprint(ClassBP, EBlueprintCompileOptions::SkipGarbageCollection);
	UObject* ClassDefaults = ClassBP->GeneratedClass->GetDefaultObject();
	if (double* Power = FindValue<double>(ClassDefaults, TEXT("Power")))
	{
		*Power = 3.0;
	}
	const FString Name = TEXT("DA_Merge_") + Suffix;
	UDataAsset* Asset = NewObject<UDataAsset>(NewTestPackage(Name), ClassBP->GeneratedClass, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
	TestEqual(TEXT("The data asset starts with the class default"), ReadDouble(Asset, TEXT("Power")), 3.0);

	const TArray<FRowResult> Results = ApplyRows(Asset, {
		MakeRow(Asset, TEXT("Power"), EMergeAction::Set, TEXT("5")),
		MakeRow(Asset, TEXT("Hidden"), EMergeAction::Set, TEXT("1")) });
	TestResult(*this, TEXT("Set on the data asset"), Results, 0, ERowResult::Applied);
	TestResult(*this, TEXT("A class-only variable isn't editable on the data asset"), Results, 1, ERowResult::Failed);
	TestEqual(TEXT("Power on the data asset"), ReadDouble(Asset, TEXT("Power")), 5.0);
	TestEqual(TEXT("The class default stays"), ReadDouble(ClassDefaults, TEXT("Power")), 3.0);
	TestEqual(TEXT("Hidden not written"), ReadDouble(Asset, TEXT("Hidden")), 0.0);
	TestTrue(TEXT("Package marked dirty"), Asset->GetPackage()->IsDirty());

	const TArray<FRowResult> Resets = ApplyRows(Asset, { MakeRow(Asset, TEXT("Power"), EMergeAction::Reset) });
	TestResult(*this, TEXT("Reset on the data asset"), Resets, 0, ERowResult::Applied);
	TestEqual(TEXT("Power back to the class default"), ReadDouble(Asset, TEXT("Power")), 3.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonBlueprintSyncAssetMergeApplyWidgetTest, "JsonBlueprintSync.AssetMerge.ApplyWidget", JsonBlueprintSync::AssetMergeTests::TestFlags)
bool FJsonBlueprintSyncAssetMergeApplyWidgetTest::RunTest(const FString& Parameters)
{
	using namespace JsonBlueprintSync::AssetMerge;
	using namespace JsonBlueprintSync::AssetMergeTests;

	// A Widget Blueprint with a canvas "Root" holding a text block "Title", built the way the Designer builds it.
	const FString Name = TEXT("WBP_Merge_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UWidgetBlueprint* Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), NewTestPackage(Name),
		FName(*Name), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Blueprint->WidgetTree->RootWidget = Root;
	UTextBlock* Title = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Title"));
	UCanvasPanelSlot* TitleSlot = Root->AddChildToCanvas(Title);
	Blueprint->OnVariableAdded(Root->GetFName());
	Blueprint->OnVariableAdded(Title->GetFName());
	TitleSlot->SetZOrder(3);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

	// Paths as the reader writes them: LayoutData.Offsets.Left is two structs deep.
	const TArray<FRowResult> Results = ApplyRows(Blueprint, {
		MakeRow(TitleSlot, TEXT("LayoutData.Offsets.Left"), EMergeAction::Set, TEXT("100")),
		MakeRow(TitleSlot, TEXT("ZOrder"), EMergeAction::Reset),
		MakeRow(Title, TEXT("RenderOpacity"), EMergeAction::Set, TEXT("0.25")) });
	TestResult(*this, TEXT("Set a member two structs deep"), Results, 0, ERowResult::Applied);
	TestResult(*this, TEXT("Reset a slot value"), Results, 1, ERowResult::Applied);
	TestResult(*this, TEXT("Set a widget value"), Results, 2, ERowResult::Applied);
	TestEqual(TEXT("Slot offset"), TitleSlot->GetOffsets().Left, 100.f);
	TestEqual(TEXT("Slot ZOrder back to the default"), TitleSlot->GetZOrder(), 0);
	TestEqual(TEXT("Widget opacity"), Title->GetRenderOpacity(), 0.25f);
	TestTrue(TEXT("Widget Blueprint marked modified"), Blueprint->GetPackage()->IsDirty());
	return true;
}

#endif
