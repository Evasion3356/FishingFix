/*
	Thread-safe file logger backed by spdlog, vendored copy (not a git
	submodule here -- FishingFix isn't a git repo, see CLAUDE.md) of
	PokerCheat/BlackjackCheat's own Log.h. Writes FishingFix.log next to
	the .asi.

	Synchronous in both configs, same reasoning as BlackjackCheat's Log.h
	(its own header comment has the full derivation: spdlog's ASYNC logger
	deadlocks DLL_PROCESS_DETACH via its worker thread's join()). This
	project's FishingFix::Tick()/DeadEyeDiag::OnTick() only call
	Log::Write on delay-window enter/exit (see FishingFix.cpp/
	DeadEyeDiag.cpp) -- not a hot path that needs to be async.
*/

#pragma once

#define SPDLOG_USE_STD_FORMAT
#define SPDLOG_WCHAR_TO_UTF8_SUPPORT

#include "..\external\spdlog\include\spdlog\spdlog.h"
#include "..\external\spdlog\include\spdlog\sinks\basic_file_sink.h"

#include <memory>
#include <utility>

namespace Log
{
	namespace detail
	{
		inline const std::shared_ptr<spdlog::logger>& GetLogger()
		{
			static const std::shared_ptr<spdlog::logger> logger = []
			{
				auto l = spdlog::basic_logger_mt<spdlog::synchronous_factory>(
					"FishingFix", "FishingFix.log", /*truncate*/ false);
				l->set_pattern("[%H:%M:%S.%e] %v");
				l->flush_on(spdlog::level::trace);
				return l;
			}();
			return logger;
		}
	}

	template <typename... Args>
	void Write(spdlog::format_string_t<Args...> fmt, Args&&... args)
	{
		detail::GetLogger()->info(fmt, std::forward<Args>(args)...);
	}

	template <typename... Args>
	void Write(spdlog::wformat_string_t<Args...> fmt, Args&&... args)
	{
		detail::GetLogger()->info(fmt, std::forward<Args>(args)...);
	}
}
