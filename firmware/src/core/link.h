#pragma once
// USB serial JSON-lines link to the host daemon. See PROTOCOL.md.
#include <ArduinoJson.h>

void link_init();
void link_poll();                          // read + dispatch incoming lines (call from loop)
void link_send(JsonDocument &doc);         // send one JSON line to host
void link_action(const char *action, const char *sid);  // {"t":"action",...}
