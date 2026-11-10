# Shared, unmodified sources from the nRF firmware
set(SHARED_DIR ${CMAKE_CURRENT_LIST_DIR}/../../src)
set(EXTERNAL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../external)

idf_component_register(
    SRCS
        main.c
        ble_central.c
        bt_comm_esp32.c
        fob_store.c
        board.c
        ${SHARED_DIR}/nuki.c
        ${SHARED_DIR}/nuki_pairing.c
        ${SHARED_DIR}/nuki_unlock.c
        ${SHARED_DIR}/utils.c
        ${EXTERNAL_DIR}/tweetnacl.c
        sha2_port.c
        ${EXTERNAL_DIR}/hmac_sha256.c
    # shim/ provides nrf_log.h and nrf_sdh_soc.h for the shared sources
    INCLUDE_DIRS . shim ${SHARED_DIR} ${EXTERNAL_DIR}
    REQUIRES bt nvs_flash driver esp_hw_support led_strip
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE SHA2_USE_INTTYPES_H)
