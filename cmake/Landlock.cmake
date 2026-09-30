# Landlock detection for the Linux sandbox backend.
#
# Landlock is a Linux-only, unprivileged file-effect confinement facility
# (kernel 5.13+ for ABI 1). It has no libc wrapper, so detection is a
# compile-time check of the raw syscall UAPI: a source program that
# self-defines the Landlock UAPI (so the result does not depend on the
# toolchain's <linux/landlock.h> vintage) and references
# __NR_landlock_create_ruleset, with the post-2011 unified syscall numbers
# as a fallback.
#
# The check is deliberately COMPILE-ONLY. Whether the running kernel actually
# ENFORCES Landlock (ABI present, landlock enabled in the LSM list) is decided
# at runtime by the sandbox provider's functional probe, which fails closed.
# A configure-time runtime probe would only describe the build host and would
# break cross-compilation, so it is not used here.
#
# Result:
#   ARAYA_HAVE_LANDLOCK     - ON when the backend may be compiled in.
#   ARAYA_LANDLOCK_COMPILES - the raw compile verdict (informational).
#
# The sandbox plugin always builds; when ARAYA_HAVE_LANDLOCK is OFF its
# provider compiles to a fail-closed stub that reports the mode as
# unavailable rather than running unconfined.

option(ARAYA_ENABLE_LANDLOCK
    "Compile the Linux Landlock sandbox backend (runtime probe decides enforcement)"
    ON)

set(ARAYA_HAVE_LANDLOCK OFF)

if(NOT ARAYA_ENABLE_LANDLOCK)
    message(STATUS "Landlock: disabled by ARAYA_ENABLE_LANDLOCK=OFF")
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Landlock: not Linux (${CMAKE_SYSTEM_NAME}); sandbox will fail closed")
else()
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_compiles("
        #include <sys/syscall.h>
        #include <unistd.h>
        #include <cstdint>
        #ifndef __NR_landlock_create_ruleset
        #define __NR_landlock_create_ruleset 444
        #define __NR_landlock_add_rule 445
        #define __NR_landlock_restrict_self 446
        #endif
        int main() {
            long ruleset = syscall(__NR_landlock_create_ruleset, nullptr, 0, 1u);
            long restricted = syscall(__NR_landlock_restrict_self, -1, 0u);
            (void)ruleset;
            (void)restricted;
            return 0;
        }
    " ARAYA_LANDLOCK_COMPILES)
    unset(CMAKE_REQUIRED_QUIET)

    if(ARAYA_LANDLOCK_COMPILES)
        set(ARAYA_HAVE_LANDLOCK ON)
        message(STATUS "Landlock: enabled (compile check passed; runtime probe decides enforcement)")
    else()
        message(STATUS "Landlock: raw UAPI did not compile; sandbox will fail closed")
    endif()
endif()
