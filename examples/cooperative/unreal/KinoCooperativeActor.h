#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KinoCooperativeActor.generated.h"

// Copy into your game's module; add its MODULE_API macro if exported elsewhere.
UCLASS()
class AKinoCooperativeActor : public AActor
{
	GENERATED_BODY()

public:
	AKinoCooperativeActor();
	virtual void Tick(float delta_seconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type end_play_reason) override;

private:
	bool session_open = false;
	bool running = false;
};
