cmake_minimum_required(VERSION 3.20)

project(cec-control-tests LANGUAGES C)

enable_testing()

add_executable(cec-control-tests
  ../../src/cec-control.c
  test_cec_control.c)

target_include_directories(cec-control-tests PRIVATE ../../include)
target_compile_features(cec-control-tests PRIVATE c_std_11)
target_compile_options(cec-control-tests PRIVATE -Wall -Wextra -Werror)

add_test(NAME cec-control-tests COMMAND cec-control-tests)
