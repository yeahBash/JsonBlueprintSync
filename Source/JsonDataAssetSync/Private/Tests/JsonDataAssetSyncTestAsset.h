#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "JsonDataAssetSyncTestAsset.generated.h"

/**
 * The data asset class of the subsystem tests (JsonDataAssetSyncSubsystemTests.cpp). Native, so that its assets can be saved to
 * disk without a class Blueprint next to them. Hidden from class pickers; it is not meant for real assets.
 */
UCLASS(HideDropdown, NotBlueprintable, NotBlueprintType)
class UJsonDataAssetSyncTestAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Test")
	double Speed = 0.0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Count = 0;
};
