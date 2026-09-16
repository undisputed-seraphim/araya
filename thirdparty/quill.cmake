# Vendored quill 13.0.0. Included from the root CMakeLists (not via
# add_subdirectory) so the quill targets land in the root directory scope
# and are visible to plugins/ and below.
include(FetchContent)

FetchContent_Declare(
    quill
    URL "${CMAKE_CURRENT_LIST_DIR}/quill-13.0.0.tar.xz"
    URL_HASH MD5=951c8f192fe08bdb6f069f58a5acee55
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)

FetchContent_MakeAvailable(quill)
