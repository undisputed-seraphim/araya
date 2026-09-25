# The one-shot bash tool over Boost.Process v2: async pipe capture on the
# control strand, with exit-code/signal/timeout markers and cooperative
# cancellation. Boost.Process is the only consumer of the process/filesystem
# components (found at the root). araya::tools supplies the tool registry
# and the system-prompt registry, araya::session the calling session's cwd.
araya_add_plugin(shell
    SOURCES src/shell.cpp
    DEPS araya::session araya::tools Boost::process Boost::filesystem
    TEST tests/shell_test.cpp
)
