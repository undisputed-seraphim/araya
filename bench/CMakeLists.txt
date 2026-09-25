# The profiling bench: a host application that drives the engine with
# synthetic workloads (see README.md in this directory for the runbook).
# Static linkage — the bench plays the orchestrator role and links the
# first-party plugins directly; a production host would load plugins by
# path through module_loader instead.
add_executable(araya_bench araya_bench.cpp)

target_link_libraries(araya_bench PRIVATE
    araya::araya
    araya::logger
    araya::timer
)

araya_enable_warnings(araya_bench)

# Link gperftools so CPUPROFILE/HEAPPROFILE work without LD_PRELOAD.
# The combined library is preferred: linking libprofiler and libtcmalloc
# side by side breaks the CPU profiler's startup.
option(ARAYA_BENCH_GPROF
    "Link gperftools into araya_bench (CPU + heap profiling)"
    OFF)
if(ARAYA_BENCH_GPROF)
    find_library(ARAYA_GPERFTOOLS_LIB tcmalloc_and_profiler)
    if(ARAYA_GPERFTOOLS_LIB)
        target_link_libraries(araya_bench PRIVATE ${ARAYA_GPERFTOOLS_LIB})
    else()
        find_library(ARAYA_PROFILER_LIB profiler)
        find_library(ARAYA_TCMALLOC_LIB tcmalloc)
        if(ARAYA_PROFILER_LIB AND ARAYA_TCMALLOC_LIB)
            target_link_libraries(araya_bench PRIVATE
                ${ARAYA_PROFILER_LIB}
                ${ARAYA_TCMALLOC_LIB})
        else()
            message(WARNING
                "ARAYA_BENCH_GPROF: gperftools not found")
        endif()
    endif()
endif()

# Keep frame pointers for perf's call-graph sampling.
option(ARAYA_BENCH_FP
    "Compile araya_bench with -fno-omit-frame-pointer"
    OFF)
if(ARAYA_BENCH_FP)
    target_compile_options(araya_bench PRIVATE -fno-omit-frame-pointer)
endif()
