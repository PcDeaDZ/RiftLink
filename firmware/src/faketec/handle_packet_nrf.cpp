/**
 * RiftLink — handlePacket для nRF52840 (логика выровнена с ESP `main.cpp`).
 */

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "handle_packet_nrf.h"
#include "async_tasks.h"
#include "ack_coalesce/ack_coalesce.h"
#include "beacon_sync/beacon_sync.h"
#include "ble/ble.h"
#include "clock_drift/clock_drift.h"
#include "collision_slots/collision_slots.h"
#include "compress/compress.h"
#include "crypto/crypto.h"
#include "frag/frag.h"
#include "gps/gps.h"
#include "groups/groups.h"
#include "log.h"
#include "msg_queue/msg_queue.h"
#include "neighbors/neighbors.h"
#include "network_coding/network_coding.h"
#include "node/node.h"
#include "offline_queue/offline_queue.h"
#include "pkt_cache/pkt_cache.h"
#include "protocol/packet.h"
#include "radio/radio.h"
#include "region/region.h"
#include "routing/routing.h"
#include "voice_buffers/voice_buffers.h"
#include "voice_frag/voice_frag.h"
#include "x25519_keys/x25519_keys.h"
#include "mesh_hello_nrf.h"

#define HELLO_STALE_KEY_REFRESH_MS 10000

static uint8_t s_fragOutBuf[frag::MAX_MSG_PLAIN];

#include "handle_packet_nrf_helpers.inc"
#include "handle_packet_nrf_body.inc"
