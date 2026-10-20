/* ws_web_test.cpp — Emscripten WebSocket backend tested on the host.
 *
 * websocket_web.cpp compiles here under a forced __EMSCRIPTEN__ with the
 * emsdk API surface replaced by the shim in tests/support/
 * emscripten_shim/ backed by the in-process transport in
 * tests/support/fake_ems.cpp. The browser's async event delivery is
 * replayed deterministically via fake_ems::Fire*; the clock is
 * test-driven so the shared kUpRedialMs redial cadence is exercised
 * without sleeping.
 *
 * What this pins that the desktop backends' ws_uplink_test cannot:
 *  - binary sends route to the uplink socket once open (control socket
 *    stays idle for relay traffic)
 *  - a dead uplink re-dials the remembered URL lazily and the frame
 *    falls back to the control socket meanwhile (never dropped)
 *  - the redial cadence gates on the fake clock
 *  - explicit close clears the URL — no resurrection
 *  - on_close terminal semantics: FAILED stays FAILED across a clean
 *    close frame; a clean close from OPEN reports CLOSED
 *  - queue/message overflow fails the connection like the threaded
 *    backends
 *  - resource balance: every emscripten_websocket_new is matched by an
 *    emscripten_websocket_delete across the churn windows
 */

#include "smash_test/check.h"
#include "fake_ems.h"

#include <cstdint>
#include <cstring>
#include <string>

/* The backend's public ABI — provided by websocket_web.cpp compiled
 * into this test. */
extern "C" {
int port_ws_connect(const char *url);
void port_ws_close(void);
int port_ws_state(void);
const char *port_ws_error(void);
int port_ws_send_text(const char *text);
int port_ws_send_binary(const void *data, int size);
int port_ws_poll_text(char *out, int capacity);
int port_ws_poll_binary(void *out, int capacity);
int port_ws_uplink_connect(const char *url);
void port_ws_uplink_close(void);
int port_ws_uplink_state(void);
int port_ws_uplink_send(const void *data, int size);
}

enum {
    WS_CLOSED = 0,
    WS_CONNECTING = 1,
    WS_OPEN = 2,
    WS_FAILED = 3,
};

static void tear_down_all(void)
{
    port_ws_uplink_close();
    port_ws_close();
    fake_ems::Reset();
}

/* T1: connect handshake — new socket starts CONNECTING, on_open flips
 * to OPEN; text sends route to the control socket. */
static void test_connect_open_send_text(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://lobby.example/ws") == 1);
    const int ctl = fake_ems::LastSocket();
    SMASH_CHECK(ctl > 0);
    SMASH_CHECK(port_ws_state() == WS_CONNECTING);
    SMASH_CHECK(fake_ems::UrlOf(ctl) == "ws://lobby.example/ws");
    fake_ems::FireOpen(ctl);
    SMASH_CHECK(port_ws_state() == WS_OPEN);
    SMASH_CHECK(port_ws_send_text("ping") == 1);
    SMASH_CHECK(fake_ems::SentTextBytes(ctl) == 4u);
    tear_down_all();
}

/* T2: connect failure — error before open marks FAILED; a later
 * unclean close keeps FAILED and records an error string. */
static void test_connect_failure_terminal(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://lobby.example/ws") == 1);
    const int ctl = fake_ems::LastSocket();
    fake_ems::FireError(ctl);
    SMASH_CHECK(port_ws_state() == WS_FAILED);
    SMASH_CHECK(std::strlen(port_ws_error()) > 0u);
    fake_ems::FireClose(ctl, 1006, /*wasClean=*/false);
    SMASH_CHECK(port_ws_state() == WS_FAILED);
    tear_down_all();
}

/* T3: clean close from OPEN reports CLOSED; explicit close is
 * idempotent and deletes the socket exactly once. */
