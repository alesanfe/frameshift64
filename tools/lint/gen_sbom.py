#!/usr/bin/env python3
"""Generate a minimal CycloneDX 1.5 SBOM for release artifacts.

Sources (all tracked, deterministic):
  - .gitmodules                 -> pinned submodules (resolved to HEAD sha)
  - CMakeLists.txt / cmake/*.cmake -> FetchContent GIT_TAG pins
  - services/netplay-lobby/package-lock.json -> npm deps

Usage: python tools/lint/gen_sbom.py [--out sbom.cdx.json]
"""
import argparse, json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

def submodules():
    comps = []
    gm = ROOT / ".gitmodules"
    if not gm.exists():
        return comps
    path = url = None
    for line in gm.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        m = re.match(r'path\s*=\s*(\S+)', line)
        if m: path = m.group(1)
        m = re.match(r'url\s*=\s*(\S+)', line)
        if m: url = m.group(1)
        if line.startswith("[submodule"):
            path = url = None
        if path and url:
            sha = subprocess.run(["git", "-C", str(ROOT), "ls-tree", "HEAD", path],
                                 capture_output=True, text=True).stdout.split()[2:3]
            comps.append({
                "type": "library", "name": path, "version": sha[0] if sha else "unpinned",
                "purl": f"pkg:github/{'/'.join(url.rsplit('/', 2)[-2:]).removesuffix('.git')}@{sha[0] if sha else ''}",
                "externalReferences": [{"type": "vcs", "url": url}],
                "properties": [{"name": "smash:surface", "value": "submodule"}],
            })
            path = url = None
    return comps

def fetchcontent():
    comps = []
    texts = [ROOT / "CMakeLists.txt"] + list((ROOT / "cmake").glob("*.cmake"))
    for f in texts:
        if not f.exists():
            continue
        txt = f.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(
                r'FetchContent_Declare\s*\(\s*(\w+)[^)]*?GIT_REPOSITORY\s+(\S+)[^)]*?GIT_TAG\s+([0-9a-f]{7,40}|[\w.+-]+)',
                txt, re.S):
            name, url, tag = m.groups()
            comps.append({
                "type": "library", "name": name, "version": tag,
                "purl": f"pkg:github/{'/'.join(url.rsplit('/', 2)[-2:]).removesuffix('.git')}@{tag}",
                "externalReferences": [{"type": "vcs", "url": url}],
                "properties": [{"name": "smash:surface", "value": "fetchcontent"}],
            })
    return comps

def npm_deps():
    lock = ROOT / "services/netplay-lobby/package-lock.json"
    if not lock.exists():
        return []
    data = json.loads(lock.read_text(encoding="utf-8"))
    comps = []
    for path, meta in data.get("packages", {}).items():
        if not path or "node_modules/" not in path:
            continue
        name = path.split("node_modules/")[-1]
        ver = meta.get("version", "?")
        comps.append({
            "type": "library", "name": name, "version": ver,
            "purl": f"pkg:npm/{name}@{ver}",
            "properties": [{"name": "smash:surface", "value": "lobby-service"}],
        })
    return comps

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="-")
    a = ap.parse_args()
    import uuid
    ver = (ROOT / "VERSION").read_text().strip() if (ROOT / "VERSION").exists() else "dev"
    bom = {
        "bomFormat": "CycloneDX", "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, 'smash-classic/' + ver)}",
        "version": 1,
        "metadata": {
            "component": {"type": "application", "name": "smash-classic",
                          "version": ver},
        },
        "components": submodules() + fetchcontent() + npm_deps(),
    }
    out = json.dumps(bom, indent=2)
    if a.out == "-":
        print(out)
    else:
        Path(a.out).write_text(out, encoding="utf-8")
        print(f"sbom: {len(bom['components'])} components -> {a.out}", file=sys.stderr)

if __name__ == "__main__":
    main()
