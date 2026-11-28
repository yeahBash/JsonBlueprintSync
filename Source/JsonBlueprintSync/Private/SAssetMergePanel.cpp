#include "SAssetMergePanel.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "JsonBlueprintMergeSubsystem.h"
#include "JsonBlueprintSync.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "JsonBlueprintSync"

using namespace JsonBlueprintSync;
using namespace JsonBlueprintSync::AssetMerge;

namespace
{
	using FMergeEntry = UJsonBlueprintMergeSubsystem::FEntry;

	const FName MergeColumnAsset(TEXT("Asset"));
	const FName MergeColumnStatus(TEXT("Status"));
	const FName MergeColumnDelete(TEXT("Delete"));
	const FName MergeColumnUse(TEXT("Use"));
	const FName MergeColumnObject(TEXT("Object"));
	const FName MergeColumnPath(TEXT("Path"));
	const FName MergeColumnCurrent(TEXT("Current"));
	const FName MergeColumnMerged(TEXT("Merged"));
	const FName MergeColumnAction(TEXT("Action"));

	const FMergeEntry* FindMergeEntry(const UJsonBlueprintMergeSubsystem* Subsystem, const FString& PackageName)
	{
		return Subsystem ? Subsystem->FindEntry(PackageName) : nullptr;
	}

	/** The object path without the package's folder: "BP_A.Default__BP_A_C", "WBP_B.WBP_B:WidgetTree.Title". */
	FString ShortObjectName(const FString& Object)
	{
		int32 Slash = INDEX_NONE;
		const int32 Dot = Object.Find(TEXT("."), ESearchCase::CaseSensitive);
		const FString Package = Dot == INDEX_NONE ? Object : Object.Left(Dot);
		return Package.FindLastChar(TEXT('/'), Slash) ? Object.Mid(Slash + 1) : Object;
	}

	FText ActionText(EMergeAction Action)
	{
		switch (Action)
		{
		case EMergeAction::Set: return LOCTEXT("ActionSet", "Set");
		case EMergeAction::Add: return LOCTEXT("ActionAdd", "Add");
		case EMergeAction::Remove: return LOCTEXT("ActionRemove", "Remove");
		case EMergeAction::Reset: return LOCTEXT("ActionReset", "Reset");
		default: return LOCTEXT("ActionNotApplicable", "Not applicable");
		}
	}

	FText ActionTooltip(EMergeAction Action)
	{
		switch (Action)
		{
		case EMergeAction::Set: return LOCTEXT("ActionSetTip", "Set the value to the merged one.");
		case EMergeAction::Add: return LOCTEXT("ActionAddTip", "Add the map entry or set element.");
		case EMergeAction::Remove: return LOCTEXT("ActionRemoveTip", "Remove the map entry or set element.");
		case EMergeAction::Reset: return LOCTEXT("ActionResetTip", "The merged value is inherited: take the parent's (or the class default) value.");
		default: return FText::GetEmpty();
		}
	}

	FText StatusText(EMergeStatus Status)
	{
		switch (Status)
		{
		case EMergeStatus::Ready: return LOCTEXT("MergeReady", "Ready");
		case EMergeStatus::Done: return LOCTEXT("MergeDone", "Done");
		case EMergeStatus::HasConflictMarkers: return LOCTEXT("MergeConflictMarkers", "Has conflict markers");
		case EMergeStatus::HasErrors: return LOCTEXT("MergeErrors", "Has errors");
		case EMergeStatus::FormatDiffers: return LOCTEXT("MergeFormat", "Format differs");
		case EMergeStatus::AssetNotFound: return LOCTEXT("MergeNoAsset", "Asset not found");
		case EMergeStatus::ReaderNotSetUp: return LOCTEXT("MergeNoReader", "Reader not set up");
		case EMergeStatus::ReaderFailed: return LOCTEXT("MergeReaderFailed", "Reader failed");
		case EMergeStatus::UnsavedChanges: return LOCTEXT("MergeUnsaved", "Asset has unsaved changes");
		}
		return FText::GetEmpty();
	}

