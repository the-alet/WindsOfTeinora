// Where the player looks. The mouse moves a point of gaze in the world; the focus
// goes there at once and the camera follows it, never instantly. What the camera
// does depends on how far the gaze is from its direction:
//   inside the neutral circle    it does not turn at all
//   between the two circles      it follows gently, to bring the gaze back to the neutral circle
//   beyond the critical circle   it is damped towards the gaze direction itself, and faster
// The camera is a critically damped spring: it starts from rest, so the focus visibly
// shifts first, then it picks up speed and settles without overshooting.
#pragma once

#include "Core/Math.h"

#include <algorithm>
#include <cmath>

namespace Teinora
{
	class Gaze
	{
	public:
		static constexpr double VerticalFov = 63.0 * Pi / 180.0;
		static constexpr double Sensitivity = 0.0022;           // radians per mouse count
		static constexpr double NeutralRadius = 0.0613;         // 5% of the screen height at this field of view
		static constexpr double CriticalRadius = VerticalFov * 0.5 * 0.8;
		static constexpr double FocusLimit = VerticalFov * 0.5 * 0.95; // the focus stays on screen
		static constexpr double PitchLimit = 1.55;
		static constexpr double FollowTime = 0.3;               // roughly how long a gentle follow takes
		static constexpr double CriticalTime = 0.14;            // the same beyond the critical circle

		double Yaw = 0.0; // camera
		double Pitch = 0.0;
		double EyeYaw = 0.0; // focus, relative to the camera
		double EyePitch = 0.0;

		static Vec3 MakeDirection(double AngleYaw, double AnglePitch)
		{
			return { std::cos(AnglePitch) * std::cos(AngleYaw), std::cos(AnglePitch) * std::sin(AngleYaw), std::sin(AnglePitch) };
		}

		Vec3 GetForward() const { return MakeDirection(Yaw, Pitch); }
		Vec3 GetLineOfSight() const { return MakeDirection(Yaw + EyeYaw, Pitch + EyePitch); }

		/** Places the camera and the focus directly, with nothing left to follow. */
		void Aim(double CameraYaw, double CameraPitch, double FocusYaw = 0.0, double FocusPitch = 0.0)
		{
			Yaw = CameraYaw;
			Pitch = CameraPitch;
			EyeYaw = FocusYaw;
			EyePitch = FocusPitch;
			TargetYaw = CameraYaw + FocusYaw;
			TargetPitch = CameraPitch + FocusPitch;
			SpeedYaw = 0.0;
			SpeedPitch = 0.0;
		}

		/** Advances by Dt. Mouse travel is in counts, with positive X to the left and positive Y up. */
		void Turn(double CountsX, double CountsY, double Dt)
		{
			TargetYaw += CountsX * Sensitivity;
			TargetPitch = std::clamp(TargetPitch + CountsY * Sensitivity, -PitchLimit, PitchLimit);

			const double OffsetYaw = TargetYaw - Yaw;
			const double OffsetPitch = TargetPitch - Pitch;
			const double Offset = std::hypot(OffsetYaw, OffsetPitch);

			// How much of the offset the camera should cover, and how quickly.
			const bool bCritical = Offset >= CriticalRadius;
			const double Travel = bCritical ? 1.0 : (Offset > NeutralRadius ? 1.0 - NeutralRadius / Offset : 0.0);
			const double Time = bCritical ? CriticalTime : FollowTime;

			Follow(Yaw, SpeedYaw, Yaw + OffsetYaw * Travel, Time, Dt);
			Follow(Pitch, SpeedPitch, Pitch + OffsetPitch * Travel, Time, Dt);

			// During a fast sweep the point of gaze can outrun the view. The focus then
			// waits on the edge of the limit, on screen, until the camera has caught up.
			const double FocusYaw = TargetYaw - Yaw;
			const double FocusPitch = TargetPitch - Pitch;
			const double Focus = std::hypot(FocusYaw, FocusPitch);
			const double Scale = Focus > FocusLimit ? FocusLimit / Focus : 1.0;
			EyeYaw = FocusYaw * Scale;
			EyePitch = FocusPitch * Scale;
		}

		/** Turns the camera directly, with no focus in between. */
		void TurnCamera(double CountsX, double CountsY)
		{
			const double NewPitch = std::clamp(TargetPitch + CountsY * Sensitivity, -PitchLimit, PitchLimit);
			Aim(TargetYaw + CountsX * Sensitivity, NewPitch);
		}

	private:
		// The exact solution of the spring for one step, so it is stable at any frame rate.
		static void Follow(double& Value, double& Speed, double Goal, double Time, double Dt)
		{
			const double Omega = 2.0 / Time;
			const double Offset = Value - Goal;
			const double Drive = (Speed + Omega * Offset) * Dt;
			const double Decay = std::exp(-Omega * Dt);

			Value = Goal + (Offset + Drive) * Decay;
			Speed = (Speed - Omega * Drive) * Decay;
		}

		double TargetYaw = 0.0;
		double TargetPitch = 0.0;
		double SpeedYaw = 0.0;
		double SpeedPitch = 0.0;
	};
}
