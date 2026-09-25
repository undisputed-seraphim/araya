# The project's shared compiler-warning policy.
#
#   araya_enable_warnings(<target> [WERROR])
#
# Applies the warning set to GNU/Clang targets. `WERROR` promotes warnings
# to errors. Two known GCC 14 false positives are kept as warnings even
# under WERROR (they are ordered after -Werror so the suppression wins):
#   - -Wno-mismatched-new-delete: awaitable frames are allocated through
#     Boost.Asio's aligned_new and freed by the frame's own delete, which
#     the detector cannot see across the inlining boundary.
#   - -Wno-error=maybe-uninitialized: libstdc++'s std::regex internals trip
#     it under the sanitizer optimization settings.
function(araya_enable_warnings target)
    cmake_parse_arguments(ARG "WERROR" "" "" ${ARGN})
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-mismatched-new-delete)
        if(ARG_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wno-error=maybe-uninitialized)
        endif()
    endif()
endfunction()
