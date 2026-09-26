# The background-job registry: ids, owner-scoped access, lifecycle, reads,
# waits, cancellation, and completion listeners. araya::session supplies the
# 'session/disposed' event the registry cleans up owners on.
araya_add_plugin(jobs
    SOURCES src/jobs.cpp
    DEPS araya::session
    TEST tests/jobs_test.cpp
)
