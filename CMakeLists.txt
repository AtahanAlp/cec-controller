cmake_minimum_required(VERSION 3.13)

set(PROJECT pico-cec)
set(BOARD "pico_sdk") # Configure TinyUSB use pico-sdk board configuration
set(PICO_BOARD "pico" CACHE STRING "Raspberry Pi Pico board type.")

set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include/boards)

# Include pico-sdk and tinyusb (must be before project)
include(pico-sdk/pico_sdk_init.cmake)
include(pico-sdk/lib/tinyusb/hw/bsp/family_support.cmake)
family_get_project_name(PROJECT ${PROJECT})

project(${PROJECT}
  DESCRIPTION "HDMI CEC to USB HID keyboard adapter."
  LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Include FreeRTOS kernel
if(${PICO_PLATFORM} STREQUAL "rp2040")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS-Kernel/portable/ThirdParty/GCC/RP2040)
elseif(${PICO_PLATFORM} STREQUAL "rp2350-arm-s")
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS-Kernel/portable/ThirdParty/GCC/RP2350_ARM_NTZ)
else()
message(FATAL_ERROR "Unsupport Pico platform: ${PICO_PLATFORM}")
endif()

# Include CRC library
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/crc)

# Initialize the SDK
pico_sdk_init()

add_compile_options(-Wall -Werror)

set(TCLI_SOURCE_DIR ${PROJECT_SOURCE_DIR}/tcli)

set(CEC_PIN "11" CACHE STRING "GPIO pin for HDMI CEC.")
set(PICO_CEC_VERSION "unknown" CACHE STRING "Pico-CEC version string.")
set(KEYMAP_DEFAULT "KODI" CACHE STRING "Default keymap, specify KODI or MISTER.")
set(CEC_OSD_NAME "Couch PC" CACHE STRING "OSD name for the CEC device (max 14 characters).")
option(CEC_ENABLE_DDC "Enable 5 V HDMI DDC/EDID access through level shifting." OFF)
option(CEC_ENABLE_RGB_LED "Enable a WS2812 status LED." OFF)

add_library(tcli STATIC
  ${TCLI_SOURCE_DIR}/source/tcli.c
  ${TCLI_SOURCE_DIR}/source/tclie.c)

target_include_directories(tcli PUBLIC
  ${TCLI_SOURCE_DIR}/include)

target_compile_definitions(tcli PUBLIC
  TCLI_COMPLETE=1
  TCLIE_ENABLE_USERS=0
  TCLIE_PATTERN_MATCH=1)

target_compile_options(tcli PRIVATE
  -Wno-stringop-truncation)

set(PICO_CEC_SOURCES
  src/blink.c
  src/cec-config.c
  src/cec-frame.c
  src/cec-log.c
  src/cec-task.c
  src/cec-user.c
  src/freertos_hook.c
  src/main.c
  src/nvs.c
  src/usb-cdc.c
  src/usb_descriptors.c
  src/usb_hid.c
  src/util.c)

set(PICO_CEC_LIBRARIES)

if(CEC_ENABLE_DDC)
  list(APPEND PICO_CEC_SOURCES src/ddc.c)
  list(APPEND PICO_CEC_LIBRARIES hardware_i2c)
else()
  list(APPEND PICO_CEC_SOURCES src/ddc-disabled.c)
endif()

if(CEC_ENABLE_RGB_LED)
  list(APPEND PICO_CEC_SOURCES src/ws2812.c src/ws2812.pio)
  list(APPEND PICO_CEC_LIBRARIES hardware_pio)
else()
  list(APPEND PICO_CEC_SOURCES src/ws2812-disabled.c)
endif()

add_executable(${PROJECT} ${PICO_CEC_SOURCES})

target_compile_definitions(${PROJECT} PRIVATE
  "CEC_ENABLE_DDC=$<BOOL:${CEC_ENABLE_DDC}>")

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)

if(CEC_ENABLE_RGB_LED)
  pico_generate_pio_header(${PROJECT} ${PROJECT_SOURCE_DIR}/src/ws2812.pio OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
endif()

target_include_directories(${PROJECT} PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/include/tusb
  ${CMAKE_CURRENT_BINARY_DIR}/generated)

set_source_files_properties(src/cec-frame.c PROPERTIES COMPILE_DEFINITIONS
  "CEC_PIN=${CEC_PIN}")

set_source_files_properties(src/usb-cdc.c PROPERTIES COMPILE_DEFINITIONS
  "PICO_CEC_VERSION=\"${PICO_CEC_VERSION}\"")

set_source_files_properties(src/nvs.c PROPERTIES COMPILE_DEFINITIONS
  "CEC_OSD_NAME=\"${CEC_OSD_NAME}\"")

# Undefine TinyUSB built-in OS, redefined in our tusb_config.h
target_compile_options(${PROJECT} PRIVATE
  -UCFG_TUSB_OS
  -DKEYMAP_DEFAULT_${KEYMAP_DEFAULT}=1)

target_link_libraries(${PROJECT}
  crc
  pico_stdlib
  pico_unique_id
  tinyusb_device
  tinyusb_board
  FreeRTOS-Kernel
  tcli
  ${PICO_CEC_LIBRARIES})

pico_add_extra_outputs(${PROJECT})
pico_set_binary_type(${PROJECT} copy_to_ram)
pico_set_linker_script(${PROJECT} ${PROJECT_SOURCE_DIR}/src/memmap_ram_nvs_${PICO_PLATFORM}.ld)

# disable stdio and uart
pico_enable_stdio_usb(${PROJECT} 0)
pico_enable_stdio_uart(${PROJECT} 0)
