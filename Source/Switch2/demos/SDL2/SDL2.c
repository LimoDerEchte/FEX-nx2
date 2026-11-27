// SPDX-License-Identifier: MIT
#include <SDL.h>

#include "GuestVideo.h"
#include "GuestSyscall.h"

struct SDL_Window { Uint32 flags; };
struct SDL_Renderer { Uint32* pixels; Uint32 color; };
struct SDL_Texture { int width, height; Uint32 pixels[]; };

static SDL_Window window;
static SDL_Renderer renderer;
static const char* error;
static Uint8 keys[SDL_NUM_SCANCODES];
static unsigned previous;
static unsigned changed;
static int quit_pending;

static int fail(const char* message) {
  error = message;
  return -1;
}

static void* allocate(size_t bytes) {
  const long address = guest_syscall(9, 0, bytes, 3, 0x22, -1, 0);
  return address < 0 ? NULL : (void*)address;
}

static void release(void* address, size_t bytes) {
  guest_call(11, (long)address, bytes, 0);
}

int SDL_Init(Uint32 flags) {
  if (flags & ~(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS)) {
    return fail("Unsupported SDL subsystem");
  }
  error = NULL;
  previous = changed = 0;
  quit_pending = 0;
  for (int i = 0; i < SDL_NUM_SCANCODES; ++i) keys[i] = 0;
  return 0;
}

const char* SDL_GetError(void) { return error ? error : ""; }
void SDL_ClearError(void) { error = NULL; }

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int width, int height, Uint32 flags) {
  (void)title; (void)x; (void)y;
  if (window.flags || width != FEX_NX2_FRAME_WIDTH || height != FEX_NX2_FRAME_HEIGHT ||
      (flags & ~(SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN))) {
    fail("Only one 320x180 window is supported");
    return NULL;
  }
  window.flags = flags | SDL_WINDOW_SHOWN;
  return &window;
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* target, int index, Uint32 flags) {
  if (target != &window || !window.flags || renderer.pixels || index != -1 ||
      (flags & ~(SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC))) {
    fail("Only one software renderer is supported");
    return NULL;
  }
  renderer.pixels = allocate(FEX_NX2_FRAME_WIDTH * FEX_NX2_FRAME_HEIGHT * sizeof(Uint32));
  if (!renderer.pixels) {
    fail("Could not allocate renderer pixels");
    return NULL;
  }
  renderer.color = 0xff000000;
  return &renderer;
}

void SDL_DestroyRenderer(SDL_Renderer* target) {
  if (target == &renderer && renderer.pixels) {
    release(renderer.pixels, FEX_NX2_FRAME_WIDTH * FEX_NX2_FRAME_HEIGHT * sizeof(Uint32));
    renderer.pixels = NULL;
  }
}

void SDL_DestroyWindow(SDL_Window* target) {
  if (target == &window) {
    SDL_DestroyRenderer(&renderer);
    window.flags = 0;
  }
}

void SDL_Quit(void) { SDL_DestroyWindow(&window); }

SDL_Texture* SDL_CreateTexture(SDL_Renderer* target, Uint32 format, int access, int width, int height) {
  if (target != &renderer || !renderer.pixels || format != SDL_PIXELFORMAT_RGBA32 ||
      (access != SDL_TEXTUREACCESS_STATIC && access != SDL_TEXTUREACCESS_STREAMING) ||
      width < 1 || height < 1 || width > FEX_NX2_FRAME_WIDTH || height > FEX_NX2_FRAME_HEIGHT) {
    fail("Unsupported texture format, access, or size");
    return NULL;
  }
  SDL_Texture* texture = allocate(sizeof(*texture) + (size_t)width * height * sizeof(Uint32));
  if (!texture) {
    fail("Could not allocate texture pixels");
    return NULL;
  }
  texture->width = width;
  texture->height = height;
  return texture;
}

void SDL_DestroyTexture(SDL_Texture* texture) {
  if (texture) {
    release(texture, sizeof(*texture) + (size_t)texture->width * texture->height * sizeof(Uint32));
  }
}

static int inside(SDL_Rect box, int width, int height) {
  return box.x >= 0 && box.y >= 0 && box.w > 0 && box.h > 0 &&
         box.x <= width && box.y <= height && box.w <= width - box.x && box.h <= height - box.y;
}

int SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch) {
  if (!texture || !pixels) return fail("Missing texture data");
  const SDL_Rect box = rect ? *rect : (SDL_Rect){0, 0, texture->width, texture->height};
  if (!inside(box, texture->width, texture->height) || pitch < box.w * (int)sizeof(Uint32)) {
    return fail("Invalid texture update region");
  }
  for (int y = 0; y < box.h; ++y) {
    Uint8* dst = (Uint8*)(texture->pixels + (box.y + y) * texture->width + box.x);
    const Uint8* src = (const Uint8*)pixels + (size_t)y * pitch;
    for (size_t x = 0; x < (size_t)box.w * sizeof(Uint32); ++x) dst[x] = src[x];
  }
  return 0;
}

int SDL_SetRenderDrawColor(SDL_Renderer* target, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  if (target != &renderer || !renderer.pixels) return fail("Missing renderer");
  renderer.color = r | ((Uint32)g << 8) | ((Uint32)b << 16) | ((Uint32)a << 24);
  return 0;
}

