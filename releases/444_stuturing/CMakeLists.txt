cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(pico_sdk_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")
  message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.4.0 (or later) required.")
endif()

project(stuturing C CXX ASM)
pico_sdk_init()

add_executable(stuturing main.cpp)

pico_set_program_name(stuturing "Stuturing")
pico_set_program_version(stuturing "1.0.0")

target_compile_options(stuturing PRIVATE -Wdouble-promotion -Wall -Wextra -O3)
target_link_options(stuturing PRIVATE -Wl,--print-memory-usage)

target_compile_definitions(stuturing PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

target_include_directories(stuturing PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../../Demonstrations+HelloWorlds/PicoSDK/ComputerCard
)

target_link_libraries(stuturing
    pico_unique_id
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_i2c
    hardware_pwm
    hardware_adc
    hardware_spi
)

pico_enable_stdio_uart(stuturing 0)
pico_enable_stdio_usb(stuturing 0)
pico_add_extra_outputs(stuturing)
