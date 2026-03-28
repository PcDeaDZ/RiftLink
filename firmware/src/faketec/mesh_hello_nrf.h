/**
 * Периодический HELLO / POLL / handshake quiet для nRF52840 (паритет с ESP main.cpp).
 */
#pragma once

#include <stdint.h>

void mesh_hello_nrf_init();
void mesh_hello_nrf_loop();

bool mesh_hello_is_handshake_quiet_active();
void mesh_hello_extend_quiet(const char* cause, uint32_t durMs = 3000);
void mesh_hello_request_discovery();
