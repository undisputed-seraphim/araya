araya_add_plugin(session
    SOURCES src/plugin.cpp src/repair.cpp src/store.cpp src/surface.cpp
    DEPS araya::araya
    TEST tests/session_test.cpp
)
