// SPDX-License-Identifier: MIT
#include "../GuestVideo.h"

#include <stddef.h>
#include <stdint.h>

enum { Width = FEX_NX2_FRAME_WIDTH, Height = FEX_NX2_FRAME_HEIGHT, TextureSize = 32 };
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

static int load_texture(uint32_t* pixels, const char* path) {
  const long fd = syscall3(OpenAt, -100, (long)path, 0);
  if (fd < 0) {
    return 0;
  }
  const size_t expected = TextureSize * TextureSize * sizeof(*pixels);
  size_t total = 0;
  while (total < expected) {
    const long received = syscall3(Read, fd, (long)((char*)pixels + total), expected - total);
    if (received <= 0) {
      break;
    }
    total += received;
  }
  char extra;
  const int complete = total == expected && syscall3(Read, fd, (long)&extra, 1) == 0;
  syscall3(Close, fd, 0, 0);
  return complete;
}

static void render(uint32_t* frame, const uint32_t* texture, int x, int y) {
  for (int row = 0; row < Height; ++row) {
    for (int col = 0; col < Width; ++col) {
      frame[row * Width + col] = ((row / 16 + col / 16) & 1) ? 0xff382a20 : 0xff49362a;
    }
  }

  for (int row = 0; row < TextureSize; ++row) {
    for (int col = 0; col < TextureSize; ++col) {
      const uint32_t pixel = texture[row * TextureSize + col];
      if (pixel >> 24) {
        frame[(y + row) * Width + x + col] = pixel;
      }
    }
  }
}

static int play(uint32_t* frame, const uint32_t* texture) {
  int x = (Width - TextureSize) / 2;
  int y = (Height - TextureSize) / 2;
  for (;;) {
    render(frame, texture, x, y);
    if (syscall3(FEX_NX2_PRESENT_RGBA, (long)frame, 0, 0) != 0) {
      PRINT("Could not present the texture demo.\n");
      return 1;
    }
    char key;
    if (syscall3(Read, 0, (long)&key, 1) != 1 || key == 'q') {
      return 0;
    }
    switch (key) {
    case 'w': y = y >= 8 ? y - 8 : 0; break;
    case 'a': x = x >= 8 ? x - 8 : 0; break;
    case 's': y = y + 8 <= Height - TextureSize ? y + 8 : Height - TextureSize; break;
    case 'd': x = x + 8 <= Width - TextureSize ? x + 8 : Width - TextureSize; break;
    }
  }
}

int graphics_main(long argc, char** argv) {
  const size_t frame_bytes = Width * Height * sizeof(uint32_t);
  const size_t bytes = frame_bytes + TextureSize * TextureSize * sizeof(uint32_t);
  const long memory = syscall6(Map, 0, bytes, 3, 0x22, -1, 0);
  if (memory < 0) {
    PRINT("Could not allocate drawing memory.\n");
    return 1;
  }
  uint32_t* frame = (uint32_t*)memory;
  uint32_t* texture = (uint32_t*)((char*)frame + frame_bytes);
  const char* path = argc > 1 ? argv[1] : "data/sprite.rgba";
  int result = 1;
  if (load_texture(texture, path)) {
    PRINT("Texture loaded from disk.\n");
    result = play(frame, texture);
  } else {
    PRINT("Could not load the 32 x 32 RGBA texture.\n");
  }
  syscall3(Unmap, memory, bytes, 0);
  return result;
}
