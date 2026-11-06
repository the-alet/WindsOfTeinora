// The simulation is right-handed and in metres, Unreal left-handed and in centimetres.
// Flipping Y turns one into the other, so the coast lies the same way in both.
#pragma once

#include "CoreMinimal.h"

THIRD_PARTY_INCLUDES_START
#include "Core/Math.h"
THIRD_PARTY_INCLUDES_END

namespace TeinoraConvert
{
	inline constexpr double UnitsPerMetre = 100.0;

	inline FVector ToUnreal(const Teinora::Vec3& Value)
	{
		return FVector(Value.X * UnitsPerMetre, -Value.Y * UnitsPerMetre, Value.Z * UnitsPerMetre);
	}

	inline FVector DirectionToUnreal(const Teinora::Vec3& Value)
	{
		return FVector(Value.X, -Value.Y, Value.Z);
	}

	inline FQuat ToUnreal(const Teinora::Quat& Value)
	{
		return FQuat(-Value.X, Value.Y, -Value.Z, Value.W);
	}
}
