// SPDX-License-Identifier: MIT
#include "FileBrowser.h"

#include <algorithm>

namespace ui {

namespace {
std::string Lower(std::string S) {
  for (char& C : S)
    if (C >= 'A' && C <= 'Z')
      C = char(C - 'A' + 'a');
  return S;
}
} // namespace

bool MatchesAccept(const std::string& Name, const std::vector<std::string>& Accept) {
  if (Accept.empty())
    return true;
  const std::string L = Lower(Name);
  for (const std::string& Ext : Accept)
    if (L.size() > Ext.size() && L.compare(L.size() - Ext.size(), Ext.size(), Lower(Ext)) == 0)
      return true;
  return false;
}

bool IsHiddenName(const std::string& Name) { return !Name.empty() && Name[0] == '.'; }

void SortEntries(std::vector<BrowseEntry>& E) {
  std::stable_sort(E.begin(), E.end(), [](const BrowseEntry& A, const BrowseEntry& B) {
    if (A.IsDir != B.IsDir)
      return A.IsDir;
    const std::string La = Lower(A.Name), Lb = Lower(B.Name);
    if (La != Lb)
      return La < Lb;
    return A.Name < B.Name;
  });
}

bool MakeEntry(const DirEntry& D, const BrowseOptions& O, BrowseEntry& Out) {
  if (IsHiddenName(D.Name) || (O.FolderMode && !D.IsDir))
    return false;
  Out.Name = D.Name;
  Out.IsDir = D.IsDir;
  Out.Size = D.Size;
  Out.Mtime = D.Mtime;
  Out.Selectable = D.IsDir || MatchesAccept(D.Name, O.Accept);
  return true;
}

std::vector<Location> DefaultLocations(const std::string& Home) {
  return {{"sd", "Console", "microSD card", "sdmc:/"},
          {"fex", "Console", "FEX folder", "sdmc:/fex/"},
          {"rootfs", "Linux", "RootFS", "/"},
          {"home", "Linux", "Home", Home.empty() ? std::string("/home/") : Home}};
}

int LocationOf(const std::vector<Location>& L, const std::string& Path) {
  const std::string P = NormalizePath(Path);
  int Best = 0;
  size_t BestLen = 0;
  for (size_t I = 0; I < L.size(); ++I) {
    const std::string R = NormalizePath(L[I].Root);
    const bool Inside = P == R || (R.back() == '/' ? P.rfind(R, 0) == 0 : P.rfind(R + "/", 0) == 0);
    if (Inside && R.size() >= BestLen) {
      Best = int(I);
      BestLen = R.size();
    }
  }
  return Best;
}

std::string StartFolder(Storage& S, const std::vector<Location>& L, const std::string& Current, bool FolderMode, const std::string& Fallback) {
  (void)FolderMode;  // a file value starts in its folder; a folder value starts in itself (or, when it is gone, its parent)
  if (IsValidPath(Current)) {
    const std::string N = NormalizePath(Current);
    if (S.IsDir(N))
      return N;
    const std::string P = ParentPath(N);
    if (S.IsDir(P))
      return P;
  }
  for (const Location& Loc : L)
    if (Loc.Id == Fallback && S.IsDir(Loc.Root))
      return NormalizePath(Loc.Root);
  return "sdmc:/";
}

void FolderLoader::Begin(Storage& S, const std::string& Path, const BrowseOptions& O, uint64_t Now) {
  Opt = O;
  Where = NormalizePath(Path);
  List.clear();
  Started = Now;
  PendingCount = 0;
  Error = false;
  Reader = S.OpenDir(Where);
  Finished = false;
  if (!Reader) {
    Error = true;
    Finished = true;
  }
}

bool FolderLoader::Step(int MaxEntries) {
  if (Finished)
    return true;
  DirEntry D;
  for (int I = 0; I < MaxEntries;) {
    const DirStep R = Reader->Next(D);
    if (R == DirStep::Pending) {
      ++PendingCount;
      return false;
    }
    if (R == DirStep::End) {
      Error = Reader->Failed();
      Finished = true;
      Reader.reset();
      SortEntries(List);
      return true;
    }
    ++I;
    BrowseEntry E;
    if (MakeEntry(D, Opt, E))
      List.push_back(std::move(E));
  }
  return false;
}

} // namespace ui
