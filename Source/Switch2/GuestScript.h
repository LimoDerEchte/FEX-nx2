// SPDX-License-Identifier: MIT
#pragma once
// Interpreter scripts for execve, as Linux fs/binfmt_script.c handles them.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Switch2Guest {
// Linux reads this many bytes of the file for the "#!" line (BINPRM_BUF_SIZE).
inline constexpr size_t ScriptHeadBytes = 256;
// Linux allows four interpreter rewrites (BINPRM_MAX_RECURSION); a fifth is ELOOP.
inline constexpr int ScriptMaxRewrites = 4;

struct ScriptLine {
  std::string Interpreter;
  std::optional<std::string> Argument; // everything after the name, as one word
};

enum class ScriptParse { NotScript, Script, Invalid };

// Parses the first bytes of a file (at most ScriptHeadBytes). Invalid maps to
// ENOEXEC: an empty interpreter name, or a name that may have been truncated
// because no newline or blank ends it inside the buffer.
inline ScriptParse ParseScriptLine(std::string_view Head, ScriptLine& Out) {
  if (Head.size() > ScriptHeadBytes) Head = Head.substr(0, ScriptHeadBytes);
  if (!Head.starts_with("#!")) return ScriptParse::NotScript;
  const auto Blank = [](char C) { return C == ' ' || C == '\t'; };
  const auto Newline = Head.find('\n', 2);
  // Without a newline Linux ends the line one byte before the buffer's end.
  const bool Full = Newline == Head.npos && Head.size() == ScriptHeadBytes;
  std::string_view Line = Head.substr(2, Newline != Head.npos ? Newline - 2 : Head.size() - 2 - Full);
  size_t Start = 0;
  while (Start < Line.size() && Blank(Line[Start])) ++Start;
  size_t NameEnd = Start;
  while (NameEnd < Line.size() && !Blank(Line[NameEnd]) && Line[NameEnd] != '\0') ++NameEnd;
  // Without a newline the line fills the buffer; a name that runs to its end
  // may be truncated.
  if (Full && NameEnd == Line.size()) return ScriptParse::Invalid;
  size_t End = Line.size();
  while (End > Start && Blank(Line[End - 1])) --End;
  if (NameEnd == Start) return ScriptParse::Invalid;
  Out.Interpreter.assign(Line.substr(Start, NameEnd - Start));
  // A NUL after the name ends the line; only a blank introduces an argument.
  size_t ArgStart = NameEnd < Line.size() && Line[NameEnd] == '\0' ? End : NameEnd;
  while (ArgStart < End && Blank(Line[ArgStart])) ++ArgStart;
  Out.Argument.reset();
  if (ArgStart < End) {
    // The kernel's argument is a C string; an embedded NUL ends it.
    auto Argument = Line.substr(ArgStart, End - ArgStart);
    Argument = Argument.substr(0, Argument.find('\0'));
    Out.Argument.emplace(Argument);
  }
  return ScriptParse::Script;
}

// argv after one rewrite: interpreter, optional argument, the script's file
// name, then the original arguments without argv[0].
inline std::vector<std::string> ScriptArguments(const ScriptLine& Line, const std::string& FileName,
                                                const std::vector<std::string>& Arguments) {
  std::vector<std::string> Result {Line.Interpreter};
  if (Line.Argument) Result.push_back(*Line.Argument);
  Result.push_back(FileName);
  if (Arguments.size() > 1) Result.insert(Result.end(), Arguments.begin() + 1, Arguments.end());
  return Result;
}
} // namespace Switch2Guest
