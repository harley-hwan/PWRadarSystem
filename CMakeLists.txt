# ============================================================================
#  PWRadarSystem - build description
#  --------------------------------------------------------------------------
#  CMake is the single source of truth for both platforms.  On Windows it
#  *generates* PWRadarSystem.sln and the two .vcxproj files with Visual
#  Studio's own generator, which is the only way to be sure they load: project
#  XML written by hand is unverifiable and was the cause of the earlier
#  "one or more projects were not loaded correctly".
#
#  Windows (Visual Studio 2022)
#      build.bat                       configure + build + self test
#      build.bat Debug                 Debug instead of Release
#      build.bat Release run           build then launch the console
#    then, to work in the IDE:
#      open build\PWRadarSystem.sln    (generated, guaranteed to load)
#
#  Linux
#      ./build.sh                      configure + build + self test
#      ./build.sh Debug
#      ./build.sh Release run
#
#  Manual equivalent:
#      cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#      cmake --build build --config Release
#      build\Release\PWRadarUI.exe --selftest
#
#  Dependencies: only what the OS already ships.  user32 + gdi32 on Windows;
#  libX11 + pthreads + libm on Linux.  No numeric library, no GUI toolkit, no
#  font library.
# ============================================================================

# 3.21 is the first release that maps CMAKE_C_STANDARD 17 onto MSVC's
# /std:c17.  Demanding it here turns a silently wrong language level into a
# clear configure-time error.
cmake_minimum_required(VERSION 3.21)

project(PWRadarSystem
        VERSION 1.0.0
        DESCRIPTION "PW radar detection system and verification console"
        LANGUAGES C)

