#include "SJsonSyncPanel.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "JsonBlueprintSyncSubsystem.h"
#include "Misc/PackageName.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Subsystems/AssetEditorSubsystem.h"
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

namespace
{
	const FName ColumnAsset(TEXT("Asset"));
	const FName ColumnKind(TEXT("Kind"));
	const FName ColumnStatus(TEXT("Status"));
	const FName ColumnTake(TEXT("Take"));
	const FName ColumnKey(TEXT("Key"));
	const FName ColumnJson(TEXT("Json"));
	const FName ColumnValue(TEXT("Value"));
	const FName ColumnChange(TEXT("Change"));

	FText ValueText(const TOptional<FString>& Value)
	{
		return Value ? FText::FromString(*Value) : LOCTEXT("DefaultValue", "(default)");
	}

	FText ChangeText(EKeyChange Change)
	{
		switch (Change)
		{
		case EKeyChange::Json: return LOCTEXT("ChangedJson", "JSON");
		case EKeyChange::Asset: return LOCTEXT("ChangedAsset", "Asset");
		default: return LOCTEXT("ChangedBoth", "Conflict");
		}
	}

	FText ChangeTooltip(EKeyChange Change)
	{
		switch (Change)
		{
		case EKeyChange::Json: return LOCTEXT("ChangedJsonTip", "Changed in the JSON; the asset still has the old value. Ticked: Apply JSON takes the JSON value.");
		case EKeyChange::Asset: return LOCTEXT("ChangedAssetTip", "Changed in the asset; the JSON still has the old value. Unticked: the asset keeps its value and the next save writes it to the JSON.");
		default: return LOCTEXT("ChangedBothTip", "Changed on both sides, or it is unknown which side changed it. Unticked: tick it to take the JSON value.");
		}
	}

	class SAssetRow : public SMultiColumnTableRow<TSharedPtr<SJsonSyncPanel::FAssetItem>>
	{
	public:
		SLATE_BEGIN_ARGS(SAssetRow) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner, TSharedPtr<SJsonSyncPanel::FAssetItem> InItem)
		{
			Item = InItem;
			SMultiColumnTableRow<TSharedPtr<SJsonSyncPanel::FAssetItem>>::Construct(FSuperRowType::FArguments(), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			FText Text;
			FSlateColor Color = FSlateColor::UseForeground();
			if (Column == ColumnAsset)
			{
				Text = FText::FromString(FPackageName::GetShortName(Item->PackageName));
			}
			else if (Column == ColumnKind)
			{
				Text = FText::FromString(LexToString(Item->Kind));
				Color = FSlateColor::UseSubduedForeground();
			}
			else
			{
				Text = Item->Status;
				if (Item->bProblem)
				{
					Color = FStyleColors::Warning;
				}
			}
			return SNew(SBox)
				.Padding(FMargin(4.0f, 2.0f))
				[
					SNew(STextBlock)
					.Text(Text)
					.ColorAndOpacity(Color)
					.ToolTipText(FText::FromString(Item->PackageName))
				];
		}

	private:
		TSharedPtr<SJsonSyncPanel::FAssetItem> Item;
	};