static void test_close_semantics(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://a/") == 1);
    const int ctl = fake_ems::LastSocket();
    fake_ems::FireOpen(ctl);
    fake_ems::FireClose(ctl, 1000, /*wasClean=*/true);
    SMASH_CHECK(port_ws_state() == WS_CLOSED);
    const int dels = fake_ems::DeleteCount();
    port_ws_close();
    port_ws_close();
    SMASH_CHECK(fake_ems::DeleteCount() == dels + 1);
    SMASH_CHECK(port_ws_state() == WS_CLOSED);
    tear_down_all();
}

/* T4: binary sends prefer the uplink once it is open. */
static void test_binary_prefers_uplink(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://a/") == 1);
    const int ctl = fake_ems::LastSocket();
    fake_ems::FireOpen(ctl);
    SMASH_EXPECT(port_ws_uplink_connect("ws://a/up") == 1);
    const int up = fake_ems::LastSocket();
    SMASH_CHECK(up != ctl);
    SMASH_CHECK(port_ws_uplink_state() == WS_CONNECTING);
    fake_ems::FireOpen(up);
    SMASH_CHECK(port_ws_uplink_state() == WS_OPEN);

    const uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    SMASH_CHECK(port_ws_send_binary(frame, sizeof(frame)) == 1);
    SMASH_CHECK(fake_ems::SentBinaryBytes(up) == 8u);
    SMASH_CHECK(fake_ems::SentBinaryBytes(ctl) == 0u);
    SMASH_CHECK(port_ws_uplink_send(frame, sizeof(frame)) == 1);
    SMASH_CHECK(fake_ems::SentBinaryBytes(up) == 16u);
    tear_down_all();
}

/* T5: dead uplink — the send that observes the failure re-dials on the
 * SAME call (web is eager: sends are the only event pump), so the
 * observable uplink state is CONNECTING, the dead handle is deleted,
 * a fresh socket carries the remembered URL, and the frame still goes
 * out via the control socket. */
static void test_uplink_dead_lazy_redial(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://a/") == 1);
    const int ctl = fake_ems::LastSocket();
    fake_ems::FireOpen(ctl);
    SMASH_EXPECT(port_ws_uplink_connect("ws://a/up") == 1);
    const int up = fake_ems::LastSocket();
    fake_ems::FireOpen(up);

    fake_ems::SetSendFails(up, true);
    const uint8_t frame[4] = {9, 9, 9, 9};
    const int news = fake_ems::NewCount();
    const int dels = fake_ems::DeleteCount();
    SMASH_CHECK(port_ws_send_binary(frame, sizeof(frame)) == 1); /* control */
    SMASH_CHECK(port_ws_uplink_state() == WS_CONNECTING);        /* re-dialed */
    SMASH_CHECK(fake_ems::NewCount() == news + 1);
    SMASH_CHECK(fake_ems::DeleteCount() == dels + 1);            /* dead handle disposed */
    SMASH_CHECK(fake_ems::SentBinaryBytes(ctl) == 4u);
    SMASH_CHECK(fake_ems::SentBinaryBytes(up) == 0u);
    const int up2 = fake_ems::LastSocket();
    SMASH_CHECK(up2 != up);
    SMASH_CHECK(fake_ems::UrlOf(up2) == "ws://a/up");

    /* While CONNECTING the uplink is unusable — frames keep falling
     * back to control with no additional dials. */
    SMASH_CHECK(port_ws_send_binary(frame, sizeof(frame)) == 1);
    SMASH_CHECK(fake_ems::NewCount() == news + 1);
    SMASH_CHECK(fake_ems::SentBinaryBytes(ctl) == 8u);

    /* New uplink opens — traffic migrates back to it. */
    fake_ems::FireOpen(up2);
    SMASH_CHECK(port_ws_send_binary(frame, sizeof(frame)) == 1);
    SMASH_CHECK(fake_ems::SentBinaryBytes(up2) == 4u);
    SMASH_CHECK(fake_ems::SentBinaryBytes(ctl) == 8u);
    tear_down_all();
}

