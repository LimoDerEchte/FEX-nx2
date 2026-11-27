// SPDX-License-Identifier: MIT
// Verification of clear-signed OpenPGP messages (RFC 4880 7, 5.2) as the Ubuntu archive publishes its InRelease files:
// a version 4 signature over canonical text (type 0x01), RSA (algorithm 1) with SHA-512 (hash algorithm 10). Only the
// pieces the RootFS installer needs: SHA-512, RSA public-key operation with e = 65537, the cleartext framework, ASCII
// armor and signature packets. Portable, tested natively (tests/unit/GuestRootfsInstallTest.cpp, tools/CheckRootfsInstall.py).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Switch2Guest::OpenPgp {

// ---------------------------------------------------------------------------
// SHA-512 (FIPS 180-4).

class Sha512 {
public:
  Sha512() { Reset(); }
  void Reset() {
    static constexpr uint64_t Initial[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                                            0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    std::memcpy(State, Initial, sizeof(State));
    Length = 0;
    Used = 0;
  }
  void Update(const void* Data, size_t Size) {
    const auto* Bytes = static_cast<const uint8_t*>(Data);
    Length += Size;
    while (Size) {
      const size_t Take = std::min(Size, sizeof(Block) - Used);
      std::memcpy(Block + Used, Bytes, Take);
      Used += Take;
      Bytes += Take;
      Size -= Take;
      if (Used == sizeof(Block)) {
        Compress();
        Used = 0;
      }
    }
  }
  void Update(std::string_view Text) { Update(Text.data(), Text.size()); }
  std::array<uint8_t, 64> Final() {
    const uint64_t Bits = Length * 8;
    const uint8_t One = 0x80;
    Update(&One, 1);
    const uint8_t Zero = 0;
    while (Used != 112) Update(&Zero, 1);
    uint8_t Tail[16] {};
    for (int Index = 0; Index < 8; ++Index) Tail[15 - Index] = static_cast<uint8_t>(Bits >> (8 * Index));
    Update(Tail, sizeof(Tail));
    std::array<uint8_t, 64> Digest {};
    for (int Word = 0; Word < 8; ++Word)
      for (int Index = 0; Index < 8; ++Index) Digest[Word * 8 + Index] = static_cast<uint8_t>(State[Word] >> (56 - 8 * Index));
    return Digest;
  }

private:
  static uint64_t Rotate(uint64_t X, int N) { return (X >> N) | (X << (64 - N)); }
  void Compress() {
    static constexpr uint64_t K[80] = {
        0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull,
        0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull,
        0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
        0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
        0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull,
        0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
        0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull,
        0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
        0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
        0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
        0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull,
        0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
        0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull,
        0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
        0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
        0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};
    uint64_t W[80];
    for (int Index = 0; Index < 16; ++Index) {
      W[Index] = 0;
      for (int Byte = 0; Byte < 8; ++Byte) W[Index] = (W[Index] << 8) | Block[Index * 8 + Byte];
    }
    for (int Index = 16; Index < 80; ++Index) {
      const uint64_t S0 = Rotate(W[Index - 15], 1) ^ Rotate(W[Index - 15], 8) ^ (W[Index - 15] >> 7);
      const uint64_t S1 = Rotate(W[Index - 2], 19) ^ Rotate(W[Index - 2], 61) ^ (W[Index - 2] >> 6);
      W[Index] = W[Index - 16] + S0 + W[Index - 7] + S1;
    }
    uint64_t A = State[0], B = State[1], C = State[2], D = State[3], E = State[4], F = State[5], G = State[6], H = State[7];
    for (int Index = 0; Index < 80; ++Index) {
      const uint64_t T1 = H + (Rotate(E, 14) ^ Rotate(E, 18) ^ Rotate(E, 41)) + ((E & F) ^ (~E & G)) + K[Index] + W[Index];
      const uint64_t T2 = (Rotate(A, 28) ^ Rotate(A, 34) ^ Rotate(A, 39)) + ((A & B) ^ (A & C) ^ (B & C));
      H = G;
      G = F;
      F = E;
      E = D + T1;
      D = C;
      C = B;
      B = A;
      A = T1 + T2;
    }
    State[0] += A, State[1] += B, State[2] += C, State[3] += D, State[4] += E, State[5] += F, State[6] += G, State[7] += H;
  }
  uint64_t State[8];
  uint8_t Block[128];
  uint64_t Length;
  size_t Used;
};

// ---------------------------------------------------------------------------
// RSA public-key operation: Signature^65537 mod Modulus (big-endian byte strings). Plain schoolbook arithmetic on 32-bit
// limbs; only public values are involved, so it need not be constant time.

namespace Detail {
using Limbs = std::vector<uint32_t>;  // little-endian

inline Limbs FromBytes(std::span<const uint8_t> Bytes, size_t Count) {
  Limbs Out(Count, 0);
  for (size_t Index = 0; Index < Bytes.size(); ++Index) {
    const size_t Bit = (Bytes.size() - 1 - Index) * 8;
    Out[Bit / 32] |= uint32_t {Bytes[Index]} << (Bit % 32);
  }
  return Out;
}

// A >= B over the same number of limbs.
inline bool NotLess(const Limbs& A, const Limbs& B) {
  for (size_t Index = A.size(); Index-- > 0;)
    if (A[Index] != B[Index]) return A[Index] > B[Index];
  return true;
}

inline void Subtract(Limbs& A, const Limbs& B) {
  uint64_t Borrow = 0;
  for (size_t Index = 0; Index < A.size(); ++Index) {
    const uint64_t Value = uint64_t {A[Index]} - B[Index] - Borrow;
    A[Index] = static_cast<uint32_t>(Value);
    Borrow = (Value >> 32) & 1;
  }
}

// (A * B) mod N; A, B < N, all with K limbs.
inline Limbs MulMod(const Limbs& A, const Limbs& B, const Limbs& N) {
  const size_t K = N.size();
  Limbs Product(2 * K, 0);
  for (size_t I = 0; I < K; ++I) {
    uint64_t Carry = 0;
    for (size_t J = 0; J < K; ++J) {
      const uint64_t Value = uint64_t {A[I]} * B[J] + Product[I + J] + Carry;
      Product[I + J] = static_cast<uint32_t>(Value);
      Carry = Value >> 32;
    }
    Product[I + K] = static_cast<uint32_t>(Carry);
  }
  // Binary long division, keeping only the remainder (K + 1 limbs, always < 2N before the subtraction).
  Limbs Remainder(K + 1, 0), Modulus(N);
  Modulus.push_back(0);
  for (size_t Bit = 2 * K * 32; Bit-- > 0;) {
    uint32_t Carry = (Product[Bit / 32] >> (Bit % 32)) & 1;
    for (auto& Limb : Remainder) {
      const uint32_t Next = Limb >> 31;
      Limb = (Limb << 1) | Carry;
      Carry = Next;
    }
    if (NotLess(Remainder, Modulus)) Subtract(Remainder, Modulus);
  }
  Remainder.pop_back();
  return Remainder;
}
} // namespace Detail

// false when the signature is not smaller than the modulus.
inline bool RsaPublic(std::span<const uint8_t> Modulus, std::span<const uint8_t> Signature, std::vector<uint8_t>& Out) {
  while (!Modulus.empty() && Modulus.front() == 0) Modulus = Modulus.subspan(1);
  while (!Signature.empty() && Signature.front() == 0) Signature = Signature.subspan(1);
  if (Modulus.empty() || Signature.size() > Modulus.size()) return false;
  const size_t K = (Modulus.size() + 3) / 4;
  const auto N = Detail::FromBytes(Modulus, K);
  const auto S = Detail::FromBytes(Signature, K);
  if (Detail::NotLess(S, N)) return false;
  auto X = S;
  for (int Square = 0; Square < 16; ++Square) X = Detail::MulMod(X, X, N);
  X = Detail::MulMod(X, S, N);
  Out.assign(Modulus.size(), 0);
  for (size_t Index = 0; Index < Out.size(); ++Index) {
    const size_t Bit = (Out.size() - 1 - Index) * 8;
    Out[Index] = static_cast<uint8_t>(X[Bit / 32] >> (Bit % 32));
  }
  return true;
}

// ---------------------------------------------------------------------------
// Cleartext signature framework and armor.

struct Clearsigned {
  std::string Text;                 // the signed lines, dash-escaping removed, joined with "\n" (with a final "\n")
  std::string Canonical;            // what the signature covers: trailing blanks removed, "\r\n" between lines, none after the last
  std::vector<uint8_t> Signature;   // the armored packets, decoded
};

inline bool Base64Decode(std::string_view Text, std::vector<uint8_t>& Out) {
  uint32_t Buffer = 0;
  int Bits = 0;
  for (char C : Text) {
    int Value;
    if (C >= 'A' && C <= 'Z') Value = C - 'A';
    else if (C >= 'a' && C <= 'z') Value = C - 'a' + 26;
    else if (C >= '0' && C <= '9') Value = C - '0' + 52;
    else if (C == '+') Value = 62;
    else if (C == '/') Value = 63;
    else if (C == '=') break;
    else return false;
    Buffer = (Buffer << 6) | uint32_t(Value);
    Bits += 6;
    if (Bits >= 8) {
      Bits -= 8;
      Out.push_back(static_cast<uint8_t>(Buffer >> Bits));
    }
  }
  return true;
}

// Returns an error text, or nullptr.
inline const char* ParseClearsigned(std::string_view Input, Clearsigned& Out) {
  std::vector<std::string_view> Lines;
  for (size_t Start = 0; Start < Input.size();) {
    auto End = Input.find('\n', Start);
    if (End == Input.npos) End = Input.size();
    auto Line = Input.substr(Start, End - Start);
    if (!Line.empty() && Line.back() == '\r') Line.remove_suffix(1);
    Lines.push_back(Line);
    Start = End + 1;
  }
  size_t Index = 0;
  if (Lines.empty() || Lines[Index++] != "-----BEGIN PGP SIGNED MESSAGE-----") return "not a clear-signed message";
  while (Index < Lines.size() && !Lines[Index].empty()) {
    if (!Lines[Index].starts_with("Hash: ")) return "unexpected cleartext header";
    ++Index;
  }
  if (Index++ >= Lines.size()) return "truncated cleartext header";
  Out = {};
  bool First = true;
  for (;; ++Index) {
    if (Index >= Lines.size()) return "no signature block";
    auto Line = Lines[Index];
    if (Line == "-----BEGIN PGP SIGNATURE-----") break;
    if (Line.starts_with("- ")) Line.remove_prefix(2);
    else if (Line.starts_with("-")) return "unescaped dash line in the signed text";
    Out.Text.append(Line).push_back('\n');
    while (!Line.empty() && (Line.back() == ' ' || Line.back() == '\t')) Line.remove_suffix(1);
    if (!First) Out.Canonical += "\r\n";
    Out.Canonical.append(Line);
    First = false;
  }
  ++Index;
  // Armor headers up to a blank line, then base64 up to the checksum line or the end line.
  while (Index < Lines.size() && !Lines[Index].empty()) {
    if (Lines[Index].find(": ") == std::string_view::npos) break;  // no headers and no blank line: tolerated like GnuPG
    ++Index;
  }
  if (Index < Lines.size() && Lines[Index].empty()) ++Index;
  std::string Body;
  for (;; ++Index) {
    if (Index >= Lines.size()) return "truncated signature block";
    const auto Line = Lines[Index];
    if (Line == "-----END PGP SIGNATURE-----") break;
    if (Line.starts_with("=")) continue;  // CRC-24 of the armor; the signature itself is what is checked
    Body.append(Line);
  }
  if (!Base64Decode(Body, Out.Signature) || Out.Signature.empty()) return "invalid signature armor";
  return nullptr;
}

// ---------------------------------------------------------------------------
// Signature packets.

// EMSA-PKCS1-v1_5 DigestInfo prefix for SHA-512 (RFC 8017 9.2).
inline constexpr uint8_t Sha512DigestInfo[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                               0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};

// One version 4 signature packet body against the canonical text and an RSA modulus (e = 65537).
inline bool VerifySignaturePacket(std::span<const uint8_t> Body, std::string_view Canonical, std::span<const uint8_t> Modulus) {
  if (Body.size() < 6 || Body[0] != 4 || Body[1] != 0x01 || Body[2] != 1 || Body[3] != 10) return false;
  const size_t Hashed = size_t {Body[4]} << 8 | Body[5];
  if (6 + Hashed + 2 > Body.size()) return false;
  const size_t Unhashed = size_t {Body[6 + Hashed]} << 8 | Body[7 + Hashed];
  size_t Offset = 8 + Hashed + Unhashed;
  if (Offset + 4 > Body.size()) return false;
  const uint8_t Left[2] = {Body[Offset], Body[Offset + 1]};
  Offset += 2;
  const size_t Bits = size_t {Body[Offset]} << 8 | Body[Offset + 1];
  Offset += 2;
  const size_t Bytes = (Bits + 7) / 8;
  if (Offset + Bytes != Body.size()) return false;

  Sha512 Hash;
  Hash.Update(Canonical);
  Hash.Update(Body.data(), 6 + Hashed);
  const uint32_t Length = static_cast<uint32_t>(6 + Hashed);
  const uint8_t Trailer[6] = {0x04, 0xff, static_cast<uint8_t>(Length >> 24), static_cast<uint8_t>(Length >> 16),
                              static_cast<uint8_t>(Length >> 8), static_cast<uint8_t>(Length)};
  Hash.Update(Trailer, sizeof(Trailer));
  const auto Digest = Hash.Final();
  if (Digest[0] != Left[0] || Digest[1] != Left[1]) return false;

  std::vector<uint8_t> Encoded;
  if (!RsaPublic(Modulus, Body.subspan(Offset, Bytes), Encoded)) return false;
  // 00 01 FF..FF 00 DigestInfo Digest, as long as the modulus, with at least 8 bytes of FF.
  const size_t Fixed = 3 + sizeof(Sha512DigestInfo) + Digest.size();
  if (Encoded.size() < Fixed + 8 || Encoded[0] != 0 || Encoded[1] != 1) return false;
  const size_t Pad = Encoded.size() - Fixed;
  for (size_t Index = 0; Index < Pad; ++Index)
    if (Encoded[2 + Index] != 0xff) return false;
  if (Encoded[2 + Pad] != 0) return false;
  if (std::memcmp(Encoded.data() + 3 + Pad, Sha512DigestInfo, sizeof(Sha512DigestInfo)) != 0) return false;
  return std::memcmp(Encoded.data() + 3 + Pad + sizeof(Sha512DigestInfo), Digest.data(), Digest.size()) == 0;
}

// True when one of the signature packets is a valid signature of the canonical text by one of the keys.
inline bool VerifyPackets(std::span<const uint8_t> Packets, std::string_view Canonical, std::span<const std::span<const uint8_t>> Keys) {
  size_t Offset = 0;
  while (Offset < Packets.size()) {
    const uint8_t Tag = Packets[Offset++];
    if (!(Tag & 0x80)) return false;
    unsigned Type;
    size_t Length;
    if (Tag & 0x40) {  // new format
      Type = Tag & 0x3f;
      if (Offset >= Packets.size()) return false;
      const uint8_t First = Packets[Offset++];
      if (First < 192) Length = First;
      else if (First < 224) {
        if (Offset >= Packets.size()) return false;
        Length = ((size_t {First} - 192) << 8) + Packets[Offset++] + 192;
      } else if (First == 255) {
        if (Offset + 4 > Packets.size()) return false;
        Length = size_t {Packets[Offset]} << 24 | size_t {Packets[Offset + 1]} << 16 | size_t {Packets[Offset + 2]} << 8 | Packets[Offset + 3];
        Offset += 4;
      } else {
        return false;  // partial lengths are not used for signatures
      }
    } else {  // old format
      Type = (Tag >> 2) & 0x0f;
      const unsigned LengthType = Tag & 3;
      if (LengthType == 3) return false;
      const size_t Count = size_t {1} << LengthType;
      if (Offset + Count > Packets.size()) return false;
      Length = 0;
      for (size_t Index = 0; Index < Count; ++Index) Length = Length << 8 | Packets[Offset + Index];
      Offset += Count;
    }
    if (Length > Packets.size() - Offset) return false;
    if (Type == 2)
      for (const auto& Key : Keys)
        if (VerifySignaturePacket(Packets.subspan(Offset, Length), Canonical, Key)) return true;
    Offset += Length;
  }
  return false;
}

// Parses and verifies a clear-signed message. On success Text holds only the signed text (anything outside the signed
// part is dropped). Returns an error text, or nullptr.
inline const char* VerifyClearsigned(std::string_view Input, std::span<const std::span<const uint8_t>> Keys, std::string& Text) {
  Clearsigned Message;
  if (const auto* Error = ParseClearsigned(Input, Message)) return Error;
  if (!VerifyPackets(Message.Signature, Message.Canonical, Keys)) return "no valid signature by a trusted key";
  Text = std::move(Message.Text);
  return nullptr;
}

} // namespace Switch2Guest::OpenPgp
