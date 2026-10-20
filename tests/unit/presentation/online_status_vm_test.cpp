/* OnlineStatusViewModel — the seam between the netplay port ABI and the
 * overlay. Stubs implement the exact extern "C" surface Capture() reads,
 * so the test exercises the real snapshot path (not a re-implementation):
 * mode resolution, self/slot-less peer filtering, fighter label mapping,
 * rematch gating, and command dispatch. */
#include "OnlineStatusViewModel.h"
#include "netlobby.h"

#include <smash_test/check.h>

using ssb64::OnlineStatusMode;
using ssb64::OnlineStatusViewModel;

/* ── Fake session state the stubs serve ─────────────────────────────── */
namespace {
struct StubPeer {
    int slot;
    int connected;
    int fkind;
};
struct StubSession {
    int state = PORT_NETLOBBY_IDLE;
    int desynced = 0;
    int afterMatch = 0;
    int mySlot = 0;
    int pingMs = -1;
    int isHost = 0;
    int isRelay = 0;
    int peerFkind = -1;
    const char* status = "status.key";
    StubPeer peers[8] = {};
    int peerCount = 0;
    int cancelCalls = 0;
    int startMatchCalls = 0;
    int matchEndCalls = 0;
} g;
} // namespace

extern "C" {

int port_netlobby_state(void) { return g.state; }
const char* port_netlobby_status(void) { return g.status; }
int port_netlobby_after_match(void) { return g.afterMatch; }
int port_netlobby_my_slot(void) { return g.mySlot; }
int port_netlobby_ping_ms(void) { return g.pingMs; }
int port_netlobby_is_host(void) { return g.isHost; }
int port_netlobby_is_relay(void) { return g.isRelay; }
int port_netlobby_peer_fkind(void) { return g.peerFkind; }
int port_netlobby_peer_count(void) { return g.peerCount; }
int port_netlobby_peer_slot(int i) { return g.peers[i].slot; }
int port_netlobby_peer_connected_at(int i) { return g.peers[i].connected; }
int port_netlobby_peer_fkind_at(int i) { return g.peers[i].fkind; }
void port_netlobby_cancel(void) { g.cancelCalls++; }
void port_netlobby_start_match(void) { g.startMatchCalls++; }
void port_netlobby_on_match_end(void) { g.matchEndCalls++; }
int syNetRollbackIsDesynced(void) { return g.desynced; }

} // extern "C"

int main() {
    const OnlineStatusViewModel vm;

    /* Idle session → hidden overlay. */
    auto view = vm.Capture();
    SMASH_CHECK(view.mode == OnlineStatusMode::Hidden);

    /* In-match without desync still hides — the overlay only interrupts
     * when something needs the player's attention. */
    g.state = PORT_NETLOBBY_IN_MATCH;
    view = vm.Capture();
    SMASH_CHECK(view.mode == OnlineStatusMode::Hidden);

    /* Desync during a match → the warning mode. */
    g.desynced = 1;
    view = vm.Capture();
    SMASH_CHECK(view.mode == OnlineStatusMode::Desynced);

    /* Desync flag while merely connected is not enough — mode requires
     * IN_MATCH. */
    g.state = PORT_NETLOBBY_CONNECTED;
    view = vm.Capture();
    SMASH_CHECK(view.mode == OnlineStatusMode::Hidden);

    /* After-match in the room → the rematch panel, carrying status, host
     * flag, relay marker and ping through the snapshot. */
    g.desynced = 0;
    g.afterMatch = 1;
    g.isHost = 1;
    g.isRelay = 1;
    g.pingMs = 64;
    g.peerFkind = 1; /* remote picked Fox → rematch enabled */
    g.mySlot = 0;
    g.peerCount = 3;
    g.peers[0] = { 1, 1, 4 };    /* connected, picked Link   → "Link"        */
    g.peers[1] = { 2, 1, -1 };   /* connected, still picking → "choosing..." */
    g.peers[2] = { 3, 0, -1 };   /* dial-up in flight        → "connecting..."*/
    view = vm.Capture();
    SMASH_CHECK(view.mode == OnlineStatusMode::AfterMatch);
    SMASH_CHECK(view.isHost);
    SMASH_CHECK(view.isRelay);
    SMASH_CHECK(view.rematchEnabled);
    SMASH_CHECK(view.pingMs == 64);
    SMASH_CHECK(view.status == "status.key");
    SMASH_CHECK(view.peers.size() == 3);
    SMASH_CHECK(view.peers[0].slot == 1 && view.peers[0].label == "Link");
    SMASH_CHECK(view.peers[1].label == "choosing...");
    SMASH_CHECK(view.peers[2].label == "connecting...");

    /* A connected peer with an out-of-range fkind also reports choosing. */
    g.peers[0].fkind = 99;
    view = vm.Capture();
    SMASH_CHECK(view.peers[0].label == "choosing...");

    /* Self-slot and slot-less peers are filtered out of the list. */
    g.mySlot = 1;
    g.peers[0] = { 1, 1, 0 };   /* that's me — must not render */
    g.peers[1] = { -1, 1, 0 };  /* no game slot — skipped */
    g.peers[2] = { 3, 1, 0 };
    view = vm.Capture();
    SMASH_CHECK(view.peers.size() == 1);
    SMASH_CHECK(view.peers[0].slot == 3);

    /* Rematch disabled while the remote hasn't picked. */
    g.peerFkind = -1;
    view = vm.Capture();
    SMASH_CHECK(!view.rematchEnabled);

    /* Commands route to the session layer — the window never calls the
     * ABI directly. */
    const int cancel0 = g.cancelCalls, start0 = g.startMatchCalls, end0 = g.matchEndCalls;
    vm.Leave();
    vm.Rematch();
    vm.ReturnToRoom();
    SMASH_CHECK(g.cancelCalls == cancel0 + 1);
    SMASH_CHECK(g.startMatchCalls == start0 + 1);
    SMASH_CHECK(g.matchEndCalls == end0 + 1);

    /* after_match without CONNECTED (e.g. still IN_MATCH tearing down)
     * stays hidden. */
    g.state = PORT_NETLOBBY_IN_MATCH;
    view = vm.Capture();
    SMASH_CHECK(view.mode == OnlineStatusMode::Hidden);

    return smash_test_report("online_status_vm_test");
}
