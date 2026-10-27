# Settings page (docs/INTEGRATION-1.0.md section B). Included from native/CMakeLists.txt with one line, AFTER core.cmake
# and after the DLL target exists.
#   mj_menu_vm      static library: the pure view-model (no ImGui, no CommonLib); also linked into the DLL
#   DLL             gets menu/MenuPage.cpp (ImGui through the vendored SKSE Menu Framework header)
#   mj_menu_tests   console test target (only with -DMJ_BUILD_TESTS=ON), built against menu/StubApp.cpp -- never the DLL
get_filename_component(MJ_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_library(mj_menu_vm STATIC ${CMAKE_CURRENT_LIST_DIR}/ViewModel.cpp)
target_compile_features(mj_menu_vm PUBLIC cxx_std_23)
target_include_directories(mj_menu_vm PUBLIC ${MJ_NATIVE_DIR})
target_link_libraries(mj_menu_vm PUBLIC mj_core)

target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/MenuPage.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE mj_menu_vm)

# The DLL must link before the playback core exists: until native/app/App.cpp is on disk (core-playback merged), the inert
# WeakAppFallback.cpp defines the AppApi functions. When App.cpp exists this block does nothing; the core's own CMake edit
# adds App.cpp to the DLL. Delete WeakAppFallback.cpp and this block at integration.
if(NOT EXISTS ${MJ_NATIVE_DIR}/app/App.cpp)
    message(STATUS "menu: native/app/App.cpp not found, linking menu/WeakAppFallback.cpp (temporary)")
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/WeakAppFallback.cpp)
endif()

if(MJ_BUILD_TESTS)
    # TestHarness.h is shared with mj_core_tests; it needs the generated mj_core_test_paths.h from core.cmake.
    add_executable(mj_menu_tests
        ${MJ_NATIVE_DIR}/tests/menu/main.cpp
        ${MJ_NATIVE_DIR}/tests/menu/MenuTests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/StubApp.cpp
    )
    target_compile_features(mj_menu_tests PRIVATE cxx_std_23)
    target_include_directories(mj_menu_tests PRIVATE ${MJ_NATIVE_DIR}/tests/core ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(mj_menu_tests PRIVATE mj_menu_vm)
    add_test(NAME mj_menu_tests COMMAND mj_menu_tests)
endif()
