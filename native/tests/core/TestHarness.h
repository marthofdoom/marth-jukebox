#pragma once

// Minimal console test harness for mj_core_tests (no framework dependency; CI fails on a non-zero exit).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "mj_core_test_paths.h"

namespace mjtest
{
	struct Case
	{
		const char* name;
		void (*fn)();
	};

	inline std::vector<Case>& Cases()
	{
		static std::vector<Case> v;
		return v;
	}

	inline std::atomic<int>& Checks()
	{
		static std::atomic<int> n{ 0 };
		return n;
	}

	inline std::atomic<int>& Failures()
	{
		static std::atomic<int> n{ 0 };
		return n;
	}

	struct Registrar
	{
		Registrar(const char* name, void (*fn)()) { Cases().push_back({ name, fn }); }
	};

	inline void Fail(const char* file, int line, const std::string& what)
	{
		++Failures();
		std::fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, what.c_str());
	}

	// Directory of the committed fixtures / golden files (compile definition from core.cmake).
	inline std::filesystem::path DataDir()
	{
		return std::filesystem::path(MJ_TEST_DATA_DIR);
	}

	// A fresh scratch directory under the system temp dir, removed on destruction.
	class ScratchDir
	{
	public:
		ScratchDir()
		{
			static std::atomic<int> counter{ 0 };
			const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
			path_ = std::filesystem::temp_directory_path() / ("mj_core_tests_" + std::to_string(ticks) + "_" + std::to_string(++counter));
			std::filesystem::create_directories(path_);
		}
		~ScratchDir()
		{
			std::error_code ec;
			std::filesystem::remove_all(path_, ec);
		}
		ScratchDir(const ScratchDir&)            = delete;
		ScratchDir& operator=(const ScratchDir&) = delete;
		[[nodiscard]] const std::filesystem::path& Path() const { return path_; }

	private:
		std::filesystem::path path_;
	};
}

#define MJ_TEST(name)                                          \
	static void name();                                        \
	static ::mjtest::Registrar mj_registrar_##name(#name, name); \
	static void name()

#define MJ_CHECK(cond)                                              \
	do {                                                            \
		++::mjtest::Checks();                                       \
		if (!(cond)) {                                              \
			::mjtest::Fail(__FILE__, __LINE__, #cond);              \
		}                                                           \
	} while (0)

#define MJ_CHECK_MSG(cond, msg)                                     \
	do {                                                            \
		++::mjtest::Checks();                                       \
		if (!(cond)) {                                              \
			::mjtest::Fail(__FILE__, __LINE__, std::string(#cond) + "  -- " + (msg)); \
		}                                                           \
	} while (0)
