# marth-jukebox native plugin. Structure copied from MFO (which copied it from
# the SkyrimDev/HelloWorld-using-CommonLibSSE-NG template). Doctrine: adapt
# working sources, never write a build system from memory.
#
# M0a is passive: CommonLibSSE only (no imgui; nlohmann_json joined with the net client). The runtime
# declaration is add_commonlibsse_plugin's default (Address Library version
# independence), exactly as MFO ships it: one DLL for 1.5.97 and 1.6.1170.
cmake_minimum_required(VERSION 3.21)

project(MarthJukebox VERSION 0.1.0 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

option(MJ_BUILD_TESTS "Build the mj_net_tests console target (not part of the shipped DLL)" OFF)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES
    plugin.cpp
    probe/Probe.cpp
    retarget/Retarget.cpp
    slots/SlotWriter.cpp
    net/HarmonyClient.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)

# Emit a PDB even in Release so a field crash symbolizes to a source line. /Zi
# adds debug info; /DEBUG writes the .pdb; /OPT:REF,ICF undo the size cost the
# linker would otherwise skip once /DEBUG is on. CI uploads the PDB beside the DLL.
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zi>)
target_link_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/DEBUG /OPT:REF /OPT:ICF>)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Pure-C++ core (environment map, settings, profile builder) and its tests.
include(${CMAKE_CURRENT_SOURCE_DIR}/core.cmake)
# Settings page (SKSE Menu Framework) + its view-model library and test target.
include(${CMAKE_CURRENT_SOURCE_DIR}/menu/menu.cmake)
# Network client: WinHTTP (system DLL, no extra dependency) + nlohmann_json for the Harmony replies.
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json winhttp)

# Test target for the network client. Built only with -DMJ_BUILD_TESTS=ON (CI turns it on); it
# is a plain console exe that never links CommonLibSSE. HarmonyClient.cpp includes "PCH.h", which
# here resolves to tests/shim/PCH.h (first on the include path) instead of the CommonLib PCH.
if(MJ_BUILD_TESTS)
    find_package(spdlog CONFIG REQUIRED)
    add_executable(mj_net_tests
        tests/net_tests.cpp
        net/HarmonyClient.cpp
    )
    target_compile_features(mj_net_tests PRIVATE cxx_std_23)
    target_include_directories(mj_net_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/shim
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(mj_net_tests PRIVATE spdlog::spdlog nlohmann_json::nlohmann_json winhttp ws2_32)

    # Optional AddressSanitizer build of the test exe (CI runs it as a separate, non-blocking step).
    option(MJ_TESTS_ASAN "Build mj_net_tests with MSVC /fsanitize=address" OFF)
    if(MJ_TESTS_ASAN)
        target_compile_options(mj_net_tests PRIVATE /fsanitize=address /Zi)
        # vcpkg's spdlog/fmt are not ASan-built: switch off the STL container annotations so the
        # linker's annotate_* mismatch check (LNK2038) passes. Heap/stack checks stay on.
        target_compile_definitions(mj_net_tests PRIVATE _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION
                                                        _DISABLE_OPTIONAL_ANNOTATION)
        target_link_options(mj_net_tests PRIVATE /DEBUG /INCREMENTAL:NO)
    endif()
endif()
