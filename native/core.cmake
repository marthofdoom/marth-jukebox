# Pure-C++ core: environment map (categories/), settings + profile builder (config/), pool/cache model and
# slot audio (pool/PoolModel, pool/Audio + dr_flac), slot assignment (retarget/SlotAssign).
# No engine, no CommonLib, no network: compiled as the static library mj_core (so CI compiles it on every
# build) and unit-tested by the console target mj_core_tests (only with -DMJ_BUILD_TESTS=ON).
# Included from native/CMakeLists.txt with one line; everything for this subsystem lives here.

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(mj_core STATIC
    ${CMAKE_CURRENT_LIST_DIR}/categories/Environments.cpp
    ${CMAKE_CURRENT_LIST_DIR}/config/Settings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/config/ProfileBuilder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pool/PoolModel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pool/Audio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pool/DrFlac.cpp
    ${CMAKE_CURRENT_LIST_DIR}/retarget/SlotAssign.cpp
)
target_compile_features(mj_core PUBLIC cxx_std_23)
target_include_directories(mj_core PUBLIC ${CMAKE_CURRENT_LIST_DIR})
# Vendored single-header libraries (dr_flac, public domain / MIT-0): included as "dr_libs/dr_flac.h".
target_include_directories(mj_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../third_party)
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
        tests/core/PoolTests.cpp
        tests/core/AudioTests.cpp
        tests/core/SlotAssignTests.cpp
    )
    target_compile_features(mj_core_tests PRIVATE cxx_std_23)
    target_include_directories(mj_core_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(mj_core_tests PRIVATE mj_core)

    enable_testing()
    add_test(NAME mj_core_tests COMMAND mj_core_tests)
endif()
