// Checks rigid bodies on PhysX against results known from mechanics, on the same
// height-field ground the game uses.
#include "Physics/Physics.h"
#include "TeinoraSim.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Teinora;
using namespace Teinora::Physics;

namespace
{
	constexpr double Dt = 1.0 / 120.0;

	int Failures = 0;

	void Expect(bool bCondition, const char* What)
	{
		std::printf("[%s] %s\n", bCondition ? " ok " : "FAIL", What);
		Failures += bCondition ? 0 : 1;
	}

	void Run(World& Target, double Seconds)
	{
		const int Steps = static_cast<int>(Seconds / Dt);
		for (int Step = 0; Step < Steps; ++Step)
		{
			Target.Step(Dt);
		}
	}

	/** A plane through the origin made of ground cells, tilted about Y so that it falls towards +X. */
	void AddPlane(World& Target, double Degrees = 0.0, double Friction = 1.0)
	{
		constexpr int Cells = 64;
		constexpr double CellSize = 8.0;
		const double Slope = std::tan(Degrees * Pi / 180.0);
		const double Half = Cells * CellSize * 0.5;

		std::vector<float> Heights;
		for (int Y = 0; Y <= Cells; ++Y)
		{
			for (int X = 0; X <= Cells; ++X)
			{
				Heights.push_back(static_cast<float>(-Slope * (X * CellSize - Half)));
			}
		}
		const std::vector<std::uint8_t> Surfaces(static_cast<std::size_t>(Cells) * Cells * 2, 0);

		Target.SetSurfaceFrictions(&Friction, 1);

		GroundDesc Desc;
		Desc.OriginX = -Half;
		Desc.OriginY = -Half;
		Desc.CellSize = CellSize;
		Desc.Cells = Cells;
		Desc.Heights = Heights.data();
		Desc.Surfaces = Surfaces.data();
		Target.AddGround(Desc);
	}

	BodyDesc MakeBox(const Vec3& Position, double Half, double Mass)
	{
		BodyDesc Desc;
		Desc.Collider = Shape::MakeBox({ Half, Half, Half });
		Desc.Mass = Mass;
		Desc.Position = Position;
		return Desc;
	}

	BodyDesc MakeSphere(const Vec3& Position, double Radius, double Mass)
	{
		BodyDesc Desc;
		Desc.Collider = Shape::MakeSphere(Radius);
		Desc.Mass = Mass;
		Desc.Position = Position;
		return Desc;
	}

	void TestVolumes()
	{
		Expect(std::abs(Shape::MakeSphere(1.0).GetVolume() - 4.0 / 3.0 * Pi) < 1e-9, "sphere volume");
		Expect(std::abs(Shape::MakeBox({ 0.5, 1.0, 2.0 }).GetVolume() - 8.0) < 1e-9, "box volume");
		Expect(std::abs(Shape::MakeCapsule(1.0, 1.0).GetVolume() - (4.0 / 3.0 * Pi + 2.0 * Pi)) < 1e-9, "capsule volume");
	}

	void TestFreeFall()
	{
		World Target;
		const BodyId Ball = Target.CreateBody(MakeSphere({ 0.0, 0.0, 100.0 }, 0.5, 1.0));
		Run(Target, 2.0);
		const double Fallen = 100.0 - Target.GetBody(Ball).Position.Z;
		Expect(std::abs(Fallen - 0.5 * 9.81 * 4.0) < 0.1, "free fall covers g t^2 / 2");
	}

	void TestMassDoesNotChangeFall()
	{
		World Target;
		const BodyId Light = Target.CreateBody(MakeSphere({ 0.0, 0.0, 50.0 }, 0.5, 1.0));
		const BodyId Heavy = Target.CreateBody(MakeSphere({ 5.0, 0.0, 50.0 }, 0.5, 1000.0));
		Run(Target, 1.5);
		Expect(std::abs(Target.GetBody(Light).Position.Z - Target.GetBody(Heavy).Position.Z) < 1e-3, "light and heavy bodies fall together");
	}