	class SKeyRow : public SMultiColumnTableRow<TSharedPtr<SJsonSyncPanel::FKeyItem>>
	{
	public:
		SLATE_BEGIN_ARGS(SKeyRow) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner, TSharedPtr<SJsonSyncPanel::FKeyItem> InItem,
			TFunction<void(const FString&, bool)> InOnTick)
		{
			Item = InItem;
			OnTick = MoveTemp(InOnTick);
			SMultiColumnTableRow<TSharedPtr<SJsonSyncPanel::FKeyItem>>::Construct(FSuperRowType::FArguments(), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			const FKeyDiff& Diff = Item->Diff;
			if (Column == ColumnTake)
			{
				return SNew(SBox)
					.HAlign(HAlign_Center)
					[
						SNew(SCheckBox)
						.ToolTipText(LOCTEXT("TakeTip", "Ticked: Apply JSON sets this key to the JSON value. Unticked: the asset keeps its value."))
						.IsChecked_Lambda([Item = Item]() { return Item->bTake ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
						{
							Item->bTake = State == ECheckBoxState::Checked;
							if (OnTick)
							{
								OnTick(Item->Diff.Key, Item->bTake);
							}
						})
					];
			}

			FText Text;
			FText Tooltip;
			FSlateColor Color = FSlateColor::UseForeground();
			if (Column == ColumnKey)
			{
				Text = FText::FromString(Diff.Key);
				Tooltip = Text;
			}
			else if (Column == ColumnJson)
			{
				Text = ValueText(Diff.JsonValue);
				Tooltip = Text;
				Color = Diff.JsonValue ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();
			}
			else if (Column == ColumnValue)
			{
				Text = ValueText(Diff.AssetValue);
				Tooltip = Text;
				Color = Diff.AssetValue ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();
			}
			else
			{
				Text = ChangeText(Diff.Change);
				Tooltip = ChangeTooltip(Diff.Change);
				if (Diff.Change == EKeyChange::Both)
				{
					Color = FStyleColors::Warning;
				}
			}
			return SNew(SBox)
				.Padding(FMargin(4.0f, 2.0f))
				[
					SNew(STextBlock)
					.Text(Text)
					.ToolTipText(Tooltip)
					.ColorAndOpacity(Color)
				];
		}

	private:
		TSharedPtr<SJsonSyncPanel::FKeyItem> Item;
		TFunction<void(const FString&, bool)> OnTick;
	};
}

void SJsonSyncPanel::Construct(const FArguments& InArgs)
{
	Subsystem = GetSubsystem();
	if (Subsystem.IsValid())
	{
		PendingChangedHandle = Subsystem->OnPendingChanged.AddSP(this, &SJsonSyncPanel::Refresh);
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
				.Text(this, &SJsonSyncPanel::GetSummaryText)
				.AutoWrapText(true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Refresh", "Refresh"))
				.ToolTipText(LOCTEXT("RefreshTip", "Check the JSON files again, e.g. after a git pull (same as the console command JsonBlueprintSync.Sync)"))
				.OnClicked(this, &SJsonSyncPanel::OnRefreshClicked)
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
					SAssignNew(AssetList, SListView<TSharedPtr<FAssetItem>>)
					.ListItemsSource(&AssetItems)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SJsonSyncPanel::GenerateAssetRow)
					.OnSelectionChanged(this, &SJsonSyncPanel::OnAssetSelected)
					.OnMouseButtonDoubleClick(this, &SJsonSyncPanel::OnAssetDoubleClicked)
					.HeaderRow
					(
						SNew(SHeaderRow)
						+ SHeaderRow::Column(ColumnAsset).DefaultLabel(LOCTEXT("ColumnAsset", "Asset")).FillWidth(0.45f)
						+ SHeaderRow::Column(ColumnKind).DefaultLabel(LOCTEXT("ColumnKind", "Type")).FillWidth(0.25f)
						+ SHeaderRow::Column(ColumnStatus).DefaultLabel(LOCTEXT("ColumnStatus", "Status")).FillWidth(0.3f)
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
					.Text(this, &SJsonSyncPanel::GetDetailText)
					.AutoWrapText(true)
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
					[
						SAssignNew(KeyList, SListView<TSharedPtr<FKeyItem>>)
						.ListItemsSource(&KeyItems)
						.SelectionMode(ESelectionMode::None)
						.OnGenerateRow(this, &SJsonSyncPanel::GenerateKeyRow)
						.HeaderRow
						(
							SNew(SHeaderRow)
							+ SHeaderRow::Column(ColumnTake).DefaultLabel(LOCTEXT("ColumnTake", "Use JSON")).FixedWidth(72.0f)
							+ SHeaderRow::Column(ColumnKey).DefaultLabel(LOCTEXT("ColumnKey", "Key")).FillWidth(0.25f)
							+ SHeaderRow::Column(ColumnJson).DefaultLabel(LOCTEXT("ColumnJson", "JSON value")).FillWidth(0.3f)
							+ SHeaderRow::Column(ColumnValue).DefaultLabel(LOCTEXT("ColumnValue", "Asset value")).FillWidth(0.3f)
							+ SHeaderRow::Column(ColumnChange).DefaultLabel(LOCTEXT("ColumnChange", "Changed in")).FillWidth(0.15f)
						)
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("TickAll", "Tick All"))
						.IsEnabled(this, &SJsonSyncPanel::CanApply)
						.OnClicked(this, &SJsonSyncPanel::OnSetAllClicked, true)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("TickNone", "Tick None"))
						.IsEnabled(this, &SJsonSyncPanel::CanApply)
						.OnClicked(this, &SJsonSyncPanel::OnSetAllClicked, false)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNullWidget::NullWidget
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Browse", "Show in Content Browser"))
						.IsEnabled(this, &SJsonSyncPanel::HasSelection)
						.OnClicked(this, &SJsonSyncPanel::OnBrowseClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Overwrite", "Overwrite JSON"))
						.ToolTipText(LOCTEXT("OverwriteTip", "Discard the JSON's changes: write the asset's current values to the JSON file."))
						.IsEnabled(this, &SJsonSyncPanel::HasSelection)
						.OnClicked(this, &SJsonSyncPanel::OnOverwriteClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.Text(LOCTEXT("Apply", "Apply JSON"))
						.ToolTipText(LOCTEXT("ApplyTip", "Set the ticked keys to their JSON values (one undo step; the asset is not saved). Unticked keys keep the asset's value; saving the asset writes them to the JSON."))
						.IsEnabled(this, &SJsonSyncPanel::CanApply)
						.OnClicked(this, &SJsonSyncPanel::OnApplyClicked)
					]
				]
			]
		]
	];

	Refresh();
}

