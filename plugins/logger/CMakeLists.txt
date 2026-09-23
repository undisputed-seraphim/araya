# quill is a forced dependency of the logger plugin: PUBLIC so consumers
# of araya::logger link it transitively.
araya_add_plugin(logger
    SOURCES src/logger.cpp src/plugin.cpp
    DEPS araya::araya quill::quill
    TEST tests/logger_test.cpp
)
