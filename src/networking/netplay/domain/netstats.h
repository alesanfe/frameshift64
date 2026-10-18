#pragma once

/* Local online stats — match history + a self-computed rating, persisted
 * to <save dir>/netstats.json. There are no accounts: opponents are
 * identified by the rating they declare in their PICK (additive trailing
 * field, ignored by older peers), and results come from the engine's
 * stock counts at match end. Ratings stay local — this is a mirror of
 * Slippi-style self-rating, not a server leaderboard. */

#ifdef __cplusplus
extern "C" {
#endif

/* Current rating (starts at 1200, K=32 Elo vs the opponent's declared
 * rating; vs unknown opponents a fixed-provisional match counts half). */
int port_netstats_rating(void);
int port_netstats_wins(void);
int port_netstats_losses(void);
int port_netstats_draws(void);
int port_netstats_provisional(void);   /* 1 while history < 20 matches */

/* Rating the last PICKing peer declared; -1 when unknown/old peer. */
int port_netstats_opponent_rating(void);
void port_netstats_set_opponent_rating(int rating);

/* Record a finished online match. my_slot/winner_slot are game slots
 * (winner_slot == -1 = draw). Called by the lobby when a VS session ends. */
void port_netstats_record_match(int my_slot, int winner_slot,
                                unsigned int duration_ticks);

/* History: index 0 = most recent. Returns 1 when the row exists. */
int port_netstats_history_count(void);
int port_netstats_history_at(int index, int *won, int *opp_rating,
                             unsigned int *duration_ticks,
                             unsigned long long *unix_ts);

/* Recently used room codes (most recent first), for the Join menu. */
int port_netstats_recent_room_count(void);
int port_netstats_recent_room_at(int index, char *out_code, int capacity);
void port_netstats_note_room(const char *code);

/* Leaderboard claim token: a locally-generated random hex string that
 * proves "same player" when updating a leaderboard entry. Written into
 * `out` (32 chars + NUL needed); returns the length. */
int port_netstats_player_key(char *out, int capacity);

/* Seasons: monthly, "YYYY-MM". A new season archives the previous record
 * and soft-resets rating halfway to 1200. Current season written into
 * `out`; past seasons via season_at (index 0 = most recent archive). */
int port_netstats_season(char *out, int capacity);
int port_netstats_season_count(void);
int port_netstats_season_at(int index, char *out_id, int capacity,
                            int *rating, int *wins, int *losses);

/* Competitive tier from the local rating — display-only projection; the
 * leaderboard stays Elo-pure. 0=Unranked(provisional) 1=Bronze 2=Silver
 * 3=Gold 4=Platinum 5=Diamond 6=Master 7=Grandmaster. tier_name writes
 * the label into `out`. */
int port_netstats_rank_tier(void);
int port_netstats_rank_tier_name(char *out, int capacity);

#ifdef __cplusplus
}
#endif
