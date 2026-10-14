/* config_json_fuzzer.cpp — libFuzzer target for the JSON parser shared
 * by the config file, mod manifests and network messages.
 *
 * Same coverage the SSB64_FUZZ_PARSER=json test hook reaches, but
 * coverage-guided instead of mutation-driven.
 *
 * Build:
 *   cmake -DSSB64_BUILD_TESTING=ON -DSSB64_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++ ...
 *   ./config_json_fuzzer tests/fuzz/corpora/json
 */

#include <cstddef>
#include <cstdint>

#include <nlohmann/json.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        auto j = nlohmann::json::parse(data, data + size,
                                       nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded()) {
            /* Round-trip to exercise the serializer too — a parse/serialize
             * disagreement is as interesting as a parse crash. */
            (void)j.dump();
        }
    } catch (...) {
        /* allow_exceptions=false makes this unreachable; kept as a guard. */
    }
    return 0;
}
