#include "SAssetMergeStatusBar.h"

#include "Editor.h"
#include "JsonBlueprintMergeSubsystem.h"
#include "Misc/PackageName.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "ToolMenus.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "JsonBlueprintSync"

namespace
{
	/** The tooltip lists this many merges, then "and N more". */
	constexpr int32 MaxTooltipMerges = 10;

	const UJsonBlueprintMergeSubsystem* GetMergeSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UJsonBlueprintMergeSubsystem>() : nullptr;
	}
}

void SAssetMergeStatusBar::Construct(const FArguments& InArgs)
{
	// Looks like the JSON Sync entry next to it.
	ChildSlot
	[
		SNew(SButton)
		.ContentPadding(FMargin(6.0f, 0.0f))
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.Visibility(this, &SAssetMergeStatusBar::GetEntryVisibility)
		.ToolTipText(this, &SAssetMergeStatusBar::GetTooltip)
		.OnClicked_Lambda([]()
		{
			UJsonBlueprintMergeSubsystem::OpenMergeTab();
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("SourceControl.Integrate"))
				.ColorAndOpacity(this, &SAssetMergeStatusBar::GetColor)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(5.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
				.Text(this, &SAssetMergeStatusBar::GetText)
				.ColorAndOpacity(this, &SAssetMergeStatusBar::GetColor)
			]
		]
	];
}

void SAssetMergeStatusBar::Register(void* Owner)
{
	FToolMenuOwnerScoped OwnerScoped(Owner);
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.StatusBar.ToolBar");
	if (!Menu)
	{
		return;
	}
	FToolMenuSection& Section = Menu->FindOrAddSection("SourceControl");
	FToolMenuEntry Entry = FToolMenuEntry::InitWidget("AssetMergeStatusBar", SNew(SAssetMergeStatusBar), FText::GetEmpty(), true, false);
	Entry.InsertPosition = FToolMenuInsert("JsonSyncStatusBar", EToolMenuInsertType::Before);
	Section.AddEntry(Entry);
}

EVisibility SAssetMergeStatusBar::GetEntryVisibility() const
{
	const UJsonBlueprintMergeSubsystem* Subsystem = GetMergeSubsystem();
	return Subsystem && Subsystem->GetEntries().Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SAssetMergeStatusBar::GetText() const
{
	const UJsonBlueprintMergeSubsystem* Subsystem = GetMergeSubsystem();
	const int32 Num = Subsystem ? Subsystem->GetToApplyNum() : 0;
	return Num == 0
		? LOCTEXT("MergeStatusDone", "Merges Done")
		: FText::Format(LOCTEXT("MergeStatusToApply", "{0} Merge(s) to Apply"), FText::AsNumber(Num));
}

FText SAssetMergeStatusBar::GetTooltip() const
{
	const UJsonBlueprintMergeSubsystem* Subsystem = GetMergeSubsystem();
	if (!Subsystem)
	{
		return FText::GetEmpty();
	}
	const TArray<UJsonBlueprintMergeSubsystem::FEntry>& Entries = Subsystem->GetEntries();
	FString List;
	for (int32 Index = 0; Index < FMath::Min(Entries.Num(), MaxTooltipMerges); ++Index)
	{
		List += FString::Printf(TEXT("\n  %s: %s"), *FPackageName::GetShortName(Entries[Index].File.Location.PackageName),
			JsonBlueprintSync::AssetMerge::LexToString(Subsystem->GetStatus(Entries[Index])));
	}
	if (Entries.Num() > MaxTooltipMerges)
	{
		List += FText::Format(LOCTEXT("MergeTooltipMore", "\n  and {0} more"), FText::AsNumber(Entries.Num() - MaxTooltipMerges)).ToString();
	}
	return FText::Format(LOCTEXT("MergeTooltip", "Asset merges saved by the merge tool:{0}\nDelete a merge in the tab once it is done. Click to open the Asset Merges tab."),
		FText::FromString(List));
}

FSlateColor SAssetMergeStatusBar::GetColor() const
{
	const UJsonBlueprintMergeSubsystem* Subsystem = GetMergeSubsystem();
	return Subsystem && Subsystem->GetToApplyNum() > 0 ? FStyleColors::Warning : FSlateColor::UseForeground();
}

#undef LOCTEXT_NAMESPACE