/* T6: redial cadence — the redial stamps retryAt = now + kUpRedialMs;
 * a SECOND failure inside that window is gated (no dial) until the
 * fake clock passes it. */
static void test_uplink_redial_cadence(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://a/") == 1);
    const int ctl = fake_ems::LastSocket();
    fake_ems::FireOpen(ctl);
    SMASH_EXPECT(port_ws_uplink_connect("ws://a/up") == 1);
    const int up = fake_ems::LastSocket();
    fake_ems::FireOpen(up);

    /* First death re-dials on the same call (retry clock was reset by
     * uplink_connect, so the gate passes immediately). */
    fake_ems::SetSendFails(up, true);
    const uint8_t frame[4] = {1, 0, 0, 1};
    int news = fake_ems::NewCount();
    port_ws_send_binary(frame, sizeof(frame));
    SMASH_CHECK(fake_ems::NewCount() == news + 1);
    const int up2 = fake_ems::LastSocket();
    SMASH_CHECK(port_ws_uplink_state() == WS_CONNECTING);

    /* Kill the freshly-dialed uplink before it opens (error → FAILED).
     * Inside the cadence window: no further dial. */
    fake_ems::FireError(up2);
    SMASH_CHECK(port_ws_uplink_state() == WS_FAILED);
    news = fake_ems::NewCount();
    port_ws_send_binary(frame, sizeof(frame));
    port_ws_uplink_send(frame, sizeof(frame));
    SMASH_CHECK(fake_ems::NewCount() == news);   /* gated — no redial yet */

    fake_ems::AdvanceNow(2100.0);                /* past kUpRedialMs */
    port_ws_send_binary(frame, sizeof(frame));
    SMASH_CHECK(fake_ems::NewCount() == news + 1); /* redial fired */
    tear_down_all();
}

/* T7: explicit uplink close clears the URL — a later send can fall
 * back to control but must never resurrect the uplink. */
static void test_uplink_close_no_resurrect(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://a/") == 1);
    const int ctl = fake_ems::LastSocket();
    fake_ems::FireOpen(ctl);
    SMASH_EXPECT(port_ws_uplink_connect("ws://a/up") == 1);
    fake_ems::FireOpen(fake_ems::LastSocket());
    port_ws_uplink_close();
    SMASH_CHECK(port_ws_uplink_state() == WS_CLOSED);

    const int news = fake_ems::NewCount();
    const uint8_t frame[4] = {7, 7, 7, 7};
    SMASH_CHECK(port_ws_send_binary(frame, sizeof(frame)) == 1);
    SMASH_CHECK(fake_ems::NewCount() == news);
    SMASH_CHECK(fake_ems::SentBinaryBytes(ctl) == 4u);
    fake_ems::AdvanceNow(10000.0);
    SMASH_CHECK(port_ws_uplink_send(frame, sizeof(frame)) == 0);
    SMASH_CHECK(fake_ems::NewCount() == news);
    tear_down_all();
}

/* T8: ingress — text and binary messages land in the poll queues. */
static void test_ingress_poll(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://a/") == 1);
    const int ctl = fake_ems::LastSocket();
    fake_ems::FireOpen(ctl);
    fake_ems::FireMessageText(ctl, "hello");
    const uint8_t bin[3] = {0xaa, 0xbb, 0xcc};
    fake_ems::FireMessageBinary(ctl, bin, sizeof(bin));

    char text[64];
    SMASH_CHECK(port_ws_poll_text(text, sizeof(text)) == 5);
    SMASH_CHECK(std::strcmp(text, "hello") == 0);
    uint8_t out[8];
    SMASH_CHECK(port_ws_poll_binary(out, sizeof(out)) == 3);
    SMASH_CHECK(out[0] == 0xaa && out[2] == 0xcc);
    SMASH_CHECK(port_ws_poll_text(text, sizeof(text)) == 0);
    tear_down_all();
}

