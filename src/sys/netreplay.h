#ifndef _SYNETREPLAY_H_
#define _SYNETREPLAY_H_

#include <PR/ultratypes.h>
#include <sc/scene.h>
#include <sys/netinput.h>

extern void syNetReplayInitDebugEnv(void);
extern void syNetReplayArmAutoSave(void);
extern void syNetReplayWriteDesyncPackage(u32 tick, u32 local_hash, u32 remote_hash);
extern void syNetReplayCaptureBattleMetadata(SCBattleState *battle_state, SYNetInputReplayMetadata *metadata);
extern void syNetReplayApplyBattleMetadata(const SYNetInputReplayMetadata *metadata);
extern void syNetReplayStartVSSession(SCBattleState *battle_state);
extern void syNetReplayUpdate(void);
extern void syNetReplayFinishVSSession(void);
extern sb32 syNetReplayWriteDebugFile(const char *path);
extern sb32 syNetReplayLoadDebugFile(const char *path);

#endif /* _SYNETREPLAY_H_ */
