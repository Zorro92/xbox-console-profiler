"""Verify that all PE binaries inside a packaged .msix only import allowed UWP/DirectX DLLs.

Adapted from rav1g's testing/uwp_imports/check_imports.py + check_package.py.
Usage:
  uv run --with pefile==2024.8.26 python tools/check_imports.py --allow tools/uwp-dlls-allow.txt --out <dir> <packages-dir-or-msix>
"""

import argparse
import os
import sys
import zipfile
from pathlib import Path

import pefile


def read_allow(path: Path):
    allowed = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        entry = line.split("#", 1)[0].strip()
        if not entry:
            continue
        name, _, functions = entry.partition(":")
        name = name.strip().casefold()
        if functions.strip():
            allowed[name] = {f.strip() for f in functions.split(",") if f.strip()}
        else:
            allowed[name] = None
    return allowed


def imports_of(path: Path):
    rows = []
    with pefile.PE(str(path)) as pe:
        for directory, kind in (("DIRECTORY_ENTRY_IMPORT", "normal"), ("DIRECTORY_ENTRY_DELAY_IMPORT", "delay")):
            for entry in getattr(pe, directory, ()):
                dll = entry.dll.decode("ascii").casefold()
                for sym in entry.imports:
                    rows.append((kind, dll, sym.name.decode("ascii") if sym.name else f"#{sym.ordinal}"))
    return rows


def find_package(target: Path) -> Path:
    if target.is_file():
        return target
    found = []
    for root, _, files in os.walk(target):
        if os.sep + "dependencies" + os.sep in (root + os.sep).lower():
            continue
        for name in files:
            if name.lower().endswith((".msix", ".appx")):
                found.append(Path(root) / name)
    if len(found) != 1:
        raise RuntimeError(f"found {len(found)} packages under {target}, expected 1")
    return found[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--allow", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("package", type=Path)
    args = parser.parse_args()

    allowed = read_allow(args.allow)
    pkg = find_package(args.package)
    unpacked = args.out / "unpacked"
    unpacked.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(pkg) as zf:
        programs = [n for n in zf.namelist() if n.lower().endswith((".exe", ".dll"))]
        for name in programs:
            zf.extract(name, unpacked)

    if not programs:
        print(f"FAIL: no .exe/.dll in {pkg}")
        return 2

    failed = False
    summary_lines = [f"{pkg.name}: {', '.join(programs)}"]
    report_lines = ["image\tkind\tdll\tfunction"]

    for name in programs:
        img = unpacked / name
        rows = imports_of(img)
        report_lines += [f"{name}\t{k}\t{d}\t{fn}" for k, d, fn in rows]
        bad = {}
        for kind, dll, fn in rows:
            if dll not in allowed:
                bad.setdefault((kind, dll, "disallowed DLL"), []).append(fn)
            elif allowed[dll] is not None and fn not in allowed[dll]:
                bad.setdefault((kind, dll, "disallowed function"), []).append(fn)
        for (kind, dll, why), fns in sorted(bad.items()):
            failed = True
            summary_lines.append(f"FAIL {name}: {kind} import from {dll} ({why}): {', '.join(sorted(fns))}")
        dlls = sorted({d for _, d, _ in rows})
        summary_lines.append(f"{'checked' if not bad else 'FAILED'} {name}: {len(rows)} functions from {len(dlls)} DLLs")

    if not failed:
        summary_lines.append(f"PASS: {len(programs)} image(s); all normal and delayed imports are UWP-safe")

    out_text = "\n".join(summary_lines) + "\n"
    print(out_text, end="")
    (args.out / "check.txt").write_text(out_text, encoding="utf-8")
    (args.out / "imports.tsv").write_text("\n".join(report_lines) + "\n", encoding="utf-8")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
