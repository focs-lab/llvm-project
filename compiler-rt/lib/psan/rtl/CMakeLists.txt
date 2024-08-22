include_directories(../..)

set(PSAN_RTL_CFLAGS ${PSAN_CFLAGS})
append_list_if(COMPILER_RT_HAS_MSSE4_2_FLAG -msse4.2 PSAN_RTL_CFLAGS)
append_list_if(SANITIZER_LIMIT_FRAME_SIZE -Wframe-larger-than=530
               PSAN_RTL_CFLAGS)
append_list_if(COMPILER_RT_HAS_WGLOBAL_CONSTRUCTORS_FLAG -Wglobal-constructors
               PSAN_RTL_CFLAGS)
append_list_if(COMPILER_RT_INTERCEPT_LIBDISPATCH ${COMPILER_RT_LIBDISPATCH_CFLAGS}
               PSAN_RTL_CFLAGS)

set(PSAN_RTL_DYNAMIC_CFLAGS ${PSAN_RTL_CFLAGS})
list(REMOVE_ITEM PSAN_RTL_DYNAMIC_CFLAGS -fPIE)

set(PSAN_DYNAMIC_LINK_LIBS
  ${COMPILER_RT_UNWINDER_LINK_LIBS}
  ${SANITIZER_CXX_ABI_LIBRARIES}
  ${SANITIZER_COMMON_LINK_LIBS})

append_list_if(COMPILER_RT_HAS_LIBDL dl PSAN_DYNAMIC_LINK_LIBS)
append_list_if(COMPILER_RT_HAS_LIBM m PSAN_DYNAMIC_LINK_LIBS)
append_list_if(COMPILER_RT_HAS_LIBPTHREAD pthread PSAN_DYNAMIC_LINK_LIBS)

set(PSAN_SOURCES
  psan_debugging.cpp
  psan_external.cpp
  psan_fd.cpp
  psan_flags.cpp
  psan_ignoreset.cpp
  psan_interceptors_memintrinsics.cpp
  psan_interceptors_posix.cpp
  psan_interface.cpp
  psan_interface_ann.cpp
  psan_interface_atomic.cpp
  psan_interface_java.cpp
  psan_malloc_mac.cpp
  psan_md5.cpp
  psan_mman.cpp
  psan_mutexset.cpp
  psan_report.cpp
  psan_rtl.cpp
  psan_rtl_access.cpp
  psan_rtl_mutex.cpp
  psan_rtl_proc.cpp
  psan_rtl_report.cpp
  psan_rtl_thread.cpp
  psan_stack_trace.cpp
  psan_suppressions.cpp
  psan_symbolize.cpp
  psan_sync.cpp
  psan_vector_clock.cpp
  )

set(PSAN_CXX_SOURCES
  psan_new_delete.cpp
  )

set(PSAN_PREINIT_SOURCES
  psan_preinit.cpp
  )

if(APPLE)
  list(APPEND PSAN_SOURCES
    psan_interceptors_mac.cpp
    psan_interceptors_mach_vm.cpp
    psan_platform_mac.cpp
    psan_platform_posix.cpp
    )
elseif(UNIX)
  # Assume Linux
  list(APPEND PSAN_SOURCES
    psan_platform_linux.cpp
    psan_platform_posix.cpp
    )
endif()

if(COMPILER_RT_INTERCEPT_LIBDISPATCH)
  list(APPEND PSAN_SOURCES
    psan_interceptors_libdispatch.cpp
    )
endif()

set(PSAN_HEADERS
  psan_defs.h
  psan_dense_alloc.h
  psan_fd.h
  psan_flags.h
  psan_flags.inc
  psan_ignoreset.h
  psan_ilist.h
  psan_interceptors.h
  psan_interface.h
  psan_interface.inc
  psan_interface_ann.h
  psan_interface_java.h
  psan_mman.h
  psan_mutexset.h
  psan_platform.h
  psan_ppc_regs.h
  psan_report.h
  psan_rtl.h
  psan_shadow.h
  psan_stack_trace.h
  psan_suppressions.h
  psan_symbolize.h
  psan_sync.h
  psan_trace.h
  psan_vector_clock.h
  )

set(PSAN_RUNTIME_LIBRARIES)
add_compiler_rt_component(psan)

if("${CMAKE_C_FLAGS}" MATCHES "-Wno-(error=)?unused-command-line-argument")
  set(EXTRA_CFLAGS "-Wno-error=unused-command-line-argument ${EXTRA_CFLAGS}")
endif()