	void TestSphereRests()
	{
		World Target;
		AddPlane(Target);
		const BodyId Ball = Target.CreateBody(MakeSphere({ 0.0, 0.0, 5.0 }, 0.5, 10.0));
		Run(Target, 6.0);
		const Body& Result = Target.GetBody(Ball);
		std::printf("       sphere rests at %.3f\n", Result.Position.Z);
		Expect(std::abs(Result.Position.Z - 0.5) < 0.03, "sphere rests on the ground");
		Expect(Result.bAsleep, "resting sphere falls asleep");
	}

	void TestBounce()
	{
		World Target;
		AddPlane(Target);
		BodyDesc Desc = MakeSphere({ 0.0, 0.0, 2.5 }, 0.5, 1.0);
		Desc.Restitution = 0.8;
		const BodyId Ball = Target.CreateBody(Desc);

		double Peak = 0.0;
		bool bLanded = false;
		for (int Step = 0; Step < 240; ++Step)
		{
			Target.Step(Dt);
			const Body& Result = Target.GetBody(Ball);
			bLanded = bLanded || Result.Velocity.Z > 0.0;
			if (bLanded)
			{
				Peak = std::max(Peak, Result.Position.Z - 0.5);
			}
		}
		std::printf("       rebound %.3f m from 2 m\n", Peak);
		Expect(std::abs(Peak - 2.0 * 0.64) < 0.15, "rebound height follows e^2 h");
	}

	/** Flat ground of a single soft surface. */
	void AddSoftPlane(World& Target, const SurfaceMaterial& Surface)
	{
		constexpr int Cells = 64;
		constexpr double CellSize = 8.0;
		const std::vector<float> Heights(static_cast<std::size_t>(Cells + 1) * (Cells + 1), 0.0f);
		const std::vector<std::uint8_t> Surfaces(static_cast<std::size_t>(Cells) * Cells * 2, 0);
		Target.SetSurfaces(&Surface, 1);

		GroundDesc Desc;
		Desc.OriginX = -Cells * CellSize * 0.5;
		Desc.OriginY = -Cells * CellSize * 0.5;
		Desc.CellSize = CellSize;
		Desc.Cells = Cells;
		Desc.Heights = Heights.data();
		Desc.Surfaces = Surfaces.data();
		Target.AddGround(Desc);
	}

	/** How high a ball dropped from Drop metres comes back up, and how deep it went in, on a soft surface. */
	void DropOnSoftGround(const SurfaceMaterial& Surface, double Drop, double& OutRebound, double& OutSink)
	{
		World Target;
		AddSoftPlane(Target, Surface);
		BodyDesc Desc = MakeSphere({ 0.0, 0.0, 0.35 + Drop }, 0.35, 8.0);
		Desc.Restitution = 0.5;
		const BodyId Ball = Target.CreateBody(Desc);

		OutRebound = 0.0;
		double Lowest = 1e9;
		bool bLanded = false;
		for (int Step = 0; Step < 2400; ++Step)
		{
			Target.Step(Dt);
			const Body& Result = Target.GetBody(Ball);
			Lowest = std::min(Lowest, Result.Position.Z - 0.35);
			bLanded = bLanded || Result.Velocity.Z > 0.0;
			if (bLanded)
			{
				OutRebound = std::max(OutRebound, Result.Position.Z - 0.35);
			}
		}
		OutSink = -Lowest;
	}

