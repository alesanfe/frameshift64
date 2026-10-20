#include "OnlineStatusViewModel.h"

#include "netlobby.h"

extern "C" int syNetRollbackIsDesynced(void);

namespace ssb64 {

namespace {
constexpr const char* const kFighterNames[] = { "Mario", "Fox",     "Donkey Kong", "Samus", "Link",      "Yoshi",
                                                "Kirby", "Captain Falcon",        "Pikachu", "Jigglypuff", "Ness" };

const char* PeerLabel(bool connected, int fkind) {
    if (!connected) {
        return "connecting...";
    }
    if (fkind >= 0 && fkind < static_cast<int>(sizeof(kFighterNames) / sizeof(kFighterNames[0]))) {
        return kFighterNames[fkind];
    }
    return "choosing...";
}
} // namespace

OnlineStatusViewState ComputeOnlineStatusView(const OnlineStatusRaw& raw) {
    OnlineStatusViewState view;
    if (raw.state == PORT_NETLOBBY_IN_MATCH && raw.desynced) {
        view.mode = OnlineStatusMode::Desynced;
    } else if (raw.afterMatch && raw.state == PORT_NETLOBBY_CONNECTED) {
        view.mode = OnlineStatusMode::AfterMatch;
    }
    view.status = raw.status != nullptr ? raw.status : "";
    view.isHost = raw.isHost;
    view.isRelay = raw.isRelay;
    view.rematchEnabled = raw.peerFkind >= 0;
    view.pingMs = raw.pingMs;
    view.peers.reserve(raw.peers.size());
    for (const OnlineStatusRaw::RawPeer& peer : raw.peers) {
        if (peer.slot < 0 || peer.slot == raw.mySlot) {
            continue;
        }
        view.peers.push_back({ peer.slot, PeerLabel(peer.connected, peer.fkind) });
    }
    return view;
}

OnlineStatusViewState OnlineStatusViewModel::Capture() const {
    OnlineStatusRaw raw;
    raw.state = port_netlobby_state();
    raw.desynced = syNetRollbackIsDesynced() != 0;
    raw.afterMatch = port_netlobby_after_match() != 0;
    raw.mySlot = port_netlobby_my_slot();
    raw.pingMs = port_netlobby_ping_ms();
    raw.isHost = port_netlobby_is_host() != 0;
    raw.isRelay = port_netlobby_is_relay() != 0;
    raw.peerFkind = port_netlobby_peer_fkind();
    raw.status = port_netlobby_status();
    const int peers = port_netlobby_peer_count();
    raw.peers.reserve(static_cast<size_t>(peers > 0 ? peers : 0));
    for (int i = 0; i < peers; i++) {
        raw.peers.push_back({ port_netlobby_peer_slot(i), port_netlobby_peer_connected_at(i) != 0,
                              port_netlobby_peer_fkind_at(i) });
    }
    return ComputeOnlineStatusView(raw);
}

void OnlineStatusViewModel::ReturnToRoom() const {
    port_netlobby_on_match_end();
}

void OnlineStatusViewModel::Leave() const {
    port_netlobby_cancel();
}

void OnlineStatusViewModel::Rematch() const {
    port_netlobby_start_match();
}

} // namespace ssb64