SJsonSyncPanel::~SJsonSyncPanel()
{
	if (UJsonBlueprintSyncSubsystem* Owner = Subsystem.Get())
	{
		Owner->OnPendingChanged.Remove(PendingChangedHandle);
	}
}

UJsonBlueprintSyncSubsystem* SJsonSyncPanel::GetSubsystem() const
{
	return GEditor ? GEditor->GetEditorSubsystem<UJsonBlueprintSyncSubsystem>() : nullptr;
}

void SJsonSyncPanel::Refresh()
{
	TGuardValue<bool> Guard(bRefreshing, true);
	UJsonBlueprintSyncSubsystem* Owner = Subsystem.Get();

	AssetItems.Reset();
	TSharedPtr<FAssetItem> ToSelect;
	if (Owner)
	{
		for (const FString& PackageName : Owner->GetPendingPackages())
		{
			UJsonBlueprintSyncSubsystem::FPendingView View;
			if (!Owner->GetPendingView(PackageName, View))
			{
				continue;
			}
			const TSharedPtr<FAssetItem> Item = MakeShared<FAssetItem>();
			Item->PackageName = PackageName;
			Item->Kind = View.Kind;
			if (!View.Error.IsEmpty())
			{
				Item->Status = LOCTEXT("StatusError", "Cannot read the JSON");
				Item->bProblem = true;
			}
			else if (View.ParentDiffers())
			{
				Item->Status = FText::Format(LOCTEXT("StatusParent", "{0} keys, parent differs"), FText::AsNumber(View.Keys.Num()));
				Item->bProblem = true;
			}
			else if (View.IsJsonOutOfDate())
			{
				Item->Status = FText::Format(LOCTEXT("StatusOutOfDate", "{0} keys, JSON out of date"), FText::AsNumber(View.Keys.Num()));
			}
			else if (View.Keys.ContainsByPredicate([](const FKeyDiff& Diff) { return Diff.Change == EKeyChange::Both; }))
			{
				Item->Status = FText::Format(LOCTEXT("StatusConflict", "{0} keys, conflicts"), FText::AsNumber(View.Keys.Num()));
				Item->bProblem = true;
			}
			else
			{
				Item->Status = FText::Format(LOCTEXT("StatusChanged", "{0} keys"), FText::AsNumber(View.Keys.Num()));
			}
			AssetItems.Add(Item);
			if (PackageName == SelectedPackage)
			{
				ToSelect = Item;
			}
		}
	}
	if (!ToSelect && AssetItems.Num() > 0)
	{
		ToSelect = AssetItems[0];
	}
	if (!ToSelect || ToSelect->PackageName != SelectedPackage)
	{
		SelectedPackage = ToSelect ? ToSelect->PackageName : FString();
		Ticks.Reset();
	}
	AssetList->RequestListRefresh();
	if (ToSelect)
	{
		AssetList->SetSelection(ToSelect, ESelectInfo::Direct);
	}
	else
	{
		AssetList->ClearSelection();
	}
	RefreshKeys();
}

