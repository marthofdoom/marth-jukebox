# Pure-C++ core: environment map (categories/), settings + profile builder (config/).
# No engine, no CommonLib, no network: compiled as the static library mj_core (so CI compiles it on every
# build) and unit-tested by the console target mj_core_tests (only with -DMJ_BUILD_TESTS=ON).
# Included from native/CMakeLists.txt with one line; everything for this subsystem lives here.

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(mj_core STATIC
    ${CMAKE_CURRENT_LIST_DIR}/categories/Environments.cpp
    ${CMAKE_CURRENT_LIST_DIR}/config/Settings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/config/ProfileBuilder.cpp
)
target_compile_features(mj_core PUBLIC cxx_std_23)
target_include_directories(mj_core PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(mj_core PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog)

option(MJ_BUILD_TESTS "Build the console unit tests (mj_core_tests)" OFF)
if(MJ_BUILD_TESTS)
    set(MJ_CORE_TEST_DATA_DIR "${CMAKE_CURRENT_LIST_DIR}/tests/core")
    configure_file(
        ${CMAKE_CURRENT_LIST_DIR}/tests/core/TestPaths.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/mj_core_test_paths.h
        @ONLY)

    add_executable(mj_core_tests
        tests/core/main.cpp
        tests/core/EnvironmentsTests.cpp
        tests/core/SettingsTests.cpp
        tests/core/ProfileTests.cpp
    )
    target_compile_features(mj_core_tests PRIVATE cxx_std_23)
    target_include_directories(mj_core_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(mj_core_tests PRIVATE mj_core)

    enable_testing()
    add_test(NAME mj_core_tests COMMAND mj_core_tests)
endif()
