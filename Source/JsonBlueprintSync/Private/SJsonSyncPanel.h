#pragma once

#include "CoreMinimal.h"
#include "JsonBlueprintSync.h"
#include "SyncState.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class UJsonBlueprintSyncSubsystem;

/**
 * The JSON Sync tab: assets whose JSON has changes that are not applied, and for the selected one the keys that differ.
 * Ticked keys take the JSON value on Apply JSON; the others keep the asset's value, which the next save writes to the JSON.
 */
class SJsonSyncPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SJsonSyncPanel) {}
	SLATE_END_ARGS()

	struct FAssetItem
	{
		FString PackageName;
		JsonBlueprintSync::EAssetKind Kind = JsonBlueprintSync::EAssetKind::None;
		FText Status;
		bool bProblem = false;
	};

	struct FKeyItem
	{
		JsonBlueprintSync::FKeyDiff Diff;
		bool bTake = false;
	};

	void Construct(const FArguments& InArgs);
	virtual ~SJsonSyncPanel() override;

private:
	UJsonBlueprintSyncSubsystem* GetSubsystem() const;

	/** Rebuilds both lists from the subsystem, keeping the selection and the ticks of keys that still differ. */
	void Refresh();
	void RefreshKeys();

	TSharedRef<ITableRow> GenerateAssetRow(TSharedPtr<FAssetItem> Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> GenerateKeyRow(TSharedPtr<FKeyItem> Item, const TSharedRef<STableViewBase>& Owner);
	void OnAssetSelected(TSharedPtr<FAssetItem> Item, ESelectInfo::Type SelectInfo);
	void OnAssetDoubleClicked(TSharedPtr<FAssetItem> Item);

	FText GetSummaryText() const;
	FText GetDetailText() const;
	bool HasSelection() const;
	bool CanApply() const;

	FReply OnRefreshClicked();
	FReply OnApplyClicked();
	FReply OnOverwriteClicked();
	FReply OnBrowseClicked();
	FReply OnSetAllClicked(bool bTake);

	TArray<TSharedPtr<FAssetItem>> AssetItems;
	TArray<TSharedPtr<FKeyItem>> KeyItems;
	TSharedPtr<SListView<TSharedPtr<FAssetItem>>> AssetList;
	TSharedPtr<SListView<TSharedPtr<FKeyItem>>> KeyList;

	/** Package shown in the key list. */
	FString SelectedPackage;
	FString SelectedError;
	bool bSelectedHasBase = false;

	/** The user's ticks for the selected package, by key, so that a refresh keeps them. */
	TMap<FString, bool> Ticks;

	TWeakObjectPtr<UJsonBlueprintSyncSubsystem> Subsystem;
	FDelegateHandle PendingChangedHandle;
	bool bRefreshing = false;
};
