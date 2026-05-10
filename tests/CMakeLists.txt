find_package(GTest REQUIRED)

include(GoogleTest)

add_executable(baa_tests
  BumpStorageTest.cpp
  BumpAllocatorTest.cpp
  FixedArenaTest.cpp
)
target_link_libraries(baa_tests PRIVATE baa::baa GTest::gtest_main)
gtest_discover_tests(baa_tests)
