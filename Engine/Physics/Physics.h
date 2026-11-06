// Rigid bodies on NVIDIA PhysX: real masses and inertia, contacts, friction and
// restitution over height-field ground, advanced at a fixed time step. PhysX stays
// behind this header, so the game only sees SI units and its own Z-up types.
#pragma once

#include "Core/Math.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Teinora::Physics
{
	using BodyId = std::int32_t;
	using GroundId = std::int32_t;
	inline constexpr BodyId NoBody = -1;
	inline constexpr GroundId NoGround = -1;

	enum class EShape : std::uint8_t
	{
		Sphere,
		Capsule,
		Box
	};

	struct Shape
	{
		EShape Type = EShape::Sphere;
		double Radius = 0.5;     // sphere and capsule
		double HalfLength = 0.0; // capsule: half the distance between the cap centres, along local Z
		Vec3 HalfExtents;        // box

		static Shape MakeSphere(double Radius);
		static Shape MakeCapsule(double Radius, double HalfLength);
		static Shape MakeBox(const Vec3& HalfExtents);

		double GetVolume() const;
	};

	struct BodyDesc
	{
		Shape Collider;
		double Mass = 0.0; // zero makes the body static; inertia follows from the mass and the shape
		Vec3 Position;
		Quat Rotation;
		Vec3 Velocity;
		double Friction = 0.6;
		double Restitution = 0.1;
		double LinearDamping = 0.0; // air is the game's to model, through forces
		double AngularDamping = 0.05;
	};

	/** A body as of the last step. */
	struct Body
	{
		Shape Collider;
		Vec3 Position;
		Quat Rotation;
		Vec3 Velocity;
		Vec3 AngularVelocity;
		Vec3 Force;               // set by the force callback, for the coming step
		Vec3 AngularAcceleration; // likewise; independent of the inertia, which suits damping
		double Mass = 0.0;
		bool bAsleep = false;
		bool bAlive = false;

		bool IsStatic() const { return Mass == 0.0; }
		double GetMass() const { return Mass; }
	};

	/** A square of terrain. Triangles split along the diagonal from (X + 1, Y) to (X, Y + 1). */
	struct GroundDesc
	{
		double OriginX = 0.0;
		double OriginY = 0.0;
		double CellSize = 1.0;
		int Cells = 0;                          // along a side
		const float* Heights = nullptr;         // (Cells + 1)^2 values, X first, then Y
		const std::uint8_t* Surfaces = nullptr; // two per cell, as indices into the surfaces set on the world
	};

	/** What a kind of ground is like to land on. */
	struct SurfaceMaterial
	{
		double Friction = 0.8;
		double Stiffness = 0.0;   // 1/s^2, per unit of the mass landing on it. Zero is rigid ground.
		double Damping = 0.0;     // 1/s, the same
		double Restitution = 0.0; // of rigid ground; soft ground gives back what its spring does
	};

	// However soft the top, there is firm ground this far under it. A spring on its own lets
	// a fast body through to the far side of the surface, and then nothing stops it falling.
	inline constexpr double SoftLayerDepth = 0.25;

	struct RayHit
	{
		Vec3 Point;
		Vec3 Normal;
		double Distance = 0.0;
		BodyId Target = NoBody; // NoBody means the ground
	};

	class World
	{
	public:
		using ForceCallback = std::function<void(BodyId, Body&, double)>;

		World();
		~World();
		World(const World&) = delete;
		World& operator=(const World&) = delete;

		BodyId CreateBody(const BodyDesc& Desc);
		void DestroyBody(BodyId Id);

		Body& GetBody(BodyId Id) { return Bodies[static_cast<std::size_t>(Id)]; }
		const Body& GetBody(BodyId Id) const { return Bodies[static_cast<std::size_t>(Id)]; }

		/** Friction of each ground surface, all of them rigid. Applies to ground added after the call. */
		void SetSurfaceFrictions(const double* Frictions, int Count);

		/** Friction and softness of each ground surface. A body sinks into soft ground and comes back with what the damping leaves it. */
		void SetSurfaces(const SurfaceMaterial* Surfaces, int Count);

		GroundId AddGround(const GroundDesc& Desc);
		void RemoveGround(GroundId Id);

		/** Runs for every awake dynamic body at each step. */
		void SetForceCallback(ForceCallback Callback) { OnForces = std::move(Callback); }

		void SetGravity(const Vec3& Gravity);
		void WakeBody(BodyId Id);

		/** Advances by exactly Dt. Call it at a fixed rate for stable contacts. */
		void Step(double Dt);

		/** Finds the first thing along a ray, ground or body. */
		bool Raycast(const Vec3& Origin, const Vec3& Direction, double MaxDistance, RayHit& Out) const;

		std::size_t GetContactCount() const { return ContactCount; }
		std::size_t GetAwakeCount() const { return AwakeCount; }

	private:
		struct Scene;

		std::unique_ptr<Scene> Sim;
		std::vector<Body> Bodies;
		std::vector<BodyId> FreeList;
		std::vector<BodyId> Dynamics;
		ForceCallback OnForces;
		std::size_t ContactCount = 0;
		std::size_t AwakeCount = 0;
	};
}
