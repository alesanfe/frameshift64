/* Mod-bridge link anchors — extracted from port.cpp.
 *
 * Force the linker to pull bridge .obj files into the EXE so their
 * exported symbols land in the .edata table; the accessor wrappers exist
 * because MSVC's WINDOWS_EXPORT_ALL_SYMBOLS exports functions but not
 * most non-trivial global data objects. See the comments below.
 */
#ifndef DISABLE_SCRIPTING
#include <ship/scripting/ScriptLoader.h>
#include <libultraship/bridge/scriptingbridge.h>
#endif

#ifndef DISABLE_SCRIPTING
/* Force the linker to pull bridge .obj files into the EXE so their
 * exported symbols land in the .edata table. Mods call these by name;
 * nothing in the EXE itself references them, so without an explicit
 * anchor they get dropped at link time and the post-build tcc -impdef
 * step doesn't see them. */
extern "C" int   mod_install_hook(const char* symbol_name, void* replacement, void** original_out);
extern "C" void* mod_resolve_symbol(const char* symbol_name);
extern "C" void* sScriptingBridgeAnchor = (void*)&ScriptGetFunction;
extern "C" void* sModBridgeAnchorHook    = (void*)&mod_install_hook;
extern "C" void* sModBridgeAnchorResolve = (void*)&mod_resolve_symbol;

/* MSVC's WINDOWS_EXPORT_ALL_SYMBOLS pass exports global *functions* but
 * not most non-trivial global *data* objects, so engine globals like
 * gGCCommonLinks don't make it into BattleShip.def and TCC mods can't
 * resolve them by name. Wrap the ones mods commonly want behind tiny
 * accessor functions; functions always export. */
extern "C" struct GObj;
extern "C" struct GObj *gGCCommonLinks_Ref(int link_id);
extern "C" struct GObj *gGCCommonLinks_Ref(int link_id) {
    extern struct GObj *gGCCommonLinks[];
    /* No bounds-check: callers pass the engine's own enum constants. */
    return gGCCommonLinks[link_id];
}
extern "C" void* sModBridgeAnchorFighterListRef = (void*)&gGCCommonLinks_Ref;

/* Stage geometry: mods that distribute spawn positions across the
 * active stage (StaryuSquad's followers etc) need map_bound_left /
 * map_bound_right / map_bound_bottom from the engine's ground data.
 * Same export hole as gGCCommonLinks -- wrap the pointer. */
struct MPGroundData;
extern "C" struct MPGroundData *gMPCollisionGroundData_Ref(void);
extern "C" struct MPGroundData *gMPCollisionGroundData_Ref(void) {
    extern struct MPGroundData *gMPCollisionGroundData;
    return gMPCollisionGroundData;
}
extern "C" void* sModBridgeAnchorGroundDataRef = (void*)&gMPCollisionGroundData_Ref;

/* Fighter data-files lookup table. Exposed so mod-loader code can
 * write a registered character's FTData* into the trailing NULL-sentinel
 * slot (index nFTKindEnumCount) during ftManagerMakeFighter and have
 * the engine's internal `fp->data = dFTManagerDataFiles[fp->fkind]`
 * indexing resolve to that entry. Same export-hole workaround as the
 * other globals. */
struct FTData;
extern "C" struct FTData **dFTManagerDataFiles_Ref(void);
extern "C" struct FTData **dFTManagerDataFiles_Ref(void) {
    extern struct FTData *dFTManagerDataFiles[];
    return dFTManagerDataFiles;
}
extern "C" void* sModBridgeAnchorDataFilesRef = (void*)&dFTManagerDataFiles_Ref;
#endif
