/**
 * Unified command handler — wires WebSocket commands to the shared parser.
 */

#include "cmd_handler.h"
#include "ble/ble.h"
#include "ws_server/ws_server.h"

static void onWsCommand(const char* json, size_t len) {
  ble::processCommand((const uint8_t*)json, len);
}

namespace cmd_handler {

void init() {
  ws_server::setOnCommand(onWsCommand);
}

void process(const char* json, size_t len) {
  ble::processCommand((const uint8_t*)json, len);
}

}  // namespace cmd_handler
