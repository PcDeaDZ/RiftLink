void handlePacket(const uint8_t* buf, size_t len, int rssi, uint8_t sf) {
  uint8_t decBuf[256];
  uint8_t tmpBuf[256];
  char msgStrBuf[256];
  uint8_t fwdBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];

  protocol::PacketHeader hdr;
  const uint8_t* payload = nullptr;
  size_t payloadLen = 0;
  protocol::ParseResult parseRes;
  static uint32_t s_rxLenMismatchCount = 0;
  if (!protocol::parsePacketEx(buf, len, &hdr, &payload, &payloadLen, &parseRes)) {
    // Strict mode: many short/broken self-echo fragments can trigger parse fail but are not real channel congestion.
    // Escalate congestion only for "substantial" malformed frames that look like real on-air collisions.
    bool hasStrictSync = (len >= 2 && buf[0] == protocol::SYNC_BYTE &&
        (((buf[1] & 0xF0) == protocol::VERSION_STRICT) || ((buf[1] & 0xF0) == protocol::VERSION_V2_PKTID)));
    bool doubleSyncNoise = (len >= 2 && buf[0] == protocol::SYNC_BYTE && buf[1] == protocol::SYNC_BYTE);
    bool substantialFrame = (len >= protocol::HEADER_LEN_BROADCAST + 12);
    bool likelyCollision = hasStrictSync && substantialFrame && !doubleSyncNoise &&
                           ((parseRes.status == protocol::ParseStatus::len_mismatch) ||
                            (parseRes.status == protocol::ParseStatus::payload_range));
    if (likelyCollision) {
      radio::notifyCongestion();
      region::switchChannelOnCongestion();
      collision_slots::recordCollision();
    }
    if (parseRes.status == protocol::ParseStatus::len_mismatch) {
      s_rxLenMismatchCount++;
    }
    // Short buffers: collision tail, noise, or truncated RX — cannot reconstruct missing bytes in SW.
    // Rate-limit UART: one summary line per window instead of hex on every fragment.
    {
      static uint32_t s_parseShortWindowCount = 0;
      static uint32_t s_parseShortLastLogMs = 0;
      constexpr uint32_t kParseShortLogPeriodMs = 5000;
      if (len <= 32) {
        s_parseShortWindowCount++;
        const uint32_t nowMs = millis();
        if (s_parseShortLastLogMs != 0 && (nowMs - s_parseShortLastLogMs) < kParseShortLogPeriodMs) {
          return;
        }
        RIFTLINK_DIAG("PARSE",
            "event=RX_PARSE_SHORT_SUMMARY n=%lu window~%ums last_status=%s (trunc/collision/noise; not recoverable) len_mismatch_total=%lu",
            (unsigned long)s_parseShortWindowCount, (unsigned)kParseShortLogPeriodMs,
            protocol::parseStatusToString(parseRes.status), (unsigned long)s_rxLenMismatchCount);
        s_parseShortWindowCount = 0;
        s_parseShortLastLogMs = nowMs;
        return;
      }
    }
    RIFTLINK_DIAG("PARSE", "event=RX_PARSE_FAIL status=%s len=%u expected=%u off=%u op=0x%02X pktId=%u rssi=%d sf=%u len_mismatch_total=%lu",
        protocol::parseStatusToString(parseRes.status), (unsigned)len, (unsigned)parseRes.expectedLen,
        (unsigned)parseRes.startOffset, (unsigned)parseRes.opcode, (unsigned)parseRes.pktId, rssi, (unsigned)sf,
        (unsigned long)s_rxLenMismatchCount);
    {
      static const char HEX_CHARS[] = "0123456789ABCDEF";
      constexpr size_t DUMP_BYTES = 24;
      char hexBuf[DUMP_BYTES * 2 + 4];
      size_t nDump = (len < DUMP_BYTES) ? len : DUMP_BYTES;
      size_t w = 0;
      for (size_t i = 0; i < nDump && (w + 2) < sizeof(hexBuf); i++) {
        uint8_t b = buf[i];
        hexBuf[w++] = HEX_CHARS[(b >> 4) & 0x0F];
        hexBuf[w++] = HEX_CHARS[b & 0x0F];
      }
      if (len > nDump && (w + 3) < sizeof(hexBuf)) {
        hexBuf[w++] = '.';
        hexBuf[w++] = '.';
        hexBuf[w++] = '.';
      }
      hexBuf[w] = '\0';
      RIFTLINK_DIAG("PARSE", "event=RX_PARSE_FAIL_HEX len=%u dump=%s",
          (unsigned)len, hexBuf);
    }
#if defined(DEBUG_PACKET_DUMP)
    Serial.printf("[RiftLink] Parse FAIL status=%s len=%u rssi=%d hex=",
        protocol::parseStatusToString(parseRes.status), (unsigned)len, rssi);
    for (size_t i = 0; i < len && i < 32; i++) Serial.printf("%02X", buf[i]);
    Serial.println();
#endif
    return;
  }

  size_t frameOffset = parseRes.startOffset;
  const uint8_t* frameBuf = buf + frameOffset;
  size_t frameLen = len - frameOffset;

  if (hdr.opcode == protocol::OP_KEY_EXCHANGE &&
      memcmp(hdr.from, node::getId(), protocol::NODE_ID_LEN) == 0) {
    return;
  }

#if defined(DEBUG_PACKET_DUMP)
  // Только успешный parsePacket: HELLO=13 B, KEY v2.1=55 B. Если в эфире только len=13 — полный KEY до парсера не дошёл (коллизия/очередь/не принят).
  Serial.printf("[PKT] len=%u rssi=%d sf=%u hex=", (unsigned)frameLen, rssi, sf);
  for (size_t i = 0; i < frameLen && i < 64; i++) Serial.printf("%02X", frameBuf[i]);
  if (frameLen > 64) Serial.print("...");
  Serial.println();
