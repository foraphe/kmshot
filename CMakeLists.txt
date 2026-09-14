cmake_minimum_required(VERSION 3.16)
project(kmshot_kms_dmabuf_gl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KMSHOT_BUILD_TESTS "Build the self-check executables" ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)
pkg_check_modules(PC_LCMS2 REQUIRED lcms2)

# Everything except the CLI entry point lives in this static library so the
# self-checks can link against it.
add_library(kmshot_core STATIC
  src/capture.cpp
  src/cli.cpp
  src/color_math.cpp
  src/color_profile.cpp
  src/color_transform.cpp
  src/dmabuf_gl.cpp
  src/drm_debug.cpp
  src/drm_util.cpp
  src/edid.cpp
  src/y4m.cpp
)

target_include_directories(kmshot_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${LIBDRM_INCLUDE_DIRS}
  ${GBM_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
  ${GLES2_INCLUDE_DIRS}
  ${PC_LCMS2_INCLUDE_DIRS}
)

target_compile_options(kmshot_core PRIVATE
  -Wall -Wextra -Wpedantic
)

target_compile_definitions(kmshot_core PUBLIC GL_GLEXT_PROTOTYPES)

target_link_directories(kmshot_core PUBLIC
  ${LIBDRM_LIBRARY_DIRS}
  ${GBM_LIBRARY_DIRS}
  ${EGL_LIBRARY_DIRS}
  ${GLES2_LIBRARY_DIRS}
  ${PC_LCMS2_LIBRARY_DIRS}
)

target_link_libraries(kmshot_core PUBLIC
  ${LIBDRM_LIBRARIES}
  ${GBM_LIBRARIES}
  ${EGL_LIBRARIES}
  ${GLES2_LIBRARIES}
  ${PC_LCMS2_LIBRARIES}
)

add_executable(kms_capture src/main.cpp)
target_compile_options(kms_capture PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(kms_capture PRIVATE kmshot_core)

if(KMSHOT_BUILD_TESTS)
  enable_testing()

  # Verifies the EDID decoder against a real panel (values cross-checked with
  # edid-decode).
  add_executable(kmshot_edid_test tests/edid_test.cpp)
  target_compile_options(kmshot_edid_test PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(kmshot_edid_test PRIVATE kmshot_core)
  add_test(NAME edid_parse
    COMMAND kmshot_edid_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ne160qdm-nm7.edid)

  # Cross-checks the LittleCMS2 derived colour matrix against an independent
  # primaries/Bradford implementation.
  add_executable(kmshot_color_verify tools/verify_color_math.cpp)
  target_compile_options(kmshot_color_verify PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(kmshot_color_verify PRIVATE kmshot_core)
  add_test(NAME color_math COMMAND kmshot_color_verify)

  # Pure-geometry checks for the slurp -> buffer crop mapping.
  add_executable(kmshot_crop_test tests/crop_test.cpp)
  target_compile_options(kmshot_crop_test PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(kmshot_crop_test PRIVATE kmshot_core)
  add_test(NAME crop_rect COMMAND kmshot_crop_test)

  # Numeric checks for the RGBA -> YUV444P16 pipeline.
  add_executable(kmshot_color_transform_test tests/color_transform_test.cpp)
  target_compile_options(kmshot_color_transform_test PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(kmshot_color_transform_test PRIVATE kmshot_core)
  add_test(NAME color_transform COMMAND kmshot_color_transform_test)
endif()
