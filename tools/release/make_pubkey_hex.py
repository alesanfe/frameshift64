#!/usr/bin/env python3
"""make_pubkey_hex — derive config/release_pubkey.hex from a minisign .pub.

The updater embeds the release public key as a compile definition
(BATTLESHIP_RELEASE_PUBKEY, raw 32-byte Ed25519 key in hex). This tool turns
the `minisign -G` public-key file into that hex string.

    minisign -G -p release.pub -s release.key   # once, offline
    python tools/release/make_pubkey_hex.py release.pub > config/release_pubkey.hex

Then commit config/release_pubkey.hex (public key — safe) and store the
contents of release.key as the MINISIGN_SECRET_KEY GitHub secret. NEVER
commit release.key.

See docs/development/release_signing.md.
"""
import base64
import sys


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write(f"usage: {sys.argv[0]} <release.pub>\n")
        return 2
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        lines = [l.strip() for l in f]
    body = None
    for i, line in enumerate(lines):
        if line.startswith("untrusted comment:") and i + 1 < len(lines):
            body = lines[i + 1]
            break
    if body is None:
        sys.stderr.write("no public-key line found after 'untrusted comment:'\n")
        return 1
    raw = base64.b64decode(body)
    # minisign PubkeyStruct: sig_alg(2, 'Ed') || keynum(8) || pubkey(32) = 42 bytes
    if len(raw) != 42 or raw[0:2] != b"Ed":
        sys.stderr.write(
            f"unexpected .pub layout ({len(raw)} bytes, alg={raw[0:2]!r}) — "
            "expected minisign Ed25519 public key\n"
        )
        return 1
    print(raw[10:].hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
