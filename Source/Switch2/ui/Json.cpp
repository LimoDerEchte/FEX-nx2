// SPDX-License-Identifier: MIT
#include "Json.h"

#include <cstdio>
#include <cstdlib>

namespace ui {

long long Json::AsInt(long long Default) const {
  if (T == Type::Number || T == Type::String) {
    char* End = nullptr;
    const long long V = std::strtoll(S.c_str(), &End, 10);
    if (End && End != S.c_str())
      return V;
  }
  return Default;
}

const Json* Json::Find(const std::string& Key) const {
  for (const auto& M : Obj)
    if (M.first == Key)
      return &M.second;
  return nullptr;
}
Json* Json::Find(const std::string& Key) {
  for (auto& M : Obj)
    if (M.first == Key)
      return &M.second;
  return nullptr;
}
Json& Json::Set(const std::string& Key, Json Value) {
  if (Json* J = Find(Key)) {
    *J = std::move(Value);
    return *J;
  }
  Obj.emplace_back(Key, std::move(Value));
  return Obj.back().second;
}
bool Json::Erase(const std::string& Key) {
  for (size_t I = 0; I < Obj.size(); ++I)
    if (Obj[I].first == Key) {
      Obj.erase(Obj.begin() + long(I));
      return true;
    }
  return false;
}

namespace {
struct Parser {
  const std::string& Src;
  size_t Pos = 0;
  std::string Err;
  int Depth = 0;

  bool Fail(const char* Msg) {
    if (Err.empty())
      Err = std::string(Msg) + " at offset " + std::to_string(Pos);
    return false;
  }
  void Ws() {
    while (Pos < Src.size() && (Src[Pos] == ' ' || Src[Pos] == '\t' || Src[Pos] == '\n' || Src[Pos] == '\r'))
      ++Pos;
  }
  bool Lit(const char* Word) {
    size_t I = 0;
    while (Word[I]) {
      if (Pos + I >= Src.size() || Src[Pos + I] != Word[I])
        return false;
      ++I;
    }
    Pos += I;
    return true;
  }
  static void Utf8(std::string& Out, unsigned Cp) {
    if (Cp < 0x80) {
      Out += char(Cp);
    } else if (Cp < 0x800) {
      Out += char(0xC0 | (Cp >> 6));
      Out += char(0x80 | (Cp & 0x3F));
    } else if (Cp < 0x10000) {
      Out += char(0xE0 | (Cp >> 12));
      Out += char(0x80 | ((Cp >> 6) & 0x3F));
      Out += char(0x80 | (Cp & 0x3F));
    } else {
      Out += char(0xF0 | (Cp >> 18));
      Out += char(0x80 | ((Cp >> 12) & 0x3F));
      Out += char(0x80 | ((Cp >> 6) & 0x3F));
      Out += char(0x80 | (Cp & 0x3F));
    }
  }
  bool Hex4(unsigned& V) {
    if (Pos + 4 > Src.size())
      return false;
    V = 0;
    for (int I = 0; I < 4; ++I) {
      const char C = Src[Pos + size_t(I)];
      V <<= 4;
      if (C >= '0' && C <= '9') V |= unsigned(C - '0');
      else if (C >= 'a' && C <= 'f') V |= unsigned(C - 'a' + 10);
      else if (C >= 'A' && C <= 'F') V |= unsigned(C - 'A' + 10);
      else return false;
    }
    Pos += 4;
    return true;
  }
  bool Str(std::string& Out) {
    if (Pos >= Src.size() || Src[Pos] != '"')
      return Fail("expected a string");
    ++Pos;
    while (Pos < Src.size()) {
      const char C = Src[Pos++];
      if (C == '"')
        return true;
      if (static_cast<unsigned char>(C) < 0x20)
        return Fail("control character in a string");
      if (C != '\\') {
        Out += C;
        continue;
      }
      if (Pos >= Src.size())
        break;
      const char E = Src[Pos++];
      switch (E) {
      case '"': Out += '"'; break;
      case '\\': Out += '\\'; break;
      case '/': Out += '/'; break;
      case 'b': Out += '\b'; break;
      case 'f': Out += '\f'; break;
      case 'n': Out += '\n'; break;
      case 'r': Out += '\r'; break;
      case 't': Out += '\t'; break;
      case 'u': {
        unsigned Cp;
        if (!Hex4(Cp))
          return Fail("bad \\u escape");
        if (Cp >= 0xD800 && Cp < 0xDC00) {
          unsigned Lo;
          if (Pos + 1 < Src.size() && Src[Pos] == '\\' && Src[Pos + 1] == 'u') {
            Pos += 2;
            if (!Hex4(Lo) || Lo < 0xDC00 || Lo > 0xDFFF)
              return Fail("bad surrogate pair");
            Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Lo - 0xDC00);
          } else {
            return Fail("lone surrogate");
          }
        }
        Utf8(Out, Cp);
        break;
      }
      default: return Fail("bad escape");
      }
    }
    return Fail("unterminated string");
  }
  bool Value(Json& Out) {
    Ws();
    if (Pos >= Src.size())
      return Fail("unexpected end");
    if (++Depth > 64)
      return Fail("nesting too deep");
    const char C = Src[Pos];
    bool Ok = false;
    if (C == '{') {
      ++Pos;
      Out = Json::Object();
      Ws();
      if (Pos < Src.size() && Src[Pos] == '}') {
        ++Pos;
        Ok = true;
      } else {
        for (;;) {
          Ws();
          std::string Key;
          if (!Str(Key))
            break;
          Ws();
          if (Pos >= Src.size() || Src[Pos] != ':') {
            Fail("expected ':'");
            break;
          }
          ++Pos;
          Json V;
          if (!Value(V))
            break;
          Out.Set(Key, std::move(V));
          Ws();
          if (Pos < Src.size() && Src[Pos] == ',') {
            ++Pos;
            continue;
          }
          if (Pos < Src.size() && Src[Pos] == '}') {
            ++Pos;
            Ok = true;
          } else {
            Fail("expected ',' or '}'");
          }
          break;
        }
      }
    } else if (C == '[') {
      ++Pos;
      Out = Json::Array();
      Ws();
      if (Pos < Src.size() && Src[Pos] == ']') {
        ++Pos;
        Ok = true;
      } else {
        for (;;) {
          Json V;
          if (!Value(V))
            break;
          Out.Push(std::move(V));
          Ws();
          if (Pos < Src.size() && Src[Pos] == ',') {
            ++Pos;
            continue;
          }
          if (Pos < Src.size() && Src[Pos] == ']') {
            ++Pos;
            Ok = true;
          } else {
            Fail("expected ',' or ']'");
          }
          break;
        }
      }
    } else if (C == '"') {
      std::string S;
      Ok = Str(S);
      if (Ok)
        Out = Json::String(std::move(S));
    } else if (Lit("true")) {
      Out = Json::Bool(true);
      Ok = true;
    } else if (Lit("false")) {
      Out = Json::Bool(false);
      Ok = true;
    } else if (Lit("null")) {
      Out = Json::Null();
      Ok = true;
    } else if (C == '-' || (C >= '0' && C <= '9')) {
      const size_t Start = Pos;
      if (Src[Pos] == '-')
        ++Pos;
      while (Pos < Src.size() && ((Src[Pos] >= '0' && Src[Pos] <= '9') || Src[Pos] == '.' || Src[Pos] == 'e' || Src[Pos] == 'E' || Src[Pos] == '+' || Src[Pos] == '-'))
        ++Pos;
      Out = Json::Number(Src.substr(Start, Pos - Start));
      Ok = Pos > Start + (Src[Start] == '-' ? 1u : 0u);
      if (!Ok)
        Fail("bad number");
    } else {
      Fail("unexpected character");
    }
    --Depth;
    return Ok;
  }
};

void Quote(std::string& Out, const std::string& S) {
  Out += '"';
  for (const char Ch : S) {
    switch (Ch) {
    case '"': Out += "\\\""; break;
    case '\\': Out += "\\\\"; break;
    case '\n': Out += "\\n"; break;
    case '\r': Out += "\\r"; break;
    case '\t': Out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(Ch) < 0x20) {
        char Buf[8];
        std::snprintf(Buf, sizeof(Buf), "\\u%04x", static_cast<unsigned char>(Ch));
        Out += Buf;
      } else {
        Out += Ch;
      }
    }
  }
  Out += '"';
}
} // namespace

