/* websocket_transport_internal.h - caps shared by the three backend TUs
 * (websocket_transport_win32/_curl/_fallback.cpp). Kept out of the public
 * header: these are receive-path tuning internals. */
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "port_log.h"
#include "websocket_transport.h"

/* Message/queue caps: lobby text and relayed game frames are tiny, so these
 * are far above any legitimate need. They bound the damage a hostile or
 * stuck server can do - applied by each backend's receive loop. */
constexpr size_t kMaxWsMessageBytes = 16u << 20;   /* reassembled message cap */
constexpr size_t kMaxWsQueuedMessages = 4096;      /* pending for the game thread */

/* Shared failure latch: record `why` under the queue mutex, flip the
 * connection state, log once. Each backend's globals live file-static in
 * its own TU; they pass them here. */
inline void WsFail(std::mutex& mu, std::string& error,
                   std::atomic<int>& state, const std::string& why)
{
    {
        std::lock_guard<std::mutex> lock(mu);
        error = why;
    }
    state = PORT_WS_FAILED;
    port_log("SSB64 WS: %s\n", why.c_str());
}

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

/* ── Lazy-reconnect uplink ────────────────────────────────────────────
 * The uplink is a second, send-only websocket to the same server so
 * control-socket reads can't stall gameplay frames. The locking
 * choreography — double-checked re-dial under mu serialized by ioMu —
 * was identical in all three backends with only the handle type and the
 * dial/kill primitives differing; it lives here once. Each backend keeps
 * its own socket slot (`sUpSock` / `sUpConn.socket` / `sUpCurl`) and
 * passes it by reference.
 *
 * LOCKING CONTRACT (load-bearing — see ws_uplink_test):
 *  1. Lock order is ioMu → mu. ioMu may be held while acquiring mu;
 *     mu must NEVER be held while acquiring ioMu. WsUplinkClose drops mu
 *     before taking ioMu precisely to preserve this.
 *  2. mu guards `url`, `retryAfterMs` and the backend's socket slot;
 *     `state` is atomic and may be read lock-free.
 *  3. ioMu serializes socket I/O: send, graceful close and lazy re-dial
 *     never overlap. A close can therefore never kill a socket mid-send.
 *  4. Backend callbacks must not call back into these helpers.
 *  5. WsUplinkConnect is race-safe against a concurrent install: the
 *     loser kills its fresh socket instead of orphaning it. Callers are
 *     still expected to serialize connects (the lobby drives it from a
 *     single thread) — the guard is a backstop, not the contract.
 *
 * Callback contracts (dial/send/kill/insideMu are the backend seams):
 *  - dial(url[, err]) → handle|bad: may block; runs under ioMu in the
 *    lazy path, lock-free in WsUplinkConnect. On failure it must free
 *    every resource it created and return `bad`.
 *  - send(handle) → bool: runs under ioMu; may take backend locks but
 *    not `up.mu`.
 *  - kill(handle): must be idempotent, noexcept, and must free the
 *    handle (and any backend auxiliaries it uniquely owns). It runs
 *    under mu on the drift paths and under ioMu on the send/close paths.
 *  - insideMu(): runs under mu during WsUplinkClose for backends that
 *    keep auxiliary handles alongside the slot (win32 Connection). */
struct WsUplink {
    std::mutex mu;      /* state/url/socket-slot bookkeeping */
    std::mutex ioMu;    /* serializes send/close/re-dial on the socket */
    std::atomic<int> state{ PORT_WS_CLOSED };
    std::string url;             /* remembered dial target — under mu */
    uint64_t retryAfterMs = 0;   /* next allowed re-dial tick — under mu */
};

/* Re-dial backoff for the lazy uplink reconnect. */
constexpr uint64_t kUpRedialMs = 2000;

/* Monotonic ms tick for the backoff gate (steady_clock backends; the
 * win32 backend passes GetTickCount64 instead). */
inline uint64_t WsSteadyMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

/* Return the live uplink socket, lazily re-dialing `up.url` at a bounded
 * cadence when the previous link FAILED. Fast path under mu; the dial
 * itself runs serialized on ioMu with a second mu check for a racing
 * winner and a URL-drift check after it returns. `bad` whenever the
 * uplink is unusable right now. dial(url)->H and kill(handle) are the
 * backend seams. */
template <typename H, typename Dial, typename Kill>
H WsUplinkSocket(WsUplink& up, uint64_t nowMs, H bad, H& socketSlot,
                 Dial&& dial, Kill&& kill)
{
    {
        std::lock_guard<std::mutex> lock(up.mu);
        if (up.state == PORT_WS_OPEN && socketSlot != bad) {
            return socketSlot;
        }
        if (up.state != PORT_WS_FAILED || up.url.empty() ||
            nowMs < up.retryAfterMs) {
            return bad;
        }
        up.retryAfterMs = nowMs + kUpRedialMs;
    }
    std::lock_guard<std::mutex> io(up.ioMu);
    {
        std::lock_guard<std::mutex> lock(up.mu);
        if (up.state == PORT_WS_OPEN && socketSlot != bad) {
            return socketSlot;   /* another sender re-dialed while we waited */
        }
    }
    std::string url;
    {
        std::lock_guard<std::mutex> lock(up.mu);
        url = up.url;
    }
    H s = dial(url);
    std::lock_guard<std::mutex> lock(up.mu);
    if (s == bad) {
        up.state = PORT_WS_FAILED;
        return bad;
    }
    if (up.url != url) {    /* closed or re-targeted while we dialed */
        if (up.url.empty()) {
            up.state = PORT_WS_CLOSED;
        }
        kill(s);
        return bad;
    }
    if (socketSlot != bad) {
        /* A racing blocking connect (same URL — passes the drift check)
         * already installed a socket; ours is the stale duplicate. */
        kill(s);
        return bad;
    }
    socketSlot = s;
    up.state = PORT_WS_OPEN;
    port_log("SSB64 WS: uplink reconnected\n");
    return s;
}

