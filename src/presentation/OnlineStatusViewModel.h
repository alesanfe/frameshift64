#pragma once

#include <string>
#include <vector>

namespace ssb64 {

/* Online status overlay, split view-model side: OnlineStatusViewModel owns
 * every read of the netplay/rollback port ABI plus every command the
 * buttons issue, and produces a plain snapshot the window can render
 * without knowing netlobby.h exists (ADR-0008: UI threads own a copy of
 * cross-thread state; the ABI is only touched inside Capture()). */

enum class OnlineStatusMode { Hidden, Desynced, AfterMatch };

struct OnlinePeerView {
    int slot = -1;       // remote's game slot — display as slot + 1
    std::string label;   // fighter name, or "choosing..." / "connecting..."
};

struct OnlineStatusViewState {
    OnlineStatusMode mode = OnlineStatusMode::Hidden;
    std::string status;     // message key — the view still applies TR()
    bool isHost = false;
    bool isRelay = false;
    bool rematchEnabled = false;
    int pingMs = -1;        // -1 = no measurement yet
    std::vector<OnlinePeerView> peers;  // self and slot-less peers filtered out
};

/* Raw ABI reads, separated from the mapping logic so the transform is
 * testable with a fabricated input — and so a future push-based producer
 * (thread-owned session state) can fill the same struct without touching
 * ComputeOnlineStatusView. */
struct OnlineStatusRaw {
    int state = 0;          // PORT_NETLOBBY_*
    bool desynced = false;
    bool afterMatch = false;
    int mySlot = -1;
    int pingMs = -1;
    bool isHost = false;
    bool isRelay = false;
    int peerFkind = -1;     // port_netlobby_peer_fkind() — rematch gate
    const char* status = "";
    struct RawPeer {
        int slot = -1;
        bool connected = false;
        int fkind = -1;
    };
    std::vector<RawPeer> peers;
};

/* Pure transform — the seam the unit test pins down. */
OnlineStatusViewState ComputeOnlineStatusView(const OnlineStatusRaw& raw);

class OnlineStatusViewModel {
  public:
    /* Snapshot the session state once for this frame. */
    OnlineStatusViewState Capture() const;

    /* Commands — the buttons' intents. */
    void ReturnToRoom() const;  // desynced: end the match, back to the room
    void Leave() const;
    void Rematch() const;
};

} // namespace ssb64
