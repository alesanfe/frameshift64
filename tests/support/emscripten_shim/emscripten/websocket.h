/* Minimal emscripten/websocket.h shim — see ../emscripten.h. Signatures
 * mirror the emsdk API surface websocket_web.cpp uses; the fake
 * transport state machine lives in fake_ems.cpp. */
#pragma once

#include "../emscripten.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int EMSCRIPTEN_WEBSOCKET_T;
typedef int EMSCRIPTEN_RESULT;

#define EMSCRIPTEN_RESULT_SUCCESS 0
#define EMSCRIPTEN_RESULT_FAILED -1
#define EMSCRIPTEN_RESULT_INVALID_PARAM -2

typedef struct EmscriptenWebSocketCreateAttributes {
    const char *url;
    const char **protocols;
    EM_BOOL createOnMainThread;
} EmscriptenWebSocketCreateAttributes;

typedef struct EmscriptenWebSocketOpenEvent {
    EMSCRIPTEN_WEBSOCKET_T socket;
} EmscriptenWebSocketOpenEvent;

typedef struct EmscriptenWebSocketErrorEvent {
    EMSCRIPTEN_WEBSOCKET_T socket;
} EmscriptenWebSocketErrorEvent;

typedef struct EmscriptenWebSocketCloseEvent {
    EMSCRIPTEN_WEBSOCKET_T socket;
    EM_BOOL wasClean;
    unsigned short code;
    char reason[512];
} EmscriptenWebSocketCloseEvent;

typedef struct EmscriptenWebSocketMessageEvent {
    EMSCRIPTEN_WEBSOCKET_T socket;
    uint8_t *data;
    uint32_t numBytes;
    EM_BOOL isText;
} EmscriptenWebSocketMessageEvent;

typedef EM_BOOL (*em_socket_open_callback)(int,
    const EmscriptenWebSocketOpenEvent *, void *);
typedef EM_BOOL (*em_socket_error_callback)(int,
    const EmscriptenWebSocketErrorEvent *, void *);
typedef EM_BOOL (*em_socket_close_callback)(int,
    const EmscriptenWebSocketCloseEvent *, void *);
typedef EM_BOOL (*em_socket_message_callback)(int,
    const EmscriptenWebSocketMessageEvent *, void *);

EMSCRIPTEN_WEBSOCKET_T emscripten_websocket_new(
    EmscriptenWebSocketCreateAttributes *attrs);
EMSCRIPTEN_RESULT emscripten_websocket_set_onopen_callback(
    EMSCRIPTEN_WEBSOCKET_T ws, void *user, em_socket_open_callback cb);
EMSCRIPTEN_RESULT emscripten_websocket_set_onerror_callback(
    EMSCRIPTEN_WEBSOCKET_T ws, void *user, em_socket_error_callback cb);
EMSCRIPTEN_RESULT emscripten_websocket_set_onclose_callback(
    EMSCRIPTEN_WEBSOCKET_T ws, void *user, em_socket_close_callback cb);
EMSCRIPTEN_RESULT emscripten_websocket_set_onmessage_callback(
    EMSCRIPTEN_WEBSOCKET_T ws, void *user, em_socket_message_callback cb);
EMSCRIPTEN_RESULT emscripten_websocket_send_utf8_text(
    EMSCRIPTEN_WEBSOCKET_T ws, const char *text);
EMSCRIPTEN_RESULT emscripten_websocket_send_binary(
    EMSCRIPTEN_WEBSOCKET_T ws, void *data, uint32_t size);
EMSCRIPTEN_RESULT emscripten_websocket_close(
    EMSCRIPTEN_WEBSOCKET_T ws, unsigned short code, const char *reason);
EMSCRIPTEN_RESULT emscripten_websocket_delete(EMSCRIPTEN_WEBSOCKET_T ws);
EM_BOOL emscripten_websocket_is_supported(void);

#ifdef __cplusplus
}
#endif
