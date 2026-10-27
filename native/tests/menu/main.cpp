// mj_menu_tests: pure view-model logic of the settings page (native/menu/ViewModel.*). No ImGui, no CommonLib.
#include "TestHarness.h"

#include "config/Settings.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
	const char* only = argc > 1 ? argv[1] : nullptr;
	mj::config::SetLogSink([](spdlog::level::level_enum, const std::string&) {});

	int ran = 0;
	int failedCases = 0;
	for (const auto& c : mjtest::Cases()) {
		if (only && std::strcmp(only, c.name) != 0) {
			continue;
		}
		const int before = mjtest::Failures().load();
		std::fprintf(stdout, "[ RUN  ] %s\n", c.name);
		c.fn();
		++ran;
		if (mjtest::Failures().load() != before) {
			++failedCases;
			std::fprintf(stdout, "[ FAIL ] %s\n", c.name);
		} else {
			std::fprintf(stdout, "[  OK  ] %s\n", c.name);
		}
	}

	std::fprintf(stdout, "[mj_menu_tests] %d tests, %d checks, %d failed checks, %d failed tests\n", ran, mjtest::Checks().load(),
		mjtest::Failures().load(), failedCases);
	if (ran == 0) {
		std::fprintf(stderr, "no tests ran\n");
		return 2;
	}
	return mjtest::Failures().load() == 0 ? 0 : 1;
}
