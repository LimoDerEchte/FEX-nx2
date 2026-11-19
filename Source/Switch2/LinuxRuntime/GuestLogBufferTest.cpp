// SPDX-License-Identifier: MIT
#include "../GuestLogBuffer.h"

#include <cassert>
#include <string>

int main() {
  Switch2Guest::GuestLogBuffer<12> Log;
  auto Read = [&] {
    const auto [First, Second] = Log.Contents();
    return std::string(First.begin(), First.end()) + std::string(Second.begin(), Second.end());
  };

  Log.Append("alpha");
  Log.Append("beta");
  assert(Read() == "alpha\nbeta\n");
  Log.Append("gamma");
  assert(Read() == "beta\ngamma\n");
  assert(Log.DroppedLines() == 1);
  Log.Append("");
  assert(Read() == "beta\ngamma\n\n");
  Log.Append("0123456789ABCDE");
  assert(Read() == "0123456789A\n");
  assert(Log.DroppedLines() == 4);
  Log.Clear();
  assert(Read().empty());
  assert(Log.DroppedLines() == 0);
  Log.Append("new");
  assert(Read() == "new\n");
}