	void TestSoftGround()
	{
		const auto Read = [](Sim::ESurface Kind)
		{
			const Sim::SurfaceGive Give = Sim::GetSurfaceGive(Kind);
			return SurfaceMaterial{ Sim::SurfaceFriction(Kind), Give.Stiffness, Give.Damping, Give.Restitution };
		};
		const SurfaceMaterial Grass = Read(Sim::ESurface::Grass);
		const SurfaceMaterial Sand = Read(Sim::ESurface::Sand);
		const SurfaceMaterial Rock = Read(Sim::ESurface::Rock);

		double GrassRebound = 0.0;
		double GrassSink = 0.0;
		double SandRebound = 0.0;
		double SandSink = 0.0;
		double RockRebound = 0.0;
		double RockSink = 0.0;
		DropOnSoftGround(Grass, 3.0, GrassRebound, GrassSink);
		DropOnSoftGround(Sand, 3.0, SandRebound, SandSink);
		DropOnSoftGround(Rock, 3.0, RockRebound, RockSink);
		std::printf("       from 3 m: grass %.2f m back, %.3f m in; sand %.2f, %.3f; rock %.2f, %.3f\n", GrassRebound, GrassSink, SandRebound, SandSink, RockRebound, RockSink);

		Expect(RockRebound > GrassRebound && GrassRebound > SandRebound, "the softer the ground, the less comes back");
		Expect(GrassRebound > 0.1 && GrassRebound < 1.5, "a ball bounces on grass, but loses most of the fall");
		Expect(GrassSink > RockSink, "and sinks into soft ground, which rock does not allow");
		Expect(GrassSink > 0.02 && GrassSink < 0.3, "by a believable amount");

		double HardRebound = 0.0;
		double HardSink = 0.0;
		DropOnSoftGround(Grass, 12.0, HardRebound, HardSink);
		std::printf("       from 12 m: grass %.2f m back, %.3f m in\n", HardRebound, HardSink);
		Expect(HardSink > GrassSink, "a harder blow sinks deeper");

		// As fast as a ball can be thrown, and then much faster: the firm layer has to hold.
		for (const SurfaceMaterial& Surface : { Grass, Sand, Read(Sim::ESurface::Snow) })
		{
			double Rebound = 0.0;
			double Sink = 0.0;
			DropOnSoftGround(Surface, 82.0, Rebound, Sink);
			double FastRebound = 0.0;
			double FastSink = 0.0;
			DropOnSoftGround(Surface, 460.0, FastRebound, FastSink);
			std::printf("       at 40 m/s: %.2f m back, %.3f m in; at 95 m/s: %.2f, %.3f\n", Rebound, Sink, FastRebound, FastSink);
			Expect(Sink < 0.35 && FastSink < 0.35, "a fast ball stops before its middle goes through the surface");
		}

		World Rest;
		AddSoftPlane(Rest, Grass);
		const BodyId Ball = Rest.CreateBody(MakeSphere({ 0.0, 0.0, 1.0 }, 0.35, 8.0));
		Run(Rest, 6.0);
		std::printf("       resting at %.4f, asleep %d\n", Rest.GetBody(Ball).Position.Z, Rest.GetBody(Ball).bAsleep ? 1 : 0);
		Expect(std::abs(Rest.GetBody(Ball).Position.Z - 0.35) < 0.03, "a ball comes to rest on soft ground, not in it");
	}

	void TestBoxSettles()
	{
		World Target;
		AddPlane(Target);
		BodyDesc Desc = MakeBox({ 0.0, 0.0, 3.0 }, 0.5, 20.0);
		Desc.Rotation = FromAxisAngle(Normalize(Vec3{ 1.0, 1.0, 0.3 }), 0.9);
		const BodyId Box = Target.CreateBody(Desc);
		Run(Target, 8.0);
		const Body& Result = Target.GetBody(Box);
		const Vec3 Up = Rotate(Result.Rotation, Vec3{ 0.0, 0.0, 1.0 });
		const double Alignment = std::max({ std::abs(Up.X), std::abs(Up.Y), std::abs(Up.Z) });
		std::printf("       box height %.3f, face alignment %.4f\n", Result.Position.Z, Alignment);
		Expect(std::abs(Result.Position.Z - 0.5) < 0.03, "tumbling box ends up on a face");
		Expect(Alignment > 0.995, "box lies flat");
	}

	void TestStack()
	{
		World Target;
		AddPlane(Target);

		BodyId Top = NoBody;
		for (int Level = 0; Level < 6; ++Level)
		{
			Top = Target.CreateBody(MakeBox({ 0.0, 0.0, 0.5 + Level * 1.02 }, 0.5, 30.0));
		}
		Run(Target, 10.0);
		const Vec3 Position = Target.GetBody(Top).Position;
		const double Drift = std::hypot(Position.X, Position.Y);
		std::printf("       top of 6 boxes at z %.3f, drift %.4f m\n", Position.Z, Drift);
		Expect(std::abs(Position.Z - 5.5) < 0.1, "stack of six boxes holds its height");
		Expect(Drift < 0.05, "stack of six boxes does not walk");
	}