	class SFileRow : public SMultiColumnTableRow<TSharedPtr<SAssetMergePanel::FFileItem>>
	{
	public:
		SLATE_BEGIN_ARGS(SFileRow) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner, TSharedPtr<SAssetMergePanel::FFileItem> InItem,
			TWeakPtr<SAssetMergePanel> InPanel)
		{
			Item = InItem;
			Panel = InPanel;
			SMultiColumnTableRow<TSharedPtr<SAssetMergePanel::FFileItem>>::Construct(FSuperRowType::FArguments(), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			const TSharedPtr<SAssetMergePanel::FFileItem> Captured = Item;
			const TWeakPtr<SAssetMergePanel> WeakPanel = Panel;
			if (Column == MergeColumnDelete)
			{
				return SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ContentPadding(FMargin(2.0f))
						.ToolTipText(LOCTEXT("DeleteMergeTip", "Delete the merge file: the merge is finished, or you want to drop it. Asks first."))
						.OnClicked_Lambda([Captured, WeakPanel]()
						{
							if (const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin())
							{
								Pinned->DeleteMergeFile(Captured->PackageName);
							}
							return FReply::Handled();
						})
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.Delete"))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
					];
			}
			if (Column == MergeColumnAsset)
			{
				return SNew(SBox)
					.Padding(FMargin(4.0f, 2.0f))
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FPackageName::GetShortName(Item->PackageName)))
						.ToolTipText(FText::FromString(Item->PackageName))
					];
			}
			auto GetStatus = [Captured, WeakPanel]() -> TOptional<EMergeStatus>
			{
				const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin();
				const UJsonBlueprintMergeSubsystem* Subsystem = Pinned ? Pinned->GetSubsystem() : nullptr;
				const FMergeEntry* Entry = FindMergeEntry(Subsystem, Captured->PackageName);
				return Entry ? TOptional<EMergeStatus>(Subsystem->GetStatus(*Entry)) : TOptional<EMergeStatus>();
			};
			return SNew(SBox)
				.Padding(FMargin(4.0f, 2.0f))
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([GetStatus]()
					{
						const TOptional<EMergeStatus> Status = GetStatus();
						return Status ? StatusText(*Status) : FText::GetEmpty();
					})
					.ColorAndOpacity_Lambda([GetStatus]()
					{
						const TOptional<EMergeStatus> Status = GetStatus();
						if (!Status || *Status == EMergeStatus::Ready)
						{
							return FSlateColor::UseForeground();
						}
						return *Status == EMergeStatus::Done ? FSlateColor::UseSubduedForeground() : FSlateColor(FStyleColors::Warning);
					})
					.ToolTipText_Lambda([Captured, WeakPanel]()
					{
						const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin();
						const UJsonBlueprintMergeSubsystem* Subsystem = Pinned ? Pinned->GetSubsystem() : nullptr;
						const FMergeEntry* Entry = FindMergeEntry(Subsystem, Captured->PackageName);
						const FString Message = Entry ? Subsystem->GetStatusMessage(*Entry) : FString();
						return FText::FromString(Message.IsEmpty() ? Captured->PackageName : Message);
					})
				];
		}

	private:
		TSharedPtr<SAssetMergePanel::FFileItem> Item;
		TWeakPtr<SAssetMergePanel> Panel;
	};

	class SMergeRowRow : public SMultiColumnTableRow<TSharedPtr<SAssetMergePanel::FRowItem>>
	{
	public:
		SLATE_BEGIN_ARGS(SMergeRowRow) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner, TSharedPtr<SAssetMergePanel::FRowItem> InItem,
			TWeakPtr<SAssetMergePanel> InPanel)
		{
			Item = InItem;
			Panel = InPanel;
			SMultiColumnTableRow<TSharedPtr<SAssetMergePanel::FRowItem>>::Construct(FSuperRowType::FArguments(), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			const TSharedPtr<SAssetMergePanel::FRowItem> Captured = Item;
			const TWeakPtr<SAssetMergePanel> WeakPanel = Panel;
			const FMergeRow& Row = Item->Row;
			const bool bApplicable = Row.Action != EMergeAction::NotApplicable;

			if (Column == MergeColumnUse)
			{
				return SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([Captured, WeakPanel]()
						{
							const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin();
							return Pinned && Pinned->IsTicked(*Captured) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
						})
						.IsEnabled_Lambda([Captured, WeakPanel]()
						{
							const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin();
							return Pinned && Pinned->CanTick(*Captured);
						})
						.OnCheckStateChanged_Lambda([Captured, WeakPanel](ECheckBoxState State)
						{
							if (const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin())
							{
								Pinned->SetTicked(*Captured, State == ECheckBoxState::Checked);
							}
						})
						.ToolTipText_Lambda([Captured, WeakPanel]()
						{
							const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin();
							return Pinned ? Pinned->GetRowTooltip(*Captured) : FText::GetEmpty();
						})
					];
			}
			if (Column == MergeColumnAction)
			{
				const FText Text = ActionText(Row.Action);
				const FText Tooltip = bApplicable ? ActionTooltip(Row.Action) : FText::FromString(Row.Reason);
				return SNew(SBox)
					.Padding(FMargin(4.0f, 2.0f))
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([Captured, WeakPanel, Text]()
						{
							const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin();
							if (Pinned && Pinned->IsAppliedNotSaved(*Captured))
							{
								return LOCTEXT("RowAppliedNotSaved", "Applied, not saved");
							}
							if (Pinned && Pinned->FindFailure(*Captured))
							{
								return FText::Format(LOCTEXT("RowFailed", "{0}: failed"), Text);
							}
							return Text;
						})
						.ToolTipText_Lambda([Captured, WeakPanel, Tooltip]()
						{
							const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin();
							const FString* Failure = Pinned ? Pinned->FindFailure(*Captured) : nullptr;
							if (Pinned && Pinned->IsAppliedNotSaved(*Captured))
							{
								return LOCTEXT("RowAppliedTip", "Applied in this session. Save the asset to finish: the list is worked out from the saved file, so the row stays until then. Ctrl+Z reverts the apply.");
							}
							return Failure ? FText::Format(LOCTEXT("RowFailedTip", "The last Apply couldn't apply this row: {0}"), FText::FromString(*Failure)) : Tooltip;
						})
						.ColorAndOpacity_Lambda([Captured, WeakPanel, bApplicable]()
						{
							const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin();
							if (Pinned && Pinned->IsAppliedNotSaved(*Captured))
							{
								return FSlateColor(FStyleColors::AccentGreen);
							}
							if (Pinned && Pinned->FindFailure(*Captured))
							{
								return FSlateColor(FStyleColors::Warning);
							}
							return bApplicable ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();
						})
					];
			}

			FText Text;
			FText Tooltip;
			bool bInherited = false;
			if (Column == MergeColumnObject)
			{
				Text = FText::FromString(ShortObjectName(Row.Object));
				Tooltip = FText::FromString(Row.Object);
			}
			else if (Column == MergeColumnPath)
			{
				Text = Row.Path.IsEmpty() ? LOCTEXT("WholeObject", "(object)") : FText::FromString(Row.Path);
				Tooltip = Text;
			}
			else
			{
				const TOptional<FString>& Value = Column == MergeColumnCurrent ? Row.Current : Row.Merged;
				bInherited = !Value.IsSet();
				Text = bInherited ? LOCTEXT("InheritedValue", "(inherited)") : FText::FromString(*Value);
				Tooltip = bInherited ? LOCTEXT("InheritedValueTip", "No line in the text: the value is the parent's, or the class default.") : Text;
			}
			return SNew(SBox)
				.Padding(FMargin(4.0f, 2.0f))
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Text)
					.ToolTipText(Tooltip)
					.ColorAndOpacity_Lambda([Captured, WeakPanel, bApplicable, bInherited]()
					{
						// Rows that are not applicable or already applied are greyed: nothing to do for them here.
						const TSharedPtr<SAssetMergePanel> Pinned = WeakPanel.Pin();
						const bool bDone = Pinned && Pinned->IsAppliedNotSaved(*Captured);
						return !bApplicable || bDone || bInherited ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground();
					})
				];
		}

	private:
		TSharedPtr<SAssetMergePanel::FRowItem> Item;
		TWeakPtr<SAssetMergePanel> Panel;
	};
}

