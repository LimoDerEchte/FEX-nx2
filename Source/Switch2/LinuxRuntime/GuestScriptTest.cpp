// SPDX-License-Identifier: MIT
#include "../GuestScript.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                           \
    }                                                         \
  } while (0)

using namespace Switch2Guest;

int main() {
  ScriptLine Line;
  CHECK(ParseScriptLine("\x7f" "ELF", Line) == ScriptParse::NotScript);
  CHECK(ParseScriptLine("#", Line) == ScriptParse::NotScript);
  CHECK(ParseScriptLine("#!/bin/sh\necho", Line) == ScriptParse::Script);
  CHECK(Line.Interpreter == "/bin/sh" && !Line.Argument);
  CHECK(ParseScriptLine("#! \t/usr/bin/perl -w  \t\nuse", Line) == ScriptParse::Script);
  CHECK(Line.Interpreter == "/usr/bin/perl" && Line.Argument == "-w");
  CHECK(ParseScriptLine("#!/usr/bin/env  python3 -u \n", Line) == ScriptParse::Script);
  CHECK(Line.Interpreter == "/usr/bin/env" && Line.Argument == "python3 -u");
  CHECK(ParseScriptLine("#!/bin/sh", Line) == ScriptParse::Script); // short file without newline
  CHECK(Line.Interpreter == "/bin/sh");
  CHECK(ParseScriptLine("#!\n", Line) == ScriptParse::Invalid);
  CHECK(ParseScriptLine("#!   \t\n", Line) == ScriptParse::Invalid);
  CHECK(ParseScriptLine("#!/bin/sh\r\n", Line) == ScriptParse::Script);
  CHECK(Line.Interpreter == "/bin/sh\r"); // Linux keeps the carriage return

  std::string Long = "#!/" + std::string(ScriptHeadBytes - 3, 'a');
  CHECK(ParseScriptLine(Long, Line) == ScriptParse::Invalid); // name may be truncated
  std::string Fits = "#!/bin/sh " + std::string(ScriptHeadBytes - 10, 'x');
  CHECK(ParseScriptLine(Fits, Line) == ScriptParse::Script); // truncated argument is fine
  CHECK(Line.Interpreter == "/bin/sh" && Line.Argument->size() == ScriptHeadBytes - 11); // last byte dropped
  CHECK(ParseScriptLine(std::string_view("#!/bin/sh\0 -x\n", 15), Line) == ScriptParse::Script);
  CHECK(Line.Interpreter == "/bin/sh" && !Line.Argument);

  CHECK(ParseScriptLine("#!/bin/sh -e\n", Line) == ScriptParse::Script);
  const auto Argv = ScriptArguments(Line, "./run", {"run", "a", "b"});
  CHECK((Argv == std::vector<std::string> {"/bin/sh", "-e", "./run", "a", "b"}));
  ParseScriptLine("#!/bin/sh\n", Line);
  CHECK((ScriptArguments(Line, "/x", {}) == std::vector<std::string> {"/bin/sh", "/x"}));
  std::puts("guest script checks passed");
}
