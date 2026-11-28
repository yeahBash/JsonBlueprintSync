#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogJsonDataAssetSync, Log, All);

namespace JsonDataAssetSync
{
	/** Id of the JSON Sync tab (Window > JSON Sync). */
	inline FName GetSyncTabName() { return FName(TEXT("JsonDataAssetSync")); }
}

class FJsonDataAssetSyncModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	bool bTabRegistered = false;
};
