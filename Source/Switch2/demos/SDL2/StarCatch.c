// SPDX-License-Identifier: MIT
#include <SDL.h>

enum { Width = 320, Height = 180, SpriteSize = 32, Target = 5 };

static int load_star(SDL_Texture* star, const char* path) {
  Uint8 pixels[SpriteSize * SpriteSize * 4];
  SDL_RWops* file = SDL_RWFromFile(path, "rb");
  if (!file) return 0;
  const size_t read = SDL_RWread(file, pixels, 1, sizeof(pixels));
  SDL_RWclose(file);
  return read == sizeof(pixels) && SDL_UpdateTexture(star, NULL, pixels, SpriteSize * 4) == 0;
}

static int intersects(SDL_Rect a, SDL_Rect b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void draw(SDL_Renderer* renderer, SDL_Texture* star, SDL_Rect player, SDL_Rect falling, int score, int lives) {
  SDL_SetRenderDrawColor(renderer, 18, 25, 46, 255);
  SDL_RenderClear(renderer);
  SDL_SetRenderDrawColor(renderer, 62, 130, 240, 255);
  SDL_RenderFillRect(renderer, &player);
  SDL_RenderCopy(renderer, star, NULL, &falling);
  SDL_SetRenderDrawColor(renderer, 255, 205, 83, 255);
  for (int i = 0; i < score; ++i) {
    const SDL_Rect marker = {8 + i * 10, 6, 7, 6};
    SDL_RenderFillRect(renderer, &marker);
  }
  SDL_SetRenderDrawColor(renderer, 248, 115, 109, 255);
  for (int i = 0; i < lives; ++i) {
    const SDL_Rect marker = {Width - 15 - i * 10, 6, 7, 6};
    SDL_RenderFillRect(renderer, &marker);
  }
}

static int run(SDL_Renderer* renderer, SDL_Texture* star) {
  SDL_Rect player = {Width / 2 - 16, Height - 20, 32, 10};
  SDL_Rect falling = {Width / 2 - 8, 16, 16, 16};
  int score = 0, lives = 3, drop = 0;
  Uint32 previous = SDL_GetTicks();
  Uint32 accumulated = 0;
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) return 0;
    }
    const Uint32 now = SDL_GetTicks();
    const Uint32 delta = now - previous;
    previous = now;
    accumulated += delta > 64 ? 64 : delta;
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    while (accumulated >= 16) {
      accumulated -= 16;
      if (score == Target || lives == 0) continue;
      if (keys[SDL_SCANCODE_LEFT] && player.x > 0) player.x -= 4;
      if (keys[SDL_SCANCODE_RIGHT] && player.x < Width - player.w) player.x += 4;
      falling.y += 3;
      if (intersects(player, falling) || falling.y >= Height - falling.h) {
        if (intersects(player, falling)) ++score;
        else --lives;
        falling.x = (53 + ++drop * 79) % (Width - falling.w);
        falling.y = 16;
      }
    }
    if (score == Target || lives == 0) {
      SDL_SetRenderDrawColor(renderer, score == Target ? 30 : 105, score == Target ? 110 : 36, 50, 255);
      SDL_RenderClear(renderer);
      const SDL_Rect center = {Width / 2 - 32, Height / 2 - 32, 64, 64};
      SDL_RenderCopy(renderer, star, NULL, &center);
    } else {
      draw(renderer, star, player, falling, score, lives);
    }
    SDL_ClearError();
    SDL_RenderPresent(renderer);
    if (*SDL_GetError()) return 1;
    SDL_Delay(8);
  }
}

int main(int argc, char** argv) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return 1;
  SDL_Window* window = SDL_CreateWindow("Star Catch", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, Width, Height, SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : NULL;
  SDL_Texture* star = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, SpriteSize, SpriteSize) : NULL;
  const int loaded = star && load_star(star, argc > 1 ? argv[1] : "data/star.rgba");
  const int result = loaded ? run(renderer, star) : 1;
  SDL_DestroyTexture(star);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}
