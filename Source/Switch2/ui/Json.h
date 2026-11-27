// SPDX-License-Identifier: MIT
// A small JSON tree for ui.json, FEX's Config.json and licenses.json. Objects keep their key order and numbers keep their
// original text, so a file that is read, changed in one place and written back keeps everything else (unknown keys included).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ui {

class Json {
public:
  enum class Type : unsigned char { Null, Bool, Number, String, Array, Object };

  Json() = default;
  static Json Null() { return Json(); }
  static Json Bool(bool V) { Json J; J.T = Type::Bool; J.B = V; return J; }
  static Json Number(std::string Text) { Json J; J.T = Type::Number; J.S = std::move(Text); return J; }
  static Json Int(long long V) { return Number(std::to_string(V)); }
  static Json String(std::string V) { Json J; J.T = Type::String; J.S = std::move(V); return J; }
  static Json Array() { Json J; J.T = Type::Array; return J; }
  static Json Object() { Json J; J.T = Type::Object; return J; }

  Type GetType() const { return T; }
  bool IsObject() const { return T == Type::Object; }
  bool IsArray() const { return T == Type::Array; }
  bool IsString() const { return T == Type::String; }
  bool IsBool() const { return T == Type::Bool; }
  bool IsNumber() const { return T == Type::Number; }

  bool AsBool(bool Default = false) const { return T == Type::Bool ? B : Default; }
  const std::string& AsString() const { return S; }  // strings, and the text of a number
  long long AsInt(long long Default = 0) const;

  // Objects. Find returns null when absent; Set replaces in place or appends.
  const Json* Find(const std::string& Key) const;
  Json* Find(const std::string& Key);
  Json& Set(const std::string& Key, Json Value);
  bool Erase(const std::string& Key);
  const std::vector<std::pair<std::string, Json>>& Members() const { return Obj; }
  // Arrays.
  void Push(Json V) { Arr.push_back(std::move(V)); }
  const std::vector<Json>& Items() const { return Arr; }

  // Parses a whole document. false and an error with the offset when it is not valid JSON (trailing text is an error).
  static bool Parse(const std::string& Text, Json& Out, std::string& Error);
  // Two-space indent, one key per line, trailing newline.
  std::string Dump() const;

private:
  void DumpTo(std::string& Out, int Depth) const;
  Type T = Type::Null;
  bool B = false;
  std::string S;
  std::vector<Json> Arr;
  std::vector<std::pair<std::string, Json>> Obj;
};

} // namespace ui
