# Measured results

Xbox Series X, Developer Mode, OS 10.0.26100.9673. Run 2026-11-20 with build 0.1.2.0 (CI run
44741781695, commit 9608bb3). Bale and rav1g's test player were stopped. Each variant ran once with the
default config: an "active" phase right after an injected Shift key (InputInjector worked), then an
"idle" phase after 15 s without input. Raw reports: [raw/](raw/).

The `game` variant (Dev Home App type = Game) was not run; the owner does not plan to use Game mode.

## Summary

| | plain | hevcPlayback | expandedResources |
|---|---|---|---|
| App memory limit (one pool, CPU + GPU) | 1024 MiB | 3328 MiB | 5120 MiB |
| DXGI local budget | 1210 MiB | 2857 MiB | 4147 MiB |
| D3D11 / D3D12 resident textures (max) | 792 / 928 MiB, stopped by app limit | 2828 / 2828 MiB, then E_OUTOFMEMORY | 4128 / 4128 MiB, then E_OUTOFMEMORY |
| CPU memory (probe stops ~300 MiB short of the limit) | 726 MiB | 3022 MiB | 4800 MiB |
| Logical CPUs the app runs on | 8 (4 cores, shared with the system) | 8 (4 cores, shared) | 6 (3 cores, dedicated CPU sets 258-263) |
| CPU ALU, 1 / 4 / 8 threads, active | 13.8 / 54.4 / 57.0 Gops/s | 13.7 / 49.7 / 56.9 | 13.6 / 41.7 / 37.4 |
| CPU ALU, 1 / 8 threads, idle | 10.8 / 45.4 | 10.8 / 45.5 | 13.8 / 38.6 |
| CPU RAM copy, 1 / 4 threads, active | 12.7 / 23.4 GB/s | 14.1 / 23.6 | 15.4 / 25.5 |
| CPU RAM copy, 1 / 4 threads, idle | 3.4 / 6.5 GB/s | 3.6 / 6.4 | 14.9 / 25.2 |
| CPU cache-resident copy, 1 thread, active / idle | 56 / 45 GB/s | 57 / 45 | 57 / 57 |
| GPU copy DEFAULT->DEFAULT, active / idle | 97 / 40 GB/s | 89 / 38 | 144 / 144 |
| GPU upload->default copy, active / idle | 68 / 37 GB/s | 69 / 37 | 78 / 78 |
| GPU ALU (sustained compute), active and idle | 1.86 TFLOP/s | 1.85 TFLOP/s | 11.25 TFLOP/s |
| GPU busy / wall time under sustained load | 99.8% | 99.8% | 99.8% |

## Findings

1. **Memory.** The hevcPlayback numbers match Bale's memprobe exactly (3328 MiB app limit, 2857 MiB DXGI
   budget, 2828 MiB in D3D11 and D3D12, no paging slowdown before E_OUTOFMEMORY). Without the capability
   the app limit is 1024 MiB and it, not the 1210 MiB DXGI budget, is what stops GPU allocations.
   `expandedResources` raises the limit to 5120 MiB (DXGI budget 4147 MiB). In every variant the
   fixed-size GPU copy stayed within 1.05x of its baseline up to the limit, so there was no paging.
2. **GPU share.** A normal app (plain or hevcPlayback) gets about 1.86 TFLOP/s of compute, about 1/6 of
   what an `expandedResources` app gets (11.25 TFLOP/s, close to the Series X's 12.15 peak). The app's GPU
   queue is never preempted (busy 99.8% of wall time); it just runs on a smaller GPU slice. GPU copies
   are also faster with `expandedResources` (144 vs 89-97 GB/s).
3. **Idle state.** For normal apps, ~10 s without input slows CPU ALU by ~21%, CPU RAM copies 3.6x or
   more, cache copies ~20%, and **GPU copies ~2.4x** (97 -> 40 GB/s). GPU compute speed is unchanged.
   The GPU copy slowdown contradicts the earlier note that GPU copies are not slowed (~31 GB/s). The
   earlier measurement likely ran with less work or a different copy path; these runs repeat it across
   two variants and three copy kinds. With `expandedResources` there is no idle slowdown at all.
4. **CPU cores.** A normal app's threads can run on all 8 logical CPUs (4 cores with SMT, shared with the
   system); throughput stops growing beyond 4 threads (4.1x one thread at 8 threads). With
   `expandedResources` the app is given 3 dedicated cores (6 logical CPUs, sets 258-263); 4 threads give
   3.1x, and 8 threads are slower than 4.

## Caveats

- One run per variant. The idle-phase numbers are from the first ~30 s of idle.
- `expandedResources` is a restricted capability meant for games. It worked for a sideloaded UWP app in
  Developer Mode here; whether it is usable in the owner's other apps (Store/retail policy, side effects on
  other running apps, suspend behaviour) is not tested.
- `cpu_1t_read_gbps` in the GPU UMA lines reports impossible values (millions of GB/s); the compiler
  probably removed that read loop. Other numbers are not affected.
- "CPU memory" stops ~300 MiB below the app limit on purpose (`allow_over_limit=0`), and the app itself
  uses ~265 MiB for D3D devices before the probe starts.
