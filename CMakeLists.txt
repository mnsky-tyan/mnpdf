cmake_minimum_required(VERSION 3.16)
project(mnpdf-linux CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

# PDFium: the vendored Linux build under third_party/pdfium-linux (headers,
# libpdfium.so and its license), the exact counterpart of the Windows
# third_party/pdfium that build.bat consumes.
set(PDFIUM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium-linux)

add_executable(mnpdf
  src/linux/main.cpp
  src/linux/doc.cpp
  src/linux/render.cpp
  src/linux/sidecar.cpp
  src/linux/search.cpp
  src/linux/opendoc.cpp
  src/linux/surgery.cpp
)

# The headless engine suite: links the platform-independent core and stubs the
# ui_* seam, so the whole document engine is exercised with no display.
add_executable(mnpdf-core-test
  tests/core-test.cpp
  src/linux/doc.cpp
  src/linux/search.cpp
  src/linux/sidecar.cpp
  src/linux/opendoc.cpp
  src/linux/surgery.cpp
)
target_include_directories(mnpdf-core-test PRIVATE
  ${PDFIUM_ROOT}/include
  src/linux
)
target_compile_options(mnpdf-core-test PRIVATE -O1 -Wall -Wno-unused-parameter)
target_link_directories(mnpdf-core-test PRIVATE ${PDFIUM_ROOT}/lib)
target_link_libraries(mnpdf-core-test PRIVATE pdfium)
set_target_properties(mnpdf-core-test PROPERTIES BUILD_RPATH "$ORIGIN")
# The scratch root lives in the build tree, never the shared /tmp: the suite
# makes its own private run directory under it, so one run can never read another
# run's leftover documents or state files.
add_test(NAME core-test COMMAND mnpdf-core-test
         ${CMAKE_CURRENT_SOURCE_DIR}/tests/arc.pdf ${CMAKE_CURRENT_BINARY_DIR}/core-test-scratch
         ${CMAKE_CURRENT_SOURCE_DIR}/tests)
enable_testing()

target_include_directories(mnpdf PRIVATE
  ${GTK3_INCLUDE_DIRS}
  ${PDFIUM_ROOT}/include
  src/linux
)

target_compile_options(mnpdf PRIVATE ${GTK3_CFLAGS_OTHER} -Wall -Wno-unused-parameter)
target_link_directories(mnpdf PRIVATE ${PDFIUM_ROOT}/lib ${GTK3_LIBRARY_DIRS})
target_link_libraries(mnpdf PRIVATE ${GTK3_LIBRARIES} pdfium)

# Keep the .so next to the binary so the app runs from build/ with no
# LD_LIBRARY_PATH, mirroring how build.bat copies pdfium.dll beside mnpdf.exe.
add_custom_command(TARGET mnpdf POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${PDFIUM_ROOT}/lib/libpdfium.so $<TARGET_FILE_DIR:mnpdf>/libpdfium.so
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/README.txt $<TARGET_FILE_DIR:mnpdf>/README.txt
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${PDFIUM_ROOT}/LICENSE $<TARGET_FILE_DIR:mnpdf>/PDFIUM-LICENSE.txt
)
set_target_properties(mnpdf PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