if(APPLE)
  # Ideally we would check the SDK version for the actual platform we are
  # building for here.  To make our lifes easier we assume the host SDK setup is
  # sane and use the macOS SDK version as a proxy for aligned SDKs.
  find_darwin_sdk_version(macosx_sdk_version "macosx")
  if ("${macosx_sdk_version}" VERSION_LESS 10.12)
    message(FATAL_ERROR "Building the PSan runtime requires at least macOS SDK 10.12 (or aligned SDK on other platforms)")
  endif()

  set(PSAN_ASM_SOURCES
    psan_rtl_amd64.S
    psan_rtl_aarch64.S
    )

  set(PSAN_LINK_LIBS ${SANITIZER_COMMON_LINK_LIBS})

  add_weak_symbols("ubsan" WEAK_SYMBOL_LINK_FLAGS)
  add_weak_symbols("sanitizer_common" WEAK_SYMBOL_LINK_FLAGS)

  add_compiler_rt_runtime(clang_rt.psan
    SHARED
    OS ${PSAN_SUPPORTED_OS}
    ARCHS ${PSAN_SUPPORTED_ARCH}
    SOURCES ${PSAN_SOURCES} ${PSAN_CXX_SOURCES} ${PSAN_ASM_SOURCES}
    ADDITIONAL_HEADERS ${PSAN_HEADERS}
    OBJECT_LIBS RTInterception
                RTSanitizerCommon
                RTSanitizerCommonLibc
                RTSanitizerCommonCoverage
                RTSanitizerCommonSymbolizer
                RTUbsan
    CFLAGS ${PSAN_RTL_CFLAGS}
    LINK_FLAGS ${SANITIZER_COMMON_LINK_FLAGS} ${WEAK_SYMBOL_LINK_FLAGS}
    LINK_LIBS ${PSAN_LINK_LIBS} objc
    PARENT_TARGET psan)
  add_compiler_rt_object_libraries(RTPsan_dynamic
    OS ${PSAN_SUPPORTED_OS}
    ARCHS ${PSAN_SUPPORTED_ARCH}
    SOURCES ${PSAN_SOURCES} ${PSAN_CXX_SOURCES} ${PSAN_ASM_SOURCES}
    ADDITIONAL_HEADERS ${PSAN_HEADERS}
    CFLAGS ${PSAN_RTL_CFLAGS})

  # Build and check Go runtime.
  set(BUILDGO_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../go/buildgo.sh)
  add_custom_target(GopsanRuntimeCheck
    COMMAND env "CC=${CMAKE_C_COMPILER} ${OSX_SYSROOT_FLAG}"
            EXTRA_CFLAGS=${EXTRA_CFLAGS}
            IN_TMPDIR=1 SILENT=1 ${BUILDGO_SCRIPT}
    DEPENDS psan ${BUILDGO_SCRIPT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../go
    COMMENT "Checking PSan Go runtime..."
    VERBATIM)
  set_target_properties(GopsanRuntimeCheck PROPERTIES FOLDER "Compiler-RT Misc")
else()
  foreach(arch ${PSAN_SUPPORTED_ARCH})
    if(arch STREQUAL "x86_64")
      set(PSAN_ASM_SOURCES
        psan_rtl_amd64.S
        )
      # Check for Go runtime.
      set(BUILDGO_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../go/buildgo.sh)
      add_custom_target(GopsanRuntimeCheck
        COMMAND env "CC=${CMAKE_C_COMPILER} ${CMAKE_C_COMPILER_ARG1}"
                EXTRA_CFLAGS=${EXTRA_CFLAGS}
                IN_TMPDIR=1 SILENT=1 ${BUILDGO_SCRIPT}
        DEPENDS clang_rt.psan-${arch} ${BUILDGO_SCRIPT}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../go
        COMMENT "Checking PSan Go runtime..."
        VERBATIM)
    elseif(arch STREQUAL "aarch64")
      set(PSAN_ASM_SOURCES
        psan_rtl_aarch64.S
        )
      # Check for Go runtime.
      set(BUILDGO_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../go/buildgo.sh)
      add_custom_target(GopsanRuntimeCheck
	COMMAND env "CC=${CMAKE_C_COMPILER} ${CMAKE_C_COMPILER_ARG1}"
		EXTRA_CFLAGS=${EXTRA_CFLAGS}
		IN_TMPDIR=1 SILENT=1 ${BUILDGO_SCRIPT}
	DEPENDS clang_rt.psan-${arch} ${BUILDGO_SCRIPT}
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../go
	COMMENT "Checking PSan Go runtime..."
	VERBATIM)
    elseif(arch MATCHES "powerpc64|powerpc64le")
      set(PSAN_ASM_SOURCES
        psan_rtl_ppc64.S
        )
      # Check for Go runtime.
      set(BUILDGO_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../go/buildgo.sh)
      add_custom_target(GopsanRuntimeCheck
	COMMAND env "CC=${CMAKE_C_COMPILER} ${CMAKE_C_COMPILER_ARG1}"
		EXTRA_CFLAGS=${EXTRA_CFLAGS}
		IN_TMPDIR=1 SILENT=1 ${BUILDGO_SCRIPT}
	DEPENDS clang_rt.psan-${arch} ${BUILDGO_SCRIPT}
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../go
	COMMENT "Checking PSan Go runtime..."
	VERBATIM)
    elseif(arch MATCHES "loongarch64")
      set(PSAN_ASM_SOURCES
        psan_rtl_loongarch64.S
        )
    elseif(arch MATCHES "mips64|mips64le")
      set(PSAN_ASM_SOURCES
        psan_rtl_mips64.S
        )
    elseif(arch MATCHES "riscv64")
      set(PSAN_ASM_SOURCES
        psan_rtl_riscv64.S
        )
    elseif(arch MATCHES "s390x")
      set(PSAN_ASM_SOURCES
        psan_rtl_s390x.S
        )
      # Check for Go runtime.
      set(BUILDGO_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../go/buildgo.sh)
      add_custom_target(GopsanRuntimeCheck
	COMMAND env "CC=${CMAKE_C_COMPILER} ${CMAKE_C_COMPILER_ARG1}"
		EXTRA_CFLAGS=${EXTRA_CFLAGS}
		IN_TMPDIR=1 SILENT=1 ${BUILDGO_SCRIPT}
	DEPENDS clang_rt.psan-${arch} ${BUILDGO_SCRIPT}
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../go
	COMMENT "Checking PSan Go runtime..."
	VERBATIM)
    else()
      set(PSAN_ASM_SOURCES)
    endif()
    add_compiler_rt_runtime(clang_rt.psan
      STATIC
      ARCHS ${arch}
      SOURCES ${PSAN_SOURCES} ${PSAN_ASM_SOURCES} ${PSAN_PREINIT_SOURCES}
              $<TARGET_OBJECTS:RTInterception.${arch}>
              $<TARGET_OBJECTS:RTSanitizerCommon.${arch}>
              $<TARGET_OBJECTS:RTSanitizerCommonLibc.${arch}>
              $<TARGET_OBJECTS:RTSanitizerCommonCoverage.${arch}>
              $<TARGET_OBJECTS:RTSanitizerCommonSymbolizer.${arch}>
              $<TARGET_OBJECTS:RTSanitizerCommonSymbolizerInternal.${arch}>
              $<TARGET_OBJECTS:RTUbsan.${arch}>
      ADDITIONAL_HEADERS ${PSAN_HEADERS}
      CFLAGS ${PSAN_RTL_CFLAGS}
      PARENT_TARGET psan)
    add_compiler_rt_runtime(clang_rt.psan_cxx
      STATIC
      ARCHS ${arch}
      SOURCES ${PSAN_CXX_SOURCES}
              $<TARGET_OBJECTS:RTUbsan_cxx.${arch}>
      ADDITIONAL_HEADERS ${PSAN_HEADERS}
      CFLAGS ${PSAN_RTL_CFLAGS}
      PARENT_TARGET psan)
    list(APPEND PSAN_RUNTIME_LIBRARIES clang_rt.psan-${arch}
                                       clang_rt.psan_cxx-${arch})
    add_compiler_rt_runtime(clang_rt.psan
      SHARED
      ARCHS ${arch}
      SOURCES ${PSAN_SOURCES} ${PSAN_ASM_SOURCES}
              $<TARGET_OBJECTS:RTInterception.${arch}>
              $<TARGET_OBJECTS:RTSanitizerCommon.${arch}>
              $<TARGET_OBJECTS:RTSanitizerCommonLibc.${arch}>
              $<TARGET_OBJECTS:RTSanitizerCommonCoverage.${arch}>
              $<TARGET_OBJECTS:RTSanitizerCommonSymbolizer.${arch}>
              $<TARGET_OBJECTS:RTSanitizerCommonSymbolizerInternal.${arch}>
              $<TARGET_OBJECTS:RTUbsan.${arch}>
      ADDITIONAL_HEADERS ${PSAN_HEADERS}
      CFLAGS ${PSAN_RTL_DYNAMIC_CFLAGS}
      DEFS SANITIZER_SHARED
      LINK_LIBS ${PSAN_DYNAMIC_LINK_LIBS}
      LINK_FLAGS ${SANITIZER_COMMON_LINK_FLAGS}
      PARENT_TARGET psan)
    add_sanitizer_rt_symbols(clang_rt.psan
      ARCHS ${arch}
      EXTRA psan.syms.extra)
    add_sanitizer_rt_symbols(clang_rt.psan_cxx
      ARCHS ${arch}
      EXTRA psan.syms.extra)
    add_dependencies(psan clang_rt.psan-${arch}
                          clang_rt.psan_cxx-${arch}
                          clang_rt.psan-${arch}-symbols
                          clang_rt.psan_cxx-${arch}-symbols)
  endforeach()
endif()


