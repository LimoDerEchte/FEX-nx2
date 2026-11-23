// SPDX-License-Identifier: MIT
#include "../GuestCredentials.h"

#include <cstdio>
#include <cstdlib>
#include <optional>

#define CHECK(Cond) do { if (!(Cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #Cond); std::exit(1); } } while (0)

using namespace Switch2Guest;

static Credentials User(uint32_t R, uint32_t E, uint32_t S) {
  Credentials C;
  C.Uid = {R, E, S, E};
  C.Gid = {R, E, S, E};
  return C;
}

int main() {
  // brief examples
  {
    auto C = Credentials {};
    CHECK(SetUid(C, 0) == -1);
    C.Uid = {1000, 1000, 2000, 1000};
    CHECK(SetUid(C, 2000) == 0 && C.Uid[1] == 2000 && C.Uid[0] == 1000);
    auto R = RootCredentials();
    CHECK(SetResUid(R, 1000, 1000, 1000) == 0 && R.CapPermitted == 0 && R.CapEffective == 0);
    auto K = RootCredentials();
    K.Securebits = SecbitKeepCaps;
    CHECK(SetResUid(K, 1000, 1000, 1000) == 0 && K.CapPermitted == CapFull && K.CapEffective == 0);
    CHECK(SetUid(C, NoId) == -22);
  }
  CHECK(CapFull == 0x1ffffffffffull);
  {
    auto R = RootCredentials();
    CHECK(R.Uid[0] == 0 && R.Uid[3] == 0 && R.Gid[2] == 0 && R.CapEffective == CapFull && R.CapPermitted == CapFull &&
          R.CapBounding == CapFull && R.Capable(CapKill));
  }
  // setuid
  {
    auto R = RootCredentials();
    CHECK(SetUid(R, 5) == 0 && R.Uid == (std::array<uint32_t, 4> {5, 5, 5, 5}) && R.CapPermitted == 0);
    auto C = User(1, 2, 3);
    CHECK(SetUid(C, 3) == 0 && C.Uid == (std::array<uint32_t, 4> {1, 3, 3, 3}));
    auto D = User(1, 2, 3);
    CHECK(SetUid(D, 1) == 0 && D.Uid[1] == 1 && D.Uid[3] == 1);
    auto E = User(1, 2, 3);
    CHECK(SetUid(E, 9) == -1 && E.Uid == (std::array<uint32_t, 4> {1, 2, 3, 2}));
  }
  // setreuid
  {
    auto C = User(1, 2, 3);
    CHECK(SetReUid(C, 9, NoId) == -1);
    CHECK(SetReUid(C, NoId, 9) == -1);
    CHECK(SetReUid(C, 2, 3) == 0 && C.Uid == (std::array<uint32_t, 4> {2, 3, 3, 3}));
    auto D = User(1, 2, 3);
    CHECK(SetReUid(D, NoId, 1) == 0 && D.Uid[1] == 1 && D.Uid[2] == 3); // e == old ruid, r == -1: suid unchanged
    auto E = User(1, 2, 4);
    CHECK(SetReUid(E, NoId, 2) == 0 && E.Uid[1] == 2 && E.Uid[2] == 2 && E.Uid[3] == 2); // finding 5: e != old ruid -> suid = e
    auto E2 = User(1, 2, 3);
    CHECK(SetReUid(E2, NoId, 3) == 0 && E2.Uid[1] == 3 && E2.Uid[2] == 3 && E2.Uid[3] == 3);
    auto F = User(1, 2, 3);
    CHECK(SetReUid(F, 2, NoId) == 0 && F.Uid[0] == 2 && F.Uid[2] == 2 && F.Uid[3] == 2);
    auto R = RootCredentials();
    CHECK(SetReUid(R, 7, 8) == 0 && R.Uid == (std::array<uint32_t, 4> {7, 8, 8, 8}));
  }
  // setresuid
  {
    auto C = User(1, 2, 3);
    CHECK(SetResUid(C, 9, NoId, NoId) == -1);
    CHECK(SetResUid(C, NoId, NoId, 9) == -1);
    CHECK(SetResUid(C, 3, 1, 2) == 0 && C.Uid == (std::array<uint32_t, 4> {3, 1, 2, 1}));
    auto D = User(1, 2, 3);
    CHECK(SetResUid(D, NoId, NoId, NoId) == 0 && D.Uid[1] == 2);
    auto R = RootCredentials();
    CHECK(SetResUid(R, 7, 8, 9) == 0 && R.Uid == (std::array<uint32_t, 4> {7, 8, 9, 8}));
  }
  // setfsuid
  {
    auto C = User(1, 2, 3);
    CHECK(SetFsUid(C, 3) == 2 && C.Uid[3] == 3);
    CHECK(SetFsUid(C, 9) == 3 && C.Uid[3] == 3);
    CHECK(SetFsUid(C, NoId) == 3 && C.Uid[3] == 3);
    auto R = RootCredentials();
    CHECK(SetFsUid(R, 9) == 0 && R.Uid[3] == 9);
  }
  // group variants use CAP_SETGID
  {
    auto C = Credentials {};
    CHECK(SetGid(C, NoId) == -22);
    CHECK(SetGid(C, 0) == -1);
    C.Gid = {1000, 1000, 2000, 1000};
    CHECK(SetGid(C, 2000) == 0 && C.Gid[1] == 2000 && C.Gid[0] == 1000);
    auto D = User(1, 2, 3);
    CHECK(SetReGid(D, 9, NoId) == -1 && SetReGid(D, 2, 3) == 0 && D.Gid == (std::array<uint32_t, 4> {2, 3, 3, 3}));
    auto E = User(1, 2, 3);
    CHECK(SetResGid(E, 9, NoId, NoId) == -1 && SetResGid(E, 3, 1, 2) == 0 && E.Gid == (std::array<uint32_t, 4> {3, 1, 2, 1}));
    auto F = User(1, 2, 3);
    CHECK(SetFsGid(F, 3) == 2 && F.Gid[3] == 3 && SetFsGid(F, 9) == 3 && F.Gid[3] == 3);
    // CAP_SETGID without CAP_SETUID
    auto G = User(1, 2, 3);
    G.CapEffective = 1ull << CapSetgid;
    CHECK(SetGid(G, 9) == 0 && G.Gid == (std::array<uint32_t, 4> {9, 9, 9, 9}));
    CHECK(SetUid(G, 9) == -1);
    // gid changes never touch caps
    auto R = RootCredentials();
    CHECK(SetResGid(R, 5, 5, 5) == 0 && R.CapPermitted == CapFull && R.CapEffective == CapFull);
  }
  // FixCapsAfterUidChange
  {
    auto C = RootCredentials();
    C.Uid = {1000, 0, 0, 0};
    auto Old = C.Uid;
    C.Uid = {1000, 1000, 1000, 1000};
    C.CapAmbient = 1;
    FixCapsSetxuid(C, Old);
    CHECK(C.CapPermitted == 0 && C.CapEffective == 0 && C.CapAmbient == 0);

    auto E = RootCredentials(); // old euid 0 -> nonzero but ruid stays 0: clear effective only
    auto OldE = E.Uid;
    E.Uid = {0, 1000, 0, 0};
    FixCapsSetxuid(E, OldE);
    CHECK(E.CapEffective == 0 && E.CapPermitted == CapFull);

    auto F = RootCredentials(); // old euid nonzero -> 0: effective = permitted
    F.Uid = {0, 1000, 0, 1000};
    F.CapEffective = 0;
    auto OldF = F.Uid;
    F.Uid = {0, 0, 0, 0};
    FixCapsSetxuid(F, OldF);
    CHECK(F.CapEffective == CapFull);

    auto G = RootCredentials(); // fsuid 0 -> nonzero clears fs caps only
    auto OldG = G.Uid;
    G.Uid = {0, 0, 0, 1000};
    FixCapsFs(G, OldG);
    CHECK(!G.Capable(CapChown) && !G.Capable(CapDacOverride) && !G.Capable(2) && !G.Capable(3) && !G.Capable(4) &&
          !G.Capable(9) && !G.Capable(27) && !G.Capable(32) && G.Capable(CapKill) && G.CapPermitted == CapFull);

    auto H = RootCredentials(); // fsuid nonzero -> 0 restores fs caps from permitted
    H.Uid = {0, 0, 0, 1000};
    H.CapEffective = CapFull & ~0x1ull;
    auto OldH = H.Uid;
    H.Uid = {0, 0, 0, 0};
    FixCapsFs(H, OldH);
    CHECK(H.Capable(CapChown) && H.Capable(CapKill));

    auto K = RootCredentials(); // KeepCaps blocks the all-nonzero clear (permitted kept)
    K.Securebits = SecbitKeepCaps;
    K.CapAmbient = 1;
    auto OldK = K.Uid;
    K.Uid = {1, 1, 1, 1};
    FixCapsSetxuid(K, OldK);
    CHECK(K.CapPermitted == CapFull && K.CapEffective == 0);
    CHECK(K.CapAmbient == 0); // finding 1: ambient cleared even with KEEP_CAPS
  }
  // ApplyExecCaps
  {
    auto C = User(1000, 1000, 1000);
    C.CapPermitted = CapFull;
    C.CapEffective = CapFull;
    C.CapAmbient = 1ull << CapKill;
    ApplyExecCaps(C, std::nullopt, false);
    CHECK(C.CapPermitted == (1ull << CapKill) && C.CapEffective == (1ull << CapKill));

    auto R = RootCredentials();
    R.CapBounding = CapFull & ~(1ull << CapSetpcap);
    R.CapPermitted = R.CapEffective = 0;
    R.Securebits = SecbitKeepCaps | SecbitKeepCapsLocked;
    ApplyExecCaps(R, std::nullopt, false);
    CHECK(R.CapPermitted == R.CapBounding && R.CapEffective == R.CapPermitted && R.Securebits == SecbitKeepCapsLocked);

    auto S = User(1000, 1000, 1000);
    ApplyExecCaps(S, 0u, false);
    CHECK(S.Uid[1] == 0 && S.Uid[0] == 1000 && S.CapPermitted == CapFull && S.CapEffective == CapFull);

    auto N = User(1000, 1000, 1000);
    ApplyExecCaps(N, 0u, true);
    CHECK(N.Uid[1] == 1000 && N.CapPermitted == 0 && N.CapEffective == 0);

    auto RR = User(0, 1000, 0); // real root, euid dropped: permitted full, effective only ambient
    ApplyExecCaps(RR, std::nullopt, false);
    CHECK(RR.CapPermitted == CapFull && RR.CapEffective == 0);

    auto NR = RootCredentials(); // NoNewPrivs does not strip real root
    ApplyExecCaps(NR, std::nullopt, true);
    CHECK(NR.CapPermitted == CapFull && NR.CapEffective == CapFull);
  }
  // findings 2,3,4,6,7
  {
    auto R = RootCredentials(); // 2: NO_SETUID_FIXUP
    R.Securebits = SecbitNoSetuidFixup;
    CHECK(SetResUid(R, 1000, 1000, 1000) == 0 && R.CapPermitted == CapFull && R.CapEffective == CapFull);
    auto F = RootCredentials();
    F.Securebits = SecbitNoSetuidFixup;
    CHECK(SetFsUid(F, 1000) == 0 && F.Uid[3] == 1000 && F.Capable(CapChown));

    auto N = RootCredentials(); // 2: NOROOT disables root rule on exec
    N.Securebits = SecbitNoRoot;
    N.CapPermitted = N.CapEffective = 0;
    ApplyExecCaps(N, std::nullopt, false);
    CHECK(N.CapPermitted == 0 && N.CapEffective == 0);

    auto S = RootCredentials(); // 3: fs fixup only in setfsuid
    CHECK(SetFsUid(S, 1000) == 0 && !S.Capable(CapChown) && S.Capable(CapKill));
    CHECK(SetUid(S, 0) == 0 && S.Uid[3] == 0 && !S.Capable(CapChown));
    CHECK(SetFsUid(S, 5) == 0 && SetFsUid(S, 0) == 5 && S.Capable(CapChown)); // fsuid nonzero->0 restores

    auto I = User(1000, 1000, 1000); // 4: root permitted = bset | inheritable
    I.CapBounding = 1;
    I.CapInheritable = 1ull << 5;
    I.CapPermitted = I.CapEffective = 0;
    I.Uid = {0, 0, 0, 0};
    ApplyExecCaps(I, std::nullopt, false);
    CHECK(I.CapPermitted == (1 | (1ull << 5)) && I.CapEffective == I.CapPermitted);

    auto A = User(1000, 1000, 1000); // 4: setid clears ambient, owner nonroot sets euid/suid/fsuid
    A.CapAmbient = A.CapPermitted = A.CapInheritable = 1ull << CapKill;
    ApplyExecCaps(A, 2000u, false);
    CHECK(A.Uid == (std::array<uint32_t, 4> {1000, 2000, 2000, 2000}) && A.CapAmbient == 0 && A.CapPermitted == 0);
    auto B = User(1000, 1000, 1000);
    B.CapAmbient = 1ull << CapKill;
    ApplyExecCaps(B, std::nullopt, false, true); // setgid exec
    CHECK(B.CapAmbient == 0 && B.CapPermitted == 0);
    auto G = User(1000, 1000, 1000);
    ApplyExecCaps(G, std::nullopt, false, true, 50u);
    CHECK(G.Gid == (std::array<uint32_t, 4> {1000, 50, 50, 50}));
    auto Nn = User(1000, 1000, 1000);
    Nn.CapAmbient = 1ull << CapKill;
    ApplyExecCaps(Nn, 2000u, true, true, 50u); // NNP: owners ignored, ambient kept
    CHECK(Nn.Uid[1] == 1000 && Nn.Gid[1] == 1000 && Nn.CapAmbient == (1ull << CapKill) && Nn.CapPermitted == (1ull << CapKill));

    auto Z = RootCredentials(); // 6: setresuid(1000,1000,-1) keeps suid 0, permitted, clears effective
    CHECK(SetResUid(Z, 1000, 1000, NoId) == 0 && Z.Uid == (std::array<uint32_t, 4> {1000, 1000, 0, 1000}) &&
          Z.CapPermitted == CapFull && Z.CapEffective == 0);

    auto M = RootCredentials(); // 6: capset ambient intersection
    M.CapAmbient = 0xF;
    CHECK(CapSet(M, 0, 0x7, 0x3) == 0 && M.CapAmbient == 0x3);

    auto U = User(1, 2, 3); // 6: failed calls leave creds unmodified
    const auto Orig = U;
    CHECK(SetReUid(U, 9, 2) == -1 && U.Uid == Orig.Uid && U.CapEffective == Orig.CapEffective);
    CHECK(SetResUid(U, 1, 2, 9) == -1 && U.Uid == Orig.Uid);
    CHECK(SetFsUid(U, 9) == 2 && U.Uid == Orig.Uid && SetFsUid(U, 2) == 2 && U.Uid == Orig.Uid);
  }
  // CapSet
  {
    auto C = RootCredentials();
    C.CapInheritable = 0;
    CHECK(CapSet(C, 1, 3, 2) == 0 && C.CapEffective == 1 && C.CapPermitted == 3 && C.CapInheritable == 2);
    CHECK(CapSet(C, 0, 7, 0) == -1);                // permitted cannot grow
    CHECK(CapSet(C, 4, 3, 0) == -1);                // effective not subset of permitted
    CHECK(CapSet(C, 0, 3, 3) == 0);                 // inheritable within old (inh|perm)

    auto D = User(1, 1, 1);
    D.CapPermitted = 1ull << CapKill;
    D.CapEffective = 1ull << CapKill;
    CHECK(CapSet(D, 0, D.CapPermitted, 1ull << CapChown) == -1); // not in perm|inh, no SETPCAP
    CHECK(CapSet(D, 0, D.CapPermitted, 1ull << CapKill) == 0);

    auto P = User(1, 1, 1);
    P.CapPermitted = P.CapEffective = 1ull << CapSetpcap;
    CHECK(CapSet(P, P.CapEffective, P.CapPermitted, 1ull << CapChown) == 0); // SETPCAP: bounding allows it
    auto Q = User(1, 1, 1);
    Q.CapPermitted = Q.CapEffective = 1ull << CapSetpcap;
    Q.CapBounding = 1ull << CapSetpcap;
    CHECK(CapSet(Q, Q.CapEffective, Q.CapPermitted, 1ull << CapChown) == -1);
  }
  std::puts("GuestCredentials: OK");
}
