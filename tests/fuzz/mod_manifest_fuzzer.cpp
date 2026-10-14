/* mod_manifest_fuzzer.cpp — libFuzzer target for Archive::ParseManifestBuffer.
 *
 * The manifest parser is a static pure-parse function; this drives it
 * with arbitrary bytes so JSON edge cases (deep nesting, huge numbers,
 * truncated UTF-8) hit the real field extraction, not just the JSON
 * tokenizer.
 *
 * Build:
 *   cmake -DSSB64_BUILD_TESTING=ON -DSSB64_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++ ...
 *   ./mod_manifest_fuzzer tests/fuzz/corpora/manifest
 */

#include <cstddef>
#include <cstdint>

#include <ship/resource/archive/Archive.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Ship::ArchiveManifest manifest;
    (void)Ship::Archive::ParseManifestBuffer(
        reinterpret_cast<const char*>(data), size, &manifest);
    return 0;
}
