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

add_executable(log_stress src/test/LogStress.cpp src/random.cpp)
target_include_directories(log_stress PRIVATE src/test)
target_link_libraries(log_stress PRIVATE btelog)
