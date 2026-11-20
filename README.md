# xbox-console-profiler

A small UWP C++/DirectX test app that measures what an app can actually use on an Xbox Series X|S in Developer Mode, so video playback projects can tune for smooth decoding and rendering.

## What it measures

Every benchmark runs in both console input states — **active** (right after input) and **idle** (`>12 s` without input, default `15 s`) — recording `ms_since_last_input` on every sample:

1. **System & topology info**
   - Package identity, OS version, CPU vendor/brand/ISA (`AVX`/`AVX2`/`FMA`), `GetSystemCpuSetInformation` entries (`Id`, `LogicalProcessorIndex`, `CoreIndex`, `LastLevelCacheIndex`, `AllocatedToTargetProcess`, `Parked`).
   - `Windows::System::MemoryManager` (`AppMemoryUsage`, `AppMemoryUsageLimit`, `AppMemoryUsageLevel`, `PrivateCommitUsage`, `TotalCommitUsage`, `TotalCommitLimit`, `PrivateWorkingSetUsage`, memory-pressure event counts).
   - Xbox extended memory pools via `Windows.Xbox.ApplicationModel.ApplicationResources` (`IApplicationResourcesStatics2::GetExtendedMemoryInfo`).
   - DXGI adapter properties, `IDXGIAdapter3::QueryVideoMemoryInfo` (`LOCAL` and `NON_LOCAL` budget, usage, reservation), and D3D12 architecture (`UMA`, `CacheCoherentUMA`, max feature level, shader model).
2. **CPU**
   - Single-thread serial dependency chains (integer and FP64) to track CPU clock scaling between active and idle states.
   - Per-CPU-set pinning (`SetThreadSelectedCpuSets` + `GetCurrentProcessorNumberEx`) to verify which cores can be pinned and whether any are throttled or rejected.
   - Multi-thread ALU throughput scaling across `1, 2, 4, 6, 8` (up to `16`) threads while sampling `GetCurrentProcessorNumberEx` to count how many distinct logical processors execute concurrently.
3. **CPU cache & RAM bandwidth**
   - Across `1, 2, 4, 6, 8` threads: L2-resident copy (`128 KiB` src/dst per thread), `64 MiB` DRAM `memcpy`, `64 MiB` DRAM SSE2 read, `64 MiB` non-temporal streaming write (`_mm_stream_si128`), and `64 MiB` streaming copy, also reporting `ms / 24.9 MB` (4K 16-bit frame reference).
4. **GPU bandwidth & time share (D3D12)**
   - `DEFAULT → DEFAULT` (`64 MiB`) and `UPLOAD → DEFAULT` copy bandwidth (plus CPU fill/read speed on the mapped upload buffer).
   - On cache-coherent UMA hardware: `D3D12_HEAP_TYPE_CUSTOM` (`WRITE_BACK`, `L0`) zero-copy heap test (create, persistent map, CPU fill/read, GPU copy).
   - Compute shader (`cs_6_0`) memory copy bandwidth (`64 MiB`).
   - **GPU time share / compute throughput**: fixed register-only FP32 FMA compute workload (`cs_alu`, `34.36 GFLOPs`/dispatch on hardware) run in both:
     - **Burst** (20 back-to-back dispatches with per-dispatch timestamps, reporting `min/p50/mean/p95/max`, `min/mean`, inter-dispatch gaps, GPU span vs wall time, and TFLOP/s).
     - **Sustained pipelined** (32 double-buffered submissions keeping the queue saturated, reporting total `sum_gpu_ms / wall_ms`, per-submission latency distribution, and sustained wall TFLOP/s).
5. **Memory limits** (stepped in `100 MiB` increments by default)
   - `cpu`: native page-touched allocations (`_aligned_malloc` + 4 KiB page write).
   - `d3d11`: `DEFAULT` `RGBA8` textures (up to `4096×4096` = `64 MiB` each), cleared and fenced to force residency, plus a timed fixed `64 MiB` GPU copy at every step to spot paging (`>5×` slowdown).
   - `d3d12`: same stepped residency + fixed `64 MiB` GPU timestamped copy probe.
   - `mixed_d3d11` and `mixed_d3d12`: holds touched CPU memory (`min(1000 MiB, 40% of AppMemoryUsageLimit)` by default) while stepping D3D11/D3D12 textures.
   - Every step flushes immediately to the text log with `AppMemoryUsage/Limit/Level`, Xbox extended pools, and DXGI `LOCAL`/`NON_LOCAL` budget + usage + reservation.

## Package variants (side by side)

CI builds four signed x64 `.msix` packages from the **exact same compiled `XboxConsoleProfiler.exe`** (verified byte-for-byte via SHA-256), each with a distinct package identity and display name so all four can stay installed on the console at once:

