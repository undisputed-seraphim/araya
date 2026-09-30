# The sandbox policy home: the deployment default mode, the session-workspace
# root resolution, and the SANDBOX_POLICY runtime-context section. araya::sandbox
# supplies the mode/policy vocabulary and araya::system-prompt the runtime
# context snapshot.
araya_add_plugin(sandbox-policy
    SOURCES src/plugin.cpp
    DEPS araya::sandbox araya::system-prompt
    TEST tests/sandbox_policy_test.cpp
)
