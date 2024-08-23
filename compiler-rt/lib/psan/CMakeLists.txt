# Build for the PredictiveSanitizer runtime support library.

set(PSAN_CFLAGS ${SANITIZER_COMMON_CFLAGS})
# SANITIZER_COMMON_CFLAGS contains -fPIC, but it's performance-critical for
# PSan runtime to be built with -fPIE to reduce the number of register spills.
# On FreeBSD however it provokes linkage issue thus we disable it.
if(NOT CMAKE_SYSTEM MATCHES "FreeBSD")
  append_list_if(COMPILER_RT_HAS_FPIE_FLAG -fPIE PSAN_CFLAGS)
endif()
append_rtti_flag(OFF PSAN_CFLAGS)

if(COMPILER_RT_PSAN_DEBUG_OUTPUT)
  # Add extra debug information to PSan runtime. This configuration is rarely
  # used, but we need to support it so that debug output will not bitrot.
  list(APPEND PSAN_CFLAGS -DPSAN_DEBUG_OUTPUT=2)
endif()

# Add the actual runtime library.
add_subdirectory(rtl)

# Build libcxx instrumented with PSan.
if(COMPILER_RT_LIBCXX_PATH AND
   COMPILER_RT_LIBCXXABI_PATH AND
   COMPILER_RT_TEST_COMPILER_ID STREQUAL "Clang" AND
   NOT ANDROID)
  set(libcxx_psan_deps)
  foreach(arch ${PSAN_SUPPORTED_ARCH})
    get_target_flags_for_arch(${arch} TARGET_CFLAGS)
    set(LIBCXX_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/libcxx_psan_${arch})
    add_custom_libcxx(libcxx_psan_${arch} ${LIBCXX_PREFIX}
      DEPS ${PSAN_RUNTIME_LIBRARIES}
      CFLAGS ${TARGET_CFLAGS} -fsanitize=predict
      USE_TOOLCHAIN)
    list(APPEND libcxx_psan_deps libcxx_psan_${arch}-build)
  endforeach()

  add_custom_target(libcxx_psan DEPENDS ${libcxx_psan_deps})
  set_target_properties(libcxx_psan PROPERTIES FOLDER "Compiler-RT Misc")
endif()

if(COMPILER_RT_INCLUDE_TESTS)
  add_subdirectory(tests)
endif()
