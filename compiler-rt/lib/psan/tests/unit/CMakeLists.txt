set(PSAN_UNIT_TEST_SOURCES
  psan_dense_alloc_test.cpp
  psan_flags_test.cpp
  psan_ilist_test.cpp
  psan_mman_test.cpp
  psan_shadow_test.cpp
  psan_stack_test.cpp
  psan_sync_test.cpp
  psan_trace_test.cpp
  psan_unit_test_main.cpp
  psan_vector_clock_test.cpp
  )

add_psan_unittest(PsanUnitTest
  SOURCES ${PSAN_UNIT_TEST_SOURCES})
