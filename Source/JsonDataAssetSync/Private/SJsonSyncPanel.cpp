#include "SJsonSyncPanel.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "InputCoreTypes.h"
#include "JsonDataAssetSyncSubsystem.h"
#include "Misc/PackageName.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "JsonDataAssetSync"

using namespace JsonDataAssetSync;

namespace
{
	using EKeySource = SJsonSyncPanel::EKeySource;

	const FName ColumnAsset(TEXT("Asset"));
	const FName ColumnStatus(TEXT("Status"));
	const FName ColumnKey(TEXT("Key"));
	const FName ColumnJson(TEXT("Json"));
	const FName ColumnValue(TEXT("Value"));
	const FName ColumnChange(TEXT("Change"));
	const FName ColumnUse(TEXT("Use"));

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
		case EKeyChange::Json: return LOCTEXT("ChangedJsonTip", "Changed in the JSON; the asset still has the old value. Uses the JSON unless you choose otherwise.");
		case EKeyChange::Asset: return LOCTEXT("ChangedAssetTip", "Changed in the asset; the JSON still has the old value. Keeps the asset's value unless you choose otherwise; saving the asset writes it to the JSON.");
		default: return LOCTEXT("ChangedBothTip", "Changed on both sides, or it is unknown which side changed it. Choose the JSON or the asset value before applying.");
		}
	}

	FText SourceText(EKeySource Source)
	{
		switch (Source)
		{
		case EKeySource::Json: return LOCTEXT("UseJsonValue", "JSON");
		case EKeySource::Asset: return LOCTEXT("UseAssetValue", "Asset");
		default: return LOCTEXT("UseNone", "Choose");
		}
	}

	FText SourceTooltip(EKeySource Source)
	{
		switch (Source)
		{
		case EKeySource::Json: return LOCTEXT("UseJsonValueTip", "Apply sets this key to the JSON value.");
		case EKeySource::Asset: return LOCTEXT("UseAssetValueTip", "The asset keeps its value; saving the asset writes it to the JSON.");
		default: return LOCTEXT("UseNoneTip", "Not chosen yet: click the JSON or the asset value, or right-click the key.");
		}
	}

	/** Background of the value a key uses. */
	FSlateColor ChosenColor()
	{
		return FStyleColors::Primary.GetSpecifiedColor().CopyWithNewOpacity(0.45f);
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
		using FOnChoose = TFunction<void(SJsonSyncPanel::FKeyItem&, EKeySource)>;

		SLATE_BEGIN_ARGS(SKeyRow) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner, TSharedPtr<SJsonSyncPanel::FKeyItem> InItem, FOnChoose InOnChoose)
		{
			Item = InItem;
			OnChoose = MoveTemp(InOnChoose);
			SMultiColumnTableRow<TSharedPtr<SJsonSyncPanel::FKeyItem>>::Construct(FSuperRowType::FArguments(), Owner);
		}

		/**
		 * Only a multi-selection (Ctrl or Shift click, for the context menu) is highlighted. A single selected row, which every click on
		 * a value makes, gets the normal row background: its highlight only competed with the one of the chosen value.
		 */
		virtual const FSlateBrush* GetBorder() const override
		{
			const TSharedPtr<ITypedTableView<TSharedPtr<SJsonSyncPanel::FKeyItem>>> Table = OwnerTablePtr.Pin();
			if (Table.IsValid() && Table->Private_GetNumSelectedItems() <= 1 && IsItemSelected())
			{
				const bool bEven = IndexInList % 2 == 0;
				if (IsHovered())
				{
					return bEven ? &Style->EvenRowBackgroundHoveredBrush : &Style->OddRowBackgroundHoveredBrush;
				}
				return bEven ? &Style->EvenRowBackgroundBrush : &Style->OddRowBackgroundBrush;
			}
			return SMultiColumnTableRow<TSharedPtr<SJsonSyncPanel::FKeyItem>>::GetBorder();
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			const FKeyDiff& Diff = Item->Diff;
			if (Column == ColumnJson)
			{
				return MakeValueCell(EKeySource::Json, Diff.JsonValue);
			}
			if (Column == ColumnValue)
			{
				return MakeValueCell(EKeySource::Asset, Diff.AssetValue);
			}
			if (Column == ColumnUse)
			{
				const TSharedPtr<SJsonSyncPanel::FKeyItem> Captured = Item;
				return SNew(SBox)
					.Padding(FMargin(4.0f, 2.0f))
					[
						SNew(STextBlock)
						.Text_Lambda([Captured]() { return SourceText(Captured->Source); })
						.ToolTipText_Lambda([Captured]() { return SourceTooltip(Captured->Source); })
						.ColorAndOpacity_Lambda([Captured]()
						{
							// One colour per side, so the column reads at a glance; Choose is the warning colour.
							switch (Captured->Source)
							{
							case EKeySource::Json:  return FStyleColors::AccentBlue;
							case EKeySource::Asset: return FStyleColors::AccentGreen;
							default:                return FStyleColors::Warning;
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
		/** A value of one side: highlighted if the key uses it, dimmed if it uses the other side. A click makes the key use it. */
		TSharedRef<SWidget> MakeValueCell(EKeySource Side, const TOptional<FString>& Value)
		{
			const TSharedPtr<SJsonSyncPanel::FKeyItem> Captured = Item;
			const FOnChoose Choose = OnChoose;
			const bool bDefault = !Value.IsSet();
			const FText Text = ValueText(Value);
			const FText Tooltip = FText::Format(Side == EKeySource::Json
				? LOCTEXT("JsonCellTip", "{0}\n\nClick to use the JSON value.")
				: LOCTEXT("AssetCellTip", "{0}\n\nClick to keep the asset's value."), Text);
			return SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([Captured, Side]()
				{
					return Captured->Source == Side ? ChosenColor() : FSlateColor(FLinearColor::Transparent);
				})
				.Padding(FMargin(4.0f, 2.0f))
				.ToolTipText(Tooltip)
				.OnMouseButtonDown_Lambda([Captured, Choose, Side](const FGeometry&, const FPointerEvent& Event)
				{
					if (Event.GetEffectingButton() == EKeys::LeftMouseButton && Choose)
					{
						Choose(*Captured, Side);
					}
					// Unhandled, so that the row still updates the selection.
					return FReply::Unhandled();
				})
				[
					SNew(STextBlock)
					.Text(Text)
					.ColorAndOpacity_Lambda([Captured, Side, bDefault]()
					{
						const bool bOtherSide = Captured->Source != EKeySource::None && Captured->Source != Side;
						return bOtherSide || bDefault ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground();
					})
				];
		}

		TSharedPtr<SJsonSyncPanel::FKeyItem> Item;
		FOnChoose OnChoose;
	};
}

SJsonSyncPanel::EKeySource SJsonSyncPanel::GetDefaultSource(const FKeyDiff& Diff)
{
	switch (Diff.Change)
	{
	case EKeyChange::Json: return EKeySource::Json;
	case EKeyChange::Asset: return EKeySource::Asset;
	default: return EKeySource::None;
	}
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
				.ToolTipText(LOCTEXT("RefreshTip", "Check the JSON files again, e.g. after a git pull (same as the console command JsonDataAssetSync.Sync)"))
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
						+ SHeaderRow::Column(ColumnAsset).DefaultLabel(LOCTEXT("ColumnAsset", "Asset")).FillWidth(0.6f)
						+ SHeaderRow::Column(ColumnStatus).DefaultLabel(LOCTEXT("ColumnStatus", "Status")).FillWidth(0.4f)
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
						.SelectionMode(ESelectionMode::Multi)
						.OnGenerateRow(this, &SJsonSyncPanel::GenerateKeyRow)
						.OnContextMenuOpening(this, &SJsonSyncPanel::OnKeyContextMenuOpening)
						.HeaderRow
						(
							SNew(SHeaderRow)
							+ SHeaderRow::Column(ColumnKey).DefaultLabel(LOCTEXT("ColumnKey", "Key")).FillWidth(0.25f)
							+ SHeaderRow::Column(ColumnJson).DefaultLabel(LOCTEXT("ColumnJson", "JSON value")).FillWidth(0.3f)
							+ SHeaderRow::Column(ColumnValue).DefaultLabel(LOCTEXT("ColumnValue", "Asset value")).FillWidth(0.3f)
							+ SHeaderRow::Column(ColumnChange).DefaultLabel(LOCTEXT("ColumnChange", "Changed in")).FillWidth(0.12f)
							+ SHeaderRow::Column(ColumnUse).DefaultLabel(LOCTEXT("ColumnUse", "Use")).FixedWidth(64.0f)
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
						.Text(LOCTEXT("AllJson", "All from JSON"))
						.ToolTipText(LOCTEXT("AllJsonTip", "Use the JSON value for every key."))
						.IsEnabled(this, &SJsonSyncPanel::HasKeys)
						.Visibility(this, &SJsonSyncPanel::GetKeyChoiceVisibility)
						.OnClicked(this, &SJsonSyncPanel::OnSetAllClicked, EKeySource::Json)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("AllAsset", "All from Asset"))
						.ToolTipText(LOCTEXT("AllAssetTip", "Keep the asset's value for every key, discarding the JSON's changes."))
						.IsEnabled(this, &SJsonSyncPanel::HasKeys)
						.Visibility(this, &SJsonSyncPanel::GetKeyChoiceVisibility)
						.OnClicked(this, &SJsonSyncPanel::OnSetAllClicked, EKeySource::Asset)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("ResetChoices", "Reset"))
						.ToolTipText(LOCTEXT("ResetChoicesTip", "Undo your choices: keys changed on one side use that side, conflicts are not chosen."))
						.IsEnabled(this, &SJsonSyncPanel::HasKeys)
						.Visibility(this, &SJsonSyncPanel::GetKeyChoiceVisibility)
						.OnClicked(this, &SJsonSyncPanel::OnSetAllClicked, EKeySource::None)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(this, &SJsonSyncPanel::GetUnresolvedText)
						.ColorAndOpacity(FStyleColors::Warning)
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
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.Text(LOCTEXT("WriteJson", "Write JSON"))
						.ToolTipText_Lambda([this]()
						{
							return SelectedError.IsEmpty()
								? LOCTEXT("WriteJsonTip", "Keep the asset as it is: write its current values to the JSON file.")
								: LOCTEXT("WriteJsonErrorTip", "Discard the unreadable JSON: write the asset's current values to the JSON file.");
						})
						.Visibility_Lambda([this]() { return HasSelection() && (!SelectedError.IsEmpty() || IsOutOfDateSelected()) ? EVisibility::Visible : EVisibility::Collapsed; })
						.OnClicked(this, &SJsonSyncPanel::OnOverwriteClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("ApplyJson", "Apply JSON"))
						.ToolTipText(LOCTEXT("ApplyJsonTip", "Revert the asset's changes: set every listed key to its JSON value (one undo step; the asset is not saved)."))
						.Visibility_Lambda([this]() { return IsOutOfDateSelected() ? EVisibility::Visible : EVisibility::Collapsed; })
						.OnClicked(this, &SJsonSyncPanel::OnApplyAllJsonClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Apply", "Apply"))
						.ToolTipText(LOCTEXT("ApplyTip", "Set the keys that use JSON to their JSON values (one undo step; the asset is not saved). Keys that use the asset keep its value; saving the asset writes them to the JSON. If nothing in the asset changes, the JSON is written now."))
						.IsEnabled(this, &SJsonSyncPanel::CanApply)
						.Visibility(this, &SJsonSyncPanel::GetKeyChoiceVisibility)
						.OnClicked(this, &SJsonSyncPanel::OnApplyClicked, false)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.Text(LOCTEXT("ApplySave", "Apply and Save"))
						.ToolTipText(LOCTEXT("ApplySaveTip", "Apply, then save the asset, which writes its JSON with the chosen values. Ctrl+Z still reverts the apply."))
						.IsEnabled(this, &SJsonSyncPanel::CanApply)
						.Visibility(this, &SJsonSyncPanel::GetKeyChoiceVisibility)
						.OnClicked(this, &SJsonSyncPanel::OnApplyClicked, true)
					]
				]
			]
		]
	];

	Refresh();
}

