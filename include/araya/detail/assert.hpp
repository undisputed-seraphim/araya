#pragma once

#include <stdexcept>
#include <string>

// ARAYA_ASSERT checks internal invariants of the engine.
//
// Debug builds (NDEBUG undefined): the check compiles to assert(cond).
// Release builds: the check is a no-op unless ARAYA_ENFORCE_INVARIANTS is
// defined, in which case a violation throws std::logic_error. Tests that must
// exercise the checks regardless of build type define
// ARAYA_ENFORCE_INVARIANTS for the araya library target.
//
// ARAYA_ASSERT_NOTHROW is the same check for noexcept contexts, where a
// violation cannot throw and instead aborts the process.

#ifdef ARAYA_ENFORCE_INVARIANTS

#define ARAYA_ASSERT(cond)                                                                                             \
	do {                                                                                                               \
		if (!(cond))                                                                                                   \
			throw ::std::logic_error(std::string("araya invariant violated: ") + #cond);                               \
	} while (false)

#define ARAYA_ASSERT_NOTHROW(cond)                                                                                     \
	do {                                                                                                               \
		if (!(cond))                                                                                                   \
			::std::abort();                                                                                            \
	} while (false)

#elif defined(NDEBUG)

#define ARAYA_ASSERT(cond) ((void)0)
#define ARAYA_ASSERT_NOTHROW(cond) ((void)0)

#else

#include <cassert>
#include <cstdlib>
#define ARAYA_ASSERT(cond) assert(cond)
#define ARAYA_ASSERT_NOTHROW(cond) assert(cond)

#endif
