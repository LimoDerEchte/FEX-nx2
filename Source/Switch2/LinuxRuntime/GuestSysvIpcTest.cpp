// SPDX-License-Identifier: MIT
#include "../GuestSysvIpc.h"
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

namespace {
struct Thing {
  IpcPerm Perm;
  uint32_t Size {};
};

IpcCred User(uint32_t Uid, uint32_t Gid, bool Cap = false) { return {Uid, Gid, {}, Cap}; }

auto Make(uint32_t Size) {
  return [Size](Thing& New) -> int64_t {
    if (!Size) return -22;
    New.Size = Size;
    return 0;
  };
}
auto AtLeast(uint32_t Size) {
  return [Size](const Thing& Old) -> int64_t { return Old.Size < Size ? -22 : 0; };
}
} // namespace

int main() {
  // IpcAllowed: owner, creator, group, other, CAP_IPC_OWNER.
  {
    IpcPerm Perm {};
    Perm.Uid = 10;
    Perm.Cuid = 11;
    Perm.Gid = 20;
    Perm.Cgid = 21;
    Perm.Mode = 0640;
    CHECK(IpcAllowed(Perm, User(10, 99), 4));
    CHECK(IpcAllowed(Perm, User(10, 99), 6));
    CHECK(!IpcAllowed(Perm, User(10, 99), 1));
    CHECK(IpcAllowed(Perm, User(11, 99), 6)); // creator counts as owner
    CHECK(IpcAllowed(Perm, User(50, 20), 4)); // group
    CHECK(!IpcAllowed(Perm, User(50, 20), 2));
    CHECK(IpcAllowed(Perm, User(50, 21), 4)); // creator group
    const uint32_t Supplementary[] {7, 20};
    CHECK(IpcAllowed(Perm, IpcCred {50, 99, Supplementary, false}, 4));
    CHECK(!IpcAllowed(Perm, User(50, 99), 4)); // other has no bits
    CHECK(IpcAllowed(Perm, User(50, 99, true), 6)); // CAP_IPC_OWNER bypass
    // The owner class is checked first and wins even when other would allow.
    Perm.Mode = 0007;
    CHECK(!IpcAllowed(Perm, User(10, 99), 4));
    CHECK(IpcAllowed(Perm, User(50, 99), 7));
  }

  // IpcTable::Get.
  {
    IpcTable<Thing> Table;
    const auto Me = User(1000, 1000);
    // No key and no IPC_CREAT.
    CHECK(Table.Get(0x42, 0600, Me, Make(8), AtLeast(8)) == -2);
    // IPC_PRIVATE always creates, and each call is a new object.
    const auto A = Table.Get(IpcPrivate, 0600, Me, Make(8), AtLeast(8));
    const auto B = Table.Get(IpcPrivate, 0600, Me, Make(8), AtLeast(8));
    CHECK(A == 0 && B == 1);
    // Create runs with ownership already set.
    const auto C = Table.Get(0x42, IpcCreat | 0640, Me, [](Thing& New) -> int64_t {
      New.Size = 16;
      return New.Perm.Uid == 1000 && New.Perm.Cuid == 1000 && New.Perm.Gid == 1000 && New.Perm.Mode == 0640 &&
                     New.Perm.Key == 0x42 ? 0 : -1;
    }, AtLeast(16));
    CHECK(C == 2);
    // Existing key: lookup, EXCL, size check, permission check.
    CHECK(Table.Get(0x42, 0, Me, Make(1), AtLeast(16)) == 2);
    CHECK(Table.Get(0x42, IpcCreat, Me, Make(1), AtLeast(16)) == 2);
    CHECK(Table.Get(0x42, IpcCreat | IpcExcl, Me, Make(1), AtLeast(16)) == -17);
    CHECK(Table.Get(0x42, 0, Me, Make(1), AtLeast(17)) == -22);
    CHECK(Table.Get(0x42, 0600, User(2000, 2000), Make(1), AtLeast(16)) == -13);
    // ipcget_public: the size check (more_checks) comes before ipcperms.
    CHECK(Table.Get(0x42, 0600, User(2000, 2000), Make(1), AtLeast(17)) == -22);
    CHECK(Table.Get(0x42, 0, User(2000, 2000), Make(1), AtLeast(16)) == 2); // no access bits requested
    CHECK(Table.Get(0x42, 0004, User(2000, 2000), Make(1), AtLeast(16)) == -13); // other has nothing
    CHECK(Table.Get(0x42, 0600, User(2000, 2000, true), Make(1), AtLeast(16)) == 2);
    // A Create error stops creation without using an index.
    CHECK(Table.Get(0x43, IpcCreat, Me, Make(0), AtLeast(0)) == -22);
    CHECK(Table.Count() == 3);
    // Find: id = seq * 32768 + index.
    CHECK(Table.Find(2) && Table.Find(2)->Perm.Key == 0x42);
    CHECK(!Table.Find(3));
    CHECK(!Table.Find(-1));
    CHECK(!Table.Find(2 + IpcMni)); // wrong sequence
    CHECK(Table.MaxIndex() == 2);
    Table.Remove(1);
    CHECK(!Table.Find(1));
    CHECK(Table.Count() == 2);
    // Indexes keep rising (cyclic) and are not reused right away.
    const auto D = Table.Get(IpcPrivate, 0600, Me, Make(8), AtLeast(8));
    CHECK(D == 3);
  }

  // Cyclic allocation wraps and bumps the sequence number.
  {
    IpcTable<Thing> Table;
    const auto Me = User(1, 1);
    int64_t Last {};
    for (int I = 0; I < 200; ++I) {
      Last = Table.Get(IpcPrivate, 0600, Me, Make(1), AtLeast(1));
      CHECK(Last >= 0);
      Table.Remove(static_cast<int32_t>(Last));
    }
    // 200 allocations with at most one live object: the window is 64 slots,
    // so the cursor wrapped three times.
    CHECK(Last / IpcMni == 3);
    const auto Id = Table.Get(IpcPrivate, 0600, Me, Make(1), AtLeast(1));
    CHECK(Table.Find(static_cast<int32_t>(Id)));
    CHECK(!Table.Find(static_cast<int32_t>(Last))); // stale id from an earlier life
  }

  // A full table gives ENOSPC.
  {
    IpcTable<Thing> Table {2};
    const auto Me = User(1, 1);
    CHECK(Table.Get(IpcPrivate, 0600, Me, Make(1), AtLeast(1)) == 0);
    CHECK(Table.Get(IpcPrivate, 0600, Me, Make(1), AtLeast(1)) == 1);
    CHECK(Table.Get(IpcPrivate, 0600, Me, Make(1), AtLeast(1)) == -28);
  }

  // TrySemOps.
  {
    uint16_t Values[3] {};
    std::vector<int16_t> Undo;
    const auto Run = [&](std::vector<SemOp> Ops) { return TrySemOps(Values, Ops, Undo, 77); };
    CHECK(Run({{0, 1, 0}}) == 0 && Values[0] == 1);
    // Negative op blocks when the value would go below zero.
    CHECK(Run({{0, -2, 0}}) == 1 && Values[0] == 1);
    // All or nothing: the second op blocks, the first is rolled back.
    CHECK(Run({{1, 5, 0}, {2, -1, 0}}) == 1);
    CHECK(Values[1] == 0 && Values[2] == 0);
    // Ops apply in order within one call.
    CHECK(Run({{2, 1, 0}, {2, -1, 0}, {2, 3, 0}}) == 0 && Values[2] == 3);
    // Zero op waits for zero.
    CHECK(Run({{0, 0, 0}}) == 1);
    CHECK(Run({{1, 0, 0}}) == 0);
    // IPC_NOWAIT turns a block into EAGAIN.
    CHECK(Run({{0, -2, IpcNowait}}) == -11);
    CHECK(Run({{0, 0, IpcNowait}}) == -11);
    // ERANGE past SEMVMX; the earlier op is rolled back.
    CHECK(Run({{1, 1, 0}, {0, 32767, 0}}) == -34);
    CHECK(Values[1] == 0 && Values[0] == 1);
    Values[0] = 32767;
    CHECK(Run({{0, 1, 0}}) == -34 && Values[0] == 32767);
    // EFBIG for a semaphore number out of range, before anything applies.
    CHECK(Run({{0, -1, 0}, {3, 1, 0}}) == -27);
    CHECK(Values[0] == 32767);
    // The pid of an applied op is recorded.
    uint32_t Pids[3] {};
    std::vector<SemOp> Op {{1, 2, 0}};
    CHECK(TrySemOps(Values, Op, Undo, 99, Pids) == 0 && Pids[1] == 99 && Pids[0] == 0);
  }

  // SEM_UNDO accumulates the negated delta, and rolls back with the op.
  {
    uint16_t Values[2] {};
    std::vector<int16_t> Undo;
    std::vector<SemOp> Ops {{0, 3, SemUndo}, {1, 2, SemUndo}};
    CHECK(TrySemOps(Values, Ops, Undo, 1) == 0);
    CHECK(Undo.size() == 2 && Undo[0] == -3 && Undo[1] == -2);
    Ops = {{0, -1, SemUndo}};
    CHECK(TrySemOps(Values, Ops, Undo, 1) == 0 && Undo[0] == -2 && Values[0] == 2);
    // A blocking op leaves the adjustment alone.
    Ops = {{0, 1, SemUndo}, {1, -5, SemUndo}};
    CHECK(TrySemOps(Values, Ops, Undo, 1) == 1);
    CHECK(Undo[0] == -2 && Values[0] == 2);
    // Undo overflow is ERANGE (below -SEMAEM - 1).
    Undo[0] = -32768;
    Ops = {{0, 1, SemUndo}};
    CHECK(TrySemOps(Values, Ops, Undo, 1) == -34 && Values[0] == 2);
    // Ops without SEM_UNDO leave the vector alone.
    std::vector<int16_t> Empty;
    Ops = {{0, 1, 0}};
    CHECK(TrySemOps(Values, Ops, Empty, 1) == 0 && Empty.empty());
    // exit_sem: apply, clamped to [0, SEMVMX].
    uint16_t Left[3] {5, 32767, 4};
    const int16_t Delta[3] {-3, 10, -9};
    ApplySemUndo(Left, Delta);
    CHECK(Left[0] == 2 && Left[1] == 32767 && Left[2] == 0);
  }

  // SelectMessage.
  {
    std::deque<Message> Queue;
    CHECK(!SelectMessage(Queue, 0, false));
    Queue.push_back({5, {1}});
    Queue.push_back({2, {2}});
    Queue.push_back({5, {3}});
    Queue.push_back({3, {4}});
    CHECK(SelectMessage(Queue, 0, false) == 0);
    CHECK(SelectMessage(Queue, 2, false) == 1);
    CHECK(SelectMessage(Queue, 5, false) == 0);
    CHECK(!SelectMessage(Queue, 9, false));
    CHECK(SelectMessage(Queue, 5, true) == 1);     // first not of type 5
    CHECK(SelectMessage(Queue, 2, true) == 0);
    CHECK(SelectMessage(Queue, -4, false) == 1);   // lowest type <= 4 is 2
    CHECK(SelectMessage(Queue, -3, false) == 1);
    CHECK(!SelectMessage(Queue, -1, false).has_value());
    CHECK(SelectMessage(Queue, -9, true) == 1);    // EXCEPT ignored for negative
    Queue.push_front({2, {9}});
    CHECK(SelectMessage(Queue, -2, false) == 0);   // first among equals
    CHECK(SelectMessage(Queue, 0, true) == 0);     // EXCEPT ignored for zero
    CHECK(SelectMessage(Queue, INT64_MIN, false) == 0);
  }

  // Layouts match the kernel's ipc64 structures.
  CHECK(sizeof(LinuxShmidDs) == 112 && sizeof(LinuxSemidDs) == 104 && sizeof(LinuxMsqidDs) == 120);
  {
    IpcPerm Perm {};
    Perm.Key = 7;
    Perm.Mode = 0600;
    Perm.Seq = 3;
    const auto Out = ToLinux(Perm, 01000);
    CHECK(Out.Key == 7 && Out.Mode == 01600 && Out.Seq == 3);
  }
  std::puts("GuestSysvIpcTest ok");
  return 0;
}
