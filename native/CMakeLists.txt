# marth-jukebox native plugin. Structure copied from MFO (which copied it from
# the SkyrimDev/HelloWorld-using-CommonLibSSE-NG template). Doctrine: adapt
# working sources, never write a build system from memory.
#
# M0a is passive: CommonLibSSE only (no imgui, no nlohmann_json). The runtime
# declaration is add_commonlibsse_plugin's default (Address Library version
# independence), exactly as MFO ships it: one DLL for 1.5.97 and 1.6.1170.
cmake_minimum_required(VERSION 3.21)

project(MarthJukebox VERSION 0.1.0 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES
    plugin.cpp
    probe/Probe.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)

# Emit a PDB even in Release so a field crash symbolizes to a source line. /Zi
# adds debug info; /DEBUG writes the .pdb; /OPT:REF,ICF undo the size cost the
# linker would otherwise skip once /DEBUG is on. CI uploads the PDB beside the DLL.
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zi>)
target_link_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/DEBUG /OPT:REF /OPT:ICF>)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