/* Send on a live uplink socket `s` (from WsUplinkSocket). Returns 1 when
 * the frame went out, -1 when the link is dead or superseded — the
 * caller falls back to the control socket rather than dropping the
 * frame. send(handle)->bool and kill(handle) are the backend seams;
 * kill runs after the FAILED latch, still inside ioMu. */
template <typename H, typename Send, typename Kill>
int WsUplinkSend(WsUplink& up, H& socketSlot, H s, H bad,
                 Send&& send, Kill&& kill)
{
    if (s == bad) {
        return -1;
    }
    std::lock_guard<std::mutex> io(up.ioMu);
    {
        std::lock_guard<std::mutex> lock(up.mu);
        if (socketSlot != s) {
            return -1;   /* closed or swapped while we waited on the lock */
        }
    }
    if (send(s)) {
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(up.mu);
        if (socketSlot != s) {
            /* A close or a racing re-dial already swapped the slot — the
             * party that swapped it owns this handle's teardown. Kill
             * only handles we personally retired, never one we don't
             * own (double WinHttpCloseHandle / double close() otherwise). */
            return -1;
        }
        socketSlot = bad;
        up.state = PORT_WS_FAILED;
    }
    kill(s);
    return -1;
}

/* Blocking uplink dial for port_ws_uplink_connect (call after
 * port_ws_uplink_close). Publishes URL + CONNECTING, dials, and either
 * installs the socket or records FAILED. Two guards run under mu after
 * the dial: URL drift (a close or re-target kills the fresh socket) and
 * a slot-occupancy check (a racing install wins; our socket is retired).
 * dial(url,&err)->H and kill(handle) are the backend seams. */
template <typename H, typename Dial, typename Kill>
int WsUplinkConnect(WsUplink& up, H& socketSlot, H bad, const char* url,
                    Dial&& dial, Kill&& kill)
{
    std::string urlStr = url != nullptr ? url : "";
    {
        std::lock_guard<std::mutex> lock(up.mu);
        up.url = urlStr;
        up.retryAfterMs = 0;
    }
    up.state = PORT_WS_CONNECTING;
    std::string err;
    H s = dial(urlStr, err);
    std::lock_guard<std::mutex> lock(up.mu);
    if (s == bad) {
        port_log("SSB64 WS: uplink failed: %s\n", err.c_str());
        up.state = PORT_WS_FAILED;
        return 0;
    }
    if (up.url != urlStr) {    /* closed or re-targeted while we dialed */
        if (up.url.empty()) {
            up.state = PORT_WS_CLOSED;
        }
        kill(s);
        return 0;
    }
    if (socketSlot != bad) {
        /* A racing connect installed a socket while we dialed — ours is
         * stale; the slot owner keeps theirs and we retire this handle. */
        kill(s);
        return 0;
    }
    socketSlot = s;
    up.state = PORT_WS_OPEN;
    return 1;
}

/* Explicit close: clear the remembered URL so the lazy path never
 * resurrects, swap the socket slot to `bad`, then graceful-close under
 * ioMu (waiting out an in-flight send). insideMu() runs under mu for
 * backends with extra handles to tear down alongside the slot; kill(s)
 * does the graceful close + handle free under ioMu. */
template <typename H, typename InsideMu, typename Kill>
void WsUplinkClose(WsUplink& up, H& socketSlot, H bad,
                   InsideMu&& insideMu, Kill&& kill)
{
    H s;
    {
        std::lock_guard<std::mutex> lock(up.mu);
        up.url.clear();
        s = socketSlot;
        socketSlot = bad;
        insideMu();
        up.state = PORT_WS_CLOSED;
    }
    if (s != bad) {
        std::lock_guard<std::mutex> io(up.ioMu);
        kill(s);
    }
}

/* Control-path send shared by all backends: try the uplink first (so
 * bulk gameplay frames don't queue behind control reads), then fall
 * back to the control socket. uplink(data,size,opcode)->int follows the
 * WsUplinkSend contract (1 sent / -1 unusable); control(data,size,opcode)
 * writes on the control socket and returns its result. */
template <typename Uplink, typename Control>
int WsSend(std::atomic<int>& ctrlState,
           const void* data, int size, unsigned char opcode,
           Uplink&& uplink, Control&& control)
{
    if (uplink(data, size, opcode) > 0) {
        return 1;
    }
    if (ctrlState != PORT_WS_OPEN) {
        return 0;
    }
    return control(data, size, opcode);
}