void SAssetMergePanel::Construct(const FArguments& InArgs)
{
	Subsystem = GEditor ? GEditor->GetEditorSubsystem<UJsonBlueprintMergeSubsystem>() : nullptr;
	if (Subsystem.IsValid())
	{
		ChangedHandle = Subsystem->OnChanged.AddSP(this, &SAssetMergePanel::Refresh);
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f, 8.0f, 8.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SAssetMergePanel::GetSummaryText)
				.AutoWrapText(true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("MergeRefresh", "Refresh"))
				.ToolTipText(LOCTEXT("MergeRefreshTip", "Check the merge files again, e.g. after a merge in TortoiseGit. Runs the reader on each asset."))
				.OnClicked(this, &SAssetMergePanel::OnRefreshClicked)
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(8.0f, 4.0f, 8.0f, 8.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)

			+ SSplitter::Slot()
			.Value(0.3f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				[
					SAssignNew(FileList, SListView<TSharedPtr<FFileItem>>)
					.ListItemsSource(&FileItems)
					.SelectionMode(ESelectionMode::Multi)
					.OnGenerateRow(this, &SAssetMergePanel::GenerateFileRow)
					.OnSelectionChanged(this, &SAssetMergePanel::OnFileSelectionChanged)
					.OnMouseButtonDoubleClick(this, &SAssetMergePanel::OnFileDoubleClicked)
					.HeaderRow
					(
						SNew(SHeaderRow)
						+ SHeaderRow::Column(MergeColumnAsset).DefaultLabel(LOCTEXT("ColumnMergeAsset", "Asset")).FillWidth(0.55f)
						+ SHeaderRow::Column(MergeColumnStatus).DefaultLabel(LOCTEXT("ColumnMergeStatus", "Status")).FillWidth(0.45f)
						+ SHeaderRow::Column(MergeColumnDelete).DefaultLabel(FText::GetEmpty()).FixedWidth(28.0f)
					)
				]
			]

			+ SSplitter::Slot()
			.Value(0.7f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(STextBlock)
					.Text(this, &SAssetMergePanel::GetDetailText)
					.AutoWrapText(true)
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
					[
						SAssignNew(RowList, SListView<TSharedPtr<FRowItem>>)
						.ListItemsSource(&RowItems)
						.SelectionMode(ESelectionMode::Multi)
						.OnGenerateRow(this, &SAssetMergePanel::GenerateRowRow)
						.HeaderRow
						(
							SNew(SHeaderRow)
							+ SHeaderRow::Column(MergeColumnUse).DefaultLabel(LOCTEXT("ColumnMergeUse", "Use")).FixedWidth(40.0f)
							+ SHeaderRow::Column(MergeColumnObject).DefaultLabel(LOCTEXT("ColumnMergeObject", "Object")).FillWidth(0.2f)
							+ SHeaderRow::Column(MergeColumnPath).DefaultLabel(LOCTEXT("ColumnMergePath", "Path")).FillWidth(0.22f)
							+ SHeaderRow::Column(MergeColumnCurrent).DefaultLabel(LOCTEXT("ColumnMergeCurrent", "Current value")).FillWidth(0.22f)
							+ SHeaderRow::Column(MergeColumnMerged).DefaultLabel(LOCTEXT("ColumnMergeMerged", "Merged value")).FillWidth(0.22f)
							+ SHeaderRow::Column(MergeColumnAction).DefaultLabel(LOCTEXT("ColumnMergeAction", "Action")).FillWidth(0.14f)
						)
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(4.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(this, &SAssetMergePanel::GetHintText)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("MergeBrowse", "Show in Content Browser"))
						.IsEnabled(this, &SAssetMergePanel::HasSelection)
						.OnClicked(this, &SAssetMergePanel::OnBrowseClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("MergeApply", "Apply"))
						.ToolTipText(LOCTEXT("MergeApplyTip", "Apply the ticked rows of the selected merges, in one undo step. The assets are not saved."))
						.IsEnabled(this, &SAssetMergePanel::CanApply)
						.OnClicked(this, &SAssetMergePanel::OnApplyClicked, false)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.Text(LOCTEXT("MergeApplySave", "Apply and Save"))
						.ToolTipText(LOCTEXT("MergeApplySaveTip", "Apply, then compile the changed Blueprints and save the changed assets. The rows are then worked out again from the saved files."))
						.IsEnabled(this, &SAssetMergePanel::CanApply)
						.OnClicked(this, &SAssetMergePanel::OnApplyClicked, true)
					]
				]
			]
		]
	];

	Refresh();
}

