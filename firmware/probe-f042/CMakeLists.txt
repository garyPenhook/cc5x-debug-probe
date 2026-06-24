cmake_minimum_required(VERSION 3.20)

# Cross build (firmware):
#   cmake --preset firmware && cmake --build --preset firmware
# Host build (unit tests):
#   cmake --preset tests && cmake --build --preset tests && ctest --preset tests
project(probe_f042 C ASM)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Host build (no ARM toolchain) -> portable unit tests only.
# ---------------------------------------------------------------------------
if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(test_relay tests/test_relay.c src/relay.c)
    target_include_directories(test_relay PRIVATE src)
    target_compile_options(test_relay PRIVATE -Wall -Wextra)
    add_test(NAME relay COMMAND test_relay)

    # USB-CDC descriptors are pure data (no CMSIS) -> host-testable like relay.
    add_executable(test_usb_desc tests/test_usb_desc.c src/usb_desc.c)
    target_include_directories(test_usb_desc PRIVATE src)
    target_compile_options(test_usb_desc PRIVATE -Wall -Wextra)
    add_test(NAME usb_desc COMMAND test_usb_desc)

    # USB control-request resolver is pure logic (no CMSIS) -> host-testable.
    add_executable(test_usb_ctrl tests/test_usb_ctrl.c src/usb_ctrl.c src/usb_desc.c)
    target_include_directories(test_usb_ctrl PRIVATE src)
    target_compile_options(test_usb_ctrl PRIVATE -Wall -Wextra)
    add_test(NAME usb_ctrl COMMAND test_usb_ctrl)
    return()
endif()

# ---------------------------------------------------------------------------
# Cross build (firmware image).
# ---------------------------------------------------------------------------
# Vendored CMSIS/device support (see vendor/README.md); NOT checked in and not
# auto-fetchable (st.com blocks bots).
set(VENDOR_DIR ${CMAKE_SOURCE_DIR}/vendor)
set(LINKER_SCRIPT ${VENDOR_DIR}/STM32F042K6Tx_FLASH.ld)
set(STARTUP_SRC   ${VENDOR_DIR}/startup_stm32f042x6.s)
set(SYSTEM_SRC    ${VENDOR_DIR}/system_stm32f0xx.c)

set(APP_SRCS src/main.c src/relay.c src/board_f042.c
             src/usb_cdc.c src/usb_desc.c src/usb_ctrl.c)

# PC-link transport: VCP (P5a, ST-LINK Virtual COM over USART2, default) or
# USB_CDC (P5b, native USB on PA11/PA12). Maps to the PROBE_PC_TRANSPORT_* enum
# in probe_pins.h.
set(PROBE_PC_TRANSPORT "VCP" CACHE STRING "PC-link transport: VCP or USB_CDC")
set_property(CACHE PROBE_PC_TRANSPORT PROPERTY STRINGS VCP USB_CDC)
# STRINGS is only an editor hint — enforce the value so a typo can't fall through
# to PROBE_PC_TRANSPORT_<typo> (which the preprocessor would treat as 0 = VCP).
if(NOT PROBE_PC_TRANSPORT STREQUAL "VCP" AND NOT PROBE_PC_TRANSPORT STREQUAL "USB_CDC")
    message(FATAL_ERROR "PROBE_PC_TRANSPORT must be VCP or USB_CDC (got '${PROBE_PC_TRANSPORT}')")
endif()

if(EXISTS ${STARTUP_SRC} AND EXISTS ${LINKER_SCRIPT})
    add_executable(probe_f042.elf ${APP_SRCS} ${STARTUP_SRC} ${SYSTEM_SRC})
    target_include_directories(probe_f042.elf PRIVATE
        src
        ${VENDOR_DIR}/CMSIS/Device/ST/STM32F0xx/Include
        ${VENDOR_DIR}/CMSIS/Include)
    target_compile_definitions(probe_f042.elf PRIVATE STM32F042x6
        PROBE_PC_TRANSPORT=PROBE_PC_TRANSPORT_${PROBE_PC_TRANSPORT})
    target_link_options(probe_f042.elf PRIVATE -T${LINKER_SCRIPT} -Wl,-Map=probe_f042.map)
    add_custom_command(TARGET probe_f042.elf POST_BUILD
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:probe_f042.elf>
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:probe_f042.elf> probe_f042.bin)
    # board_f042.c peripheral init is implemented against RM0091 (page-cited),
    # compile-smoked against a mock CMSIS, but NOT yet hardware-validated.
    # Review ES0243 errata and bench-test before relying on the image.
else()
    # Fatal, not a warning: a "successful" configure with no firmware target would
    # let CI/automation report green without ever building the image. The host-only
    # path is the separate 'tests' preset (which returns above, never reaching here).
    message(FATAL_ERROR
        "Vendor files missing in ${VENDOR_DIR}/ (need ${STARTUP_SRC} and "
        "${LINKER_SCRIPT}) — see vendor/README.md. The cross target cannot be "
        "built. For the host-testable relay core use: cmake --preset tests.")
endif()
