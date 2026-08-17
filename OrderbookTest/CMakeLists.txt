# GoogleTest via FetchContent: pinned release, built as part of this project.
# SYSTEM marks its headers as system includes so our -Werror doesn't apply to them.
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)
FetchContent_MakeAvailable(googletest)

add_executable(orderbook_tests test.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook orderbook_warnings GTest::gtest_main)

# Scenario files are read from the source tree, independent of the runner's cwd.
target_compile_definitions(orderbook_tests PRIVATE TEST_FILES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/TestFiles")

include(GoogleTest)
gtest_discover_tests(orderbook_tests)
