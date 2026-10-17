# Vendored SQLite 3.54.1 amalgamation: the official single-file embedding
# form (sqlite3.c + sqlite3.h), repacked as tar.xz. Included from the root
# CMakeLists (not via add_subdirectory) so the araya::sqlite3 target lands in
# the root directory scope. A parent that already provides araya::sqlite3
# keeps its own copy.
#
# FTS5 is a compile-time option and the raw amalgamation does not enable it;
# session-query-sqlite is entirely FTS5, so it is defined here. Thread-safe
# (serialized) mode keeps one connection usable from any strand; the engine
# still confines it to the control strand.
if(NOT TARGET araya::sqlite3)
    # The engine is C++; the amalgamation is the only C translation unit.
    enable_language(C)
    include(FetchContent)

    FetchContent_Declare(
        sqlite
        URL "${CMAKE_CURRENT_LIST_DIR}/sqlite-amalgamation-3540100.tar.xz"
        URL_HASH SHA256=2c3dbc1fcf4862de166906efd855f2eb8dd72daefc5d31ff37a549c83a2b1e4a
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )

    # The amalgamation ships no CMakeLists.txt, so MakeAvailable only
    # populates the source directory.
    FetchContent_MakeAvailable(sqlite)

    add_library(araya_sqlite3 STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
    add_library(araya::sqlite3 ALIAS araya_sqlite3)

    set_target_properties(araya_sqlite3 PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(araya_sqlite3 SYSTEM PUBLIC
        "${sqlite_SOURCE_DIR}")
    target_compile_definitions(araya_sqlite3 PRIVATE
        SQLITE_ENABLE_FTS5
        SQLITE_THREADSAFE=1
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_DQS=0)

    find_package(Threads REQUIRED)
    target_link_libraries(araya_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
