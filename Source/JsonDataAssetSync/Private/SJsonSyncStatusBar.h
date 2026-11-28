#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * The JSON Sync entry in the editor's status bar, just left of the unsaved assets: "All JSON Synced", or how many assets have JSON changes
 * to review. The tooltip lists them; clicking opens the JSON Sync tab.
 */
class SJsonSyncStatusBar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SJsonSyncStatusBar) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Adds the widget to the level editor's status bar. Call once the tool menus are ready (UToolMenus::RegisterStartupCallback). */
	static void Register(void* Owner);

private:
	static int32 GetPendingNum();
	FText GetText() const;
	FText GetTooltip() const;
	FSlateColor GetColor() const;
};
