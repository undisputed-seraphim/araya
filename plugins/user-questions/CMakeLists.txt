# The user-interaction seam: a service a tool awaits while the human answers.
# One registered answerer (the app's script/console prompt) serves ask().
araya_add_plugin(user-questions
    SOURCES src/user_questions.cpp
    DEPS araya::araya
    TEST tests/user_questions_test.cpp
)
