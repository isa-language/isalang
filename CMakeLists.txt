cmake_minimum_required(VERSION 3.20)
project(
  Isac
  VERSION 0.2
  LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(
  isac_frontend
  src/frontend.cpp
  src/intrinsics.cpp
  src/macros.cpp
  src/diagnostics.cpp
  src/primitives.cpp
  src/modules.cpp
  src/hir.cpp
  src/core.cpp
  src/core_types.cpp
  src/frame_lifecycle.cpp
  src/async.cpp
  src/async_borrows.cpp
  src/async_lowering.cpp
  src/execution_storage.cpp
  src/thread_contracts.cpp
  src/erasure.cpp
  src/runtime.cpp
  src/liveness.cpp
  src/regions.cpp
  src/region_arguments.cpp
  src/field_region_arguments.cpp
  src/parent_obligations.cpp
  src/projection_bounds.cpp
  src/storage.cpp
  src/storage_paths.cpp
  src/element_bounds.cpp
  src/storage_slots.cpp
  src/carriers.cpp
  src/references.cpp
  src/reference_inference.cpp
  src/concurrency.cpp
  src/execution.cpp
  src/frames.cpp
  src/frame_lowering.cpp
  src/frame_heap.cpp
  src/frame_outline.cpp
  src/frame_state.cpp
  src/frame_step.cpp
  src/frame_projection.cpp
  src/frame_branch.cpp
  src/frame_deferred.cpp
  src/container_instances.cpp
  src/consteval.cpp
  src/ffi.cpp)
target_include_directories(isac_frontend PUBLIC include)
set(ISAC_LIBRARY_ROOT
    "/usr/lib/isac/lib"
    CACHE PATH "Path to the trusted Isalang standard library")
target_compile_definitions(isac_frontend PRIVATE ISAC_LIBRARY_ROOT="${ISAC_LIBRARY_ROOT}")
target_compile_options(isac_frontend PRIVATE -Wall -Wextra -Wpedantic)

add_executable(isac src/main.cpp)
target_link_libraries(isac PRIVATE isac_frontend)

find_package(MLIR REQUIRED CONFIG HINTS /usr/local/lib/cmake/mlir)
find_program(ISAC_CLANG NAMES clang-${LLVM_VERSION_MAJOR} clang REQUIRED)
set(ISAC_ALLOCATOR_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/isac_allocator_libc.c"
    CACHE FILEPATH "Single allocator provider linked into the Isac runtime")
get_filename_component(ISAC_ALLOCATOR_SOURCE "${ISAC_ALLOCATOR_SOURCE}" ABSOLUTE BASE_DIR
                       "${CMAKE_CURRENT_SOURCE_DIR}")
find_package(Threads REQUIRED)
add_library(isac_runtime STATIC runtime/src/isac_runtime.c runtime/src/isac_threads.c
                                "${ISAC_ALLOCATOR_SOURCE}")
target_link_libraries(isac_runtime PUBLIC Threads::Threads)
target_include_directories(isac_runtime PUBLIC runtime/include)
target_compile_options(isac_runtime PRIVATE -Wall -Wextra -Wpedantic)

option(ISAC_USE_INSTALLED_RUNTIME "Use a runtime installation for generated native programs" OFF)
set(ISAC_RUNTIME_ROOT
    "/usr/lib/isac/runtime"
    CACHE PATH "Installed Isac runtime directory")
if(ISAC_USE_INSTALLED_RUNTIME)
  set(ISAC_NATIVE_RUNTIME_SOURCE "${ISAC_RUNTIME_ROOT}/src/isac_runtime.c")
  set(ISAC_NATIVE_THREAD_SOURCE "${ISAC_RUNTIME_ROOT}/src/isac_threads.c")
  set(ISAC_NATIVE_RUNTIME_INCLUDE "${ISAC_RUNTIME_ROOT}/include")
  set(ISAC_NATIVE_RUNTIME_LIBRARY "${ISAC_RUNTIME_ROOT}/libisac_runtime.a")

  if(EXISTS "${ISAC_RUNTIME_ROOT}/src/isac_allocator_libc.c")
    set(ISAC_NATIVE_ALLOCATOR_SOURCE "${ISAC_RUNTIME_ROOT}/src/isac_allocator_libc.c")
  else()
    set(ISAC_NATIVE_ALLOCATOR_SOURCE "")
  endif()
  set(ISAC_INSTALLED_ALLOCATOR_SOURCE
      "${ISAC_NATIVE_ALLOCATOR_SOURCE}"
      CACHE
        FILEPATH
        "Optional allocator source matching the installed archive; empty uses the archive provider")
  set(ISAC_NATIVE_ALLOCATOR_SOURCE "${ISAC_INSTALLED_ALLOCATOR_SOURCE}")
  if(ISAC_NATIVE_ALLOCATOR_SOURCE AND NOT EXISTS "${ISAC_NATIVE_ALLOCATOR_SOURCE}")
    message(FATAL_ERROR "Missing installed allocator provider: ${ISAC_NATIVE_ALLOCATOR_SOURCE}")
  endif()
  foreach(REQUIRED_RUNTIME_FILE IN
          ITEMS "${ISAC_NATIVE_RUNTIME_SOURCE}" "${ISAC_NATIVE_RUNTIME_INCLUDE}/isac_runtime.h"
                "${ISAC_NATIVE_RUNTIME_LIBRARY}")
    if(NOT EXISTS "${REQUIRED_RUNTIME_FILE}")
      message(FATAL_ERROR "Missing installed runtime file: ${REQUIRED_RUNTIME_FILE}")
    endif()
  endforeach()
else()
  set(ISAC_NATIVE_RUNTIME_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/isac_runtime.c")
  set(ISAC_NATIVE_THREAD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/isac_threads.c")
  set(ISAC_NATIVE_RUNTIME_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
  set(ISAC_NATIVE_RUNTIME_LIBRARY "$<TARGET_FILE:isac_runtime>")
  set(ISAC_NATIVE_ALLOCATOR_SOURCE "${ISAC_ALLOCATOR_SOURCE}")
endif()

set(ISAC_ALLOCATOR_THREAD_CONTRACT
    "auto"
    CACHE STRING "Unsafe provider contract: auto, cross-thread (including free), or local-only")
set_property(CACHE ISAC_ALLOCATOR_THREAD_CONTRACT PROPERTY STRINGS auto cross-thread local-only)
set(ISAC_ALLOCATOR_CROSS_THREAD 0)
if(ISAC_ALLOCATOR_THREAD_CONTRACT STREQUAL "cross-thread")
  set(ISAC_ALLOCATOR_CROSS_THREAD 1)
elseif(ISAC_ALLOCATOR_THREAD_CONTRACT STREQUAL "auto")
  if(NOT ISAC_USE_INSTALLED_RUNTIME
     AND ISAC_ALLOCATOR_SOURCE STREQUAL
         "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/isac_allocator_libc.c")
    set(ISAC_ALLOCATOR_CROSS_THREAD 1)
  endif()
elseif(NOT ISAC_ALLOCATOR_THREAD_CONTRACT STREQUAL "local-only")
  message(FATAL_ERROR "Invalid ISAC_ALLOCATOR_THREAD_CONTRACT")
endif()
target_compile_definitions(isac_frontend
                           PRIVATE ISAC_ALLOCATOR_CROSS_THREAD=${ISAC_ALLOCATOR_CROSS_THREAD})

add_library(isac_backend src/backend.cpp src/native.cpp src/target.cpp)
target_include_directories(isac_backend PUBLIC include)
target_include_directories(isac_backend SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_definitions(
  isac_backend
  PRIVATE ${LLVM_DEFINITIONS}
          ISAC_CLANG="${ISAC_CLANG}"
          ISAC_THREAD_SOURCE="${ISAC_NATIVE_THREAD_SOURCE}"
          ISAC_RUNTIME_SOURCE="${ISAC_NATIVE_RUNTIME_SOURCE}"
          ISAC_ALLOCATOR_SOURCE="${ISAC_NATIVE_ALLOCATOR_SOURCE}"
          ISAC_RUNTIME_INCLUDE="${ISAC_NATIVE_RUNTIME_INCLUDE}"
          ISAC_RUNTIME_LIBRARY="${ISAC_NATIVE_RUNTIME_LIBRARY}")
if(NOT ISAC_USE_INSTALLED_RUNTIME)
  add_dependencies(isac_backend isac_runtime)
endif()
target_link_libraries(
  isac_backend
  PRIVATE MLIRIR
          MLIRPass
          MLIRArithDialect
          MLIRControlFlowDialect
          MLIRFuncDialect
          MLIRMemRefDialect
          MLIRLLVMDialect
          MLIRMemRefTransforms
          MLIRAffineToStandard
          MLIRArithToLLVM
          MLIRControlFlowToLLVM
          MLIRFuncToLLVM
          MLIRMemRefToLLVM
          MLIRReconcileUnrealizedCasts
          MLIRBuiltinToLLVMIRTranslation
          MLIRLLVMToLLVMIRTranslation
          MLIRTargetLLVMIRExport)
llvm_map_components_to_libnames(ISAC_LLVM_TARGET_LIBS ${LLVM_TARGETS_TO_BUILD} Passes AsmParser
                                Linker)
target_link_libraries(isac_backend PRIVATE ${ISAC_LLVM_TARGET_LIBS})
target_link_libraries(isac PRIVATE isac_backend)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(isac_backend PRIVATE -fno-rtti)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(isac_execution_contracts_test tests/execution_contracts.cpp)
  target_link_libraries(isac_execution_contracts_test PRIVATE isac_frontend)
  add_test(NAME execution_contract_invariants COMMAND isac_execution_contracts_test)
  add_test(
    NAME scoped_threads_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/scoped_threads_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_scoped_threads_runtime_test tests/scoped_threads_runtime.c)
  target_link_libraries(isac_scoped_threads_runtime_test PRIVATE isac_runtime)
  target_link_options(isac_scoped_threads_runtime_test PRIVATE -Wl,--wrap=pthread_create
                      -Wl,--wrap=pthread_join)
  add_test(
    NAME scoped_threads_runtime
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/scoped_threads_runtime.py
            $<TARGET_FILE:isac_scoped_threads_runtime_test>)
  add_executable(isac_deferred_task_test tests/deferred_task_test.cpp)
  target_link_libraries(isac_deferred_task_test PRIVATE isac_frontend isac_backend)
  add_test(NAME deferred_task_invariants COMMAND isac_deferred_task_test
                                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(
    NAME deferred_task_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/deferred_task_pipeline.py
            $<TARGET_FILE:isac_deferred_task_test> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_async_threads_test tests/async_threads_test.cpp)
  target_link_libraries(isac_async_threads_test PRIVATE isac_frontend)
  target_link_options(isac_async_threads_test PRIVATE
                      -Wl,--wrap=_ZN4isac9execution20allocatorCrossThreadEv)
  add_test(NAME async_threads_invariants COMMAND isac_async_threads_test)
  add_test(NAME public_async_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/async_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(
    NAME suspension_borrows_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/suspension_borrows_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_suspension_borrows_test tests/suspension_borrows_test.cpp)
  target_link_libraries(isac_suspension_borrows_test PRIVATE isac_frontend isac_backend)
  add_test(NAME suspension_borrow_invariants COMMAND isac_suspension_borrows_test)
  add_test(
    NAME suspension_cancel_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/suspension_cancel_pipeline.py
            $<TARGET_FILE:isac_suspension_borrows_test>)
  add_executable(isac_protocol_execution_test tests/protocol_execution_test.cpp)
  target_link_libraries(isac_protocol_execution_test PRIVATE isac_frontend)
  add_test(NAME protocol_execution_invariants COMMAND isac_protocol_execution_test)
  add_test(
    NAME protocol_execution_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_execution_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME typed_thread_calls_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/thread_calls_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(
    NAME scoped_carrier_threads_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/thread_carriers_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_thread_carriers_test tests/thread_carriers_test.cpp)
  target_link_libraries(isac_thread_carriers_test PRIVATE isac_frontend)
  target_link_options(isac_thread_carriers_test PRIVATE
                      -Wl,--wrap=_ZN4isac9execution20allocatorCrossThreadEv)
  add_test(NAME scoped_carrier_thread_invariants COMMAND isac_thread_carriers_test)
endif()
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME aggregate_syntax_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aggregate_syntax_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
endif()
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  file(GLOB STD_FOUNDATION_CASES CONFIGURE_DEPENDS tests/std_foundations/*.isa)

  list(FILTER STD_FOUNDATION_CASES EXCLUDE REGEX "/valid_random\\.isa$")
  foreach(CASE IN LISTS STD_FOUNDATION_CASES)
    get_filename_component(NAME ${CASE} NAME_WE)
    add_test(NAME std_foundation_${NAME}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macro_test.py
                     $<TARGET_FILE:isac> ${CASE})
  endforeach()
  add_test(
    NAME std_foundations_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/std_foundations_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
endif()
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  file(GLOB CASES CONFIGURE_DEPENDS tests/cases/*.isa)
  foreach(CASE IN LISTS CASES)
    get_filename_component(NAME ${CASE} NAME_WE)
    add_test(NAME ${NAME} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check.py
                                  $<TARGET_FILE:isac> ${CASE})
  endforeach()
  function(isac_native_test NAME SOURCE EXPECTED)
    add_test(NAME native_${NAME}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/native.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE} "${EXPECTED}")
  endfunction()
  isac_native_test(file examples/file.isa "7|8|")
  isac_native_test(array tests/cases/valid_array.isa "8|")
  isac_native_test(call tests/cases/valid_call.isa "42|")
  isac_native_test(branch tests/cases/valid_branch.isa "")
  isac_native_test(move tests/cases/valid_move.isa "4|")
  isac_native_test(borrow tests/cases/valid_unique_borrow.isa "20|")
  isac_native_test(early_return tests/cases/valid_early_return.isa "")
  isac_native_test(resource_arg tests/cases/valid_resource_argument.isa "0|")
  isac_native_test(borrow_protocol tests/cases/valid_borrow_protocol.isa "7|")
  isac_native_test(connection examples/connection.isa "11|22|24|")
  isac_native_test(logic tests/cases/valid_logic.isa "0|1|99|1|99|1|1|0|-2147483648|-2147483648|5|")
  isac_native_test(nested_branch tests/cases/valid_nested_branch.isa "15|25|")
  isac_native_test(return_else tests/cases/valid_return_else.isa "1|3|2|")
  add_test(NAME pipeline_boundaries
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME edge_cases
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/edge_cases.py
                   $<TARGET_FILE:isac>)
  add_executable(isac_core_test tests/core_boundary.cpp)
  target_link_libraries(isac_core_test PRIVATE isac_frontend)
  add_test(NAME core_trust_boundary COMMAND isac_core_test)
endif()

if(BUILD_TESTING)
  execute_process(
    COMMAND
      ${ISAC_CLANG} -fsanitize=address,undefined ${CMAKE_CURRENT_SOURCE_DIR}/tests/sanitizer_probe.c
      -o ${CMAKE_CURRENT_BINARY_DIR}/sanitizer-probe
    RESULT_VARIABLE ISAC_SANITIZER_STATUS
    OUTPUT_QUIET ERROR_QUIET)
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(NAME public_async_sanitized_pipeline
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/async_pipeline.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(public_async_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME suspension_borrows_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/suspension_borrows_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(suspension_borrows_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME suspension_cancel_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/suspension_cancel_pipeline.py
              $<TARGET_FILE:isac_suspension_borrows_test> sanitized)
    set_tests_properties(suspension_cancel_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME protocol_execution_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_execution_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(protocol_execution_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME typed_thread_calls_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/thread_calls_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(typed_thread_calls_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME scoped_carrier_threads_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/thread_carriers_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(scoped_carrier_threads_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME scoped_threads_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/scoped_threads_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(scoped_threads_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME deferred_task_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/deferred_task_pipeline.py
              $<TARGET_FILE:isac_deferred_task_test> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(deferred_task_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME aggregate_syntax_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aggregate_syntax_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(aggregate_syntax_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    foreach(CASE IN LISTS STD_FOUNDATION_CASES)
      get_filename_component(NAME ${CASE} NAME_WE)
      if(NAME MATCHES "^valid_")
        add_test(NAME std_foundation_sanitized_${NAME}
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macro_test.py
                         $<TARGET_FILE:isac> ${CASE} sanitized)
        set_tests_properties(std_foundation_sanitized_${NAME} PROPERTIES LABELS "sanitizers")
      endif()
    endforeach()
  endif()
  file(GLOB HEAP_CASES CONFIGURE_DEPENDS tests/heap/*.isa)
  foreach(CASE IN LISTS HEAP_CASES)
    get_filename_component(NAME ${CASE} NAME_WE)
    add_test(NAME heap_check_${NAME}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/heap_test.py
                     $<TARGET_FILE:isac> ${CASE} check)
    if(NAME MATCHES "^valid_")
      add_test(NAME heap_native_${NAME}
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/heap_test.py
                       $<TARGET_FILE:isac> ${CASE} native)
      if(ISAC_SANITIZER_STATUS EQUAL 0)
        add_test(NAME heap_sanitized_${NAME}
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/heap_test.py
                         $<TARGET_FILE:isac> ${CASE} sanitized)
        set_tests_properties(heap_sanitized_${NAME} PROPERTIES LABELS "sanitizers")
      endif()
    endif()
  endforeach()
endif()
if(BUILD_TESTING)
  add_executable(isac_runtime_test tests/runtime_test.c runtime/src/isac_runtime.c
                                   "${ISAC_ALLOCATOR_SOURCE}")
  target_include_directories(isac_runtime_test PRIVATE runtime/include)
  target_compile_definitions(isac_runtime_test PRIVATE ISAC_RT_TESTING)
  add_test(NAME runtime_abi COMMAND isac_runtime_test)
  add_executable(isac_library_test tests/library_contracts.cpp)
  target_link_libraries(isac_library_test PRIVATE isac_frontend)
  add_test(NAME library_contracts COMMAND isac_library_test)
  add_test(NAME runtime_panics
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_failures.py
                   $<TARGET_FILE:isac> $<TARGET_FILE:isac_runtime_test>)
endif()
if(BUILD_TESTING)
  add_test(NAME heap_pipeline_boundaries
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/heap_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(BUILD_TESTING)
  file(GLOB STAGE3_CASES CONFIGURE_DEPENDS tests/stage3/*.isa)
  foreach(CASE IN LISTS STAGE3_CASES)
    get_filename_component(NAME ${CASE} NAME_WE)
    add_test(NAME stage3_${NAME}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/stage3_test.py
                     $<TARGET_FILE:isac> ${CASE})
  endforeach()
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    foreach(NAME valid_loop_heap_cleanup valid_loop_return valid_match_payload valid_primitives)
      add_test(
        NAME stage3_sanitized_${NAME}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/stage3_test.py
                $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR}/tests/stage3/${NAME}.isa sanitized)
      set_tests_properties(stage3_sanitized_${NAME} PROPERTIES LABELS "sanitizers")
    endforeach()
  endif()
endif()
if(BUILD_TESTING)
  add_test(NAME stage3_targets
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/targets.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME stage3_optimization
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/optimization.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
endif()
if(BUILD_TESTING)
  file(GLOB STRING_CASES CONFIGURE_DEPENDS tests/strings/*.isa)
  foreach(CASE IN LISTS STRING_CASES)
    get_filename_component(NAME ${CASE} NAME_WE)
    add_test(NAME string_${NAME}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/string_test.py
                     $<TARGET_FILE:isac> ${CASE})
    if(NAME MATCHES "^valid_" AND ISAC_SANITIZER_STATUS EQUAL 0)
      add_test(NAME string_sanitized_${NAME}
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/string_test.py
                       $<TARGET_FILE:isac> ${CASE} sanitized)
      set_tests_properties(string_sanitized_${NAME} PROPERTIES LABELS "sanitizers")
    endif()
  endforeach()
endif()
if(BUILD_TESTING)
  add_test(NAME diagnostic_snapshots
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics_test.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME string_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/string_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
endif()
if(BUILD_TESTING)
  add_executable(isac_diagnostics_test tests/diagnostics_unit.cpp)
  target_link_libraries(isac_diagnostics_test PRIVATE isac_frontend)
  add_test(NAME diagnostic_model COMMAND isac_diagnostics_test)
  add_executable(isac_string_runtime_test tests/string_runtime.c runtime/src/isac_runtime.c
                                          "${ISAC_ALLOCATOR_SOURCE}")
  target_include_directories(isac_string_runtime_test PRIVATE runtime/include)
  target_compile_definitions(isac_string_runtime_test PRIVATE ISAC_RT_TESTING)
  add_test(NAME string_runtime_abi COMMAND isac_string_runtime_test)
endif()

if(BUILD_TESTING)

  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS tests/macros/expectations.json)
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/tests/macros/expectations.json MACRO_EXPECTATIONS)
  string(JSON MACRO_COUNT LENGTH "${MACRO_EXPECTATIONS}")
  math(EXPR MACRO_LAST "${MACRO_COUNT}-1")
  foreach(INDEX RANGE 0 ${MACRO_LAST})
    string(JSON NAME MEMBER "${MACRO_EXPECTATIONS}" ${INDEX})
    set(CASE ${CMAKE_CURRENT_SOURCE_DIR}/tests/macros/${NAME}.isa)
    add_test(NAME macro_${NAME}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macro_test.py
                     $<TARGET_FILE:isac> ${CASE})
    if(NAME MATCHES "^valid_" AND ISAC_SANITIZER_STATUS EQUAL 0)
      add_test(NAME macro_sanitized_${NAME}
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macro_test.py
                       $<TARGET_FILE:isac> ${CASE} sanitized)
      set_tests_properties(macro_sanitized_${NAME} PROPERTIES LABELS "sanitizers")
    endif()
  endforeach()
endif()
if(BUILD_TESTING)
  add_executable(isac_liveness_test tests/liveness_test.cpp)
  target_link_libraries(isac_liveness_test PRIVATE isac_frontend)
  add_test(NAME cfg_liveness COMMAND isac_liveness_test)
endif()
if(BUILD_TESTING)
  add_test(NAME macros_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macros_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
endif()
if(BUILD_TESTING)
  file(GLOB REFERENCE_CASES CONFIGURE_DEPENDS tests/references/*.isa)
  add_test(NAME env_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/env_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME env_sanitized_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/env_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
  set_tests_properties(env_sanitized_pipeline PROPERTIES LABELS "sanitizers")
  add_test(
    NAME std_refinement_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/std_refinement_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(
      NAME std_refinement_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/std_refinement_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(std_refinement_sanitized_pipeline PROPERTIES LABELS "sanitizers")
  endif()
  add_test(NAME semantic_regions_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/regions_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(NAME semantic_regions_sanitized_pipeline
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/regions_pipeline.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(semantic_regions_sanitized_pipeline PROPERTIES LABELS "sanitizers")
  endif()
  add_test(NAME generic_impl_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_impl_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(
      NAME generic_impl_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_impl_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(generic_impl_sanitized_pipeline PROPERTIES LABELS "sanitizers")
  endif()
  add_test(NAME containers_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/containers_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  file(GLOB CONTAINER_CASES CONFIGURE_DEPENDS tests/containers/*.isa)
  foreach(CASE ${CONTAINER_CASES})
    get_filename_component(NAME ${CASE} NAME_WE)
    add_test(NAME container_${NAME}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macro_test.py
                     $<TARGET_FILE:isac> ${CASE})
    if(NAME MATCHES "^valid_")
      add_test(NAME container_sanitized_${NAME}
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macro_test.py
                       $<TARGET_FILE:isac> ${CASE} sanitized)
      set_tests_properties(container_sanitized_${NAME} PROPERTIES LABELS "sanitizers")
    endif()
  endforeach()
  file(GLOB EXPRESSION_CASES CONFIGURE_DEPENDS tests/expressions/*.isa)
  add_test(NAME expressions_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/expressions_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  foreach(CASE IN LISTS EXPRESSION_CASES)
    get_filename_component(NAME ${CASE} NAME_WE)
    add_test(NAME expression_${NAME}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macro_test.py
                     $<TARGET_FILE:isac> ${CASE})
    if(NAME MATCHES "^valid_" AND ISAC_SANITIZER_STATUS EQUAL 0)
      add_test(NAME expression_sanitized_${NAME}
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macro_test.py
                       $<TARGET_FILE:isac> ${CASE} sanitized)
      set_tests_properties(expression_sanitized_${NAME} PROPERTIES LABELS "sanitizers")
    endif()
  endforeach()
  foreach(CASE IN LISTS REFERENCE_CASES)
    get_filename_component(NAME ${CASE} NAME_WE)
    add_test(NAME reference_${NAME}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macro_test.py
                     $<TARGET_FILE:isac> ${CASE})
    if(NAME MATCHES "^valid_" AND ISAC_SANITIZER_STATUS EQUAL 0)
      add_test(NAME reference_sanitized_${NAME}
               COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/macro_test.py
                       $<TARGET_FILE:isac> ${CASE} sanitized)
      set_tests_properties(reference_sanitized_${NAME} PROPERTIES LABELS "sanitizers")
    endif()
  endforeach()
endif()
if(BUILD_TESTING)
  add_test(NAME owned_fields_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/owned_fields_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME owned_fields_sanitized_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/owned_fields_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
  set_tests_properties(owned_fields_sanitized_pipeline PROPERTIES LABELS "sanitizers")
endif()
if(BUILD_TESTING)
  add_test(
    NAME generic_storage_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_storage_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(
      NAME generic_storage_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_storage_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(generic_storage_sanitized_pipeline PROPERTIES LABELS "sanitizers")
  endif()
endif()
if(BUILD_TESTING)
  add_test(NAME imports_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/imports_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(
    NAME container_access_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/container_access_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(
    NAME container_mutation_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/container_mutation_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME slice_access_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/slice_access_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(
    NAME exclusive_results_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/exclusive_results_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(NAME imports_sanitized_pipeline
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/imports_pipeline.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    add_test(
      NAME container_access_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/container_access_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    add_test(
      NAME container_mutation_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/container_mutation_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(container_mutation_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME slice_access_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/slice_access_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(slice_access_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME exclusive_results_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/exclusive_results_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(exclusive_results_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    set_tests_properties(imports_sanitized_pipeline container_access_sanitized_pipeline
                         PROPERTIES LABELS "sanitizers")
  endif()
endif()

if(BUILD_TESTING)
  file(GLOB FFI_CASES CONFIGURE_DEPENDS tests/ffi/*.isa)
  foreach(CASE IN LISTS FFI_CASES)
    get_filename_component(NAME ${CASE} NAME_WE)
    add_test(NAME ffi_${NAME}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_case.py
                     $<TARGET_FILE:isac> ${CASE})
  endforeach()
  add_executable(isac_ffi_boundary_test tests/ffi_boundary.cpp)
  target_link_libraries(isac_ffi_boundary_test PRIVATE isac_frontend)
  add_test(NAME ffi_core_boundary COMMAND isac_ffi_boundary_test)
  add_test(NAME ffi_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} ${ISAC_CLANG})
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(NAME ffi_pipeline_sanitized
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_pipeline.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} ${ISAC_CLANG} sanitized)
    set_tests_properties(ffi_pipeline_sanitized PROPERTIES LABELS "sanitizers")
  endif()
endif()

if(BUILD_TESTING)
  add_test(NAME ffi_sequences
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_sequences.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} ${ISAC_CLANG})
  add_test(NAME import_namespaces
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/import_namespaces.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(NAME ffi_sequences_sanitized
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_sequences.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} ${ISAC_CLANG} sanitized)
    add_test(NAME import_namespaces_sanitized
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/import_namespaces.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(ffi_sequences_sanitized import_namespaces_sanitized
                         PROPERTIES LABELS "sanitizers")
  endif()
endif()

if(BUILD_TESTING)
  add_test(
    NAME language_foundations
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/language_foundations/pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(
      NAME language_foundations_sanitized
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/language_foundations/pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(language_foundations_sanitized PROPERTIES LABELS "sanitizers")
  endif()
endif()

if(BUILD_TESTING)
  add_test(NAME compilation_performance
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/compilation_performance.py $<TARGET_FILE:isac>)
endif()

if(BUILD_TESTING)
  add_test(NAME reborrow_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/reborrow_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME allocator_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/allocator_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} ${ISAC_CLANG})
  add_executable(isac_semantic_regions_test tests/semantic_regions_test.cpp)
  target_link_libraries(isac_semantic_regions_test PRIVATE isac_frontend)
  add_test(NAME semantic_region_invariants COMMAND isac_semantic_regions_test)
  add_executable(isac_reference_schema_test tests/reference_schema_test.cpp)
  target_link_libraries(isac_reference_schema_test PRIVATE isac_frontend)
  add_test(NAME reference_schema_invariants COMMAND isac_reference_schema_test)
  add_executable(isac_semantic_carriers_test tests/semantic_carriers_test.cpp)
  target_link_libraries(isac_semantic_carriers_test PRIVATE isac_frontend)
  add_test(NAME semantic_carrier_invariants COMMAND isac_semantic_carriers_test)
  add_executable(isac_region_arguments_test tests/region_arguments_test.cpp)
  target_link_libraries(isac_region_arguments_test PRIVATE isac_frontend)
  add_test(NAME region_argument_invariants COMMAND isac_region_arguments_test)
  add_executable(isac_position_contracts_test tests/position_contracts_test.cpp)
  target_link_libraries(isac_position_contracts_test PRIVATE isac_frontend)
  add_test(NAME positional_reference_contract_invariants
           COMMAND isac_position_contracts_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/position_contracts/valid_positions.isa)
  add_test(
    NAME positional_reference_contract_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/position_contracts_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_carrier_instances_test tests/carrier_instances_test.cpp)
  target_link_libraries(isac_carrier_instances_test PRIVATE isac_frontend)
  add_test(NAME carrier_instance_invariants
           COMMAND isac_carrier_instances_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/carrier_instances/valid_lifecycle.isa)
  add_test(
    NAME carrier_instances_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/carrier_instances_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_local_carrier_fields_test tests/local_carrier_fields_test.cpp)
  target_link_libraries(isac_local_carrier_fields_test PRIVATE isac_frontend)
  add_test(NAME local_carrier_field_invariants
           COMMAND isac_local_carrier_fields_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_carrier_fields)
  add_test(
    NAME local_carrier_fields_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_carrier_fields_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_direct_reference_fields_test tests/direct_reference_fields_test.cpp)
  target_link_libraries(isac_direct_reference_fields_test PRIVATE isac_frontend)
  add_test(NAME direct_reference_field_invariants
           COMMAND isac_direct_reference_fields_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/direct_reference_fields)
  add_test(
    NAME direct_reference_fields_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/direct_reference_fields_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_struct_region_contracts_test tests/struct_region_contracts_test.cpp)
  target_link_libraries(isac_struct_region_contracts_test PRIVATE isac_frontend)
  add_test(NAME struct_region_contract_invariants
           COMMAND isac_struct_region_contracts_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/struct_region_contracts)
  add_test(
    NAME struct_region_contracts_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/struct_region_contracts_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(
    NAME mixed_region_contracts_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mixed_region_contracts_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_mixed_region_contracts_test tests/mixed_region_contracts_test.cpp)
  target_link_libraries(isac_mixed_region_contracts_test PRIVATE isac_frontend)
  add_test(NAME mixed_region_contract_invariants
           COMMAND isac_mixed_region_contracts_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/mixed_region_contracts)
  add_test(
    NAME unique_carrier_fields_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/unique_carrier_fields_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_unique_carrier_fields_test tests/unique_carrier_fields_test.cpp)
  target_link_libraries(isac_unique_carrier_fields_test PRIVATE isac_frontend)
  add_test(NAME unique_carrier_field_invariants
           COMMAND isac_unique_carrier_fields_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/unique_carrier_fields)
  add_test(
    NAME direct_unique_fields_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/direct_unique_fields_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_direct_unique_fields_test tests/direct_unique_fields_test.cpp)
  target_link_libraries(isac_direct_unique_fields_test PRIVATE isac_frontend)
  add_test(NAME direct_unique_field_invariants
           COMMAND isac_direct_unique_fields_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/direct_unique_fields)
  add_test(
    NAME stored_field_reborrow_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/stored_field_reborrow_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_stored_field_reborrow_test tests/stored_field_reborrow_test.cpp)
  target_link_libraries(isac_stored_field_reborrow_test PRIVATE isac_frontend)
  add_test(NAME stored_field_reborrow_invariants
           COMMAND isac_stored_field_reborrow_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/stored_field_reborrow)
  add_test(
    NAME unique_struct_inputs_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/unique_struct_inputs_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_unique_struct_inputs_test tests/unique_struct_inputs_test.cpp)
  target_link_libraries(isac_unique_struct_inputs_test PRIVATE isac_frontend)
  add_test(NAME unique_struct_input_invariants
           COMMAND isac_unique_struct_inputs_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/unique_struct_inputs)
  add_test(
    NAME unique_struct_contracts_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/unique_struct_contracts_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_unique_struct_contracts_test tests/unique_struct_contracts_test.cpp)
  target_link_libraries(isac_unique_struct_contracts_test PRIVATE isac_frontend)
  add_test(NAME unique_struct_contract_invariants
           COMMAND isac_unique_struct_contracts_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/unique_struct_contracts)
  add_test(
    NAME owned_unique_transfer_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/owned_unique_transfer_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_owned_unique_transfer_test tests/owned_unique_transfer_test.cpp)
  target_link_libraries(isac_owned_unique_transfer_test PRIVATE isac_frontend)
  add_test(NAME owned_unique_transfer_invariants
           COMMAND isac_owned_unique_transfer_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/owned_unique_transfer)
  add_test(
    NAME multi_unique_transfer_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/multi_unique_transfer_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_multi_unique_transfer_test tests/multi_unique_transfer_test.cpp)
  target_link_libraries(isac_multi_unique_transfer_test PRIVATE isac_frontend)
  add_test(NAME multi_unique_transfer_invariants
           COMMAND isac_multi_unique_transfer_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/multi_unique_transfer)
  add_test(
    NAME inferred_unique_routing_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/inferred_unique_routing_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_inferred_unique_routing_test tests/inferred_unique_routing_test.cpp)
  target_link_libraries(isac_inferred_unique_routing_test PRIVATE isac_frontend)
  add_test(NAME inferred_unique_routing_invariants
           COMMAND isac_inferred_unique_routing_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/inferred_unique_routing)
  add_test(
    NAME cfg_unique_routing_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cfg_unique_routing_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_cfg_unique_routing_test tests/cfg_unique_routing_test.cpp)
  target_link_libraries(isac_cfg_unique_routing_test PRIVATE isac_frontend)
  add_test(NAME cfg_unique_routing_invariants
           COMMAND isac_cfg_unique_routing_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/cfg_unique_routing)
  add_test(
    NAME mixed_result_contracts_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mixed_result_contracts_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_mixed_result_contracts_test tests/mixed_result_contracts_test.cpp)
  target_link_libraries(isac_mixed_result_contracts_test PRIVATE isac_frontend)
  add_test(NAME mixed_result_contract_invariants
           COMMAND isac_mixed_result_contracts_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/mixed_result_contracts)
  add_test(
    NAME concurrency_foundations_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency_foundations_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_concurrency_foundations_test tests/concurrency_foundations_test.cpp)
  target_link_libraries(isac_concurrency_foundations_test PRIVATE isac_frontend)
  add_test(NAME concurrency_foundations_invariants
           COMMAND isac_concurrency_foundations_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency_foundations)
  add_executable(isac_capture_region_dependencies_test tests/capture_region_dependencies_test.cpp)
  target_link_libraries(isac_capture_region_dependencies_test PRIVATE isac_frontend)
  add_test(NAME capture_region_dependency_invariants
           COMMAND isac_capture_region_dependencies_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/capture_region_dependencies)
  add_executable(isac_capture_states_test tests/capture_states_test.cpp)
  target_link_libraries(isac_capture_states_test PRIVATE isac_frontend)
  add_test(NAME capture_states_invariants COMMAND isac_capture_states_test
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/capture_states)
  add_test(
    NAME capture_states_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/capture_states_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_frame_construction_test tests/frame_construction_test.cpp)
  target_link_libraries(isac_frame_construction_test PRIVATE isac_frontend)
  add_test(NAME frame_construction_invariants COMMAND isac_frame_construction_test
                                                      ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(
    NAME frame_construction_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_construction_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_frame_storage_test tests/frame_storage_test.cpp)
  target_link_libraries(isac_frame_storage_test PRIVATE isac_frontend)
  add_test(NAME frame_storage_invariants COMMAND isac_frame_storage_test
                                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_storage)
  add_test(
    NAME frame_storage_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_storage_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_frame_heap_test tests/frame_heap_test.cpp)
  target_link_libraries(isac_frame_heap_test PRIVATE isac_frontend)
  add_test(NAME frame_heap_invariants COMMAND isac_frame_heap_test
                                              ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME frame_heap_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_heap_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_frame_dispatch_test tests/frame_dispatch_test.cpp)
  target_link_libraries(isac_frame_dispatch_test PRIVATE isac_frontend)
  add_test(NAME frame_dispatch_invariants COMMAND isac_frame_dispatch_test
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_dispatch)
  add_test(
    NAME frame_dispatch_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_dispatch_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_frame_outline_test tests/frame_outline_test.cpp)
  target_link_libraries(isac_frame_outline_test PRIVATE isac_frontend isac_backend)
  add_test(NAME frame_outline_invariants COMMAND isac_frame_outline_test
                                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(
    NAME frame_outline_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_outline_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_outline_test>)
  add_executable(isac_frame_state_test tests/frame_state_test.cpp)
  target_link_libraries(isac_frame_state_test PRIVATE isac_frontend isac_backend)
  add_test(NAME frame_state_invariants COMMAND isac_frame_state_test
                                               ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(
    NAME frame_state_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_state_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_state_test>)
  add_executable(isac_frame_step_test tests/frame_step_test.cpp)
  target_link_libraries(isac_frame_step_test PRIVATE isac_frontend isac_backend)
  add_test(NAME frame_step_invariants COMMAND isac_frame_step_test
                                              ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(
    NAME frame_step_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_step_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_step_test>)
  add_test(
    NAME frame_completion_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_completion_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_step_test>)
  add_executable(isac_frame_multiple_test tests/frame_multiple_test.cpp)
  target_link_libraries(isac_frame_multiple_test PRIVATE isac_frontend isac_backend)
  add_test(NAME frame_multiple_invariants COMMAND isac_frame_multiple_test
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(
    NAME frame_multiple_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_multiple_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_multiple_test>)
  add_executable(isac_frame_projection_test tests/frame_projection_test.cpp)
  target_link_libraries(isac_frame_projection_test PRIVATE isac_frontend isac_backend)
  add_test(NAME frame_projection_invariants COMMAND isac_frame_projection_test
                                                    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(
    NAME frame_projection_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_projection_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_projection_test>)
  add_executable(isac_frame_deep_test tests/frame_deep_test.cpp)
  target_link_libraries(isac_frame_deep_test PRIVATE isac_frontend)
  add_test(NAME frame_deep_invariants COMMAND isac_frame_deep_test
                                              ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME frame_deep_pipeline
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_deep_pipeline.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_frame_branch_test tests/frame_branch_test.cpp)
  target_link_libraries(isac_frame_branch_test PRIVATE isac_frontend isac_backend)
  add_test(NAME frame_branch_invariants COMMAND isac_frame_branch_test
                                                ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(
    NAME frame_branch_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_branch_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_branch_test>)
  if(UNIX)
    find_package(Threads QUIET)
    if(Threads_FOUND AND CMAKE_USE_PTHREADS_INIT)
      add_test(
        NAME runtime_concurrency_instrumentation
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_concurrency.py
                ${ISAC_CLANG} ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
  endif()
  add_executable(isac_projection_bounds_test tests/projection_bounds_test.cpp)
  target_link_libraries(isac_projection_bounds_test PRIVATE isac_frontend)
  add_test(NAME projection_storage_invariants
           COMMAND isac_projection_bounds_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/projection_storage/valid_nested.isa)
  add_executable(isac_storage_epochs_test tests/storage_epochs_test.cpp)
  target_link_libraries(isac_storage_epochs_test PRIVATE isac_frontend)
  add_test(NAME storage_epoch_invariants
           COMMAND isac_storage_epochs_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_epochs/valid_transfers.isa)
  add_executable(isac_storage_paths_test tests/storage_paths_test.cpp)
  target_link_libraries(isac_storage_paths_test PRIVATE isac_frontend)
  add_test(NAME storage_path_invariants
           COMMAND isac_storage_paths_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_paths/valid_fields.isa)
  add_executable(isac_element_bounds_test tests/element_bounds_test.cpp)
  target_link_libraries(isac_element_bounds_test PRIVATE isac_frontend)
  add_test(NAME element_bound_invariants
           COMMAND isac_element_bounds_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_paths/valid_elements.isa)
  add_executable(isac_storage_slots_test tests/storage_slots_test.cpp)
  target_link_libraries(isac_storage_slots_test PRIVATE isac_frontend)
  add_test(NAME storage_slot_invariants
           COMMAND isac_storage_slots_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_slots/valid_slots.isa)
  add_test(
    NAME storage_slots_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_slots_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME storage_slot_scaling
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_slot_scaling.py
                   $<TARGET_FILE:isac>)
  add_test(
    NAME recursive_storage_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_paths_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(
    NAME storage_epochs_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_epochs_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(
    NAME projection_storage_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/projection_storage_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(
    NAME stack_lowering_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/stack_lowering_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(isac_backend_stack_test tests/backend_stack_boundary.cpp)
  target_link_libraries(isac_backend_stack_test PRIVATE isac_backend isac_frontend)
  add_test(NAME backend_stack_boundary COMMAND isac_backend_stack_test)
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(
      NAME positional_reference_contract_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/position_contracts_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(positional_reference_contract_sanitized_pipeline PROPERTIES LABELS
                                                                                     "sanitizers")
    add_test(
      NAME carrier_instances_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/carrier_instances_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(carrier_instances_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME local_carrier_fields_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_carrier_fields_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(local_carrier_fields_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME direct_reference_fields_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/direct_reference_fields_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(direct_reference_fields_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME struct_region_contracts_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/struct_region_contracts_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(struct_region_contracts_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME mixed_region_contracts_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mixed_region_contracts_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(mixed_region_contracts_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME unique_carrier_fields_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/unique_carrier_fields_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(unique_carrier_fields_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME direct_unique_fields_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/direct_unique_fields_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(direct_unique_fields_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME stored_field_reborrow_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/stored_field_reborrow_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(stored_field_reborrow_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME unique_struct_inputs_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/unique_struct_inputs_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(unique_struct_inputs_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME unique_struct_contracts_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/unique_struct_contracts_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(unique_struct_contracts_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME owned_unique_transfer_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/owned_unique_transfer_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(owned_unique_transfer_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME multi_unique_transfer_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/multi_unique_transfer_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(multi_unique_transfer_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME inferred_unique_routing_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/inferred_unique_routing_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(inferred_unique_routing_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME cfg_unique_routing_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cfg_unique_routing_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(cfg_unique_routing_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME mixed_result_contracts_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mixed_result_contracts_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(mixed_result_contracts_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME concurrency_foundations_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency_foundations_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(concurrency_foundations_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME capture_states_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/capture_states_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(capture_states_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME frame_construction_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_construction_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(frame_construction_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME frame_storage_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_storage_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(frame_storage_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(NAME frame_heap_sanitized_pipeline
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_heap_pipeline.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(frame_heap_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME frame_dispatch_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_dispatch_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(frame_dispatch_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME frame_outline_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_outline_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_outline_test>
        sanitized)
    set_tests_properties(frame_outline_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME frame_state_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_state_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_state_test>
        sanitized)
    set_tests_properties(frame_state_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME frame_step_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_step_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_step_test>
        sanitized)
    set_tests_properties(frame_step_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME frame_completion_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_completion_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_step_test>
        sanitized)
    set_tests_properties(frame_completion_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME frame_multiple_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_multiple_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_multiple_test>
        sanitized)
    set_tests_properties(frame_multiple_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME frame_projection_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_projection_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_projection_test>
        sanitized)
    set_tests_properties(frame_projection_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(NAME frame_deep_sanitized_pipeline
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_deep_pipeline.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(frame_deep_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME frame_branch_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_branch_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:isac_frame_branch_test>
        sanitized)
    set_tests_properties(frame_branch_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    if(UNIX
       AND Threads_FOUND
       AND CMAKE_USE_PTHREADS_INIT)
      add_test(
        NAME runtime_concurrency_instrumentation_sanitized
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_concurrency.py
                ${ISAC_CLANG} ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
      set_tests_properties(runtime_concurrency_instrumentation_sanitized PROPERTIES LABELS
                                                                                    "sanitizers")
    endif()
    add_test(
      NAME storage_slots_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_slots_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(storage_slots_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME recursive_storage_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_paths_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(recursive_storage_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME storage_epochs_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage_epochs_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(storage_epochs_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME projection_storage_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/projection_storage_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(projection_storage_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME stack_lowering_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/stack_lowering_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(stack_lowering_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(NAME reborrow_sanitized_pipeline
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/reborrow_pipeline.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    add_test(NAME allocator_sanitized_pipeline
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/allocator_pipeline.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} ${ISAC_CLANG} sanitized)
    set_tests_properties(reborrow_sanitized_pipeline allocator_sanitized_pipeline
                         PROPERTIES LABELS "sanitizers")
  endif()
endif()

if(BUILD_TESTING)
  add_test(NAME ffi_advanced
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_advanced.py
                   $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} ${ISAC_CLANG})
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(NAME ffi_advanced_sanitized
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_advanced.py
                     $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} ${ISAC_CLANG} sanitized)
    set_tests_properties(ffi_advanced_sanitized PROPERTIES LABELS "sanitizers")
  endif()
endif()

if(BUILD_TESTING)
  add_test(
    NAME std_expansion_pipeline
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/std_expansion_pipeline.py
            $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(
    NAME collections_refinement_pipeline
    COMMAND
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/collections_refinement_pipeline.py
      $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR})
  if(ISAC_SANITIZER_STATUS EQUAL 0)
    add_test(
      NAME std_expansion_sanitized_pipeline
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/std_expansion_pipeline.py
              $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(std_expansion_sanitized_pipeline PROPERTIES LABELS "sanitizers")
    add_test(
      NAME collections_refinement_sanitized_pipeline
      COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/collections_refinement_pipeline.py
        $<TARGET_FILE:isac> ${CMAKE_CURRENT_SOURCE_DIR} sanitized)
    set_tests_properties(collections_refinement_sanitized_pipeline PROPERTIES LABELS "sanitizers")
  endif()
endif()
