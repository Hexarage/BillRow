if(NOT BUILD_TESTS)
    return()
endif()

# Fetch GoogleTest automatically
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

auto_discover_sources(${CMAKE_CURRENT_SOURCE_DIR} TEST_SOURCES)

add_executable(${PROJECT_NAME}_tests ${TEST_SOURCES})

target_link_libraries(${PROJECT_NAME}_tests PRIVATE
    ${PROJECT_NAME}_headers
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME}_tests)