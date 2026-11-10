// ESP32 side of the transport declared in src/bt_comm.h (send_buffer / send_with_response).
// Mirrors bt_comm_on_ble_evt() / process_messages() from src/bt_comm.c on top of NimBLE.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bt_comm.h"

void bt_comm_reset(void);
// Called from the NimBLE host task for every indication on the IO characteristic.
void bt_comm_on_indication(const uint8_t* data, uint16_t length);
// Called from the app task loop. Starts queued writes and dispatches completed responses
// to the protocol callbacks. Returns false on a transport/protocol error.
bool bt_comm_process(void* context, uint16_t connection_handle, uint16_t attribute_handle);