SAssetMergePanel::~SAssetMergePanel()
{
	if (UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get())
	{
		Owner->OnChanged.Remove(ChangedHandle);
	}
}

UJsonBlueprintMergeSubsystem* SAssetMergePanel::GetSubsystem() const
{
	return Subsystem.Get();
}

void SAssetMergePanel::Refresh()
{
	TGuardValue<bool> Guard(bRefreshing, true);
	const UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get();

	FileItems.Reset();
	TArray<TSharedPtr<FFileItem>> ToSelect;
	TSet<FString> Listed;
	if (Owner)
	{
		for (const FMergeEntry& Entry : Owner->GetEntries())
		{
			const TSharedPtr<FFileItem> Item = MakeShared<FFileItem>();
			Item->PackageName = Entry.File.Location.PackageName;
			FileItems.Add(Item);
			Listed.Add(Item->PackageName);
			if (SelectedPackages.Contains(Item->PackageName))
			{
				ToSelect.Add(Item);
			}
		}
	}
	for (auto It = Unticked.CreateIterator(); It; ++It)
	{
		if (!Listed.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	if (ToSelect.IsEmpty() && FileItems.Num() > 0)
	{
		ToSelect.Add(FileItems[0]);
	}
	SelectedPackages.Reset();
	for (const TSharedPtr<FFileItem>& Item : ToSelect)
	{
		SelectedPackages.Add(Item->PackageName);
	}
	FileList->RequestListRefresh();
	FileList->ClearSelection();
	FileList->SetItemSelection(ToSelect, true, ESelectInfo::Direct);
	RefreshRows();
}

void SAssetMergePanel::RefreshRows()
{
	RowItems.Reset();
	const UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get();
	for (const FString& PackageName : SelectedPackages)
	{
		const FMergeEntry* Entry = FindMergeEntry(Owner, PackageName);
		if (!Entry)
		{
			continue;
		}
		for (const FMergeRow& Row : Entry->File.Rows)
		{
			const TSharedPtr<FRowItem> Item = MakeShared<FRowItem>();
			Item->PackageName = PackageName;
			Item->Row = Row;
			Item->Key = UJsonBlueprintMergeSubsystem::GetRowKey(Row);
			RowItems.Add(Item);
		}
	}
	RowList->ClearSelection();
	RowList->RequestListRefresh();
}

TSharedRef<ITableRow> SAssetMergePanel::GenerateFileRow(TSharedPtr<FFileItem> Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SFileRow, Owner, Item, StaticCastSharedRef<SAssetMergePanel>(AsShared()));
}

TSharedRef<ITableRow> SAssetMergePanel::GenerateRowRow(TSharedPtr<FRowItem> Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SMergeRowRow, Owner, Item, StaticCastSharedRef<SAssetMergePanel>(AsShared()));
}

