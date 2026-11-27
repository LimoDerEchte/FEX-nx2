// SPDX-License-Identifier: MIT
// Host tests of GuestMounts.h. Expectations were checked against native Linux 7.3 under `unshare -rm` (small C
// programs; the sequences are noted per block and listed in ContainerInventory.md).
#include "GuestMounts.h"
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
using namespace Switch2Guest::Mounts;

static const std::string R = "/fex/rootfs";

static std::shared_ptr<Superblock> Sd(Ids& I, Ns::Registry& Reg) {
  auto Sb = std::make_shared<Superblock>();
  Sb->Dev = 1;
  Sb->Type = "msdos";
  Sb->Source = "sdmc";
  Sb->FsRoot = R;
  Sb->Owner = Reg.Initial.User;
  (void)I;
  return Sb;
}

struct World {
  Ns::Registry Reg;
  Ids I;
  Table T;
  MountRef Rootfs, Slash;
  World() {
    Rootfs = MakeMount(T, I, nullptr, "", R, Sd(I, Reg), 0);
    Slash = MakeMount(T, I, Rootfs, R, R, Sd(I, Reg), MntRelatime);
  }
  Place Root() { return {Slash, R}; }
  Place At(const MountRef& M, std::string P) { return {M, std::move(P)}; }
  MountRef Tmpfs(const MountRef& Parent, const std::string& Where, std::string_view Data = "", uint64_t Ms = 0, uint32_t Uid = 0) {
    auto Parsed = ParseFsOptions("tmpfs", Data, 1u << 30);
    CHECK(std::holds_alternative<FsOptions>(Parsed));
    const auto F = FlagsFromMs(Ms);
    const auto Sb = MakeSuperblock(I, "tmpfs", "", std::get<FsOptions>(Parsed), F, Reg.Initial.User, "/fex/.tmpfs/" + std::to_string(I.NextDev), Uid, Uid);
    MountRef Out;
    CHECK(NewMount(T, I, {Parent, Where}, true, Sb, F.Mnt, &Out) == 0);
    return Out;
  }
};

