"""Stamp and verify side-by-side UWP package variants for xbox-console-profiler.

All variants share the exact same compiled XboxConsoleProfiler.exe (verified byte-for-byte
via SHA-256 across the produced .msix packages), while carrying distinct package identities
and display names so all variants can be installed side by side on the console.

Variants:
  plain     XboxConsoleProfiler.Plain     internetClient + inputInjectionBrokered
  hevc      XboxConsoleProfiler.Hevc      + rescap:hevcPlayback
  game      XboxConsoleProfiler.Game      same capabilities as plain; dedicated slot for
                                          flipping Dev Home "App type" to Game
  expanded  XboxConsoleProfiler.Expanded  + rescap:expandedResources (deprecated/dev-only)

Usage:
  python tools/make_variants.py stamp --variant <name> --version <M.m.b.r> --publisher <CN=...> <Package.appxmanifest>
  python tools/make_variants.py check --base-dir <plain packages dir> --variant <name> --variant-dir <packages dir> [--note <file>]
  python tools/make_variants.py self-test
"""

import argparse
import hashlib
import os
import re
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

FOUNDATION = "http://schemas.microsoft.com/appx/manifest/foundation/windows10"
RESCAP = "http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"

VARIANTS = {
    "plain": {
        "identity": "XboxConsoleProfiler.Plain",
        "phone_id": "2c693047-96c2-46d2-8a4a-1b7d42000001",
        "display_name": "Xbox Profiler (Plain)",
        "description": "Xbox Console Profiler - Plain App variant",
        "extra_rescap": [],
    },
    "hevc": {
        "identity": "XboxConsoleProfiler.Hevc",
        "phone_id": "2c693047-96c2-46d2-8a4a-1b7d42000002",
        "display_name": "Xbox Profiler (hevcPlayback)",
        "description": "Xbox Console Profiler - hevcPlayback capability variant",
        "extra_rescap": ["hevcPlayback"],
    },
    "game": {
        "identity": "XboxConsoleProfiler.Game",
        "phone_id": "2c693047-96c2-46d2-8a4a-1b7d42000003",
        "display_name": "Xbox Profiler (Game)",
        "description": "Xbox Console Profiler - Dedicated slot for Dev Home App type = Game",
        "extra_rescap": [],
    },
    "expanded": {
        "identity": "XboxConsoleProfiler.Expanded",
        "phone_id": "2c693047-96c2-46d2-8a4a-1b7d42000004",
        "display_name": "Xbox Profiler (expandedResources)",
        "description": "Xbox Console Profiler - expandedResources capability variant (dev only)",
        "extra_rescap": ["expandedResources"],
    },
}

BASE_CAPABILITIES = [
    (FOUNDATION, "internetClient"),
    (RESCAP, "inputInjectionBrokered"),
]


class Problem(Exception):
    pass


