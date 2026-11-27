// SPDX-License-Identifier: MIT
#include "render/Fonts.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "platform/Log.h"
#include "render/Atlas.h"

namespace ui {
namespace {

constexpr const char* EllipsisUtf8 = "\xE2\x80\xA6";
constexpr uint64_t EvictAfterFrames = 600;
constexpr size_t MaxShapeCache = 2048;

// ---- UTF-8 -----------------------------------------------------------------------------------------------------------

uint32_t DecodeOne(std::string_view S, size_t& I) {
  const unsigned char C = static_cast<unsigned char>(S[I]);
  uint32_t Cp = 0xFFFD;
  size_t Len = 1;
  if (C < 0x80) {
    Cp = C;
  } else if ((C & 0xE0) == 0xC0 && I + 1 < S.size()) {
    Cp = (uint32_t(C & 0x1F) << 6) | (static_cast<unsigned char>(S[I + 1]) & 0x3F);
    Len = 2;
  } else if ((C & 0xF0) == 0xE0 && I + 2 < S.size()) {
    Cp = (uint32_t(C & 0x0F) << 12) | (uint32_t(static_cast<unsigned char>(S[I + 1]) & 0x3F) << 6) | (static_cast<unsigned char>(S[I + 2]) & 0x3F);
    Len = 3;
  } else if ((C & 0xF8) == 0xF0 && I + 3 < S.size()) {
    Cp = (uint32_t(C & 0x07) << 18) | (uint32_t(static_cast<unsigned char>(S[I + 1]) & 0x3F) << 12) |
         (uint32_t(static_cast<unsigned char>(S[I + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(S[I + 3]) & 0x3F);
    Len = 4;
  }
  I += Len;
  return Cp;
}

void Encode(std::string& Out, uint32_t Cp) {
  if (Cp < 0x80) {
    Out.push_back(char(Cp));
  } else if (Cp < 0x800) {
    Out.push_back(char(0xC0 | (Cp >> 6)));
    Out.push_back(char(0x80 | (Cp & 0x3F)));
  } else if (Cp < 0x10000) {
    Out.push_back(char(0xE0 | (Cp >> 12)));
    Out.push_back(char(0x80 | ((Cp >> 6) & 0x3F)));
    Out.push_back(char(0x80 | (Cp & 0x3F)));
  } else {
    Out.push_back(char(0xF0 | (Cp >> 18)));
    Out.push_back(char(0x80 | ((Cp >> 12) & 0x3F)));
    Out.push_back(char(0x80 | ((Cp >> 6) & 0x3F)));
    Out.push_back(char(0x80 | (Cp & 0x3F)));
  }
}

// Uppercase for ASCII, Latin-1, Greek and Cyrillic basics (the `section` style is UPPERCASE; the UI strings are English).
uint32_t Upper(uint32_t C) {
  if (C >= 'a' && C <= 'z')
    return C - 32;
  if (C >= 0xE0 && C <= 0xFE && C != 0xF7)
    return C - 32;
  if (C == 0xFF)
    return 0x178;
  if (C >= 0x3B1 && C <= 0x3C9 && C != 0x3C2)
    return C - 32;
  if (C >= 0x430 && C <= 0x44F)
    return C - 32;
  if (C >= 0x450 && C <= 0x45F)
    return C - 80;
  return C;
}

std::string UpperUtf8(std::string_view S) {
  std::string Out;
  Out.reserve(S.size());
  for (size_t I = 0; I < S.size();)
    Encode(Out, Upper(DecodeOne(S, I)));
  return Out;
}

// Byte offsets of every code point start, plus the end.
std::vector<size_t> CodepointOffsets(std::string_view S) {
  std::vector<size_t> Off;
  for (size_t I = 0; I < S.size();) {
    Off.push_back(I);
    DecodeOne(S, I);
  }
  Off.push_back(S.size());
  return Off;
}

int Px64(float Size, float S) { return std::max(64, int(std::lround(double(Size) * double(S) * 64.0))); }

} // namespace

struct Fonts::Impl {
  struct Face {
    bool Tried = false;
    FT_Face Ft = nullptr;
    hb_blob_t* Blob = nullptr;
    hb_face_t* Hb = nullptr;
  };
  struct Glyph {
    int16_t Page = -1;  // -1: no bitmap (space, or the atlas is full)
    int16_t X = 0, Y = 0, W = 0, H = 0;
    int16_t Left = 0, Top = 0;
  };
  struct Sized {
    hb_font_t* Hf = nullptr;
    std::vector<std::unique_ptr<AtlasPage>> Pages;
    std::unordered_map<uint32_t, Glyph> Glyphs;
    float Ascent = 0, Descent = 0, Gap = 0;
    uint64_t LastUsed = 0;
  };
  struct Placed {
    uint32_t Gid;
    float X;
  };
  struct Shaped {
    std::vector<Placed> G;
    float Width = 0;
  };

  explicit Impl(Renderer& R) : R(R) {
    if (FT_Init_FreeType(&Lib)) {
      Log("FAIL fonts: FT_Init_FreeType");
      Lib = nullptr;
    }
  }
  ~Impl() {
    for (auto& E : Atlases)
      DestroySized(E.second);
    Atlases.clear();
    for (Face& F : Faces) {
      if (F.Hb)
        hb_face_destroy(F.Hb);
      if (F.Blob)
        hb_blob_destroy(F.Blob);
      if (F.Ft)
        FT_Done_Face(F.Ft);
    }
    if (Lib)
      FT_Done_FreeType(Lib);
  }

  void DestroySized(Sized& Sz) {
    Sz.Pages.clear();  // AtlasPage destructors destroy the textures
    if (Sz.Hf)
      hb_font_destroy(Sz.Hf);
    Sz.Hf = nullptr;
  }

  Face* GetFace(FontId Id) {
    Face& F = Faces[size_t(Id)];
    if (F.Tried)
      return F.Ft ? &F : nullptr;
    F.Tried = true;
    const Blob B = GetFont(Id);
    if (!Lib || !B.Data || FT_New_Memory_Face(Lib, B.Data, FT_Long(B.Size), 0, &F.Ft)) {
      Log("FAIL fonts: cannot open %s (%zu bytes)", FontName(Id), B.Size);
      F.Ft = nullptr;
      return nullptr;
    }
    F.Blob = hb_blob_create(reinterpret_cast<const char*>(B.Data), unsigned(B.Size), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    F.Hb = hb_face_create(F.Blob, 0);
    return &F;
  }

  Sized* GetSized(FontId Id, int Px) {
    const uint64_t Key = (uint64_t(Id) << 32) | uint64_t(uint32_t(Px));
    auto It = Atlases.find(Key);
    if (It != Atlases.end()) {
      It->second.LastUsed = Frame;
      return &It->second;
    }
    Face* F = GetFace(Id);
    if (!F)
      return nullptr;
    Sized Sz;
    Sz.Hf = hb_font_create(F->Hb);
    hb_font_set_scale(Sz.Hf, Px, Px);
    hb_font_extents_t Ext {};
    hb_font_get_h_extents(Sz.Hf, &Ext);
    Sz.Ascent = float(Ext.ascender) / 64.0f;
    Sz.Descent = float(-Ext.descender) / 64.0f;
    Sz.Gap = float(Ext.line_gap) / 64.0f;
    Sz.LastUsed = Frame;
    ++Created;
    return &Atlases.emplace(Key, std::move(Sz)).first->second;
  }

  // Rasterizes one glyph into the atlas (on first use).
  const Glyph& GetGlyph(FontId Id, int Px, Sized& Sz, uint32_t Gid, bool Italic) {
    const uint32_t Key = Gid | (uint32_t(Italic) << 31);
    auto It = Sz.Glyphs.find(Key);
    if (It != Sz.Glyphs.end())
      return It->second;
    Glyph G;
    Face* F = GetFace(Id);
    FT_Error Loaded = 1;
    if (F && !FT_Set_Char_Size(F->Ft, 0, FT_F26Dot6(Px), 72, 72)) {
      // Shear only the bitmap. Terminal cell advances stay fixed.
      FT_Matrix Slant {65536, 13107, 0, 65536};
      if (Italic)
        FT_Set_Transform(F->Ft, &Slant, nullptr);
      Loaded = FT_Load_Glyph(F->Ft, Gid, FT_LOAD_TARGET_LIGHT | FT_LOAD_RENDER);
      if (Italic)
        FT_Set_Transform(F->Ft, nullptr, nullptr);
    }
    if (!Loaded) {
      FT_GlyphSlot Slot = F->Ft->glyph;
      const FT_Bitmap& Bm = Slot->bitmap;
      const int W = int(Bm.width), H = int(Bm.rows);
      if (W > 0 && H > 0 && Bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
        std::vector<uint8_t> Tight(size_t(W) * size_t(H));
        for (int Row = 0; Row < H; ++Row) {
          const uint8_t* Src = Bm.pitch >= 0 ? Bm.buffer + size_t(Row) * size_t(Bm.pitch) : Bm.buffer + size_t(H - 1 - Row) * size_t(-Bm.pitch);
          std::memcpy(&Tight[size_t(Row) * size_t(W)], Src, size_t(W));
        }
        int X = 0, Y = 0, Page = -1;
        if (!Sz.Pages.empty() && Sz.Pages.back()->Allocate(W, H, X, Y))
          Page = int(Sz.Pages.size()) - 1;
        if (Page < 0) {
          const int PageSize = Px <= 28 * 64 ? 512 : 1024;
          auto NewPage = std::make_unique<AtlasPage>(R, PageSize, PageSize, TextureFormat::Alpha8, TextureFilter::Nearest);
          if (NewPage->Valid() && NewPage->Allocate(W, H, X, Y)) {
            Sz.Pages.push_back(std::move(NewPage));
            Page = int(Sz.Pages.size()) - 1;
          } else {
            Log("FAIL fonts: no atlas room for a %dx%d glyph (%s %.1f px)", W, H, FontName(Id), double(Px) / 64.0);
          }
        }
        if (Page >= 0) {
          Sz.Pages[size_t(Page)]->Write(X, Y, W, H, Tight.data(), W);
          G.Page = int16_t(Page);
          G.X = int16_t(X);
          G.Y = int16_t(Y);
          G.W = int16_t(W);
          G.H = int16_t(H);
          G.Left = int16_t(Slot->bitmap_left);
          G.Top = int16_t(Slot->bitmap_top);
        }
      }
    }
    return Sz.Glyphs.emplace(Key, G).first->second;
  }

  const Shaped* Shape(const TextStyle& St, std::string_view Text, float S) {
    const int Px = Px64(St.Size, S);
    const float TrackPx = St.Tracking * S;
    std::string Key;
    Key.reserve(Text.size() + 16);
    Key.push_back(char(St.Font));
    Key.append(reinterpret_cast<const char*>(&Px), sizeof(Px));
    const int Track64 = int(std::lround(double(TrackPx) * 64.0));
    Key.append(reinterpret_cast<const char*>(&Track64), sizeof(Track64));
    Key.push_back(St.Upper ? 'U' : 'n');
    Key.append(Text);
    auto Hit = ShapeCache.find(Key);
    if (Hit != ShapeCache.end())
      return &Hit->second;
    Sized* Sz = GetSized(St.Font, Px);
    if (!Sz)
      return nullptr;
    if (ShapeCache.size() >= MaxShapeCache)
      ShapeCache.clear();

    std::string Upped;
    if (St.Upper) {
      Upped = UpperUtf8(Text);
      Text = Upped;
    }
    hb_buffer_t* Buf = hb_buffer_create();
    hb_buffer_add_utf8(Buf, Text.data(), int(Text.size()), 0, int(Text.size()));
    hb_buffer_guess_segment_properties(Buf);
    hb_feature_t Features[2];
    unsigned NumFeatures = 0;
    if (Track64 != 0) {  // letter-spacing turns ligatures off, as CSS does
      hb_feature_from_string("liga=0", -1, &Features[NumFeatures++]);
      hb_feature_from_string("clig=0", -1, &Features[NumFeatures++]);
    }
    hb_shape(Sz->Hf, Buf, Features, NumFeatures);
    unsigned Count = 0;
    const hb_glyph_info_t* Info = hb_buffer_get_glyph_infos(Buf, &Count);
    const hb_glyph_position_t* Pos = hb_buffer_get_glyph_positions(Buf, &Count);
    Shaped Out;
    Out.G.reserve(Count);
    float Pen = 0;
    for (unsigned I = 0; I < Count; ++I) {
      Out.G.push_back(Placed {Info[I].codepoint, Pen + float(Pos[I].x_offset) / 64.0f});
      Pen += float(Pos[I].x_advance) / 64.0f;
      if (I + 1 < Count)
        Pen += TrackPx;
    }
    Out.Width = Pen;
    hb_buffer_destroy(Buf);
    return &ShapeCache.emplace(std::move(Key), std::move(Out)).first->second;
  }

  Renderer& R;
  FT_Library Lib = nullptr;
  Face Faces[size_t(FontId::Count)];
  std::unordered_map<uint64_t, Sized> Atlases;
  std::unordered_map<std::string, Shaped> ShapeCache;
  uint64_t Frame = 0;
  int Created = 0, Evicted = 0;
};

Fonts::Fonts(Renderer& R) : P(std::make_unique<Impl>(R)) {}
Fonts::~Fonts() = default;
bool Fonts::Ok() const { return P->Lib != nullptr; }

float Fonts::Measure(const TextStyle& St, std::string_view Utf8, float S) {
  const Impl::Shaped* Sh = P->Shape(St, Utf8, S);
  return Sh ? Sh->Width : 0.0f;
}

LineMetrics Fonts::Metrics(const TextStyle& St, float S) {
  Impl::Sized* Sz = P->GetSized(St.Font, Px64(St.Size, S));
  LineMetrics M;
  if (Sz) {
    M.Ascent = Sz->Ascent;
    M.Descent = Sz->Descent;
    M.Gap = Sz->Gap;
  }
  return M;
}

float Fonts::LineHeightPx(const TextStyle& St, float S) {
  if (St.LineHeight > 0)
    return std::round(St.LineHeight * St.Size * S);
  return std::round(Metrics(St, S).Height());
}

float Fonts::BaselineInBox(const TextStyle& St, float S, float BoxTop, float BoxH) {
  const LineMetrics M = Metrics(St, S);
  return std::round(BoxTop + (BoxH - (M.Ascent + M.Descent)) * 0.5f + M.Ascent);
}

float Fonts::Draw(const TextStyle& St, std::string_view Utf8, float S, float X, float Baseline, Color C, Align A) {
  const Impl::Shaped* Sh = P->Shape(St, Utf8, S);
  if (!Sh)
    return 0.0f;
  const int Px = Px64(St.Size, S);
  Impl::Sized* Sz = P->GetSized(St.Font, Px);
  if (!Sz)
    return Sh->Width;
  const float Origin = A == Align::Left ? X : A == Align::Center ? X - Sh->Width * 0.5f : X - Sh->Width;
  const float By = std::round(Baseline);
  for (const Impl::Placed& G : Sh->G) {
    const Impl::Glyph& Gl = P->GetGlyph(St.Font, Px, *Sz, G.Gid, St.Italic);
    if (Gl.Page < 0)
      continue;
    AtlasPage& Page = *Sz->Pages[size_t(Gl.Page)];
    P->R.Image(Page.Texture(), Rect {std::round(Origin + G.X) + float(Gl.Left), By - float(Gl.Top), float(Gl.W), float(Gl.H)},
               Rect {float(Gl.X), float(Gl.Y), float(Gl.W), float(Gl.H)}, C);
  }
  return Sh->Width;
}

std::string Fonts::Fit(const TextStyle& St, std::string_view Utf8, float S, float MaxWidth, Ellipsis Mode) {
  if (Measure(St, Utf8, S) <= MaxWidth)
    return std::string(Utf8);
  const std::vector<size_t> Off = CodepointOffsets(Utf8);
  const int N = int(Off.size()) - 1;  // code points
  auto Build = [&](int Keep) {        // Keep code points, Keep < N
    std::string Out;
    if (Mode == Ellipsis::End) {
      size_t End = Off[size_t(Keep)];
      while (End > 0 && Utf8[End - 1] == ' ')
        --End;
      Out.assign(Utf8.substr(0, End));
      Out += EllipsisUtf8;
    } else {
      const int Head = (Keep + 1) / 2, Tail = Keep / 2;
      size_t HeadEnd = Off[size_t(Head)];
      while (HeadEnd > 0 && Utf8[HeadEnd - 1] == ' ')
        --HeadEnd;
      Out.assign(Utf8.substr(0, HeadEnd));
      Out += EllipsisUtf8;
      size_t TailStart = Off[size_t(N - Tail)];
      while (TailStart < Utf8.size() && Utf8[TailStart] == ' ')
        ++TailStart;
      Out.append(Utf8.substr(TailStart));
    }
    return Out;
  };
  int Lo = 0, Hi = N - 1;  // largest Keep that fits; Keep = 0 is the bare ellipsis
  if (Measure(St, EllipsisUtf8, S) > MaxWidth)
    return std::string();
  while (Lo < Hi) {
    const int Mid = (Lo + Hi + 1) / 2;
    if (Measure(St, Build(Mid), S) <= MaxWidth)
      Lo = Mid;
    else
      Hi = Mid - 1;
  }
  return Build(Lo);
}

void Fonts::EndFrame() {
  for (auto& E : P->Atlases)
    for (auto& Page : E.second.Pages)
      Page->Flush();
  ++P->Frame;
  if (P->Frame % 64 == 0) {
    for (auto It = P->Atlases.begin(); It != P->Atlases.end();) {
      if (P->Frame - It->second.LastUsed > EvictAfterFrames) {
        P->DestroySized(It->second);
        It = P->Atlases.erase(It);
        ++P->Evicted;
      } else {
        ++It;
      }
    }
  }
}

FontsStats Fonts::Stats() const {
  FontsStats St;
  St.Atlases = int(P->Atlases.size());
  for (const auto& E : P->Atlases) {
    St.Pages += int(E.second.Pages.size());
    for (const auto& G : E.second.Glyphs)
      St.Glyphs += G.second.Page >= 0 ? 1 : 0;
  }
  St.Created = P->Created;
  St.Evicted = P->Evicted;
  return St;
}

} // namespace ui
