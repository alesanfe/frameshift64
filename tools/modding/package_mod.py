#!/usr/bin/env python3
"""Package a C-mod source folder into a runtime-loadable mod directory.

Produces the exact layout the runtime loader expects (see
mods/examples/*/CMakeLists.txt — this script replicates their output so
the engine build can stage bundled example mods without a nested
configure):

    <out-dir>/
        manifest.json
        build.gen          # newline list of dist/*.c entry points
        dist/<relpath>.c   # merge_mod.py preprocessed sources
        include/**         # copied verbatim (reference / leftover includes)
        assets/**          # copied verbatim, if present
        preview.png|.jpg|.jpeg  # if present

Usage:
    python tools/modding/package_mod.py \
        --mod-dir mods/examples/playertint \
        --out-dir <bindir>/bundled-mods/playertint \
        --repo-root <repo>

Include resolution matches the mods' own CMakeLists: the mod's include/
and src/ first, then the repo roots (src/, decomp/, libultraship/) the
mod sources include against.
"""

import argparse
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def fnv1a_64(data: bytes, h: int = 0xCBF29CE484222325) -> int:
    """FNV-1a 64 over `data`, matching Archive::IsChecksumValid."""
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def manifest_checksum(out_dir: Path) -> str:
    """FNV-1a 64 over every file except manifest.json, name-sorted.
    Format: 'fnv1a64:<16-hex>'."""
    files = []
    for p in out_dir.rglob("*"):
        if p.is_file() and p.name != "manifest.json":
            files.append(p.relative_to(out_dir).as_posix())
    files.sort()
    h = 0xCBF29CE484222325
    for name in files:
        data = (out_dir / name).read_bytes()
        h = fnv1a_64(name.encode("utf-8"), h)
        h = fnv1a_64(struct.pack("<Q", len(data)), h)
        h = fnv1a_64(data, h)
    return f"fnv1a64:{h:016x}"


def first_minisig_body(path: Path) -> str:
    """Return the first non-comment base64 line of a minisig/pub file."""
    body = ""
    comment_seen = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("untrusted comment:"):
            comment_seen = True
            continue
        if comment_seen and line.strip():
            body = line.strip()
            break
    return body


def sign_checksum(checksum: str, secret_key: Path, pubkey_path: Path) -> tuple[str, str]:
    """Sign the checksum string with minisign -l; return (signature_b64, pubkey_b64)."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
        f.write(checksum)
        msg_path = Path(f.name)
    try:
        result = subprocess.run(
            ["minisign", "-S", "-l", "-s", str(secret_key), "-m", str(msg_path)],
            capture_output=True, text=True, check=False
        )
        if result.returncode != 0:
            raise RuntimeError(f"minisign failed: {result.stderr}")
        sig_path = msg_path.with_suffix(msg_path.suffix + ".minisig")
        if not sig_path.is_file():
            raise RuntimeError(f"minisig did not produce {sig_path}")
        signature = first_minisig_body(sig_path)
        if not signature:
            raise RuntimeError("could not read signature line")
        pubkey = first_minisig_body(pubkey_path)
        if not pubkey:
            raise RuntimeError(f"could not read public key from {pubkey_path}")
        return signature, pubkey
    finally:
        msg_path.unlink(missing_ok=True)
        sig_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Package a C mod for runtime loading.")
    parser.add_argument("--mod-dir", required=True, type=Path, help="Mod source folder (manifest.json + src/)")
    parser.add_argument("--out-dir", required=True, type=Path, help="Packaged output directory")
    parser.add_argument("--repo-root", required=True, type=Path, help="Repository root (for include dirs)")
    parser.add_argument("--checksum", action="store_true",
                        help="Inject an FNV-1a 64 checksum of the packaged files")
    parser.add_argument("--sign-key", type=Path, default=None,
                        help="Path to a minisign secret key; implies --checksum")
    parser.add_argument("--pubkey", type=Path, default=None,
                        help="Path to the minisign public key matching --sign-key")
    args = parser.parse_args()

    mod_dir = args.mod_dir.resolve()
    out_dir = args.out_dir.resolve()
    repo_root = args.repo_root.resolve()

    manifest = mod_dir / "manifest.json"
    if not manifest.is_file():
        print(f"package_mod: no manifest.json in {mod_dir}", file=sys.stderr)
        return 1

    merge_mod = Path(__file__).resolve().parent / "merge_mod.py"

    # Same include search order as mods/examples/*/CMakeLists.txt.
    include_dirs = [
        mod_dir / "include",
        mod_dir / "src",
        repo_root,
        repo_root / "src",
        repo_root / "decomp" / "src",
        repo_root / "decomp" / "include",
        repo_root / "libultraship" / "src",
        repo_root / "libultraship" / "include",
    ]

    src_dir = mod_dir / "src"
    src_files = sorted(
        p for p in src_dir.rglob("*.c") if not p.name.endswith(".inc.c")
    )
    if not src_files:
        print(f"package_mod: no .c sources under {src_dir}", file=sys.stderr)
        return 1

    dist_dir = out_dir / "dist"
    build_gen_entries = []
    for src in src_files:
        rel = src.relative_to(src_dir)
        out_file = dist_dir / rel
        out_file.parent.mkdir(parents=True, exist_ok=True)
        cmd = [
            sys.executable, str(merge_mod),
            "--out", str(out_file),
            "--includes", *[str(d) for d in include_dirs],
            "--srcs", str(src),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            print(f"package_mod: merge_mod failed for {src}:\n{result.stdout}{result.stderr}",
                  file=sys.stderr)
            return 1
        build_gen_entries.append(f"dist/{rel.as_posix()}")

    # build.gen — the manifest's "main" entry point list, paths relative
    # to the packaged root, one per line (matches the CMake echo output).
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "build.gen").write_text("\n".join(build_gen_entries), encoding="utf-8")

    shutil.copy2(manifest, out_dir / "manifest.json")

    for sub in ("include", "assets"):
        src_sub = mod_dir / sub
        if src_sub.is_dir():
            shutil.copytree(src_sub, out_dir / sub, dirs_exist_ok=True)

    for name in ("preview.png", "preview.jpg", "preview.jpeg"):
        if (mod_dir / name).is_file():
            shutil.copy2(mod_dir / name, out_dir / name)
            break

    if args.checksum or args.sign_key:
        out_manifest = out_dir / "manifest.json"
        manifest_obj = json.loads(out_manifest.read_text(encoding="utf-8"))
        manifest_obj["checksum"] = manifest_checksum(out_dir)
        if args.sign_key:
            if not args.pubkey:
                print("package_mod: --pubkey is required when --sign-key is used", file=sys.stderr)
                return 1
            sig, pub = sign_checksum(manifest_obj["checksum"], args.sign_key, args.pubkey)
            manifest_obj["signature"] = sig
            manifest_obj["public-key"] = pub
        out_manifest.write_text(json.dumps(manifest_obj, indent=4) + "\n", encoding="utf-8")

    print(f"package_mod: {mod_dir.name} -> {out_dir} ({len(build_gen_entries)} source unit(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
