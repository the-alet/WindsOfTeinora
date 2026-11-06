#include "Physics/Physics.h"

#include "Core/Log.h"

#include <PxPhysicsAPI.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace Teinora::Physics
{
	using namespace physx;

	namespace
	{
		constexpr PxU32 WorkerThreads = 2;
		constexpr float MaxDepenetrationSpeed = 10.0f;
		constexpr float HeightSteps = 60000.0f; // of the 16 bits a height sample has

		PxVec3 ToPx(const Vec3& V)
		{
			return PxVec3(static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z));
		}

		PxQuat ToPx(const Quat& Q)
		{
			return PxQuat(static_cast<float>(Q.X), static_cast<float>(Q.Y), static_cast<float>(Q.Z), static_cast<float>(Q.W)).getNormalized();
		}

		Vec3 FromPx(const PxVec3& V)
		{
			return { V.x, V.y, V.z };
		}

		Quat FromPx(const PxQuat& Q)
		{
			return { Q.x, Q.y, Q.z, Q.w };
		}

		class ErrorLog final : public PxErrorCallback
		{
		public:
			void reportError(PxErrorCode::Enum Code, const char* Message, const char* File, int Line) override
			{
				Log("PhysX error %d: %s (%s:%d)", static_cast<int>(Code), Message, File, Line);
			}
		};

		/** One SDK per process, however many worlds there are. */
		struct Sdk
		{
			PxDefaultAllocator Allocator;
			ErrorLog Errors;
			PxFoundation* Foundation = nullptr;
			PxPhysics* Physics = nullptr;

			Sdk()
			{
				Foundation = PxCreateFoundation(PX_PHYSICS_VERSION, Allocator, Errors);
				Physics = PxCreatePhysics(PX_PHYSICS_VERSION, *Foundation, PxTolerancesScale());
			}

			~Sdk()
			{
				Physics->release();
				Foundation->release();
			}
		};

		std::shared_ptr<Sdk> AcquireSdk()
		{
			static std::weak_ptr<Sdk> Shared;
			std::shared_ptr<Sdk> Result = Shared.lock();
			if (Result == nullptr)
			{
				Result = std::make_shared<Sdk>();
				Shared = Result;
			}
			return Result;
		}

		// Every pair collides, and fast bodies are swept so they cannot pass through thin things.
		PxFilterFlags FilterContacts(PxFilterObjectAttributes, PxFilterData, PxFilterObjectAttributes, PxFilterData, PxPairFlags& Flags, const void*, PxU32)
		{
			Flags = PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eDETECT_CCD_CONTACT;
			return PxFilterFlag::eDEFAULT;
		}

		void AddCollider(PxRigidActor& Actor, const Shape& Collider, const PxMaterial& Material)
		{
			switch (Collider.Type)
			{
			case EShape::Sphere:
				PxRigidActorExt::createExclusiveShape(Actor, PxSphereGeometry(static_cast<float>(Collider.Radius)), Material);
				break;
			case EShape::Box:
				PxRigidActorExt::createExclusiveShape(Actor, PxBoxGeometry(ToPx(Collider.HalfExtents)), Material);
				break;
			case EShape::Capsule:
			{
				// A PhysX capsule lies along its X; ours stands along Z.
				PxShape* Created = PxRigidActorExt::createExclusiveShape(Actor,
					PxCapsuleGeometry(static_cast<float>(Collider.Radius), static_cast<float>(Collider.HalfLength)), Material);
				Created->setLocalPose(PxTransform(PxQuat(PxHalfPi, PxVec3(0.0f, -1.0f, 0.0f))));
				break;
			}
			}
		}
	}

	Shape Shape::MakeSphere(double Radius)
	{
		Shape Result;
		Result.Type = EShape::Sphere;
		Result.Radius = Radius;
		return Result;
	}

	Shape Shape::MakeCapsule(double Radius, double HalfLength)
	{
		Shape Result;
		Result.Type = EShape::Capsule;
		Result.Radius = Radius;
		Result.HalfLength = HalfLength;
		return Result;
	}

	Shape Shape::MakeBox(const Vec3& HalfExtents)
	{
		Shape Result;
		Result.Type = EShape::Box;
		Result.HalfExtents = HalfExtents;
		return Result;
	}

	double Shape::GetVolume() const
	{
		const double Ball = 4.0 / 3.0 * Pi * Radius * Radius * Radius;
		switch (Type)
		{
		case EShape::Sphere:  return Ball;
		case EShape::Capsule: return Ball + Pi * Radius * Radius * HalfLength * 2.0;
		default:              return 8.0 * HalfExtents.X * HalfExtents.Y * HalfExtents.Z;
		}
	}

	struct World::Scene
	{
		struct GroundSlot
		{
			PxRigidStatic* Actor = nullptr;
			PxRigidStatic* Floor = nullptr; // firm ground under a soft surface
			PxHeightField* Field = nullptr;
		};

		std::shared_ptr<Sdk> Core = AcquireSdk();
		PxDefaultCpuDispatcher* Dispatcher = nullptr;
		PxScene* Handle = nullptr;
		std::map<std::pair<float, float>, PxMaterial*> Materials;
		std::vector<PxMaterial*> SoftMaterials;
		std::vector<PxMaterial*> Surfaces;
		PxMaterial* FloorMaterial = nullptr; // when any of the surfaces is soft
		std::vector<PxRigidActor*> Actors; // by BodyId
		std::vector<GroundSlot> Grounds;
		std::vector<GroundId> FreeGrounds;

		Scene()
		{
			PxPhysics& Sdk = *Core->Physics;
			Dispatcher = PxDefaultCpuDispatcherCreate(WorkerThreads);

			PxSceneDesc Desc(Sdk.getTolerancesScale());
			Desc.gravity = PxVec3(0.0f, 0.0f, -9.81f);
			Desc.cpuDispatcher = Dispatcher;
			Desc.filterShader = FilterContacts;
			Desc.solverType = PxSolverType::ePGS; // TGS loses the restitution of slow collisions
			Desc.flags |= PxSceneFlag::eENABLE_CCD;
			Handle = Sdk.createScene(Desc);
		}

		~Scene()
		{
			for (PxRigidActor* Actor : Actors)
			{
				if (Actor != nullptr)
				{
					Actor->release();
				}
			}
			for (GroundSlot& Slot : Grounds)
			{
				if (Slot.Actor != nullptr)
				{
					Slot.Actor->release();
					if (Slot.Floor != nullptr)
					{
						Slot.Floor->release();
					}
					Slot.Field->release();
				}
			}
			Handle->release();
			Dispatcher->release();
			for (const auto& [Key, Material] : Materials)
			{
				Material->release();
			}
			for (PxMaterial* Material : SoftMaterials)
			{
				Material->release();
			}
		}

		PxMaterial* GetMaterial(double Friction, double Restitution)
		{
			const std::pair<float, float> Key(static_cast<float>(Friction), static_cast<float>(Restitution));
			PxMaterial*& Found = Materials[Key];
			if (Found == nullptr)
			{
				Found = Core->Physics->createMaterial(Key.first, Key.first, Key.second);
				Found->setRestitutionCombineMode(PxCombineMode::eMAX); // a ball keeps its bounce on bare ground
			}
			return Found;
		}

		/**
		 * A negative restitution is PhysX's way of asking for a spring in place of a rigid contact.
		 * Reckoned as an acceleration, it treats a ball and a boulder alike, and it takes
		 * over from the restitution of whatever lands on it.
		 */
		PxMaterial* MakeSoftMaterial(const SurfaceMaterial& Surface)
		{
			PxMaterial* Made = Core->Physics->createMaterial(static_cast<float>(Surface.Friction), static_cast<float>(Surface.Friction), -static_cast<float>(Surface.Stiffness));
			Made->setDamping(static_cast<float>(Surface.Damping));
			Made->setFlag(PxMaterialFlag::eCOMPLIANT_ACCELERATION_SPRING, true);
			Made->setRestitutionCombineMode(PxCombineMode::eMIN);
			SoftMaterials.push_back(Made);
			return Made;
		}
	};

	World::World() : Sim(std::make_unique<Scene>())
	{
		const double Default = 0.8;
		SetSurfaceFrictions(&Default, 1);
	}

	World::~World() = default;

	BodyId World::CreateBody(const BodyDesc& Desc)
	{
		PxPhysics& Sdk = *Sim->Core->Physics;
		const PxTransform Pose(ToPx(Desc.Position), ToPx(Desc.Rotation));
		PxMaterial* Material = Sim->GetMaterial(Desc.Friction, Desc.Restitution);

		PxRigidActor* Actor = nullptr;
		if (Desc.Mass > 0.0)
		{
			PxRigidDynamic* Dynamic = Sdk.createRigidDynamic(Pose);
			AddCollider(*Dynamic, Desc.Collider, *Material);
			PxRigidBodyExt::setMassAndUpdateInertia(*Dynamic, static_cast<float>(Desc.Mass));
			Dynamic->setLinearVelocity(ToPx(Desc.Velocity));
			Dynamic->setLinearDamping(static_cast<float>(Desc.LinearDamping));
			Dynamic->setAngularDamping(static_cast<float>(Desc.AngularDamping));
			Dynamic->setMaxDepenetrationVelocity(MaxDepenetrationSpeed);
			Dynamic->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
			Actor = Dynamic;
		}
		else
		{
			PxRigidStatic* Static = Sdk.createRigidStatic(Pose);
			AddCollider(*Static, Desc.Collider, *Material);
			Actor = Static;
		}

		BodyId Id;
		if (!FreeList.empty())
		{
			Id = FreeList.back();
			FreeList.pop_back();
		}
		else
		{
			Id = static_cast<BodyId>(Bodies.size());
			Bodies.emplace_back();
			Sim->Actors.push_back(nullptr);
		}

		Actor->userData = reinterpret_cast<void*>(static_cast<std::intptr_t>(Id + 1));
		Sim->Handle->addActor(*Actor);
		Sim->Actors[static_cast<std::size_t>(Id)] = Actor;

		Body& Item = GetBody(Id);
		Item = {};
		Item.Collider = Desc.Collider;
		Item.Position = Desc.Position;
		Item.Rotation = Desc.Rotation;
		Item.Velocity = Desc.Velocity;
		Item.Mass = Desc.Mass;
		Item.bAlive = true;

		if (Desc.Mass > 0.0)
		{
			Dynamics.push_back(Id);
		}
		return Id;
	}

	void World::DestroyBody(BodyId Id)
	{
		PxRigidActor*& Actor = Sim->Actors[static_cast<std::size_t>(Id)];
		Actor->release(); // this also wakes whatever rested on it
		Actor = nullptr;

		if (!GetBody(Id).IsStatic())
		{
			std::erase(Dynamics, Id);
		}
		GetBody(Id).bAlive = false;
		FreeList.push_back(Id);
	}

	void World::SetSurfaceFrictions(const double* Frictions, int Count)
	{
		Sim->Surfaces.clear();
		Sim->FloorMaterial = nullptr;
		for (int Index = 0; Index < Count; ++Index)
		{
			Sim->Surfaces.push_back(Sim->GetMaterial(Frictions[Index], 0.0));
		}
	}

	void World::SetSurfaces(const SurfaceMaterial* Surfaces, int Count)
	{
		Sim->Surfaces.clear();
		Sim->FloorMaterial = nullptr;
		for (int Index = 0; Index < Count; ++Index)
		{
			const SurfaceMaterial& Surface = Surfaces[Index];
			if (Surface.Stiffness > 0.0)
			{
				Sim->Surfaces.push_back(Sim->MakeSoftMaterial(Surface));
				Sim->FloorMaterial = Sim->GetMaterial(Surface.Friction, 0.0);
			}
			else
			{
				Sim->Surfaces.push_back(Sim->GetMaterial(Surface.Friction, Surface.Restitution));
			}
		}
	}

	GroundId World::AddGround(const GroundDesc& Desc)
	{
		const int Side = Desc.Cells + 1;
		const std::size_t Count = static_cast<std::size_t>(Side) * Side;
		const auto [Lowest, Highest] = std::minmax_element(Desc.Heights, Desc.Heights + Count);
		const float Middle = (*Lowest + *Highest) * 0.5f;
		const float Scale = std::max((*Highest - *Lowest) / HeightSteps, 1e-4f);

		// Sample rows run along world Y and columns along world X, like the heights given.
		std::vector<PxHeightFieldSample> Samples(Count);
		for (int Row = 0; Row < Side; ++Row)
		{
			for (int Column = 0; Column < Side; ++Column)
			{
				const std::size_t Index = static_cast<std::size_t>(Row) * Side + Column;
				PxHeightFieldSample& Sample = Samples[Index];
				Sample.height = static_cast<PxI16>(std::lround((Desc.Heights[Index] - Middle) / Scale));
				if (Row < Desc.Cells && Column < Desc.Cells)
				{
					const std::size_t Cell = (static_cast<std::size_t>(Row) * Desc.Cells + Column) * 2;
					Sample.materialIndex0 = PxBitAndByte(Desc.Surfaces[Cell]);
					Sample.materialIndex1 = PxBitAndByte(Desc.Surfaces[Cell + 1]);
				}
			}
		}

		PxHeightFieldDesc FieldDesc;
		FieldDesc.nbRows = static_cast<PxU32>(Side);
		FieldDesc.nbColumns = static_cast<PxU32>(Side);
		FieldDesc.samples.data = Samples.data();
		FieldDesc.samples.stride = sizeof(PxHeightFieldSample);
		FieldDesc.flags = PxHeightFieldFlag::eNO_BOUNDARY_EDGES; // bodies roll over the seams between chunks

		PxPhysics& Sdk = *Sim->Core->Physics;
		PxHeightField* Field = PxCreateHeightField(FieldDesc, Sdk.getPhysicsInsertionCallback());

		// PhysX puts rows along its X, height along its Y and columns along its Z.
		// A third of a turn about (1, 1, 1) sends those to world Y, Z and X.
		const PxTransform Pose(PxVec3(static_cast<float>(Desc.OriginX), static_cast<float>(Desc.OriginY), Middle), PxQuat(0.5f, 0.5f, 0.5f, 0.5f));
		PxRigidStatic* Actor = Sdk.createRigidStatic(Pose);
		const float Cell = static_cast<float>(Desc.CellSize);
		PxRigidActorExt::createExclusiveShape(*Actor, PxHeightFieldGeometry(Field, PxMeshGeometryFlags(), Scale, Cell, Cell),
			Sim->Surfaces.data(), static_cast<PxU16>(Sim->Surfaces.size()));
		Sim->Handle->addActor(*Actor);

		PxRigidStatic* Floor = nullptr;
		if (Sim->FloorMaterial != nullptr)
		{
			Floor = Sdk.createRigidStatic(PxTransform(Pose.p - PxVec3(0.0f, 0.0f, static_cast<float>(SoftLayerDepth)), Pose.q));
			PxShape* Shape = PxRigidActorExt::createExclusiveShape(*Floor, PxHeightFieldGeometry(Field, PxMeshGeometryFlags(), Scale, Cell, Cell), *Sim->FloorMaterial);
			Shape->setFlag(PxShapeFlag::eSCENE_QUERY_SHAPE, false); // rays stop at the top of the ground
			Sim->Handle->addActor(*Floor);
		}

		GroundId Id;
		if (!Sim->FreeGrounds.empty())
		{
			Id = Sim->FreeGrounds.back();
			Sim->FreeGrounds.pop_back();
		}
		else
		{
			Id = static_cast<GroundId>(Sim->Grounds.size());
			Sim->Grounds.emplace_back();
		}
		Sim->Grounds[static_cast<std::size_t>(Id)] = { Actor, Floor, Field };
		return Id;
	}

	void World::RemoveGround(GroundId Id)
	{
		Scene::GroundSlot& Slot = Sim->Grounds[static_cast<std::size_t>(Id)];
		Slot.Actor->release();
		if (Slot.Floor != nullptr)
		{
			Slot.Floor->release();
		}
		Slot.Field->release();
		Slot = {};
		Sim->FreeGrounds.push_back(Id);
	}

	void World::SetGravity(const Vec3& Gravity)
	{
		Sim->Handle->setGravity(ToPx(Gravity));
	}

	void World::WakeBody(BodyId Id)
	{
		static_cast<PxRigidDynamic*>(Sim->Actors[static_cast<std::size_t>(Id)])->wakeUp();
	}

	void World::Step(double Dt)
	{
		for (const BodyId Id : Dynamics)
		{
			PxRigidDynamic* Dynamic = static_cast<PxRigidDynamic*>(Sim->Actors[static_cast<std::size_t>(Id)]);
			if (Dynamic->isSleeping())
			{
				continue;
			}

			Body& Item = GetBody(Id);
			Item.Force = {};
			Item.AngularAcceleration = {};
			if (OnForces)
			{
				OnForces(Id, Item, Dt);
			}

			// Without waking: a force of any size would otherwise keep the body from ever sleeping.
			Dynamic->addForce(ToPx(Item.Force), PxForceMode::eFORCE, false);
			Dynamic->addTorque(ToPx(Item.AngularAcceleration), PxForceMode::eACCELERATION, false);
		}

		Sim->Handle->simulate(static_cast<float>(Dt));
		Sim->Handle->fetchResults(true);

		AwakeCount = 0;
		for (const BodyId Id : Dynamics)
		{
			const PxRigidDynamic* Dynamic = static_cast<const PxRigidDynamic*>(Sim->Actors[static_cast<std::size_t>(Id)]);
			const PxTransform Pose = Dynamic->getGlobalPose();

			Body& Item = GetBody(Id);
			Item.Position = FromPx(Pose.p);
			Item.Rotation = FromPx(Pose.q);
			Item.Velocity = FromPx(Dynamic->getLinearVelocity());
			Item.AngularVelocity = FromPx(Dynamic->getAngularVelocity());
			Item.bAsleep = Dynamic->isSleeping();
			AwakeCount += Item.bAsleep ? 0 : 1;
		}

		PxSimulationStatistics Statistics;
		Sim->Handle->getSimulationStatistics(Statistics);
		ContactCount = Statistics.nbDiscreteContactPairsWithContacts;
	}

	bool World::Raycast(const Vec3& Origin, const Vec3& Direction, double MaxDistance, RayHit& Out) const
	{
		PxRaycastBuffer Hit;
		if (!Sim->Handle->raycast(ToPx(Origin), ToPx(Direction).getNormalized(), static_cast<float>(MaxDistance), Hit) || !Hit.hasBlock)
		{
			return false;
		}

		Out.Point = FromPx(Hit.block.position);
		Out.Normal = FromPx(Hit.block.normal);
		Out.Distance = Hit.block.distance;
		Out.Target = static_cast<BodyId>(reinterpret_cast<std::intptr_t>(Hit.block.actor->userData)) - 1;
		return true;
	}
}