bool Json::Parse(const std::string& Text, Json& Out, std::string& Error) {
  Parser P {Text, 0, {}, 0};
  Json V;
  if (!P.Value(V)) {
    Error = P.Err.empty() ? "invalid JSON" : P.Err;
    return false;
  }
  P.Ws();
  if (P.Pos != Text.size()) {
    Error = "trailing text at offset " + std::to_string(P.Pos);
    return false;
  }
  Out = std::move(V);
  return true;
}

void Json::DumpTo(std::string& Out, int Depth) const {
  const std::string Pad(size_t(Depth + 1) * 2, ' '), PadEnd(size_t(Depth) * 2, ' ');
  switch (T) {
  case Type::Null: Out += "null"; break;
  case Type::Bool: Out += B ? "true" : "false"; break;
  case Type::Number: Out += S.empty() ? "0" : S; break;
  case Type::String: Quote(Out, S); break;
  case Type::Array:
    if (Arr.empty()) {
      Out += "[]";
      break;
    }
    Out += "[\n";
    for (size_t I = 0; I < Arr.size(); ++I) {
      Out += Pad;
      Arr[I].DumpTo(Out, Depth + 1);
      Out += I + 1 < Arr.size() ? ",\n" : "\n";
    }
    Out += PadEnd + "]";
    break;
  case Type::Object:
    if (Obj.empty()) {
      Out += "{}";
      break;
    }
    Out += "{\n";
    for (size_t I = 0; I < Obj.size(); ++I) {
      Out += Pad;
      Quote(Out, Obj[I].first);
      Out += ": ";
      Obj[I].second.DumpTo(Out, Depth + 1);
      Out += I + 1 < Obj.size() ? ",\n" : "\n";
    }
    Out += PadEnd + "}";
    break;
  }
}

std::string Json::Dump() const {
  std::string Out;
  DumpTo(Out, 0);
  Out += '\n';
  return Out;
}

} // namespace ui