	void TestElasticCollision()
	{
		World Target;
		Target.SetGravity({});
		BodyDesc Left = MakeSphere({ -2.0, 0.0, 0.0 }, 0.5, 2.0);
		Left.Velocity = { 3.0, 0.0, 0.0 };
		Left.Restitution = 1.0;
		BodyDesc Right = MakeSphere({ 2.0, 0.0, 0.0 }, 0.5, 2.0);
		Right.Restitution = 1.0;
		const BodyId A = Target.CreateBody(Left);
		const BodyId B = Target.CreateBody(Right);
		Run(Target, 3.0);
		std::printf("       after: %.3f and %.3f m/s\n", Target.GetBody(A).Velocity.X, Target.GetBody(B).Velocity.X);
		Expect(std::abs(Target.GetBody(A).Velocity.X) < 0.05, "equal masses swap velocity: striker stops");
		Expect(std::abs(Target.GetBody(B).Velocity.X - 3.0) < 0.05, "equal masses swap velocity: target leaves");
	}

	void TestUnequalMasses()
	{
		World Target;
		Target.SetGravity({});
		BodyDesc Heavy = MakeSphere({ -2.0, 0.0, 0.0 }, 0.5, 6.0);
		Heavy.Velocity = { 4.0, 0.0, 0.0 };
		Heavy.Restitution = 1.0;
		BodyDesc Light = MakeSphere({ 2.0, 0.0, 0.0 }, 0.5, 2.0);
		Light.Restitution = 1.0;
		const BodyId A = Target.CreateBody(Heavy);
		const BodyId B = Target.CreateBody(Light);
		Run(Target, 3.0);

		// v1' = v (m1 - m2) / (m1 + m2), v2' = v 2 m1 / (m1 + m2)
		const double After = Target.GetBody(A).Velocity.X;
		const double Struck = Target.GetBody(B).Velocity.X;
		std::printf("       after: %.3f and %.3f m/s\n", After, Struck);
		Expect(std::abs(After - 2.0) < 0.1, "heavy striker keeps going at (m1 - m2) / (m1 + m2)");
		Expect(std::abs(Struck - 6.0) < 0.1, "light target leaves at 2 m1 / (m1 + m2)");
	}

	void TestMomentum()
	{
		World Target;
		Target.SetGravity({});
		BodyDesc Heavy = MakeBox({ -3.0, 0.1, 0.05 }, 0.5, 8.0);
		Heavy.Velocity = { 4.0, 0.0, 0.0 };
		Heavy.AngularDamping = 0.0;
		BodyDesc Light = MakeBox({ 0.0, 0.0, 0.0 }, 0.5, 2.0);
		Light.AngularDamping = 0.0;
		const BodyId A = Target.CreateBody(Heavy);
		const BodyId B = Target.CreateBody(Light);
		Run(Target, 2.0);
		const Vec3 Total = Target.GetBody(A).Velocity * 8.0 + Target.GetBody(B).Velocity * 2.0;
		std::printf("       momentum %.4f %.4f %.4f\n", Total.X, Total.Y, Total.Z);
		Expect(std::abs(Total.X - 32.0) < 1e-3 && std::abs(Total.Y) < 1e-3 && std::abs(Total.Z) < 1e-3, "box collision conserves momentum");
	}

	void TestStaticFriction()
	{
		const auto Slide = [](double Friction)
		{
			World Target;
			const double Tilt = 20.0;
			AddPlane(Target, Tilt, Friction);

			BodyDesc Desc = MakeBox({ 0.0, 0.0, 0.0 }, 0.5, 10.0);
			const double Radians = Tilt * Pi / 180.0;
			Desc.Rotation = FromAxisAngle({ 0.0, 1.0, 0.0 }, Radians);
			Desc.Position = Vec3{ std::sin(Radians), 0.0, std::cos(Radians) } * 0.5;
			Desc.Friction = Friction;
			const BodyId Box = Target.CreateBody(Desc);
			Run(Target, 2.0);
			return Target.GetBody(Box).Position.X - Desc.Position.X;
		};

		Expect(std::abs(Slide(0.8)) < 0.02, "friction 0.8 holds a box on a 20 degree slope");

		const double Radians = 20.0 * Pi / 180.0;
		const double Expected = 0.5 * 9.81 * (std::sin(Radians) - 0.1 * std::cos(Radians)) * 4.0 * std::cos(Radians);
		const double Measured = Slide(0.1);
		std::printf("       slid %.2f m, expected %.2f m\n", Measured, Expected);
		Expect(std::abs(Measured - Expected) < Expected * 0.1, "sliding box follows g (sin a - mu cos a)");
	}

