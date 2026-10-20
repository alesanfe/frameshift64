/* ws_uplink_test — controlled-interleaving tests for the shared lazy-
 * reconnect uplink choreography
 * (src/networking/netplay/transport/websocket_transport_internal.h).
 *
 * The uplink helpers are templates over a backend-supplied handle type,
 * so this suite instantiates them with a fake `int` socket and
 * instrumented dial/send/kill lambdas — no sockets, no network, no game
 * state. Real std::thread + hand-rolled gates force the interleavings a
 * race could produce:
 *
 *   - happy path: blocking connect installs the socket, OPEN
 *   - connect failure records FAILED, installs nothing
 *   - send failure latches FAILED, clears the slot, kills once
 *   - lazy re-dial honors the retryAfter cadence gate
 *   - close while a lazy dial is blocked kills the fresh socket, never
 *     installs it (URL-drift path)
 *   - retarget A->B mid-dial also kills the fresh socket
 *   - two sends serialize on ioMu (in-flight count never exceeds 1)
 *   - close waits out an in-flight send, then kills (send-before-kill)
 *   - send-fail racing a close kills the socket exactly once
 *   - repeated close is idempotent
 *   - close during a blocking connect kills the fresh socket
 *   - WsSend triage: uplink first, control fallback, closed = 0
 *
 * Regression trap for the ioMu -> mu lock order (see the contract in the
 * header): the send-in-flight + close scenarios hold ioMu and mu on
 * opposite threads; a mu -> ioMu inversion would deadlock and the CTest
 * TIMEOUT reports it.
 *
 * Fake backend keeps dial/send/kill counters and an event log so every
 * scenario can assert exact resource balance (installs == kills + live). */

#include "smash_test/check.h"

#include "networking/netplay/transport/websocket_transport_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* The internal header's helpers call port_log on reconnect/fail paths —
 * the test binary doesn't link observability, so satisfy the extern "C"
 * symbol with a no-op. */
extern "C" void port_log(const char*, ...) {}

namespace {

constexpr int BAD = -1;

/* Single-shot rendezvous: a lambda calls wait_inside() (optionally
 * holding uplink locks — that is the point), the test thread calls
 * await_entered() then release(). */
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool entered = false;
    bool open = false;

    void wait_inside()
    {
        std::unique_lock<std::mutex> l(m);
        entered = true;
        cv.notify_all();
        cv.wait(l, [this] { return open; });
    }
    void await_entered()
    {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [this] { return entered; });
    }
    void release()
    {
        {
            std::lock_guard<std::mutex> l(m);
            open = true;
        }
        cv.notify_all();
    }
};

struct FakeLink {
    std::atomic<int> dials{0};
    std::atomic<int> sends{0};
    std::atomic<int> kills{0};
    std::atomic<int> nextHandle{100};
    std::atomic<int> inFlightSends{0};
    std::atomic<int> maxInFlight{0};
    bool dialSucceeds = true;
    bool sendSucceeds = true;

    std::mutex evMu;
    std::vector<std::string> events;
    void ev(const char* tag)
    {
        std::lock_guard<std::mutex> l(evMu);
        events.emplace_back(tag);
    }
    size_t evIndex(const char* tag)
    {
        std::lock_guard<std::mutex> l(evMu);
        auto it = std::find(events.begin(), events.end(), tag);
        return it == events.end() ? events.size()
                                  : static_cast<size_t>(it - events.begin());
    }
};

/* Force `up` into the lazy-reconnect entry state the same way a backend
 * send-failure leaves it. */
void latch_failed(WsUplink& up, const char* url)
{
    std::lock_guard<std::mutex> l(up.mu);
    up.url = url;
    up.retryAfterMs = 0;
    up.state = PORT_WS_FAILED;
}

} // namespace

