#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

THIRD_PARTY_INCLUDES_START
#include "Core/Math.h"
THIRD_PARTY_INCLUDES_END

#include "TeinoraWorld.generated.h"

class ATeinoraCamera;
class UDirectionalLightComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UProceduralMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UTexture2D;
class UVolumetricCloudComponent;
struct FTeinoraSimulation;

namespace Teinora
{
	struct ChunkMesh;
}

struct FTeinoraWorldStats
{
	double Hour = 0.0;
	double Day = 1.0;
	double WindSpeed = 0.0;
	int32 Bodies = 0;
	int32 Awake = 0;
	int32 Contacts = 0;
	double MagicEarth = 0.0;  // kg/m3 of each kind at the camera
	double MagicWater = 0.0;
	double MagicAir = 0.0;
	double MagicNature = 0.0;
	bool bMagic = false;      // whether there is magic to report at all
	bool bMagicView = false;
};

/**
 * Runs the standalone game's world and physics, and shows them through Unreal: terrain
 * as mesh components, bodies as instances, and a sky of two suns, a ringed planet and the
 * star map, all following the hour.
 */
UCLASS()
class ATeinoraWorld : public AActor
{
	GENERATED_BODY()

public:
	ATeinoraWorld();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

	bool IsSettled() const;
	FTeinoraWorldStats GetStats() const;

	void SetPaused(bool bInPaused) { bPaused = bInPaused; }
	void SetClock(double Hour, double Day);
	void SetTimeRate(double Rate) { TimeRate = Rate; }
	void SetView(int32 ViewRadius, int32 GeometryDetail, bool bClouds);
	void ThrowBall(const Teinora::Vec3& Eye, const Teinora::Vec3& LineOfSight, double Speed);
	void ToggleMagicView() { bMagicView = !bMagicView; }

	/** What the terrain calls back into. */
	void ShowChunk(int32 X, int32 Y, const Teinora::ChunkMesh& Mesh);
	void HideChunk(int32 X, int32 Y);

private:
	void SpawnSky();
	void SpawnSkyDome();
	UDirectionalLightComponent* SpawnSkyLight(int32 AtmosphereIndex);
	void CreateWater();
	void UpdateSky(ATeinoraCamera* Eye);
	void UpdateWater(const Teinora::Vec3& Eye);
	void UpdateLakes(const Teinora::Vec3& Eye);
	void UpdateBodies();
	void UpdateMagic(ATeinoraCamera& Eye, float DeltaSeconds);
	void RunAutomation();

	UPROPERTY()
	TMap<int64, TObjectPtr<UProceduralMeshComponent>> Chunks;

	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Water;

	UPROPERTY()
	TMap<uint32, TObjectPtr<UProceduralMeshComponent>> Lakes; // those near enough to see, by their number

	UPROPERTY()
	TObjectPtr<UDirectionalLightComponent> Sun;

	UPROPERTY()
	TObjectPtr<UDirectionalLightComponent> SecondSun;

	UPROPERTY()
	TObjectPtr<UDirectionalLightComponent> PlanetLight;

	UPROPERTY()
	TObjectPtr<UVolumetricCloudComponent> Clouds;

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> SkyDome;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> SkyMaterial;

	UPROPERTY()
	TObjectPtr<UTexture2D> StarCells;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> TerrainMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> LeafMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> WaterMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> BodyMaterial;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> Primitives;

	UPROPERTY()
	TArray<TObjectPtr<UTexture2D>> SurfaceTextures;

	UPROPERTY()
	TObjectPtr<UTexture2D> MagicNearAtlas;

	UPROPERTY()
	TObjectPtr<UTexture2D> MagicFarAtlas;

	TSharedPtr<FTeinoraSimulation> Sim; // shared only because a unique one could not hide its type
	bool bPaused = false;
	bool bMagicView = false;
	double TimeRate = 1.0;
	float MagicFade = 0.0f;

	FString ShotPath;
	int32 AutoFrames = 0;
	int32 SettledFrames = 0;
	int32 ThrowsLeft = 0;
};
