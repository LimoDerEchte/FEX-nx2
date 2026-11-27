// SPDX-License-Identifier: MIT
#include <stddef.h>

enum { Columns = 13, Rows = 7, Stride = Columns + 1, PageSize = 4096 };
enum { Read = 0, Write = 1, Close = 3, Map = 9, Unmap = 11, OpenAt = 257 };

static long syscall6(long number, long first, long second, long third, long fourth, long fifth, long sixth) {
  register long r10 __asm__("r10") = fourth;
  register long r8 __asm__("r8") = fifth;
  register long r9 __asm__("r9") = sixth;
  __asm__ volatile("syscall" : "+a"(number) : "D"(first), "S"(second), "d"(third), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return number;
}

static long syscall3(long number, long first, long second, long third) {
  return syscall6(number, first, second, third, 0, 0, 0);
}

#define PRINT(text) syscall3(Write, 1, (long)(text), sizeof(text) - 1)

static int load_board(char* board, const char* path) {
  const long file = syscall3(OpenAt, -100, (long)path, 0);
  if (file < 0) {
    return 0;
  }

  size_t total = 0;
  while (total < Rows * Stride + 1) {
    const long received = syscall3(Read, file, (long)(board + total), Rows * Stride + 1 - total);
    if (received < 0) {
      syscall3(Close, file, 0, 0);
      return 0;
    }
    if (!received) {
      break;
    }
    total += received;
  }
  syscall3(Close, file, 0, 0);
  return total == Rows * Stride;
}

static int validate_board(const char* board) {
  int player = -1;
  int exits = 0;
  for (int row = 0; row < Rows; ++row) {
    for (int col = 0; col < Columns; ++col) {
      const int pos = row * Stride + col;
      const char tile = board[pos];
      if (tile == '@' && player == -1) {
        player = pos;
      } else if (tile == 'E') {
        ++exits;
      } else if (tile != '#' && tile != '.') {
        return -1;
      }
    }
    if (board[row * Stride + Columns] != '\n') {
      return -1;
    }
  }
  return exits == 1 ? player : -1;
}

static int play(char* board, int player) {
  for (;;) {
    PRINT("\033[2J\033[HSD Maze\n\n");
    board[player] = '@';
    syscall3(Write, 1, (long)board, Rows * Stride);
    board[player] = '.';
    PRINT("\nFind E. D-pad or WASD to move. B or Q to quit.\n");

    char key;
    if (syscall3(Read, 0, (long)&key, 1) != 1 || key == 'q') {
      PRINT("\nMaze closed.\n");
      return 0;
    }

    const int col = player % Stride;
    const int row = player / Stride;
    int next = player;
    switch (key) {
    case 'w': if (row > 0) next -= Stride; break;
    case 'a': if (col > 0) --next; break;
    case 's': if (row + 1 < Rows) next += Stride; break;
    case 'd': if (col + 1 < Columns) ++next; break;
    default: break;
    }
    if (board[next] == 'E') {
      PRINT("\nYou found the exit!\n");
      return 0;
    }
    if (board[next] != '#') {
      player = next;
    }
  }
}

int maze_main(long argc, char** argv) {
  const long memory = syscall6(Map, 0, PageSize, 3, 0x22, -1, 0);
  if (memory < 0) {
    PRINT("Could not allocate the maze board.\n");
    return 1;
  }

  char* board = (char*)memory;
  const char* path = argc > 1 ? argv[1] : "data/maze.txt";
  const int player = load_board(board, path) ? validate_board(board) : -1;
  int result = 1;
  if (player < 0) {
    PRINT("Could not load the maze. Check the data file and launch argument.\n");
  } else {
    result = play(board, player);
  }
  syscall3(Unmap, memory, PageSize, 0);
  return result;
}
