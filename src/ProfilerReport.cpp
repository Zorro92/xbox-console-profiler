#include "pch.h"
#include "ProfilerReport.h"

namespace {

void WriteEscapedString(std::ostringstream& os, const std::string& s) {
  os << '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': os << "\\\""; break;
      case '\\': os << "\\\\"; break;
      case '\b': os << "\\b"; break;
      case '\f': os << "\\f"; break;
      case '\n': os << "\\n"; break;
      case '\r': os << "\\r"; break;
      case '\t': os << "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
          os << buf;
        } else {
          os << static_cast<char>(c);
        }
        break;
    }
  }
  os << '"';
}

std::string CurrentTimestampCompact() {
  std::time_t t = std::time(nullptr);
  std::tm tm_val{};
  localtime_s(&tm_val, &t);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
                tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
  return buf;
}

std::string CurrentTimestampIso() {
  std::time_t t = std::time(nullptr);
  std::tm tm_val{};
  gmtime_s(&tm_val, &t);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
                tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
  return buf;
}

} // namespace

JsonValue JsonValue::Bool(bool v) {
  JsonValue j;
  j.type_ = Type::Bool;
  j.bool_val_ = v;
  return j;
}

JsonValue JsonValue::Int(int64_t v) {
  JsonValue j;
  j.type_ = Type::Int64;
  j.int_val_ = v;
  return j;
}

JsonValue JsonValue::UInt(uint64_t v) {
  JsonValue j;
  j.type_ = Type::UInt64;
  j.uint_val_ = v;
  return j;
}

JsonValue JsonValue::Double(double v) {
  if (!std::isfinite(v)) return JsonValue::Null();
  JsonValue j;
  j.type_ = Type::Double;
  j.double_val_ = v;
  return j;
}

JsonValue JsonValue::String(std::string v) {
  JsonValue j;
  j.type_ = Type::String;
  j.str_val_ = std::move(v);
  return j;
}

JsonValue JsonValue::Array() {
  JsonValue j;
  j.type_ = Type::Array;
  return j;
}

JsonValue JsonValue::Object() {
  JsonValue j;
  j.type_ = Type::Object;
  return j;
}

void JsonValue::Push(JsonValue v) {
  if (type_ != Type::Array) type_ = Type::Array;
  arr_val_.push_back(std::move(v));
}

void JsonValue::Set(const std::string& key, JsonValue v) {
  if (type_ != Type::Object) type_ = Type::Object;
  for (auto& kv : obj_val_) {
    if (kv.first == key) {
      kv.second = std::move(v);
      return;
    }
  }
  obj_val_.emplace_back(key, std::move(v));
}

std::string JsonValue::Dump(int indent) const {
  std::ostringstream os;
  DumpImpl(os, indent, 0);
  os << '\n';
  return os.str();
}

void JsonValue::DumpImpl(std::ostringstream& os, int indent, int depth) const {
  auto pad = [&](int d) {
    if (indent > 0) {
      for (int i = 0; i < d * indent; ++i) os << ' ';
    }
  };
  switch (type_) {
    case Type::Null:
      os << "null";
      break;
    case Type::Bool:
      os << (bool_val_ ? "true" : "false");
      break;
    case Type::Int64:
      os << int_val_;
      break;
    case Type::UInt64:
      os << uint_val_;
      break;
    case Type::Double: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.6g", double_val_);
      os << buf;
      break;
    }
    case Type::String:
      WriteEscapedString(os, str_val_);
      break;
    case Type::Array:
      if (arr_val_.empty()) {
        os << "[]";
        break;
      }
      os << '[';
      if (indent > 0) os << '\n';
      for (size_t i = 0; i < arr_val_.size(); ++i) {
        pad(depth + 1);
        arr_val_[i].DumpImpl(os, indent, depth + 1);
        if (i + 1 < arr_val_.size()) os << ',';
        if (indent > 0) os << '\n';
      }
      pad(depth);
      os << ']';
      break;
    case Type::Object:
      if (obj_val_.empty()) {
        os << "{}";
        break;
      }
      os << '{';
      if (indent > 0) os << '\n';
      for (size_t i = 0; i < obj_val_.size(); ++i) {
        pad(depth + 1);
        WriteEscapedString(os, obj_val_[i].first);
        os << (indent > 0 ? ": " : ":");
        obj_val_[i].second.DumpImpl(os, indent, depth + 1);
        if (i + 1 < obj_val_.size()) os << ',';
        if (indent > 0) os << '\n';
      }
      pad(depth);
      os << '}';
      break;
  }
}

