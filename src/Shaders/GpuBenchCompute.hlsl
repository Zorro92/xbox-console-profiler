// Compute shaders for D3D12 GPU bandwidth and GPU time-share / ALU benchmarks.
// Compiled with DXC cs_6_0 into C headers at build time.
// Uses pure FP32 / uint32 (no FP64) for full compatibility across Xbox RDNA2 and WARP.

cbuffer CopyParams : register(b0) {
  uint g_count_vec4;
  uint g_iterations;
  float g_k_mul;
  float g_k_add;
};

ByteAddressBuffer g_src : register(t0);
RWByteAddressBuffer g_dst : register(u0);

[numthreads(256, 1, 1)]
void cs_copy(uint3 dtid : SV_DispatchThreadID) {
  uint idx = dtid.x;
  if (idx < g_count_vec4) {
    uint byte_off = idx * 16u;
    uint4 v = g_src.Load4(byte_off);
    g_dst.Store4(byte_off, v);
  }
}

[numthreads(256, 1, 1)]
void cs_alu(uint3 dtid : SV_DispatchThreadID) {
  uint tid = dtid.x;
  float s = frac(float(tid + 1u) * 0.0001234567f);
  float4 a = float4(s, s + 0.1f, -s, 0.25f - s);
  float4 b = float4(0.3f - s, s + 0.2f, 0.4f - s, -0.15f + s);
  float4 c = float4(-0.2f + s, 0.35f - s, s + 0.05f, -0.3f + s);
  float4 d = float4(0.15f + s, -0.25f + s, -0.1f - s, 0.45f - s);

  // Runtime root constants keep every accumulator strictly bounded in [-1, 1]
  // while preventing compile-time constant folding.
  // Each loop iteration performs 8 float4 mad ops = 8 * 4 * 2 = 64 FLOPs per thread.
  float4 k0 = float4(g_k_mul, g_k_mul * 0.9997f, g_k_mul * 0.9993f, g_k_mul * 0.9989f);
  float4 k1 = float4(g_k_mul * 0.9991f, g_k_mul * 0.9987f, g_k_mul, g_k_mul * 0.9995f);
  float4 c0 = float4(g_k_add, -g_k_add, g_k_add * 0.75f, -g_k_add * 0.5f);
  float4 c1 = float4(-g_k_add * 0.6f, g_k_add * 0.8f, -g_k_add * 0.9f, g_k_add * 0.4f);

  [loop]
  for (uint i = 0; i < g_iterations; ++i) {
    a = mad(a, k0, c0);
    b = mad(b, k1, c1);
    c = mad(c, k0, c1);
    d = mad(d, k1, c0);
    a = mad(a, k1, c1);
    b = mad(b, k0, c0);
    c = mad(c, k1, c0);
    d = mad(d, k0, c1);
  }

  float4 acc = (a + b) + (c + d);
  // Write only the first 256 threads to keep memory traffic near zero while
  // ensuring the entire ALU loop stays live.
  if (tid < g_count_vec4) {
    g_dst.Store4(tid * 16u, asuint(acc));
  }
}
