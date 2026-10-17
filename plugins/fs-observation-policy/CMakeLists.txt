# The event-only filesystem observation policy. araya::fs supplies the
# shared vocabulary/keys; the plugin registers listeners and holds state.
araya_add_plugin(fs-observation-policy
    SOURCES src/fs_observation_policy.cpp
    DEPS araya::fs araya::logger
    TEST tests/fs_observation_policy_test.cpp
)