void SAssetMergePanel::OnFileSelectionChanged(TSharedPtr<FFileItem> Item, ESelectInfo::Type SelectInfo)
{
	if (bRefreshing)
	{
		return;
	}
	// In list order, so the rows come in the same order as the files.
	SelectedPackages.Reset();
	for (const TSharedPtr<FFileItem>& Each : FileItems)
	{
		if (FileList->IsItemSelected(Each))
		{
			SelectedPackages.Add(Each->PackageName);
		}
	}
	RefreshRows();
}

void SAssetMergePanel::OnFileDoubleClicked(TSharedPtr<FFileItem> Item)
{
	if (!Item || !GEditor || !FPackageName::DoesPackageExist(Item->PackageName))
	{
		return;
	}
	if (UObject* Asset = LoadAsset(Item->PackageName))
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
	}
}

bool SAssetMergePanel::CanTick(const FRowItem& Item) const
{
	const UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get();
	const FMergeEntry* Entry = FindMergeEntry(Owner, Item.PackageName);
	return Entry && Item.Row.Action != EMergeAction::NotApplicable && Owner->CanApply(*Entry) && !Owner->IsAppliedNotSaved(*Entry, Item.Key);
}

bool SAssetMergePanel::IsTicked(const FRowItem& Item) const
{
	const TSet<FString>* PackageUnticked = Unticked.Find(Item.PackageName);
	return CanTick(Item) && !(PackageUnticked && PackageUnticked->Contains(Item.Key));
}

