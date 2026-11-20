#!/usr/bin/env bash
# Windows Device Portal helper for xbox-console-profiler.
#
# Console host resolution (never hardcode IPs in this repo):
#   1. XBOX_HOST environment variable (e.g. XBOX_HOST=<console-ip>)
#   2. ~/.config/xbox-console-profiler/host (single line containing host/IP)
# Optional Device Portal Basic Auth if enabled on console:
#   XBOX_USER and XBOX_PASS environment variables.
#
# Usage:
#   portal.sh procs
#   portal.sh packages
#   portal.sh install   <plain|hevc|game|expanded> <artifact-dir>
#   portal.sh uninstall <plain|hevc|game|expanded>
#   portal.sh config    <plain|hevc|game|expanded> <local-config.txt>
#   portal.sh launch    <plain|hevc|game|expanded>
#   portal.sh stop      <plain|hevc|game|expanded>
#   portal.sh status    <plain|hevc|game|expanded>
#   portal.sh results   <plain|hevc|game|expanded> [out-dir]
#   portal.sh ls        <plain|hevc|game|expanded> <LocalState-subpath>

set -euo pipefail

HOST="${XBOX_HOST:-}"
if [ -z "$HOST" ] && [ -f "$HOME/.config/xbox-console-profiler/host" ]; then
  HOST="$(tr -d ' \t\r\n' < "$HOME/.config/xbox-console-profiler/host")"
fi
if [ -z "$HOST" ]; then
  echo "error: set XBOX_HOST=<ip-or-host> or write it to ~/.config/xbox-console-profiler/host" >&2
  exit 2
fi

PORT="${XBOX_PORT:-11443}"
BASE="https://$HOST:$PORT"
TMPDIR_WORK="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_WORK"' EXIT
JAR="$TMPDIR_WORK/cookies"

CURL=(curl -sSk --cookie-jar "$JAR" --cookie "$JAR")
if [ -n "${XBOX_USER:-}" ]; then
  CURL+=(-u "$XBOX_USER:${XBOX_PASS:-}")
fi

"${CURL[@]}" -f -o /dev/null "$BASE/api/os/info"
CSRF="$(awk '$6 == "CSRF-Token" { print $7 }' "$JAR")"

enc() { jq -rn --arg v "$1" '$v|@uri'; }
b64uri() { printf %s "$1" | base64 -w0 | jq -sRr @uri; }

variant_prefix() {
  case "$1" in
    plain)    echo "XboxConsoleProfiler.Plain_" ;;
    hevc)     echo "XboxConsoleProfiler.Hevc_" ;;
    game)     echo "XboxConsoleProfiler.Game_" ;;
    expanded) echo "XboxConsoleProfiler.Expanded_" ;;
    *) echo "error: unknown variant '$1' (expected plain|hevc|game|expanded)" >&2; exit 2 ;;
  esac
}

pkg_json() {
  local prefix
  prefix="$(variant_prefix "$1")"
  "${CURL[@]}" -f "$BASE/api/app/packagemanager/packages" \
    | jq -c --arg p "$prefix" \
      '[.InstalledPackages[] | select(.PackageFullName | startswith($p))]
       | sort_by([.Version.Major, .Version.Minor, .Version.Build, .Version.Revision])
       | last // empty'
}

require_pkg() {
  local p
  p="$(pkg_json "$1")"
  if [ -z "$p" ]; then
    echo "error: variant '$1' is not installed on console" >&2
    exit 1
  fi
  printf '%s\n' "$p"
}

fs_query() {
  local p="$1"
  local subpath="$2"
  local full
  full="$(jq -r .PackageFullName <<<"$p")"
  # Device Portal needs a leading backslash (\LocalState\...), else it appends the
  # path to the package folder name without a separator and finds nothing.
  case "$subpath" in \\*) ;; *) subpath="\\$subpath" ;; esac
  echo "knownfolderid=LocalAppData&packagefullname=$(enc "$full")&path=$(enc "$subpath")"
}

