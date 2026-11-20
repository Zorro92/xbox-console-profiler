# xbox-console-profiler

A small UWP test app that measures what an app can actually use on an Xbox Series X|S in Developer Mode:
memory limits (CPU, D3D11, D3D12, mixed), CPU cores and clock, cache and RAM bandwidth, GPU bandwidth and
time share, and how each changes in the console's input-idle state. It builds in several package variants
(with and without `hevcPlayback`, App and Game type) so their differences can be measured side by side.