	void TestRolling()
	{
		World Target;
		const double Tilt = 10.0;
		AddPlane(Target, Tilt, 1.0);

		const double Radians = Tilt * Pi / 180.0;
		BodyDesc Desc = MakeSphere(Vec3{ std::sin(Radians), 0.0, std::cos(Radians) } * 0.5, 0.5, 5.0);
		Desc.AngularDamping = 0.0;
		const BodyId Ball = Target.CreateBody(Desc);
		Run(Target, 2.0);

		const double Distance = (Target.GetBody(Ball).Position.X - Desc.Position.X) / std::cos(Radians);
		const double Expected = 0.5 * (5.0 / 7.0) * 9.81 * std::sin(Radians) * 4.0;
		std::printf("       rolled %.2f m, expected %.2f m\n", Distance, Expected);
		Expect(std::abs(Distance - Expected) < Expected * 0.05, "sphere rolls without slipping at 5/7 g sin a");
	}

	void TestCapsule()
	{
		World Target;
		AddPlane(Target);

		BodyDesc Desc;
		Desc.Collider = Shape::MakeCapsule(0.3, 0.5);
		Desc.Mass = 10.0;
		Desc.Position = { 0.0, 0.0, 2.0 };
		Desc.Rotation = FromAxisAngle({ 0.0, 1.0, 0.0 }, 1.2);
		const BodyId Barrel = Target.CreateBody(Desc);
		Run(Target, 8.0);

		const Body& Result = Target.GetBody(Barrel);
		const Vec3 Axis = Rotate(Result.Rotation, Vec3{ 0.0, 0.0, 1.0 });
		std::printf("       capsule at z %.3f, axis z %.3f\n", Result.Position.Z, Axis.Z);
		Expect(std::abs(Result.Position.Z - 0.3) < 0.03, "capsule lies down at its radius");
		Expect(std::abs(Axis.Z) < 0.02, "capsule axis ends up level");
	}

	void TestStaticObstacle()
	{
		World Target;
		AddPlane(Target);

		BodyDesc Tree;
		Tree.Collider = Shape::MakeCapsule(0.3, 3.0);
		Tree.Position = { 5.0, 0.0, 3.0 };
		Target.CreateBody(Tree);

		BodyDesc Desc = MakeBox({ 0.0, 0.0, 0.6 }, 0.5, 40.0);
		Desc.Velocity = { 12.0, 0.0, 0.0 };
		const BodyId Crate = Target.CreateBody(Desc);
		Run(Target, 3.0);
		Expect(Target.GetBody(Crate).Position.X < 5.0, "static tree stops a crate");
	}

	void TestSleepAndWake()
	{
		World Target;
		AddPlane(Target);

		const BodyId Crate = Target.CreateBody(MakeBox({ 0.0, 0.0, 0.5 }, 0.5, 20.0));
		Run(Target, 2.0);
		Expect(Target.GetBody(Crate).bAsleep, "undisturbed crate sleeps");

		BodyDesc Desc = MakeSphere({ -6.0, 0.0, 0.5 }, 0.5, 15.0);
		Desc.Velocity = { 15.0, 0.0, 0.0 };
		Target.CreateBody(Desc);
		Run(Target, 1.5);
		Expect(Target.GetBody(Crate).Position.X > 0.3, "a thrown ball wakes and moves the crate");
	}

	void TestSupportRemoved()
	{
		World Target;
		AddPlane(Target);

		const BodyId Lower = Target.CreateBody(MakeBox({ 0.0, 0.0, 0.5 }, 0.5, 20.0));
		const BodyId Upper = Target.CreateBody(MakeBox({ 0.0, 0.0, 1.5 }, 0.5, 20.0));
		Run(Target, 3.0);
		Target.DestroyBody(Lower);
		Run(Target, 2.0);
		Expect(std::abs(Target.GetBody(Upper).Position.Z - 0.5) < 0.03, "removing a support drops what rested on it");
	}