void SAssetMergePanel::SetTicked(const FRowItem& Item, bool bTicked)
{
	if (bTicked)
	{
		if (TSet<FString>* PackageUnticked = Unticked.Find(Item.PackageName))
		{
			PackageUnticked->Remove(Item.Key);
		}
	}
	else
	{
		Unticked.FindOrAdd(Item.PackageName).Add(Item.Key);
	}
}

bool SAssetMergePanel::IsAppliedNotSaved(const FRowItem& Item) const
{
	const UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get();
	const FMergeEntry* Entry = FindMergeEntry(Owner, Item.PackageName);
	return Entry && Owner->IsAppliedNotSaved(*Entry, Item.Key);
}

const FString* SAssetMergePanel::FindFailure(const FRowItem& Item) const
{
	const FMergeEntry* Entry = FindMergeEntry(Subsystem.Get(), Item.PackageName);
	return Entry ? Entry->Failures.Find(Item.Key) : nullptr;
}

FText SAssetMergePanel::GetRowTooltip(const FRowItem& Item) const
{
	const UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get();
	const FMergeEntry* Entry = FindMergeEntry(Owner, Item.PackageName);
	if (!Entry)
	{
		return FText::GetEmpty();
	}
	if (Item.Row.Action == EMergeAction::NotApplicable)
	{
		return FText::Format(LOCTEXT("UseNotApplicableTip", "Can't be applied: {0}"), FText::FromString(Item.Row.Reason));
	}
	if (Owner->IsAppliedNotSaved(*Entry, Item.Key))
	{
		return LOCTEXT("UseAppliedTip", "Applied, not saved yet.");
	}
	if (!Owner->CanApply(*Entry))
	{
		return FText::Format(LOCTEXT("UseBlockedTip", "The merge can't be applied now: {0}."), StatusText(Owner->GetStatus(*Entry)));
	}
	return LOCTEXT("UseTip", "Apply this row.");
}

void SAssetMergePanel::DeleteMergeFile(const FString& PackageName)
{
	UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get();
	const FMergeEntry* Entry = FindMergeEntry(Owner, PackageName);
	if (!Entry)
	{
		return;
	}
	const FText Message = FText::Format(LOCTEXT("DeleteMergeConfirm", "Are you sure you want to delete the file?\n\n{0}"),
		FText::FromString(Entry->File.Location.MergedFile));
	if (FMessageDialog::Open(EAppMsgType::YesNo, Message, LOCTEXT("DeleteMergeTitle", "Delete Merge")) == EAppReturnType::Yes)
	{
		// Broadcasts OnChanged, which refreshes this panel.
		Owner->DeleteMergeFile(PackageName);
	}
}

FText SAssetMergePanel::GetSummaryText() const
{
	const UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get();
	if (!Owner)
	{
		return LOCTEXT("MergeNoSubsystem", "Asset merges are not available.");
	}
	if (FileItems.Num() == 0)
	{
		return FText::Format(LOCTEXT("NoMerges", "No asset merges. The merge tool saves them to {0}."), FText::FromString(GetMergeDir()));
	}
	return FText::Format(LOCTEXT("MergesSummary", "{0} merge file(s), {1} to apply. A merge stays here until you delete it with its delete button."),
		FText::AsNumber(FileItems.Num()), FText::AsNumber(Owner->GetToApplyNum()));
}

