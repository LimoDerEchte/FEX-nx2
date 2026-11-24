// SPDX-License-Identifier: MIT
#include "../GuestArena.h"

#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                           \
    }                                                         \
  } while (0)

using namespace Switch2Guest;

// Host backend: a PROT_NONE mmap region that becomes RW on commit.
struct FakeBackend : ArenaBackend {
  int Commits {}, Decommits {};
  bool Fail {};
  bool Commit(uint64_t Address, size_t Size) override {
    if (Fail) return false;
    ++Commits;
    return mprotect(reinterpret_cast<void*>(Address), Size, PROT_READ | PROT_WRITE) == 0;
  }
  bool FailDecommit {};
  bool Decommit(uint64_t Address, size_t Size) override {
    ++Decommits;
    if (FailDecommit) return false;
    return mprotect(reinterpret_cast<void*>(Address), Size, PROT_NONE) == 0;
  }
};

constexpr size_t G = GuestArena::Granule;

int main() {
  constexpr size_t Total = 64 * G;
  void* Map = mmap(nullptr, Total + G, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  CHECK(Map != MAP_FAILED);
  const uint64_t Base = (reinterpret_cast<uint64_t>(Map) + G - 1) & ~(G - 1);

  // 1. Reserve returns page-aligned, non-overlapping ranges inside the arena.
  {
    FakeBackend Backend;
    GuestArena Arena {Backend, Base, Total, 8 * G};
    const auto A = Arena.Reserve(3 * PageSize), B = Arena.Reserve(PageSize), C = Arena.Reserve(5 * PageSize + 1);
    CHECK(A && B && C);
    for (const auto V : {A, B, C}) CHECK(V % PageSize == 0 && V >= Base && V < Base + Total);
    CHECK(A + 3 * PageSize <= B || B + PageSize <= A);
    CHECK(C + 6 * PageSize <= A || A + 3 * PageSize <= C);
    CHECK(C + 6 * PageSize <= B || B + PageSize <= C);
    CHECK(Backend.Commits == 0 && Arena.Committed() == 0);
    CHECK(!Arena.Reserve(0));
    CHECK(!Arena.Reserve(Total + PageSize));
  }

  // 2. A page commits its whole granule once; a second page does not call again.
  {
    FakeBackend Backend;
    GuestArena Arena {Backend, Base, Total, 8 * G};
    const auto A = Arena.Reserve(4 * PageSize);
    CHECK(Arena.Commit(A, PageSize));
    CHECK(Backend.Commits == 1 && Arena.Committed() == G);
    CHECK(Arena.IsCommitted(A, PageSize));
    CHECK(Arena.Commit(A + PageSize, PageSize));
    CHECK(Backend.Commits == 1 && Arena.Committed() == G);
    CHECK(Arena.IsCommitted(A, 2 * PageSize));
    // Only newly committed pages are zero-filled; the first page keeps its data.
    *reinterpret_cast<char*>(A) = 'x';
    CHECK(Arena.Commit(A + PageSize, 2 * PageSize));
    CHECK(*reinterpret_cast<char*>(A) == 'x');
    CHECK(Backend.Commits == 1);
  }

  // Newly committed pages read zero even when the granule held stale bytes.
  {
    FakeBackend Backend;
    GuestArena Arena {Backend, Base, Total, 8 * G};
    const auto A = Arena.Reserve(4 * PageSize);
    CHECK(Arena.Commit(A, 2 * PageSize));
    std::memset(reinterpret_cast<void*>(A), 0x55, 2 * PageSize);
    Arena.Decommit(A + PageSize, PageSize);
    CHECK(Backend.Decommits == 0); // the granule keeps page 0
    CHECK(Arena.Commit(A + PageSize, PageSize));
    CHECK(static_cast<uint8_t*>(reinterpret_cast<void*>(A))[PageSize] == 0);
    CHECK(static_cast<uint8_t*>(reinterpret_cast<void*>(A))[0] == 0x55);
  }

  // 3. Decommit keeps a shared granule and drops it with the last page.
  {
    FakeBackend Backend;
    GuestArena Arena {Backend, Base, Total, 8 * G};
    const auto A = Arena.Reserve(4 * PageSize);
    CHECK(Arena.Commit(A, 2 * PageSize));
    Arena.Decommit(A, PageSize);
    CHECK(Backend.Decommits == 0 && Arena.Committed() == G);
    CHECK(!Arena.IsCommitted(A, PageSize) && Arena.IsCommitted(A + PageSize, PageSize));
    Arena.Decommit(A + PageSize, PageSize);
    CHECK(Backend.Decommits == 1 && Arena.Committed() == 0);
    // Decommitting uncommitted pages is harmless.
    Arena.Decommit(A, 4 * PageSize);
    CHECK(Backend.Decommits == 1);
  }

  // 4. Commit beyond the limit fails and changes nothing.
  {
    FakeBackend Backend;
    GuestArena Arena {Backend, Base, Total, 2 * G};
    const auto A = Arena.Reserve(4 * G, G);
    CHECK(A % G == 0);
    CHECK(!Arena.Commit(A, 3 * G));
    CHECK(Backend.Commits == 0 && Arena.Committed() == 0 && !Arena.IsCommitted(A, PageSize));
    CHECK(Arena.Commit(A, 2 * G));
    CHECK(Arena.Committed() == 2 * G);
    CHECK(!Arena.Commit(A + 2 * G, PageSize));
    CHECK(Arena.Committed() == 2 * G && Arena.CommitLimit() == 2 * G);
    // Backend failure rolls back.
    Backend.Fail = true;
    Arena.Decommit(A, 2 * G);
    CHECK(Arena.Committed() == 0);
    CHECK(!Arena.Commit(A, PageSize) && Arena.Committed() == 0 && !Arena.IsCommitted(A, PageSize));
  }

  // 5. Release decommits and allows reuse of the VA.
  {
    FakeBackend Backend;
    GuestArena Arena {Backend, Base, Total, 8 * G};
    const auto A = Arena.Reserve(8 * PageSize);
    CHECK(Arena.Commit(A, 8 * PageSize));
    CHECK(Arena.Release(A, 8 * PageSize));
    CHECK(Arena.Committed() == 0 && !Arena.IsCommitted(A, PageSize));
    CHECK(!Arena.Release(A, 8 * PageSize)); // double free
    CHECK(Arena.Reserve(8 * PageSize) == A);
    // Coalescing: three neighbours free into one block.
    const auto B = Arena.Reserve(PageSize), C = Arena.Reserve(PageSize);
    CHECK(Arena.Release(B, PageSize) && Arena.Release(A, 8 * PageSize) && Arena.Release(C, PageSize));
    CHECK(Arena.Reserve(10 * PageSize) == A);
  }

  // 6. IsCommitted is false for reserved-only pages.
  {
    FakeBackend Backend;
    GuestArena Arena {Backend, Base, Total, 8 * G};
    const auto A = Arena.Reserve(4 * PageSize);
    CHECK(!Arena.IsCommitted(A, PageSize));
    CHECK(Arena.Commit(A + PageSize, PageSize));
    CHECK(!Arena.IsCommitted(A, 2 * PageSize) && Arena.IsCommitted(A + PageSize, PageSize));
    CHECK(!Arena.IsCommitted(A, 0));
  }

  // 7. Reserve with 2 MiB alignment, and ReserveAt of a free range.
  {
    FakeBackend Backend;
    GuestArena Arena {Backend, Base, Total, 8 * G};
    Arena.Reserve(PageSize);
    const auto A = Arena.Reserve(PageSize, G);
    CHECK(A && A % G == 0);
    CHECK(Arena.ReserveAt(A + PageSize, PageSize));
    CHECK(!Arena.ReserveAt(A, PageSize));
    CHECK(!Arena.ReserveAt(Base + Total, PageSize));
    CHECK(Arena.Contains(A, PageSize) && !Arena.Contains(Base - PageSize, PageSize));
  }
  // 8. A granule the backend fails to decommit stays counted and mapped;
  // the next commit there reuses it without a backend commit.
  {
    FakeBackend Backend;
    GuestArena Arena {Backend, Base, Total, 8 * G};
    const auto A = Arena.Reserve(G, G);
    CHECK(Arena.Commit(A, PageSize) && Arena.Committed() == G && Backend.Commits == 1);
    *reinterpret_cast<char*>(A) = 'x';
    Backend.FailDecommit = true;
    Arena.Decommit(A, PageSize);
    CHECK(Arena.Committed() == G && !Arena.IsCommitted(A, PageSize));
    Backend.FailDecommit = false;
    CHECK(Arena.Commit(A + PageSize, PageSize) && Backend.Commits == 1 && Arena.Committed() == G);
    CHECK(*reinterpret_cast<char*>(A + PageSize) == 0);
    Arena.Decommit(A + PageSize, PageSize);
    CHECK(Arena.Committed() == 0);
  }
  std::puts("guest arena checks passed");
}