cmd="${1:-}"
case "$cmd" in
  procs)
    "${CURL[@]}" -f "$BASE/api/resourcemanager/processes" \
      | jq -r '.Processes[] | select(.IsRunning != false) | "\(.ImageName)\t\(.CPUUsage)\t\(.PackageFullName // "")"' \
      | sort -u
    ;;

  packages)
    "${CURL[@]}" -f "$BASE/api/app/packagemanager/packages" \
      | jq -r '.InstalledPackages[] | select(.PackageFullName | startswith("XboxConsoleProfiler.")) | "\(.PackageFullName)\t\(.Name)"' \
      | sort
    ;;

  install)
    [ $# -ge 3 ] || { echo "usage: portal.sh install <variant> <artifact-dir>" >&2; exit 2; }
    variant="$2"
    pkgdir="$3"
    prefix="$(variant_prefix "$variant")"
    msix="$(find "$pkgdir" -name '*.msix' -not -path '*/Dependencies/*' | grep -i "${prefix%_}" | head -n1 || true)"
    if [ -z "$msix" ]; then
      msix="$(find "$pkgdir" -name '*.msix' -not -path '*/Dependencies/*' | head -n1)"
    fi
    cer="$(find "$pkgdir" -name '*.cer' | head -n1)"
    [ -n "$msix" ] && [ -n "$cer" ] || { echo "error: could not find .msix and .cer in $pkgdir" >&2; exit 1; }
    FORM=(-F "package=@$msix" -F "certificate=@$cer")
    i=0
    while IFS= read -r dep; do
      FORM+=(-F "dependency$i=@$dep")
      i=$((i + 1))
    done < <(find "$(dirname "$msix")" -path '*/Dependencies/x64/*' \( -name '*.appx' -o -name '*.msix' \))
    "${CURL[@]}" -f -o /dev/null -H "X-CSRF-Token: $CSRF" "${FORM[@]}" \
      "$BASE/api/app/packagemanager/package?package=$(enc "$(basename "$msix")")"
    n=0
    while true; do
      code="$("${CURL[@]}" -o "$TMPDIR_WORK/state" -w '%{http_code}' "$BASE/api/app/packagemanager/state")"
      case "$code" in
        204) sleep 2 ;;
        404) n=$((n + 1)); [ "$n" -lt 15 ] || { echo "install never started" >&2; exit 1; }; sleep 1 ;;
        200) break ;;
        *) echo "install failed ($code): $(cat "$TMPDIR_WORK/state")" >&2; exit 1 ;;
      esac
    done
    sleep 2
    require_pkg "$variant" | jq -c '{PackageFullName, Version}'
    ;;

  uninstall)
    [ $# -ge 2 ] || { echo "usage: portal.sh uninstall <variant>" >&2; exit 2; }
    p="$(pkg_json "$2")"
    if [ -z "$p" ]; then
      echo "variant '$2' is not installed"
      exit 0
    fi
    full="$(jq -r .PackageFullName <<<"$p")"
    "${CURL[@]}" -o /dev/null -w '%{http_code}\n' -X DELETE -H "X-CSRF-Token: $CSRF" \
      "$BASE/api/app/packagemanager/package?package=$(enc "$full")"
    ;;

  config)
    [ $# -ge 3 ] || { echo "usage: portal.sh config <variant> <local-config.txt>" >&2; exit 2; }
    p="$(require_pkg "$2")"
    cfg="$3"
    [ -f "$cfg" ] || { echo "error: config file not found: $cfg" >&2; exit 1; }
    "${CURL[@]}" -o /dev/null --data "" -H "X-CSRF-Token: $CSRF" \
      "$BASE/api/filesystem/apps/folder?$(fs_query "$p" 'LocalState')&newfoldername=args" || true
    cp "$cfg" "$TMPDIR_WORK/config.txt"
    "${CURL[@]}" -f -o /dev/null -w '%{http_code}\n' -H "X-CSRF-Token: $CSRF" \
      -F "file=@$TMPDIR_WORK/config.txt" \
      "$BASE/api/filesystem/apps/file?$(fs_query "$p" 'LocalState\args')"
    ;;

  launch)
    [ $# -ge 2 ] || { echo "usage: portal.sh launch <variant>" >&2; exit 2; }
    p="$(require_pkg "$2")"
    "${CURL[@]}" -f -o /dev/null --data '' -H "X-CSRF-Token: $CSRF" \
      "$BASE/api/taskmanager/app?appid=$(b64uri "$(jq -r .PackageRelativeId <<<"$p")")&package=$(b64uri "$(jq -r .PackageFullName <<<"$p")")"
    echo "launched $2"
    ;;

  stop)
    [ $# -ge 2 ] || { echo "usage: portal.sh stop <variant>" >&2; exit 2; }
    p="$(require_pkg "$2")"
    "${CURL[@]}" -o /dev/null -w '%{http_code}\n' -X DELETE -H "X-CSRF-Token: $CSRF" \
      "$BASE/api/taskmanager/app?package=$(b64uri "$(jq -r .PackageFullName <<<"$p")")"
    ;;

  status)
    [ $# -ge 2 ] || { echo "usage: portal.sh status <variant>" >&2; exit 2; }
    p="$(require_pkg "$2")"
    "${CURL[@]}" -f "$BASE/api/filesystem/apps/file?$(fs_query "$p" 'LocalState\results')&filename=status.txt"
    ;;

  ls)
    [ $# -ge 3 ] || { echo "usage: portal.sh ls <variant> <LocalState-subpath>" >&2; exit 2; }
    p="$(require_pkg "$2")"
    "${CURL[@]}" -f "$BASE/api/filesystem/apps/files?$(fs_query "$p" "$3")" \
      | jq -c '.Items[] | {Name, Type, FileSize}'
    ;;

  results)
    [ $# -ge 2 ] || { echo "usage: portal.sh results <variant> [out-dir]" >&2; exit 2; }
    variant="$2"
    outdir="${3:-./results-$variant}"
    mkdir -p "$outdir"
    p="$(require_pkg "$variant")"
    items="$("${CURL[@]}" -f "$BASE/api/filesystem/apps/files?$(fs_query "$p" 'LocalState\results')")"
    echo "$items" | jq -r '.Items[] | select(.Type == 32) | .Name' | while IFS= read -r fname; do
      [ -n "$fname" ] || continue
      "${CURL[@]}" -f -o "$outdir/$fname" \
        "$BASE/api/filesystem/apps/file?$(fs_query "$p" 'LocalState\results')&filename=$(enc "$fname")"
      echo "downloaded $outdir/$fname"
    done
    ;;

  *)
    sed -n '2,22p' "$0" >&2
    exit 2
    ;;
esac
