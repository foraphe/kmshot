cmake_minimum_required(VERSION 3.16)
project(kmshot_kms_dmabuf_gl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(GBM REQUIRED gbm)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)
pkg_check_modules(PC_LCMS2 REQUIRED lcms2)

add_executable(kms_capture
  kms.cpp
  color_transform.cpp
)

target_include_directories(kms_capture PRIVATE
  ${LIBDRM_INCLUDE_DIRS}
  ${GBM_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
  ${GLES2_INCLUDE_DIRS}
  ${PC_LCMS2_INCLUDE_DIRS}
)

target_compile_options(kms_capture PRIVATE
  -Wall -Wextra -Wpedantic
)

target_compile_definitions(kms_capture PRIVATE GL_GLEXT_PROTOTYPES)

target_link_directories(kms_capture PRIVATE
  ${LIBDRM_LIBRARY_DIRS}
  ${GBM_LIBRARY_DIRS}
  ${EGL_LIBRARY_DIRS}
  ${GLES2_LIBRARY_DIRS}
  ${PC_LCMS2_LIBRARY_DIRS}
)

target_link_libraries(kms_capture PRIVATE
  ${LIBDRM_LIBRARIES}
  ${GBM_LIBRARIES}
  ${EGL_LIBRARIES}
  ${GLES2_LIBRARIES}
  ${PC_LCMS2_LIBRARIES}
)