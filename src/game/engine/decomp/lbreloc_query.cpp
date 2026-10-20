/** lbreloc_query.cpp — extern/intern/force buffer and heap getters.
 * Split from lbreloc_bridge.cpp; the pipeline overview lives there. */
#include "lbreloc_bridge_internal.h"

extern "C" {

size_t lbRelocGetExternBytesNum(u32 file_id)
{
	s32 i;

	if (lbRelocFindStatusBufferFile(file_id) != NULL)
	{
		return 0;
	}

	for (i = 0; i < sLBRelocExternFileIDsNum; i++)
	{
		if (file_id == sLBRelocExternFileIDs[i])
		{
			return 0;
		}
	}

	if (sLBRelocExternFileIDsNum >= sLBRelocExternFileIDsMax)
	{
		while (TRUE)
		{
			syDebugPrintf("Relocatable Data Manager: External Data is over %d!!\n",
			              sLBRelocExternFileIDsMax);
			scManagerRunPrintGObjStatus();
		}
	}

	auto relocFile = portLoadRelocResource(file_id);
	if (!relocFile) { return 0; }

	size_t bytes_read = (u32)LBRELOC_CACHE_ALIGN(relocFile->Data.size());
	sLBRelocExternFileIDs[sLBRelocExternFileIDsNum++] = file_id;

	for (u16 dep_id : relocFile->ExternFileIds)
	{
		bytes_read += lbRelocGetExternBytesNum(dep_id);
	}

	return bytes_read;
}

size_t lbRelocGetFileSize(u32 id)
{
	u32 file_ids_extern_buf[50];

	sLBRelocExternFileIDs = file_ids_extern_buf;
	sLBRelocExternFileIDsNum = 0;
	sLBRelocExternFileIDsMax = ARRAY_COUNT(file_ids_extern_buf);

	return lbRelocGetExternBytesNum(id);
}

// // // // // // // // // // // //
//                               //
//     BRIDGE: FILE LOADING      //
//                               //
// // // // // // // // // // // //

void* lbRelocGetExternBufferFile(u32 id)
{
	void *file = lbRelocFindStatusBufferFile(id);
	if (file != NULL) { return file; }

	auto relocFile = portLoadRelocResource(id);
	if (!relocFile) { return NULL; }

	void *file_alloc = (void *)LBRELOC_CACHE_ALIGN((uintptr_t)sLBRelocExternFileHeap);
	size_t file_size = relocFile->Data.size();
	sLBRelocExternFileHeap = (void *)((uintptr_t)file_alloc + file_size);

	lbRelocLoadAndRelocFile(id, file_alloc, (u32)file_size, nLBFileLocationExtern);
	return file_alloc;
}

void* lbRelocGetExternHeapFile(u32 id, void *heap)
{
	sLBRelocExternFileHeap = heap;
	return lbRelocGetExternBufferFile(id);
}

void* lbRelocGetInternBufferFile(u32 id)
{
	void *file = lbRelocFindStatusBufferFile(id);
	if (file != NULL) { return file; }

	auto relocFile = portLoadRelocResource(id);
	if (!relocFile) { return NULL; }

	void *file_alloc = (void *)LBRELOC_CACHE_ALIGN((uintptr_t)sLBRelocInternBuffer.heap_ptr);
	size_t file_size = relocFile->Data.size();

	if (((uintptr_t)file_alloc + file_size) > (uintptr_t)sLBRelocInternBuffer.heap_end)
	{
		while (TRUE)
		{
			syDebugPrintf("Relocatable Data Manager: Buffer is full !!\n");
			scManagerRunPrintGObjStatus();
		}
	}
	sLBRelocInternBuffer.heap_ptr = (void *)((uintptr_t)file_alloc + file_size);

	lbRelocLoadAndRelocFile(id, file_alloc, (u32)file_size, nLBFileLocationDefault);
	return file_alloc;
}

void* lbRelocGetForceExternBufferFile(u32 id)
{
	void *file = lbRelocFindForceStatusBufferFile(id);
	if (file != NULL) { return file; }

	auto relocFile = portLoadRelocResource(id);
	if (!relocFile) { return NULL; }

	void *file_alloc = (void *)LBRELOC_CACHE_ALIGN((uintptr_t)sLBRelocExternFileHeap);
	size_t file_size = relocFile->Data.size();
	sLBRelocExternFileHeap = (void *)((uintptr_t)file_alloc + file_size);

	lbRelocLoadAndRelocFile(id, file_alloc, (u32)file_size, nLBFileLocationForce);
	return file_alloc;
}

void* lbRelocGetForceExternHeapFile(u32 id, void *heap)
{
	/* NOTE: rewinding abandons prior loads above `heap`, and their corpse
	 * entries linger in the address-keyed registries until a same-range
	 * load evicts them (a smaller reload leaves the old tail's entries).
	 * Do NOT try to evict [heap, sLBRelocExternFileHeap) here:
	 * sLBRelocExternFileHeap is shared with the extern-heap bump path, so
	 * that span can contain LIVE extern files — evicting it invalidates
	 * their tokens and crashes scene 28 (verified regression). Corpse
	 * chain-slot entries are instead detected at use: the runtime texture
	 * fixup validates a candidate slot's word via
	 * portRelocTryResolvePointer before clamping. */
	sLBRelocExternFileHeap = heap;
	sLBRelocInternBuffer.force_status_buffer_num = 0;
	return lbRelocGetForceExternBufferFile(id);
}

/* Mod helper: same force-status-buffer reset that
 * lbRelocGetForceExternHeapFile does on entry. Mods that route mod-
 * registered anim file IDs through portRelocLoadFileFromBytes (skipping
 * the orig wrapper because the file isn't in the engine's file table)
 * still need this reset, otherwise the lbRelocAddForceStatusBufferFile
 * call inside portRelocLoadFileFromBytes accumulates entries across
 * fighter status changes until the per-scene cap is exceeded and the
 * engine enters its "Force Status Buffer is full !!" panic loop. */
extern "C" void portRelocResetForceStatusBuffer(void)
{
	sLBRelocInternBuffer.force_status_buffer_num = 0;
}

/* Accessors for the extern-file bump cursor. portRelocLoadFileFromBytes
 * resets sLBRelocExternFileHeap to point past the file it just loaded
 * so that file's own extern deps bump-allocate contiguously
 * after it. That's correct when the destination is the engine's shared
 * extern heap, but when a mod loads a file into its own malloc'd buffer
 * (CharacterEngine's private-buffer hooks) the reset leaves the cursor
 * pointing *inside* that private buffer. If the OUTER file's chain walk
 * then loads a later vanilla dep, it bump-allocates into the small
 * private buffer and overruns it. CharacterEngine brackets each private
 * mod-file load with get/set so the outer walk's cursor survives the
 * nested load. */
extern "C" void *portRelocGetExternFileHeap(void)
{
	return sLBRelocExternFileHeap;
}

extern "C" void portRelocSetExternFileHeap(void *heap)
{
	sLBRelocExternFileHeap = heap;
}

// // // // // // // // // // // //
//                               //
//     BRIDGE: BATCH LOADING     //
//                               //
// // // // // // // // // // // //

} // extern "C"
