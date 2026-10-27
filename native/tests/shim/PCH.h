#pragma once

// Test-target stand-in for native/PCH.h. net/HarmonyClient.cpp does #include "PCH.h"; in the
// mj_net_tests target this directory is first on the include path, so that resolves here and the
// test exe needs no CommonLibSSE. Keep the std/spdlog list in step with native/PCH.h.

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::literals;
