#include "KinoCooperativeActor.h"
#include "cooperative_bridge.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

AKinoCooperativeActor::AKinoCooperativeActor()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AKinoCooperativeActor::BeginPlay()
{
	Super::BeginPlay();
	check(IsInGameThread());
	TArray<uint8> wasm_data;
	const FString wasm_path = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("KinoWASM/cooperative.wasm"));
	if(!FFileHelper::LoadFileToArray(wasm_data, *wasm_path)) {
		UE_LOG(LogTemp, Error, TEXT("Cannot read %s"), *wasm_path);
		return;
	}
	if(kino_example_open(wasm_data.GetData(), static_cast<uint32>(wasm_data.Num())) != 0) {
		UE_LOG(LogTemp, Error, TEXT("KinoWASM load failed: %u"), kino_example_error());
		return;
	}
	session_open = true;
	running = true;
}

void AKinoCooperativeActor::Tick(float delta_seconds)
{
	Super::Tick(delta_seconds);
	check(IsInGameThread());
	if(!running)
		return;

	// Exactly one invoke/resume per Tick. Yield returns control to the engine.
	const int32 status = kino_example_tick();
	UE_LOG(LogTemp, Display, TEXT("KinoWASM progress: %d"), kino_example_progress());
	if(status == 1)
		return;

	running = false;
	if(status == 0) {
		UE_LOG(LogTemp, Display, TEXT("KinoWASM completed: %d"), kino_example_result());
	} else {
		UE_LOG(LogTemp, Error, TEXT("KinoWASM failed: %u"), kino_example_error());
	}
}

void AKinoCooperativeActor::EndPlay(const EEndPlayReason::Type end_play_reason)
{
	check(IsInGameThread());
	running = false;
	if(session_open)
		kino_example_close();

	session_open = false;
	Super::EndPlay(end_play_reason);
}
