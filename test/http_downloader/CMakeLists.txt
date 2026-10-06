add_executable(HttpDownloaderTest
  HttpDownloaderTest.cpp
  ${REPO_ROOT}/src/network/HttpDownloader.cpp
)
target_include_directories(HttpDownloaderTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/pdf_extract/stubs
  ${REPO_ROOT}/src/network
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/freeink-sdk/libs/network/SecureNet/include
)
target_compile_definitions(HttpDownloaderTest PRIVATE FREEINK_NET_WOLFSSL=1 CROSSPOINT_VERSION="host-test")
target_link_libraries(HttpDownloaderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HttpDownloaderTest)
