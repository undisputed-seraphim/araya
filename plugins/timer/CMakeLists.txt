araya_add_plugin(timer
    SOURCES src/plugin.cpp src/timer.cpp
    DEPS araya::araya
    TEST tests/timer_test.cpp
)