#endif

  // from=broadcast — некорректно, отбрасываем
  if (node::isBroadcast(hdr.from) || node::isInvalidNodeId(hdr.from)) return;
  // Любые self-echo из эфира игнорируем централизованно, чтобы не разгонять ping/hello/key loops.
  if (node::isForMe(hdr.from)) {
    RIFTLINK_DIAG("RADIO", "event=RX_DROP_DUP cause=self_from op=0x%02X pktId=%u",
        (unsigned)hdr.opcode, (unsigned)hdr.pktId);
    return;
  }
  uint32_t nowRxMs = millis();
  if (rxNoiseSeenRecently(hdr, payload, payloadLen, nowRxMs)) {
    rxNoiseDropAccount(hdr.opcode);
    RIFTLINK_DIAG("RADIO", "event=RX_DROP_NOISE op=0x%02X pktId=%u len=%u from=%02X%02X",
        (unsigned)hdr.opcode, (unsigned)hdr.pktId, (unsigned)payloadLen, hdr.from[0], hdr.from[1]);
    rxNoiseMaybeLogSummary(nowRxMs);
    return;
  }
  // Fast-path prefilter before heavy crypto branches.
  switch (hdr.opcode) {
    case protocol::OP_MSG:
    case protocol::OP_MSG_BATCH:
    case protocol::OP_GROUP_MSG:
    case protocol::OP_VOICE_MSG:
    case protocol::OP_TELEMETRY:
    case protocol::OP_LOCATION:
    case protocol::OP_ACK:
    case protocol::OP_ACK_BATCH:
    case protocol::OP_ACK_SELECTIVE:
    case protocol::OP_FRAG_CTRL:
    case protocol::OP_PARITY:
      if (!protocol::isEncrypted(hdr)) {
        RIFTLINK_DIAG("PARSE", "event=RX_DROP_FAST reason=missing_encrypted_flag op=0x%02X from=%02X%02X",
            (unsigned)hdr.opcode, hdr.from[0], hdr.from[1]);
        return;
      }
      break;
    default:
      break;
  }

  // OP_XOR_RELAY: попытка декодировать (если есть один из пакетов в кэше)
  if (hdr.opcode == protocol::OP_XOR_RELAY) {
    uint8_t decodedBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
    size_t decodedLen = 0;
    if (network_coding::onXorRelayReceived(frameBuf, frameLen, decodedBuf, &decodedLen) && decodedLen > 0) {
      handlePacket(decodedBuf, decodedLen, rssi, sf);
    }
    return;
  }

  // HELLO всегда broadcast — иначе сдвиг/коррупция (ghost-соседи)
  if (hdr.opcode == protocol::OP_HELLO && !node::isBroadcast(hdr.to)) return;

  // Relay: unicast не для нас, или GROUP_MSG (broadcast) с TTL>0
  // HELLO — всегда broadcast, не ретранслируем (защита от парсинга с перепутанным to)
  // KEY_EXCHANGE — только прямой unicast, не ретранслируем
  // ROUTE_REQ/REPLY обрабатываются модулем routing
  // Broadcast relay только при 2+ соседях — иначе ping-pong между двумя устройствами, HELLO не проходят
  bool needRelay = (hdr.ttl > 0) && (hdr.opcode != protocol::OP_ROUTE_REQ) &&
      (hdr.opcode != protocol::OP_ROUTE_REPLY) && (hdr.opcode != protocol::OP_HELLO) &&
      (hdr.opcode != protocol::OP_SF_BEACON) && (hdr.opcode != protocol::OP_NACK) &&
      (hdr.opcode != protocol::OP_KEY_EXCHANGE) && (
      (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) ||
      ((hdr.opcode == protocol::OP_GROUP_MSG || hdr.opcode == protocol::OP_VOICE_MSG || hdr.opcode == protocol::OP_SOS) &&
       neighbors::getCount() >= 2));
  uint32_t relayPayloadHashVal = 0;
  if (needRelay) {
    relayPayloadHashVal = relayPayloadHash(payload ? payload : (const uint8_t*)"", payloadLen);
    if (relayDedupSeen(hdr.from, relayPayloadHashVal)) {
      needRelay = false;
      relayHeard(hdr.from, relayPayloadHashVal);
    }
    if (needRelay) {
      if (hdr.opcode == protocol::OP_SOS) {
        if (relaySosRateLimitExceeded() || relaySosPerSourceLimitExceeded(hdr.from)) needRelay = false;
      } else if (relayRateLimitExceeded()) {
        needRelay = false;
      }
    }
  }
  if (needRelay) {
    relayDedupAdd(hdr.from, relayPayloadHashVal);
    if (hdr.opcode == protocol::OP_SOS) {
      relaySosRateRecord();
      relaySosPerSourceRecord(hdr.from);
    } else relayRateRecord();
    if (hdr.opcode == protocol::OP_MSG && hdr.pktId != 0 && !node::isBroadcast(hdr.to)) {
      pkt_cache::addOverheard(hdr.from, hdr.to, hdr.pktId, frameBuf, frameLen);
      offline_queue::enqueueCourier(frameBuf, frameLen);  // SCF courier для разорванных сегментов
    }

    uint8_t txSf = 0;
    if (node::isBroadcast(hdr.to)) {
      int minRssi = neighbors::getMinRssi();
      txSf = neighbors::rssiToSf(minRssi);
    } else {
      uint8_t nextHop[protocol::NODE_ID_LEN];
      if (routing::getNextHop(hdr.to, nextHop)) {
        int r = neighbors::getRssiFor(nextHop);
        txSf = neighbors::rssiToSf(r);
      }
      if (txSf == 0) txSf = 12;
    }
    int rssiClamp = (rssi < -120) ? -120 : (rssi > -40 ? -40 : rssi);
    int32_t d = 10 * (rssiClamp + 100);
    uint32_t delayMs = (d <= 0) ? 0 : ((d > 500) ? 500 : (uint32_t)d);
    if (hdr.opcode == protocol::OP_SOS) {
      delayMs += 60 + (uint32_t)(random() % 90);  // anti-storm backoff
    }

    bool xorSent = false;
    if (network_coding::addForXor(frameBuf, frameLen, hdr.from, hdr.to)) {
      uint8_t otherFrom[protocol::NODE_ID_LEN];
      uint32_t otherHash;
      network_coding::getLastPairOther(otherFrom, &otherHash);
      relayHeard(otherFrom, otherHash);
      uint8_t xorBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
      size_t xorLen = 0;
      if (network_coding::getXorPacket(xorBuf, sizeof(xorBuf), &xorLen)) {
        queueDeferredSend(xorBuf, xorLen, txSf, delayMs);
        xorSent = true;
      }
    }
    if (!xorSent) {
      uint8_t decodedBuf[protocol::PAYLOAD_OFFSET + protocol::MAX_PAYLOAD + crypto::OVERHEAD];
      size_t decodedLen = 0;
      if (network_coding::getDecodedFromPending(frameBuf, frameLen, hdr.from, hdr.to, hdr.pktId,
                                                decodedBuf, &decodedLen) && decodedLen > 0) {
        handlePacket(decodedBuf, decodedLen, rssi, sf);
      }
      memcpy(fwdBuf, frameBuf, frameLen);
      size_t ttlOff = node::isBroadcast(hdr.to)
          ? ((hdr.pktId != 0) ? 13 : 11)
          : ((hdr.pktId != 0) ? 21 : 19);
      if (ttlOff < frameLen) fwdBuf[ttlOff]--;
      queueDeferredRelay(fwdBuf, frameLen, txSf, delayMs, hdr.from, relayPayloadHashVal);
      if ((random() % 100) < 35) {  // Proof-of-Relay Lite sampling
        ble::notifyRelayProof(node::getId(), hdr.from, hdr.to, hdr.pktId, hdr.opcode);
      }
    }
  }

  if (!node::isForMe(hdr.to) && !node::isBroadcast(hdr.to)) {
    if (hdr.opcode == protocol::OP_NACK && payloadLen >= 2) {
      if (neighbors::isOnline(hdr.from) && !node::isInvalidNodeId(hdr.to)) {
        uint16_t pktId = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        if (pkt_cache::retransmitOverheard(hdr.from, hdr.to, pktId)) {
          mesh_hello_extend_quiet("nack_retransmit_overheard");
          RIFTLINK_LOG_EVENT("[RiftLink] Overhear retransmit pktId=%u\n", (unsigned)pktId);
        }
      } else {
        RIFTLINK_DIAG("NACK", "event=NACK_REJECT reason=overhear_source_invalid from=%02X%02X to=%02X%02X",
            hdr.from[0], hdr.from[1], hdr.to[0], hdr.to[1]);
      }
    }
    if (hdr.opcode == protocol::OP_ROUTE_REQ) {
      routing::onRouteReq(hdr.from, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_ROUTE_REPLY) {
      routing::onRouteReply(hdr.from, hdr.to, payload, payloadLen);
    } else if (hdr.opcode == protocol::OP_KEY_EXCHANGE && payloadLen == 32) {
      // Strict business logic: ignore KEY_EXCHANGE frames not addressed to this node.
      // Replying here can create unnecessary key chatter and delivery regressions.
      RIFTLINK_DIAG("KEY", "event=KEY_RX_SKIP reason=not_for_me from=%02X%02X to=%02X%02X pktId=%u",
          hdr.from[0], hdr.from[1], hdr.to[0], hdr.to[1], (unsigned)hdr.pktId);
    } else if (hdr.opcode == protocol::OP_KEY_EXCHANGE) {
      RIFTLINK_DIAG("KEY", "event=KEY_RX_PARSE_FAIL cause=payload_len_ne_32 from=%02X%02X payload=%u pktId=%u",
          hdr.from[0], hdr.from[1], (unsigned)payloadLen, (unsigned)hdr.pktId);
    } else if (hdr.opcode == protocol::OP_HELLO) {
      if (!validateHelloSenderTag(hdr, payload, payloadLen)) {
        RIFTLINK_DIAG("HELLO", "event=HELLO_DROP reason=tag_invalid from=%02X%02X len=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen);
        return;
      }
      beacon_sync::onBeaconReceived(hdr.from);
      offline_queue::onNodeOnline(hdr.from);
      if (neighbors::onHello(hdr.from, rssi)) {
        ble::requestNeighborsNotify();
        queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info при новом соседе
        RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
      }
      if (!x25519_keys::hasKeyFor(hdr.from)) {
        x25519_keys::sendKeyExchange(hdr.from, false, false, "hello_fwd");  // троттл 60с в x25519_keys
      }
    }
    return;
  }

  neighbors::updateRssi(hdr.from, rssi);

  switch (hdr.opcode) {
    case protocol::OP_KEY_EXCHANGE:
      RIFTLINK_DIAG("KEY", "event=KEY_RX_RAW from=%02X%02X len=%u rssi=%d sf=%u pktId=%u",
          hdr.from[0], hdr.from[1], (unsigned)payloadLen, rssi, (unsigned)sf, (unsigned)hdr.pktId);
      if (payloadLen == 32) {
        if (neighbors::onHello(hdr.from, rssi)) {
          ble::requestNeighborsNotify();
          queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info
          RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
        }
        bool keyMismatch = x25519_keys::isPeerPubKeyMismatch(hdr.from, payload);
        if (keyMismatch) {
          /* Пир сменил ключевую пару (перепрошивка, сброс NVS/FS) при том же node id — не блокировать;
           * пересчитать shared secret через onKeyExchange ниже. */
          RIFTLINK_DIAG("KEY", "event=KEY_ROTATE peer=%02X%02X pktId=%u (pubkey changed, accepting)",
              hdr.from[0], hdr.from[1], (unsigned)hdr.pktId);
          RIFTLINK_LOG_EVENT("[RiftLink] KEY_EXCHANGE: peer pubkey changed, re-key for %02X%02X\n",
              hdr.from[0], hdr.from[1]);
        }
        bool hadKey = x25519_keys::hasKeyFor(hdr.from);
        if (hadKey && !keyMismatch) {
          RIFTLINK_DIAG("KEY", "event=KEY_RX_DUP from=%02X%02X pktId=%u action=reply_with_throttle",
              hdr.from[0], hdr.from[1], (unsigned)hdr.pktId);
          mesh_hello_extend_quiet("key_rx_dup");
          // Peer may have lost pairwise state while keeping the same pubkey.
          // Reply with throttled KEY_EXCHANGE to heal asymmetry without KEY storm.
          x25519_keys::sendKeyExchange(hdr.from, true, true, "key_rx_dup");
          break;
        }
        mesh_hello_extend_quiet("key_rx_parsed");
        RIFTLINK_DIAG("KEY", "event=KEY_RX_PARSED_OK from=%02X%02X payload=%u pktId=%u hadKey=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen, (unsigned)hdr.pktId, (unsigned)hadKey);
        RIFTLINK_LOG_EVENT("[RiftLink] KEY_EXCHANGE rx parsed payload=%u from %02X%02X (→ onKeyExchange)\n",
            (unsigned)payloadLen, hdr.from[0], hdr.from[1]);
        x25519_keys::onKeyExchange(hdr.from, payload);
        if (x25519_keys::hasKeyFor(hdr.from)) {
          ble::requestNeighborsNotify();  // приложение: обновить hasKey (ожидание ключа → готов)
          x25519_keys::sendKeyExchange(hdr.from, true, false, "key_rx");
        }
      }
      else {
        RIFTLINK_DIAG("KEY", "event=KEY_RX_PARSE_FAIL cause=payload_len_ne_32 from=%02X%02X payload=%u pktId=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen, (unsigned)hdr.pktId);
      }
      break;

    case protocol::OP_ACK_SELECTIVE:
      if (!node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=selective_bad_direction from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (!protocol::isEncrypted(hdr)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=selective_unencrypted from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (payloadLen > 0) {
        uint8_t selPlain[64];
        size_t selPlainLen = sizeof(selPlain);
        if (!crypto::decryptFrom(hdr.from, payload, payloadLen, selPlain, &selPlainLen) || selPlainLen < 5) {
          RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=selective_decrypt_fail from=%02X%02X len=%u",
              hdr.from[0], hdr.from[1], (unsigned)payloadLen);
          break;
        }
        uint8_t selType = selPlain[0];
        if (selType == 1) {
          uint16_t batchPktId = (uint16_t)selPlain[1] | ((uint16_t)selPlain[2] << 8);
          uint16_t ackBitmap = (uint16_t)selPlain[3] | ((uint16_t)selPlain[4] << 8);
          msg_queue::onSelectiveAckReceived(hdr.from, batchPktId, ackBitmap, rssi, ble::notifyDelivered);
        }
      }
      break;

    case protocol::OP_FRAG_CTRL:
      if (!node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) break;
      if (!protocol::isEncrypted(hdr) || payloadLen == 0) break;
      {
        uint8_t ctrlPlain[64];
        size_t ctrlPlainLen = sizeof(ctrlPlain);
        if (!crypto::decryptFrom(hdr.from, payload, payloadLen, ctrlPlain, &ctrlPlainLen) || ctrlPlainLen < 10) break;
        if (ctrlPlain[0] == 0x01) {
          uint32_t fragMsgId = 0;
          memcpy(&fragMsgId, ctrlPlain + 1, 4);
          uint8_t total = ctrlPlain[5];
          uint32_t mask = (uint32_t)ctrlPlain[6] |
              ((uint32_t)ctrlPlain[7] << 8) |
              ((uint32_t)ctrlPlain[8] << 16) |
              ((uint32_t)ctrlPlain[9] << 24);
          frag::onFragCtrl(hdr.from, fragMsgId, total, mask);
        }
      }
      break;

    case protocol::OP_PARITY:
      // v3 adaptive redundancy helper frame: currently used for delivery robustness telemetry path.
      break;

    case protocol::OP_SF_BEACON:
      // SF beacon не используется в режиме фиксированного SF.
      break;

    case protocol::OP_HELLO: {
      if (!validateHelloSenderTag(hdr, payload, payloadLen)) {
        RIFTLINK_DIAG("HELLO", "event=HELLO_DROP reason=tag_invalid from=%02X%02X len=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen);
        break;
      }
      RIFTLINK_DIAG("HELLO", "event=HELLO_RX from=%02X%02X rssi=%d sf=%u heap=%u",
          hdr.from[0], hdr.from[1], rssi, (unsigned)sf, 0U);
      Serial.printf("[RiftLink] HELLO rx from %02X%02X rssi=%d heap=%u\n",
          hdr.from[0], hdr.from[1], rssi, 0U);
      uint32_t freshnessBeforeMs = neighbors::getFreshnessMs(hdr.from);
      bool staleSeenBefore = (freshnessBeforeMs != UINT32_MAX) && (freshnessBeforeMs >= HELLO_STALE_KEY_REFRESH_MS);
      clock_drift::onHelloReceived(hdr.from);
      offline_queue::onNodeOnline(hdr.from);
      bool rediscoveredPeer = false;
      {
        bool wasOnlineBefore = neighbors::isOnline(hdr.from);
        int nBefore = neighbors::getCount();
        if (neighbors::onHello(hdr.from, rssi)) {
          Serial.printf("[RiftLink] Discovery: new neighbor %02X%02X heap=%u\n",
              hdr.from[0], hdr.from[1], 0U);
          if (nBefore == 0) {
            // Не вызывать sendHello() здесь (packetTask) — только очередь TX из loop
            // и долго держит s_radioMutex — radioScheduler в это время ждёт mutex → «мертвый» mesh, loop залипает.
            mesh_hello_request_discovery();
          }
          ble::requestNeighborsNotify();
          queueDisplayRequestInfoRedraw();  // Paper: обновить вкладку Info при новом соседе
          RIFTLINK_LOG_EVENT("[RiftLink] Neighbor: %02X%02X\n", hdr.from[0], hdr.from[1]);
        } else if (!wasOnlineBefore) {
          ble::requestNeighborsNotify();
          rediscoveredPeer = true;
          RIFTLINK_DIAG("NEIGH", "event=NEIGHBOR_REDISCOVER peer=%02X%02X had_key=%u",
              hdr.from[0], hdr.from[1], (unsigned)x25519_keys::hasKeyFor(hdr.from));
        }
      }
      if (!x25519_keys::hasKeyFor(hdr.from)) {
        x25519_keys::sendKeyExchange(hdr.from, false, false, "hello");  // троттл 60с в x25519_keys
      } else if (rediscoveredPeer) {
        RIFTLINK_DIAG("KEY", "event=KEY_REUSE_ATTEMPT peer=%02X%02X reason=hello_rediscover",
            hdr.from[0], hdr.from[1]);
        // Rediscover after peer reboot: push our pubkey immediately to heal asymmetry.
        // Use force first-response path to bypass long throttle/debounce window.
        x25519_keys::sendKeyExchange(hdr.from, true, false, "hello_rediscover");
      } else if (staleSeenBefore) {
        // Peer looked stale right before HELLO refresh; this often means reboot on the other side.
        // Proactively re-announce our pubkey to recover one-sided "A has key, B doesn't" state.
        RIFTLINK_DIAG("KEY", "event=KEY_REUSE_ATTEMPT peer=%02X%02X reason=hello_stale_refresh age_ms=%lu",
            hdr.from[0], hdr.from[1], (unsigned long)freshnessBeforeMs);
        x25519_keys::sendKeyExchange(hdr.from, true, true, "hello_stale_refresh");
      }
      break;
    }

    case protocol::OP_MSG:
      if (payloadLen > 0) {
        RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=op_msg from=%02X%02X len=%u rssi=%d sf=%u encrypted=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen, rssi, (unsigned)sf, (unsigned)protocol::isEncrypted(hdr));
        if (protocol::isEncrypted(hdr)) {
          size_t decLen = 0;
          if (!crypto::decryptFrom(hdr.from, payload, payloadLen, decBuf, &decLen) || decLen >= 256) {
            RIFTLINK_LOG_ERR("[RiftLink] Decrypt FAILED (from %02X%02X — no key?)\n", hdr.from[0], hdr.from[1]);
            bool hasKeyNow = x25519_keys::hasKeyFor(hdr.from);
            RIFTLINK_DIAG("KEY", "event=KEY_DECRYPT_FAIL type=op_msg from=%02X%02X has_key=%u len=%u pktId=%u",
                hdr.from[0], hdr.from[1], (unsigned)hasKeyNow, (unsigned)payloadLen, (unsigned)hdr.pktId);
            // Self-heal both paths:
            // - no key locally
            // - key exists, but pairwise state may be stale/asymmetric
            x25519_keys::sendKeyExchange(hdr.from, true, hasKeyNow,
                hasKeyNow ? "decrypt_fail_msg" : "msg_no_key");
            break;
          }
          if (protocol::isCompressed(hdr)) {
            size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
            if (d == 0 || d >= 256) {
              RIFTLINK_LOG_ERR("[RiftLink] Decompress FAILED\n");
              break;
            }
            memcpy(decBuf, tmpBuf, d);
            decLen = d;
          }
          const char* msg;
          size_t msgLen;
          uint32_t msgId = 0;
          const bool isBroadcastMsg = node::isBroadcast(hdr.to);
          const bool ackEligible = protocol::isAckReq(hdr) &&
                                   decLen >= msg_queue::MSG_ID_LEN &&
                                   node::isForMe(hdr.to) &&
                                   !isBroadcastMsg;
          if (ackEligible) {
            memcpy(&msgId, decBuf, msg_queue::MSG_ID_LEN);
            uint8_t txSf = neighbors::rssiToSfOrthogonal(hdr.from);
            if (txSf == 0) txSf = 12;
            ack_coalesce::add(hdr.from, msgId, txSf);
            // Unicast: ACK only. ECHO witness is reserved for broadcast/group paths.
            size_t msgOff = msg_queue::MSG_ID_LEN;
            if (decLen > msgOff) {
              uint8_t flags = decBuf[msgOff++];
              if ((flags & 0x01) != 0 && decLen >= msgOff + 1) {
                uint8_t piggyCount = decBuf[msgOff++];
                for (uint8_t pi = 0; pi < piggyCount && decLen >= msgOff + msg_queue::MSG_ID_LEN; pi++) {
                  uint32_t piggyMsgId = 0;
                  memcpy(&piggyMsgId, decBuf + msgOff, msg_queue::MSG_ID_LEN);
                  msgOff += msg_queue::MSG_ID_LEN;
                  uint8_t singlePayload[msg_queue::MSG_ID_LEN];
                  memcpy(singlePayload, &piggyMsgId, msg_queue::MSG_ID_LEN);
                  if (msg_queue::onAckReceived(hdr.from, singlePayload, msg_queue::MSG_ID_LEN, false, true, true)) {
                    ble::notifyDelivered(hdr.from, piggyMsgId, rssi);
                  }
                }
              }
            }
            msg = (const char*)(decBuf + msgOff);
            msgLen = (decLen > msgOff) ? (decLen - msgOff) : 0;
          } else {
            if (protocol::isAckReq(hdr) && isBroadcastMsg) {
              RIFTLINK_DIAG("ACK", "event=ACK_SUPPRESSED type=op_msg mode=broadcast_echo_only from=%02X%02X",
                  hdr.from[0], hdr.from[1]);
            }
            msg = (const char*)(decBuf);
            msgLen = decLen;
          }
          if (msgLen < 256) {
            bool skipDisplay = (msgId != 0 && ucDedupSeen(hdr.from, msgId));
            if (!skipDisplay) {
              if (msgId != 0) ucDedupAdd(hdr.from, msgId);
              if (msgLen > 0) {
                memcpy(msgStrBuf, msg, msgLen);
                msgStrBuf[msgLen] = '\0';
              } else {
                strncpy(msgStrBuf, "(пустое)", sizeof(msgStrBuf) - 1);
                msgStrBuf[sizeof(msgStrBuf) - 1] = '\0';
              }
              ble::requestMsgNotify(hdr.from, msgStrBuf, msgId, rssi, 0,
                  (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal", "text");
              RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=request_msg_notify from=%02X%02X msgId=%u len=%u lane=%s type=text",
                  hdr.from[0], hdr.from[1], (unsigned)msgId, (unsigned)msgLen,
                  (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal");
              char fromHex[17];
              snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X%02X%02X%02X%02X",
                  hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3], hdr.from[4], hdr.from[5], hdr.from[6], hdr.from[7]);
              queueDisplayLastMsg(fromHex, msgStrBuf);
            }
          }
        } else {
          RIFTLINK_DIAG("PARSE", "event=RX_DROP_STRICT reason=msg_not_encrypted from=%02X%02X pktId=%u",
              hdr.from[0], hdr.from[1], (unsigned)hdr.pktId);
        }
      }
      break;

    case protocol::OP_MSG_BATCH:
      if (payloadLen >= 1 && node::isForMe(hdr.to)) {
        uint8_t count = payload[0];
        if (count >= 1 && count <= 8) {
          uint16_t ackBitmap = 0;
          size_t off = 1;
          for (uint8_t i = 0; i < count && off + 2 <= payloadLen; i++) {
            uint16_t encLen = (uint16_t)payload[off] | ((uint16_t)payload[off + 1] << 8);
            off += 2;
            if (encLen == 0 || off + encLen > payloadLen) break;
            const uint8_t* encPtr = payload + off;
            off += encLen;
            size_t decLen = 0;
            if (!crypto::decryptFrom(hdr.from, encPtr, encLen, decBuf, &decLen) || decLen >= 256) {
              bool hasKeyNow = x25519_keys::hasKeyFor(hdr.from);
              RIFTLINK_DIAG("KEY", "event=KEY_DECRYPT_FAIL type=op_msg_batch from=%02X%02X has_key=%u len=%u pktId=%u",
                  hdr.from[0], hdr.from[1], (unsigned)hasKeyNow, (unsigned)encLen, (unsigned)hdr.pktId);
              x25519_keys::sendKeyExchange(hdr.from, true, hasKeyNow,
                  hasKeyNow ? "decrypt_fail_batch" : "msg_batch_no_key");
              continue;
            }
            size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
            if (d > 0 && d < 256) {
              memcpy(decBuf, tmpBuf, d);
              decLen = d;
            }
            if (decLen >= (msg_queue::MSG_ID_LEN + 1)) {
              size_t msgOff = 0;
              uint32_t msgId = 0;
              memcpy(&msgId, decBuf + msgOff, msg_queue::MSG_ID_LEN);
              msgOff += msg_queue::MSG_ID_LEN;
              uint8_t flags = decBuf[msgOff++];
              if ((flags & 0x01) != 0 && decLen >= msgOff + 1) {
                uint8_t piggyCount = decBuf[msgOff++];
                for (uint8_t pi = 0; pi < piggyCount && decLen >= msgOff + msg_queue::MSG_ID_LEN; pi++) {
                  uint32_t piggyMsgId = 0;
                  memcpy(&piggyMsgId, decBuf + msgOff, msg_queue::MSG_ID_LEN);
                  msgOff += msg_queue::MSG_ID_LEN;
                  uint8_t singlePayload[msg_queue::MSG_ID_LEN];
                  memcpy(singlePayload, &piggyMsgId, msg_queue::MSG_ID_LEN);
                  if (msg_queue::onAckReceived(hdr.from, singlePayload, msg_queue::MSG_ID_LEN, false, true, true)) {
                    ble::notifyDelivered(hdr.from, piggyMsgId, rssi);
                  }
                }
              }
              ackBitmap |= (uint16_t)(1u << i);
              size_t msgLen = decLen - msgOff;
              if (msgLen < 256 && !ucDedupSeen(hdr.from, msgId)) {
                ucDedupAdd(hdr.from, msgId);
                memcpy(msgStrBuf, decBuf + msgOff, msgLen);
                msgStrBuf[msgLen] = '\0';
                ble::requestMsgNotify(hdr.from, msgStrBuf, msgId, rssi, 0,
                    (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal", "text");
                RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=request_msg_notify from=%02X%02X msgId=%u len=%u lane=%s type=text",
                    hdr.from[0], hdr.from[1], (unsigned)msgId, (unsigned)msgLen,
                    (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal");
                char fromHex[17];
                snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X%02X%02X%02X%02X",
                    hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3], hdr.from[4], hdr.from[5], hdr.from[6], hdr.from[7]);
                queueDisplayLastMsg(fromHex, msgStrBuf);
              }
            }
          }
          if (ackBitmap != 0 && hdr.pktId != 0) {
            uint8_t ackSelPlain[5];
            ackSelPlain[0] = 1;  // batch selective ack
            ackSelPlain[1] = (uint8_t)(hdr.pktId & 0xFF);
            ackSelPlain[2] = (uint8_t)(hdr.pktId >> 8);
            ackSelPlain[3] = (uint8_t)(ackBitmap & 0xFF);
            ackSelPlain[4] = (uint8_t)(ackBitmap >> 8);
            uint8_t ackSelEnc[5 + crypto::OVERHEAD];
            size_t ackSelEncLen = sizeof(ackSelEnc);
            if (!crypto::encryptFor(hdr.from, ackSelPlain, sizeof(ackSelPlain), ackSelEnc, &ackSelEncLen)) {
              RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=encrypt_fail from=%02X%02X type=selective",
                  hdr.from[0], hdr.from[1]);
              break;
            }
            uint8_t ackPkt[protocol::PAYLOAD_OFFSET + 96];
            size_t ackLen = protocol::buildPacket(ackPkt, sizeof(ackPkt),
                node::getId(), hdr.from, 31, protocol::OP_ACK_SELECTIVE,
                ackSelEnc, ackSelEncLen, true, false);
            if (ackLen > 0) {
              uint8_t txSf = neighbors::rssiToSfOrthogonal(hdr.from);
              if (txSf == 0) txSf = 12;
              queueDeferredAck(ackPkt, ackLen, txSf, 30);
            }
          }
        }
      }
      break;

    case protocol::OP_ACK:
      if (!node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=bad_direction from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (!protocol::isEncrypted(hdr)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=strict_unencrypted_reject from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (payloadLen > 0) {
        uint8_t ackPlain[32];
        size_t ackPlainLen = sizeof(ackPlain);
        if (!crypto::decryptFrom(hdr.from, payload, payloadLen, ackPlain, &ackPlainLen) ||
            ackPlainLen != msg_queue::MSG_ID_LEN) {
          RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=decrypt_or_len from=%02X%02X len=%u",
              hdr.from[0], hdr.from[1], (unsigned)payloadLen);
          bool hasKeyNow = x25519_keys::hasKeyFor(hdr.from);
          x25519_keys::sendKeyExchange(hdr.from, true, hasKeyNow,
              hasKeyNow ? "decrypt_fail_ack" : "ack_no_key");
          break;
        }
        uint32_t msgId = 0;
        memcpy(&msgId, ackPlain, msg_queue::MSG_ID_LEN);
        if (msg_queue::onAckReceived(hdr.from, ackPlain, ackPlainLen, false, true, true)) {
          ble::notifyDelivered(hdr.from, msgId, rssi);
        } else if (voice_frag::matchAck(hdr.from, msgId)) {
          ble::notifyDelivered(hdr.from, msgId, rssi);
          RIFTLINK_DIAG("ACK", "event=VOICE_ACK_MATCHED from=%02X%02X msgId=%u", hdr.from[0], hdr.from[1], (unsigned)msgId);
        }
      }
      break;

    case protocol::OP_ACK_BATCH:
      if (!node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=batch_bad_direction from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (!protocol::isEncrypted(hdr)) {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=strict_unencrypted_reject_batch from=%02X%02X", hdr.from[0], hdr.from[1]);
        break;
      }
      if (payloadLen > 0) {
        uint8_t batchPlain[64];
        size_t batchPlainLen = sizeof(batchPlain);
        if (!crypto::decryptFrom(hdr.from, payload, payloadLen, batchPlain, &batchPlainLen)) {
          RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=batch_decrypt_fail from=%02X%02X len=%u",
              hdr.from[0], hdr.from[1], (unsigned)payloadLen);
          bool hasKeyNow = x25519_keys::hasKeyFor(hdr.from);
          x25519_keys::sendKeyExchange(hdr.from, true, hasKeyNow,
              hasKeyNow ? "decrypt_fail_ack_batch" : "ack_batch_no_key");
          break;
        }
        msg_queue::onAckBatchReceived(hdr.from, batchPlain, batchPlainLen, rssi, ble::notifyDelivered);
      }
      break;

    case protocol::OP_ECHO:
      // Echo Protocol: broadcast msgId+originalFrom — отправитель принимает как ACK
      if (payloadLen >= 12 && memcmp(payload + msg_queue::MSG_ID_LEN, node::getId(), protocol::NODE_ID_LEN) == 0) {
        uint32_t msgId;
        memcpy(&msgId, payload, msg_queue::MSG_ID_LEN);
        (void)msg_queue::onBroadcastAckWitness(hdr.from, msgId, false);
      } else {
        RIFTLINK_DIAG("ACK", "event=ACK_REJECT reason=echo_payload_or_target from=%02X%02X len=%u",
            hdr.from[0], hdr.from[1], (unsigned)payloadLen);
      }
      break;

    case protocol::OP_POLL:
      // RIT: получатель шлёт «присылайте пакеты» — ускоряем отправку pending для него
      msg_queue::onPollReceived(hdr.from);
      break;

    case protocol::OP_READ:
      if (payloadLen >= msg_queue::MSG_ID_LEN) {
        uint32_t msgId;
        memcpy(&msgId, payload, msg_queue::MSG_ID_LEN);
        ble::notifyRead(hdr.from, msgId, rssi);
      }
      break;

    case protocol::OP_TELEMETRY:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        uint8_t tbuf[64];
        size_t tlen = 0;
        if (crypto::decrypt(payload, payloadLen, tbuf, &tlen) && tlen >= 4) {
          uint16_t batteryMv = 0;
          uint16_t heapKb = 0;
          memcpy(&batteryMv, tbuf, 2);
          memcpy(&heapKb, tbuf + 2, 2);
          neighbors::updateBattery(hdr.from, batteryMv);
          if (node::isForMe(hdr.from)) {
            ble::notifyTelemetry(hdr.from, batteryMv, heapKb, rssi);
          } else {
            RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_filter action=skip evt=telemetry from=%02X%02X",
                hdr.from[0], hdr.from[1]);
          }
        }
      }
      break;

    case protocol::OP_LOCATION:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        uint8_t decBuf[32];
        size_t decLen = 0;
        if (crypto::decrypt(payload, payloadLen, decBuf, &decLen) && decLen >= 10) {
          int32_t lat7, lon7;
          int16_t alt;
          memcpy(&lat7, decBuf, 4);
          memcpy(&lon7, decBuf + 4, 4);
          memcpy(&alt, decBuf + 8, 2);
          float lat = float(lat7) / 1e7f;
          float lon = float(lon7) / 1e7f;
          uint32_t nowMs = millis();
          if (isLocationSpoofLike(hdr.from, lat, lon, nowMs)) {
            RIFTLINK_LOG_ERR("[RiftLink] LOCATION dropped (anti-spoof) from %02X%02X lat=%.5f lon=%.5f\n",
                hdr.from[0], hdr.from[1], lat, lon);
            break;
          }
          bool inGeo = true;
          if (decLen >= 16) {
            uint16_t radiusM = 0;
            uint32_t expiryEpochSec = 0;
            memcpy(&radiusM, decBuf + 10, 2);
            memcpy(&expiryEpochSec, decBuf + 12, 4);
            if (radiusM > GEOFENCE_RADIUS_MAX_M) {
              RIFTLINK_LOG_ERR("[RiftLink] LOCATION dropped (radius too large=%u) from %02X%02X\n",
                  (unsigned)radiusM, hdr.from[0], hdr.from[1]);
              break;
            }
            if (expiryEpochSec > 0 && expiryEpochSec < 1700000000UL) {
              RIFTLINK_LOG_ERR("[RiftLink] LOCATION dropped (bad expiry=%u) from %02X%02X\n",
                  (unsigned)expiryEpochSec, hdr.from[0], hdr.from[1]);
              break;
            }
            if (expiryEpochSec > 0) {
              uint32_t nowEpoch = 0;
              if (gps::getEpochSec(&nowEpoch)) {
                if (nowEpoch > expiryEpochSec) inGeo = false;
              } else {
                inGeo = true;  // без epoch-времени не отбрасываем пакет
              }
            }
            if (inGeo && radiusM > 0 && gps::hasFix()) {
              float dLat = (gps::getLat() - lat) * 111000.0f;
              float dLon = (gps::getLon() - lon) * 111000.0f;
              float dist2 = dLat * dLat + dLon * dLon;
              inGeo = dist2 <= (float)radiusM * (float)radiusM;
            }
          }
          if (inGeo) {
            rememberLocationTrust(hdr.from, lat, lon, nowMs);
            if (node::isForMe(hdr.from)) {
              ble::notifyLocation(hdr.from, lat, lon, alt, rssi);
            } else {
              RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_filter action=skip evt=location from=%02X%02X",
                  hdr.from[0], hdr.from[1]);
            }
          }
        }
      }
      break;

    case protocol::OP_SOS:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        size_t decLen = 0;
        if (crypto::decrypt(payload, payloadLen, decBuf, &decLen) && decLen >= msg_queue::MSG_ID_LEN) {
          uint32_t msgId = 0;
          memcpy(&msgId, decBuf, msg_queue::MSG_ID_LEN);
          size_t msgLen = decLen - msg_queue::MSG_ID_LEN;
          if (msgLen > 0 && msgLen < sizeof(msgStrBuf)) {
            memcpy(msgStrBuf, decBuf + msg_queue::MSG_ID_LEN, msgLen);
            msgStrBuf[msgLen] = '\0';
            ble::requestMsgNotify(hdr.from, msgStrBuf, msgId, rssi, 0, "critical", "sos");
            RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=request_msg_notify from=%02X%02X msgId=%u len=%u lane=critical type=sos",
                hdr.from[0], hdr.from[1], (unsigned)msgId, (unsigned)msgLen);
          }
        }
      }
      break;

    case protocol::OP_PING:
      if (node::isForMe(hdr.to) || node::isBroadcast(hdr.to)) {
        uint32_t nowMs = millis();
        uint16_t pingKey = hdr.pktId;
        // Для ping без pktId используем deterministic fallback-ключ, чтобы не отвечать штормом.
        if (pingKey == 0) {
          pingKey = (uint16_t)(((uint16_t)hdr.from[0] << 8) | (uint16_t)hdr.from[1]);
        }
        if (pingReplySeenRecently(hdr.from, pingKey, nowMs)) {
          RIFTLINK_DIAG("QUEUE", "event=TX_DROP cause=ping_dup_guard from=%02X%02X pktId=%u",
              hdr.from[0], hdr.from[1], (unsigned)pingKey);
          break;
        }
        pingReplyMarkSeen(hdr.from, pingKey, nowMs);
        // Эхо pktId из OP_PING в OP_PONG — на приёмнике совпадение с исходным пингом по эфиру.
        uint8_t pongPkt[protocol::SYNC_LEN + protocol::HEADER_LEN_PKTID];
        size_t pongLen = protocol::buildPacket(pongPkt, sizeof(pongPkt),
            node::getId(), hdr.from, 31, protocol::OP_PONG, nullptr, 0,
            false, false, false, protocol::CHANNEL_DEFAULT, hdr.pktId);
        if (pongLen > 0) {
          uint8_t txSf = neighbors::rssiToSf(neighbors::getRssiFor(hdr.from));
          if (txSf == 0) txSf = 12;
          char reasonBuf[40];
          if (!queueTxPacket(pongPkt, pongLen, txSf, true, TxRequestClass::control, reasonBuf, sizeof(reasonBuf))) {
            queueDeferredSend(pongPkt, pongLen, txSf, 50, true);  // deferred send при полной radioCmdQueue
            RIFTLINK_DIAG("PING", "event=PONG_TX_DEFER to=%02X%02X cause=%s",
                hdr.from[0], hdr.from[1], reasonBuf[0] ? reasonBuf : "?");
          }
        }
      }
      break;

    case protocol::OP_ROUTE_REQ:
      routing::onRouteReq(hdr.from, payload, payloadLen);
      break;

    case protocol::OP_ROUTE_REPLY:
      if (routing::onRouteReply(hdr.from, hdr.to, payload, payloadLen) && node::isForMe(hdr.to)) {
        ble::notifyRoutes();
      }
      break;

    case protocol::OP_PONG:
      ble::clearPingRetryForPeer(hdr.from);
      ble::notifyPong(hdr.from, rssi, hdr.pktId);
      break;

    case protocol::OP_NACK:
      if (payloadLen >= 2 && node::isForMe(hdr.to)) {
        if (!neighbors::isOnline(hdr.from)) {
          RIFTLINK_DIAG("NACK", "event=NACK_REJECT reason=from_offline from=%02X%02X", hdr.from[0], hdr.from[1]);
          break;
        }
        uint16_t pktId = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        if (pkt_cache::retransmitOnNack(hdr.from, pktId)) {
          mesh_hello_extend_quiet("nack_retransmit");
          region::switchChannelOnCongestion();  // Channel Hopping при NACK
          RIFTLINK_LOG_EVENT("[RiftLink] NACK retransmit pktId=%u to %02X%02X\n",
              (unsigned)pktId, hdr.from[0], hdr.from[1]);
        }
      } else if (payloadLen >= 2) {
        RIFTLINK_DIAG("NACK", "event=NACK_REJECT reason=bad_direction from=%02X%02X", hdr.from[0], hdr.from[1]);
      }
      break;

    case protocol::OP_GROUP_MSG:
      if (payloadLen > 0 && protocol::isEncrypted(hdr)) {
        if (node::isForMe(hdr.from)) break;  // своё сообщение (relay echo) — пропуск
        size_t decLen = 0;
        bool decrypted = crypto::decrypt(payload, payloadLen, decBuf, &decLen);
        if (!decrypted) {
          // V2-only: пробуем только активные V2 group keys.
          const int v2Count = groups::getV2Count();
          for (int gi = 0; gi < v2Count; gi++) {
            char groupUid[groups::GROUP_UID_MAX_LEN + 1] = {0};
            char groupTag[groups::GROUP_TAG_MAX_LEN + 1] = {0};
            uint32_t channelId32 = 0;
            if (!groups::getV2At(gi, groupUid, sizeof(groupUid), &channelId32, groupTag, sizeof(groupTag), nullptr, 0, nullptr, nullptr, nullptr, nullptr)) continue;
            if (channelId32 == 0) continue;
            uint8_t gk[32];
            if (!groups::getGroupKeyV2(groupUid, gk, nullptr)) continue;
            size_t tryLen = 0;
            if (!crypto::decryptWithGroupKey(gk, payload, payloadLen, tmpBuf, &tryLen) || tryLen < GROUP_ID_LEN) continue;
            size_t plainLen = tryLen;
            uint8_t* plainPtr = tmpBuf;
            if (protocol::isCompressed(hdr)) {
              size_t d = compress::decompress(tmpBuf, tryLen, decBuf, sizeof(decBuf));
              if (d == 0 || d < GROUP_ID_LEN) continue;
              plainPtr = decBuf;
              plainLen = d;
            }
            uint32_t testGroupId = 0;
            memcpy(&testGroupId, plainPtr, GROUP_ID_LEN);
            if (testGroupId != channelId32) continue;
            memcpy(decBuf, plainPtr, plainLen);
            decLen = plainLen;
            decrypted = true;
            break;
          }
        } else if (protocol::isCompressed(hdr)) {
          size_t d = compress::decompress(decBuf, decLen, tmpBuf, sizeof(tmpBuf));
          if (d == 0 || d < GROUP_ID_LEN) break;
          memcpy(decBuf, tmpBuf, d);
          decLen = d;
        }
        if (!decrypted || decLen < GROUP_ID_LEN) break;
        uint32_t groupId;
        memcpy(&groupId, decBuf, GROUP_ID_LEN);
        char groupUid[groups::GROUP_UID_MAX_LEN + 1] = {0};
        // GROUP_ALL — служебный id широковещательных сообщений; не хранится в списке подписок
        if (groupId != groups::GROUP_ALL &&
            !groups::findGroupUidByChannelV2(groupId, groupUid, sizeof(groupUid))) {
          break;
        }
        const char* msg;
        size_t msgLen;
        uint32_t appMsgId = 0;
        if (decLen >= GROUP_ID_LEN + msg_queue::MSG_ID_LEN) {
          uint32_t bcMsgId;
          memcpy(&bcMsgId, decBuf + GROUP_ID_LEN, msg_queue::MSG_ID_LEN);
          if (bcDedupSeen(hdr.from, bcMsgId)) break;  // дубликат broadcast (3x repeat)
          bcDedupAdd(hdr.from, bcMsgId);
          appMsgId = bcMsgId;
          msg = (const char*)(decBuf + GROUP_ID_LEN + msg_queue::MSG_ID_LEN);
          msgLen = decLen - GROUP_ID_LEN - msg_queue::MSG_ID_LEN;
          // strict mode: для group/broadcast подтверждение только через ECHO witness (без ACK).
          uint8_t echoPayload[12];
          memcpy(echoPayload, &bcMsgId, msg_queue::MSG_ID_LEN);
          memcpy(echoPayload + msg_queue::MSG_ID_LEN, hdr.from, protocol::NODE_ID_LEN);
          uint8_t echoPkt[protocol::PAYLOAD_OFFSET + 32];
          size_t echoLen = protocol::buildPacket(echoPkt, sizeof(echoPkt),
              node::getId(), protocol::BROADCAST_ID, 31, protocol::OP_ECHO,
              echoPayload, sizeof(echoPayload), false, false);
          if (echoLen > 0) {
            uint8_t echoSf = neighbors::rssiToSf(neighbors::getMinRssi());
            if (echoSf == 0) echoSf = 12;
            uint32_t echoDelay = 220 + (uint32_t)(random() % 160);
            queueDeferredSend(echoPkt, echoLen, echoSf, echoDelay);
            RIFTLINK_DIAG("ACK", "event=ACK_SUPPRESSED type=group_msg mode=echo_only from=%02X%02X delay=%lu",
                hdr.from[0], hdr.from[1], (unsigned long)echoDelay);
          }
        } else {
          msg = (const char*)(decBuf + GROUP_ID_LEN);
          msgLen = decLen - GROUP_ID_LEN;
        }
        if (msgLen < 256) {
          memcpy(msgStrBuf, msg, msgLen);
          msgStrBuf[msgLen] = '\0';
          ble::requestMsgNotify(hdr.from, msgStrBuf, appMsgId, rssi, 0,
              (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal", "text",
              groupId, groupUid[0] ? groupUid : nullptr);
          RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=request_msg_notify from=%02X%02X msgId=%u len=%u lane=%s type=text",
              hdr.from[0], hdr.from[1], (unsigned)appMsgId, (unsigned)msgLen,
              (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal");
          char fromHex[17];
          snprintf(fromHex, sizeof(fromHex), "grp%u %02X%02X", (unsigned)groupId, hdr.from[0], hdr.from[1]);
          queueDisplayLastMsg(fromHex, msgStrBuf);
        }
      }
      break;

    case protocol::OP_MSG_FRAG:
      if (payloadLen >= frag::FRAG_HEADER_LEN) {
        size_t outLen = 0;
        uint32_t fragMsgId = 0;
        if (frag::onFragment(hdr.from, hdr.to, payload, payloadLen,
                             protocol::isCompressed(hdr), s_fragOutBuf, sizeof(s_fragOutBuf), &outLen, &fragMsgId)) {
          if (outLen > 0 && outLen < sizeof(s_fragOutBuf)) {
            s_fragOutBuf[outLen] = '\0';
            const char* msgStr = (const char*)s_fragOutBuf;
            if (fragMsgId == 0) {
              RIFTLINK_DIAG("FRAG", "event=FRAG_RX_DROP reason=msg_id_missing from=%02X%02X len=%u",
                  hdr.from[0], hdr.from[1], (unsigned)outLen);
              break;
            }
            ble::requestMsgNotify(hdr.from, msgStr, fragMsgId, rssi, 0,
                (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal", "text");
            RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_packet action=request_msg_notify from=%02X%02X msgId=%u len=%u lane=%s type=text",
                hdr.from[0], hdr.from[1], (unsigned)fragMsgId, (unsigned)outLen,
                (hdr.channel == protocol::CHANNEL_CRITICAL) ? "critical" : "normal");
            char fromHex[17];
            snprintf(fromHex, sizeof(fromHex), "%02X%02X%02X%02X%02X%02X%02X%02X",
                hdr.from[0], hdr.from[1], hdr.from[2], hdr.from[3], hdr.from[4], hdr.from[5], hdr.from[6], hdr.from[7]);
            queueDisplayLastMsg(fromHex, msgStr);
          }
        }
      }
      break;

    case protocol::OP_VOICE_MSG: {
      bool forMe = node::isForMe(hdr.to);
      RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_voice action=enter from=%02X%02X payloadLen=%u isForMe=%u",
          hdr.from[0], hdr.from[1], (unsigned)payloadLen, (unsigned)forMe);
      if (payloadLen >= 6 && forMe) {
        size_t outLen = 0;
        uint32_t voiceMsgId = 0;
        bool assembled = voice_frag::onFragment(hdr.from, hdr.to, payload, payloadLen,
                nullptr, voice_buffers_plain_cap(), &outLen, &voiceMsgId);
        if (assembled) {
          RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_voice action=assembled from=%02X%02X msgId=%u outLen=%u",
              hdr.from[0], hdr.from[1], (unsigned)voiceMsgId, (unsigned)outLen);
          if (outLen > 0) {
            ble::notifyVoice(hdr.from, voice_buffers_plain(), outLen, voiceMsgId);
            if (voiceMsgId != 0) {
              uint8_t txSf = neighbors::rssiToSfOrthogonal(hdr.from);
              if (txSf == 0) txSf = 12;
              ack_coalesce::add(hdr.from, voiceMsgId, txSf);
            }
          }
          voice_buffers_release();
        }
      } else if (!forMe) {
        RIFTLINK_DIAG("BLE_CHAIN", "stage=fw_handle_voice action=skip reason=not_for_me from=%02X%02X to=%02X%02X",
            hdr.from[0], hdr.from[1], hdr.to[0], hdr.to[1]);
      }
      break;
    }

    default:
      break;
  }
}
