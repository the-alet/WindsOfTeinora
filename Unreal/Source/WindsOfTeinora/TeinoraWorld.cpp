#include "TeinoraWorld.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProceduralMeshComponent.h"
#include "ShaderCompiler.h"
#include "TeinoraCamera.h"
#include "TeinoraConvert.h"
#include "TextureResource.h"
#include "UnrealClient.h"

THIRD_PARTY_INCLUDES_START
#include "GameWorld.h"
#include "MagicField.h"
#include "Physics/Physics.h"
#include "StarMap.h"
#include "SurfaceTextures.h"
#include "TeinoraSim.h"
#include <vector>
THIRD_PARTY_INCLUDES_END

using Teinora::Vec3;
namespace Sim = Teinora::Sim;

namespace
{
	constexpr double PhysicsStep = 1.0 / 120.0;
	constexpr int32 MaxPhysicsSteps = 8;
	constexpr double DayLength = 1200.0;

	constexpr int32 WaterCells = 128;
	constexpr double WaterReach = 4000.0; // metres from the camera to the edge of the sea
	constexpr double WaterSnap = 8.0;

	constexpr float SunIntensity = 8.0f;
	constexpr float SecondSunIntensity = 8.8f; // a little the brighter, as in the standalone renderer
	const FLinearColor SunTint(1.0f, 0.77f, 0.49f);
	const FLinearColor SecondSunTint(0.40f, 1.0f, 0.93f);

	constexpr float PlanetAlbedo = 0.125f; // it gives back 10 to 15% of what the suns send it
	constexpr float PlanetShine = 0.55f;
	constexpr double SkyDomeRadius = 2.0e7; // 200 km, past all the ground there is to see

	constexpr int32 SurfaceTextureSize = 1024;

	FLinearColor ToSkyVector(const Vec3& Value)
	{
		return FLinearColor(static_cast<float>(Value.X), static_cast<float>(Value.Y), static_cast<float>(Value.Z));
	}

	/** From 0 by day to 1 once the sky is dark enough for stars: the standalone sky's measure of it. */
	float GetNight(float Height, float SecondHeight, float Past, float SecondPast)
	{
		const float Day = FMath::SmoothStep(-0.08f, 0.22f, Height) * Past;
		const float SecondDay = FMath::SmoothStep(-0.08f, 0.22f, SecondHeight) * SecondPast;
		return 1.0f - FMath::SmoothStep(0.03f, 0.25f, 0.019f + 0.75f * Day + 0.35f * SecondDay);
	}

	/** The star map as the sky material reads it, one exact texel at a time. */
	UTexture2D* MakeStarTexture()
	{
		const Teinora::StarSheet Sheet = Teinora::MakeStarSheet(Teinora::MakeStarMap(Sim::WorldSeed));
		UE_LOG(LogTemp, Display, TEXT("Star map: %d of %d stars on the sheet"), Sheet.Placed, Teinora::StarMapCount);

		UTexture2D* Texture = UTexture2D::CreateTransient(Teinora::StarCellSide * Teinora::StarsPerCell * 2, Teinora::StarCellSide, PF_R8G8B8A8);
		Texture->SRGB = false;
		Texture->Filter = TF_Nearest;

		FByteBulkData& Bulk = Texture->GetPlatformData()->Mips[0].BulkData;
		const int64 Bytes = static_cast<int64>(Sheet.Pixels.size());
		Bulk.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Bulk.Realloc(Bytes), Sheet.Pixels.data(), Bytes);
		Bulk.Unlock();

