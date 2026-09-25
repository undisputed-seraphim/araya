# The filesystem event vocabulary shared by the fs executor (coreutil) and
# the observation policy (fs-observation). Header-first; the one source
# implements the freshness-token helper so both sides agree on the format.
araya_add_library(fs
    SOURCES src/fs.cpp
    DEPS araya::araya
)
