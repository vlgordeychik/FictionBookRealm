#!/usr/bin/env python3
"""Fetch the third-party prerequisites that are NOT shipped in this archive.

Run this once, before `idf.py build`:

    python tools/fetch_deps.py

It downloads two M5Stack libraries from GitHub at pinned tags, unpacks them
into components/, and re-applies the local modifications this project relies
on.  Everything else (FreeType2, FatFs, pugixml, stb, bm8563) is vendored and
needs no network.

    M5GFX     0.2.28   ~49 MiB tarball / ~170 MiB unpacked (CJK bitmap fonts
                       account for ~122 MiB of that; they are compiled into the
                       component but never referenced by the FB2 reader)
    M5Unified 0.2.21   ~0.6 MiB tarball

Once this has run, the tree builds fully offline.

Options:
    --check     report which prerequisites are present / missing, change nothing
    --force     re-download even if the target directory already exists
    --strict    fail if a tarball checksum differs from the pinned value
                (GitHub tarball hashes are stable in practice but are not
                 contractually guaranteed, so a mismatch is a warning by default)
"""

import argparse
import hashlib
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMPONENTS_DIR = os.path.join(PROJECT_ROOT, "components")

CACHE_DIR = os.path.join(PROJECT_ROOT, ".deps-cache")

# name, tag, component dir, expected tarball size, expected sha256
DEPS = [
    (
        "m5stack/M5GFX",
        "0.2.28",
        "M5GFX",
        49_127_684,
        "5fcaaa11366f4f80e291564275ae4ecb1e827f28957b4224894d256c88512f1b",
    ),
    (
        "m5stack/M5Unified",
        "0.2.21",
        "M5Unified",
        589_124,
        "b6a7b6bceacc2116777b602b2878e16da93a378234c2cc2794b7b54528e60aeb",
    ),
]

# ─── Local modifications that must survive the download ──────────────────────
#
# Both are needed to run on this hardware / this ESP-IDF. They are applied
# with strict matching, so an upstream change will fail loudly instead of
# silently dropping the fix.

PATCHES = [
    {
        "component": "M5GFX",
        "file": "src/lgfx/v1/platforms/esp32/common.cpp",
        "why": "spi_bus_config_t::dma_burst_size is mandatory from ESP-IDF 6.0",
        "anchor": """        buscfg.intr_flags = 0;
""",
        "replacement": """        buscfg.intr_flags = 0;
#if defined (ESP_IDF_VERSION_VAL) && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
        buscfg.dma_burst_size = 0;
#endif
""",
    },
    {
        "component": "M5Unified",
        "file": "src/utility/Power_Class.cpp",
        "why": "M5Paper: switch the SD card rail on (M5PM1 + M5IOE1 gpio6)",
        "anchor": """        ioe1.setDirection(M5IOE1_Class::gpio3, false);
      }
      M5pm1.setBatteryCharge(true);
""",
        "replacement": """        ioe1.setDirection(M5IOE1_Class::gpio3, false);

        // Turn on SD card power
        ioe1.setHighImpedance(M5IOE1_Class::gpio6, false); 
        ioe1.setDirection(M5IOE1_Class::gpio6, true);
        ioe1.digitalWrite(M5IOE1_Class::gpio6, true);
      }
      M5pm1.setBatteryCharge(true);
""",
    },
]

# M5Unified declares a registry dependency on m5stack/m5gfx. M5GFX is already
# unpacked into components/M5GFX, and the registry copy would land in
# managed_components/m5stack__m5gfx -- a second, redundant M5GFX (another 122 MiB
# of CJK fonts) that then has to be downloaded on every configure. Drop it so
# the tree stays offline after this script has run.
M5UNIFIED_MANIFEST = "idf_component.yml"


def human(num):
    return f"{num / (1024 * 1024):.1f} MiB"


def tarball_url(repo, tag):
    owner, name = repo.split("/")
    return f"https://codeload.github.com/{owner}/{name}/tar.gz/refs/tags/{tag}"