def stamp_manifest_text(text: str, variant: str, version: str, publisher: str) -> str:
    if variant not in VARIANTS:
        raise Problem(f"unknown variant {variant!r}; expected one of {', '.join(VARIANTS)}")
    spec = VARIANTS[variant]

    if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", version):
        raise Problem(f"invalid version {version!r}; expected M.m.b.r")

    # Ensure rescap namespace is declared and ignorable.
    pkg_match = re.search(r"<Package\b[^>]*>", text)
    if not pkg_match:
        raise Problem("no <Package> element")
    pkg_tag = pkg_match.group(0)
    new_pkg_tag = pkg_tag
    if RESCAP not in pkg_tag:
        new_pkg_tag = re.sub(
            r"\s*>$",
            f' xmlns:rescap="{RESCAP}">',
            new_pkg_tag,
        )
    ign_match = re.search(r'\bIgnorableNamespaces\s*=\s*"([^"]*)"', new_pkg_tag)
    if ign_match is None:
        new_pkg_tag = re.sub(r">$", ' IgnorableNamespaces="rescap">', new_pkg_tag)
    else:
        tokens = ign_match.group(1).split()
        if "rescap" not in tokens:
            tokens.append("rescap")
            new_pkg_tag = new_pkg_tag.replace(
                ign_match.group(0), f'IgnorableNamespaces="{" ".join(tokens)}"'
            )
    text = text.replace(pkg_tag, new_pkg_tag, 1)

    # Update <Identity ...>
    id_match = re.search(r"<Identity\b[^>]*>", text)
    if not id_match:
        raise Problem("no <Identity> element")
    id_tag = id_match.group(0)
    new_id_tag = re.sub(r'\bName\s*=\s*"[^"]*"', f'Name="{spec["identity"]}"', id_tag)
    new_id_tag = re.sub(r'\bPublisher\s*=\s*"[^"]*"', f'Publisher="{publisher}"', new_id_tag)
    new_id_tag = re.sub(r'\bVersion\s*=\s*"[^"]*"', f'Version="{version}"', new_id_tag)
    text = text.replace(id_tag, new_id_tag, 1)

    # Update <mp:PhoneIdentity PhoneProductId="..."> if present
    phone_match = re.search(r"<mp:PhoneIdentity\b[^>]*>", text)
    if phone_match:
        new_phone = re.sub(
            r'\bPhoneProductId\s*=\s*"[^"]*"',
            f'PhoneProductId="{spec["phone_id"]}"',
            phone_match.group(0),
        )
        text = text.replace(phone_match.group(0), new_phone, 1)

    # Update <Properties><DisplayName>
    text, count = re.subn(
        r"(<Properties\b[^>]*>.*?<DisplayName>)([^<]*)(</DisplayName>)",
        rf"\g<1>{spec['display_name']}\g<3>",
        text,
        count=1,
        flags=re.S,
    )
    if count != 1:
        raise Problem("could not update <Properties><DisplayName>")

    # Update <uap:VisualElements DisplayName="..." Description="...">
    ve_match = re.search(r"<uap:VisualElements\b[^>]*>", text, re.S)
    if not ve_match:
        raise Problem("no <uap:VisualElements> element")
    ve_tag = ve_match.group(0)
    new_ve = re.sub(r'\bDisplayName\s*=\s*"[^"]*"', f'DisplayName="{spec["display_name"]}"', ve_tag)
    new_ve = re.sub(r'\bDescription\s*=\s*"[^"]*"', f'Description="{spec["description"]}"', new_ve)
    text = text.replace(ve_tag, new_ve, 1)

    # Rebuild <Capabilities> cleanly so switching variants on the same file is idempotent.
    newline = "\r\n" if "\r\n" in text else "\n"
    caps_match = re.search(r"<Capabilities\b[^>]*>.*?</Capabilities>", text, re.S)
    if not caps_match:
        raise Problem("no <Capabilities> element")
    cap_lines = [
        "  <Capabilities>",
        '    <Capability Name="internetClient" />',
        '    <rescap:Capability Name="inputInjectionBrokered" />',
    ]
    for cap in spec["extra_rescap"]:
        cap_lines.append(f'    <rescap:Capability Name="{cap}" />')
    cap_lines.append("  </Capabilities>")
    new_caps = newline.join(cap_lines)
    text = text[: caps_match.start()] + new_caps.lstrip() + text[caps_match.end() :]

    ET.fromstring(text)
    return text


def find_package(path: str) -> str:
    if os.path.isfile(path):
        return path
    found = []
    for directory, _, files in os.walk(path):
        if os.sep + "dependencies" + os.sep in (directory + os.sep).lower():
            continue
        for name in files:
            if name.lower().endswith((".msix", ".appx")):
                found.append(os.path.join(directory, name))
    if len(found) != 1:
        raise Problem(f"found {len(found)} packages under {path}, expected 1")
    return found[0]


