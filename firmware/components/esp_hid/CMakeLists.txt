# SPDX-FileCopyrightText: 2026 EasyInput contributors
#
# SPDX-License-Identifier: Apache-2.0

# Keep this override intentionally thin: the NimBLE HID-device backend and the
# single HID-service source needed to preserve the FEATURE writer connection
# are patched locally. Every other source and all upstream public/private
# headers come from the exact ESP-IDF components pinned below.
idf_component_get_property(upstream_esp_hid_dir esp_hid COMPONENT_OVERRIDEN_DIR)
if(NOT upstream_esp_hid_dir)
    message(FATAL_ERROR
        "EasyInput esp_hid safety adapter must override an ESP-IDF esp_hid component")
endif()

idf_build_get_property(idf_ver IDF_VER)
if(NOT idf_ver STREQUAL "v5.5.5")
    message(FATAL_ERROR
        "EasyInput esp_hid safety adapter is pinned to ESP-IDF v5.5.5 "
        "(got '${idf_ver}'). Re-audit the adapter before changing ESP-IDF.")
endif()

set(expected_upstream_nimble_hidd_sha256
    "b117a0236c2086d07b74fb6ca356be62d5e94335b020abc18f53a2ec9dec7428")
set(upstream_nimble_hidd "${upstream_esp_hid_dir}/src/nimble_hidd.c")
if(NOT EXISTS "${upstream_nimble_hidd}")
    message(FATAL_ERROR
        "ESP-IDF v5.5.5 esp_hid backend not found at '${upstream_nimble_hidd}'")
endif()
file(SHA256 "${upstream_nimble_hidd}" upstream_nimble_hidd_sha256)
if(NOT upstream_nimble_hidd_sha256 STREQUAL expected_upstream_nimble_hidd_sha256)
    message(FATAL_ERROR
        "ESP-IDF esp_hid/nimble_hidd.c does not match the audited v5.5.5 source. "
        "Expected ${expected_upstream_nimble_hidd_sha256}, "
        "got ${upstream_nimble_hidd_sha256}.")
endif()

# ESP-IDF v5.5.5's HID service discards ble_svc_hid_access()'s writer
# conn_handle before invoking the report callback. EasyInput needs that exact
# endpoint identity for chunked FEATURE reports, so replace only this pinned
# source with the audited local companion while retaining the rest of bt.
idf_component_get_property(upstream_bt_dir bt COMPONENT_DIR)
set(upstream_ble_svc_hid
    "${upstream_bt_dir}/host/nimble/nimble/nimble/host/services/hid/src/ble_svc_hid.c")
set(expected_upstream_ble_svc_hid_sha256
    "b9c92dce9a0a678ca96c36b2d078456459a6cb66438b5ea47ac99d2c314390bf")
if(NOT EXISTS "${upstream_ble_svc_hid}")
    message(FATAL_ERROR
        "ESP-IDF v5.5.5 NimBLE HID service not found at '${upstream_ble_svc_hid}'")
endif()
file(SHA256 "${upstream_ble_svc_hid}" upstream_ble_svc_hid_sha256)
if(NOT upstream_ble_svc_hid_sha256 STREQUAL
       expected_upstream_ble_svc_hid_sha256)
    message(FATAL_ERROR
        "ESP-IDF bt/ble_svc_hid.c does not match the audited v5.5.5 source. "
        "Expected ${expected_upstream_ble_svc_hid_sha256}, "
        "got ${upstream_ble_svc_hid_sha256}.")
endif()

idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    return()
endif()

set(srcs
    "${upstream_esp_hid_dir}/src/esp_hidd.c"
    "${upstream_esp_hid_dir}/src/esp_hidh.c"
    "${upstream_esp_hid_dir}/src/esp_hid_common.c")

if(CONFIG_BT_ENABLED)
    if(CONFIG_BT_BLUEDROID_ENABLED)
        list(APPEND srcs
            "${upstream_esp_hid_dir}/src/ble_hidd.c"
            "${upstream_esp_hid_dir}/src/ble_hidh.c"
            "${upstream_esp_hid_dir}/src/bt_hidh.c"
            "${upstream_esp_hid_dir}/src/bt_hidd.c")
    endif()
    if(CONFIG_BT_NIMBLE_ENABLED)
        list(APPEND srcs
            "${CMAKE_CURRENT_LIST_DIR}/src/nimble_hidd.c"
            "${upstream_esp_hid_dir}/src/nimble_hidh.c")
    endif()
endif()

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS
        "include"
        "${upstream_esp_hid_dir}/include"
    PRIV_INCLUDE_DIRS
        "${upstream_esp_hid_dir}/private"
    REQUIRES esp_event bt)

# The component-requirements discovery pass executes this file in CMake
# script mode, where target inspection is unavailable. Perform the surgical
# bt source swap only during the real configure pass, after both component
# targets exist.
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND
   NOT CMAKE_SCRIPT_MODE_FILE AND
   CONFIG_BT_NIMBLE_ENABLED)
    idf_component_get_property(bt_component_lib bt COMPONENT_LIB)
    get_target_property(bt_component_sources ${bt_component_lib} SOURCES)
    list(FIND bt_component_sources "${upstream_ble_svc_hid}"
         upstream_ble_svc_hid_index)
    if(upstream_ble_svc_hid_index EQUAL -1)
        message(FATAL_ERROR
            "Audited bt/ble_svc_hid.c was not found in the ESP-IDF bt target")
    endif()
    list(REMOVE_AT bt_component_sources ${upstream_ble_svc_hid_index})
    set_property(TARGET ${bt_component_lib} PROPERTY SOURCES
                 "${bt_component_sources}")
    target_sources(${bt_component_lib} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/src/ble_svc_hid.c")
    target_include_directories(${bt_component_lib} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include")
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