void SJsonSyncPanel::RefreshKeys()
{
	KeyItems.Reset();
	SelectedError.Reset();
	bSelectedHasBase = false;
	bSelectedOutOfDate = false;
	SelectedJsonParent.Reset();
	SelectedAssetParent.Reset();

	UJsonBlueprintSyncSubsystem::FPendingView View;
	UJsonBlueprintSyncSubsystem* Owner = Subsystem.Get();
	if (Owner && !SelectedPackage.IsEmpty() && Owner->GetPendingView(SelectedPackage, View))
	{
		SelectedError = View.Error;
		bSelectedHasBase = View.bHasBase;
		bSelectedOutOfDate = View.IsJsonOutOfDate();
		SelectedJsonParent = View.JsonParent;
		SelectedAssetParent = View.AssetParent;
		for (const FKeyDiff& Diff : View.Keys)
		{
			const TSharedPtr<FKeyItem> Item = MakeShared<FKeyItem>();
			Item->Diff = Diff;
			const bool* Tick = Ticks.Find(Diff.Key);
			Item->bTake = Tick ? *Tick : Diff.TakeJsonByDefault();
			KeyItems.Add(Item);
		}
	}
	KeyList->RequestListRefresh();
}

TSharedRef<ITableRow> SJsonSyncPanel::GenerateAssetRow(TSharedPtr<FAssetItem> Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SAssetRow, Owner, Item);
}

TSharedRef<ITableRow> SJsonSyncPanel::GenerateKeyRow(TSharedPtr<FKeyItem> Item, const TSharedRef<STableViewBase>& Owner)
{
	TWeakPtr<SJsonSyncPanel> WeakThis = StaticCastSharedRef<SJsonSyncPanel>(AsShared());
	return SNew(SKeyRow, Owner, Item, [WeakThis](const FString& Key, bool bTake)
	{
		if (const TSharedPtr<SJsonSyncPanel> Panel = WeakThis.Pin())
		{
			Panel->Ticks.Add(Key, bTake);
		}
	});
}

void SJsonSyncPanel::OnAssetSelected(TSharedPtr<FAssetItem> Item, ESelectInfo::Type SelectInfo)
{
	if (bRefreshing)
	{
		return;
	}
	const FString PackageName = Item ? Item->PackageName : FString();
	if (PackageName != SelectedPackage)
	{
		SelectedPackage = PackageName;
		Ticks.Reset();
	}
	RefreshKeys();
}

void SJsonSyncPanel::OnAssetDoubleClicked(TSharedPtr<FAssetItem> Item)
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

FText SJsonSyncPanel::GetSummaryText() const
{
	if (!Subsystem.IsValid())
	{
		return LOCTEXT("NoSubsystem", "JSON Sync is not running.");
	}
	if (AssetItems.Num() == 0)
	{
		return LOCTEXT("NothingPending", "Every synced asset matches its JSON file.");
	}
	return FText::Format(LOCTEXT("Summary", "{0} asset(s) differ from their JSON file. Saving one of them does not write its JSON until you apply or overwrite it, unless the JSON is only out of date."),
		FText::AsNumber(AssetItems.Num()));
}

