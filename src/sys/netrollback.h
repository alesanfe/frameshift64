#ifndef _SYNETROLLBACK_H_
#define _SYNETROLLBACK_H_

#include <PR/ultratypes.h>
#include <ssb_types.h>
#include <stddef.h>

/* Rollback snapshot/restore for VS battles. See docs/rollback_design.md.
 *
 * A snapshot is: every global of the gameplay modules (grouped into one PE
 * section by port/netrollback_seg.h, which CMake force-includes into those
 * TUs), the general heap allocator, and the scene arena from the VS setup
 * watermark up to the allocator's bump pointer. */

#define SYNETROLLBACK_SLOT_NUM 8

extern void syNetRollbackInitDebugEnv(void);
extern void syNetRollbackStartVSSession(void);
extern void syNetRollbackFinishVSSession(void);

extern void syNetRollbackNoteFileLoad(void *dst, size_t size, sb32 is_figatree);

extern sb32 syNetRollbackIsTickTimer(void);
extern sb32 syNetRollbackIsResimulating(void);
extern sb32 syNetRollbackCheckCanSave(void);
extern sb32 syNetRollbackSave(s32 slot);
extern void syNetRollbackLoad(s32 slot);

extern void syNetRollbackRunBattleUpdate(void (*update)(void));

#endif /* _SYNETROLLBACK_H_ */
