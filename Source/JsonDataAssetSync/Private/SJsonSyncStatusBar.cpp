#include "SJsonSyncStatusBar.h"

#include "Editor.h"
#include "JsonDataAssetSyncSubsystem.h"
#include "Misc/PackageName.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "ToolMenus.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "JsonDataAssetSync"

namespace
{
	/** The tooltip lists this many assets, then "and N more". */
	constexpr int32 MaxTooltipAssets = 10;

	UJsonDataAssetSyncSubsystem* GetSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UJsonDataAssetSyncSubsystem>() : nullptr;
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
			UJsonDataAssetSyncSubsystem::OpenSyncTab();
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
	// Directly left of the unsaved assets entry: first in the engine's "SourceControl" section, whose one widget starts with the
	// unsaved assets. A section of our own placed before "SourceControl" could end up left of the Compile button, which the level
	// editor also places before "SourceControl".
	FToolMenuSection& Section = Menu->FindOrAddSection("SourceControl");
	FToolMenuEntry Entry = FToolMenuEntry::InitWidget("JsonSyncStatusBar", SNew(SJsonSyncStatusBar), FText::GetEmpty(), true, false);
	Entry.InsertPosition = FToolMenuInsert(NAME_None, EToolMenuInsertType::First);
	Section.AddEntry(Entry);
}

int32 SJsonSyncStatusBar::GetPendingNum()
{
	const UJsonDataAssetSyncSubsystem* Subsystem = GetSubsystem();
	return Subsystem ? Subsystem->GetPendingNum() : 0;
}

FText SJsonSyncStatusBar::GetText() const
{
	const int32 Num = GetPendingNum();
	return Num == 0
		? LOCTEXT("StatusSynced", "All JSON Synced")
		: FText::Format(LOCTEXT("StatusPending", "{0} JSON to Review"), FText::AsNumber(Num));
}

FText SJsonSyncStatusBar::GetTooltip() const
{
	const UJsonDataAssetSyncSubsystem* Subsystem = GetSubsystem();
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
