// SPDX-License-Identifier: MIT
#include "GuestTerminal.h"

#include <cassert>

int main() {
  using namespace Switch2Guest;
  const auto Key = [](unsigned Usage, bool Shift = false, bool Ctrl = false, bool Alt = false, bool Caps = false) {
    const auto Encoded = EncodeTerminalKey(Usage, Shift, Ctrl, Alt, Caps);
    return std::string(Encoded.Bytes.data(), Encoded.Length);
  };

  assert(Key(4) == "a");
  assert(Key(4, true) == "A");
  assert(Key(4, false, false, false, true) == "A");
  assert(Key(4, true, false, false, true) == "a");
  assert(Key(7, false, true) == "\x04");
  assert(Key(30, true) == "!");
  assert(Key(52, true) == "\"");
  assert(Key(82) == "\x1b[A");
  assert(Key(76, false, false, true) == "\x1b\x1b[3~");
  assert(Key(58).empty());

  GuestConsoleOutput Output;
  assert(Output.Filter("\x1b[?2004hfex") == "fex");
  assert(Output.Filter("\x1b") == "");
  assert(Output.Filter("[?20") == "");
  assert(Output.Filter("04l\x1b[36mblue\x1b[0m") == "\x1b[36mblue\x1b[0m");
  assert(Output.Filter("\x1b[D") == "\x1b[D");

  GuestTerminalInput Input;
  assert(Input.Feed('a') == "a");
  assert(Input.Feed('b') == "b");
  assert(!Input.Readable());
  assert(Input.Feed(127) == "\b \b");
  assert(Input.Feed('\r') == "\n");
  char Buffer[16] {};
  assert(Input.Read(Buffer, sizeof(Buffer)) == 2);
  assert(std::string_view(Buffer, 2) == "a\n");
  Input.Feed(4);
  assert(Input.TakeEof());
  assert(!Input.Readable());

  auto Raw = Input.Attributes;
  Raw.Input = 0;
  Raw.Local = 0;
  Input.SetAttributes(Raw, true);
  assert(Input.Feed('\r').empty());
  assert(Input.Readable());
  assert(Input.Read(Buffer, 1) == 1 && Buffer[0] == '\r');
  Input.Feed(4);
  assert(!Input.TakeEof());
  assert(Input.Read(Buffer, 1) == 1 && Buffer[0] == 4);
}
