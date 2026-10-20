# Stamps sw.js with the content hash of the final wasm so a rebuild that
# changes the game binary also changes sw.js — which is what triggers the
# browser's service-worker update check.
# Invoked as: cmake -DIN=<sw.js.in> -DOUT=<sw.js> -DWASM=<BattleShip.wasm> -P stamp_sw.cmake
file(SHA256 "${WASM}" SSB64_SW_VERSION)
string(SUBSTRING "${SSB64_SW_VERSION}" 0 12 SSB64_SW_VERSION)
configure_file("${IN}" "${OUT}" @ONLY)
