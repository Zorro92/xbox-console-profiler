#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class JsonValue {
 public:
  enum class Type { Null, Bool, Int64, UInt64, Double, String, Array, Object };

  JsonValue() : type_(Type::Null) {}
  static JsonValue Null() { return JsonValue(); }
  static JsonValue Bool(bool v);
  static JsonValue Int(int64_t v);
  static JsonValue UInt(uint64_t v);
  static JsonValue Double(double v);
  static JsonValue String(std::string v);
  static JsonValue Array();
  static JsonValue Object();

  void Push(JsonValue v);
  void Set(const std::string& key, JsonValue v);

  std::string Dump(int indent = 2) const;

 private:
  void DumpImpl(std::ostringstream& os, int indent, int depth) const;

  Type type_ = Type::Null;
  bool bool_val_ = false;
  int64_t int_val_ = 0;
  uint64_t uint_val_ = 0;
  double double_val_ = 0.0;
  std::string str_val_;
  std::vector<JsonValue> arr_val_;
  std::vector<std::pair<std::string, JsonValue>> obj_val_;
};

class ProfilerReport {
 public:
  ProfilerReport();
  ~ProfilerReport();

  bool Open(const std::string& results_dir, const std::string& variant_tag);
  void Log(const std::string& line);
  void SetStatus(const std::string& status);
  void SetSection(const std::string& key, JsonValue value);
  void FlushJson();

  const std::string& TextPath() const { return text_path_; }
  const std::string& JsonPath() const { return json_path_; }

 private:
  std::mutex mu_;
  std::string results_dir_;
  std::string text_path_;
  std::string json_path_;
  std::string latest_txt_path_;
  std::string latest_json_path_;
  std::string status_path_;
  std::ofstream text_file_;
  std::ofstream latest_text_file_;
  JsonValue root_;
};

std::string FormatMib(uint64_t bytes);
std::string FormatHr(HRESULT hr);
