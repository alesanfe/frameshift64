/* websocket_transport_internal.h - caps shared by the three backend TUs
 * (websocket_transport_win32/_curl/_fallback.cpp). Kept out of the public
 * header: these are receive-path tuning internals. */
#pragma once

#include <cstddef>

/* Message/queue caps: lobby text and relayed game frames are tiny, so these
 * are far above any legitimate need. They bound the damage a hostile or
 * stuck server can do - applied by each backend's receive loop. */
constexpr size_t kMaxWsMessageBytes = 16u << 20;   /* reassembled message cap */
constexpr size_t kMaxWsQueuedMessages = 4096;      /* pending for the game thread */
