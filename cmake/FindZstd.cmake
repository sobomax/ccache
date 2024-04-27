mark_as_advanced(ZSTD_INCLUDE_DIR ZSTD_LIBRARY)

if(DEPS STREQUAL "DOWNLOAD" OR DEP_ZSTD STREQUAL "DOWNLOAD")
  message(STATUS "Downloading Zstd as requested")
  set(_download_zstd TRUE)
else()
  find_path(ZSTD_INCLUDE_DIR zstd.h)
  find_library(ZSTD_LIBRARY zstd)
  if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    file(READ "${ZSTD_INCLUDE_DIR}/zstd.h" _zstd_h)
    string(REGEX MATCH "#define ZSTD_VERSION_MAJOR +([0-9]+).*#define ZSTD_VERSION_MINOR +([0-9]+).*#define ZSTD_VERSION_RELEASE +([0-9]+)" _ "${_zstd_h}")
    set(_zstd_version_string "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    if(NOT "${CMAKE_MATCH_0}" STREQUAL "" AND "${_zstd_version_string}" VERSION_GREATER_EQUAL "${Zstd_FIND_VERSION}")
      message(STATUS "Using system Zstd (${ZSTD_LIBRARY})")
      set(_zstd_origin "SYSTEM (${ZSTD_LIBRARY})")
      add_library(dep_zstd UNKNOWN IMPORTED)
      set_target_properties(
        dep_zstd
        PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}"
        IMPORTED_LOCATION "${ZSTD_LIBRARY}"
      )
    endif()
  endif()
  if(NOT _zstd_origin)
    if(DEPS STREQUAL "AUTO")
      message(STATUS "Downloading Zstd from the internet since Zstd>=${Zstd_FIND_VERSION} was not found locally and DEPS=AUTO")
      set(_download_zstd TRUE)
    else()
      message(FATAL_ERROR "Could not find Zstd>=${Zstd_FIND_VERSION}")
    endif()
  endif()
endif()

if(_download_zstd)
  set(_zstd_version_string 1.5.5)

  if(XCODE)
    # See https://github.com/facebook/zstd/pull/3665
    set(_zstd_patch PATCH_COMMAND sed -i .bak -e "s/^set_source_files_properties.*PROPERTIES.*LANGUAGE.*C/\\#&/ build/cmake/lib/CMakeLists.txt")
  endif()

  set(ZSTD_BUILD_PROGRAMS OFF)
  set(ZSTD_BUILD_SHARED OFF)
  set(ZSTD_BUILD_STATIC ON)
  set(ZSTD_BUILD_TESTS OFF)

  include(FetchContent)
  FetchContent_Declare(
    Zstd
    URL "https://github.com/facebook/zstd/releases/download/v${_zstd_version_string}/zstd-${_zstd_version_string}.tar.gz"
    URL_HASH SHA256=9c4396cc829cfae319a6e2615202e82aad41372073482fce286fac78646d3ee4
    SOURCE_SUBDIR build/cmake
    ${_zstd_patch}
  )
  FetchContent_MakeAvailable(Zstd)

  # Workaround until https://github.com/facebook/zstd/pull/3968 is included in a
  # release:
  set_target_properties(libzstd_static PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib>")

  unset(ZSTD_BUILD_PROGRAMS)
  unset(ZSTD_BUILD_SHARED)
  unset(ZSTD_BUILD_STATIC)
  unset(ZSTD_BUILD_TESTS)

  set(_zstd_origin DOWNLOADED)
  add_library(dep_zstd ALIAS libzstd_static)
endif()

register_dependency(Zstd "${_zstd_origin}" "${_zstd_version_string}")