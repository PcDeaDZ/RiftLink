/**
 * RiftLink — handlePacket для nRF52840 (логика выровнена с ESP `main.cpp`).
 */

#include <Arduino.h>
#include <atomic>
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

#define HANDSHAKE_TRAFFIC_QUIET_MS 3000
#define HELLO_STALE_KEY_REFRESH_MS 10000

static uint32_t s_handshakeQuietUntilMs = 0;
static inline void extendHandshakeQuiet(const char* cause, uint32_t durMs = HANDSHAKE_TRAFFIC_QUIET_MS) {
  uint32_t now = millis();
  uint32_t until = now + durMs;
  if ((int32_t)(until - s_handshakeQuietUntilMs) > 0) {
    s_handshakeQuietUntilMs = until;
  }
  RIFTLINK_DIAG("HELLO", "event=HANDSHAKE_QUIET cause=%s quiet_ms=%lu",
      cause ? cause : "-", (unsigned long)(s_handshakeQuietUntilMs - now));
}

static uint8_t s_fragOutBuf[frag::MAX_MSG_PLAIN];
static std::atomic<bool> s_pendingDiscoveryHello{false};

#include "handle_packet_nrf_helpers.inc"
#include "handle_packet_nrf_body.inc"