FText SJsonSyncPanel::GetDetailText() const
{
	if (SelectedPackage.IsEmpty())
	{
		return FText::GetEmpty();
	}
	if (!SelectedError.IsEmpty())
	{
		return FText::Format(LOCTEXT("DetailError", "{0}\nThe JSON cannot be applied: {1}\nFix the file and press Refresh, or overwrite it with the asset's values."),
			FText::FromString(SelectedPackage), FText::FromString(SelectedError));
	}
	if (!SelectedJsonParent.IsEmpty())
	{
		return FText::Format(LOCTEXT("DetailParent", "{0}\nThe JSON was written for parent {1}, but the asset's parent is {2} (it was reparented, or one side is out of date). The JSON's values were relative to the old parent, so keys it does not list may differ as well. Usually: Overwrite JSON. Ticked keys take the JSON value."),
			FText::FromString(SelectedPackage), FText::FromString(SelectedJsonParent), FText::FromString(SelectedAssetParent));
	}
	if (bSelectedOutOfDate)
	{
		return FText::Format(LOCTEXT("DetailOutOfDate", "{0}\nThe JSON has no changes of its own: the asset changed since its JSON was written (e.g. the .uasset was committed without it). Saving the asset or Overwrite JSON updates the JSON. Ticking a key reverts the asset's change."),
			FText::FromString(SelectedPackage));
	}
	if (!bSelectedHasBase)
	{
		return FText::Format(LOCTEXT("DetailNoBase", "{0}\nIt is unknown which side changed each key (both the JSON and the asset changed since the last sync, or the asset was never synced on this machine), so every key is marked as a conflict. Tick the keys to take from the JSON."),
			FText::FromString(SelectedPackage));
	}
	return FText::Format(LOCTEXT("Detail", "{0}\nTicked keys take the JSON value. Unticked keys keep the asset's value."), FText::FromString(SelectedPackage));
}

bool SJsonSyncPanel::HasSelection() const
{
	return !SelectedPackage.IsEmpty() && Subsystem.IsValid();
}

bool SJsonSyncPanel::CanApply() const
{
	return HasSelection() && SelectedError.IsEmpty();
}

FReply SJsonSyncPanel::OnRefreshClicked()
{
	if (UJsonBlueprintSyncSubsystem* Owner = Subsystem.Get())
	{
		// Broadcasts OnPendingChanged, which refreshes this panel.
		Owner->SyncFromJson();
	}
	return FReply::Handled();
}

FReply SJsonSyncPanel::OnApplyClicked()
{
	TArray<FString> Keys;
	for (const TSharedPtr<FKeyItem>& Item : KeyItems)
	{
		if (Item->bTake)
		{
			Keys.Add(Item->Diff.Key);
		}
	}
	if (UJsonBlueprintSyncSubsystem* Owner = Subsystem.Get())
	{
		Owner->ApplyJson(SelectedPackage, Keys);
	}
	return FReply::Handled();
}

FReply SJsonSyncPanel::OnOverwriteClicked()
{
	if (UJsonBlueprintSyncSubsystem* Owner = Subsystem.Get())
	{
		Owner->OverwriteJson(SelectedPackage);
	}
	return FReply::Handled();
}

FReply SJsonSyncPanel::OnBrowseClicked()
{
	const FString ObjectPath = SelectedPackage + TEXT(".") + FPackageName::GetShortName(SelectedPackage);
	const FAssetData Asset = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	if (Asset.IsValid() && GEditor)
	{
		GEditor->SyncBrowserToObjects(TArray<FAssetData>{ Asset });
	}
	return FReply::Handled();
}

FReply SJsonSyncPanel::OnSetAllClicked(bool bTake)
{
	for (const TSharedPtr<FKeyItem>& Item : KeyItems)
	{
		Item->bTake = bTake;
		Ticks.Add(Item->Diff.Key, bTake);
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
