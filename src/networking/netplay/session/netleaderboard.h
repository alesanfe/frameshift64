#pragma once

/* netleaderboard — client side of the lobby's self-reported rating board
 * (services/netplay-lobby /leaderboard + /submit). Entries are claim-key
 * bound display names: the server can't verify match outcomes, so this
 * is a friends-scale board, not authoritative ranking.
 *
 * Both calls are non-blocking: work runs on a detached thread and the UI
 * polls state()/row accessors. Requires SSB64_HAVE_CURL — on platforms
 * without it the functions compile to no-ops so callers stay simple. */

#ifdef __cplusplus
extern "C" {
#endif

/* Fetch state for the UI list. */
int port_netleaderboard_state(void);    /* 0 idle, 1 fetching, 2 ready, -1 error */

/* Kick a background GET <server>/leaderboard. `server` is the lobby URL
 * as configured (ws:// or wss:// — converted to http(s) here). */
void port_netleaderboard_fetch(const char *server);

/* Kick a background POST <server>/submit with the local name/rating.
 * Called by the lobby after a match is recorded. Gated by the
 * gNetplay.SubmitStats CVar at the call site. */
void port_netleaderboard_submit(const char *server);

/* Kick a background POST <server>/result — the verified path. Both
 * clients of a 1v1 report the same match: the server cross-checks the
 * two claim keys and applies Elo only when they mirror (win vs loss, or
 * draw vs draw). `won` is from our perspective (1 win, 0 loss, -1 draw).
 * The match_id is derived here from code|session|sorted names|duration
 * so both clients hash the same bytes without a wire round-trip. */
void port_netleaderboard_report_result(const char *server, const char *code,
                                       unsigned int session, const char *opp_name,
                                       int won, unsigned int duration_ticks);

/* Cached rows (sorted, rating desc). Row accessors return 1 when the
 * index exists; name is NUL-truncated to `capacity`. */
int port_netleaderboard_count(void);
int port_netleaderboard_at(int index, char *out_name, int capacity,
                           int *rating, int *wins, int *losses);
/* My current rank (1-based) if my claimed name is on the board, else 0. */
int port_netleaderboard_my_rank(void);

#ifdef __cplusplus
}
#endif