SJsonSyncPanel::~SJsonSyncPanel()
{
	if (UJsonDataAssetSyncSubsystem* Owner = Subsystem.Get())
	{
		Owner->OnPendingChanged.Remove(PendingChangedHandle);
	}
}

UJsonDataAssetSyncSubsystem* SJsonSyncPanel::GetSubsystem() const
{
	return GEditor ? GEditor->GetEditorSubsystem<UJsonDataAssetSyncSubsystem>() : nullptr;
}

void SJsonSyncPanel::Refresh()
{
	TGuardValue<bool> Guard(bRefreshing, true);
	UJsonDataAssetSyncSubsystem* Owner = Subsystem.Get();

	AssetItems.Reset();
	TSharedPtr<FAssetItem> ToSelect;
	TSet<FString> Listed;
	if (Owner)
	{
		for (const FString& PackageName : Owner->GetPendingPackages())
		{
			UJsonDataAssetSyncSubsystem::FPendingView View;
			if (!Owner->GetPendingView(PackageName, View))
			{
				continue;
			}
			const TSharedPtr<FAssetItem> Item = MakeShared<FAssetItem>();
			Item->PackageName = PackageName;
			if (!View.Error.IsEmpty())
			{
				Item->Status = LOCTEXT("StatusError", "Cannot read the JSON");
				Item->bProblem = true;
			}
			else if (View.ParentDiffers())
			{
				Item->Status = FText::Format(LOCTEXT("StatusParent", "{0} keys, class differs"), FText::AsNumber(View.Keys.Num()));
				Item->bProblem = true;
			}
			else if (View.IsJsonOutOfDate())
			{
				Item->Status = FText::Format(LOCTEXT("StatusOutOfDate", "{0} keys, asset changed"), FText::AsNumber(View.Keys.Num()));
				Item->bProblem = true;
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
			Listed.Add(PackageName);
			if (PackageName == SelectedPackage)
			{
				ToSelect = Item;
			}
		}
	}
	// Choices for assets that are no longer listed (applied, or the JSON matches again) are of no use.
	for (auto It = Choices.CreateIterator(); It; ++It)
	{
		if (!Listed.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	if (!ToSelect && AssetItems.Num() > 0)
	{
		ToSelect = AssetItems[0];
	}
	SelectedPackage = ToSelect ? ToSelect->PackageName : FString();
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

	UJsonDataAssetSyncSubsystem::FPendingView View;
	UJsonDataAssetSyncSubsystem* Owner = Subsystem.Get();
	if (Owner && !SelectedPackage.IsEmpty() && Owner->GetPendingView(SelectedPackage, View))
	{
		SelectedError = View.Error;
		bSelectedHasBase = View.bHasBase;
		bSelectedOutOfDate = View.IsJsonOutOfDate();
		SelectedJsonParent = View.JsonParent;
		SelectedAssetParent = View.AssetParent;

		TMap<FString, FKeyChoice> OldChoices;
		Choices.RemoveAndCopyValue(SelectedPackage, OldChoices);
		TMap<FString, FKeyChoice> KeptChoices;
		for (const FKeyDiff& Diff : View.Keys)
		{
			const TSharedPtr<FKeyItem> Item = MakeShared<FKeyItem>();
			Item->Diff = Diff;
			Item->Source = GetDefaultSource(Diff);
			// A choice was made for particular values: if the JSON or the asset changed the key since, it has to be made again.
			const FKeyChoice* Choice = OldChoices.Find(Diff.Key);
			if (Choice && Choice->JsonValue == Diff.JsonValue && Choice->AssetValue == Diff.AssetValue)
			{
				Item->Source = Choice->Source;
				KeptChoices.Add(Diff.Key, *Choice);
			}
			KeyItems.Add(Item);
		}
		if (KeptChoices.Num() > 0)
		{
			Choices.Add(SelectedPackage, MoveTemp(KeptChoices));
		}
	}
	KeyList->ClearSelection();
	KeyList->RequestListRefresh();
}

TSharedRef<ITableRow> SJsonSyncPanel::GenerateAssetRow(TSharedPtr<FAssetItem> Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SAssetRow, Owner, Item);
}

TSharedRef<ITableRow> SJsonSyncPanel::GenerateKeyRow(TSharedPtr<FKeyItem> Item, const TSharedRef<STableViewBase>& Owner)
{
	TWeakPtr<SJsonSyncPanel> WeakThis = StaticCastSharedRef<SJsonSyncPanel>(AsShared());
	return SNew(SKeyRow, Owner, Item, [WeakThis](FKeyItem& Key, EKeySource Source)
	{
		if (const TSharedPtr<SJsonSyncPanel> Panel = WeakThis.Pin())
		{
			Panel->SetSource(Key, Source);
		}
	});
}

void SJsonSyncPanel::OnAssetSelected(TSharedPtr<FAssetItem> Item, ESelectInfo::Type SelectInfo)
{
	if (bRefreshing)
	{
		return;
	}
	SelectedPackage = Item ? Item->PackageName : FString();
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

TSharedPtr<SWidget> SJsonSyncPanel::OnKeyContextMenuOpening()
{
	const int32 NumSelected = KeyList->GetNumItemsSelected();
	if (NumSelected == 0)
	{
		return nullptr;
	}
	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection(NAME_None, NumSelected == 1 ? LOCTEXT("MenuOneKey", "1 key") : FText::Format(LOCTEXT("MenuKeys", "{0} keys"), FText::AsNumber(NumSelected)));
	Menu.AddMenuEntry(LOCTEXT("UseJson", "Use JSON"), LOCTEXT("UseJsonTip", "Apply sets the selected keys to their JSON values."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SJsonSyncPanel::SetSelectedSource, EKeySource::Json)));
	Menu.AddMenuEntry(LOCTEXT("UseAsset", "Use Asset"), LOCTEXT("UseAssetTip", "The selected keys keep the asset's values; saving the asset writes them to the JSON."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SJsonSyncPanel::SetSelectedSource, EKeySource::Asset)));
	Menu.AddMenuEntry(LOCTEXT("ResetChoice", "Reset"), LOCTEXT("ResetChoiceTip", "Undo your choice: a key changed on one side uses that side, a conflict is not chosen."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SJsonSyncPanel::SetSelectedSource, EKeySource::None)));
	Menu.EndSection();
	return Menu.MakeWidget();
}

