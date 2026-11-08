#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

THIRD_PARTY_INCLUDES_START
#include "Core/Math.h"
THIRD_PARTY_INCLUDES_END

#include "TeinoraWorld.generated.h"

class ATeinoraCamera;
class UDirectionalLightComponent;
class UExponentialHeightFogComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPointLightComponent;
class UProceduralMeshComponent;
class USkyLightComponent;
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
	double TimeRate = 1.0;   // how many times faster than usual the world goes by
	int32 Bodies = 0;
	int32 Awake = 0;
	int32 Contacts = 0;
	double MagicEarth = 0.0;  // kg/m3 of each kind at the camera
	double MagicWater = 0.0;
	double MagicAir = 0.0;
	double MagicNature = 0.0;
	double MagicFire = 0.0;   // in all its forms
	double MagicNeutral = 0.0;
	double CloudDepth = 0.0;  // the cloud over the camera
	double CloudWater = 0.0;
	double CloudAir = 0.0;
	double CloudSpeed = 0.0;
	double CloudRubbing = 0.0;
	double CloudLightning = 0.0;
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
	void SetView(int32 ViewRadius, int32 GeometryDetail, bool bClouds, double CloudCover);
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
	void CreateFires();
	void UpdateFires(const Teinora::Vec3& Eye);
	void UpdateStorms(const ATeinoraCamera& Eye);
	void UpdateSky(ATeinoraCamera* Eye);
	void UpdateWater(const Teinora::Vec3& Eye);
	void UpdateLakes(const Teinora::Vec3& Eye);
	void UpdateLiquid(const Teinora::Vec3& Eye);
	void UpdatePlants(const Teinora::Vec3& Eye);
	void UpdateEyeInWater(ATeinoraCamera& Eye);
	void UpdateBodies();
	void UpdateMagic(ATeinoraCamera& Eye, float DeltaSeconds);
	void UpdateWeather(ATeinoraCamera& Eye, double WorldDt);
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
	TObjectPtr<UExponentialHeightFogComponent> Fog;

	UPROPERTY()
	TObjectPtr<USkyLightComponent> SkyFill;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> CloudMaterial;
	float CloudBaseDensity = 1.0f; // the cloud material's own density, which the weather thickens

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> SkyDome;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> SkyMaterial;

	UPROPERTY()
	TObjectPtr<UTexture2D> StarCells;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> TerrainMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> LeafMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> SeaMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> LakeMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> BodyMaterial;

	UPROPERTY()
	TArray<TObjectPtr<UProceduralMeshComponent>> Flames;

	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> FlameMaterials;

	UPROPERTY()
	TArray<TObjectPtr<UPointLightComponent>> FireLights;

	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> PuffMesh;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> PuffMaterial;

	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> BoltMesh;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> BoltMaterial;

	UPROPERTY()
	TObjectPtr<UTexture2D> LiquidMap;

	UPROPERTY()
	TObjectPtr<UTexture2D> PlantMap;

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
	FLinearColor DayHazeColor;
	bool bPlanetLightsAir = false;

	FString ShotPath;
	int32 AutoFrames = 0;
	int32 SettledFrames = 0;
	int32 ThrowsLeft = 0;
	double StormCover = 1.0;
	double StrikeAhead = 0.0; // an automated run ends with a bolt this far ahead
	int32 StrikeFrame = -1;   // or has it once, at this frame
};