int main()
{
    /* ── T1: blocking connect installs + opens ─────────────────────── */
    {
        WsUplink up;
        int slot = BAD;
        FakeLink f;
        int rc = WsUplinkConnect(up, slot, BAD, "ws://a",
            [&](const std::string& u, std::string&) {
                SMASH_CHECK(u == "ws://a");
                f.dials++;
                return f.nextHandle++;
            },
            [&](int) { f.kills++; });
        SMASH_CHECK(rc == 1);
        SMASH_CHECK(up.state == PORT_WS_OPEN);
        SMASH_CHECK(slot == 100);
        SMASH_CHECK(f.dials == 1 && f.kills == 0);

        /* Lazy getter returns the installed socket without re-dialing. */
        int s = WsUplinkSocket(up, WsSteadyMs(), BAD, slot,
            [&](const std::string&) { f.dials++; return f.nextHandle++; },
            [&](int) { f.kills++; });
        SMASH_CHECK(s == 100 && f.dials == 1);
    }

    /* ── T2: connect failure -> FAILED, nothing installed ──────────── */
    {
        WsUplink up;
        int slot = BAD;
        FakeLink f;
        int rc = WsUplinkConnect(up, slot, BAD, "ws://a",
            [&](const std::string&, std::string& err) {
                f.dials++;
                err = "refused";
                return BAD;
            },
            [&](int) { f.kills++; });
        SMASH_CHECK(rc == 0);
        SMASH_CHECK(up.state == PORT_WS_FAILED);
        SMASH_CHECK(slot == BAD);
        SMASH_CHECK(f.dials == 1 && f.kills == 0);
    }

    /* ── T3: send failure latches FAILED + kills once ──────────────── */
    {
        WsUplink up;
        int slot = 100;
        FakeLink f;
        up.state = PORT_WS_OPEN;
        f.sendSucceeds = false;
        int rc = WsUplinkSend(up, slot, /*s=*/100, BAD,
            [&](int) { f.sends++; return false; },
            [&](int h) { f.kills++; SMASH_CHECK(h == 100); });
        SMASH_CHECK(rc == -1);
        SMASH_CHECK(up.state == PORT_WS_FAILED);
        SMASH_CHECK(slot == BAD);
        SMASH_CHECK(f.sends == 1 && f.kills == 1);
    }

    /* ── T4: cadence gate — no re-dial before retryAfter ───────────── */
    {
        WsUplink up;
        int slot = BAD;
        FakeLink f;
        {
            std::lock_guard<std::mutex> l(up.mu);
            up.url = "ws://a";
            up.retryAfterMs = 5000;
            up.state = PORT_WS_FAILED;
        }
        int s = WsUplinkSocket(up, /*nowMs=*/4999, BAD, slot,
            [&](const std::string&) { f.dials++; return f.nextHandle++; },
            [&](int) { f.kills++; });
        SMASH_CHECK(s == BAD && f.dials == 0);

        /* …and no dial while CONNECTING/OPEN regardless of cadence. */
        up.state = PORT_WS_CONNECTING;
        s = WsUplinkSocket(up, 6000, BAD, slot,
            [&](const std::string&) { f.dials++; return f.nextHandle++; },
            [&](int) { f.kills++; });
        SMASH_CHECK(s == BAD && f.dials == 0);
    }

    /* ── T5: lazy re-dial installs after cadence expires ───────────── */
    {
        WsUplink up;
        int slot = BAD;
        FakeLink f;
        latch_failed(up, "ws://a");
        int s = WsUplinkSocket(up, WsSteadyMs(), BAD, slot,
            [&](const std::string& u) {
                SMASH_CHECK(u == "ws://a");
                f.dials++;
                return f.nextHandle++;
            },
            [&](int) { f.kills++; });
        SMASH_CHECK(s == 100);
        SMASH_CHECK(slot == 100);
        SMASH_CHECK(up.state == PORT_WS_OPEN);
        SMASH_CHECK(f.dials == 1);

        /* The re-dial armed the next cadence window under mu. */
        std::lock_guard<std::mutex> l(up.mu);
        SMASH_CHECK(up.retryAfterMs > 0);
    }

    /* ── T6: close while lazy dial blocked -> kill, never install ──── */
    {
        WsUplink up;
        int slot = BAD;
        FakeLink f;
        Gate dialGate;
        latch_failed(up, "ws://a");

        std::thread t([&] {
            int s = WsUplinkSocket(up, WsSteadyMs(), BAD, slot,
                [&](const std::string&) {
                    f.dials++;
                    dialGate.wait_inside();   /* blocked holding ioMu */
                    return f.nextHandle++;
                },
                [&](int) { f.ev("kill"); f.kills++; });
            SMASH_CHECK(s == BAD);
        });
        dialGate.await_entered();

        WsUplinkClose(up, slot, BAD,
            [] {},
            [&](int) { f.kills++; });
        SMASH_CHECK(up.state == PORT_WS_CLOSED);
        dialGate.release();
        t.join();

        /* Fresh socket was killed on the drift path, slot stayed bad. */
        SMASH_CHECK(f.dials == 1);
        SMASH_CHECK(f.kills == 1);
        SMASH_CHECK(slot == BAD);
        SMASH_CHECK(f.evIndex("kill") == 0);
    }

    /* ── T7: retarget A->B mid-dial kills A's fresh socket ─────────── */
    {
        WsUplink up;
        int slot = BAD;
        FakeLink f;
        Gate dialGate;
        latch_failed(up, "ws://a");

        std::thread t([&] {
            int s = WsUplinkSocket(up, WsSteadyMs(), BAD, slot,
                [&](const std::string& u) {
                    f.dials++;
                    SMASH_CHECK(u == "ws://a");
                    dialGate.wait_inside();
                    return f.nextHandle++;
                },
                [&](int) { f.kills++; });
            SMASH_CHECK(s == BAD);
        });
        dialGate.await_entered();
        {
            std::lock_guard<std::mutex> l(up.mu);
            up.url = "ws://b";
        }
        dialGate.release();
        t.join();

        /* A's socket killed; URL stays B; state is not flipped to
         * CLOSED (a live request for B is in flight). */
        SMASH_CHECK(f.kills == 1);
        SMASH_CHECK(slot == BAD);
        {
            std::lock_guard<std::mutex> l(up.mu);
            SMASH_CHECK(up.url == "ws://b");
        }
        SMASH_CHECK(up.state == PORT_WS_FAILED);
    }

    /* ── T8: concurrent sends serialize on ioMu ────────────────────── */
    {
        WsUplink up;
        int slot = 100;
        FakeLink f;
        up.state = PORT_WS_OPEN;
        std::atomic<int> done{0};

        auto send_body = [&] {
            WsUplinkSend(up, slot, /*s=*/100, BAD,
                [&](int) {
                    f.sends++;
                    int n = ++f.inFlightSends;
                    int prev = f.maxInFlight.load();
                    while (n > prev &&
                           !f.maxInFlight.compare_exchange_weak(prev, n)) {}
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));
                    --f.inFlightSends;
                    return true;
                },
                [&](int) { f.kills++; });
            done++;
        };
        std::thread a(send_body), b(send_body), c(send_body);
        a.join(); b.join(); c.join();
        SMASH_CHECK(done == 3);
        SMASH_CHECK(f.sends == 3);
        SMASH_CHECK(f.maxInFlight == 1);
        SMASH_CHECK(f.kills == 0);
        SMASH_CHECK(slot == 100);
    }

    /* ── T9: close waits out an in-flight send, then kills ─────────── */
    {
        WsUplink up;
        int slot = 100;
        FakeLink f;
        up.state = PORT_WS_OPEN;
        Gate sendGate;

        std::thread sender([&] {
            WsUplinkSend(up, slot, /*s=*/100, BAD,
                [&](int) {
                    f.sends++;
                    sendGate.wait_inside();   /* blocked holding ioMu */
                    f.ev("send-done");
                    return true;
                },
                [&](int) { f.kills++; });
        });
        sendGate.await_entered();

        std::thread closer([&] {
            WsUplinkClose(up, slot, BAD,
                [] {},
                [&](int) { f.ev("kill"); f.kills++; });
        });
        /* Close already latched CLOSED + cleared the slot under mu; it
         * now waits on ioMu behind the send. */
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        SMASH_CHECK(up.state == PORT_WS_CLOSED);
        SMASH_CHECK(slot == BAD);
        SMASH_CHECK(f.kills == 0);

        sendGate.release();
        sender.join();
        closer.join();
        SMASH_CHECK(f.sends == 1 && f.kills == 1);
        /* Kill strictly after the send finished — never mid-send. */
        SMASH_CHECK(f.evIndex("send-done") < f.evIndex("kill"));
    }

    /* ── T10: send-fail racing a waiting close kills exactly once ──── */
    {
        WsUplink up;
        int slot = 100;
        FakeLink f;
        up.state = PORT_WS_OPEN;
        Gate sendGate;

        std::thread sender([&] {
            WsUplinkSend(up, slot, /*s=*/100, BAD,
                [&](int) {
                    f.sends++;
                    sendGate.wait_inside();
                    return false;             /* link died mid-send */
                },
                [&](int) { f.ev("kill"); f.kills++; });
        });
        sendGate.await_entered();

        std::thread closer([&] {
            WsUplinkClose(up, slot, BAD, [] {}, [&](int) { f.kills++; });
        });
        sendGate.release();
        sender.join();
        closer.join();

        /* Send path killed the dead socket; close saw slot == BAD and
         * did not kill again. */
        SMASH_CHECK(f.kills == 1);
        SMASH_CHECK(up.state == PORT_WS_CLOSED);
        SMASH_CHECK(slot == BAD);
    }

    /* ── T11: repeated close is idempotent ─────────────────────────── */
    {
        WsUplink up;
        int slot = 100;
        FakeLink f;
        up.state = PORT_WS_OPEN;

        WsUplinkClose(up, slot, BAD, [] {}, [&](int) { f.kills++; });
        WsUplinkClose(up, slot, BAD, [] {}, [&](int) { f.kills++; });
        WsUplinkClose(up, slot, BAD, [] {}, [&](int) { f.kills++; });
        SMASH_CHECK(f.kills == 1);
        SMASH_CHECK(up.state == PORT_WS_CLOSED);
    }

    /* ── T12: close during blocking connect kills fresh socket ─────── */
    {
        WsUplink up;
        int slot = BAD;
        FakeLink f;
        Gate dialGate;

        std::thread t([&] {
            int rc = WsUplinkConnect(up, slot, BAD, "ws://a",
                [&](const std::string&, std::string&) {
                    f.dials++;
                    dialGate.wait_inside();   /* dial in flight, no locks */
                    return f.nextHandle++;
                },
                [&](int) { f.ev("kill"); f.kills++; });
            SMASH_CHECK(rc == 0);
        });
        dialGate.await_entered();

        WsUplinkClose(up, slot, BAD, [] {}, [&](int) { f.kills++; });
        SMASH_CHECK(up.state == PORT_WS_CLOSED);
        dialGate.release();
        t.join();

        SMASH_CHECK(f.dials == 1);
        SMASH_CHECK(f.kills == 1);   /* the late socket, not the slot */
        SMASH_CHECK(slot == BAD);
        SMASH_CHECK(f.evIndex("kill") == 0);
    }

    /* ── T13: WsSend triage — uplink first, control fallback ───────── */
    {
        std::atomic<int> ctrl{PORT_WS_OPEN};
        int upCalls = 0, ctrlCalls = 0;

        /* Uplink wins: control socket untouched. */
        SMASH_CHECK(WsSend(ctrl, "x", 1, 0x1,
            [&](const void*, int, unsigned char) { upCalls++; return 1; },
            [&](const void*, int, unsigned char) { ctrlCalls++; return 1; }) == 1);
        SMASH_CHECK(upCalls == 1 && ctrlCalls == 0);

        /* Uplink unusable + control OPEN -> control socket used. */
        SMASH_CHECK(WsSend(ctrl, "x", 1, 0x1,
            [&](const void*, int, unsigned char) { upCalls++; return -1; },
            [&](const void*, int, unsigned char) { ctrlCalls++; return 1; }) == 1);
        SMASH_CHECK(upCalls == 2 && ctrlCalls == 1);

        /* Uplink unusable + control down -> frame dropped, 0. */
        ctrl = PORT_WS_FAILED;
        SMASH_CHECK(WsSend(ctrl, "x", 1, 0x1,
            [&](const void*, int, unsigned char) { upCalls++; return -1; },
            [&](const void*, int, unsigned char) { ctrlCalls++; return 1; }) == 0);
        SMASH_CHECK(upCalls == 3 && ctrlCalls == 1);
    }

    /* ── T14: resource balance across a full lifecycle ─────────────── */
    {
        WsUplink up;
        int slot = BAD;
        FakeLink f;

        /* connect -> send-fail -> lazy redial -> close: every installed
         * socket is killed exactly once, nothing orphaned. */
        int rc = WsUplinkConnect(up, slot, BAD, "ws://a",
            [&](const std::string&, std::string&) { f.dials++; return f.nextHandle++; },
            [&](int) { f.kills++; });
        SMASH_CHECK(rc == 1);

        f.sendSucceeds = false;
        WsUplinkSend(up, slot, slot, BAD,
            [&](int) { return false; },
            [&](int) { f.kills++; });
        SMASH_CHECK(up.state == PORT_WS_FAILED);
        SMASH_CHECK(slot == BAD);
        SMASH_CHECK(f.kills == 1);

        /* re-dial under mu needs url set — send-fail kept it. */
        int s = WsUplinkSocket(up, WsSteadyMs(), BAD, slot,
            [&](const std::string&) { f.dials++; return f.nextHandle++; },
            [&](int) { f.kills++; });
        SMASH_CHECK(s == 101);
        SMASH_CHECK(up.state == PORT_WS_OPEN);

        WsUplinkClose(up, slot, BAD, [] {}, [&](int) { f.kills++; });
        /* 2 successful dials -> 2 kills -> 0 live. */
        SMASH_CHECK(f.dials == 2 && f.kills == 2);
        SMASH_CHECK(slot == BAD);
    }

    /* ── T15: racing connect with the same URL — loser retires ─────── */
    {
        /* Two connects dialing the same URL: the drift check can't tell
         * them apart (textual identity), so the slot-occupancy guard is
         * what prevents the loser's socket being orphaned. */
        WsUplink up;
        int slot = BAD;
        FakeLink f;
        Gate dialGate;

        std::thread loser([&] {
            int rc = WsUplinkConnect(up, slot, BAD, "ws://a",
                [&](const std::string&, std::string&) {
                    f.dials++;
                    dialGate.wait_inside();
                    return 100;              /* loser dialed handle 100 */
                },
                [&](int h) { f.ev("kill"); f.kills++; SMASH_CHECK(h == 100); });
            SMASH_CHECK(rc == 0);            /* lost the install race */
        });
        dialGate.await_entered();

        /* Winner installs while the loser is still blocked in dial. */
        {
            std::lock_guard<std::mutex> l(up.mu);
            slot = 200;                      /* winner's socket */
            up.state = PORT_WS_OPEN;
        }
        dialGate.release();
        loser.join();

        /* Loser retired its own handle — winner's slot untouched. */
        SMASH_CHECK(f.kills == 1);
        SMASH_CHECK(slot == 200);
        SMASH_CHECK(up.state == PORT_WS_OPEN);
    }

    /* ── T16: lazy redial racing a same-URL install — loser retires ── */
    {
        /* Mirror of T15 through the lazy path: WsUplinkSocket's dial
         * ran under ioMu while a blocking connect installed the same
         * URL — drift passes (textual match) so the occupancy guard is
         * the only thing stopping a duplicate install. */
        WsUplink up;
        int slot = BAD;
        FakeLink f;
        Gate dialGate;
        latch_failed(up, "ws://a");

        std::thread t([&] {
            int s = WsUplinkSocket(up, WsSteadyMs(), BAD, slot,
                [&](const std::string&) {
                    f.dials++;
                    dialGate.wait_inside();
                    return 100;
                },
                [&](int h) { f.kills++; SMASH_CHECK(h == 100); });
            SMASH_CHECK(s == BAD);
        });
        dialGate.await_entered();
        {
            std::lock_guard<std::mutex> l(up.mu);
            slot = 200;
            up.state = PORT_WS_OPEN;
        }
        dialGate.release();
        t.join();

        SMASH_CHECK(f.kills == 1);
        SMASH_CHECK(slot == 200);
        SMASH_CHECK(up.state == PORT_WS_OPEN);
    }

    return smash_test_report("ws_uplink_test");
}
