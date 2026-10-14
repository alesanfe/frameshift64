#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ControllerBlockGameInput(uint16_t inputBlockId);
void ControllerUnblockGameInput(uint16_t inputBlockId);

// Returns 1 if game port `portIndex` has a real input source routed to it:
// an SDL gamepad routed to the port, a plugged-in GC-adapter controller
// routed to the port, a claimed Raphnet channel, or keyboard/mouse
// mappings (the PC's built-in input, defaulted onto port 1). Used to
// report honest N64 controller connection state to the game.
uint8_t ControllerPortHasPhysicalDevice(int32_t portIndex);

#ifdef __cplusplus
};
#endif
