# The built-in file and search tools (read/write/edit/glob/grep). The fs
# work is native (std::filesystem); araya::tools supplies the tool registry
# and (transitively) the system-prompt registry, araya::session the calling
# session's cwd.
araya_add_plugin(coreutil
    SOURCES src/coreutil.cpp
    DEPS araya::session araya::tools
    TEST tests/coreutil_test.cpp
)
