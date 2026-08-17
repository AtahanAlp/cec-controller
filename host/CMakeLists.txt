cmake_minimum_required(VERSION 3.20)
project(cecctl VERSION 0.1.0 LANGUAGES C)

include(CTest)

add_library(cecctl-core STATIC
  src/edid.c
  src/protocol.c
)
target_include_directories(cecctl-core PUBLIC include)
target_compile_features(cecctl-core PUBLIC c_std_11)
target_compile_options(cecctl-core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(cecctl src/main.c)
target_link_libraries(cecctl PRIVATE cecctl-core)
target_compile_definitions(cecctl PRIVATE _DEFAULT_SOURCE)
target_compile_options(cecctl PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  add_executable(cecctl-tests tests/test_cecctl.c)
  target_link_libraries(cecctl-tests PRIVATE cecctl-core)
  target_compile_options(cecctl-tests PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  add_test(NAME cecctl-tests COMMAND cecctl-tests)
  add_executable(cecctl-serial-test tests/test_serial.c)
  target_compile_options(cecctl-serial-test PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  add_test(NAME cecctl-serial-test COMMAND cecctl-serial-test $<TARGET_FILE:cecctl>)
  add_executable(cecctl-discovery-test tests/test_discovery.c)
  target_compile_options(cecctl-discovery-test PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  add_test(NAME cecctl-discovery-test COMMAND cecctl-discovery-test $<TARGET_FILE:cecctl>)
  add_test(NAME cecctl-manual-address
    COMMAND cecctl --config /nonexistent --physical-address 2.1.0.0 detect)
  set_tests_properties(cecctl-manual-address PROPERTIES
    PASS_REGULAR_EXPRESSION "physical_address=2.1.0.0 source=config")
endif()

install(TARGETS cecctl RUNTIME DESTINATION bin)
