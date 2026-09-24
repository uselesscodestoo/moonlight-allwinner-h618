# Private, pinned CedarC build. No dependency discovery, downloads or install
# rules for Cedar libraries. Include this module and call once per build tree.
get_filename_component(_K2B_CEDAR_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(k2b_add_cedar_runtime)
  set(K2B_CEDARC_ARCHIVE "" CACHE FILEPATH "Pinned Tina 243f2cbe CedarC source archive")
  set(K2B_VENDOR_CEDAR_HEADERS "" CACHE PATH "Pinned vendor Linux 5.4 cedar-ve header directory")
  if(NOT EXISTS "${K2B_CEDARC_ARCHIVE}" OR IS_DIRECTORY "${K2B_CEDARC_ARCHIVE}")
    message(FATAL_ERROR "K2B CedarC archive is missing: ${K2B_CEDARC_ARCHIVE}")
  endif()
  file(SHA256 "${K2B_CEDARC_ARCHIVE}" archive_hash)
  if(NOT archive_hash STREQUAL "ee32abb0100b6763b11d7db61d69865c43adeff07acc2be99b28edd95b85ed04")
    message(FATAL_ERROR "K2B CedarC archive SHA256 mismatch: ${archive_hash}")
  endif()
  set(vendor_header "${K2B_VENDOR_CEDAR_HEADERS}/cedar_ve.h")
  if(NOT EXISTS "${vendor_header}" OR IS_DIRECTORY "${vendor_header}")
    message(FATAL_ERROR "K2B vendor cedar_ve.h is missing: ${vendor_header}")
  endif()
  file(SHA256 "${vendor_header}" vendor_hash)
  if(NOT vendor_hash STREQUAL "42910bf9b511239b4589fd18616606e5b33c523eef4e607a97fd25e0fb2de24b")
    message(FATAL_ERROR "K2B vendor cedar_ve.h SHA256 mismatch: ${vendor_hash}")
  endif()

  include(CheckCSourceCompiles)
  # Never accept a processor string or a cached successful compiler probe alone.
  unset(K2B_CEDAR_COMPILER_ABI CACHE)
  check_c_source_compiles("
    #if !defined(__linux__) || !defined(__aarch64__)
    #error Linux AArch64 required
    #endif
    typedef char pointer_must_be_64_bit[sizeof(void*) == 8 ? 1 : -1];
    int main(void) { return 0; }" K2B_CEDAR_COMPILER_ABI)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT K2B_CEDAR_COMPILER_ABI)
    message(FATAL_ERROR "K2B runtime requires a Linux AArch64 64-bit compiler")
  endif()

  set(managed "${CMAKE_CURRENT_BINARY_DIR}/managed-source")
  set(source "${managed}/libcedarc_v2.0-243f2cbe84a817344d2502f4dd3d66b81338282f-243f2cbe84a817344d2502f4dd3d66b81338282f")
  file(MAKE_DIRECTORY "${managed}")
  # This directory is managed build output, overwritten from verified input
  # on every configure. No externally maintained source tree is touched.
  execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${K2B_CEDARC_ARCHIVE}"
    WORKING_DIRECTORY "${managed}" RESULT_VARIABLE extract_result)
  if(NOT extract_result EQUAL 0 OR NOT EXISTS "${source}/include/vdecoder.h")
    message(FATAL_ERROR "K2B CedarC archive extraction failed or pinned top directory is missing")
  endif()
  set(runtime "${CMAKE_CURRENT_BINARY_DIR}/runtime")
  file(MAKE_DIRECTORY "${runtime}")
  # A separate, opt-in preload artifact. Never link it into the nine-library
  # Cedar closure or Moonlight; the explicit private launcher controls activation.
  add_library(k2b_cedar54_compat SHARED
    "${_K2B_CEDAR_PROJECT_ROOT}/src/video/k2b/cedar54_compat.c"
    "${_K2B_CEDAR_PROJECT_ROOT}/src/video/k2b/cedar54_ioctl_aarch64.S")
  set_target_properties(k2b_cedar54_compat PROPERTIES
    OUTPUT_NAME k2b_cedar54_compat
    C_STANDARD 99 C_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES
    LIBRARY_OUTPUT_DIRECTORY "${runtime}" LINK_FLAGS "-Wl,-z,defs")
  target_compile_options(k2b_cedar54_compat PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-Wall>" "$<$<COMPILE_LANGUAGE:C>:-Wextra>"
    "$<$<COMPILE_LANGUAGE:C>:-Werror>")

  # Built but never registered for automatic execution, including cross builds.
  add_executable(k2b_cedar54_preload_test
    "${_K2B_CEDAR_PROJECT_ROOT}/tests/k2b/test_cedar54_preload.c")
  set_target_properties(k2b_cedar54_preload_test PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES RUNTIME_OUTPUT_DIRECTORY "${runtime}")
  target_compile_options(k2b_cedar54_preload_test PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(k2b_cedar54_preload_test PRIVATE ${CMAKE_DL_LIBS})

  file(STRINGS "${_K2B_CEDAR_PROJECT_ROOT}/docs/k2b-cedarc-blobs.sha256" manifest)
  list(LENGTH manifest manifest_count)
  if(NOT manifest_count EQUAL 4)
    message(FATAL_ERROR "K2B CedarC blob manifest must contain exactly four entries")
  endif()
  foreach(blob VE videoengine awh264 vdecVcs)
    set(relative "library/aarch64-none-linux-gnu/lib${blob}.so")
    set(binary "${source}/${relative}")
    if(NOT EXISTS "${binary}")
      message(FATAL_ERROR "K2B CedarC blob is missing: ${relative}")
    endif()
    file(SHA256 "${binary}" blob_hash)
    list(FIND manifest "${blob_hash}  ${relative}" manifest_index)
    if(manifest_index EQUAL -1)
      message(FATAL_ERROR "K2B CedarC blob SHA256 mismatch: ${relative}")
    endif()
    configure_file("${binary}" "${runtime}/lib${blob}.so" COPYONLY)
    add_library(k2b_${blob} SHARED IMPORTED)
    set_target_properties(k2b_${blob} PROPERTIES
      IMPORTED_LOCATION "${runtime}/lib${blob}.so"
      IMPORTED_SONAME "lib${blob}.so")
  endforeach()

  set(common_includes "${source}/include" "${source}/base/include"
    "${source}/base/include/gralloc_metadata" "${source}/base/filesink"
    "${source}/base/filesink/include" "${source}/vdecoder"
    "${source}/vdecoder/include" "${source}/vdecoder/aftertreatment")
  set(common_definitions TINA_LINUX_SUPPORT=0 CEDAR_TINA CONF_USE_IOMMU=1
    CONF_KERNEL_VERSION_5_4 CONF_VE_ENCODER_VERSION_1 ENABLE_AFTERTREATMENT=0)

  add_library(k2b_cedar_abi_check OBJECT "${_K2B_CEDAR_PROJECT_ROOT}/tests/k2b/check_cedar_abi.c")
  target_include_directories(k2b_cedar_abi_check PRIVATE ${common_includes})
  target_compile_definitions(k2b_cedar_abi_check PRIVATE ${common_definitions})
  set_target_properties(k2b_cedar_abi_check PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)

  add_library(k2b_cdc_base SHARED
    "${source}/base/CdcIonUtil.c" "${source}/base/CdcLog.c"
    "${source}/base/CdcMalloc.c" "${source}/base/CdcMessageQueue.c"
    "${source}/base/CdcSinkInterface.c" "${source}/base/CdcSysinfo.c"
    "${source}/base/CdcTimeUtil.c"
    "${source}/base/cdcIniparser/cdcDictionary.c"
    "${source}/base/cdcIniparser/cdcIniparser.c"
    "${source}/base/cdcIniparser/cdcIniparserapi.c"
    "${source}/base/filesink/CdcBSSink.c" "${source}/base/filesink/CdcPicSink.c"
    "${source}/base/filesink/SinkMd5.c")
  add_library(k2b_MemAdapter SHARED "${_K2B_CEDAR_PROJECT_ROOT}/src/video/k2b/cedar_memory.c")
  add_library(k2b_sbm SHARED
    "${source}/vdecoder/sbm/sbmStream.c" "${source}/vdecoder/sbm/sbmHwProcess.c"
    "${source}/vdecoder/sbm/sbmFrameH264.c" "${source}/vdecoder/sbm/sbmFrameH265.c"
    "${source}/vdecoder/sbm/sbmFrameAvs2.c" "${source}/vdecoder/sbm/sbmFrameBase.c")
  add_library(k2b_fbm SHARED "${source}/vdecoder/fbm/fbm.c")
  add_library(k2b_vdecoder SHARED "${source}/vdecoder/pixel_format.c" "${source}/vdecoder/vdecoder.c")

  find_package(Threads REQUIRED)
  # Only this loader is linked by the pure-load check. Cedar symbols are
  # resolved at runtime after private-path and preload-provider validation.
  add_library(k2b_cedar_runtime STATIC
    "${_K2B_CEDAR_PROJECT_ROOT}/src/video/k2b/cedar_runtime.c"
    "${_K2B_CEDAR_PROJECT_ROOT}/src/video/k2b/cedar_picture.c"
    "${_K2B_CEDAR_PROJECT_ROOT}/src/video/k2b/frame.c")
  target_include_directories(k2b_cedar_runtime PUBLIC ${common_includes}
    "${_K2B_CEDAR_PROJECT_ROOT}/src/video/k2b")
  target_compile_definitions(k2b_cedar_runtime PUBLIC ${common_definitions})
  set_target_properties(k2b_cedar_runtime PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)
  target_compile_options(k2b_cedar_runtime PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(k2b_cedar_runtime PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  add_dependencies(k2b_cedar_runtime k2b_cedar_abi_check)

  # Build only: this target is never registered as an automatic test.
  add_executable(k2b_runtime_load_check
    "${_K2B_CEDAR_PROJECT_ROOT}/tests/k2b/runtime_load_check.c")
  set_target_properties(k2b_runtime_load_check PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES RUNTIME_OUTPUT_DIRECTORY "${runtime}")
  target_compile_options(k2b_runtime_load_check PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(k2b_runtime_load_check PRIVATE k2b_cedar_runtime)

  foreach(lib cdc_base MemAdapter sbm fbm vdecoder)
    target_include_directories(k2b_${lib} PRIVATE ${common_includes})
    target_compile_definitions(k2b_${lib} PRIVATE ${common_definitions})
    set_target_properties(k2b_${lib} PROPERTIES OUTPUT_NAME "${lib}"
      C_STANDARD 99 C_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES
      LIBRARY_OUTPUT_DIRECTORY "${runtime}"
      BUILD_WITH_INSTALL_RPATH YES INSTALL_RPATH "$ORIGIN"
      LINK_FLAGS "-Wl,-z,defs")
    target_link_libraries(k2b_${lib} PRIVATE Threads::Threads)
    add_dependencies(k2b_${lib} k2b_cedar_abi_check)
  endforeach()
  foreach(lib cdc_base sbm fbm vdecoder)
    target_compile_definitions(k2b_${lib} PRIVATE _GNU_SOURCE)
  endforeach()
  # cedar_memory.c already defines _GNU_SOURCE before any includes.
  target_include_directories(k2b_MemAdapter PRIVATE "${K2B_VENDOR_CEDAR_HEADERS}")
  target_compile_options(k2b_MemAdapter PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(k2b_cdc_base PRIVATE rt)
  target_link_libraries(k2b_sbm PRIVATE k2b_cdc_base k2b_MemAdapter k2b_VE k2b_videoengine ${CMAKE_DL_LIBS} m)
  target_link_libraries(k2b_fbm PRIVATE k2b_cdc_base k2b_MemAdapter k2b_VE k2b_videoengine)
  target_link_libraries(k2b_vdecoder PRIVATE k2b_sbm k2b_fbm k2b_cdc_base k2b_MemAdapter k2b_VE k2b_videoengine ${CMAKE_DL_LIBS} m)

  add_executable(k2b_runtime_link_check "${_K2B_CEDAR_PROJECT_ROOT}/tests/k2b/runtime_link_check.c")
  target_include_directories(k2b_runtime_link_check PRIVATE ${common_includes}
    "${_K2B_CEDAR_PROJECT_ROOT}/src/video/k2b")
  target_compile_definitions(k2b_runtime_link_check PRIVATE ${common_definitions})
  set_target_properties(k2b_runtime_link_check PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES RUNTIME_OUTPUT_DIRECTORY "${runtime}"
    BUILD_WITH_INSTALL_RPATH YES INSTALL_RPATH "$ORIGIN"
    LINK_FLAGS "-Wl,--no-as-needed -Wl,--no-allow-shlib-undefined")
  target_link_libraries(k2b_runtime_link_check PRIVATE k2b_vdecoder k2b_sbm k2b_fbm
    k2b_cdc_base k2b_MemAdapter k2b_VE k2b_videoengine k2b_awh264 k2b_vdecVcs
    Threads::Threads ${CMAKE_DL_LIBS} m rt)
endfunction()