# ----------------------------------------------------------------------------
#  Language level
# ----------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)          # strict ISO C17; the sources declare
                                     # their own POSIX feature-test macros
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Single-config generators need a default; multi-config ones ignore this.
get_property(PWR_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT PWR_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
if(PWR_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

# The console and the library must sit in the same directory so the executable
# finds the DLL with no PATH juggling.  Multi-config generators append the
# configuration name, giving build/Release/{PWRadarUI.exe,PWRadarCore.dll}.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ----------------------------------------------------------------------------
#  Warning policy
#  --------------------------------------------------------------------------
#  Both toolchains build warning-clean at the levels below: GCC/Clang at the
#  -Wall/-Wextra set and MSVC at /W4, in both configurations, /WX included.
#  Warnings-as-errors stays opt-in by policy: a future compiler version may
#  introduce new diagnostics, and a first build should fail on a defect, not
#  on a diagnostic nobody has read yet.
#      cmake -S . -B build -DPWR_WERROR=ON
# ----------------------------------------------------------------------------
option(PWR_WERROR "Treat compiler warnings as errors" OFF)

if(MSVC)
    set(PWR_WARN /W4)
    # 4820 padding after a member - the ABI structures are padded deliberately
    # 4324 structure padded because of alignment
    # 4710/4711 inlining decisions, informational
    # 4127 conditional expression is constant - the do{}while(0) macro idiom
    # 4200 zero-sized array in a struct
    # 5045 Spectre mitigation insertion notice
    list(APPEND PWR_WARN /wd4820 /wd4324 /wd4710 /wd4711 /wd4127 /wd4200 /wd5045)
    if(PWR_WERROR)
        list(APPEND PWR_WARN /WX)
    endif()
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    # Deterministic floating point: the dB calibration and the self test's
    # tolerances assume no reassociation.
    add_compile_options(/fp:precise)
else()
    set(PWR_WARN -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                 -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual
                 -Wpointer-arith -Wwrite-strings)
    if(PWR_WERROR)
        list(APPEND PWR_WARN -Werror)
    endif()
endif()

# ============================================================================
#  PWRadarCore - the signal-processing library
# ============================================================================
file(GLOB PWR_CORE_SOURCES CONFIGURE_DEPENDS "PWRadarCore/src/*.c")
file(GLOB PWR_CORE_PRIVATE_HEADERS           "PWRadarCore/src/*.h")
file(GLOB PWR_CORE_PUBLIC_HEADERS            "PWRadarCore/include/pwradar/*.h")

add_library(PWRadarCore SHARED
    ${PWR_CORE_SOURCES}
    ${PWR_CORE_PRIVATE_HEADERS}
    ${PWR_CORE_PUBLIC_HEADERS})

target_include_directories(PWRadarCore
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/PWRadarCore/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/PWRadarCore/src)

# PWR_BUILD_SHARED selects __declspec(dllexport) / visibility("default") in
# pwr_api.h.  Consumers get neither, so they see dllimport, which is correct.
target_compile_definitions(PWRadarCore PRIVATE PWR_BUILD_SHARED)
target_compile_options(PWRadarCore PRIVATE ${PWR_WARN})

set_target_properties(PWRadarCore PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(PWRadarCore PRIVATE m Threads::Threads)
endif()

# ============================================================================
#  PWRadarUI - the verification console
# ============================================================================
file(GLOB PWR_UI_SOURCES CONFIGURE_DEPENDS "PWRadarUI/src/*.c")
file(GLOB PWR_UI_HEADERS                   "PWRadarUI/src/*.h")

add_executable(PWRadarUI ${PWR_UI_SOURCES} ${PWR_UI_HEADERS})
target_include_directories(PWRadarUI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/PWRadarUI/src)
target_compile_options(PWRadarUI PRIVATE ${PWR_WARN})
target_link_libraries(PWRadarUI PRIVATE PWRadarCore)

if(WIN32)
    target_link_libraries(PWRadarUI PRIVATE user32 gdi32)
    # Console subsystem so --selftest and --list-scenarios output is visible;
    # the window is created regardless of the subsystem.
    set_target_properties(PWRadarUI PROPERTIES WIN32_EXECUTABLE OFF)
    # Make F5 in the IDE start the console rather than ALL_BUILD.
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                 PROPERTY VS_STARTUP_PROJECT PWRadarUI)
else()
    find_package(X11 REQUIRED)
    if(NOT X11_FOUND)
        message(FATAL_ERROR
            "libX11 development files not found.  Install them, e.g.\n"
            "  Debian/Ubuntu: sudo apt install libx11-dev\n"
            "  Fedora/RHEL:   sudo dnf install libX11-devel")
    endif()
    target_include_directories(PWRadarUI PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(PWRadarUI PRIVATE ${X11_LIBRARIES} m)
    # Find libPWRadarCore.so beside the executable, no LD_LIBRARY_PATH needed.
    set_target_properties(PWRadarUI PROPERTIES
        BUILD_RPATH   "$ORIGIN"
        INSTALL_RPATH "$ORIGIN")
endif()

# ----------------------------------------------------------------------------
#  Solution / project folders so the generated .sln reads like the source tree
# ----------------------------------------------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/PWRadarCore" PREFIX "src"
             FILES ${PWR_CORE_SOURCES} ${PWR_CORE_PRIVATE_HEADERS}
                   ${PWR_CORE_PUBLIC_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/PWRadarUI" PREFIX "src"
             FILES ${PWR_UI_SOURCES} ${PWR_UI_HEADERS})

# ============================================================================
#  Tests - the library's own numerical acceptance suite is the gate
# ============================================================================
include(CTest)
if(BUILD_TESTING)
    add_test(NAME core_selftest COMMAND PWRadarUI --selftest)
    set_tests_properties(core_selftest PROPERTIES
        TIMEOUT 180
        PASS_REGULAR_EXPRESSION "result: PASS")
endif()

# ============================================================================
#  Install
# ============================================================================
install(TARGETS PWRadarCore PWRadarUI
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
install(DIRECTORY PWRadarCore/include/pwradar DESTINATION include)

# ============================================================================
#  Configure summary
# ============================================================================
message(STATUS "")
message(STATUS "PWRadarSystem ${PROJECT_VERSION}")
message(STATUS "  generator      : ${CMAKE_GENERATOR}")
message(STATUS "  compiler       : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C standard     : C${CMAKE_C_STANDARD}")
if(PWR_MULTI_CONFIG)
    message(STATUS "  configurations : ${CMAKE_CONFIGURATION_TYPES}")
else()
    message(STATUS "  build type     : ${CMAKE_BUILD_TYPE}")
endif()
message(STATUS "  warnings-as-errors : ${PWR_WERROR}")
message(STATUS "  output         : ${CMAKE_BINARY_DIR}")
message(STATUS "")
