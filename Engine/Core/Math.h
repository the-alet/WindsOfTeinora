// Double-precision maths for the simulation, single-precision types for the GPU.
// The world is right-handed with Z up, and all simulation units are SI.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Teinora
{
	inline constexpr double Pi = 3.14159265358979323846;

	struct Vec3
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
	};

	inline Vec3 operator+(const Vec3& A, const Vec3& B) { return { A.X + B.X, A.Y + B.Y, A.Z + B.Z }; }
	inline Vec3 operator-(const Vec3& A, const Vec3& B) { return { A.X - B.X, A.Y - B.Y, A.Z - B.Z }; }
	inline Vec3 operator-(const Vec3& V) { return { -V.X, -V.Y, -V.Z }; }
	inline Vec3 operator*(const Vec3& V, double Scale) { return { V.X * Scale, V.Y * Scale, V.Z * Scale }; }
	inline Vec3 operator*(double Scale, const Vec3& V) { return V * Scale; }
	inline Vec3& operator+=(Vec3& A, const Vec3& B) { A = A + B; return A; }
	inline Vec3& operator-=(Vec3& A, const Vec3& B) { A = A - B; return A; }

	inline double Dot(const Vec3& A, const Vec3& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
	inline double LengthSquared(const Vec3& V) { return Dot(V, V); }
	inline double Length(const Vec3& V) { return std::sqrt(Dot(V, V)); }

	inline Vec3 Cross(const Vec3& A, const Vec3& B)
	{
		return { A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X };
	}

	inline Vec3 Normalize(const Vec3& V)
	{
		const double Size = Length(V);
		return Size > 0.0 ? V * (1.0 / Size) : Vec3{};
	}

	struct Quat
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		double W = 1.0;
	};

	inline Quat operator*(const Quat& A, const Quat& B)
	{
		return {
			A.W * B.X + A.X * B.W + A.Y * B.Z - A.Z * B.Y,
			A.W * B.Y - A.X * B.Z + A.Y * B.W + A.Z * B.X,
			A.W * B.Z + A.X * B.Y - A.Y * B.X + A.Z * B.W,
			A.W * B.W - A.X * B.X - A.Y * B.Y - A.Z * B.Z };
	}

	inline Quat Normalize(const Quat& Q)
	{
		const double Size = std::sqrt(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W);
		return { Q.X / Size, Q.Y / Size, Q.Z / Size, Q.W / Size };
	}

	inline Quat FromAxisAngle(const Vec3& UnitAxis, double Radians)
	{
		const double Half = Radians * 0.5;
		const double Sine = std::sin(Half);
		return { UnitAxis.X * Sine, UnitAxis.Y * Sine, UnitAxis.Z * Sine, std::cos(Half) };
	}

	inline Vec3 Rotate(const Quat& Q, const Vec3& V)
	{
		const Vec3 Axis{ Q.X, Q.Y, Q.Z };
		const Vec3 T = Cross(Axis, V) * 2.0;
		return V + T * Q.W + Cross(Axis, T);
	}

	inline Vec3 RotateInverse(const Quat& Q, const Vec3& V)
	{
		return Rotate(Quat{ -Q.X, -Q.Y, -Q.Z, Q.W }, V);
	}

	struct Mat3
	{
		double M[3][3] = {};

		Vec3 Column(int Index) const { return { M[0][Index], M[1][Index], M[2][Index] }; }
	};

	inline Vec3 operator*(const Mat3& A, const Vec3& V)
	{
		return {
			A.M[0][0] * V.X + A.M[0][1] * V.Y + A.M[0][2] * V.Z,
			A.M[1][0] * V.X + A.M[1][1] * V.Y + A.M[1][2] * V.Z,
			A.M[2][0] * V.X + A.M[2][1] * V.Y + A.M[2][2] * V.Z };
	}

	inline Mat3 ToMatrix(const Quat& Q)
	{
		const double XX = Q.X * Q.X, YY = Q.Y * Q.Y, ZZ = Q.Z * Q.Z;
		const double XY = Q.X * Q.Y, XZ = Q.X * Q.Z, YZ = Q.Y * Q.Z;
		const double WX = Q.W * Q.X, WY = Q.W * Q.Y, WZ = Q.W * Q.Z;

		Mat3 R;
		R.M[0][0] = 1.0 - 2.0 * (YY + ZZ);
		R.M[0][1] = 2.0 * (XY - WZ);
		R.M[0][2] = 2.0 * (XZ + WY);
		R.M[1][0] = 2.0 * (XY + WZ);
		R.M[1][1] = 1.0 - 2.0 * (XX + ZZ);
		R.M[1][2] = 2.0 * (YZ - WX);
		R.M[2][0] = 2.0 * (XZ - WY);
		R.M[2][1] = 2.0 * (YZ + WX);
		R.M[2][2] = 1.0 - 2.0 * (XX + YY);
		return R;
	}

	struct Float2
	{
		float X = 0.0f;
		float Y = 0.0f;
	};

	struct Float3
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
	};

	struct Float4
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
		float W = 0.0f;
	};

	inline Float3 ToFloat(const Vec3& V)
	{
		return { static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z) };
	}

	/** Stored row by row and used with column vectors; shaders build with -Zpr to match. */
	struct Float4x4
	{
		float M[4][4] = {};
	};

	inline Float4x4 operator*(const Float4x4& A, const Float4x4& B)
	{
		Float4x4 Result;
		for (int Row = 0; Row < 4; ++Row)
		{
			for (int Column = 0; Column < 4; ++Column)
			{
				for (int Index = 0; Index < 4; ++Index)
				{
					Result.M[Row][Column] += A.M[Row][Index] * B.M[Index][Column];
				}
			}
		}
		return Result;
	}

	/** Maps world directions to a view space where +Z points forwards, +X right and +Y up. */
	inline Float4x4 MakeView(const Vec3& Forward, const Vec3& Right, const Vec3& Up, const Vec3& Eye)
	{
		const auto Row = [&Eye](const Vec3& Axis, float* Out)
		{
			Out[0] = static_cast<float>(Axis.X);
			Out[1] = static_cast<float>(Axis.Y);
			Out[2] = static_cast<float>(Axis.Z);
			Out[3] = static_cast<float>(-Dot(Axis, Eye));
		};

		Float4x4 View;
		Row(Right, View.M[0]);
		Row(Up, View.M[1]);
		Row(Forward, View.M[2]);
		View.M[3][3] = 1.0f;
		return View;
	}

	/** Infinite perspective with reversed depth: the near plane maps to 1, the horizon to 0. */
	inline Float4x4 MakePerspective(double VerticalFovRadians, double Aspect, double Near)
	{
		const double Focal = 1.0 / std::tan(VerticalFovRadians * 0.5);
		Float4x4 Projection;
		Projection.M[0][0] = static_cast<float>(Focal / Aspect);
		Projection.M[1][1] = static_cast<float>(Focal);
		Projection.M[2][3] = static_cast<float>(Near);
		Projection.M[3][2] = 1.0f;
		return Projection;
	}

	/** Orthographic box around the view axis, with depth 0 at Near and 1 at Far. */
	inline Float4x4 MakeOrthographic(double HalfWidth, double Near, double Far)
	{
		Float4x4 Projection;
		Projection.M[0][0] = static_cast<float>(1.0 / HalfWidth);
		Projection.M[1][1] = static_cast<float>(1.0 / HalfWidth);
		Projection.M[2][2] = static_cast<float>(1.0 / (Far - Near));
		Projection.M[2][3] = static_cast<float>(-Near / (Far - Near));
		Projection.M[3][3] = 1.0f;
		return Projection;
	}
}