FText SAssetMergePanel::GetDetailText() const
{
	const UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get();
	if (SelectedPackages.Num() > 1)
	{
		return FText::Format(LOCTEXT("MergeDetailMany", "{0} merges selected. Apply applies the ticked rows of all of them in one undo step, parents before children."),
			FText::AsNumber(SelectedPackages.Num()));
	}
	const FMergeEntry* Entry = SelectedPackages.Num() == 1 ? FindMergeEntry(Owner, SelectedPackages[0]) : nullptr;
	if (!Entry)
	{
		return FText::GetEmpty();
	}
	const FText Package = FText::FromString(Entry->File.Location.PackageName);
	const EMergeStatus Status = Owner->GetStatus(*Entry);
	switch (Status)
	{
	case EMergeStatus::Ready:
		return FText::Format(LOCTEXT("MergeDetailReady", "{0}\nWhat to change in the saved asset to get merged.txt. Apply sets the ticked rows (Ctrl+Z reverts) and leaves the asset unsaved; Apply and Save saves it, and the rows are worked out again from the saved file."),
			Package);
	case EMergeStatus::Done:
		return FText::Format(LOCTEXT("MergeDetailDone", "{0}\nThe saved asset has the merged values. Delete the merge file to finish the merge."), Package);
	default:
		return FText::Format(LOCTEXT("MergeDetailProblem", "{0}\n{1}: {2}"), Package, StatusText(Status), FText::FromString(Owner->GetStatusMessage(*Entry)));
	}
}

FText SAssetMergePanel::GetHintText() const
{
	int32 NotApplicable = 0;
	int32 Failed = 0;
	for (const TSharedPtr<FRowItem>& Item : RowItems)
	{
		NotApplicable += Item->Row.Action == EMergeAction::NotApplicable ? 1 : 0;
		Failed += FindFailure(*Item) ? 1 : 0;
	}
	TArray<FText> Lines;
	if (Failed > 0)
	{
		Lines.Add(FText::Format(LOCTEXT("MergeHintFailed", "{0} row(s) failed at the last Apply; hover their action for the reason."), FText::AsNumber(Failed)));
	}
	if (NotApplicable > 0)
	{
		Lines.Add(FText::Format(LOCTEXT("MergeHintNotApplicable", "{0} row(s) can't be applied here: take the other side's .uasset and merge again, or redo the change by hand."),
			FText::AsNumber(NotApplicable)));
	}
	return FText::Join(FText::FromString(TEXT("\n")), Lines);
}

bool SAssetMergePanel::HasSelection() const
{
	return Subsystem.IsValid() && SelectedPackages.Num() > 0;
}

bool SAssetMergePanel::CanApply() const
{
	return RowItems.ContainsByPredicate([this](const TSharedPtr<FRowItem>& Item) { return IsTicked(*Item); });
}

FReply SAssetMergePanel::OnRefreshClicked()
{
	if (UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get())
	{
		// Broadcasts OnChanged, which refreshes this panel.
		Owner->Refresh(false);
	}
	return FReply::Handled();
}

FReply SAssetMergePanel::OnApplyClicked(bool bSave)
{
	UJsonBlueprintMergeSubsystem* Owner = Subsystem.Get();
	if (!Owner || !CanApply())
	{
		return FReply::Handled();
	}
	TArray<UJsonBlueprintMergeSubsystem::FApplyRequest> Requests;
	for (const FString& PackageName : SelectedPackages)
	{
		TArray<FString> Keys;
		for (const TSharedPtr<FRowItem>& Item : RowItems)
		{
			if (Item->PackageName == PackageName && IsTicked(*Item))
			{
				Keys.Add(Item->Key);
			}
		}
		if (Keys.Num() > 0)
		{
			Requests.Add({ PackageName, MoveTemp(Keys) });
		}
	}
	// Broadcasts OnChanged, which refreshes this panel.
	Owner->Apply(Requests, bSave);
	return FReply::Handled();
}

FReply SAssetMergePanel::OnBrowseClicked()
{
	TArray<FAssetData> Assets;
	for (const FString& PackageName : SelectedPackages)
	{
		const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		const FAssetData Asset = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		if (Asset.IsValid())
		{
			Assets.Add(Asset);
		}
	}
	if (Assets.Num() > 0 && GEditor)
	{
		GEditor->SyncBrowserToObjects(Assets);
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
