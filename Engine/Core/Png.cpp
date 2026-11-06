#include "Core/Png.h"

#include <algorithm>
#include <cstdio>

namespace Teinora
{
	namespace
	{
		std::uint32_t Crc32(const std::uint8_t* Data, std::size_t Size, std::uint32_t Crc = 0xFFFFFFFFu)
		{
			for (std::size_t Index = 0; Index < Size; ++Index)
			{
				Crc ^= Data[Index];
				for (int Bit = 0; Bit < 8; ++Bit)
				{
					Crc = (Crc >> 1) ^ (0xEDB88320u & (0u - (Crc & 1u)));
				}
			}
			return Crc;
		}

		void AppendBig32(std::vector<std::uint8_t>& Out, std::uint32_t Value)
		{
			Out.push_back(static_cast<std::uint8_t>(Value >> 24));
			Out.push_back(static_cast<std::uint8_t>(Value >> 16));
			Out.push_back(static_cast<std::uint8_t>(Value >> 8));
			Out.push_back(static_cast<std::uint8_t>(Value));
		}

		void AppendChunk(std::vector<std::uint8_t>& Out, const char* Type, const std::vector<std::uint8_t>& Body)
		{
			AppendBig32(Out, static_cast<std::uint32_t>(Body.size()));
			const std::size_t CrcStart = Out.size();
			Out.insert(Out.end(), Type, Type + 4);
			Out.insert(Out.end(), Body.begin(), Body.end());
			AppendBig32(Out, ~Crc32(Out.data() + CrcStart, Out.size() - CrcStart));
		}
	}

	bool WritePng(const std::string& Path, const std::vector<std::uint8_t>& Pixels, std::uint32_t Width, std::uint32_t Height)
	{
		const std::size_t RowBytes = static_cast<std::size_t>(Width) * 4 + 1;
		std::vector<std::uint8_t> Raw;
		Raw.reserve(RowBytes * Height);
		for (std::uint32_t Row = 0; Row < Height; ++Row)
		{
			Raw.push_back(0); // no filter
			const std::uint8_t* Start = Pixels.data() + static_cast<std::size_t>(Row) * Width * 4;
			Raw.insert(Raw.end(), Start, Start + static_cast<std::size_t>(Width) * 4);
		}

		// A zlib stream of stored blocks, which any decoder accepts.
		std::vector<std::uint8_t> Stream = { 0x78, 0x01 };
		std::uint32_t AdlerA = 1;
		std::uint32_t AdlerB = 0;
		for (std::size_t Offset = 0; Offset < Raw.size();)
		{
			const std::size_t Block = std::min<std::size_t>(Raw.size() - Offset, 65535);
			const bool bLast = Offset + Block == Raw.size();
			Stream.push_back(bLast ? 1 : 0);
			Stream.push_back(static_cast<std::uint8_t>(Block));
			Stream.push_back(static_cast<std::uint8_t>(Block >> 8));
			Stream.push_back(static_cast<std::uint8_t>(~Block));
			Stream.push_back(static_cast<std::uint8_t>(~Block >> 8));
			Stream.insert(Stream.end(), Raw.begin() + static_cast<std::ptrdiff_t>(Offset), Raw.begin() + static_cast<std::ptrdiff_t>(Offset + Block));

			for (std::size_t Index = 0; Index < Block; ++Index)
			{
				AdlerA = (AdlerA + Raw[Offset + Index]) % 65521;
				AdlerB = (AdlerB + AdlerA) % 65521;
			}
			Offset += Block;
		}
		AppendBig32(Stream, (AdlerB << 16) | AdlerA);

		std::vector<std::uint8_t> Header;
		AppendBig32(Header, Width);
		AppendBig32(Header, Height);
		Header.insert(Header.end(), { 8, 6, 0, 0, 0 }); // 8 bits, RGBA

		std::vector<std::uint8_t> File = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		AppendChunk(File, "IHDR", Header);
		AppendChunk(File, "IDAT", Stream);
		AppendChunk(File, "IEND", {});

		std::FILE* Output = std::fopen(Path.c_str(), "wb");
		if (Output == nullptr)
		{
			return false;
		}
		const bool bWritten = std::fwrite(File.data(), 1, File.size(), Output) == File.size();
		std::fclose(Output);
		return bWritten;
	}
}
