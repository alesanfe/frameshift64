/* Lobby state objects and peer bookkeeping — split from netlobby.cpp; the wire-protocol overview lives there. */
#include "netlobby_internal.h"
#include <cctype>
/* State-free helpers (compat token, env, split, rules reads)
 * live in netlobby_util.cpp; wire JSON parsing in lobby_json.cpp. */
using namespace ssb64::netlobby;

namespace ssb64::netlobby {

Lobby sLobby;
AutoLobby sAuto;

void set_state(PortNetLobbyState state, const std::string &status)
{
    if (sLobby.state != state || sLobby.status != status) {
        sLobby.state = state;
        sLobby.status = status;
        port_log("SSB64 Lobby: %s\n", status.c_str());
    }
}

static int peer_index_for_slot(int slot)
{
    for (int i = 0; i < kMaxRemotePeers; i++) {
        if (sLobby.peers[i].slot == slot) {
            return i;
        }
    }
    return -1;
}

Peer *peer_for_slot(int slot)
{
    int i = peer_index_for_slot(slot);
    return (i >= 0) ? &sLobby.peers[i] : nullptr;
}

Peer *peer_new(int slot)
{
    Peer *p = peer_for_slot(slot);
    if (p != nullptr) {
        return p;
    }
    for (Peer &cand : sLobby.peers) {
        if (cand.slot < 0) {
            cand = Peer{};
            cand.slot = slot;
            return &cand;
        }
    }
    return nullptr;
}

void peer_drop(int slot)
{
    if (peer_index_for_slot(slot) >= 0) {
        sLobby.peers[peer_index_for_slot(slot)] = Peer{};
        port_udp_link_clear(slot);
        if (is_spec_slot(slot)) {
            syNetPeerRemoveSpecLink(slot);
        }
    }
}

int count_connected_peers(void)      /* players only; spectators are never counted */
{
    int n = 0;
    for (const Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.slot <= 3 && p.connected) {
            n++;
        }
    }
    return n;
}

int count_connected_specs(void)
{
    int n = 0;
    for (const Peer &p : sLobby.peers) {
        if (is_spec_slot(p.slot) && p.connected) {
            n++;
        }
    }
    return n;
}

} // namespace ssb64::netlobby
