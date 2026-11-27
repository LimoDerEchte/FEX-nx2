// SPDX-License-Identifier: MIT
#include "PathUtil.h"

#include <cstdio>
#include <ctime>

namespace ui {

namespace {
void SplitRoot(const std::string& P, std::string& Root, std::string& Rest) {
  if (IsSdPath(P)) {
    Root = "sdmc:/";
    Rest = P.substr(6);
  } else {
    Root = "/";
    Rest = P.substr(1);
  }
}
std::vector<std::string> Segments(const std::string& Rest) {
  std::vector<std::string> Out;
  size_t I = 0;
  while (I <= Rest.size()) {
    size_t J = Rest.find('/', I);
    if (J == std::string::npos)
      J = Rest.size();
    Out.push_back(Rest.substr(I, J - I));
    I = J + 1;
  }
  return Out;
}
} // namespace

std::string NormalizePath(const std::string& P) {
  if (!IsValidPath(P))
    return P;
  std::string Root, Rest;
  SplitRoot(P, Root, Rest);
  std::vector<std::string> Out;
  for (const std::string& S : Segments(Rest)) {
    if (S.empty() || S == ".")
      continue;
    if (S == "..") {
      if (!Out.empty())
        Out.pop_back();
      continue;
    }
    Out.push_back(S);
  }
  std::string R = Root;
  for (size_t I = 0; I < Out.size(); ++I) {
    if (I)
      R += '/';
    R += Out[I];
  }
  return R;
}

std::string ParentPath(const std::string& P) {
  const std::string N = NormalizePath(P);
  if (!IsValidPath(N) || IsRootPath(N))
    return N;
  const size_t Slash = N.rfind('/');
  if (Slash == std::string::npos)
    return N;
  if (IsSdPath(N) && Slash < 6)
    return "sdmc:/";
  if (Slash == 0)
    return "/";
  return N.substr(0, Slash);
}

std::string JoinPath(const std::string& Dir, const std::string& Name) {
  if (Dir.empty())
    return Name;
  return Dir.back() == '/' ? Dir + Name : Dir + "/" + Name;
}

std::string BaseName(const std::string& P) {
  const std::string N = NormalizePath(P);
  if (IsRootPath(N))
    return N;
  const size_t Slash = N.rfind('/');
  return Slash == std::string::npos ? N : N.substr(Slash + 1);
}

std::vector<PathCrumb> Breadcrumbs(const std::string& P) {
  std::vector<PathCrumb> Out;
  const std::string N = NormalizePath(P);
  if (!IsValidPath(N))
    return Out;
  std::string Root, Rest;
  SplitRoot(N, Root, Rest);
  Out.push_back({IsSdPath(N) ? "microSD" : "RootFS", Root});
  std::string Cur = Root;
  for (const std::string& S : Segments(Rest)) {
    if (S.empty())
      continue;
    Cur = JoinPath(Cur, S);
    Out.push_back({S, Cur});
  }
  return Out;
}

std::string FormatBytes(uint64_t Bytes) {
  char Buf[32];
  if (Bytes < 1024) {
    std::snprintf(Buf, sizeof(Buf), "%llu B", static_cast<unsigned long long>(Bytes));
    return Buf;
  }
  const char* const Units[] = {"KB", "MB", "GB", "TB"};
  double V = double(Bytes);
  int U = -1;
  while (V >= 1024.0 && U < 3) {
    V /= 1024.0;
    ++U;
  }
  std::snprintf(Buf, sizeof(Buf), V >= 100 ? "%.0f %s" : "%.1f %s", V, Units[U]);
  return Buf;
}

std::string FormatDate(int64_t Unix) {
  if (Unix <= 0)
    return {};
  const time_t T = time_t(Unix);
  struct tm Tm {};
  gmtime_r(&T, &Tm);
  char Buf[48];
  std::snprintf(Buf, sizeof(Buf), "%04d-%02d-%02d", Tm.tm_year + 1900, Tm.tm_mon + 1, Tm.tm_mday);
  return Buf;
}

} // namespace ui
