cmake_minimum_required(VERSION 3.20)
project(
  Isac
  VERSION 0.1
  LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(
  isac_frontend
  src/frontend.cpp
  src/modules.cpp
  src/hir.cpp
  src/core.cpp
  src/core_types.cpp
  src/erasure.cpp
  src/runtime.cpp)
target_include_directories(isac_frontend PUBLIC include)
target_compile_definitions(isac_frontend
                           PRIVATE ISAC_LIBRARY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/lib")
target_compile_options(isac_frontend PRIVATE -Wall -Wextra -Wpedantic)

add_executable(isac src/main.cpp)
target_link_libraries(isac PRIVATE isac_frontend)

find_package(MLIR REQUIRED CONFIG HINTS /usr/local/lib/cmake/mlir)
find_program(ISAC_CLANG NAMES clang REQUIRED)
add_library(isac_runtime STATIC runtime/src/isac_runtime.c)
target_include_directories(isac_runtime PUBLIC runtime/include)
target_compile_options(isac_runtime PRIVATE -Wall -Wextra -Wpedantic)

add_library(isac_backend src/backend.cpp src/native.cpp)
target_include_directories(isac_backend PUBLIC include)
target_include_directories(isac_backend SYSTEM PRIVATE ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_definitions(
  isac_backend
  PRIVATE ${LLVM_DEFINITIONS} ISAC_CLANG="${ISAC_CLANG}"
          ISAC_RUNTIME_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/isac_runtime.c"
          ISAC_RUNTIME_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")
target_link_libraries(
  isac_backend
  PRIVATE MLIRIR
          MLIRPass
          MLIRArithDialect
          MLIRControlFlowDialect
          MLIRFuncDialect
          MLIRMemRefDialect
          MLIRLLVMDialect
          MLIRArithToLLVM
          MLIRControlFlowToLLVM
          MLIRFuncToLLVM
          MLIRMemRefToLLVM
          MLIRReconcileUnrealizedCasts
          MLIRBuiltinToLLVMIRTranslation
          MLIRLLVMToLLVMIRTranslation
          MLIRTargetLLVMIRExport)
target_link_libraries(isac PRIVATE isac_backend)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(isac_backend PRIVATE -fno-rtti)
endif()

include(CTest)
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
