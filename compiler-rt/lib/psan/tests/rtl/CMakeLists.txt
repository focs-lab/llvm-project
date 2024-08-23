set(PSAN_RTL_TEST_SOURCES
  psan_bench.cpp
  psan_mop.cpp
  psan_mutex.cpp
  psan_posix.cpp
  psan_string.cpp
  psan_test.cpp
  psan_thread.cpp
  )

if(UNIX)
  list(APPEND PSAN_RTL_TEST_SOURCES
    psan_test_util_posix.cpp
    )
endif()

set(PSAN_RTL_TEST_HEADERS
  psan_test_util.h
  )

add_psan_unittest(PsanRtlTest
  SOURCES ${PSAN_RTL_TEST_SOURCES}
  HEADERS ${PSAN_RTL_TEST_HEADERS})
