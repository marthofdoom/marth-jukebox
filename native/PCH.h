#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/sinks/basic_file_sink.h>

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

// REQUIRED, not stylistic. add_commonlibsse_plugin generates
// __MarthJukeboxPlugin.cpp with "..."sv literals and force-includes this PCH;
// without this the build dies with C3688 "invalid literal suffix 'sv'" in a
// file we never wrote (same reason as MFO's PCH).
using namespace std::literals;
