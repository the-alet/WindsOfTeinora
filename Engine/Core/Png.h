#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Teinora
{
	/** Writes tightly packed RGBA8 pixels as an uncompressed PNG. */
	bool WritePng(const std::string& Path, const std::vector<std::uint8_t>& Pixels, std::uint32_t Width, std::uint32_t Height);
}
