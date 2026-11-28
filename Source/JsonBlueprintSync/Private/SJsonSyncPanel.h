#pragma once

#include "CoreMinimal.h"
#include "JsonBlueprintSync.h"
#include "SyncState.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class UJsonBlueprintSyncSubsystem;

/**
 * The JSON Sync tab, a merge view: assets whose JSON has changes that are not applied, and for the selected one the keys that differ.
 * Each key takes its value from the JSON or keeps the asset's. Keys changed on one side only start with that side; conflicts must be
 * chosen (click a value, or right-click selected keys) before Apply or Apply and Save.
 */
class SJsonSyncPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SJsonSyncPanel) {}
	SLATE_END_ARGS()

	/** Where a key's value comes from. None: not chosen yet (a conflict). */
	enum class EKeySource : uint8
	{
		None,
		Json,
		Asset,
	};

	struct FAssetItem
	{
		FString PackageName;
		FText Status;
		bool bProblem = false;
	};

	struct FKeyItem
	{
		JsonBlueprintSync::FKeyDiff Diff;
		EKeySource Source = EKeySource::None;
	};

	void Construct(const FArguments& InArgs);
	virtual ~SJsonSyncPanel() override;

	/** The side a key starts with: the side that changed it, or None for a conflict. */
	static EKeySource GetDefaultSource(const JsonBlueprintSync::FKeyDiff& Diff);

private:
	/** A source the user chose, with the values it was chosen for. If either value changes, the choice is dropped. */
	struct FKeyChoice
	{
		EKeySource Source = EKeySource::None;
		TOptional<FString> JsonValue;
		TOptional<FString> AssetValue;
	};

	UJsonBlueprintSyncSubsystem* GetSubsystem() const;

	/** Rebuilds both lists from the subsystem, keeping the selection and the choices for keys whose values have not changed. */
	void Refresh();
	void RefreshKeys();

	TSharedRef<ITableRow> GenerateAssetRow(TSharedPtr<FAssetItem> Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> GenerateKeyRow(TSharedPtr<FKeyItem> Item, const TSharedRef<STableViewBase>& Owner);
	void OnAssetSelected(TSharedPtr<FAssetItem> Item, ESelectInfo::Type SelectInfo);
	void OnAssetDoubleClicked(TSharedPtr<FAssetItem> Item);
	TSharedPtr<SWidget> OnKeyContextMenuOpening();

	/** Sets the key's source and remembers it for the selected package. None resets it to its default. */
	void SetSource(FKeyItem& Item, EKeySource Source);
	void SetSelectedSource(EKeySource Source);

	FText GetSummaryText() const;
	FText GetDetailText() const;
	FText GetUnresolvedText() const;
	int32 GetUnresolvedCount() const;
	bool HasSelection() const;
	bool HasKeys() const;
	bool CanApply() const;

	FReply OnRefreshClicked();
	FReply OnApplyClicked(bool bSave);
	FReply OnOverwriteClicked();
	/** An asset that changed while its JSON did not: every key from the JSON. */
	FReply OnApplyAllJsonClicked();
	bool IsOutOfDateSelected() const;
	/** The per-key controls are hidden for an asset that changed while its JSON did not; it gets Write JSON and Apply JSON. */
	EVisibility GetKeyChoiceVisibility() const;
	FReply OnBrowseClicked();
	FReply OnSetAllClicked(EKeySource Source);

	TArray<TSharedPtr<FAssetItem>> AssetItems;
	TArray<TSharedPtr<FKeyItem>> KeyItems;
	TSharedPtr<SListView<TSharedPtr<FAssetItem>>> AssetList;
	TSharedPtr<SListView<TSharedPtr<FKeyItem>>> KeyList;

	/** Package shown in the key list. */
	FString SelectedPackage;
	FString SelectedError;
	bool bSelectedHasBase = false;
	bool bSelectedOutOfDate = false;
	/** Set if the JSON was written for another class than the asset has. */
	FString SelectedJsonParent;
	FString SelectedAssetParent;

	/** The user's choices by package and key, so that switching assets or a refresh keeps them. */
	TMap<FString, TMap<FString, FKeyChoice>> Choices;

	TWeakObjectPtr<UJsonBlueprintSyncSubsystem> Subsystem;
	FDelegateHandle PendingChangedHandle;
	bool bRefreshing = false;
};
