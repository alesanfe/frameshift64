/* Test hooks for the emscripten WebSocket shim (fake_ems.cpp). The shim
 * headers under tests/support/emscripten_shim/ provide the emsdk API
 * surface; this fake implements it as an in-process transport whose
 * events are fired explicitly by the test — deterministic, single-
 * threaded, matching the browser event-delivery model. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fake_ems {

/* Clock — emscripten_get_now() reads this. */
void Reset();
void AdvanceNow(double ms);
double Now();

/* Socket event injection (browser would deliver these on the event loop). */
int  LastSocket();
void FireOpen(int ws);
void FireError(int ws);
void FireClose(int ws, unsigned short code, bool wasClean);
void FireMessageText(int ws, const std::string &payload);
void FireMessageBinary(int ws, const void *data, uint32_t size);

/* Fault/environment injection. */
void SetSendFails(int ws, bool fails);      /* send returns non-SUCCESS */
void SetSupported(bool supported);          /* is_supported() answer    */
void SetNextNewFails(bool fails);           /* websocket_new returns 0  */
void SetScriptResult(const char *result);   /* canned run_script result;
                                             * nullptr restores node exec */

/* Observability / resource accounting. */
int  NewCount();
int  DeleteCount();
int  CloseCount();
size_t SentBinaryBytes(int ws);
size_t SentTextBytes(int ws);
std::string UrlOf(int ws);

} // namespace fake_ems
