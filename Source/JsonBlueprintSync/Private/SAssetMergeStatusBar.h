#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * The asset merges entry in the editor's status bar, left of the JSON Sync one: how many merges are left to apply, or that they are
 * done and wait to be deleted. Hidden while there is no merge file. The tooltip lists them; clicking opens the Asset Merges tab.
 */
class SAssetMergeStatusBar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAssetMergeStatusBar) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Adds the widget to the level editor's status bar, after SJsonSyncStatusBar::Register. */
	static void Register(void* Owner);

private:
	EVisibility GetEntryVisibility() const;
	FText GetText() const;
	FText GetTooltip() const;
	FSlateColor GetColor() const;
};
