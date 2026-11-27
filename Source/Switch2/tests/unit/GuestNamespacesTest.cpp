// SPDX-License-Identifier: MIT
// Host tests of GuestNamespaces.h. Expectations were checked against native Linux 7.3 (uid 1000 and `unshare -r`);
// the sequences are those of the native probes named in ContainerInventory.md.
#include "GuestNamespaces.h"
#include "GuestCredentials.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                                     \
  do {                                                               \
    if (!(x)) {                                                      \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);        \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

using namespace Switch2Guest;
using namespace Switch2Guest::Ns;

static Subject User(Registry& R, uint32_t Uid = 1000, uint32_t Gid = 1000) {
  return {R.Initial.User, 0, Uid, Gid};
}
static Subject RootSubject(Registry& R) { return {R.Initial.User, CapInitialRoot, 0, 0}; }

static std::shared_ptr<UserNs> Create(Registry& R, const Subject& Who) {
  auto Out = CreateUserNs(R, Who, false);
  CHECK(std::holds_alternative<std::shared_ptr<UserNs>>(Out));
  return std::get<std::shared_ptr<UserNs>>(Out);
}
// The subject right after unshare(CLONE_NEWUSER): full capabilities in the new namespace.
static Subject Inside(const std::shared_ptr<UserNs>& Ns, uint32_t Euid, uint32_t Egid) { return {Ns, CapFull, Euid, Egid}; }

int main() {
  // ---- id maps
  {
    IdMap Map {{{0, 1000, 1}, {10, 5000, 100}}};
    CHECK(Map.ToKernel(0) == 1000u && Map.ToKernel(1) == std::nullopt && Map.ToKernel(10) == 5000u && Map.ToKernel(109) == 5099u);
    CHECK(Map.ToKernel(110) == std::nullopt);
    CHECK(Map.FromKernel(1000) == 0u && Map.FromKernel(5050) == 60u && Map.FromKernel(4999) == std::nullopt);
    CHECK(Map.RangeToKernel(10, 100) == 5000u && Map.RangeToKernel(9, 2) == std::nullopt && Map.RangeToKernel(0, 2) == std::nullopt);
    const auto Identity = IdMap::Identity();
    CHECK(Identity.ToKernel(0) == 0u && Identity.ToKernel(4294967294u) == 4294967294u && Identity.ToKernel(NoId) == std::nullopt);
    CHECK(Identity.RangeToKernel(0, UINT32_MAX) == 0u);
  }

  // ---- the text of a map write (native: uid_map/gid_map writes as uid 1000 after unshare(CLONE_NEWUSER))
  {
    CHECK(ParseIdMapText("0 1000 1")->size() == 1);
    CHECK((*ParseIdMapText("5 1000 1\n"))[0].First == 5);
    CHECK(ParseIdMapText("0 1000 1\n10 2000 5\n")->size() == 2);
    CHECK(ParseIdMapText("  0\t1000   1  \n")->size() == 1);
    CHECK(!ParseIdMapText("5 1000 0\n"));            // count 0: EINVAL
    CHECK(!ParseIdMapText("3 1000 1 junk\n"));       // a fourth field: EINVAL
    CHECK(!ParseIdMapText("3 1000\n"));              // a missing field
    CHECK(!ParseIdMapText("x 1000 1\n"));            // not a number
    CHECK(!ParseIdMapText("-1 1000 1\n"));
    CHECK(!ParseIdMapText("5 1000 1\n0 1000 1\n"));  // overlapping lower range (native: EINVAL)
    CHECK(!ParseIdMapText("5 1000 3\n6 2000 1\n"));  // overlapping namespace range
    CHECK(!ParseIdMapText("0 1000 1\n\n"));          // an empty line
    CHECK(!ParseIdMapText("4294967295 0 1\n"));      // the invalid id
    CHECK(!ParseIdMapText("0 0 4294967296\n"));      // out of range
    CHECK(ParseIdMapText("0 0 4294967295\n")->size() == 1);
    CHECK(!ParseIdMapText("1 0 4294967295\n"));      // wraps
    CHECK(ParseIdMapText("")->empty());
    std::string Many;
    for (int I = 0; I < 340; ++I) Many += std::to_string(I * 2) + " " + std::to_string(I * 2) + " 1\n";
    CHECK(ParseIdMapText(Many)->size() == 340);
    Many += "800 800 1\n";
    CHECK(!ParseIdMapText(Many));                    // 341 extents
  }

  // ---- unshare(CLONE_NEWUSER) as uid 1000, then the map writes of the native probe, in order
  {
    Registry R;
    const auto Creator = User(R);
    auto Ns = Create(R, Creator);
    CHECK(Ns->Level == 1 && Ns->Parent == R.Initial.User && Ns->OwnerUid == 1000 && Ns->OwnerGid == 1000);
    CHECK(!Ns->ParentCouldSetfcap);
    const auto Me = Inside(Ns, 1000, 1000);
    // Before any map: ids are overflowed, root is unmapped, setgroups is refused.
    CHECK(MungeUid(*Ns, 1000) == 65534 && MungeGid(*Ns, 1000) == 65534 && RootKuid(*Ns) == NoId);
    CHECK(!MaySetgroups(*Ns));
    CHECK(RenderSetgroups(*Ns) == "allow\n" && RenderIdMap(*Ns, false, *Ns).empty());
    // gid_map before "deny": EPERM (native).
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, "0 1000 1") == ErrPerm);
    CHECK(WriteSetgroups(*Ns, 0, "bogus") == ErrInval);
    CHECK(WriteSetgroups(*Ns, 1, "deny") == ErrInval);
    CHECK(WriteSetgroups(*Ns, 0, "denyX") == ErrInval && WriteSetgroups(*Ns, 0, "allow  x") == ErrInval);
    CHECK(WriteSetgroups(*Ns, 0, "toolongtext") == ErrInval);
    CHECK(WriteSetgroups(*Ns, 0, "deny") == 4 && RenderSetgroups(*Ns) == "deny\n" && !Ns->SetgroupsAllowed);
    // More than one id, an id that is not the caller's, bad text, a wrong offset: native errnos.
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, "0 1000 2") == ErrPerm);
    CHECK(WriteIdMap(*Ns, true, Me, Me, 1, "0 1000 1\n") == ErrInval);
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, "5 1000 0\n") == ErrInval);
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, "5 1001 1\n") == ErrPerm);
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, "5 1000 1\n0 1000 1\n") == ErrInval);
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, "") == ErrInval);
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, std::string(4096, ' ')) == ErrInval);
    CHECK(Ns->Gid.Empty());
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, "5 1000 1\n") == 9);
    CHECK(RenderIdMap(*Ns, true, *Ns) == "         5       1000          1\n");
    CHECK(RenderIdMap(*Ns, true, *R.Initial.User) == "         5       1000          1\n");
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, "6 1000 1\n") == ErrPerm); // one write only
    CHECK(WriteSetgroups(*Ns, 0, "allow") == ErrPerm);                // cannot re-enable (native)
    CHECK(WriteSetgroups(*Ns, 0, "deny\n") == ErrPerm);               // deny after the gid map: EPERM (native)
    CHECK(!MaySetgroups(*Ns));                                        // denied
    // uid_map: two ids and the parent's root are EPERM; trailing junk EINVAL; the caller's own id works.
    CHECK(WriteIdMap(*Ns, false, Me, Me, 0, "0 1000 2\n") == ErrPerm);
    CHECK(WriteIdMap(*Ns, false, Me, Me, 0, "0 0 1\n") == ErrPerm);
    CHECK(WriteIdMap(*Ns, false, Me, Me, 0, "3 1000 1 junk\n") == ErrInval);
    CHECK(WriteIdMap(*Ns, false, Me, Me, 0, "3 1000 1\n") == 9);
    CHECK(WriteIdMap(*Ns, false, Me, Me, 0, "4 1000 1\n") == ErrPerm);
    CHECK(MakeKuid(*Ns, 3) == 1000u && MakeKuid(*Ns, 0) == std::nullopt && MungeUid(*Ns, 1000) == 3 && MungeUid(*Ns, 0) == 65534);
    CHECK(MungeGid(*Ns, 1000) == 5 && MungeGid(*Ns, 1001) == 65534);
    CHECK(RootKuid(*Ns) == NoId);
  }

  // ---- bwrap with --dev: a nested namespace inherits "deny" and maps the sandbox ids back onto its parent's root
  {
    Registry R;
    auto Outer = Create(R, User(R));
    const auto Me = Inside(Outer, 1000, 1000);
    CHECK(WriteSetgroups(*Outer, 0, "deny\n") == 5);
    CHECK(WriteIdMap(*Outer, false, Me, Me, 0, "0 1000 1\n") == 9 && WriteIdMap(*Outer, true, Me, Me, 0, "0 1000 1\n") == 9);
    auto Inner = Create(R, Me);
    CHECK(!Inner->SetgroupsAllowed && RenderSetgroups(*Inner) == "deny\n");
    const auto InnerMe = Inside(Inner, 1000, 1000);
    CHECK(WriteIdMap(*Inner, false, InnerMe, InnerMe, 0, "1000 0 1\n") == 9);
    CHECK(WriteIdMap(*Inner, true, InnerMe, InnerMe, 0, "1000 0 1\n") == 9);
    // From the initial namespace (setgroups allowed there) the child starts with "allow".
    CHECK(Create(R, User(R))->SetgroupsAllowed);
  }

  // ---- unshare -r: map root to the caller; setgroups stays allowed only with privilege over the parent
  {
    Registry R;
    auto Ns = Create(R, User(R));
    const auto Me = Inside(Ns, 1000, 1000);
    CHECK(WriteSetgroups(*Ns, 0, "deny\n") == 5);
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, "0 1000 1\n") == 9);
    CHECK(WriteIdMap(*Ns, false, Me, Me, 0, "0 1000 1\n") == 9);
    CHECK(RootKuid(*Ns) == 1000u && MungeUid(*Ns, 1000) == 0);
    // Someone in the namespace without the capability cannot write.
    auto Other = Create(R, User(R));
    const Subject Weak {Other, 0, 1000, 1000};
    CHECK(WriteIdMap(*Other, false, Weak, Weak, 0, "0 1000 1\n") == ErrPerm);
    // A subject outside the namespace and its parent is refused outright.
    auto Third = Create(R, User(R));
    const auto Stranger = Inside(Third, 1000, 1000);
    CHECK(WriteIdMap(*Other, false, Stranger, Stranger, 0, "0 1000 1\n") == ErrPerm);
  }

  // ---- privileged parent: root of the initial namespace may map anything, mapping uid 0 needs CAP_SETFCAP
  {
    Registry R;
    const auto Root = RootSubject(R);
    auto Ns = Create(R, Root);
    CHECK(Ns->ParentCouldSetfcap && Ns->OwnerUid == 0);
    CHECK(WriteIdMap(*Ns, true, Root, Root, 0, "0 0 10\n100000 100000 65536\n") > 0); // gid map as root of the parent
    CHECK(WriteIdMap(*Ns, false, Root, Root, 0, "0 0 1\n1 100000 65536\n") > 0);
    CHECK(Ns->Uid.ToKernel(2) == 100001u && Ns->Uid.ToKernel(65537) == std::nullopt);
    // A reader in the initial namespace sees what the extent holds; the namespace itself sees its parent's ids.
    CHECK(RenderIdMap(*Ns, false, *R.Initial.User) == "         0          0          1\n         1     100000      65536\n");
    // Without CAP_SETFCAP in the parent, mapping its uid 0 is refused even with CAP_SETUID.
    Subject NoFcap {R.Initial.User, CapInitialRoot & ~(1ull << CapSetfcap), 0, 0};
    auto Ns2 = Create(R, NoFcap);
    CHECK(!Ns2->ParentCouldSetfcap);
    const auto Own = Inside(Ns2, 0, 0);
    CHECK(WriteIdMap(*Ns2, false, Own, NoFcap, 0, "0 1 1\n") == ErrPerm);    // opened inside: no CAP_SETUID over the parent
    CHECK(WriteIdMap(*Ns2, false, NoFcap, NoFcap, 0, "0 0 1\n") == ErrPerm); // the parent's root without CAP_SETFCAP
    CHECK(WriteIdMap(*Ns2, false, NoFcap, NoFcap, 0, "0 1 1\n") == 6);
    // A nested namespace maps through its parent's map: a range outside the parent's extents is EPERM.
    auto Child = Create(R, Inside(Ns, 100000, 100000));
    const auto ChildMe = Inside(Child, 100000, 100000);
    CHECK(Child->Level == 2 && Child->OwnerUid == 100000u);
    const Subject InNs {Ns, CapFull, 100000u, 100000u};
    CHECK(WriteIdMap(*Child, false, InNs, InNs, 0, "0 0 70000\n") == ErrPerm); // 70000 > the parent's extent
    CHECK(WriteIdMap(*Child, false, InNs, InNs, 0, "0 1 10\n") == 7);           // parent ids 1..10 = kernel 100000..
    CHECK(Child->Uid.ToKernel(3) == 100003u);
    (void)ChildMe;
  }

  // ---- create_user_ns refusals
  {
    Registry R;
    // An unmapped euid or egid cannot create (native: unshare(CLONE_NEWUSER) inside a fresh namespace is EPERM).
    auto Ns = Create(R, User(R));
    const auto Unmapped = Inside(Ns, 1000, 1000);
    CHECK(std::get<int64_t>(CreateUserNs(R, Unmapped, false)) == ErrPerm);
    CHECK(std::get<int64_t>(CreateUserNs(R, User(R), true)) == ErrPerm); // chrooted
    // The depth limit: 33 nested namespaces exist (levels 1..33), the next creation gives EUSERS.
    Subject Who = RootSubject(R);
    for (int I = 0; I < 33; ++I) {
      auto Made = CreateUserNs(R, Who, false);
      CHECK(std::holds_alternative<std::shared_ptr<UserNs>>(Made));
      auto Level = std::get<std::shared_ptr<UserNs>>(Made);
      CHECK(Level->Level == static_cast<uint32_t>(I + 1));
      Level->Uid = Level->Gid = IdMap::Identity();
      Who = Inside(Level, 0, 0);
    }
    CHECK(std::get<int64_t>(CreateUserNs(R, Who, false)) == ErrUsers);
  }

  // ---- capabilities in a namespace (cap_capable)
  {
    Registry R;
    const auto Root = RootSubject(R);
    auto Ns = Create(R, User(R, 1000, 1000));
    Ns->Uid = Ns->Gid = IdMap::Identity();
    const auto Owner = Subject {R.Initial.User, 0, 1000, 1000};
    CHECK(NsCapable(Root, *R.Initial.User, 21) && NsCapable(Root, *Ns, 21));           // an ancestor's capability reaches down
    CHECK(!NsCapable(Owner, *R.Initial.User, 21));
    CHECK(NsCapable(Owner, *Ns, 21) && NsCapable(Owner, *Ns, 0));                      // the creator's euid owns every capability
    const Subject Other {R.Initial.User, 0, 1001, 1000};
    CHECK(!NsCapable(Other, *Ns, 21));
    const auto In = Inside(Ns, 5, 5);
    CHECK(NsCapable(In, *Ns, 21) && !NsCapable(In, *R.Initial.User, 21));              // nothing upwards
    auto Grand = Create(R, In);
    CHECK(NsCapable(In, *Grand, 21));                                                  // the capability of its ancestor
    const Subject Weak {Ns, 0, 6, 6};
    CHECK(!NsCapable(Weak, *Grand, 21));                                               // not the owner, no capability
    const Subject WeakOwner {Ns, 0, 5, 5};
    CHECK(NsCapable(WeakOwner, *Grand, 21));                                           // the owner needs none
  }

  // ---- credentials in a new user namespace
  {
    Registry R;
    Credentials C = RootCredentials();
    CHECK(C.Capable(21) && C.OwnCapable(21) && !C.Capable(22));       // CAP_SYS_BOOT is outside CapInitialRoot
    auto Ns = Create(R, RootSubject(R));
    Ns->Uid = IdMap {{{0, 0, 1}}};
    EnterUserNs(C, Ns);
    CHECK(!C.InitialNs && !C.Capable(21) && C.OwnCapable(21) && C.OwnCapable(22));
    CHECK(C.CapEffective == CapFull && C.CapBounding == CapFull && C.CapInheritable == 0 && C.RootUid == 0);
    // A namespace whose uid 0 maps elsewhere moves root's treatment to that kernel id.
    Credentials D;
    D.Uid = {1000, 1000, 1000, 1000};
    auto Mapped = Create(R, User(R));
    CHECK(WriteSetgroups(*Mapped, 0, "deny") > 0);
    const auto Me = Inside(Mapped, 1000, 1000);
    CHECK(WriteIdMap(*Mapped, true, Me, Me, 0, "0 1000 1") > 0 && WriteIdMap(*Mapped, false, Me, Me, 0, "0 1000 1") > 0);
    EnterUserNs(D, Mapped);
    CHECK(D.RootUid == 1000u);
    // EnterUserNs on a namespace whose map is still empty leaves no root; RootKuid follows the map once it is written.
    auto Late = Create(R, User(R));
    CHECK(WriteSetgroups(*Late, 0, "deny") > 0);
    Credentials Fresh;
    Fresh.Uid = {1000, 1000, 1000, 1000};
    EnterUserNs(Fresh, Late);
    CHECK(Fresh.RootUid == NoId && RootKuid(*Late) == NoId);
    const auto LateMe = Inside(Late, 1000, 1000);
    CHECK(WriteIdMap(*Late, true, LateMe, LateMe, 0, "0 1000 1") > 0 && WriteIdMap(*Late, false, LateMe, LateMe, 0, "0 1000 1") > 0);
    CHECK(Fresh.RootUid == NoId && RootKuid(*Late) == 1000u); // the stale copy is why the runtime re-syncs after the write
    // setuid inside: the privilege is the namespace's own capability.
    CHECK(SetUid(D, 1234) == 0 && D.Uid[1] == 1234 && D.CapEffective == 0); // dropped root: caps cleared
    Credentials E;
    E.Uid = {1000, 1000, 1000, 1000};
    EnterUserNs(E, Mapped);
    // exec in the namespace: root (the kernel id the namespace maps to 0) gets the full set.
    E.CapPermitted = E.CapEffective = 0;
    ApplyExecCaps(E, std::nullopt, false);
    CHECK(E.CapEffective == CapFull);
  }

  // ---- clone and unshare flags
  {
    Registry R;
    const auto Root = RootSubject(R);
    const auto Mine = R.Initial;
    // Unprivileged: every namespace except a user namespace is EPERM; time is EINVAL.
    for (auto Flag : {CloneNewNs, CloneNewUts, CloneNewIpc, CloneNewPid, CloneNewNet, CloneNewCgroup}) {
      CHECK(std::get<int64_t>(BuildNamespaces(R, Mine, User(R), Flag, false)) == ErrPerm);
    }
    CHECK(std::get<int64_t>(BuildNamespaces(R, Mine, User(R), CloneNewTime, false)) == ErrInval);
    CHECK(std::get<int64_t>(BuildNamespaces(R, Mine, Root, CloneNewTime, false)) == ErrInval);
    // Network cannot be isolated: EPERM even for root, even together with a user namespace.
    CHECK(std::get<int64_t>(BuildNamespaces(R, Mine, Root, CloneNewNet, false)) == ErrPerm);
    CHECK(std::get<int64_t>(BuildNamespaces(R, Mine, User(R), CloneNewUser | CloneNewNet, false)) == ErrPerm);
    // Unprivileged CLONE_NEWUSER with the rest: the new namespace owns them.
    {
      auto Out = BuildNamespaces(R, Mine, User(R), CloneNewUser | CloneNewUts | CloneNewIpc | CloneNewPid | CloneNewNs | CloneNewCgroup, false);
      CHECK(std::holds_alternative<Switch>(Out));
      const auto& S = std::get<Switch>(Out);
      CHECK(S.NewUser && S.Ns.User == S.NewUser && S.Ns.Uts != Mine.Uts && S.Ns.Uts->Owner == S.NewUser);
      CHECK(S.Ns.Ipc != Mine.Ipc && S.Ns.Ipc->Owner == S.NewUser && S.Ns.Cgroup != Mine.Cgroup && S.Ns.Mnt != Mine.Mnt && S.NewMnt);
      CHECK(S.Ns.Pid == Mine.Pid && S.Ns.PidForChildren != Mine.Pid && S.Ns.PidForChildren->Level == 1);
      CHECK(S.Ns.PidForChildren->Parent == Mine.Pid && S.Ns.Net == Mine.Net);
      CHECK(S.Who.Ns == S.NewUser && S.Who.CapEffective == CapFull);
    }
    // A privileged call creates objects owned by the caller's namespace and copies the host and domain name.
    {
      NsSet Old = Mine;
      Old.Uts->Hostname = "fex";
      Old.Uts->Domainname = "dom";
      auto Out = BuildNamespaces(R, Old, Root, CloneNewUts, false);
      const auto& S = std::get<Switch>(Out);
      CHECK(!S.NewUser && S.Ns.Uts != Old.Uts && S.Ns.Uts->Hostname == "fex" && S.Ns.Uts->Domainname == "dom" && S.Ns.Uts->Owner == Old.User);
      CHECK(S.Ns.Uts->Ino != Old.Uts->Ino);
    }
    // A user namespace from an unmapped euid fails first.
    {
      auto Ns = Create(R, User(R));
      NsSet Old = Mine;
      Old.User = Ns;
      CHECK(std::get<int64_t>(BuildNamespaces(R, Old, Inside(Ns, 1000, 1000), CloneNewUser, false)) == ErrPerm);
      CHECK(std::get<int64_t>(BuildNamespaces(R, Old, User(R), CloneNewUser, true)) == ErrPerm);
    }
    // CLONE_NEWPID twice without a fork in between: EINVAL. Past 32 pid levels: ENOSPC.
    {
      auto First = std::get<Switch>(BuildNamespaces(R, Mine, Root, CloneNewPid, false));
      CHECK(std::get<int64_t>(BuildNamespaces(R, First.Ns, Root, CloneNewPid, false)) == ErrInval);
      NsSet Deep = Mine;
      for (uint32_t Level = 1; Level <= MaxPidNsLevel; ++Level) {
        auto Next = std::make_shared<PidNs>(R.NextIno(), Mine.User);
        Next->Parent = Deep.PidForChildren;
        Next->Level = Level;
        Deep.Pid = Deep.PidForChildren = Next;
      }
      CHECK(std::get<int64_t>(BuildNamespaces(R, Deep, Root, CloneNewPid, false)) == ErrNoSpc);
    }
    // No flags: nothing changes.
    {
      auto Out = std::get<Switch>(BuildNamespaces(R, Mine, User(R), 0, false));
      CHECK(Out.Ns.Uts == Mine.Uts && !Out.NewUser);
    }
  }

  // ---- pid namespaces: numbering, visibility, init
  {
    Registry R;
    auto Root = R.Initial.Pid;
    CHECK(Root->Allocate(105) == 105 && Root->ToLocal(105) == 105 && Root->ToGlobal(7) == 7);
    auto One = std::make_shared<PidNs>(R.NextIno(), R.Initial.User);
    One->Parent = Root;
    One->Level = 1;
    auto Two = std::make_shared<PidNs>(R.NextIno(), R.Initial.User);
    Two->Parent = One;
    Two->Level = 2;
    CHECK(One->Allocate(200) == 1 && One->Init == 200 && One->IsInitOf(200) && !One->IsInitOf(201) && !Root->IsInitOf(200));
    CHECK(One->Allocate(201) == 2);
    // A task of the inner namespace is numbered in both; ancestors see it, descendants do not see ancestors.
    CHECK(Two->Allocate(300) == 1 && One->ToLocal(300) == 3 && Two->ToLocal(300) == 1 && Root->ToLocal(300) == 300);
    CHECK(Two->ToLocal(200) == 0 && Two->ToLocal(105) == 0 && One->ToLocal(105) == 0);
    CHECK(One->ToGlobal(3) == 300 && One->ToGlobal(1) == 200 && One->ToGlobal(9) == 0 && Two->ToGlobal(1) == 300);
    CHECK(Two->Init == 300 && One->Init == 200);
    CHECK(Root->Contains(*Two) && One->Contains(*Two) && !Two->Contains(*One) && One->Contains(*One));
    // A task whose creation failed gives its numbers back (the init slot too).
    auto Three = std::make_shared<PidNs>(R.NextIno(), R.Initial.User);
    Three->Parent = Root;
    Three->Level = 1;
    CHECK(Three->Allocate(400) == 1 && Three->Init == 400);
    Three->Release(400);
    CHECK(Three->Init == 0 && Three->ToLocal(400) == 0 && Three->ToGlobal(1) == 0 && Three->Allocate(401) == 1 && Three->Init == 401);
    // Retire (a reaped task): the maps shrink in this namespace and its ancestors; the counter and Init stay.
    CHECK(Three->Allocate(402) == 2);
    Three->Retire(402);
    CHECK(Three->ToLocal(402) == 0 && Three->ToGlobal(2) == 0 && Three->Allocate(403) == 3);
    Three->Retire(401);
    CHECK(Three->ToLocal(401) == 0 && Three->Init == 401 && Three->IsInitOf(401));
    One->Retire(300);
    CHECK(One->ToLocal(300) == 0 && Two->ToLocal(300) == 1); // Two is a child of One here: retiring from One's view leaves Two's map
    Two->Retire(300);
    CHECK(Two->ToLocal(300) == 0 && One->ToLocal(300) == 0);
    Three->Retire(999); // unknown ids are ignored
  }

  // ---- signals to the init of a pid namespace (native: p2 mode 0)
  {
    CHECK(!PidInitDropsSignal(false, true, true, 15));      // an ordinary process
    CHECK(PidInitDropsSignal(true, true, true, 15));        // SIGTERM from inside: ignored
    CHECK(PidInitDropsSignal(true, true, true, 9));         // SIGKILL from inside: ignored
    CHECK(PidInitDropsSignal(true, true, true, 19));        // SIGSTOP from inside: ignored
    CHECK(PidInitDropsSignal(true, true, false, 15));       // SIGTERM from the parent namespace: ignored
    CHECK(!PidInitDropsSignal(true, true, false, 9));       // SIGKILL from the parent namespace: delivered
    CHECK(!PidInitDropsSignal(true, true, false, 19));      // SIGSTOP from the parent namespace: delivered
    CHECK(!PidInitDropsSignal(true, false, true, 15));      // a handler: delivered
  }

  // ---- setns installation checks (native: p1 modes 9 and 10)
  {
    Registry R;
    auto Child = Create(R, User(R));
    const auto In = Inside(Child, 1000, 1000);
    NsSet MySet = R.Initial;
    MySet.User = Child;
    // Entering the namespace you are in: EINVAL; entering another one needs CAP_SYS_ADMIN over it.
    CHECK(CheckInstall(MySet, In, *Child, true, false) == ErrInval);
    CHECK(CheckInstall(MySet, In, *R.Initial.User, true, false) == ErrPerm);
    const auto Out = User(R);
    NsSet Init = R.Initial;
    CHECK(CheckInstall(Init, Out, *Child, true, false) == 0);                   // the owner enters its namespace
    CHECK(CheckInstall(Init, Out, *Child, false, false) == ErrInval);            // threaded
    CHECK(CheckInstall(Init, Out, *Child, true, true) == ErrInval);              // fs shared
    CHECK(CheckInstall(Init, Out, *R.Initial.User, true, false) == ErrInval);    // own namespace (native: uid 1000 EINVAL)
    CHECK(CheckInstall(Init, Out, *R.Initial.Uts, true, false) == ErrPerm);      // no capability
    CHECK(CheckInstall(Init, RootSubject(R), *R.Initial.Uts, true, false) == 0);
    CHECK(CheckInstall(Init, RootSubject(R), *R.Initial.Mnt, true, false) == 0);
    CHECK(CheckInstall(Init, RootSubject(R), *R.Initial.Mnt, true, true) == ErrInval);
    CHECK(CheckInstall(Init, Out, *R.Initial.Pid, true, false) == ErrPerm);      // native: setns(pidfd) EPERM
    // A uts namespace owned by the child: the subject needs capabilities in both.
    auto Uts = std::make_shared<UtsNs>(R.NextIno(), Child);
    CHECK(CheckInstall(Init, Out, *Uts, true, false) == ErrPerm);                // no CAP_SYS_ADMIN in its own namespace
    CHECK(CheckInstall(Init, Subject {R.Initial.User, CapInitialRoot, 0, 0}, *Uts, true, false) == 0);
    // After setns(user) in the same call the subject is the new one with full capabilities.
    CHECK(CheckInstall(Init, In, *Uts, true, false) == 0);
    // pid namespaces: only descendants of the active one.
    auto Level1 = std::make_shared<PidNs>(R.NextIno(), R.Initial.User);
    Level1->Parent = R.Initial.Pid;
    Level1->Level = 1;
    auto Level2 = std::make_shared<PidNs>(R.NextIno(), R.Initial.User);
    Level2->Parent = Level1;
    Level2->Level = 2;
    NsSet InLevel1 = Init;
    InLevel1.Pid = InLevel1.PidForChildren = Level1;
    const auto Rt = RootSubject(R);
    CHECK(CheckInstall(Init, Rt, *Level2, true, false) == 0 && CheckInstall(Init, Rt, *Level1, true, false) == 0);
    CHECK(CheckInstall(InLevel1, Rt, *Level2, true, false) == 0 && CheckInstall(InLevel1, Rt, *Level1, true, false) == 0);
    CHECK(CheckInstall(InLevel1, Rt, *R.Initial.Pid, true, false) == ErrInval); // an ancestor
    NsSet Install = Init;
    ::Switch2Guest::Ns::Install(Install, Level2);
    CHECK(Install.PidForChildren == Level2 && Install.Pid == Init.Pid);
    ::Switch2Guest::Ns::Install(Install, Uts);
    CHECK(Install.Uts == Uts);
  }

  // ---- nsfs ioctls (native: p1 mode 9)
  {
    Registry R;
    auto Child = Create(R, User(R));
    Child->Uid = Child->Gid = IdMap::Identity();
    auto Grand = Create(R, Inside(Child, 1, 1));
    // NS_GET_USERNS: the owner must be the caller's namespace or a descendant of it.
    CHECK(std::get<int64_t>(NsOwner(*R.Initial.User, *R.Initial.User)) == ErrPerm);          // the initial namespace has no owner
    CHECK(std::get<std::shared_ptr<UserNs>>(NsOwner(*R.Initial.Uts, *R.Initial.User)) == R.Initial.User); // uid 1000 reads uts' owner
    CHECK(std::get<int64_t>(NsOwner(*R.Initial.Uts, *Child)) == ErrPerm);                    // from inside: the owner is an ancestor
    CHECK(std::get<std::shared_ptr<UserNs>>(NsOwner(*Child, *R.Initial.User)) == R.Initial.User);
    CHECK(std::get<int64_t>(NsOwner(*Child, *Child)) == ErrPerm);                            // native: ioctl(u2, NS_GET_USERNS) EPERM
    CHECK(std::get<std::shared_ptr<UserNs>>(NsOwner(*Grand, *Child)) == Child);
    // NS_GET_PARENT: user and pid namespaces only.
    CHECK(std::get<int64_t>(NsParent(*R.Initial.Uts, *R.Initial.User, *R.Initial.Pid)) == ErrInval);
    CHECK(std::get<int64_t>(NsParent(*R.Initial.User, *R.Initial.User, *R.Initial.Pid)) == ErrPerm);
    CHECK(std::get<std::shared_ptr<Namespace>>(NsParent(*Child, *R.Initial.User, *R.Initial.Pid)) == R.Initial.User);
    CHECK(std::get<int64_t>(NsParent(*Child, *Child, *R.Initial.Pid)) == ErrPerm);
    CHECK(std::get<int64_t>(NsParent(*R.Initial.Pid, *R.Initial.User, *R.Initial.Pid)) == ErrPerm);
    auto Pid1 = std::make_shared<PidNs>(R.NextIno(), Child);
    Pid1->Parent = R.Initial.Pid;
    Pid1->Level = 1;
    CHECK(std::get<std::shared_ptr<Namespace>>(NsParent(*Pid1, *R.Initial.User, *R.Initial.Pid)) == R.Initial.Pid);
    CHECK(std::get<int64_t>(NsParent(*Pid1, *R.Initial.User, *Pid1)) == ErrPerm);
    CHECK(FlagOf(Kind::User) == CloneNewUser && FlagOf(Kind::Uts) == CloneNewUts && FlagOf(Kind::Mnt) == CloneNewNs);
    CHECK(NsGetNstype == 0xb703 && NsGetOwnerUid == 0xb704);
  }

  // ---- /proc/<pid>/ns links
  {
    Registry R;
    CHECK(*NsLinkText(R.Initial, "user") == "user:[4026531837]" && *NsLinkText(R.Initial, "uts") == "uts:[4026531838]");
    CHECK(*NsLinkText(R.Initial, "ipc") == "ipc:[4026531839]" && *NsLinkText(R.Initial, "pid") == "pid:[4026531836]");
    CHECK(*NsLinkText(R.Initial, "pid_for_children") == "pid:[4026531836]" && *NsLinkText(R.Initial, "net") == "net:[4026531833]");
    CHECK(*NsLinkText(R.Initial, "mnt") == "mnt:[4026531832]" && *NsLinkText(R.Initial, "cgroup") == "cgroup:[4026531835]");
    CHECK(*NsLinkText(R.Initial, "time") == "time:[4026531834]" && !NsLinkText(R.Initial, "bogus"));
    auto Out = std::get<Switch>(BuildNamespaces(R, R.Initial, RootSubject(R), CloneNewPid | CloneNewUts, false));
    CHECK(*NsLinkText(Out.Ns, "pid") == "pid:[4026531836]" && *NsLinkText(Out.Ns, "pid_for_children") != "pid:[4026531836]");
    CHECK(NsLinkTarget(Out.Ns, "uts") == Out.Ns.Uts && NsLinkTarget(Out.Ns, "time") == nullptr && NsLinkTarget(Out.Ns, "x") == nullptr);
    CHECK(Out.Ns.Get(Kind::Pid) == Out.Ns.Pid && Out.Ns.Get(Kind::Time) == nullptr);
  }

  // ---- sethostname and setdomainname (native: p1 modes 3 and 4)
  {
    Registry R;
    UtsNs Initial(1, R.Initial.User);
    CHECK(CheckSetName(User(R), Initial, 5) == ErrPerm);                      // uid 1000 in the initial namespace
    CHECK(CheckSetName(RootSubject(R), Initial, 5) == 5);
    auto Child = Create(R, User(R));
    UtsNs Mine(2, Child);
    const auto In = Inside(Child, 1000, 1000);
    CHECK(CheckSetName(In, Mine, 5) == 5 && CheckSetName(In, Mine, 64) == 64 && CheckSetName(In, Mine, 0) == 0);
    CHECK(CheckSetName(In, Mine, 65) == ErrInval && CheckSetName(In, Mine, static_cast<uint64_t>(-1)) == ErrInval);
    CHECK(CheckSetName(In, Mine, 0x100000001ULL) == 1);                       // the length is an int
    CHECK(CheckSetName(In, Initial, 5) == ErrPerm);                           // the owner is the initial namespace
    CHECK(CheckSetName(User(R), Mine, 5) == 5);                               // the creator in the parent owns the namespace
    CHECK(CheckSetName(User(R), Mine, 65) == ErrInval);                       // permission first, then length
    CHECK(CheckSetName(User(R, 1001), Mine, 65) == ErrPerm);
  }

  // ---- console 1 (item 5): the sequence of ns/user_map_rules as real root of the initial namespace
  {
    Registry R;
    auto Ns = Create(R, RootSubject(R));
    const auto Me = Inside(Ns, 0, 0);
    // Written from inside, before "deny": CAP_SETGID over the parent is missing, the own-id shortcut needs "deny".
    CHECK(WriteIdMap(*Ns, true, Me, Me, 0, "0 0 1") == ErrPerm);
    CHECK(WriteSetgroups(*Ns, 0, "deny") == 4);
    CHECK(WriteIdMap(*Ns, false, Me, Me, 0, "0 0 1") == 5);
    CHECK(WriteIdMap(*Ns, false, Me, Me, 0, "0 0 1") == ErrPerm);
    CHECK(WriteSetgroups(*Ns, 0, "allow") == ErrPerm);
  }

  // ---- console 1 (items 2, 6, 7): "/proc/self" read through the initial proc mount from inside a new PID
  // namespace is the caller's number in the mount's namespace (its global id), not its own number there (1): the
  // link text must come from the namespace of the mount being walked.
  {
    Registry R;
    auto Child = std::make_shared<PidNs>(R.NextIno(), R.Initial.User);
    Child->Parent = R.Initial.Pid;
    Child->Level = 1;
    CHECK(Child->Allocate(4242) == 1);
    CHECK(R.Initial.Pid->ToLocal(4242) == 4242u && Child->ToLocal(4242) == 1u);
    // Reading /proc/1/... through the initial mount names the global pid 1, which is not the caller.
    CHECK(R.Initial.Pid->ToGlobal(Child->ToLocal(4242)) == 1u && R.Initial.Pid->ToGlobal(R.Initial.Pid->ToLocal(4242)) == 4242u);
  }

  std::puts("guest namespaces ok");
  return 0;
}