		Texture->UpdateResource();
		return Texture;
	}

	/** One layer of the ground's textures, mips and all, as a texture of its own. */
	UTexture2D* MakeSurfaceTexture(const std::vector<std::uint8_t>& Pixels, const Teinora::SurfaceTextures& Surfaces, int32 Layer)
	{
		int64 LayerBytes = 0;
		for (int32 Mip = 0; Mip < Surfaces.Mips; ++Mip)
		{
			const int64 Side = FMath::Max(Surfaces.Size >> Mip, 1);
			LayerBytes += Side * Side * 4;
		}

		UTexture2D* Texture = UTexture2D::CreateTransient(Surfaces.Size, Surfaces.Size, PF_R8G8B8A8);
		Texture->SRGB = false;
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Wrap;

		FTexturePlatformData* Data = Texture->GetPlatformData();
		const uint8* Source = Pixels.data() + LayerBytes * Layer;
		for (int32 Mip = 0; Mip < Surfaces.Mips; ++Mip)
		{
			const int32 Side = FMath::Max(Surfaces.Size >> Mip, 1);
			const int64 Bytes = static_cast<int64>(Side) * Side * 4;
			if (Mip > 0)
			{
				Data->Mips.Add(new FTexture2DMipMap(Side, Side, 1));
			}

			FByteBulkData& Bulk = Data->Mips[Mip].BulkData;
			Bulk.Lock(LOCK_READ_WRITE);
			FMemory::Memcpy(Bulk.Realloc(Bytes), Source, Bytes);
			Bulk.Unlock();
			Source += Bytes;
		}

		Texture->UpdateResource();
		return Texture;
	}

	/** Lays a box of magic out as a sheet of slices, which materials read as easily as anything else. */
	void UploadMagicAtlas(TObjectPtr<UTexture2D>& Atlas, const Teinora::MagicVolume& Volume, int32 Columns, int32 Rows)
	{
		const int32 Width = Volume.SizeX * Columns;
		const int32 Height = Volume.SizeY * Rows;
		if (Atlas == nullptr)
		{
			Atlas = UTexture2D::CreateTransient(Width, Height, PF_R8G8B8A8);
			Atlas->SRGB = false;
			Atlas->Filter = TF_Bilinear;
			Atlas->AddressX = TA_Clamp;
			Atlas->AddressY = TA_Clamp;
			Atlas->UpdateResource();
		}

		const int32 RowBytes = Volume.SizeX * 4;
		uint8* Sheet = static_cast<uint8*>(FMemory::Malloc(static_cast<SIZE_T>(Width) * Height * 4));
		for (int32 Slice = 0; Slice < Volume.SizeZ; ++Slice)
		{
			const int32 Left = (Slice % Columns) * Volume.SizeX;
			const int32 Top = (Slice / Columns) * Volume.SizeY;
			for (int32 Row = 0; Row < Volume.SizeY; ++Row)
			{
				FMemory::Memcpy(Sheet + (static_cast<SIZE_T>(Top + Row) * Width + Left) * 4,
					Volume.Pixels.data() + static_cast<SIZE_T>(Slice * Volume.SizeY + Row) * RowBytes, RowBytes);
			}
		}

		const FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);
		Atlas->UpdateTextureRegions(0, 1, Region, Width * 4, 4, Sheet, [](uint8* Data, const FUpdateTextureRegion2D* Used)
		{
			FMemory::Free(Data);
			delete Used;
		});
	}

	/** Spreads grid lines so they are dense under the camera and sparse towards the horizon. */
	double SpreadWater(int32 Index)
	{
		const double Position = static_cast<double>(Index) / WaterCells * 2.0 - 1.0;
		return Position * FMath::Abs(Position) * WaterReach;
	}

	int64 MakeChunkKey(int32 X, int32 Y)
	{
		return (static_cast<int64>(X) << 32) | static_cast<uint32>(Y);
	}

	/** The terrain's side of Unreal: the shared world calls this, the actor does the work. */
	class FChunkMeshes final : public Teinora::IChunkDisplay
	{
	public:
		explicit FChunkMeshes(ATeinoraWorld& InOwner) : Owner(InOwner) {}

		void ShowChunk(int X, int Y, const Teinora::ChunkMesh& Mesh) override { Owner.ShowChunk(X, Y, Mesh); }
		void HideChunk(int X, int Y) override { Owner.HideChunk(X, Y); }

	private:
		ATeinoraWorld& Owner;
	};

	/** All the bodies that look alike, drawn as instances of one mesh. */
	struct FBodyGroup
	{
		Teinora::EPrimitive Primitive = Teinora::EPrimitive::Cube;
		FLinearColor Color;
		UInstancedStaticMeshComponent* Mesh = nullptr; // the actor owns it
		TArray<FTransform> Transforms;
	};
}

struct FTeinoraSimulation
{
	explicit FTeinoraSimulation(ATeinoraWorld& Owner)
		: Terrain(Owner)
		, World(Terrain, Physics, 10, 1)
	{
		if constexpr (Sim::bMagicEnabled)
		{
			Magic = std::make_unique<Teinora::MagicSimulation>(Sim::WorldSeed);
		}

		Physics.SetForceCallback([this](Teinora::Physics::BodyId Id, Teinora::Physics::Body& Item, double Dt)
		{
			World.ApplyForces(Id, Item, Time, Dt);
		});
	}

	Teinora::Physics::World Physics;
	FChunkMeshes Terrain;
	Teinora::GameWorld World;
	std::unique_ptr<Teinora::MagicSimulation> Magic;
	Teinora::MagicVolume NearMagic;
	Teinora::MagicVolume FarMagic;
	double Time = 0.0;
	double Accumulator = 0.0;
	double Hour = 10.0;
	double Day = 1.0;
	std::vector<Teinora::ObjectDraw> Objects;
	TArray<FBodyGroup> Groups;
	TArray<FVector> WaterVertices;
	TArray<FVector> WaterNormals;
};

