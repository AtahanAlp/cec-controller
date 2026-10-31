cmake_minimum_required(VERSION 3.20)

project(cec-control-tests LANGUAGES C)

enable_testing()

add_executable(cec-control-tests
  ../../src/cec-control.c
  ../../src/cec-validate.c
  ../../src/cec-config.c
  test_cec_control.c)

target_include_directories(cec-control-tests PRIVATE ../../include)
target_compile_features(cec-control-tests PRIVATE c_std_11)
target_compile_options(cec-control-tests PRIVATE -Wall -Wextra -Werror)

add_test(NAME cec-control-tests COMMAND cec-control-tests)

add_executable(tv-cli-tests
  ../../tcli/source/tcli.c
  ../../tcli/source/tclie.c
  test_tv_cli.c)

target_include_directories(tv-cli-tests PRIVATE
  ../../include
  ../../tcli/include)
target_compile_definitions(tv-cli-tests PRIVATE
  TCLI_COMPLETE=1
  TCLIE_ENABLE_USERS=0
  TCLIE_PATTERN_MATCH=1)
target_compile_features(tv-cli-tests PRIVATE c_std_11)
target_compile_options(tv-cli-tests PRIVATE -Wall -Wextra -Werror)

add_test(NAME tv-cli-tests COMMAND tv-cli-tests)

foreach(name IN ITEMS cec-frame cec-log cec-task)
  string(REPLACE "-" "_" source_name "${name}")
  add_executable(${name}-tests test_${source_name}.c ../../src/cec-validate.c ../../src/cec-config.c)
  target_include_directories(${name}-tests PRIVATE stubs ../../include)
  target_compile_definitions(${name}-tests PRIVATE CEC_PIN=1)
  target_compile_features(${name}-tests PRIVATE c_std_11)
  target_compile_options(${name}-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME ${name}-tests COMMAND ${name}-tests)
endforeach()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME pico-flash-guards COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_pico_maintenance.py)
endif()
