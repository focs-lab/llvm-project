include_directories(../rtl)

add_custom_target(PsanUnitTests)
set_target_properties(PsanUnitTests PROPERTIES
  FOLDER "Compiler-RT Tests")

set(PSAN_UNITTEST_CFLAGS
  ${COMPILER_RT_UNITTEST_CFLAGS}
  ${COMPILER_RT_GTEST_CFLAGS}
  ${SANITIZER_TEST_CXX_CFLAGS}
  -I${COMPILER_RT_SOURCE_DIR}/include
  -I${COMPILER_RT_SOURCE_DIR}/lib
  -I${COMPILER_RT_SOURCE_DIR}/lib/psan/rtl
  -DSANITIZER_COMMON_NO_REDEFINE_BUILTINS
  -DGTEST_HAS_RTTI=0
  -fno-rtti
)

if(COMPILER_RT_PSAN_DEBUG_OUTPUT)
  # Need to match these flags with the runtime.
  list(APPEND PSAN_UNITTEST_CFLAGS -DPSAN_COLLECT_STATS=1
                                   -DPSAN_DEBUG_OUTPUT=2)
endif()

append_list_if(COMPILER_RT_HAS_MSSE4_2_FLAG -msse4.2 PSAN_UNITTEST_CFLAGS)

set(PSAN_TEST_ARCH ${PSAN_SUPPORTED_ARCH})

set(PSAN_UNITTEST_LINK_FLAGS
  ${COMPILER_RT_UNITTEST_LINK_FLAGS}
  ${COMPILER_RT_UNWINDER_LINK_LIBS}
  ${SANITIZER_TEST_CXX_LIBRARIES})

if(APPLE)

  # Create a static library for test dependencies.
  set(PSAN_TEST_RUNTIME_OBJECTS
    $<TARGET_OBJECTS:RTPsan_dynamic.osx>
    $<TARGET_OBJECTS:RTInterception.osx>
    $<TARGET_OBJECTS:RTSanitizerCommon.osx>
    $<TARGET_OBJECTS:RTSanitizerCommonLibc.osx>
    $<TARGET_OBJECTS:RTSanitizerCommonCoverage.osx>
    $<TARGET_OBJECTS:RTSanitizerCommonSymbolizer.osx>
    $<TARGET_OBJECTS:RTUbsan.osx>)
  set(PSAN_TEST_RUNTIME RTPsanTest)
  add_library(${PSAN_TEST_RUNTIME} STATIC ${PSAN_TEST_RUNTIME_OBJECTS})
  set_target_properties(${PSAN_TEST_RUNTIME} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  darwin_filter_host_archs(PSAN_SUPPORTED_ARCH PSAN_TEST_ARCH)
  list(APPEND PSAN_UNITTEST_CFLAGS ${DARWIN_osx_CFLAGS})

  list(APPEND PSAN_UNITTEST_LINK_FLAGS ${DARWIN_osx_LINK_FLAGS})
  add_weak_symbols("ubsan" PSAN_UNITTEST_LINK_FLAGS)
  add_weak_symbols("sanitizer_common" PSAN_UNITTEST_LINK_FLAGS)
else()
  list(APPEND PSAN_UNITTEST_LINK_FLAGS -fsanitize=thread)
  list(APPEND PSAN_UNITTEST_LINK_FLAGS -lm)
  list(APPEND PSAN_UNITTEST_LINK_FLAGS ${COMPILER_RT_TEST_LIBDISPATCH_CFLAGS})
endif()

set(PSAN_RTL_HEADERS)
foreach (header ${PSAN_HEADERS})
  list(APPEND PSAN_RTL_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/../${header})
endforeach()

set(PSAN_DEPS psan)
# PSan uses C++ standard library headers.
if (TARGET cxx-headers OR HAVE_LIBCXX)
  list(APPEND PSAN_DEPS cxx-headers)
endif()

# add_psan_unittest(<name>
#                   SOURCES <sources list>
#                   HEADERS <extra headers list>)
macro(add_psan_unittest testname)
  cmake_parse_arguments(TEST "" "" "SOURCES;HEADERS" ${ARGN})
  if(UNIX)
    foreach(arch ${PSAN_TEST_ARCH})
      set(PsanUnitTestsObjects)
      generate_compiler_rt_tests(PsanUnitTestsObjects PsanUnitTests
        "${testname}-${arch}-Test" ${arch}
        SOURCES ${TEST_SOURCES} ${COMPILER_RT_GTEST_SOURCE}
        RUNTIME ${PSAN_TEST_RUNTIME}
        COMPILE_DEPS ${TEST_HEADERS} ${PSAN_RTL_HEADERS}
        DEPS ${PSAN_DEPS}
        CFLAGS ${PSAN_UNITTEST_CFLAGS}
        LINK_FLAGS ${PSAN_UNITTEST_LINK_FLAGS})
    endforeach()
  endif()
endmacro()

if(COMPILER_RT_CAN_EXECUTE_TESTS AND NOT ANDROID)
  add_subdirectory(rtl)
  add_subdirectory(unit)
endif()
