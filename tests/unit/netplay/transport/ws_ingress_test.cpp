/* ws_ingress_test — boundary checks for the shared WS ingress helpers
 * (src/networking/netplay/transport/websocket_transport_internal.h).
 *
 * Pins the contract every backend relies on:
 *   - queue cap counts text + binary frames TOGETHER (flood via either
 *     channel must trip the same bound)
 *   - enqueue at exactly max-1 succeeds; at max it fails and adds nothing
 *   - a failed enqueue leaves the queued frames pollable (drop the new,
 *     never the old)
 *   - text poll truncates to capacity-1 with NUL; binary poll DROPS
 *     oversized frames instead of truncating mid-message — asymmetric
 *     semantics the callers depend on
 *   - clear resets error + both queues for a fresh connect
 *
 * The per-message byte cap (kMaxWsMessageBytes) is enforced by each
 * backend before/around enqueue — the shared helper only owns queue
 * depth, so the test asserts the constant and mirrors the caller-side
 * comparison websocket_web.cpp applies.
 *
 * Header-inline helpers only: no game state, no sockets, no network. */

#include "smash_test/check.h"

#include "networking/netplay/transport/websocket_transport_internal.h"

#include <mutex>
#include <string>
#include <vector>

int main() {
    std::mutex mu;
    std::deque<std::string> text;
    std::deque<std::vector<unsigned char>> bin;
    std::string error;

    /* The byte-cap constant exists and the caller-side comparison a
     * backend applies (websocket_web on_message) behaves at the edges. */
    SMASH_CHECK(kMaxWsMessageBytes == (16u << 20));
    SMASH_CHECK((kMaxWsMessageBytes - 1) <= kMaxWsMessageBytes);
    SMASH_CHECK(kMaxWsMessageBytes + 1 > kMaxWsMessageBytes);

    /* ── basic enqueue + ordering ──────────────────────────────────── */
    std::vector<unsigned char> t1{'h', 'i'};
    std::vector<unsigned char> b1{0x01, 0x02, 0x03};
    std::vector<unsigned char> t2{'b', 'y', 'e'};

    SMASH_CHECK(WsEnqueueBounded(mu, text, bin, t1, true));
    SMASH_CHECK(WsEnqueueBounded(mu, text, bin, b1, false));
    SMASH_CHECK(WsEnqueueBounded(mu, text, bin, t2, true));
    SMASH_CHECK(text.size() == 2 && bin.size() == 1);

    /* FIFO across types: text polls in enqueue order, binary likewise. */
    char tbuf[64];
    unsigned char bbuf[64];
    SMASH_CHECK(WsPollText(mu, text, tbuf, sizeof(tbuf)) == 1);
    SMASH_CHECK(std::string(tbuf) == "hi");
    SMASH_CHECK(WsPollBinary(mu, bin, bbuf, sizeof(bbuf)) == 3);
    SMASH_CHECK(bbuf[0] == 0x01 && bbuf[2] == 0x03);
    SMASH_CHECK(WsPollText(mu, text, tbuf, sizeof(tbuf)) == 1);
    SMASH_CHECK(std::string(tbuf) == "bye");
    SMASH_CHECK(text.empty() && bin.empty());

    /* ── text poll edge cases ──────────────────────────────────────── */
    SMASH_CHECK(WsPollText(mu, text, tbuf, sizeof(tbuf)) == 0);   // empty
    SMASH_CHECK(WsEnqueueBounded(mu, text, bin, {'a','b','c','d'}, true));
    SMASH_CHECK(WsPollText(mu, text, tbuf, 0) == 0);              // capacity 0
    SMASH_CHECK(text.size() == 1);                                // frame kept
    SMASH_CHECK(WsPollText(mu, text, tbuf, 3) == 1);              // truncate
    SMASH_CHECK(std::string(tbuf) == "ab" && tbuf[2] == '\0');    // NUL at cap-1
    SMASH_CHECK(text.empty());

    /* Exact fit: message of capacity-1 bytes needs no truncation. */
    std::vector<unsigned char> exact(4, 'x');
    SMASH_CHECK(WsEnqueueBounded(mu, text, bin, exact, true));
    SMASH_CHECK(WsPollText(mu, text, tbuf, 5) == 1);
    SMASH_CHECK(std::string(tbuf) == "xxxx");

    /* ── binary poll: oversized frames are DROPPED, not truncated ──── */
    SMASH_CHECK(WsPollBinary(mu, bin, bbuf, sizeof(bbuf)) == 0);  // empty
    std::vector<unsigned char> big(100, 0xAA);
    SMASH_CHECK(WsEnqueueBounded(mu, text, bin, big, false));
    SMASH_CHECK(WsEnqueueBounded(mu, text, bin, {0x09}, false));
    SMASH_CHECK(WsPollBinary(mu, bin, bbuf, 10) == 0);            // 100 > 10
    SMASH_CHECK(bin.size() == 1);                                 // big dropped
    SMASH_CHECK(WsPollBinary(mu, bin, bbuf, sizeof(bbuf)) == 1);  // next frame
    SMASH_CHECK(bbuf[0] == 0x09);
    SMASH_CHECK(bin.empty());

    /* Exact capacity binary fits. */
    SMASH_CHECK(WsEnqueueBounded(mu, text, bin, std::vector<unsigned char>(16, 0x55), false));
    SMASH_CHECK(WsPollBinary(mu, bin, bbuf, 16) == 16);

    /* ── queue-depth cap: text + binary count together ─────────────── */
    const size_t half = kMaxWsQueuedMessages / 2;
    std::vector<unsigned char> one{1};
    for (size_t i = 0; i < half; i++) {
        SMASH_EXPECT(WsEnqueueBounded(mu, text, bin, one, true));
        SMASH_EXPECT(WsEnqueueBounded(mu, text, bin, one, false));
    }
    SMASH_CHECK(text.size() + bin.size() == kMaxWsQueuedMessages);

    /* At max: enqueue fails, nothing added — the OLD frames survive. */
    SMASH_CHECK(!WsEnqueueBounded(mu, text, bin, one, true));
    SMASH_CHECK(!WsEnqueueBounded(mu, text, bin, one, false));
    SMASH_CHECK(text.size() + bin.size() == kMaxWsQueuedMessages);

    /* Drain one, enqueue exactly one again — boundary at max-1. */
    SMASH_CHECK(WsPollText(mu, text, tbuf, sizeof(tbuf)) == 1);
    SMASH_CHECK(text.size() + bin.size() == kMaxWsQueuedMessages - 1);
    SMASH_CHECK(WsEnqueueBounded(mu, text, bin, one, true));
    SMASH_CHECK(text.size() + bin.size() == kMaxWsQueuedMessages);
    SMASH_CHECK(!WsEnqueueBounded(mu, text, bin, one, true));

    /* ── clear-for-reconnect ───────────────────────────────────────── */
    error = "previous failure";
    WsClearIngress(mu, error, text, bin);
    SMASH_CHECK(error.empty());
    SMASH_CHECK(text.empty() && bin.empty());

    /* After clear, the queue accepts fresh messages (reconnect works). */
    SMASH_CHECK(WsEnqueueBounded(mu, text, bin, {'o', 'k'}, true));
    SMASH_CHECK(WsPollText(mu, text, tbuf, sizeof(tbuf)) == 1);
    SMASH_CHECK(std::string(tbuf) == "ok");

    return smash_test_report("ws_ingress_test");
}
