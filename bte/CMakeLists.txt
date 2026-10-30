cmake_minimum_required(VERSION 3.28)
project(bte_logger CXX)

# backtest-engine's logger as tools/sync_bte.py copies it, with its tests and log_stress, on its own
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

add_library(btelog STATIC
    src/AsyncLog.cpp
    src/DestructionManager.cpp
    src/SyncLog.cpp
    src/format.cpp
    src/log.cpp
    src/sysutil.cpp
    src/time.cpp
    src/time_tsc.cpp
)
if (WIN32)
    target_sources(btelog PRIVATE src/GetProcAddr.cpp)
    target_compile_definitions(btelog PUBLIC NOMINMAX= _CRT_SECURE_NO_WARNINGS= _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
target_include_directories(btelog PUBLIC src ${THIRD_PARTY}/quill_bte/include ${THIRD_PARTY}/int128/include)
if (MSVC)
    target_compile_options(btelog PUBLIC /Zc:preprocessor /bigobj /utf-8)
else()
    target_compile_options(btelog PUBLIC -Wall)
    target_compile_definitions(btelog PUBLIC _GNU_SOURCE)
    find_package(Threads REQUIRED)
    target_link_libraries(btelog PUBLIC Threads::Threads)
endif()

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY}/googletest googletest EXCLUDE_FROM_ALL)
add_executable(bte_log_tests
    testmain.cpp
    src/test/LogCrashTest.cpp
    src/test/LogEarlyInit.cpp
    src/test/LogTest.cpp
)
target_include_directories(bte_log_tests PRIVATE src/test)
target_link_libraries(bte_log_tests PRIVATE btelog gtest)

# An optimized build fails when a LOG call site makes a call (src/test/LogCallSiteCheck.cmake)
add_library(log_call_sites OBJECT src/test/LogCallSites.cpp)
target_link_libraries(log_call_sites PRIVATE btelog)
target_sources(bte_log_tests PRIVATE $<TARGET_OBJECTS:log_call_sites>)
if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_CXX_FLAGS MATCHES "sanitize")
    # Without the compiler's stack checks, every call a LOG site makes is LOG's own
    if (MSVC)
        target_compile_options(log_call_sites PRIVATE /GS-)
        # Visual Studio's LLVM tools, beside clang-cl or under VC/Tools for cl
        get_filename_component(COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(LOG_DISASSEMBLER llvm-objdump HINTS "${COMPILER_DIR}"
                     "${COMPILER_DIR}/../../../../../Llvm/x64/bin" "${COMPILER_DIR}/../../../../../Llvm/ARM64/bin")
    else()
        target_compile_options(log_call_sites PRIVATE -fno-stack-protector)
        set(LOG_DISASSEMBLER "${CMAKE_OBJDUMP}")
    endif()
    if (LOG_DISASSEMBLER)
        add_custom_command(TARGET bte_log_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} "-DTOOL=${LOG_DISASSEMBLER}" "-DOBJECT=$<TARGET_OBJECTS:log_call_sites>"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/src/test/LogCallSiteCheck.cmake
            COMMENT "Checking that LOG call sites call nothing on their fast path")
    else()
        message(WARNING "No llvm-objdump: the LOG call sites go unchecked")
    endif()
endif()

add_executable(log_stress src/test/LogStress.cpp src/random.cpp)
target_include_directories(log_stress PRIVATE src/test)
target_link_libraries(log_stress PRIVATE btelog)

# Built by clock.yml only
add_executable(clock_drift EXCLUDE_FROM_ALL clock_drift.cpp)
target_link_libraries(clock_drift PRIVATE btelog)
if (WIN32)
    target_link_libraries(clock_drift PRIVATE ws2_32)
endif()
