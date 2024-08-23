set(PSAN_LIT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(PSAN_TEST_DEPS ${SANITIZER_COMMON_LIT_TEST_DEPS})
if(${COMPILER_RT_DEFAULT_TARGET_ARCH} MATCHES "(x86_64|s390x)")
  list(APPEND PSAN_TEST_DEPS GopsanRuntimeCheck)
endif()
list(APPEND PSAN_TEST_DEPS psan)
if(COMPILER_RT_LIBCXX_PATH AND
   COMPILER_RT_LIBCXXABI_PATH AND
   COMPILER_RT_TEST_COMPILER_ID STREQUAL "Clang"
   AND NOT APPLE AND NOT ANDROID)
  list(APPEND PSAN_TEST_DEPS libcxx_psan)
  set(PSAN_HAS_LIBCXX True)
else()
  set(PSAN_HAS_LIBCXX False)
endif()

set(PSAN_DYNAMIC_TEST_DEPS ${PSAN_TEST_DEPS})
set(PSAN_TESTSUITES)
set(PSAN_DYNAMIC_TESTSUITES)

if (NOT DEFINED PSAN_TEST_DEFLAKE_THRESHOLD)
  set(PSAN_TEST_DEFLAKE_THRESHOLD "10")
endif()

set(PSAN_TEST_ARCH ${PSAN_SUPPORTED_ARCH})
if(APPLE)
  darwin_filter_host_archs(PSAN_SUPPORTED_ARCH PSAN_TEST_ARCH)
endif()

foreach(arch ${PSAN_TEST_ARCH})
  set(PSAN_TEST_APPLE_PLATFORM "osx")
  set(PSAN_TEST_MIN_DEPLOYMENT_TARGET_FLAG "${DARWIN_osx_MIN_VER_FLAG}")

  set(PSAN_TEST_TARGET_ARCH ${arch})
  string(TOLOWER "-${arch}" PSAN_TEST_CONFIG_SUFFIX)
  get_test_cc_for_arch(${arch} PSAN_TEST_TARGET_CC PSAN_TEST_TARGET_CFLAGS)

  string(REPLACE ";" " " LIBDISPATCH_CFLAGS_STRING " ${COMPILER_RT_TEST_LIBDISPATCH_CFLAGS}")
  string(APPEND PSAN_TEST_TARGET_CFLAGS ${LIBDISPATCH_CFLAGS_STRING})

  if (COMPILER_RT_HAS_MSSE4_2_FLAG)
    string(APPEND PSAN_TEST_TARGET_CFLAGS " -msse4.2 ")
  endif()

  string(TOUPPER ${arch} ARCH_UPPER_CASE)
  set(CONFIG_NAME ${ARCH_UPPER_CASE}Config)

  configure_lit_site_cfg(
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
    ${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_NAME}/lit.site.cfg.py
    MAIN_CONFIG
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
    )
  list(APPEND PSAN_TESTSUITES ${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_NAME})

  if(COMPILER_RT_PSAN_HAS_STATIC_RUNTIME)
    string(TOLOWER "-${arch}-${OS_NAME}-dynamic" PSAN_TEST_CONFIG_SUFFIX)
    set(CONFIG_NAME ${ARCH_UPPER_CASE}${OS_NAME}DynamicConfig)
    configure_lit_site_cfg(
      ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
      ${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_NAME}/lit.site.cfg.py
      MAIN_CONFIG
      ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
      )
    list(APPEND PSAN_DYNAMIC_TESTSUITES
      ${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_NAME})
  endif()
endforeach()

# iOS and iOS simulator test suites
# These are not added into "check-all", in order to run these tests, use
# "check-psan-iossim-x86_64" and similar. They also require an extra environment
# variable to select which iOS device or simulator to use, e.g.:
# SANITIZER_IOSSIM_TEST_DEVICE_IDENTIFIER="iPhone 6"
if(APPLE)
  set(PSAN_TEST_TARGET_CC ${COMPILER_RT_TEST_COMPILER})
  set(PSAN_APPLE_PLATFORMS ${PSAN_SUPPORTED_OS})
  foreach(platform ${PSAN_APPLE_PLATFORMS})
    if ("${platform}" STREQUAL "osx")
      # Skip macOS because it's handled by the code above that builds tests for the host machine.
      continue()
    endif()
    list_intersect(
      PSAN_TEST_${platform}_ARCHS
      PSAN_SUPPORTED_ARCH
      DARWIN_${platform}_ARCHS
    )
    foreach(arch ${PSAN_TEST_${platform}_ARCHS})
      get_test_cflags_for_apple_platform(
        "${platform}"
        "${arch}"
        PSAN_TEST_TARGET_CFLAGS
        )
      string(TOUPPER "${arch}" ARCH_UPPER_CASE)
      get_capitalized_apple_platform("${platform}" PLATFORM_CAPITALIZED)
      set(CONFIG_NAME "${PLATFORM_CAPITALIZED}${ARCH_UPPER_CASE}Config")
      set(PSAN_TEST_CONFIG_SUFFIX "-${arch}-${platform}")
      set(PSAN_TEST_APPLE_PLATFORM "${platform}")
      set(PSAN_TEST_TARGET_ARCH "${arch}")
      set(PSAN_TEST_MIN_DEPLOYMENT_TARGET_FLAG "${DARWIN_${platform}_MIN_VER_FLAG}")
      configure_lit_site_cfg(
        ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
        ${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_NAME}/lit.site.cfg.py
        )
      add_lit_testsuite(check-psan-${platform}-${arch} "PredictiveSanitizer ${platform} ${arch} tests"
        ${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_NAME}/
        EXCLUDE_FROM_CHECK_ALL
        DEPENDS ${PSAN_TEST_DEPS})
    endforeach()
  endforeach()
endif()

if(COMPILER_RT_INCLUDE_TESTS)
  configure_lit_site_cfg(
    ${CMAKE_CURRENT_SOURCE_DIR}/Unit/lit.site.cfg.py.in
    ${CMAKE_CURRENT_BINARY_DIR}/Unit/lit.site.cfg.py)
  if(COMPILER_RT_PSAN_HAS_STATIC_RUNTIME)
    configure_lit_site_cfg(
      ${CMAKE_CURRENT_SOURCE_DIR}/Unit/lit.site.cfg.py.in
      ${CMAKE_CURRENT_BINARY_DIR}/Unit/dynamic/lit.site.cfg.py)
  endif()
  list(APPEND PSAN_TEST_DEPS PsanUnitTests)
  list(APPEND PSAN_TESTSUITES ${CMAKE_CURRENT_BINARY_DIR}/Unit)
  if(COMPILER_RT_PSAN_HAS_STATIC_RUNTIME)
    list(APPEND PSAN_DYNAMIC_TESTSUITES ${CMAKE_CURRENT_BINARY_DIR}/Unit/dynamic)
  endif()
endif()

add_lit_testsuite(check-psan "Running PredictiveSanitizer tests"
  ${PSAN_TESTSUITES}
  DEPENDS ${PSAN_TEST_DEPS})
set_target_properties(check-psan PROPERTIES FOLDER "Compiler-RT Tests")

if(COMPILER_RT_PSAN_HAS_STATIC_RUNTIME)
  add_lit_testsuite(check-psan-dynamic "Running the PredictiveSanitizer tests with dynamic runtime"
                    ${PSAN_DYNAMIC_TESTSUITES}
                    EXCLUDE_FROM_CHECK_ALL
                    DEPENDS ${PSAN_DYNAMIC_TEST_DEPS})
  set_target_properties(check-psan-dynamic PROPERTIES FOLDER "Compiler-RT Misc")
endif()
