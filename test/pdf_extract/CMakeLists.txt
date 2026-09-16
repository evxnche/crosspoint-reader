add_executable(PdfExtractTest
  PdfExtractTest.cpp
  ${REPO_ROOT}/lib/Pdf/PdfSource.cpp
  ${REPO_ROOT}/lib/Pdf/PdfLexer.cpp
  ${REPO_ROOT}/lib/Pdf/PdfFilters.cpp
  ${REPO_ROOT}/lib/Pdf/PdfDocument.cpp
  ${REPO_ROOT}/lib/Pdf/PdfFontMap.cpp
  ${REPO_ROOT}/lib/Pdf/PdfTextExtractor.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/hal/HalMemory.cpp
)
target_include_directories(PdfExtractTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Pdf
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/miniz/src
)
target_link_libraries(PdfExtractTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PdfExtractTest)
