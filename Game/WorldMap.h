// A picture of the land from above, for seeing what the generator makes without flying over it.
#pragma once

#include <cstdint>
#include <vector>

namespace Teinora
{
	/** RGBA8, Size pixels a side, covering Span metres around a point: sea, lakes, biomes and shaded relief. */
	std::vector<std::uint8_t> DrawWorldMap(int Size, double Span, double CenterX, double CenterY, std::uint32_t Seed);
}