	void TestNoTunnelling()
	{
		World Target;
		AddPlane(Target);
		BodyDesc Desc = MakeBox({ 0.0, 0.0, 20.0 }, 0.3, 5.0);
		Desc.Velocity = { 0.0, 0.0, -120.0 };
		const BodyId Fast = Target.CreateBody(Desc);
		Run(Target, 4.0);
		Expect(Target.GetBody(Fast).Position.Z > 0.2, "a 120 m/s body does not pass through the ground");
	}

	void TestBuoyancy()
	{
		World Target;
		Target.SetForceCallback([](BodyId, Body& Item, double)
		{
			const double Submerged = Sim::SubmergedFraction(Item.Position.Z, 0.5);
			Item.Force += Sim::WaterForce(Submerged, 1.0, Item.Velocity, 1.2, Item.GetMass());
		});

		const BodyId Wood = Target.CreateBody(MakeBox({ 0.0, 0.0, 1.5 }, 0.5, 400.0));
		const BodyId Stone = Target.CreateBody(MakeBox({ 8.0, 0.0, 1.5 }, 0.5, 2200.0));
		Run(Target, 20.0);

		// A body of 400 kg/m^3 floats with 40% of its height under water.
		const double Draught = 0.5 - Target.GetBody(Wood).Position.Z;
		std::printf("       draught %.3f m\n", Draught);
		Expect(std::abs(Draught - 0.4) < 0.02, "wood floats at its density ratio");
		Expect(Target.GetBody(Stone).Position.Z < -5.0, "stone sinks");
	}

	void TestAirLift()
	{
		World Target;
		const double Volume = Shape::MakeSphere(0.7).GetVolume();
		Target.SetForceCallback([Volume](BodyId, Body& Item, double)
		{
			Item.Force += Sim::AirLift(Volume);
		});

		// Lighter than the air it displaces, so it rises; heavier, and it falls slower than a stone.
		const BodyId Light = Target.CreateBody(MakeSphere({ 0.0, 0.0, 0.0 }, 0.7, Volume * 0.5));
		const BodyId Heavy = Target.CreateBody(MakeSphere({ 5.0, 0.0, 0.0 }, 0.7, Volume * 2.0));
		Run(Target, 1.0);
		Expect(Target.GetBody(Light).Position.Z > 0.5, "a balloon lighter than air rises");

		const double Net = 9.81 * (1.0 - Sim::AirDensity / 2.0);
		const double Fallen = -Target.GetBody(Heavy).Position.Z;
		std::printf("       fell %.2f m, expected %.2f m\n", Fallen, 0.5 * Net);
		Expect(std::abs(Fallen - 0.5 * Net) < 0.1, "a light balloon falls at g (1 - air / body)");
	}