int SDL_RenderFillRect(SDL_Renderer* target, const SDL_Rect* rect) {
  if (target != &renderer || !renderer.pixels) return fail("Missing renderer");
  const SDL_Rect box = rect ? *rect : (SDL_Rect){0, 0, FEX_NX2_FRAME_WIDTH, FEX_NX2_FRAME_HEIGHT};
  if (!inside(box, FEX_NX2_FRAME_WIDTH, FEX_NX2_FRAME_HEIGHT)) return fail("Unsupported draw region");
  for (int y = 0; y < box.h; ++y)
    for (int x = 0; x < box.w; ++x)
      renderer.pixels[(box.y + y) * FEX_NX2_FRAME_WIDTH + box.x + x] = renderer.color;
  return 0;
}

int SDL_RenderClear(SDL_Renderer* target) { return SDL_RenderFillRect(target, NULL); }

int SDL_RenderCopy(SDL_Renderer* target, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst) {
  if (target != &renderer || !renderer.pixels || !texture) return fail("Missing renderer or texture");
  const SDL_Rect from = src ? *src : (SDL_Rect){0, 0, texture->width, texture->height};
  const SDL_Rect to = dst ? *dst : (SDL_Rect){0, 0, FEX_NX2_FRAME_WIDTH, FEX_NX2_FRAME_HEIGHT};
  if (!inside(from, texture->width, texture->height) || !inside(to, FEX_NX2_FRAME_WIDTH, FEX_NX2_FRAME_HEIGHT)) {
    return fail("Unsupported copy region");
  }
  for (int y = 0; y < to.h; ++y)
    for (int x = 0; x < to.w; ++x)
      renderer.pixels[(to.y + y) * FEX_NX2_FRAME_WIDTH + to.x + x] =
        texture->pixels[(from.y + y * from.h / to.h) * texture->width + from.x + x * from.w / to.w];
  return 0;
}

void SDL_RenderPresent(SDL_Renderer* target) {
  if (target != &renderer || !renderer.pixels || guest_call(FEX_NX2_PRESENT_RGBA, (long)renderer.pixels, 0, 0)) {
    fail("Could not present frame");
    quit_pending = 1;
  }
}

static const SDL_Scancode directions[] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};

void SDL_PumpEvents(void) {
  const long input = guest_call(FEX_NX2_POLL_INPUT, 0, 0, 0);
  if (input < 0 || (input & FEX_NX2_QUIT)) quit_pending = 1;
  if (input < 0) return;
  const unsigned held = (unsigned)input & 15u;
  changed |= previous ^ held;
  previous = held;
  for (unsigned i = 0; i < 4; ++i) keys[directions[i]] = (held >> i) & 1;
}

const Uint8* SDL_GetKeyboardState(int* count) {
  if (count) *count = SDL_NUM_SCANCODES;
  return keys;
}

int SDL_PollEvent(SDL_Event* event) {
  if (!changed && !quit_pending) SDL_PumpEvents();
  if (quit_pending) {
    if (event) {
      *event = (SDL_Event){.type = SDL_QUIT};
      quit_pending = 0;
    }
    return 1;
  }
  for (unsigned i = 0; i < 4; ++i) {
    if (!(changed & (1u << i))) continue;
    if (event) {
      const SDL_Scancode code = directions[i];
      const Uint8 pressed = keys[code];
      changed &= ~(1u << i);
      *event = (SDL_Event){.key = {.type = pressed ? SDL_KEYDOWN : SDL_KEYUP, .state = pressed,
                                  .keysym = {.scancode = code, .sym = SDL_SCANCODE_TO_KEYCODE(code)}}};
    }
    return 1;
  }
  return 0;
}

Uint32 SDL_GetTicks(void) { return (Uint32)guest_call(FEX_NX2_TICKS_MS, 0, 0, 0); }

void SDL_Delay(Uint32 ms) {
  while (ms > 1000) {
    guest_call(FEX_NX2_DELAY_MS, 1000, 0, 0);
    ms -= 1000;
  }
  guest_call(FEX_NX2_DELAY_MS, ms, 0, 0);
}

SDL_RWops* SDL_RWFromFile(const char* path, const char* mode) {
  if (!path || !mode || mode[0] != 'r' || mode[1] != 'b' || mode[2] != 0) {
    fail("Only binary read mode is supported");
    return NULL;
  }
  const long fd = guest_call(257, -100, (long)path, 0);
  if (fd < 0) {
    fail("Could not open asset");
    return NULL;
  }
  SDL_RWops* file = allocate(sizeof(*file));
  if (!file) {
    guest_call(3, fd, 0, 0);
    fail("Could not allocate file handle");
    return NULL;
  }
  file->hidden.unknown.data1 = (void*)fd;
  return file;
}

size_t SDL_RWread(SDL_RWops* file, void* buffer, size_t size, size_t count) {
  if (!file || !size || count > SIZE_MAX / size) return 0;
  const size_t bytes = size * count;
  size_t total = 0;
  while (total < bytes) {
    const long read = guest_call(0, (long)file->hidden.unknown.data1, (long)((Uint8*)buffer + total), bytes - total);
    if (read <= 0) break;
    total += (size_t)read;
  }
  return total / size;
}

int SDL_RWclose(SDL_RWops* file) {
  if (!file) return fail("Missing file handle");
  const long result = guest_call(3, (long)file->hidden.unknown.data1, 0, 0);
  release(file, sizeof(*file));
  return result < 0 ? fail("Could not close asset") : 0;
}
