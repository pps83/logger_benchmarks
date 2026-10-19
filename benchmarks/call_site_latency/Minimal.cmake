# Included in place of benchmarks/ under LB_MINIMAL: the call site latency of quill (vendored) and fmtlog, and with
# BTE_DIR set, of backtest-engine's LOG (its libcommon.a) and of the quill benchmark built against its quill fork
set(LB_CALL_SITE ${CMAKE_CURRENT_LIST_DIR})
if (MSVC)
    set(BTE_COMMON_LIB ${BTE_BUILD_DIR}/common.lib)
else ()
    set(BTE_COMMON_LIB ${BTE_BUILD_DIR}/libcommon.a)
endif ()

foreach (BENCH int_int_double int_int_largestr)
    string(TOUPPER ${BENCH} BENCH_DEFINE)

    set(TARGET_NAME benchmark_quill_unbounded_call_site_latency_${BENCH})
    add_executable(${TARGET_NAME} ${LB_CALL_SITE}/benchmark_quill_call_site_latency.cpp)
    target_compile_definitions(${TARGET_NAME} PRIVATE BENCH_${BENCH_DEFINE} QUILL_ENABLE_IMMEDIATE_FLUSH=0)
    target_link_libraries(${TARGET_NAME} PRIVATE quill)

    set(TARGET_NAME benchmark_fmtlog_call_site_latency_${BENCH})
    add_executable(${TARGET_NAME} ${LB_CALL_SITE}/benchmark_fmtlog_call_site_latency.cpp)
    target_compile_definitions(${TARGET_NAME} PRIVATE BENCH_${BENCH_DEFINE})
    target_link_libraries(${TARGET_NAME} PRIVATE fmtlog Threads::Threads)

    if (BTE_DIR)
        set(TARGET_NAME benchmark_bte_call_site_latency_${BENCH})
        add_executable(${TARGET_NAME} ${LB_CALL_SITE}/benchmark_bte_call_site_latency.cpp)
        target_compile_definitions(${TARGET_NAME} PRIVATE BENCH_${BENCH_DEFINE})
        target_compile_features(${TARGET_NAME} PRIVATE cxx_std_23)
        if (MSVC)
            target_compile_options(${TARGET_NAME} PRIVATE /Zc:preprocessor) # LOG's argument counting needs it
        endif ()
        target_include_directories(${TARGET_NAME} PRIVATE ${BTE_DIR}/src)
        target_link_libraries(${TARGET_NAME} PRIVATE ${BTE_COMMON_LIB} Threads::Threads ${CMAKE_DL_LIBS})

        set(TARGET_NAME benchmark_quill_fork_unbounded_call_site_latency_${BENCH})
        add_executable(${TARGET_NAME} ${LB_CALL_SITE}/benchmark_quill_call_site_latency.cpp)
        target_compile_definitions(${TARGET_NAME} PRIVATE BENCH_${BENCH_DEFINE} QUILL_ENABLE_IMMEDIATE_FLUSH=0)
        target_include_directories(${TARGET_NAME} PRIVATE ${BTE_DIR}/ext/quill/include)
        target_link_libraries(${TARGET_NAME} PRIVATE Threads::Threads)
    endif ()
endforeach ()
