/* websocket_transport_internal.h - caps shared by the three backend TUs
 * (websocket_transport_win32/_curl/_fallback.cpp). Kept out of the public
 * header: these are receive-path tuning internals. */
#pragma once

#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

/* Message/queue caps: lobby text and relayed game frames are tiny, so these
 * are far above any legitimate need. They bound the damage a hostile or
 * stuck server can do - applied by each backend's receive loop. */
constexpr size_t kMaxWsMessageBytes = 16u << 20;   /* reassembled message cap */
constexpr size_t kMaxWsQueuedMessages = 4096;      /* pending for the game thread */

/* Push one complete message into the backend's ingress queues under `mu`.
 * The queues were file-static per TU pre-split; each backend passes its own.
 * false -> overflow: the caller fails the connection rather than letting a
 * flooding server grow the queue while the game thread is stalled. */
inline bool WsEnqueueBounded(std::mutex& mu,
                             std::deque<std::string>& text,
                             std::deque<std::vector<unsigned char>>& bin,
                             const std::vector<unsigned char>& message,
                             bool is_text)
{
    std::lock_guard<std::mutex> lock(mu);
    if (text.size() + bin.size() >= kMaxWsQueuedMessages) {
        return false;
    }
    if (is_text) {
        text.emplace_back(message.begin(), message.end());
    } else {
        bin.push_back(message);
    }
    return true;
}

/* Pop one queued text frame into `out` (NUL-terminated, truncated to
 * capacity-1). Shared by the three backends' port_ws_poll_text. */
inline int WsPollText(std::mutex& mu, std::deque<std::string>& text,
                      char* out, int capacity)
{
    std::lock_guard<std::mutex> lock(mu);
    if (text.empty() || capacity <= 0) {
        return 0;
    }
    std::string s = std::move(text.front());
    text.pop_front();
    const size_t n = (s.size() < static_cast<size_t>(capacity - 1))
                     ? s.size() : static_cast<size_t>(capacity - 1);
    std::memcpy(out, s.data(), n);
    out[n] = '\0';
    return 1;
}

/* Clear the ingress side for a fresh port_ws_connect: error string +
 * both queues. Backends tear down their own handles first (curl easy /
 * BSD socket / WinHTTP session), then call this under the queue mutex. */
inline void WsClearIngress(std::mutex& mu, std::string& error,
                           std::deque<std::string>& text,
                           std::deque<std::vector<unsigned char>>& bin)
{
    std::lock_guard<std::mutex> lock(mu);
    error.clear();
    text.clear();
    bin.clear();
}

/* Pop one queued binary frame into `out`. Oversized frames are dropped
 * (returns 0) rather than truncated mid-message. Shared by the three
 * backends' port_ws_poll_binary. */
inline int WsPollBinary(std::mutex& mu,
                        std::deque<std::vector<unsigned char>>& bin,
                        void* out, int capacity)
{
    std::lock_guard<std::mutex> lock(mu);
    if (bin.empty()) {
        return 0;
    }
    std::vector<unsigned char> data = std::move(bin.front());
    bin.pop_front();
    if (static_cast<int>(data.size()) > capacity) {
        return 0;   /* oversized: drop */
    }
    std::memcpy(out, data.data(), data.size());
    return static_cast<int>(data.size());
}
