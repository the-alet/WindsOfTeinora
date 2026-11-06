#pragma once

#include <cstdio>
#include <utility>

namespace Teinora
{
	/** printf to stdout, flushed at once so that logs survive a crash. */
	template <typename... Args>
	void Log(const char* Format, Args&&... Arguments)
	{
		if constexpr (sizeof...(Arguments) == 0)
		{
			std::fputs(Format, stdout);
		}
		else
		{
			std::printf(Format, std::forward<Args>(Arguments)...);
		}
		std::fputc('\n', stdout);
		std::fflush(stdout);
	}
}