void SJsonSyncPanel::SetSource(FKeyItem& Item, EKeySource Source)
{
	if (Source == EKeySource::None)
	{
		Item.Source = GetDefaultSource(Item.Diff);
		if (TMap<FString, FKeyChoice>* PackageChoices = Choices.Find(SelectedPackage))
		{
			PackageChoices->Remove(Item.Diff.Key);
		}
		return;
	}
	Item.Source = Source;
	FKeyChoice& Choice = Choices.FindOrAdd(SelectedPackage).FindOrAdd(Item.Diff.Key);
	Choice.Source = Source;
	Choice.JsonValue = Item.Diff.JsonValue;
	Choice.AssetValue = Item.Diff.AssetValue;
}

void SJsonSyncPanel::SetSelectedSource(EKeySource Source)
{
	for (const TSharedPtr<FKeyItem>& Item : KeyList->GetSelectedItems())
	{
		SetSource(*Item, Source);
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
	return FText::Format(LOCTEXT("Summary", "{0} asset(s) differ from their JSON file. Saving one of them does not write its JSON until you apply it, unless the JSON is only out of date."),
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
		return FText::Format(LOCTEXT("DetailParent", "{0}\nThe JSON was written for class {1}, but the asset's class is {2} (the asset was recreated, or one side is out of date). The JSON's values were relative to the other class's defaults, so keys it does not list may differ as well. Usually: All from Asset, then Apply."),
			FText::FromString(SelectedPackage), FText::FromString(SelectedJsonParent), FText::FromString(SelectedAssetParent));
	}
	if (bSelectedOutOfDate)
	{
		return FText::Format(LOCTEXT("DetailOutOfDate", "{0}\nThe asset changed, but its JSON did not (e.g. the .uasset was committed without its JSON, or saved while the plugin was off). The JSON is the source of truth, so this is never fixed automatically: Write JSON keeps the asset's values, Apply JSON reverts the asset to the JSON."),
			FText::FromString(SelectedPackage));
	}
	if (!bSelectedHasBase)
	{
		return FText::Format(LOCTEXT("DetailNoBase", "{0}\nIt is unknown which side changed each key (both the JSON and the asset changed since the last sync, or the asset was never synced on this machine), so every key is a conflict. Choose a source for each: click its JSON or asset value, right-click selected keys, or use All from JSON / All from Asset."),
			FText::FromString(SelectedPackage));
	}
	return FText::Format(LOCTEXT("Detail", "{0}\nChoose where each key's value comes from: click its JSON or asset value, or right-click selected keys. Keys changed on one side start with that side."),
		FText::FromString(SelectedPackage));
}

int32 SJsonSyncPanel::GetUnresolvedCount() const
{
	int32 Count = 0;
	for (const TSharedPtr<FKeyItem>& Item : KeyItems)
	{
		Count += Item->Source == EKeySource::None ? 1 : 0;
	}
	return Count;
}

FText SJsonSyncPanel::GetUnresolvedText() const
{
	const int32 Count = GetUnresolvedCount();
	if (Count == 0)
	{
		return FText::GetEmpty();
	}
	return Count == 1 ? LOCTEXT("OneUnresolved", "1 conflict to choose") : FText::Format(LOCTEXT("Unresolved", "{0} conflicts to choose"), FText::AsNumber(Count));
}

bool SJsonSyncPanel::HasSelection() const
{
	return !SelectedPackage.IsEmpty() && Subsystem.IsValid();
}

bool SJsonSyncPanel::HasKeys() const
{
	return HasSelection() && SelectedError.IsEmpty() && KeyItems.Num() > 0;
}

bool SJsonSyncPanel::CanApply() const
{
	return HasSelection() && SelectedError.IsEmpty() && GetUnresolvedCount() == 0;
}

FReply SJsonSyncPanel::OnRefreshClicked()
{
	if (UJsonDataAssetSyncSubsystem* Owner = Subsystem.Get())
	{
		// Broadcasts OnPendingChanged, which refreshes this panel.
		Owner->SyncFromJson();
	}
	return FReply::Handled();
}

FReply SJsonSyncPanel::OnApplyClicked(bool bSave)
{
	if (!CanApply())
	{
		return FReply::Handled();
	}
	TArray<FString> Keys;
	for (const TSharedPtr<FKeyItem>& Item : KeyItems)
	{
		if (Item->Source == EKeySource::Json)
		{
			Keys.Add(Item->Diff.Key);
		}
	}
	if (UJsonDataAssetSyncSubsystem* Owner = Subsystem.Get())
	{
		const FString PackageName = SelectedPackage;
		Owner->ApplyJson(PackageName, Keys, bSave);
	}
	return FReply::Handled();
}

FReply SJsonSyncPanel::OnOverwriteClicked()
{
	if (UJsonDataAssetSyncSubsystem* Owner = Subsystem.Get())
	{
		Owner->OverwriteJson(SelectedPackage);
	}
	return FReply::Handled();
}

bool SJsonSyncPanel::IsOutOfDateSelected() const
{
	return HasSelection() && SelectedError.IsEmpty() && bSelectedOutOfDate;
}

EVisibility SJsonSyncPanel::GetKeyChoiceVisibility() const
{
	return IsOutOfDateSelected() ? EVisibility::Collapsed : EVisibility::Visible;
}

FReply SJsonSyncPanel::OnApplyAllJsonClicked()
{
	if (!IsOutOfDateSelected())
	{
		return FReply::Handled();
	}
	TArray<FString> Keys;
	for (const TSharedPtr<FKeyItem>& Item : KeyItems)
	{
		Keys.Add(Item->Diff.Key);
	}
	if (UJsonDataAssetSyncSubsystem* Owner = Subsystem.Get())
	{
		const FString PackageName = SelectedPackage;
		Owner->ApplyJson(PackageName, Keys, false);
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

FReply SJsonSyncPanel::OnSetAllClicked(EKeySource Source)
{
	for (const TSharedPtr<FKeyItem>& Item : KeyItems)
	{
		SetSource(*Item, Source);
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
