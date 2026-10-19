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
 * index exists; name is NUL-truncated to `capacity`. `banned` is the
 * server community-moderation flag (LB_BAN_REPORTS distinct claim keys
 * reported the name inside 24h). */
int port_netleaderboard_count(void);
int port_netleaderboard_at(int index, char *out_name, int capacity,
                           int *rating, int *wins, int *losses,
                           int *suspicious, int *banned);
/* Report a board name for moderation — POST <server>/report with our
 * claim key (the server requires the reporter's key to back an entry). */
void port_netleaderboard_report(const char *server, const char *name);
/* My current rank (1-based) if my claimed name is on the board, else 0. */
int port_netleaderboard_my_rank(void);

/* Match history — GET <server>/history (newest-first verified matches).
 * Same fetch-state convention as the board: 0 idle, 1 fetching,
 * 2 ready, -1 error. */
int  port_netleaderboard_history_state(void);
void port_netleaderboard_fetch_history(const char *server);
int  port_netleaderboard_history_count(void);
/* One cached match row. Names are NUL-truncated to their capacities;
 * `draw` records a draw (w/l empty); `stamp` gets "MM-DD HH:MM" UTC.
 * Returns 0 for an out-of-range index. */
typedef struct PortLbMatchRow {
    char a[128], b[128];   /* both participants */
    char w[128], l[128];   /* winner/loser (empty on draw) */
    char stamp[24];
    int draw;
} PortLbMatchRow;
int port_netleaderboard_history_at(int index, PortLbMatchRow *out);

/* Player card — GET <server>/player?name=X returns the board row plus
 * rank and that player's recent verified matches. Same state convention;
 * a 404 (unclaimed name) lands on -1 like any other failure. */
int  port_netleaderboard_player_state(void);
void port_netleaderboard_fetch_player(const char *server, const char *name);
/* Fills the loaded card; returns 0 when no card is ready. */
int  port_netleaderboard_player_card(char *name, int capacity, int *rank,
                                     int *rating, int *wins, int *losses);
int  port_netleaderboard_player_matches_count(void);
int  port_netleaderboard_player_match_at(int index, PortLbMatchRow *out);

/* Replay sharing — uploads the .ssr at `path` to POST <server>/replay.
 * share_state: 0 idle / 1 in flight / 2 ready (link in share_link) / -1 failed. */
int port_netleaderboard_share_state(void);
int port_netleaderboard_share_link(char *out, int capacity);
void port_netleaderboard_share_replay(const char *server, const char *path);

/* Shared-replay browsing — GET <server>/replays returns the index
 * {replays: [{id, at, by}]} newest-first; GET <server>/replay/<id>
 * returns {data: base64}. Download decodes and saves the .ssr into the
 * local replays dir so the replay browser can play it.
 * states: 0 idle / 1 in flight / 2 ready / -1 error. */
int  port_netleaderboard_replays_state(void);
void port_netleaderboard_fetch_replays(const char *server);
int  port_netleaderboard_replays_count(void);
int  port_netleaderboard_replays_at(int index, char *id, int idCap,
                                    char *by, int byCap, char *stamp, int stCap);
int  port_netleaderboard_replay_dl_state(void);
int  port_netleaderboard_replay_dl_path(char *out, int capacity);
void port_netleaderboard_replay_download(const char *server, const char *id);

/* Tournaments — GET <server>/tournament lists every bracket view
 * ({brackets: [{id,name,owner,size,status,winner,players,rounds}]}).
 * join/create/start are claim-key POSTs — the player needs a submitted
 * leaderboard name first (the server checks keyOwns). After a successful
 * mutation the list re-fetches so the UI converges automatically. */
int  port_netleaderboard_tournament_state(void);
void port_netleaderboard_fetch_tournaments(const char *server);
int  port_netleaderboard_tournament_count(void);

typedef struct PortLbBracket {
    char id[16];
    char name[64];
    char owner[128];
    char status[12];      /* open | live | done */
    char winner[128];     /* empty unless done */
    int size;             /* pow2 bracket slots */
    int joined;           /* players currently joined */
    int mine;             /* owner or joined player is the local claim */
    int owner_mine;       /* local claim owns the bracket (can start it) */
} PortLbBracket;
int  port_netleaderboard_tournament_at(int index, PortLbBracket *out);
/* Bracket detail: round count and one pairing of bracket `index` —
 * pairing reuses PortLbMatchRow with w=winner (empty while unsettled). */
int  port_netleaderboard_tournament_rounds(int index);
int  port_netleaderboard_tournament_pairing(int index, int round, int slot,
                                            PortLbMatchRow *out);
/* Joined-player names (open brackets list signups before start). */
int  port_netleaderboard_tournament_player_at(int index, int p,
                                              char *out, int capacity);
void port_netleaderboard_tournament_join(const char *server, const char *id);
void port_netleaderboard_tournament_start(const char *server, const char *id);
void port_netleaderboard_tournament_create(const char *server,
                                           const char *name, int size);

#ifdef __cplusplus
}
#endif
