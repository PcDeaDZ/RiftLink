/**
 * RiftLink FakeTech V5 — nRF52840 (NiceNano) + HT-RA62/RA-01SH
 * Минимальный mesh: HELLO, MSG, ACK, relay
 */

#include <Arduino.h>
#include "board.h"
#include "storage.h"
#include "node.h"
#include "region.h"
#include "crypto.h"
#include "radio.h"
#include "display.h"
#include "ble.h"
#include "neighbors.h"
#include "protocol/packet.h"
#include <string.h>

#define HELLO_INTERVAL_MS  36000
#define HELLO_JITTER_MS    3000

static uint32_t lastHello = 0;
static uint8_t rxBuf[protocol::SYNC_LEN + protocol::HEADER_LEN + protocol::MAX_PAYLOAD + 64];

static void sendHello() {
  uint8_t pkt[64];
  size_t n = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), protocol::BROADCAST_ID,
      1, protocol::OP_HELLO,
      nullptr, 0, false, false, false, protocol::CHANNEL_DEFAULT, 0);
  if (n > 0) radio::send(pkt, n);
}

static void handlePacket(const uint8_t* buf, int len) {
  protocol::PacketHeader hdr;
  const uint8_t* payload;
  size_t payloadLen;

  if (!protocol::parsePacket(buf, len, &hdr, &payload, &payloadLen)) return;

  int rssi = radio::getLastRssi();

  switch (hdr.opcode) {
    case protocol::OP_HELLO:
      if (neighbors::onHello(hdr.from, rssi)) {
        if (display::isPresent()) {
          display::clear();
          display::setCursor(0, 0);
          display::print("Neighbors: ");
          display::print(neighbors::getCount());
          display::show();
        }
      }
      break;

    case protocol::OP_MSG: {
      if (node::isForMe(hdr.to)) {
        char text[protocol::MAX_PAYLOAD + 1];
        size_t copyLen = payloadLen < protocol::MAX_PAYLOAD ? payloadLen : protocol::MAX_PAYLOAD;
        memcpy(text, payload, copyLen);
        text[copyLen] = '\0';

        uint32_t msgId = 0;
        if (copyLen >= 4) msgId = (uint32_t)payload[0] | ((uint32_t)payload[1]<<8) |
            ((uint32_t)payload[2]<<16) | ((uint32_t)payload[3]<<24);

        ble::notifyMsg(hdr.from, text, msgId, rssi, 0);

        if (display::isPresent()) {
          display::clear();
          display::setCursor(0, 0);
          display::print("MSG:");
          display::setCursor(0, 1);
          for (size_t i = 4; i < copyLen && (i-4) < 16; i++)
            display::print((char)payload[i]);
          display::show();
        }

        if (protocol::isAckReq(hdr)) {
          uint8_t ackPayload[4];
          memcpy(ackPayload, payload, 4);
          uint8_t ackPkt[64];
          size_t an = protocol::buildPacket(ackPkt, sizeof(ackPkt),
              node::getId(), hdr.from, 1, protocol::OP_ACK,
              ackPayload, 4, false, false, false, protocol::CHANNEL_DEFAULT, 0);
          if (an > 0) radio::send(ackPkt, an);
        }
      } else if (hdr.ttl > 1) {
        uint8_t relayBuf[256];
        size_t relayLen = len < (int)sizeof(relayBuf) ? len : sizeof(relayBuf);
        memcpy(relayBuf, buf, relayLen);
        if (relayLen >= 12) relayBuf[11] = hdr.ttl - 1;
        radio::send(relayBuf, relayLen);
      }
      break;
    }

    case protocol::OP_ACK:
      break;

    default:
      break;
  }
}

static void onBleSend(const uint8_t* to, const char* text, uint8_t ttlMinutes) {
  (void)ttlMinutes;
  uint8_t plain[protocol::MAX_PAYLOAD];
  uint32_t msgId = (uint32_t)random(0x7FFFFFFF);
  memcpy(plain, &msgId, 4);
  size_t textLen = strlen(text);
  if (textLen > protocol::MAX_PAYLOAD - 4) textLen = protocol::MAX_PAYLOAD - 4;
  memcpy(plain + 4, text, textLen);

  uint8_t enc[protocol::MAX_PAYLOAD + 32];
  size_t encLen = sizeof(enc);
  if (!crypto::encryptFor(to, plain, 4 + textLen, enc, &encLen)) return;

  uint8_t pkt[256];
  size_t n = protocol::buildPacket(pkt, sizeof(pkt),
      node::getId(), to, 8, protocol::OP_MSG,
      enc, encLen, false, true, false, protocol::CHANNEL_DEFAULT, 0);
  if (n > 0) radio::send(pkt, n);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("[RiftLink] FakeTech V5");

  storage::init();
  node::init();
  region::init();
  crypto::init();
  neighbors::init();

  if (!radio::init()) {
    Serial.println("[RiftLink] Radio FAIL");
    ble::notifyError("radio", "init failed");
    for (;;) delay(1000);
  }

  display::init();
  ble::init();
  ble::setOnSend(onBleSend);

  if (display::isPresent()) {
    display::clear();
    display::setCursor(0, 0);
    display::print("RiftLink FT");
    display::setCursor(0, 1);
    const uint8_t* id = node::getId();
    char buf[20];
    sprintf(buf, "%02X%02X...", id[0], id[1]);
    display::print(buf);
    display::show();
  }

  lastHello = millis();
  Serial.println("[RiftLink] Ready");
}

void loop() {
  uint32_t now = millis();

  int n = radio::receive(rxBuf, sizeof(rxBuf));
  if (n > 0) {
    handlePacket(rxBuf, n);
  }

  if (now - lastHello >= HELLO_INTERVAL_MS) {
    lastHello = now + (random(HELLO_JITTER_MS * 2) - HELLO_JITTER_MS);
    sendHello();
  }

  ble::update();
  delay(10);
}