def inspect_package(path: str):
    with zipfile.ZipFile(path) as zf:
        names = [n for n in zf.namelist() if not n.endswith("/")]
        manifest_bytes = None
        exes = {}
        for name in names:
            low = name.lower()
            if low == "appxmanifest.xml":
                manifest_bytes = zf.read(name)
            elif low.endswith((".exe", ".dll")):
                exes[name] = hashlib.sha256(zf.read(name)).hexdigest()
    if manifest_bytes is None:
        raise Problem(f"{path} has no AppxManifest.xml")
    root = ET.fromstring(manifest_bytes)
    identity_el = root.find(f"{{{FOUNDATION}}}Identity")
    if identity_el is None:
        raise Problem(f"{path} manifest has no Identity")
    caps = []
    for parent in root.iter(f"{{{FOUNDATION}}}Capabilities"):
        for child in parent:
            ns = child.tag[1:].split("}")[0] if child.tag.startswith("{") else ""
            caps.append((ns, child.get("Name")))
    lower_names = {n.lower() for n in names}
    return {
        "identity": identity_el.get("Name", ""),
        "version": identity_el.get("Version", ""),
        "publisher": identity_el.get("Publisher", ""),
        "caps": caps,
        "exes": exes,
        "signed": "appxsignature.p7x" in lower_names and "appxblockmap.xml" in lower_names,
    }


def command_stamp(args) -> int:
    with open(args.manifest, "rb") as f:
        raw = f.read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw[3:].decode("utf-8") if bom else raw.decode("utf-8")
    updated = stamp_manifest_text(text, args.variant, args.version, args.publisher)
    with open(args.manifest, "wb") as f:
        f.write((b"\xef\xbb\xbf" if bom else b"") + updated.encode("utf-8"))
    spec = VARIANTS[args.variant]
    print(
        f"make_variants.py: stamped {args.manifest} -> {spec['identity']} "
        f"({spec['display_name']}) v{args.version} extra_rescap={spec['extra_rescap']}"
    )
    return 0


def command_check(args) -> int:
    base_pkg = find_package(args.base_dir)
    var_pkg = find_package(args.variant_dir)
    base = inspect_package(base_pkg)
    var = inspect_package(var_pkg)
    spec = VARIANTS[args.variant]

    problems = []
    if not var["signed"]:
        problems.append(f"{os.path.basename(var_pkg)} is not signed")
    if var["identity"] != spec["identity"]:
        problems.append(f"expected identity {spec['identity']}, got {var['identity']}")
    if args.variant != "plain" and var["identity"] == base["identity"]:
        problems.append("variant shares identity with plain; cannot install side by side")
    expected_caps = list(BASE_CAPABILITIES) + [(RESCAP, c) for c in spec["extra_rescap"]]
    if var["caps"] != expected_caps:
        problems.append(f"expected capabilities {expected_caps}, got {var['caps']}")
    if base["exes"] != var["exes"]:
        problems.append(f"executables differ between plain ({base['exes']}) and {args.variant} ({var['exes']})")
    if not var["exes"]:
        problems.append("no .exe/.dll found in package")

    lines = [
        f"variant: {args.variant} ({os.path.basename(var_pkg)})",
        f"identity: {var['identity']} v{var['version']}",
        f"capabilities: {', '.join(name for _, name in var['caps'])}",
        "programs (sha256, identical to plain):" if not problems else "programs (sha256):",
    ] + [f"  {digest}  {name}" for name, digest in sorted(var["exes"].items())]
    lines += [f"PROBLEM: {p}" for p in problems]

    text = "\n".join(lines) + "\n"
    print(text, end="")
    if args.note:
        with open(args.note, "w", encoding="utf-8") as f:
            f.write(text)
    return 1 if problems else 0


