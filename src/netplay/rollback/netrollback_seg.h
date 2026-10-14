/* Force-included (MSVC /FI) into every gameplay-module TU so their globals
 * land in dedicated PE sections that sys/netrollback.c snapshots wholesale.
 * The linker orders ".ssbd$a" < ".ssbd$m" < ".ssbd$z", and netrollback.c
 * places start/end markers in the $a/$z subsections. Initialized and
 * zero-initialized globals use separate sections so their attributes match.
 * const data is unaffected (it stays in .rdata and is never rolled back). */
#if defined(_MSC_VER)
#pragma data_seg(".ssbd$m")
#pragma bss_seg(".ssbb$m")
#endif
