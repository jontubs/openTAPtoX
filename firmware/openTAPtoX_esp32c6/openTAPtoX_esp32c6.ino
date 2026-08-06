#include "opentaptox_esp32c6_app.h"

// Stable Arduino entry point.
// Keep this file readable for newcomers: setup() starts the controller and
// loop() runs one gateway cycle. Shared logic should move into common modules,
// while board-specific details stay behind the ESP32-C6 app/runtime adapter.
void setup() {
  opentaptoxEsp32c6Setup();
}

void loop() {
  opentaptoxEsp32c6Loop();
}
