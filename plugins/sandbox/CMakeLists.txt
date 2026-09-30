# The process-confinement seam and the Linux Landlock provider. The backend is
# an in-process spawn initializer (applied between fork and execve; no helper
# binary). ARAYA_HAVE_LANDLOCK (cmake/Landlock.cmake) gates the syscall
# implementation; without it the provider compiles to a fail-closed stub that
# reports any confining mode as unavailable. The parent supplies only the
# plugin ABI/service headers.
araya_add_plugin(sandbox
    SOURCES src/sandbox.cpp src/landlock.cpp src/plugin.cpp
    DEPS araya::araya
    TEST tests/sandbox_test.cpp
)

if(ARAYA_HAVE_LANDLOCK)
    target_compile_definitions(araya_sandbox PRIVATE ARAYA_HAVE_LANDLOCK=1)
endif()