ATeinoraWorld::ATeinoraWorld()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics; // after the camera has moved
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ATeinoraWorld::BeginPlay()
{
	Super::BeginPlay();

	// The same textures as the standalone game, drawn here rather than stored as assets.
	const Teinora::SurfaceTextures Surfaces = Teinora::MakeSurfaceTextures(SurfaceTextureSize);
	UMaterialInstanceDynamic* Ground = UMaterialInstanceDynamic::Create(LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Teinora/M_Terrain.M_Terrain")), this);
	UMaterialInstanceDynamic* Leaves = UMaterialInstanceDynamic::Create(LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Teinora/M_Leaf.M_Leaf")), this);
	const TCHAR* const LayerNames[] = { TEXT("Sand"), TEXT("Grass"), TEXT("Rock"), TEXT("Snow"), TEXT("Bark"), TEXT("Leaf"), TEXT("Needle") };
	for (int32 Layer = 0; Layer < Surfaces.Layers; ++Layer)
	{
		UTexture2D* Colors = MakeSurfaceTexture(Surfaces.Colors, Surfaces, Layer);
		UTexture2D* Normals = MakeSurfaceTexture(Surfaces.Normals, Surfaces, Layer);
		UMaterialInstanceDynamic* Target = Layer >= Teinora::LeafLayer ? Leaves : Ground;
		Target->SetTextureParameterValue(*(FString(LayerNames[Layer]) + TEXT("Color")), Colors);
		if (Target == Ground)
		{
			Target->SetTextureParameterValue(*(FString(LayerNames[Layer]) + TEXT("Normal")), Normals);
		}
		SurfaceTextures.Add(Colors);
		SurfaceTextures.Add(Normals);
	}
	TerrainMaterial = Ground;
	LeafMaterial = Leaves;

	WaterMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Teinora/M_Water.M_Water"));
	BodyMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Teinora/M_Body.M_Body"));

	Primitives.SetNum(static_cast<int32>(Teinora::EPrimitive::Count));
	Primitives[static_cast<int32>(Teinora::EPrimitive::Cube)] = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	Primitives[static_cast<int32>(Teinora::EPrimitive::Sphere)] = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	Primitives[static_cast<int32>(Teinora::EPrimitive::Cylinder)] = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

	Sim = MakeShared<FTeinoraSimulation>(*this);

	SpawnSky();
	CreateWater();

	FParse::Value(FCommandLine::Get(), TEXT("TeinoraFrames="), AutoFrames);
	FParse::Value(FCommandLine::Get(), TEXT("TeinoraShot="), ShotPath);
	FParse::Value(FCommandLine::Get(), TEXT("TeinoraThrows="), ThrowsLeft);
	FParse::Value(FCommandLine::Get(), TEXT("TeinoraHour="), Sim->Hour);
	FParse::Value(FCommandLine::Get(), TEXT("TeinoraDay="), Sim->Day);
	if (FParse::Param(FCommandLine::Get(), TEXT("TeinoraMagic")))
	{
		bMagicView = true;
		MagicFade = 1.0f;
	}
}

void ATeinoraWorld::EndPlay(const EEndPlayReason::Type Reason)
{
	Sim.Reset();
	Super::EndPlay(Reason);
}

bool ATeinoraWorld::IsSettled() const
{
	return Sim != nullptr && Sim->World.IsSettled();
}

FTeinoraWorldStats ATeinoraWorld::GetStats() const
{
	FTeinoraWorldStats Stats;
	if (Sim != nullptr)
	{
		Stats.Hour = Sim->Hour;
		Stats.Day = Sim->Day;
		Stats.Bodies = static_cast<int32>(Sim->World.GetPropCount());
		Stats.Awake = static_cast<int32>(Sim->Physics.GetAwakeCount());
		Stats.Contacts = static_cast<int32>(Sim->Physics.GetContactCount());

		const APlayerController* Player = GetWorld()->GetFirstPlayerController();
		const ATeinoraCamera* Eye = Player != nullptr ? Cast<ATeinoraCamera>(Player->GetPawn()) : nullptr;
		if (Eye != nullptr)
		{
			Stats.WindSpeed = Teinora::Length(Sim::WindAt(Eye->GetEye(), Sim->Time, Sim::WorldSeed));

			const Sim::MagicMix Mix = Sim->NearMagic.GetMix(Eye->GetEye());
			Stats.MagicEarth = Mix.Get(Sim::EMagicType::Earth);
			Stats.MagicWater = Mix.Get(Sim::EMagicType::Water);
			Stats.MagicAir = Mix.Get(Sim::EMagicType::Air);
			Stats.MagicNature = Mix.Get(Sim::EMagicType::Nature);
		}
		Stats.bMagic = Sim->Magic != nullptr;
		Stats.bMagicView = bMagicView;
	}
	return Stats;
}

void ATeinoraWorld::SetClock(double Hour, double Day)
{
	if (Sim != nullptr)
	{
		Sim->Hour = Hour;
		Sim->Day = Day;
	}
}

void ATeinoraWorld::SetView(int32 ViewRadius, int32 GeometryDetail, bool bClouds)
{
	if (Sim != nullptr)
	{
		Sim->World.SetViewRadius(ViewRadius);
		Sim->World.SetDetail(GeometryDetail);
	}
	if (Clouds != nullptr)
	{
		Clouds->SetVisibility(bClouds);
	}
}

void ATeinoraWorld::ThrowBall(const Vec3& Eye, const Vec3& LineOfSight, double Speed)
{
	Sim->World.ThrowBall(Eye, LineOfSight, Speed);
}

void ATeinoraWorld::ShowChunk(int32 X, int32 Y, const Teinora::ChunkMesh& Mesh)
{
	TObjectPtr<UProceduralMeshComponent>& Component = Chunks.FindOrAdd(MakeChunkKey(X, Y));
	if (Component == nullptr)
	{
		Component = NewObject<UProceduralMeshComponent>(this);
		Component->SetupAttachment(RootComponent);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->RegisterComponent();
		Component->SetWorldLocation(TeinoraConvert::ToUnreal(Mesh.Origin));
	}

	// Whole triangles in one section, the cut-out leaf cards in another with a material of their own.
	FProcMeshSection Solid;
	FProcMeshSection Leaves;
	const int32 Count = static_cast<int32>(Mesh.VertexCount);
	const int32 FirstLeaf = static_cast<int32>(Mesh.SolidVertexCount);
	const int32 LastLeaf = static_cast<int32>(Mesh.ShadowVertexCount);
	Solid.ProcVertexBuffer.Reserve(Count - (LastLeaf - FirstLeaf));
	Solid.ProcIndexBuffer.Reserve(Count - (LastLeaf - FirstLeaf));
	Leaves.ProcVertexBuffer.Reserve(LastLeaf - FirstLeaf);
	Leaves.ProcIndexBuffer.Reserve(LastLeaf - FirstLeaf);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const Teinora::TerrainVertex& Source = Mesh.Vertices[Index];
		const bool bLeaf = Index >= FirstLeaf && Index < LastLeaf;
		FProcMeshSection& Section = bLeaf ? Leaves : Solid;

		FProcMeshVertex Target;
		Target.Position = FVector(Source.Position[0], -Source.Position[1], Source.Position[2]) * TeinoraConvert::UnitsPerMetre;
		Target.Normal = FVector(Source.Normal[0], -Source.Normal[1], Source.Normal[2]) / 127.0;
		Target.Color = FColor(Source.Color[0], Source.Color[1], Source.Color[2], Source.Color[3]);
		Target.UV0 = bLeaf
			? FVector2D(Source.Color[0] / 255.0, Source.Color[1] / 255.0) // where on the card
			: FVector2D(Source.Normal[3] / 127.0, 0.0);                   // which material this is, see ETerrainMaterial
		Target.UV1 = FVector2D(Source.Normal[3] == Teinora::TerrainNeedle ? 1.0 : 0.0, Source.Color[2] / 255.0); // leaf or needle, and how light
		Section.ProcIndexBuffer.Add(Section.ProcVertexBuffer.Add(Target));
		Section.SectionLocalBox += Target.Position;
	}

	Solid.bEnableCollision = false;
	Leaves.bEnableCollision = false;
	Component->SetProcMeshSection(0, Solid);
	Component->SetMaterial(0, TerrainMaterial);
	Component->SetProcMeshSection(1, Leaves);
	Component->SetMaterial(1, LeafMaterial);
}

void ATeinoraWorld::HideChunk(int32 X, int32 Y)
{
	TObjectPtr<UProceduralMeshComponent> Component;
	if (Chunks.RemoveAndCopyValue(MakeChunkKey(X, Y), Component) && Component != nullptr)
	{
		Component->DestroyComponent();
	}
}

UDirectionalLightComponent* ATeinoraWorld::SpawnSkyLight(int32 AtmosphereIndex)
{
	ADirectionalLight* Actor = GetWorld()->SpawnActor<ADirectionalLight>();
	Actor->GetRootComponent()->SetMobility(EComponentMobility::Movable);

	UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Actor->GetLightComponent());
	Light->SetAtmosphereSunLight(true);
	Light->SetAtmosphereSunLightIndex(AtmosphereIndex);
	Light->SetForwardShadingPriority(1 - AtmosphereIndex); // translucency and fog can only follow one light
	Light->SetDynamicShadowDistanceMovableLight(60000.0f);
	Light->SetDynamicShadowCascades(4);
	return Light;
}

