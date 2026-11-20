#include "pch.h"
#include "ProfilerConfig.h"

namespace {

std::string Trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

std::string ToLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

bool ParseBool(const std::string& v) {
  std::string l = ToLower(Trim(v));
  return l == "1" || l == "true" || l == "yes" || l == "on";
}

uint32_t ParseU32(const std::string& v, uint32_t def, uint32_t min_v, uint32_t max_v) {
  char* end = nullptr;
  unsigned long parsed = std::strtoul(v.c_str(), &end, 10);
  if (end == v.c_str()) return def;
  if (parsed < min_v) return min_v;
  if (parsed > max_v) return max_v;
  return static_cast<uint32_t>(parsed);
}

void ApplyTestSelection(ProfilerConfig& cfg, const std::string& val) {
  std::string lower = ToLower(val);
  if (lower == "all") {
    cfg.run_sysinfo = cfg.run_cpu = cfg.run_bandwidth = cfg.run_gpu = true;
    cfg.run_mem_cpu = cfg.run_mem_d3d11 = cfg.run_mem_d3d12 = true;
    cfg.run_mem_mixed_d3d11 = cfg.run_mem_mixed_d3d12 = true;
    return;
  }
  cfg.run_sysinfo = true; // always collect basic system info
  cfg.run_cpu = false;
  cfg.run_bandwidth = false;
  cfg.run_gpu = false;
  cfg.run_mem_cpu = false;
  cfg.run_mem_d3d11 = false;
  cfg.run_mem_d3d12 = false;
  cfg.run_mem_mixed_d3d11 = false;
  cfg.run_mem_mixed_d3d12 = false;

  std::stringstream ss(lower);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = Trim(item);
    if (item == "cpu") cfg.run_cpu = true;
    else if (item == "bandwidth" || item == "bw") cfg.run_bandwidth = true;
    else if (item == "gpu") cfg.run_gpu = true;
    else if (item == "memory" || item == "mem") {
      cfg.run_mem_cpu = cfg.run_mem_d3d11 = cfg.run_mem_d3d12 = true;
      cfg.run_mem_mixed_d3d11 = cfg.run_mem_mixed_d3d12 = true;
    } else if (item == "mem_cpu") cfg.run_mem_cpu = true;
    else if (item == "mem_d3d11") cfg.run_mem_d3d11 = true;
    else if (item == "mem_d3d12") cfg.run_mem_d3d12 = true;
    else if (item == "mem_mixed") {
      cfg.run_mem_mixed_d3d11 = cfg.run_mem_mixed_d3d12 = true;
    } else if (item == "mem_mixed_d3d11") cfg.run_mem_mixed_d3d11 = true;
    else if (item == "mem_mixed_d3d12") cfg.run_mem_mixed_d3d12 = true;
  }
}

} // namespace

ProfilerConfig LoadProfilerConfig(const std::string& config_path) {
  ProfilerConfig cfg;
  if (config_path.empty()) return cfg;
  std::ifstream in(config_path);
  if (!in.is_open()) return cfg;

  cfg.source = config_path;
  std::string line;
  while (std::getline(in, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    line = Trim(line);
    if (line.empty()) continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = ToLower(Trim(line.substr(0, eq)));
    std::string val = Trim(line.substr(eq + 1));

    if (key == "tests") {
      ApplyTestSelection(cfg, val);
    } else if (key == "states") {
      std::string l = ToLower(val);
      if (l == "active") {
        cfg.run_active = true;
        cfg.run_idle = false;
      } else if (l == "idle") {
        cfg.run_active = false;
        cfg.run_idle = true;
      } else {
        cfg.run_active = true;
        cfg.run_idle = true;
      }
    } else if (key == "idle_wait_sec") {
      cfg.idle_wait_sec = static_cast<int>(ParseU32(val, 15, 0, 120));
    } else if (key == "manual_wait_sec") {
      cfg.manual_wait_sec = static_cast<int>(ParseU32(val, 20, 1, 120));
    } else if (key == "wake_mode") {
      std::string l = ToLower(val);
      if (l == "auto" || l == "inject" || l == "manual" || l == "none") cfg.wake_mode = l;
    } else if (key == "mem_step_mib") {
      cfg.mem_step_mib = ParseU32(val, 100, 16, 1024);
    } else if (key == "mem_max_mib") {
      cfg.mem_max_mib = ParseU32(val, 16384, 128, 32768);
    } else if (key == "mixed_cpu_mib") {
      cfg.mixed_cpu_mib = ParseU32(val, 0, 0, 16384);
    } else if (key == "allow_over_limit") {
      cfg.allow_over_limit = ParseBool(val);
    } else if (key == "use_warp") {
      cfg.use_warp = ParseBool(val);
    } else if (key == "exit_on_done") {
      cfg.exit_on_done = ParseBool(val);
    }
  }
  return cfg;
}

std::string FormatProfilerConfig(const ProfilerConfig& cfg) {
  std::ostringstream os;
  os << "source=" << cfg.source
     << " states=" << (cfg.run_active && cfg.run_idle ? "both" : (cfg.run_active ? "active" : "idle"))
     << " wake_mode=" << cfg.wake_mode
     << " idle_wait_sec=" << cfg.idle_wait_sec
     << " mem_step_mib=" << cfg.mem_step_mib
     << " mem_max_mib=" << cfg.mem_max_mib
     << " mixed_cpu_mib=" << cfg.mixed_cpu_mib
     << " allow_over_limit=" << (cfg.allow_over_limit ? 1 : 0)
     << " tests=[cpu=" << cfg.run_cpu
     << ",bw=" << cfg.run_bandwidth
     << ",gpu=" << cfg.run_gpu
     << ",mem_cpu=" << cfg.run_mem_cpu
     << ",mem_d3d11=" << cfg.run_mem_d3d11
     << ",mem_d3d12=" << cfg.run_mem_d3d12
     << ",mem_mixed_d3d11=" << cfg.run_mem_mixed_d3d11
     << ",mem_mixed_d3d12=" << cfg.run_mem_mixed_d3d12 << "]";
  return os.str();
}