	void TestTurbulence()
	{
		// Two identical balloons a little apart, with gravity balanced out, drifting in the same wind.
		World Target;
		Target.SetGravity({});
		const double Volume = Shape::MakeSphere(0.7).GetVolume();
		constexpr double Area = 1.54;

		double Time = 0.0;
		Target.SetForceCallback([&](BodyId, Body& Item, double)
		{
			Item.Force += Sim::AeroForce(Sim::AirFlowAt(Item.Position, {}, Time, Sim::WorldSeed).Velocity - Item.Velocity, 0.47, Area);
		});

		const Vec3 Start{ 40.0, 40.0, 120.0 };
		const BodyId Left = Target.CreateBody(MakeSphere(Start, 0.7, Volume * 0.3));
		const BodyId Right = Target.CreateBody(MakeSphere(Start + Vec3{ 30.0, 0.0, 0.0 }, 0.7, Volume * 0.3));

		for (int Step = 0; Step < 2400; ++Step)
		{
			Target.Step(1.0 / 120.0);
			Time += 1.0 / 120.0;
		}
		const Vec3 Moved = Target.GetBody(Right).Position - Target.GetBody(Left).Position;

		World Calm;
		Calm.SetGravity({});
		Time = 0.0;
		Calm.SetForceCallback([&](BodyId, Body& Item, double)
		{
			Item.Force += Sim::AeroForce(Sim::WindAt(Item.Position, Time, Sim::WorldSeed) - Item.Velocity, 0.47, Area);
		});
		const BodyId CalmLeft = Calm.CreateBody(MakeSphere(Start, 0.7, Volume * 0.3));
		const BodyId CalmRight = Calm.CreateBody(MakeSphere(Start + Vec3{ 30.0, 0.0, 0.0 }, 0.7, Volume * 0.3));
		for (int Step = 0; Step < 2400; ++Step)
		{
			Calm.Step(1.0 / 120.0);
			Time += 1.0 / 120.0;
		}
		const Vec3 Still = Calm.GetBody(CalmRight).Position - Calm.GetBody(CalmLeft).Position;

		const double Strayed = Length(Target.GetBody(Left).Position - Calm.GetBody(CalmLeft).Position);
		std::printf("       after 20 s a balloon is %.1f m off its calm path, and %.1f m up or down from its neighbour\n", Strayed, std::abs(Moved.Z));
		Expect(Strayed > 2.0, "turbulence carries a body off the path of the mean wind");
		Expect(std::abs(Moved.Z) > 0.05 && std::abs(Still.Z) < 1e-6, "turbulence moves bodies up and down, which a level wind cannot");

		// Air spinning about a sphere drags it round until it keeps up, at half the vorticity.
		World Spin;
		Spin.SetGravity({});
		const Vec3 Vorticity{ 0.0, 0.0, 4.0 };
		Spin.SetForceCallback([&](BodyId, Body& Item, double)
		{
			const double Inertia = 0.4 * Item.GetMass() * 0.25;
			Item.AngularAcceleration += Sim::AeroTorque(Vorticity * 0.5 - Item.AngularVelocity, 0.5) * (1.0 / Inertia);
		});
		const BodyId Ball = Spin.CreateBody(MakeSphere({ 0.0, 0.0, 10.0 }, 0.5, 0.5));
		for (int Step = 0; Step < 4800; ++Step)
		{
			Spin.Step(1.0 / 120.0);
		}
		std::printf("       spun up to %.2f rad/s, the air's half vorticity is 2.00\n", Spin.GetBody(Ball).AngularVelocity.Z);
		Expect(Spin.GetBody(Ball).AngularVelocity.Z > 1.2, "spinning air turns a body with it");
	}

	void TestRaycast()
	{
		World Target;
		AddPlane(Target);

		const BodyId Crate = Target.CreateBody(MakeBox({ 10.0, 0.0, 0.5 }, 0.5, 20.0));

		RayHit Hit;
		const bool bGround = Target.Raycast({ -5.0, 3.0, 10.0 }, { 0.0, 0.0, -1.0 }, 100.0, Hit);
		Expect(bGround && Hit.Target == NoBody && std::abs(Hit.Distance - 10.0) < 0.01, "a ray finds the ground under it");
		Expect(std::abs(Hit.Point.X + 5.0) < 0.01 && std::abs(Hit.Point.Y - 3.0) < 0.01, "a ray hits the ground where it points");
		Expect(Hit.Normal.Z > 0.99, "the ground normal points up");

		const bool bBody = Target.Raycast({ 0.0, 0.0, 0.5 }, { 1.0, 0.0, 0.0 }, 100.0, Hit);
		Expect(bBody && Hit.Target == Crate && std::abs(Hit.Distance - 9.5) < 0.01, "a ray finds the first body");

		Expect(!Target.Raycast({ 0.0, 0.0, 5.0 }, { 0.0, 0.0, 1.0 }, 100.0, Hit), "a ray into the sky finds nothing");
	}

	void TestThrowLandsOnTarget()
	{
		constexpr double Radius = 0.35;
		constexpr double Mass = 8.0;
		constexpr double Drag = 0.47;
		const double Area = Pi * Radius * Radius;
		const double DragPerMass = 0.5 * Sim::AirDensity * Drag * Area / Mass;

		World Target;
		AddPlane(Target);
		Target.SetForceCallback([Area](BodyId, Body& Item, double)
		{
			Item.Force += Sim::AeroForce(Item.Velocity * -1.0, Drag, Area);
		});

		const Vec3 Start{ 0.0, 0.0, 5.0 };
		const double Distance = 35.0;
		double Angle = 0.0;
		const bool bSolved = Sim::SolveThrowAngle(Distance, Radius - Start.Z, 32.0, DragPerMass, Sim::Gravity, Angle);

		BodyDesc Desc = MakeSphere(Start, Radius, Mass);
		Desc.Velocity = Vec3{ std::cos(Angle), 0.0, std::sin(Angle) } * 32.0;
		const BodyId Ball = Target.CreateBody(Desc);

		double Landed = -1.0;
		for (int Step = 0; Step < 1200 && Landed < 0.0; ++Step)
		{
			Target.Step(Dt);
			if (Target.GetBody(Ball).Position.Z <= Radius + 0.02)
			{
				Landed = Target.GetBody(Ball).Position.X;
			}
		}
		std::printf("       thrown at %.1f degrees, landed at %.2f m, aimed at %.2f m\n", Angle * 180.0 / Pi, Landed, Distance);
		Expect(bSolved && std::abs(Landed - Distance) < 0.5, "a solved throw lands on its target through PhysX and air drag");
	}

