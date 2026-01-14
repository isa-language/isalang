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
  src/erasure.cpp
  src/runtime.cpp
  src/liveness.cpp
  src/regions.cpp
  src/references.cpp
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
add_library(isac_runtime STATIC runtime/src/isac_runtime.c "${ISAC_ALLOCATOR_SOURCE}")
target_include_directories(isac_runtime PUBLIC runtime/include)
target_compile_options(isac_runtime PRIVATE -Wall -Wextra -Wpedantic)

add_library(isac_backend src/backend.cpp src/native.cpp src/target.cpp)
target_include_directories(isac_backend PUBLIC include)
target_include_directories(isac_backend SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_definitions(
  isac_backend
  PRIVATE ${LLVM_DEFINITIONS}
          ISAC_CLANG="${ISAC_CLANG}"
          ISAC_RUNTIME_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/isac_runtime.c"
          ISAC_ALLOCATOR_SOURCE="${ISAC_ALLOCATOR_SOURCE}"
          ISAC_RUNTIME_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}/runtime/include"
          ISAC_RUNTIME_LIBRARY="$<TARGET_FILE:isac_runtime>")
add_dependencies(isac_backend isac_runtime)
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
  if(ISAC_SANITIZER_STATUS EQUAL 0)
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
