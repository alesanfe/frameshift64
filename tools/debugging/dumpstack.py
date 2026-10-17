#!/usr/bin/env python3
"""Extract the faulting thread's stack return-addresses from a minidump.

Usage: dumpstack.py <crash.dmp>
Prints candidate code addresses (RVA into each module) so a symbolizer can
resolve them. Works with minidump files written by portWriteMinidump().
"""
import sys
from minidump.minidumpfile import MinidumpFile
from minidump.streams import (ExceptionStream, ModuleListStream,
                              ThreadListStream, MemoryInfoListStream,
                              MemoryListStream)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: dumpstack.py <crash.dmp>", file=sys.stderr)
        return 2
    mdmp = MinidumpFile.parse(sys.argv[1])

    # module list -> name/base pairs
    mods = []
    for m in mdmp.modules.modules:
        name = m.name.split("\\")[-1].split("/")[-1]
        mods.append((m.baseaddress, m.baseaddress + m.size, name))
    mods.sort()

    def sym(addr):
        for base, end, name in mods:
            if base <= addr < end:
                return f"{name}+0x{addr - base:x}"
        return None

    exc_list = mdmp.exception
    if exc_list is None or not exc_list.exception_records:
        print("no exception stream", file=sys.stderr)
        return 1
    exc = exc_list.exception_records[0]
    er = exc.ExceptionRecord
    print(f"exception code: 0x{er.ExceptionCode:08X} addr: 0x{er.ExceptionAddress:x} ({sym(er.ExceptionAddress)})")
    print(f"params: {[hex(p) for p in er.ExceptionInformation[:4] if p]}")
    print(f"faulting thread id: {exc.ThreadId}")

    # find the faulting thread + its stack memory
    tid = exc.ThreadId
    tctx = exc.ThreadContext   # context comes with the exception record
    stack_thread = None
    for t in mdmp.threads.threads:
        if t.thread_id == tid:
            stack_thread = t
            break
    if stack_thread is None:
        print("faulting thread not in thread list", file=sys.stderr)
        return 1

    # thread context: Windows x64 -> RIP/RSP
    rip = tctx.rip if hasattr(tctx, "rip") else None
    rsp = tctx.rsp if hasattr(tctx, "rsp") else None
    print(f"RIP=0x{rip:x} RSP=0x{rsp:x}")

    # locate the stack bytes in memory ranges
    stack_bytes = b""
    for r in mdmp.memory_segments.memory_segments:
        if r.start_address <= rsp < r.start_address + r.size:
            off = rsp - r.start_address
            stack_bytes = r.data[off:]
            break
    print(f"stack bytes from RSP: {len(stack_bytes)}")

    # walk qwords: anything inside a module is a candidate return addr
    import struct
    seen = []
    for i in range(0, min(len(stack_bytes), 0x4000) - 8, 8):
        val = struct.unpack("<Q", stack_bytes[i:i + 8])[0]
        s = sym(val)
        if s and "BattleShip.exe" in s or (s and s.startswith("libultraship")):
            seen.append((rsp + i, val, s))
    print("\nreturn-addr candidates into our code:")
    for sp, val, s in seen[:40]:
        print(f"  [rsp+{sp - rsp:04x}] 0x{val:x} -> {s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
