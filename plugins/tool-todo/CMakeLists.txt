# The model-facing todo tool and its `todos` session projection.
# araya::session supplies the store/projection, araya::tools the registry.
araya_add_plugin(tool-todo
    SOURCES src/tool_todo.cpp
    DEPS araya::session araya::tools
    TEST tests/tool_todo_test.cpp
)
