#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogJsonBlueprintSync, Log, All);

namespace JsonBlueprintSync
{
	/** Id of the JSON Sync tab (Window > JSON Sync). */
	inline FName GetSyncTabName() { return FName(TEXT("JsonBlueprintSync")); }
}

class FJsonBlueprintSyncModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	bool bTabRegistered = false;
};