/* T9: oversized message — numBytes above the cap fails the connection
 * and asks the peer to close (the fake records the close). */
static void test_ingress_overflow_fails(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://a/") == 1);
    const int ctl = fake_ems::LastSocket();
    fake_ems::FireOpen(ctl);
    const uint8_t tiny[4] = {0};
    /* 17 MiB claimed on a 4-byte buffer — the bound check runs before
     * any read of ev.data, so the inflated size is safe. */
    const int closes = fake_ems::CloseCount();
    fake_ems::FireMessageBinary(ctl, tiny, (17u << 20));
    SMASH_CHECK(port_ws_state() == WS_FAILED);
    SMASH_CHECK(fake_ems::CloseCount() == closes + 1);
    SMASH_CHECK(std::strlen(port_ws_error()) > 0u);
    tear_down_all();
}

/* T10: reconnect — a second connect disposes the old control socket
 * exactly once (delete balance, no leak of browser handles). */
static void test_reconnect_disposes_old(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://a/") == 1);
    const int first = fake_ems::LastSocket();
    fake_ems::FireOpen(first);
    const int dels = fake_ems::DeleteCount();
    SMASH_EXPECT(port_ws_connect("ws://b/") == 1);
    const int second = fake_ems::LastSocket();
    SMASH_CHECK(second != first);
    SMASH_CHECK(fake_ems::UrlOf(second) == "ws://b/");
    SMASH_CHECK(fake_ems::DeleteCount() == dels + 1);
    /* Events on the dead socket are inert for live state. */
    fake_ems::FireOpen(first);
    SMASH_CHECK(port_ws_state() == WS_CONNECTING);
    fake_ems::FireOpen(second);
    SMASH_CHECK(port_ws_state() == WS_OPEN);
    tear_down_all();
}

/* T11: unsupported browser — connect refuses cleanly. */
static void test_unsupported_browser(void)
{
    tear_down_all();
    fake_ems::SetSupported(false);
    SMASH_CHECK(port_ws_connect("ws://a/") == 0);
    SMASH_CHECK(port_ws_state() == WS_FAILED);
    SMASH_CHECK(fake_ems::NewCount() == 0);
    tear_down_all();
}

/* T12: resource balance — across connect/uplink/churn/teardown every
 * new is matched by a delete. The uplink redial deletes its dead
 * handle before re-dialing. */
static void test_resource_balance(void)
{
    tear_down_all();
    SMASH_EXPECT(port_ws_connect("ws://a/") == 1);
    const int ctl = fake_ems::LastSocket();
    fake_ems::FireOpen(ctl);
    SMASH_EXPECT(port_ws_uplink_connect("ws://a/up") == 1);
    const int up = fake_ems::LastSocket();
    fake_ems::FireOpen(up);
    fake_ems::SetSendFails(up, true);
    const uint8_t frame[4] = {3, 3, 3, 3};
    port_ws_send_binary(frame, sizeof(frame));   /* kills uplink state */
    port_ws_send_binary(frame, sizeof(frame));   /* redial: delete+new */
    port_ws_uplink_close();
    port_ws_close();
    SMASH_CHECK(fake_ems::NewCount() == fake_ems::DeleteCount());
    SMASH_CHECK(fake_ems::NewCount() > 0);
    tear_down_all();
}

int main(void)
{
    test_connect_open_send_text();
    test_connect_failure_terminal();
    test_close_semantics();
    test_binary_prefers_uplink();
    test_uplink_dead_lazy_redial();
    test_uplink_redial_cadence();
    test_uplink_close_no_resurrect();
    test_ingress_poll();
    test_ingress_overflow_fails();
    test_reconnect_disposes_old();
    test_unsupported_browser();
    test_resource_balance();
    return smash_test_report("ws_web_test");
}
