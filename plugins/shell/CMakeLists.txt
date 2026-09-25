# The one-shot bash tool over Boost.Process v2: async pipe capture on the
# control strand, with exit-code/signal/timeout markers and cooperative
# cancellation. The child route resolves relative workdirs against the
# calling session's cwd.
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost 1.91 REQUIRED COMPONENTS json process filesystem)

araya_add_plugin(shell
    SOURCES src/shell.cpp
    DEPS araya::session araya::system-prompt araya::tools Boost::json Boost::process Boost::filesystem
    TEST tests/shell_test.cpp
    TEST_DEPS araya::session araya::system-prompt araya::tools
)