def download(url, dest, expect_size, expect_sha, strict):
    print(f"  downloading {url}")
    with urllib.request.urlopen(url, timeout=300) as response, open(dest, "wb") as out:
        shutil.copyfileobj(response, out, length=1 << 20)

    size = os.path.getsize(dest)
    if size != expect_size:
        sys.exit(f"error: size mismatch, got {size}, expected {expect_size}")

    digest = hashlib.sha256()
    with open(dest, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    actual = digest.hexdigest()

    if actual != expect_sha:
        message = f"checksum mismatch for {os.path.basename(dest)}:\n  got      {actual}\n  expected {expect_sha}"
        if strict:
            sys.exit("error: " + message)
        print(f"  warning: {message}\n  warning: continuing (GitHub tarballs are not hash-stable by contract)")
    else:
        print(f"  sha256 ok ({human(size)})")
    return dest


def fetch_archive(repo, tag, expect_size, expect_sha, force, strict):
    os.makedirs(CACHE_DIR, exist_ok=True)
    cached = os.path.join(CACHE_DIR, f"{repo.replace('/', '_')}-{tag}.tar.gz")
    if os.path.exists(cached) and not force:
        print(f"  using cached {os.path.basename(cached)}")
        return cached
    return download(tarball_url(repo, tag), cached, expect_size, expect_sha, strict)


def unpack(archive, repo, tag, component):
    target = os.path.join(COMPONENTS_DIR, component)
    if os.path.isdir(target):
        print(f"  {component}: already present, skipping (use --force to refresh)")
        return False

    print(f"  unpacking into components/{component}")
    staging = tempfile.mkdtemp(prefix=f"{component}-", dir=COMPONENTS_DIR)
    try:
        with tarfile.open(archive, "r:gz") as tar:
            # Refuse absolute paths / traversal before extracting anything.
            for member in tar.getmembers():
                name = member.name.replace("\\", "/")
                if name.startswith("/") or ".." in name.split("/"):
                    sys.exit(f"error: unsafe path in {archive}: {member.name}")
            tar.extractall(staging)

        prefix = f"{repo.split('/')[1]}-{tag}"
        extracted = os.path.join(staging, prefix)
        if not os.path.isdir(extracted):
            entries = os.listdir(staging)
            if len(entries) != 1:
                sys.exit(f"error: unexpected archive layout in {archive}: {entries}")
            extracted = os.path.join(staging, entries[0])

        # Drop any .git dir that git-archive style tarballs may carry.
        shutil.rmtree(os.path.join(staging, ".git"), ignore_errors=True)
        os.replace(extracted, target)

    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return True


def apply_patches(component, file, why, anchor, replacement, skip_missing=False):
    path = os.path.join(COMPONENTS_DIR, component, *file.split("/"))
    if not os.path.exists(path):
        if skip_missing:
            return False
        sys.exit(f"error: {component}/{file} not found (upstream layout changed?)")

    with open(path, "r", encoding="utf-8", newline="") as handle:
        text = handle.read()

    if replacement in text:
        print(f"  = {component}/{file}: patch already applied")
        return False

    count = text.count(anchor)
    if count != 1:
        sys.exit(
            f"error: cannot apply patch to {component}/{file}\n"
            f"  reason: expected exactly 1 occurrence of the anchor, found {count}\n"
            f"  patch : {why}\n"
            f"  The upstream file changed - port this modification by hand."
        )

    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text.replace(anchor, replacement))
    print(f"  + {component}/{file}: {why}")
    return True


def strip_m5unified_registry_dep():
    path = os.path.join(COMPONENTS_DIR, "M5Unified", M5UNIFIED_MANIFEST)
    if not os.path.exists(path):
        return
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "m5stack/m5gfx" not in text:
        print(f"  = M5Unified/{M5UNIFIED_MANIFEST}: no registry dependency")
        return
    kept, dropped = [], []
    skipping = False
    for line in text.splitlines(keepends=True):
        if line.startswith("dependencies:"):
            skipping = True
            dropped.append(line)
            continue
        if skipping and (line[:1].isspace() or not line.strip()):
            dropped.append(line)
            continue
        skipping = False
        kept.append(line)
    note = (
        "# NOTE (fetch_deps.py): the upstream registry dependency on m5stack/m5gfx\n"
        "# was removed. M5GFX lives in components/M5GFX; resolving it through the\n"
        "# component manager would download a redundant second copy.\n"
    )
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("".join(kept) + note)
    print(f"  + M5Unified/{M5UNIFIED_MANIFEST}: dropped registry dep (kept M5GFX vendored)")


def check():
    ok = True
    for repo, tag, component, _, _ in DEPS:
        target = os.path.join(COMPONENTS_DIR, component)
        if not os.path.isdir(target):
            print(f"  MISSING  components/{component}  ({repo} {tag})")
            ok = False
            continue
        pending = []
        for patch in PATCHES:
            if patch["component"] != component:
                continue
            path = os.path.join(target, *patch["file"].split("/"))
            if not os.path.exists(path):
                continue
            with open(path, "r", encoding="utf-8", newline="") as handle:
                text = handle.read()
            if patch["replacement"] not in text:
                pending.append(patch["file"])
        if pending:
            print(f"  STALE    components/{component}  (local patch not applied: {', '.join(pending)})")
            ok = False
        else:
            print(f"  OK       components/{component}  ({repo} {tag})")
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="report status, change nothing")
    parser.add_argument("--force", action="store_true", help="re-download even if present")
    parser.add_argument("--strict", action="store_true", help="fail on tarball checksum mismatch")
    args = parser.parse_args()

    if not os.path.isdir(COMPONENTS_DIR):
        sys.exit(f"error: {COMPONENTS_DIR} not found - run this from the project root")

    if args.check:
        print("prerequisites:")
        return check()

    print("fetching prerequisites into components/ ...\n")
    for repo, tag, component, size, sha in DEPS:
        print(f"{repo} {tag}")
        archive = fetch_archive(repo, tag, size, sha, args.force, args.strict)
        unpack(archive, repo, tag, component)
        print()

    print("applying local modifications ...\n")
    for patch in PATCHES:
        apply_patches(**patch)
    strip_m5unified_registry_dep()

    print("\ndone. Next:  idf.py set-target esp32s3 && idf.py build")
    return 0


if __name__ == "__main__":
    sys.exit(main())