int main() {
  // ---- flags (path_mount)
  {
    CHECK(FlagsFromMs(0).Mnt == MntRelatime);
    CHECK(FlagsFromMs(MsNoatime).Mnt == MntNoatime);
    CHECK(FlagsFromMs(MsNoatime | MsRelatime).Mnt == MntNoatime);          // native: noatime
    CHECK(FlagsFromMs(MsNoatime | MsStrictatime).Mnt == 0);                // native: no atime flag
    CHECK(FlagsFromMs(MsNodiratime).Mnt == (MntNodiratime | MntRelatime)); // native: nodiratime,relatime
    CHECK(FlagsFromMs(MsStrictatime).Mnt == 0);
    const auto F = FlagsFromMs(MsRdonly | MsNosuid | MsNodev | MsNoexec | MsSynchronous | MsLazytime);
    CHECK(F.Mnt == (MntReadonly | MntNosuid | MntNodev | MntNoexec | MntRelatime) && F.SbReadOnly && F.SbFlags == (SbSynchronous | SbLazytime));
  }

  // ---- tmpfs options (native: mount of tmpfs with data strings as uid 0 of a user namespace)
  {
    const auto Opt = [](std::string_view Data) { return ParseFsOptions("tmpfs", Data, 8ULL << 30); };
    const auto Ok = [&](std::string_view Data) { auto R = Opt(Data); CHECK(std::holds_alternative<FsOptions>(R)); return std::get<FsOptions>(R); };
    const auto Bad = [&](std::string_view Data) { auto R = Opt(Data); CHECK(std::holds_alternative<int64_t>(R) && std::get<int64_t>(R) == -22); };
    CHECK(Ok("").SizeBytes == 0);
    CHECK(Ok("size=64k").SizeBytes == 65536);
    CHECK(Ok("size=1m,nr_inodes=5,mode=0755,uid=0,gid=0").SizeBytes == (1u << 20));
    CHECK(Ok("size=1m,nr_inodes=5,mode=0755,uid=0,gid=0").NrInodes == 5 && *Ok("mode=0755").Mode == 0755);
    CHECK(Ok("size=1k,size=2k").SizeBytes == 4096);                        // native: size=4k (rounded to a page; last wins)
    CHECK(Ok("size=1g").SizeBytes == (1ULL << 30));
    CHECK(Ok("size=50%").SizeBytes == (4ULL << 30));
    CHECK(Ok("mode=0755,,size=1k").SizeBytes == 4096);                     // an empty item is skipped
    CHECK(*Ok("mode=0").Mode == 0 && *Ok("mode=1777").Mode == 01777);
    CHECK(Ok("nr_blocks=10").SizeBytes == 40960);
    CHECK(*Ok("uid=5,gid=6").Uid == 5 && *Ok("uid=5,gid=6").Gid == 6);
    Ok("huge=never,mpol=default,inode64,noswap");
    Bad("bogus=1"); Bad("size=abc"); Bad("mode=99999"); Bad("size="); Bad("mode=17777"); Bad("size=101%"); Bad("huge=bogus");
    Bad("uid=abc"); Bad("size"); Bad("inode64=1");
    CHECK(std::holds_alternative<int64_t>(ParseFsOptions("sysfs", "x", 0)) && std::holds_alternative<FsOptions>(ParseFsOptions("sysfs", "", 0)));
    CHECK(std::holds_alternative<int64_t>(ParseFsOptions("mqueue", "size=1", 0)));
    CHECK(std::holds_alternative<int64_t>(ParseFsOptions("proc", "x", 0)));  // native: proc with data "x" EINVAL
    CHECK(std::get<FsOptions>(ParseFsOptions("proc", "hidepid=2,gid=5", 0)).Extra == ",hidepid=2,gid=5");
    CHECK(std::holds_alternative<FsOptions>(ParseFsOptions("devpts", "ptmxmode=000,mode=600", 0)));
    CHECK(KnownFsType("tmpfs") && KnownFsType("proc") && KnownFsType("sysfs") && KnownFsType("devpts") && KnownFsType("mqueue") &&
          !KnownFsType("bogusfs") && !KnownFsType("ext4") && !KnownFsType(""));
  }

  // ---- new mount, bind, ro remount (native n2: bind of a tmpfs, MS_BIND|MS_RDONLY ignored on creation,
  // remount|bind|ro takes effect)
  {
    World W;
    auto A = W.Tmpfs(W.Slash, R + "/a", "size=64k");
    CHECK(A->Id == 3 && A->Parent == W.Slash && A->Sb->SizeBytes == 65536 && A->Sb->Dev == 40);
    MountRef B;
    CHECK(Bind(W.T, W.I, {A, A->Root}, true, {W.Slash, R + "/b"}, true, false, &B) == 0);
    CHECK(B->Sb == A->Sb && B->Root == A->Root && !(B->Flags & MntReadonly));
    // MS_BIND|MS_RDONLY on creation changes nothing: the caller does not pass MS_RDONLY into the clone
    RemountArgs Ro; Ro.Bind = true; Ro.Ms = MsRdonly;
    CHECK(Remount(W.T, {B, B->Root}, Ro) == 0 && B->ReadOnly() && !A->ReadOnly());
    // console 1, item 1: statvfs of a read-only bind mount reports ST_RDONLY (f_flags); its source does not
    CHECK((StatfsFlags(*B) & 1) && !(StatfsFlags(*A) & 1));
    CHECK(StatfsFlags(*B) == (1 | ((B->Flags & MntNosuid) ? 2 : 0) | ((B->Flags & MntNodev) ? 4 : 0) | ((B->Flags & MntNoexec) ? 8 : 0) |
                              ((B->Flags & MntNoatime) ? 0x400 : 0) | ((B->Flags & MntNodiratime) ? 0x800 : 0) | ((B->Flags & MntRelatime) ? 0x1000 : 0)));
    Ro.Writers = true;
    RemountArgs Rw; Rw.Bind = true; Rw.Ms = 0;
    CHECK(Remount(W.T, {B, B->Root}, Rw) == 0 && !B->ReadOnly());          // rw again; clearing needs no writers check
    CHECK(Remount(W.T, {B, B->Root}, Ro) == -16);                          // native: remount ro with a writer is EBUSY
    Ro.Writers = false;
    CHECK(Remount(W.T, {B, B->Root}, Ro) == 0);
    CHECK(Remount(W.T, {B, B->Root + "/x"}, Ro) == -22);                  // not a mount point
    // sb ro remount of the tmpfs: A and B both read-only through the superblock
    RemountArgs Sbro; Sbro.Ms = MsRdonly; Sbro.SbCapable = true;
    CHECK(Remount(W.T, {A, A->Root}, Sbro) == 0 && A->Sb->ReadOnly && A->ReadOnly());
    RemountArgs Back; Back.Bind = true; Back.Ms = 0;
    CHECK(Remount(W.T, {B, B->Root}, Back) == 0 && !(B->Flags & MntReadonly) && B->ReadOnly());   // native: b rw, sb ro
    CHECK(WriteCheck(*B) == -30 && CreateCheck(*A) == -30);
    RemountArgs Sbrw; Sbrw.Ms = 0; Sbrw.SbCapable = false;
    CHECK(Remount(W.T, {A, A->Root}, Sbrw) == -1);                         // no capability over the superblock owner
    Sbrw.SbCapable = true;
    CHECK(Remount(W.T, {A, A->Root}, Sbrw) == 0 && !A->ReadOnly());
    Sbrw.DataError = -22;
    CHECK(Remount(W.T, {A, A->Root}, Sbrw) == -22);
    // dir/file mismatch
    CHECK(Bind(W.T, W.I, {A, A->Root}, true, {W.Slash, R + "/f"}, false, false) == -20);
    CHECK(Bind(W.T, W.I, {A, A->Root}, false, {W.Slash, R + "/d"}, true, false) == -20);
    CHECK(NewMount(W.T, W.I, {W.Slash, R + "/f"}, false, A->Sb, 0) == -20);
  }

  // ---- CopyTable copies a re-parented mount after its new parent (move_mount puts a mount below a later one)
  {
    World W;
    auto A = W.Tmpfs(W.Slash, R + "/a", "", 0);
    auto B = W.Tmpfs(W.Slash, R + "/b", "", 0);
    CHECK(Move(W.T, {A, A->Root}, true, {B, B->Root}, true) == 0 && A->Parent == B);
    const auto IndexA = std::find(W.T.List.begin(), W.T.List.end(), A) - W.T.List.begin();
    const auto IndexB = std::find(W.T.List.begin(), W.T.List.end(), B) - W.T.List.begin();
    CHECK(IndexA < IndexB); // the child precedes its parent in the list
    std::vector<std::pair<MountRef, MountRef>> Map;
    auto Copy = CopyTable(W.T, W.I, false, &Map);
    CHECK(Copy->List.size() == W.T.List.size() && Map.size() == W.T.List.size());
    MountRef CopyA, CopyB;
    for (const auto& [Old, New] : Map) {
      if (Old == A) CopyA = New;
      if (Old == B) CopyB = New;
    }
    CHECK(CopyA && CopyB && CopyA->Parent == CopyB && CopyB->Parent && CopyB->Parent == Map[1].second);
    // the copy keeps the parent before the child, so the child is found below it
    const auto CopyIndexA = std::find(Copy->List.begin(), Copy->List.end(), CopyA) - Copy->List.begin();
    const auto CopyIndexB = std::find(Copy->List.begin(), Copy->List.end(), CopyB) - Copy->List.begin();
    CHECK(CopyIndexB < CopyIndexA);
  }

  // ---- locked flags in a mount namespace of a child user namespace (native: remount of inherited mounts)
  {
    World W;
    auto Ro = W.Tmpfs(W.Slash, R + "/ro", "", MsRdonly | MsNosuid);
    CHECK(Ro->Flags == (MntReadonly | MntNosuid | MntRelatime));
    std::vector<std::pair<MountRef, MountRef>> Map;
    auto Copy = CopyTable(W.T, W.I, true, &Map);
    CHECK(Copy->List.size() == W.T.List.size());
    const auto RoCopy = Map[2].second;
    CHECK(RoCopy->Id != Ro->Id && RoCopy->Sb == Ro->Sb && RoCopy->Parent == Map[1].second);
    CHECK(RoCopy->Locked && !Map[0].second->Locked && Map[1].second->Locked);
    CHECK((RoCopy->Locks & (LockReadonly | LockNosuid | LockAtime)) == (LockReadonly | LockNosuid | LockAtime) && !(RoCopy->Locks & LockNodev));
    RemountArgs Clear; Clear.Bind = true; Clear.Ms = MsNosuid;              // clears the read-only flag: locked
    CHECK(Remount(*Copy, {RoCopy, RoCopy->Root}, Clear) == -1);
    Clear.Ms = MsRdonly | MsNosuid | MsNodev;                              // may add nodev, keep the rest
    CHECK(Remount(*Copy, {RoCopy, RoCopy->Root}, Clear) == 0 && (RoCopy->Flags & MntNodev));
    Clear.Ms = MsRdonly | MsNosuid | MsNodev | MsNoatime;                  // the atime flags are locked
    CHECK(Remount(*Copy, {RoCopy, RoCopy->Root}, Clear) == -1);
    // a bind made in the copy is not locked and a copy to the same owner locks nothing
    MountRef Mine;
    CHECK(Bind(*Copy, W.I, {Map[1].second, R}, true, {Map[1].second, R + "/mine"}, true, false, &Mine) == 0);
    CHECK(!Mine->Locked && (Mine->Locks & LockAtime));
    auto Plain = CopyTable(W.T, W.I, false);
    CHECK(!Plain->List[2]->Locked && Plain->List[2]->Locks == 0);
    // a locked mount can be neither unmounted nor moved (native: EINVAL)
    UmountArgs U; U.Capable = true;
    CHECK(Umount(*Copy, {RoCopy, RoCopy->Root}, U, [](const MountRef&) { return false; }) == -22);
    CHECK(Move(*Copy, {RoCopy, RoCopy->Root}, true, {Map[1].second, R + "/z"}, true) == -22);
  }

  // ---- propagation (native: shared:N, master:N, unbindable, errors)
  {
    World W;
    auto B = W.Tmpfs(W.Slash, R + "/b");
    CHECK(ChangeType(W.T, W.I, {B, B->Root}, MsShared, false) == 0 && B->Group == 1);
    CHECK(MountInfo(W.T, W.Root()).find(" shared:1 - tmpfs") != std::string::npos);
    CHECK(ChangeType(W.T, W.I, {B, B->Root}, MsSlave, false) == 0 && !B->Group && !B->Master);   // alone: private
    CHECK(ChangeType(W.T, W.I, {B, B->Root}, MsUnbindable, false) == 0 && B->Unbindable);
    CHECK(MountInfo(W.T, W.Root()).find(" unbindable - tmpfs") != std::string::npos);
    CHECK(Bind(W.T, W.I, {B, B->Root}, true, {W.Slash, R + "/a"}, true, false) == -22);          // unbindable source
    CHECK(ChangeType(W.T, W.I, {B, B->Root}, MsPrivate, false) == 0 && !B->Unbindable);
    CHECK(ChangeType(W.T, W.I, {B, B->Root}, MsShared | MsPrivate, false) == -22);
    CHECK(ChangeType(W.T, W.I, {B, B->Root}, MsRdonly, false) == -22);
    CHECK(ChangeType(W.T, W.I, {B, B->Root + "/sub"}, MsPrivate, false) == -22);                  // not a mount point
    // peers: a bind of a shared mount joins its group; slave of a peer keeps master
    ChangeType(W.T, W.I, {B, B->Root}, MsShared, false);
    MountRef C;
    CHECK(Bind(W.T, W.I, {B, B->Root}, true, {W.Slash, R + "/c"}, true, false, &C) == 0 && C->Group == B->Group);
    CHECK(ChangeType(W.T, W.I, {C, C->Root}, MsSlave, false) == 0 && C->Master == B->Group && !C->Group);
    CHECK(MountInfo(W.T, W.Root()).find(" shared:2 - tmpfs") != std::string::npos && MountInfo(W.T, W.Root()).find(" master:2 - tmpfs") != std::string::npos);
    // recursive: all below
    auto Child = W.Tmpfs(B, B->Root + "/c");
    ChangeType(W.T, W.I, {B, B->Root}, MsShared | MsRec, true);
    CHECK(Child->Group != 0);
    ChangeType(W.T, W.I, {B, B->Root}, MsPrivate | MsRec, true);
    CHECK(!Child->Group && !B->Group);
    // copy to a less privileged namespace turns shared into slave
    ChangeType(W.T, W.I, {B, B->Root}, MsShared, false);
    auto Slaves = CopyTable(W.T, W.I, true);
    CHECK(Slaves->List[2]->Master == B->Group && !Slaves->List[2]->Group);
    auto Peers = CopyTable(W.T, W.I, false);
    CHECK(Peers->List[2]->Group == B->Group && !Peers->List[2]->Master);
    // pivot_root refuses a shared parent of the new root (here the root mount)
    ChangeType(W.T, W.I, {W.Slash, R}, MsShared, false);
    auto N = W.Tmpfs(W.Slash, R + "/n");
    W.Tmpfs(N, N->Root + "/old");
    CHECK(std::get<int64_t>(PivotRoot(W.T, W.Root(), {N, N->Root}, {N, N->Root + "/old"}, true)) == -22);
  }

  // ---- recursive bind, stacking, umount (native n3)
  {
    World W;
    auto A = W.Tmpfs(W.Slash, R + "/a");
    auto Sub = W.Tmpfs(A, A->Root + "/sub");
    auto Skip = W.Tmpfs(A, A->Root + "/skip");
    Skip->Unbindable = true;
    MountRef B;
    CHECK(Bind(W.T, W.I, {A, A->Root}, true, {W.Slash, R + "/b"}, true, false, &B) == 0);
    CHECK(W.T.ChildAt(B.get(), A->Root + "/sub") == nullptr && W.T.ChildAt(W.Slash.get(), R + "/b") == B);
    UmountArgs U; U.Capable = true;
    const auto Idle = [](const MountRef&) { return false; };
    CHECK(Umount(W.T, {B, B->Root}, U, Idle) == 0 && !W.T.Has(B.get()) && !B->Parent);
    CHECK(Bind(W.T, W.I, {A, A->Root}, true, {W.Slash, R + "/b"}, true, true, &B) == 0);
    CHECK(W.T.ChildAt(B.get(), A->Root + "/sub") != nullptr && W.T.ChildAt(B.get(), A->Root + "/sub")->Sb == Sub->Sb);
    CHECK(W.T.ChildAt(B.get(), A->Root + "/skip") == nullptr);             // unbindable children are skipped
    CHECK(Umount(W.T, {B, B->Root}, U, Idle) == -16);                      // native: busy with a child
    std::vector<MountRef> Removed;
    U.Flags = MntDetach;
    CHECK(Umount(W.T, {B, B->Root}, U, Idle, &Removed) == 0 && Removed.size() == 2 && !W.T.Has(B.get()));
    // stacking: the last mount on a point is on top, unmounting reveals the one below
    auto One = W.Tmpfs(W.Slash, R + "/d");
    auto Cover = Cross(W.T, {W.Slash, R + "/d"});
    CHECK(Cover.M == One && Cover.Path == One->Root);
    auto Two = W.Tmpfs(One, One->Root);                                     // mounted through the first, as the walk does
    CHECK(Two->Parent == One);
    CHECK(Cross(W.T, {W.Slash, R + "/d"}).M == Two);
    U.Flags = 0;
    CHECK(Umount(W.T, {Two, Two->Root}, U, Idle) == 0 && Cross(W.T, {W.Slash, R + "/d"}).M == One);
    // errors: not a mount point, root, force without capability, expiry
    CHECK(Umount(W.T, {One, One->Root + "/x"}, U, Idle) == -22);
    U.IsRoot = true;
    CHECK(Umount(W.T, {One, One->Root}, U, Idle) == -16);                  // the caller's root mount
    U.IsRoot = false;
    U.Flags = MntForce;
    CHECK(Umount(W.T, {One, One->Root}, U, Idle) == -1);                   // no capability over the superblock owner
    U.Flags = MntExpire | MntDetach;
    CHECK(Umount(W.T, {One, One->Root}, U, Idle) == -22);
    U.CapableForce = true;
    U.Flags = MntExpire | MntForce;
    CHECK(Umount(W.T, {One, One->Root}, U, Idle) == -22);
    U.Flags = 0x40;
    CHECK(Umount(W.T, {One, One->Root}, U, Idle) == -22);
    U.Flags = MntExpire;
    CHECK(Umount(W.T, {One, One->Root}, U, Idle) == -11 && One->ExpiryMark);   // first: EAGAIN
    CHECK(Cross(W.T, {W.Slash, R + "/d"}).M == One && !One->ExpiryMark);        // use clears the mark
    CHECK(Umount(W.T, {One, One->Root}, U, Idle) == -11);
    CHECK(Umount(W.T, {One, One->Root}, U, Idle) == 0);                     // second: unmounted
    U.Flags = 0; U.Capable = false;
    auto Three = W.Tmpfs(W.Slash, R + "/e");
    CHECK(Umount(W.T, {Three, Three->Root}, U, Idle) == -1);
    U.Capable = true;
    CHECK(Umount(W.T, {Three, Three->Root}, U, [](const MountRef&) { return true; }) == -16);   // busy by cwd or open file
    U.Flags = MntDetach;
    CHECK(Umount(W.T, {Three, Three->Root}, U, [](const MountRef&) { return true; }) == 0);
    // expirable children go first with a plain umount
    auto Parent = W.Tmpfs(W.Slash, R + "/p");
    auto Kid = W.Tmpfs(Parent, Parent->Root + "/k");
    U.Flags = 0;
    CHECK(Umount(W.T, {Parent, Parent->Root}, U, Idle) == -16 && W.T.Has(Kid.get()));
    U.Flags = MntExpire;
    CHECK(Umount(W.T, {Kid, Kid->Root}, U, Idle) == -11);
    U.Flags = 0;
    CHECK(Umount(W.T, {Parent, Parent->Root}, U, Idle) == 0 && !W.T.Has(Kid.get()));
  }

  // ---- move (native n3)
  {
    World W;
    auto A = W.Tmpfs(W.Slash, R + "/a");
    auto Sub = W.Tmpfs(A, A->Root + "/s");
    CHECK(Move(W.T, {A, A->Root}, true, {W.Slash, R + "/c"}, true) == 0 && A->Mountpoint == R + "/c" && Sub->Parent == A);
    CHECK(Move(W.T, {A, A->Root + "/x"}, true, {W.Slash, R + "/a"}, true) == -22);             // not a mount root
    CHECK(Move(W.T, {A, A->Root}, true, {A, A->Root + "/s"}, true) == -40);                    // into itself
    CHECK(Move(W.T, {A, A->Root}, true, {Sub, Sub->Root}, true) == -40);                       // below itself
    CHECK(Move(W.T, {A, A->Root}, true, {A, A->Root}, true) == -40);
    CHECK(Move(W.T, {A, A->Root}, true, {W.Slash, R + "/f"}, false) == -22);                   // dir onto file
    CHECK(Move(W.T, {W.Rootfs, R}, true, {W.Slash, R + "/f"}, true) == -22);                  // the top has no parent
    ChangeType(W.T, W.I, {W.Slash, R}, MsShared, false);
    CHECK(Move(W.T, {A, A->Root}, true, {W.Slash, R + "/g"}, true) == -22);                    // out of a shared parent
  }

  // ---- pivot_root (native n4)
  {
    World W;
    auto N = W.Tmpfs(W.Slash, R + "/new");
    auto Old = W.Tmpfs(N, N->Root + "/old");
    (void)Old;
    const auto Pivot = [&](const Place& Root, const Place& New, const Place& OldPlace) { return PivotRoot(W.T, Root, New, OldPlace, true); };
    // locked / capability
    CHECK(std::get<int64_t>(PivotRoot(W.T, W.Root(), {N, N->Root}, {N, N->Root + "/old"}, false)) == -1);
    // put_old not below new_root: EINVAL
    auto Z = W.Tmpfs(W.Slash, R + "/z");
    CHECK(std::get<int64_t>(Pivot(W.Root(), {N, N->Root}, {Z, Z->Root})) == -22);
    CHECK(std::get<int64_t>(Pivot(W.Root(), {N, N->Root}, {W.Slash, R + "/b"})) == -16);   // put_old on the root mount: EBUSY
    // new_root inside the root mount (not a mount point of its own): EBUSY here, EINVAL when the root mount is
    // locked (native, in a copied namespace: the locked check comes first)
    CHECK(std::get<int64_t>(Pivot(W.Root(), {W.Slash, R + "/a"}, {W.Slash, R + "/a/o"})) == -16);
    W.Slash->Locked = true;
    CHECK(std::get<int64_t>(Pivot(W.Root(), {W.Slash, R + "/a"}, {W.Slash, R + "/a/o"})) == -22);
    W.Slash->Locked = false;
    // new_root a directory in another mount, not its root: EINVAL
    CHECK(std::get<int64_t>(Pivot(W.Root(), {N, N->Root + "/dir"}, {N, N->Root + "/dir/o"})) == -22);
    // new_root is the root mount: EBUSY
    CHECK(std::get<int64_t>(Pivot(W.Root(), {W.Slash, R}, {W.Slash, R + "/x"})) == -16);
    // the root mount has no parent in a plain top: refuse
    CHECK(std::get<int64_t>(Pivot({W.Rootfs, R}, {N, N->Root}, {N, N->Root + "/old"})) == -22);
    // success
    const auto OldRootMount = W.Slash;
    auto Result = Pivot(W.Root(), {N, N->Root}, {N, N->Root + "/old"});
    CHECK(std::holds_alternative<PivotResult>(Result));
    CHECK(N->Parent == W.Rootfs && N->Mountpoint == R);
    CHECK(OldRootMount->Parent == N && OldRootMount->Mountpoint == N->Root + "/old");
    // seen from the new root: old root at /old, its tmpfs children below it
    const Place NewRoot {N, N->Root};
    CHECK(VisiblePath(NewRoot, OldRootMount.get(), R + "/etc") == "/old/etc");
    CHECK(VisiblePath(NewRoot, N.get(), N->Root) == "/");
    CHECK(VisiblePath(NewRoot, N.get(), N->Root + "/x/y") == "/x/y");
    // put_old may not be below a mount that is not under new_root; the old root can leave with MNT_DETACH
    UmountArgs U; U.Capable = true; U.Flags = MntDetach;
    CHECK(Umount(W.T, {OldRootMount, OldRootMount->Root}, U, [](const MountRef&) { return false; }) == 0);
    CHECK(!W.T.Has(OldRootMount.get()));
    // pivot_root(".", "."): put_old on the new root's own mount point
    World W2;
    auto N2 = W2.Tmpfs(W2.Slash, R + "/new");
    auto Same = PivotRoot(W2.T, W2.Root(), {N2, N2->Root}, {N2, N2->Root}, true);
    CHECK(std::holds_alternative<PivotResult>(Same) && W2.Slash->Parent == N2 && W2.Slash->Mountpoint == N2->Root);
    // a shared root parent blocks it
    World W3;
    auto N3 = W3.Tmpfs(W3.Slash, R + "/n");
    ChangeType(W3.T, W3.I, {W3.Slash, R}, MsShared, false);                // the root mount shared: its children's parent
    CHECK(std::get<int64_t>(PivotRoot(W3.T, W3.Root(), {N3, N3->Root}, {N3, N3->Root}, true)) == -22);
    // locked new root
    World W4;
    auto N4 = W4.Tmpfs(W4.Slash, R + "/n");
    N4->Locked = true;
    CHECK(std::get<int64_t>(PivotRoot(W4.T, W4.Root(), {N4, N4->Root}, {N4, N4->Root}, true)) == -22);
  }

  // ---- visible paths and text (native mountinfo and mounts of nested binds, ro, shared, spaces)
  {
    World W;
    auto Sp = W.Tmpfs(W.Slash, R + "/sp ace", "size=1m", MsNosuid);
    MountRef B;
    CHECK(Bind(W.T, W.I, {Sp, Sp->Root + "/sub"}, true, {W.Slash, R + "/b"}, true, false, &B) == 0);
    RemountArgs Ro; Ro.Bind = true; Ro.Ms = MsRdonly | MsNodev;
    CHECK(Remount(W.T, {B, B->Root}, Ro) == 0);
    auto C = W.Tmpfs(W.Slash, R + "/c", "mode=700", MsRdonly);
    ChangeType(W.T, W.I, {C, C->Root}, MsShared, false);
    const auto Info = MountInfo(W.T, W.Root());
    CHECK(Info == "2 1 0:1 / / rw,relatime - msdos sdmc rw\n"
                  "3 2 0:40 / /sp\\040ace rw,nosuid,relatime - tmpfs none rw,size=1024k\n"
                  "4 2 0:40 /sub /b ro,nodev,relatime - tmpfs none rw,size=1024k\n"
                  "5 2 0:41 / /c ro,relatime shared:1 - tmpfs none ro,mode=700\n");
    const auto Mounts = MountsText(W.T, W.Root());
    CHECK(Mounts == "sdmc / msdos rw,relatime 0 0\n"
                    "none /sp\\040ace tmpfs rw,nosuid,relatime,size=1024k 0 0\n"
                    "none /b tmpfs ro,nodev,relatime,size=1024k 0 0\n"
                    "none /c tmpfs ro,relatime,mode=700 0 0\n");
    // a process rooted inside /sp ace sees only what is below; mounts above are hidden
    const Place Inner {Sp, Sp->Root};
    CHECK(MountInfo(W.T, Inner) == "3 2 0:40 / / rw,nosuid,relatime - tmpfs none rw,size=1024k\n");
    // a nested mount below another: path joins the mount points
    auto Deep = W.Tmpfs(Sp, Sp->Root + "/in/er");
    CHECK(VisiblePath(W.Root(), Deep.get(), Deep->Root + "/f") == "/sp ace/in/er/f");
    CHECK(VisiblePath(Inner, Deep.get(), Deep->Root + "/f") == "/in/er/f");
    CHECK(VisiblePath({W.Slash, R + "/etc"}, Deep.get(), Deep->Root) == std::nullopt);   // outside the chroot
    CHECK(UnreachablePath(Deep.get(), Deep->Root + "/f") == "(unreachable)/sp ace/in/er/f");
    // a detached mount is unreachable
    UmountArgs U; U.Capable = true; U.Flags = MntDetach;
    CHECK(Umount(W.T, {Sp, Sp->Root}, U, [](const MountRef&) { return false; }) == 0);
    CHECK(VisiblePath(W.Root(), Deep.get(), Deep->Root) == std::nullopt);
    CHECK(UnreachablePath(Sp.get(), Sp->Root + "/q") == "(unreachable)/q");
    CHECK(Escape("a b\tc\nd\\e") == "a\\040b\\011c\\012d\\134e");
  }

  // ---- bind of a file, and pseudo filesystems
  {
    World W;
    MountRef F;
    CHECK(Bind(W.T, W.I, {W.Slash, R + "/f"}, false, {W.Slash, R + "/tf"}, false, false, &F) == 0);
    CHECK(F->Root == R + "/f" && F->Sb == W.Slash->Sb);
    auto Sys = MakeSuperblock(W.I, "sysfs", "sysfs", {}, FlagsFromMs(0), W.Reg.Initial.User, "/fex/.tmpfs/9", 0, 0);
    CHECK(Sys->Pseudo && CreateCheck(*W.Slash) == 0);
    MountRef M;
    CHECK(NewMount(W.T, W.I, {W.Slash, R + "/sys"}, true, Sys, FlagsFromMs(0).Mnt, &M) == 0);
    CHECK(CreateCheck(*M) == -1 && WriteCheck(*M) == 0);
    CHECK(MountInfo(W.T, W.Root()).find(" - sysfs sysfs rw\n") != std::string::npos);
    auto Pts = MakeSuperblock(W.I, "devpts", "devpts", {}, FlagsFromMs(0), W.Reg.Initial.User, "/fex/.tmpfs/10", 0, 0);
    MountRef P;
    CHECK(NewMount(W.T, W.I, {W.Slash, R + "/pts"}, true, Pts, FlagsFromMs(0).Mnt, &P) == 0);
    CHECK(MountInfo(W.T, W.Root()).find(" - devpts devpts rw,mode=600,ptmxmode=000\n") != std::string::npos);
    CHECK(PathContains("/a", "/a/b") && !PathContains("/a", "/ab") && PathContains("/a", "/a") && !PathContains("/a/b", "/a"));
  }

  // ---- the SD card mount: root "/", read-only at /mnt/sd, FEX's folders hidden
  {
    CHECK(PathContains("/", "/") && PathContains("/", "/fex/games") && !PathContains("/", "fex"));
    CHECK(PathBelow("/", "/") == "" && PathBelow("/", "/fex/games/a.elf") == "/fex/games/a.elf");
    CHECK(PathBelow("/fex", "/fex/games") == "/games" && PathBelow("/fex", "/fex") == "");
    World W;
    auto Sb = Sd(W.I, W.Reg);
    Sb->FsRoot = "/";
    Sb->ReadOnly = true;
    Sb->Hidden = {R, "/fex/.inode"};
    const auto Card = MakeMount(W.T, W.I, W.Slash, R + "/mnt/sd", "/", Sb, MntReadonly | MntNosuid | MntNodev | MntRelatime);
    CHECK(Cross(W.T, W.At(W.Slash, R + "/mnt/sd")).M == Card);
    CHECK(VisiblePath(W.Root(), Card.get(), "/fex/games/a.elf") == std::optional<std::string> {"/mnt/sd/fex/games/a.elf"});
    CHECK(VisiblePath(W.Root(), Card.get(), "/") == std::optional<std::string> {"/mnt/sd"});
    CHECK(HidesPath(Card.get(), R) && HidesPath(Card.get(), R + "/usr") && HidesPath(Card.get(), "/FEX/RootFS/etc"));
    CHECK(HidesPath(Card.get(), "/fex/.inode") && !HidesPath(Card.get(), "/fex/rootfs2") && !HidesPath(Card.get(), "/fex/games"));
    CHECK(!HidesPath(W.Slash.get(), R + "/usr"));
    CHECK(Card->ReadOnly());
    const auto Info = MountInfo(W.T, W.Root());
    CHECK(Info.find(" / /mnt/sd ro,nosuid,nodev,relatime") != std::string::npos);
    // a bind of a card folder shows its folder as the root inside the file system
    const auto Games = MakeMount(W.T, W.I, W.Slash, R + "/opt", "/fex/games", Sb, MntRelatime);
    CHECK(MountInfo(W.T, W.Root()).find(" /fex/games /opt ") != std::string::npos);
    CHECK(VisiblePath(W.Root(), Games.get(), "/fex/games/a.elf") == std::optional<std::string> {"/opt/a.elf"});
  }

  std::printf("guest-mounts: ok\n");
  return 0;
}