	void TestGroundMatchesHeights()
	{
		// A hill, to prove rows, columns and heights land where the terrain says they are.
		constexpr int Cells = 16;
		constexpr double CellSize = 4.0;
		const auto HeightAt = [](double X, double Y) { return 3.0 + 0.5 * X - 0.25 * Y; };

		std::vector<float> Heights;
		for (int Y = 0; Y <= Cells; ++Y)
		{
			for (int X = 0; X <= Cells; ++X)
			{
				Heights.push_back(static_cast<float>(HeightAt(100.0 + X * CellSize, 200.0 + Y * CellSize)));
			}
		}

		// Two materials checkered by triangle: the first triangle of each cell slips, the second grips.
		std::vector<std::uint8_t> Surfaces;
		for (int Index = 0; Index < Cells * Cells; ++Index)
		{
			Surfaces.push_back(0);
			Surfaces.push_back(1);
		}

		World Target;
		const double Frictions[] = { 0.1, 1.0 };
		Target.SetSurfaceFrictions(Frictions, 2);

		GroundDesc Desc;
		Desc.OriginX = 100.0;
		Desc.OriginY = 200.0;
		Desc.CellSize = CellSize;
		Desc.Cells = Cells;
		Desc.Heights = Heights.data();
		Desc.Surfaces = Surfaces.data();
		const GroundId Ground = Target.AddGround(Desc);

		bool bAllMatch = true;
		RayHit Hit;
		for (const Vec3& Point : { Vec3{ 103.0, 205.0, 0.0 }, Vec3{ 150.0, 210.0, 0.0 }, Vec3{ 120.0, 255.0, 0.0 }, Vec3{ 161.0, 259.0, 0.0 } })
		{
			const bool bFound = Target.Raycast({ Point.X, Point.Y, 200.0 }, { 0.0, 0.0, -1.0 }, 300.0, Hit);
			const double Expected = HeightAt(Point.X, Point.Y);
			std::printf("       ground at (%.0f, %.0f): %.3f, expected %.3f\n", Point.X, Point.Y, Hit.Point.Z, Expected);
			bAllMatch = bAllMatch && bFound && std::abs(Hit.Point.Z - Expected) < 0.01;
		}
		Expect(bAllMatch, "height field cells sit where the terrain puts them");

		const Vec3 Normal = Normalize(Vec3{ -0.5, 0.25, 1.0 });
		Expect(Length(Hit.Normal - Normal) < 0.01, "height field faces up, with the terrain's slope");

		Target.RemoveGround(Ground);
		Expect(!Target.Raycast({ 120.0, 220.0, 200.0 }, { 0.0, 0.0, -1.0 }, 300.0, Hit), "removed ground is gone");
	}
}

int main()
{
	TestVolumes();
	TestFreeFall();
	TestMassDoesNotChangeFall();
	TestSphereRests();
	TestBounce();
	TestSoftGround();
	TestBoxSettles();
	TestStack();
	TestElasticCollision();
	TestUnequalMasses();
	TestMomentum();
	TestStaticFriction();
	TestRolling();
	TestCapsule();
	TestStaticObstacle();
	TestSleepAndWake();
	TestSupportRemoved();
	TestNoTunnelling();
	TestBuoyancy();
	TestAirLift();
	TestTurbulence();
	TestRaycast();
	TestThrowLandsOnTarget();
	TestGroundMatchesHeights();
	std::printf("%d failure(s)\n", Failures);
	return Failures == 0 ? 0 : 1;
}
