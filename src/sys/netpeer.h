#ifndef _SYNETPEER_H_
#define _SYNETPEER_H_

#include <PR/ultratypes.h>
#include <ssb_types.h>
#include <sys/netinput.h>

extern void syNetPeerInitDebugEnv(void);
extern void syNetPeerStartVSSession(void);
extern void syNetPeerBeginFrame(void);
extern sb32 syNetPeerCheckBattleExecutionReady(void);
extern sb32 syNetPeerIsRollbackActive(void);
extern s32 syNetPeerGetRemotePlayer(void);
extern u32 syNetPeerGetRemoteConfirmedTick(void);
extern sb32 syNetPeerCheckStartBarrierReleased(void);
extern void syNetPeerUpdateBattleGate(void);
extern void syNetPeerUpdate(void);
extern void syNetPeerStopVSSession(void);
extern void syNetPeerStartOnline(s32 local_player, s32 player_count, u32 delay, sb32 is_host,
                                 const SYNetInputReplayMetadata *metadata, const s32 *wire_slots,
                                 u32 session_id);
extern void syNetPeerMakeOnlineMetadata(SYNetInputReplayMetadata *metadata, u32 stage, u32 stocks, sb32 is_items, u32 seed,
                                        u32 player_count, const u8 *fighter_kinds, const u8 *costumes);

#ifdef PORT
extern sb32 sSYNetPeerIsSpectator;
extern void portNetPeerGetRemoteStats(s32 player, u32 *gaps, u32 *dups, u32 *ooo, s32 *advantage,
                                      u32 *recvHighwater);
extern void syNetPeerAddSpecLink(s32 link);
extern void syNetPeerRemoveSpecLink(s32 link);
extern void syNetPeerBeginSpecCatchUp(s32 link);
extern void syNetPeerSpectateCatchUp(void (*update)(void));
extern void syNetPeerStartOnlineSpectate(s32 player_count, u32 delay, u32 stage, u32 stocks, s32 is_items, u32 seed,
                                         u32 session_id, const s32 *fkinds, const s32 *costumes);
#endif

#endif /* _SYNETPEER_H_ */