void ATeinoraWorld::SpawnSky()
{
	UWorld* Level = GetWorld();

	Sun = SpawnSkyLight(0);
	Sun->SetLightColor(SunTint);
	SecondSun = SpawnSkyLight(1);
	SecondSun->SetLightColor(SecondSunTint);

	// What the planet sends down. It is weak and falls from far up, and casts no shadows.
	ADirectionalLight* PlanetActor = Level->SpawnActor<ADirectionalLight>();
	PlanetActor->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	PlanetLight = Cast<UDirectionalLightComponent>(PlanetActor->GetLightComponent());
	PlanetLight->SetAtmosphereSunLight(false); // or it would turn the night sky blue
	PlanetLight->SetCastShadows(false);
	PlanetLight->SetForwardShadingPriority(-1);

	Level->SpawnActor<ASkyAtmosphere>();
	SpawnSkyDome();

	ASkyLight* SkyActor = Level->SpawnActor<ASkyLight>();
	USkyLightComponent* Sky = SkyActor->GetLightComponent();
	Sky->SetMobility(EComponentMobility::Movable);
	Sky->SetRealTimeCapture(true);

	AExponentialHeightFog* Fog = Level->SpawnActor<AExponentialHeightFog>();
	if (UExponentialHeightFogComponent* FogComponent = Fog->FindComponentByClass<UExponentialHeightFogComponent>())
	{
		FogComponent->SetFogDensity(0.003f);
	}

	AVolumetricCloud* CloudActor = Level->SpawnActor<AVolumetricCloud>();
	Clouds = CloudActor->FindComponentByClass<UVolumetricCloudComponent>();
	Clouds->SetMaterial(LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst")));
}

void ATeinoraWorld::SpawnSkyDome()
{
	StarCells = MakeStarTexture();

	SkyMaterial = UMaterialInstanceDynamic::Create(LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Teinora/M_Sky.M_Sky")), this);
	SkyMaterial->SetTextureParameterValue(TEXT("StarCells"), StarCells);
	SkyMaterial->SetVectorParameterValue(TEXT("StarPole"), ToSkyVector(Teinora::GetSkyPole()));

	// Their light on a white surface, unchanged by anything on the ground. The planet is mostly
	// seen at night, when the camera opens up, so it is matched to the standalone game there.
	SkyMaterial->SetVectorParameterValue(TEXT("SunLight"), SunTint * (SunIntensity / UE_PI * PlanetShine));
	SkyMaterial->SetVectorParameterValue(TEXT("SecondLight"), SecondSunTint * (SecondSunIntensity / UE_PI * PlanetShine));

	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EngineSky/SM_SkySphere.SM_SkySphere"));
	SkyDome = NewObject<UStaticMeshComponent>(this);
	SkyDome->SetupAttachment(RootComponent);
	SkyDome->SetMobility(EComponentMobility::Movable);
	SkyDome->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SkyDome->SetStaticMesh(Sphere);
	SkyDome->SetMaterial(0, SkyMaterial);
	SkyDome->SetCastShadow(false);
	SkyDome->SetVisibleInRayTracing(false);
	SkyDome->SetWorldScale3D(FVector(SkyDomeRadius / Sphere->GetBounds().SphereRadius));
	SkyDome->RegisterComponent();
}

void ATeinoraWorld::CreateWater()
{
	Water = NewObject<UProceduralMeshComponent>(this);
	Water->SetupAttachment(RootComponent);
	Water->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Water->SetCastShadow(false);
	Water->RegisterComponent();

	const int32 Side = WaterCells + 1;
	Sim->WaterVertices.SetNumZeroed(Side * Side);
	Sim->WaterNormals.Init(FVector::UpVector, Side * Side);

	TArray<int32> Triangles;
	Triangles.Reserve(WaterCells * WaterCells * 6);
	for (int32 Y = 0; Y < WaterCells; ++Y)
	{
		for (int32 X = 0; X < WaterCells; ++X)
		{
			const int32 A = X + Y * Side;
			const int32 B = A + 1;
			const int32 C = A + Side;
			const int32 D = C + 1;

			// The same order as the terrain's triangles, so these face up as well.
			Triangles.Append({ A, B, C, B, D, C });
		}
	}

	Water->CreateMeshSection(0, Sim->WaterVertices, Triangles, Sim->WaterNormals, TArray<FVector2D>(), TArray<FColor>(), TArray<FProcMeshTangent>(), false);
	Water->SetMaterial(0, WaterMaterial);
}

void ATeinoraWorld::UpdateWater(const Vec3& Eye)
{
	const int32 Side = WaterCells + 1;
	const double CenterX = FMath::RoundToDouble(Eye.X / WaterSnap) * WaterSnap;
	const double CenterY = FMath::RoundToDouble(Eye.Y / WaterSnap) * WaterSnap;

	TArray<FVector>& Vertices = Sim->WaterVertices;
	for (int32 Y = 0; Y < Side; ++Y)
	{
		const double WorldY = CenterY + SpreadWater(Y);
		for (int32 X = 0; X < Side; ++X)
		{
			const double WorldX = CenterX + SpreadWater(X);
			const double Moved = Sim->World.GetLiquid().GetHeight(WorldX, WorldY);
			Vertices[X + Y * Side] = TeinoraConvert::ToUnreal(Vec3{ WorldX, WorldY, Sim::WaterHeight(WorldX, WorldY, Sim->Time) + Moved });
		}
	}

	for (int32 Y = 0; Y < Side; ++Y)
	{
		const int32 Below = FMath::Max(Y - 1, 0);
		const int32 Above = FMath::Min(Y + 1, WaterCells);
		for (int32 X = 0; X < Side; ++X)
		{
			const FVector AlongX = Vertices[FMath::Min(X + 1, WaterCells) + Y * Side] - Vertices[FMath::Max(X - 1, 0) + Y * Side];
			const FVector AlongY = Vertices[X + Above * Side] - Vertices[X + Below * Side];
			Sim->WaterNormals[X + Y * Side] = FVector::CrossProduct(AlongX, AlongY).GetSafeNormal() * -1.0;
		}
	}

	Water->UpdateMeshSection(0, Vertices, Sim->WaterNormals, TArray<FVector2D>(), TArray<FColor>(), TArray<FProcMeshTangent>());
}

void ATeinoraWorld::UpdateLakes(const Vec3& Eye)
{
	constexpr int32 Rings = 28;
	constexpr int32 Segments = 72;

	std::vector<Sim::Lake> Near = Sim::FindLakes(Eye.X, Eye.Y, Sim->World.GetViewDistance(), Sim::WorldSeed);

	TSet<uint32> Wanted;
	for (const Sim::Lake& Item : Near)
	{
		Wanted.Add(Item.Id);
		if (Lakes.Contains(Item.Id))
		{
			continue;
		}

		// A still sheet the shape of what holds the water. The ground around it hides all but the lake.
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		Vertices.Add(TeinoraConvert::ToUnreal(Vec3{ Item.X, Item.Y, Item.Level }));
		for (int32 Ring = 1; Ring <= Rings; ++Ring)
		{
			const double Reach = Item.Radius * Sim::Lake::WaterEdge * Ring / Rings;
			for (int32 Segment = 0; Segment < Segments; ++Segment)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * Segment / Segments;
				const double Along = FMath::Cos(Angle) * Reach * Item.Stretch;
				const double Across = FMath::Sin(Angle) * Reach;
				Vertices.Add(TeinoraConvert::ToUnreal(Vec3{ Item.X + Along * FMath::Cos(Item.Angle) - Across * FMath::Sin(Item.Angle),
					Item.Y + Along * FMath::Sin(Item.Angle) + Across * FMath::Cos(Item.Angle), Item.Level }));
			}
		}

		// The same way round as the sea's grid, so that these face up as well.
		const auto At = [](int32 Ring, int32 Segment) { return 1 + (Ring - 1) * Segments + (Segment + Segments) % Segments; };
		for (int32 Segment = 0; Segment < Segments; ++Segment)
		{
			Triangles.Append({ 0, At(1, Segment), At(1, Segment + 1) });
		}
		for (int32 Ring = 1; Ring < Rings; ++Ring)
		{
			for (int32 Segment = 0; Segment < Segments; ++Segment)
			{
				Triangles.Append({ At(Ring, Segment), At(Ring + 1, Segment), At(Ring, Segment + 1), At(Ring, Segment + 1), At(Ring + 1, Segment), At(Ring + 1, Segment + 1) });
			}
		}

		UProceduralMeshComponent* Sheet = NewObject<UProceduralMeshComponent>(this);
		Sheet->SetupAttachment(RootComponent);
		Sheet->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Sheet->SetCastShadow(false);
		Sheet->RegisterComponent();

		TArray<FVector> Ups;
		Ups.Init(FVector::UpVector, Vertices.Num());
		Sheet->CreateMeshSection(0, Vertices, Triangles, Ups, TArray<FVector2D>(), TArray<FColor>(), TArray<FProcMeshTangent>(), false);
		Sheet->SetMaterial(0, WaterMaterial);
		Lakes.Add(Item.Id, Sheet);
	}

	for (auto It = Lakes.CreateIterator(); It; ++It)
	{
		if (!Wanted.Contains(It.Key()))
		{
			It.Value()->DestroyComponent();
			It.RemoveCurrent();
		}
	}
}

void ATeinoraWorld::UpdateSky(ATeinoraCamera* Eye)
{
	const Vec3 Towards = Teinora::GetSunDirection(Sim->Hour, Sim->Day);
	const Vec3 TowardsSecond = Teinora::GetSecondSunDirection(Sim->Hour, Sim->Day);
	const Vec3 Planet = Teinora::GetPlanetDirection(Sim->Hour, Sim->Day);

	const float Height = static_cast<float>(Towards.Z);
	const float SecondHeight = static_cast<float>(TowardsSecond.Z);
	const double Days = Sim->Day + Sim->Hour / 24.0;
	const Vec3 PlanetAxis = Teinora::GetPlanetAxis(Days);
	const float Past = static_cast<float>(Teinora::GetLightPastPlanet(Towards, Planet, PlanetAxis, Teinora::SunSize));
	const float SecondPast = static_cast<float>(Teinora::GetLightPastPlanet(TowardsSecond, Planet, PlanetAxis, Teinora::SecondSunSize));
	const float SunUp = FMath::SmoothStep(-0.02f, 0.03f, Height) * Past;
	const float SecondUp = FMath::SmoothStep(-0.02f, 0.03f, SecondHeight) * SecondPast;

	Sun->SetWorldRotation((TeinoraConvert::DirectionToUnreal(Towards) * -1.0).Rotation());
	Sun->SetIntensity(SunIntensity * SunUp);
	Sun->SetVisibility(SunUp > 0.0f);

	SecondSun->SetWorldRotation((TeinoraConvert::DirectionToUnreal(TowardsSecond) * -1.0).Rotation());
	SecondSun->SetIntensity(SecondSunIntensity * SecondUp);
	SecondSun->SetVisibility(SecondUp > 0.0f);

	// The planet sends down a share of what falls on the side of it facing the ground.
	const float Facing = 0.5f - 0.5f * static_cast<float>(Teinora::Dot(Planet, Towards));
	const float SecondFacing = 0.5f - 0.5f * static_cast<float>(Teinora::Dot(Planet, TowardsSecond));
	const float PlanetUp = FMath::SmoothStep(-0.02f, 0.03f, static_cast<float>(Planet.Z)) * PlanetAlbedo;
	FLinearColor Reflected = (SunTint * (SunIntensity * Facing) + SecondSunTint * (SecondSunIntensity * SecondFacing)) * PlanetUp;
	Reflected *= FLinearColor(0.8f, 0.95f, 1.25f);
	const float ReflectedPeak = Reflected.GetMax();
	PlanetLight->SetVisibility(ReflectedPeak > 0.0f);
	if (ReflectedPeak > 0.0f)
	{
		PlanetLight->SetWorldRotation((TeinoraConvert::DirectionToUnreal(Planet) * -1.0).Rotation());
		PlanetLight->SetLightColor(Reflected / ReflectedPeak);
		PlanetLight->SetIntensity(ReflectedPeak);
	}

	SkyMaterial->SetVectorParameterValue(TEXT("SunWay"), ToSkyVector(Towards));
	SkyMaterial->SetVectorParameterValue(TEXT("SecondWay"), ToSkyVector(TowardsSecond));
	SkyMaterial->SetVectorParameterValue(TEXT("PlanetWay"), ToSkyVector(Planet));
	SkyMaterial->SetVectorParameterValue(TEXT("PlanetAxis"), ToSkyVector(PlanetAxis));
	SkyMaterial->SetScalarParameterValue(TEXT("Night"), GetNight(Height, SecondHeight, Past, SecondPast));
	SkyMaterial->SetScalarParameterValue(TEXT("SkyTime"), static_cast<float>(Sim->Time));
	SkyMaterial->SetScalarParameterValue(TEXT("StarTurn"), static_cast<float>(FMath::Fmod(Days, 1.0) * 2.0 * UE_DOUBLE_PI));
	SkyMaterial->SetScalarParameterValue(TEXT("PlanetSpin"), static_cast<float>(Teinora::GetPlanetSpin(Days)));

	if (Eye != nullptr)
	{
		SkyDome->SetWorldLocation(TeinoraConvert::ToUnreal(Eye->GetEye()));
		Eye->SetExposure(FMath::Lerp(2.6f, 1.0f, FMath::SmoothStep(-0.2f, 0.02f, FMath::Max(Height, SecondHeight))));
	}
}

void ATeinoraWorld::UpdateBodies()
{
	Sim->Objects.clear();
	Sim->World.CollectObjects(Sim->Objects);

	for (FBodyGroup& Group : Sim->Groups)
	{
		Group.Transforms.Reset();
	}

	for (const Teinora::ObjectDraw& Item : Sim->Objects)
	{
		const FLinearColor Color(Item.Color.X, Item.Color.Y, Item.Color.Z);
		FBodyGroup* Group = Sim->Groups.FindByPredicate([&Item, &Color](const FBodyGroup& Other)
		{
			return Other.Primitive == Item.Primitive && Other.Color == Color;
		});

		if (Group == nullptr)
		{
			UInstancedStaticMeshComponent* Mesh = NewObject<UInstancedStaticMeshComponent>(this);
			Mesh->SetupAttachment(RootComponent);
			Mesh->SetMobility(EComponentMobility::Movable);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->SetStaticMesh(Primitives[static_cast<int32>(Item.Primitive)]);
			Mesh->RegisterComponent();

			UMaterialInstanceDynamic* Tint = UMaterialInstanceDynamic::Create(BodyMaterial, Mesh);
			Tint->SetVectorParameterValue(TEXT("Tint"), Color);
			Mesh->SetMaterial(0, Tint);

			Group = &Sim->Groups.AddDefaulted_GetRef();
			Group->Primitive = Item.Primitive;
			Group->Color = Color;
			Group->Mesh = Mesh;
		}

		Group->Transforms.Emplace(TeinoraConvert::ToUnreal(Item.Rotation), TeinoraConvert::ToUnreal(Item.Position), FVector(Item.Scale.X, Item.Scale.Y, Item.Scale.Z));
	}

	for (FBodyGroup& Group : Sim->Groups)
	{
		if (Group.Mesh->GetInstanceCount() == Group.Transforms.Num())
		{
			Group.Mesh->BatchUpdateInstancesTransforms(0, Group.Transforms, true, true, true);
		}
		else
		{
			Group.Mesh->ClearInstances();
			Group.Mesh->AddInstances(Group.Transforms, false, true, false);
		}
	}
}

void ATeinoraWorld::UpdateMagic(ATeinoraCamera& Eye, float DeltaSeconds)
{
	if (Sim->Magic == nullptr)
	{
		return;
	}

	Sim->Magic->Advance(Eye.GetEye(), Sim->World.GetAirDrift(), Sim->Time);
	if (Sim->Magic->TakeNear(Sim->NearMagic))
	{
		UploadMagicAtlas(MagicNearAtlas, Sim->NearMagic, 8, 6);
	}
	if (Sim->Magic->TakeFar(Sim->FarMagic))
	{
		UploadMagicAtlas(MagicFarAtlas, Sim->FarMagic, 4, 3);
	}
	MagicFade += ((bMagicView ? 1.0f : 0.0f) - MagicFade) * FMath::Min(DeltaSeconds * 7.0f, 1.0f);

	if (MagicNearAtlas != nullptr && MagicFarAtlas != nullptr)
	{
		const Teinora::MagicVolume& Near = Sim->NearMagic;
		const Teinora::MagicVolume& Far = Sim->FarMagic;
		Eye.ShowMagic(MagicFade, Near.PeakDensity > Sim::MagicVisibleDensity, MagicNearAtlas, MagicFarAtlas,
			Near.Corner, Vec3{ Near.SizeX * Near.CellSize, Near.SizeY * Near.CellSize, Near.SizeZ * Near.CellSize },
			Far.Corner, Vec3{ Far.SizeX * Far.CellSize, Far.SizeY * Far.CellSize, Far.SizeZ * Far.CellSize }, Sim->Time);
	}
}

void ATeinoraWorld::RunAutomation()
{
	if (AutoFrames <= 0 || !Sim->World.IsSettled() || (Sim->Magic != nullptr && !Sim->Magic->IsReady()))
	{
		return;
	}

	// Materials compile in the background, and a frame without them proves nothing.
#if WITH_EDITOR
	if (GShaderCompilingManager != nullptr && GShaderCompilingManager->IsCompiling())
	{
		return;
	}
#endif

	const APlayerController* Player = GetWorld()->GetFirstPlayerController();
	const ATeinoraCamera* Eye = Player != nullptr ? Cast<ATeinoraCamera>(Player->GetPawn()) : nullptr;
	if (ThrowsLeft > 0 && Eye != nullptr)
	{
		--ThrowsLeft;
		ThrowBall(Eye->GetEye(), Eye->GetLineOfSight(), Sim::MaxThrowSpeed);
	}

	++SettledFrames;
	if (SettledFrames == AutoFrames && !ShotPath.IsEmpty())
	{
		FScreenshotRequest::RequestScreenshot(ShotPath, true, false); // with the menu, if it is open
	}
	if (SettledFrames == AutoFrames + 10)
	{
		const FTeinoraWorldStats Stats = GetStats();
		UE_LOG(LogTemp, Display, TEXT("Finished: %d bodies, %d awake, %d contacts"), Stats.Bodies, Stats.Awake, Stats.Contacts);
		FPlatformMisc::RequestExit(false, TEXT("TeinoraAutomation"));
	}
}

void ATeinoraWorld::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bPaused)
	{
		Sim->Accumulator = FMath::Min(Sim->Accumulator + DeltaSeconds, PhysicsStep * MaxPhysicsSteps);
		while (Sim->Accumulator >= PhysicsStep)
		{
			Sim->World.Step(PhysicsStep);
			Sim->Time += PhysicsStep;
			Sim->Accumulator -= PhysicsStep;
		}
		Sim->Hour += DeltaSeconds / DayLength * 24.0 * TimeRate;
		if (Sim->Hour >= 24.0)
		{
			Sim->Hour -= 24.0;
			Sim->Day += 1.0;
		}
	}

	const APlayerController* Player = GetWorld()->GetFirstPlayerController();
	ATeinoraCamera* Eye = Player != nullptr ? Cast<ATeinoraCamera>(Player->GetPawn()) : nullptr;
	if (Eye != nullptr)
	{
		const Vec3 Here = Eye->GetEye();
		Sim->World.Update(Here, Sim->Time);
		UpdateWater(Here);
		UpdateLakes(Here);
		UpdateMagic(*Eye, DeltaSeconds);

		const double WaterTop = Sim::WaterSurfaceAt(Here.X, Here.Y, Sim->Time, Sim::WorldSeed) + Sim->World.GetLiquid().GetHeight(Here.X, Here.Y);
		const bool bUnder = Here.Z < WaterTop && Sim::TerrainHeight(Here.X, Here.Y, Sim::WorldSeed) < WaterTop;
		Eye->SetUnderwater(bUnder ? WaterTop - Here.Z : 0.0);
	}

	UpdateBodies();
	UpdateSky(Eye);
	RunAutomation();
}