| Variant | Identity Name | Display Name | Extra Capabilities | Purpose |
| --- | --- | --- | --- | --- |
| `plain` | `XboxConsoleProfiler.Plain` | Xbox Profiler (Plain) | *(none beyond base)* | Standard UWP App baseline (~1 GiB) |
| `hevc` | `XboxConsoleProfiler.Hevc` | Xbox Profiler (hevcPlayback) | `rescap:hevcPlayback` | Media app memory budget (~3.25 GiB) |
| `game` | `XboxConsoleProfiler.Game` | Xbox Profiler (Game) | *(none beyond base)* | Dedicated install slot to flip **App type → Game** in Dev Home |
| `expanded` | `XboxConsoleProfiler.Expanded` | Xbox Profiler (expandedResources) | `rescap:expandedResources` | Dev-only check of whether the deprecated capability alone changes limits without Dev Home's switch |

All variants declare `internetClient` and `rescap:inputInjectionBrokered` so the app can inject a harmless `Shift` key via `InputInjector` to wake the console between idle and active phases without UI side effects (never `F24`, which triggers the Xbox Capture overlay). If `InputInjector` is unavailable, the app falls back to waiting for manual controller/keyboard input and still logs `ms_since_last_input`.

### Setting App Type to Game (Dev Home)

Sideloaded UWP manifests cannot directly set the console's **Game** app type (Store submissions use Partner Center metadata). To measure **Game** mode:

1. Install the `game` variant (`XboxConsoleProfiler.Game`).
2. On the Xbox, open **Dev Home** and highlight **Xbox Profiler (Game)** under **Games & apps**.
3. Press the **View** button (two overlapping squares) → **View details**.
4. Change **App type** from **App** to **Game**, then press **B** to save.
5. Keep `plain` and `hevc` set to **App** so all three can be compared cleanly side by side.

## Building (CI)

`.github/workflows/build.yml` runs on `windows-latest` (`v145`, Windows SDK `10.0.26100.0`, `MinVersion` `10.0.17763.0`, Release x64):

- Runs `tools/make_variants.py self-test`.
- Builds and signs all four `.msix` variants with a throwaway per-run self-signed certificate (`XboxConsoleProfiler.cer`).
- Verifies that all four `.msix` packages contain the exact same `.exe` (SHA-256) and pass `tools/check_imports.py` against `tools/uwp-dlls-allow.txt` (no desktop `kernel32.dll` or desktop CRT imports).
- Compiles and runs `tests/warp_smoke` on WARP and validates its generated JSON report.

Download the CI artifact:

```sh
gh run list --workflow build.yml --limit 5
gh run download <run-id> --name xbox-console-profiler-all-x64-<run-id> --dir ./pkg
```

## Installing, running, and fetching results via Device Portal

> **Before running on a shared dev console:** stop other apps (such as Bale or `rav1g` test player) first so background memory or GPU/CPU activity does not skew measurements.

### Using `tools/portal.sh`

Set the console address via environment variable or a local file outside the repo (never commit console IPs):

```sh
export XBOX_HOST=<console-ip>
# or:
mkdir -p ~/.config/xbox-console-profiler
echo "<console-ip>" > ~/.config/xbox-console-profiler/host
```

Install any variant(s), check running processes, launch, and download results:

```sh
# 1. Confirm other test apps are stopped
tools/portal.sh procs

# 2. Install variants side by side
tools/portal.sh install plain    ./pkg/plain
tools/portal.sh install hevc     ./pkg/hevc
tools/portal.sh install game     ./pkg/game
tools/portal.sh install expanded ./pkg/expanded

# 3. Launch (auto-runs full active + idle suite and exits cleanly when done)
tools/portal.sh launch plain

# 4. Check status until DONE
tools/portal.sh status plain

# 5. Fetch LocalState\results (plain text + JSON)
tools/portal.sh results plain ./results-plain
```

You can also install through the Device Portal web UI (`https://<console-ip>:11443`): upload the variant's `.msix`, `XboxConsoleProfiler.cer`, and the x64 framework packages from `packages/*/Dependencies/x64/`.

### Optional configuration (`LocalState\args\config.txt`)

If `LocalState\args\config.txt` is absent, the app runs the full default suite (`active` then `idle`, `15 s` idle wait, `100 MiB` memory steps, safe stop `32 MiB` below `AppMemoryUsageLimit`). To customize a run, upload a `key=value` file with `tools/portal.sh config <variant> my-config.txt`:

```ini
# Comma-separated subset or "all":
# cpu, bandwidth, gpu, memory, mem_cpu, mem_d3d11, mem_d3d12, mem_mixed_d3d11, mem_mixed_d3d12
tests=all
states=both          # both | active | idle
wake_mode=auto       # auto | inject | manual | none
idle_wait_sec=15     # wait >12 s for Xbox input-idle state
manual_wait_sec=20
mem_step_mib=100
mem_max_mib=16384
mixed_cpu_mib=0      # 0 = auto: min(1000, 40% of AppMemoryUsageLimit)
allow_over_limit=0   # 1 = keep allocating past AppMemoryUsageLimit (may trigger OS kill)
exit_on_done=1
```

### Output files (`LocalState\results\`)

- `profile_<YYYYMMDD_HHMMSS>_<variant>.txt` and `latest.txt` — human-readable log flushed after every test and memory step.
- `profile_<YYYYMMDD_HHMMSS>_<variant>.json` and `latest.json` — structured JSON report updated after every phase.
- `status.txt` — current phase (`RUNNING ...`, `WAITING_FOR_IDLE`, `DONE`, or `CANCELLED`).
