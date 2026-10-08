/*
	Logging for the FishingFix library (FishingFixLib.vcxproj). The library
	doesn't own a log file: whoever links it (this repo's own ASI, or a
	trainer that pulls this repo in as a submodule) sets a sink and decides
	where the lines go. Lines written before a sink is set are dropped.

	Kept separate from the ASI's spdlog Log.h on purpose: a host with its
	own `namespace Log` would otherwise get two different inline
	Log::detail::GetLogger() bodies in one binary.
*/

#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace FishingFix::Log
{
	using Sink = void (*)(std::string_view line);

	void SetSink(Sink sink);
	void Emit(std::string_view line);

	template <typename... Args>
	void Write(std::format_string<Args...> fmt, Args&&... args)
	{
		Emit(std::format(fmt, std::forward<Args>(args)...));
	}
}
