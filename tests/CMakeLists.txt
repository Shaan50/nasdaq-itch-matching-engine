include(GoogleTest)

add_executable(ironbook_tests
  test_histogram.cpp
  test_parser.cpp
  test_flat_hash_map.cpp
  test_pool.cpp
  test_order_book.cpp
  test_market_differential.cpp
  test_validation.cpp
)
target_link_libraries(ironbook_tests PRIVATE ironbook GTest::gtest_main)
ironbook_configure_target(ironbook_tests)
# gtest macros trip a few of our stricter warnings.
target_compile_options(ironbook_tests PRIVATE -Wno-sign-conversion -Wno-conversion -Wno-old-style-cast
  $<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wno-character-conversion -Wno-unknown-warning-option>)
target_compile_definitions(ironbook_tests PRIVATE
  IRONBOOK_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

gtest_discover_tests(ironbook_tests DISCOVERY_TIMEOUT 60)
