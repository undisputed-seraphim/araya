#pragma once

#include <stdexcept>
#include <string>

// MEDULLA_ASSERT checks internal invariants of the engine.
//
// Debug builds (NDEBUG undefined): the check compiles to assert(cond).
// Release builds: the check is a no-op unless MEDULLA_ENFORCE_INVARIANTS is
// defined, in which case a violation throws std::logic_error. Tests that must
// exercise the checks regardless of build type define
// MEDULLA_ENFORCE_INVARIANTS for the medulla library target.
//
// MEDULLA_ASSERT_NOTHROW is the same check for noexcept contexts, where a
// violation cannot throw and instead aborts the process.

#ifdef MEDULLA_ENFORCE_INVARIANTS

#define MEDULLA_ASSERT(cond)                                              \
    do {                                                                  \
        if (!(cond))                                                      \
            throw ::std::logic_error(                                     \
                std::string("medulla invariant violated: ") + #cond);     \
    } while (false)

#define MEDULLA_ASSERT_NOTHROW(cond)         \
    do {                                     \
        if (!(cond))                         \
            ::std::abort();                  \
    } while (false)

#elif defined(NDEBUG)

#define MEDULLA_ASSERT(cond) ((void)0)
#define MEDULLA_ASSERT_NOTHROW(cond) ((void)0)

#else

#include <cassert>
#include <cstdlib>
#define MEDULLA_ASSERT(cond) assert(cond)
#define MEDULLA_ASSERT_NOTHROW(cond) assert(cond)

#endif