SELF_TEST_MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<Package
  xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
  xmlns:mp="http://schemas.microsoft.com/appx/2014/phone/manifest"
  xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
  xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
  IgnorableNamespaces="uap mp rescap">
  <Identity Name="XboxConsoleProfiler.Plain" Publisher="CN=Test" Version="0.1.0.0" />
  <mp:PhoneIdentity PhoneProductId="00000000-0000-0000-0000-000000000001" PhonePublisherId="00000000-0000-0000-0000-000000000000" />
  <Properties>
    <DisplayName>Xbox Profiler (Plain)</DisplayName>
    <PublisherDisplayName>Test</PublisherDisplayName>
  </Properties>
  <Applications>
    <Application Id="App" Executable="$targetnametoken$.exe" EntryPoint="XboxConsoleProfiler.App">
      <uap:VisualElements DisplayName="Xbox Profiler (Plain)" Description="Base" BackgroundColor="#181E26" />
    </Application>
  </Applications>
  <Capabilities>
    <Capability Name="internetClient" />
    <rescap:Capability Name="inputInjectionBrokered" />
  </Capabilities>
</Package>
"""


def self_test() -> int:
    seen_identities = set()
    with tempfile.TemporaryDirectory() as tmp:
        packages = {}
        current = SELF_TEST_MANIFEST
        # Stamp sequentially on the same text to verify idempotency across variants.
        for name in ("plain", "hevc", "game", "expanded"):
            current = stamp_manifest_text(current, name, "0.1.42.0", "CN=Test")
            pkg_path = os.path.join(tmp, f"{name}.msix")
            with zipfile.ZipFile(pkg_path, "w") as zf:
                zf.writestr("AppxManifest.xml", current.encode("utf-8"))
                zf.writestr("XboxConsoleProfiler.exe", b"MZ_IDENTICAL_EXE")
                zf.writestr("AppxBlockMap.xml", "bm")
                zf.writestr("AppxSignature.p7x", "sig")
            packages[name] = pkg_path
            info = inspect_package(pkg_path)
            assert info["identity"] == VARIANTS[name]["identity"]
            assert info["identity"] not in seen_identities
            seen_identities.add(info["identity"])

        for name in ("plain", "hevc", "game", "expanded"):
            ns = argparse.Namespace(base_dir=packages["plain"], variant=name, variant_dir=packages[name], note=None)
            rc = command_check(ns)
            assert rc == 0, f"check failed for {name}"

        # Tampered executable must fail check.
        bad_pkg = os.path.join(tmp, "bad.msix")
        with zipfile.ZipFile(bad_pkg, "w") as zf:
            zf.writestr(
                "AppxManifest.xml",
                stamp_manifest_text(SELF_TEST_MANIFEST, "hevc", "0.1.42.0", "CN=Test").encode("utf-8"),
            )
            zf.writestr("XboxConsoleProfiler.exe", b"MZ_DIFFERENT_EXE")
            zf.writestr("AppxBlockMap.xml", "bm")
            zf.writestr("AppxSignature.p7x", "sig")
        rc = command_check(argparse.Namespace(base_dir=packages["plain"], variant="hevc", variant_dir=bad_pkg, note=None))
        assert rc == 1, "tampered exe should have failed"

    print("make_variants.py self-test: PASS (4 distinct side-by-side variants verified)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_stamp = sub.add_parser("stamp")
    p_stamp.add_argument("--variant", required=True, choices=list(VARIANTS.keys()))
    p_stamp.add_argument("--version", required=True)
    p_stamp.add_argument("--publisher", required=True)
    p_stamp.add_argument("manifest")

    p_check = sub.add_parser("check")
    p_check.add_argument("--base-dir", required=True)
    p_check.add_argument("--variant", required=True, choices=list(VARIANTS.keys()))
    p_check.add_argument("--variant-dir", required=True)
    p_check.add_argument("--note")

    sub.add_parser("self-test")

    args = parser.parse_args()
    try:
        if args.cmd == "stamp":
            return command_stamp(args)
        if args.cmd == "check":
            return command_check(args)
        return self_test()
    except Problem as e:
        print(f"make_variants.py: ERROR: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
