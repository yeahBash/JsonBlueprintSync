#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "JsonDataAssetSyncCommandlet.generated.h"

/**
 * Syncs data asset values with JSON files.
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=JsonDataAssetSync -export|-import|-verify [-dir=/Game/...] [-jsondir=<path>] [-nosave]
 *
 * -export  writes the JSON for every data asset under -dir (only files that change).
 * -import  applies every JSON file under -dir to its data asset, then saves the changed packages (unless -nosave).
 * -verify  fails if any JSON file is missing, differs from its data asset, or has no data asset.
 * Data assets in the ExcludedFolders setting are skipped.
 */
UCLASS()
class UJsonDataAssetSyncCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UJsonDataAssetSyncCommandlet();

	virtual int32 Main(const FString& Params) override;
};