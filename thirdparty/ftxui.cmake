# Vendored FTXUI 7.0.3. Included from the root CMakeLists (not via
# add_subdirectory) so the ftxui targets land in the root directory scope
# and are visible to apps/ and below.
include(FetchContent)

FetchContent_Declare(
    ftxui
    URL "${CMAKE_CURRENT_LIST_DIR}/FTXUI-7.0.3.tar.xz"
    URL_HASH MD5=2a07a6aaa76a594cca62de335515b737
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)

# FTXUI builds examples/doc/gtest by default; none are needed here.
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(ftxui)
