# The json component needs the compiled Boost.JSON library; prefer the
# module-mode search so a header-only Boost elsewhere (e.g. a user-local
# install without libboost_json) cannot shadow it.
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost 1.83 REQUIRED COMPONENTS json)

araya_add_plugin(session
    SOURCES src/plugin.cpp src/repair.cpp src/store.cpp src/surface.cpp
    DEPS araya::araya Boost::json
    TEST tests/session_test.cpp
)
