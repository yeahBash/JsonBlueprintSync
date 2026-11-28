#pragma once

#include "AssetMerge.h"
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class UJsonBlueprintMergeSubsystem;

/**
 * The Asset Merges tab: the merge files the merge tool saved, and for the selected ones what to change in each asset to get the
 * merged values. Ticked rows are applied with Apply (one undo step, not saved) or Apply and Save. A merge file stays until it is
 * deleted with its delete button.
 */
class SAssetMergePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAssetMergePanel) {}
	SLATE_END_ARGS()

	struct FFileItem
	{
		FString PackageName;
	};

	struct FRowItem
	{
		FString PackageName;
		JsonBlueprintSync::AssetMerge::FMergeRow Row;
		/** UJsonBlueprintMergeSubsystem::GetRowKey(Row). */
		FString Key;
	};

	void Construct(const FArguments& InArgs);
	virtual ~SAssetMergePanel() override;

	UJsonBlueprintMergeSubsystem* GetSubsystem() const;

	/** Whether the row can be ticked now: applicable, not applied yet, and its file can be applied. */
	bool CanTick(const FRowItem& Item) const;
	bool IsTicked(const FRowItem& Item) const;
	void SetTicked(const FRowItem& Item, bool bTicked);
	bool IsAppliedNotSaved(const FRowItem& Item) const;
	/** The reason the last Apply gave for the row, or null. */
	const FString* FindFailure(const FRowItem& Item) const;
	/** Why the row's checkbox is off or greyed, or empty. */
	FText GetRowTooltip(const FRowItem& Item) const;

	/** Asks, then deletes the merge file of the package. */
	void DeleteMergeFile(const FString& PackageName);

private:
	/** Rebuilds the file list from the subsystem, keeping the selection, then the rows. */
	void Refresh();
	void RefreshRows();

	TSharedRef<ITableRow> GenerateFileRow(TSharedPtr<FFileItem> Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> GenerateRowRow(TSharedPtr<FRowItem> Item, const TSharedRef<STableViewBase>& Owner);
	void OnFileSelectionChanged(TSharedPtr<FFileItem> Item, ESelectInfo::Type SelectInfo);
	void OnFileDoubleClicked(TSharedPtr<FFileItem> Item);

	FText GetSummaryText() const;
	FText GetDetailText() const;
	FText GetHintText() const;
	bool HasSelection() const;
	bool CanApply() const;

	FReply OnRefreshClicked();
	FReply OnApplyClicked(bool bSave);
	FReply OnBrowseClicked();

	TArray<TSharedPtr<FFileItem>> FileItems;
	TArray<TSharedPtr<FRowItem>> RowItems;
	TSharedPtr<SListView<TSharedPtr<FFileItem>>> FileList;
	TSharedPtr<SListView<TSharedPtr<FRowItem>>> RowList;

	/** Packages whose rows are shown. */
	TArray<FString> SelectedPackages;

	/** Rows the user unticked, by package and row key; everything else that can be ticked is. */
	TMap<FString, TSet<FString>> Unticked;

	TWeakObjectPtr<UJsonBlueprintMergeSubsystem> Subsystem;
	FDelegateHandle ChangedHandle;
	bool bRefreshing = false;
};