ProfilerReport::ProfilerReport() : root_(JsonValue::Object()) {}

ProfilerReport::~ProfilerReport() {
  if (text_file_.is_open()) text_file_.flush();
  if (latest_text_file_.is_open()) latest_text_file_.flush();
}

bool ProfilerReport::Open(const std::string& results_dir, const std::string& variant_tag) {
  std::lock_guard<std::mutex> lock(mu_);
  results_dir_ = results_dir;
  CreateDirectoryA(results_dir_.c_str(), nullptr);

  std::string ts = CurrentTimestampCompact();
  std::string safe_tag = variant_tag.empty() ? "run" : variant_tag;
  for (char& c : safe_tag) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
      c = '_';
    }
  }

  text_path_ = results_dir_ + "\\profile_" + ts + "_" + safe_tag + ".txt";
  json_path_ = results_dir_ + "\\profile_" + ts + "_" + safe_tag + ".json";
  latest_txt_path_ = results_dir_ + "\\latest.txt";
  latest_json_path_ = results_dir_ + "\\latest.json";
  status_path_ = results_dir_ + "\\status.txt";

  text_file_.open(text_path_, std::ios::out | std::ios::trunc);
  latest_text_file_.open(latest_txt_path_, std::ios::out | std::ios::trunc);

  root_ = JsonValue::Object();
  root_.Set("schema_version", JsonValue::Int(1));
  root_.Set("started_utc", JsonValue::String(CurrentTimestampIso()));
  root_.Set("variant_tag", JsonValue::String(safe_tag));
  return text_file_.is_open();
}

void ProfilerReport::Log(const std::string& line) {
  std::lock_guard<std::mutex> lock(mu_);
  if (text_file_.is_open()) {
    text_file_ << line << "\n";
    text_file_.flush();
  }
  if (latest_text_file_.is_open()) {
    latest_text_file_ << line << "\n";
    latest_text_file_.flush();
  }
#if !defined(WINAPI_FAMILY) || (WINAPI_FAMILY != WINAPI_FAMILY_APP)
  std::printf("%s\n", line.c_str());
  std::fflush(stdout);
#endif
  OutputDebugStringA((line + "\n").c_str());
}

void ProfilerReport::SetStatus(const std::string& status) {
  std::lock_guard<std::mutex> lock(mu_);
  if (status_path_.empty()) return;
  std::ofstream f(status_path_, std::ios::out | std::ios::trunc);
  if (f.is_open()) {
    f << status << "\n";
    f.flush();
  }
}

void ProfilerReport::SetSection(const std::string& key, JsonValue value) {
  std::lock_guard<std::mutex> lock(mu_);
  root_.Set(key, std::move(value));
}

void ProfilerReport::FlushJson() {
  std::lock_guard<std::mutex> lock(mu_);
  root_.Set("updated_utc", JsonValue::String(CurrentTimestampIso()));
  std::string payload = root_.Dump(2);
  if (!json_path_.empty()) {
    std::ofstream f(json_path_, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
      f << payload;
      f.flush();
    }
  }
  if (!latest_json_path_.empty()) {
    std::ofstream f(latest_json_path_, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
      f << payload;
      f.flush();
    }
  }
}

std::string FormatMib(uint64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return buf;
}

std::string FormatHr(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
  return buf;
}
