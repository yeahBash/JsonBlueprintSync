#include "SJsonSyncStatusBar.h"

#include "Editor.h"
#include "JsonBlueprintSyncSubsystem.h"
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
	/** The tooltip lists this many assets, then "and N more". */
	constexpr int32 MaxTooltipAssets = 10;

	UJsonBlueprintSyncSubsystem* GetSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UJsonBlueprintSyncSubsystem>() : nullptr;
	}
}

void SJsonSyncStatusBar::Construct(const FArguments& InArgs)
{
	// Looks like the unsaved assets entry next to it: a flat button with an icon and a label.
	ChildSlot
	[
		SNew(SButton)
		.ContentPadding(FMargin(6.0f, 0.0f))
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.ToolTipText(this, &SJsonSyncStatusBar::GetTooltip)
		.OnClicked_Lambda([]()
		{
			UJsonBlueprintSyncSubsystem::OpenSyncTab();
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Merge"))
				.ColorAndOpacity(this, &SJsonSyncStatusBar::GetColor)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(5.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
				.Text(this, &SJsonSyncStatusBar::GetText)
				.ColorAndOpacity(this, &SJsonSyncStatusBar::GetColor)
			]
		]
	];
}

void SJsonSyncStatusBar::Register(void* Owner)
{
	FToolMenuOwnerScoped OwnerScoped(Owner);
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.StatusBar.ToolBar");
	if (!Menu)
	{
		return;
	}
	// Just left of the unsaved assets and revision control entries.
	FToolMenuSection& Section = Menu->AddSection("JsonBlueprintSync", FText::GetEmpty(), FToolMenuInsert("SourceControl", EToolMenuInsertType::Before));
	Section.AddEntry(FToolMenuEntry::InitWidget("JsonSyncStatusBar", SNew(SJsonSyncStatusBar), FText::GetEmpty(), true, false));
}

int32 SJsonSyncStatusBar::GetPendingNum()
{
	const UJsonBlueprintSyncSubsystem* Subsystem = GetSubsystem();
	return Subsystem ? Subsystem->GetPendingNum() : 0;
}

FText SJsonSyncStatusBar::GetText() const
{
	const int32 Num = GetPendingNum();
	return Num == 0
		? LOCTEXT("StatusSynced", "JSON Synced")
		: FText::Format(LOCTEXT("StatusPending", "{0} JSON to Review"), FText::AsNumber(Num));
}

FText SJsonSyncStatusBar::GetTooltip() const
{
	const UJsonBlueprintSyncSubsystem* Subsystem = GetSubsystem();
	const TArray<FString> Packages = Subsystem ? Subsystem->GetPendingPackages() : TArray<FString>();
	if (Packages.Num() == 0)
	{
		return LOCTEXT("TooltipSynced", "JSON Sync: no asset has JSON changes to review.\nClick to open the JSON Sync tab.");
	}
	FString List;
	for (int32 Index = 0; Index < FMath::Min(Packages.Num(), MaxTooltipAssets); ++Index)
	{
		List += TEXT("\n  ") + FPackageName::GetShortName(Packages[Index]);
	}
	if (Packages.Num() > MaxTooltipAssets)
	{
		List += FText::Format(LOCTEXT("TooltipMore", "\n  and {0} more"), FText::AsNumber(Packages.Num() - MaxTooltipAssets)).ToString();
	}
	return FText::Format(LOCTEXT("TooltipPending", "JSON Sync: these assets have JSON changes to review.{0}\nClick to open the JSON Sync tab."),
		FText::FromString(List));
}

FSlateColor SJsonSyncStatusBar::GetColor() const
{
	return GetPendingNum() == 0 ? FSlateColor::UseForeground() : FStyleColors::Warning;
}

#undef LOCTEXT_NAMESPACE
