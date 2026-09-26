# The model-facing ask_user_question tool over the user-questions service.
araya_add_plugin(tool-ask-user
    SOURCES src/tool_ask_user.cpp
    DEPS araya::tools araya::user-questions
    TEST tests/tool_ask_user_test.cpp
)
