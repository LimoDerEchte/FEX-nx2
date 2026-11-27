// SPDX-License-Identifier: MIT
#include "LicenseStore.h"

#include "Json.h"
#include "PathUtil.h"
#include "assets/Assets.h"

namespace ui {

bool ParseLicenses(const std::string& Text, std::vector<LicenseEntry>& Out, std::string& Error) {
  Json Root;
  if (!Json::Parse(Text, Root, Error))
    return false;
  const Json* List = Root.Find("components");
  if (!List || !List->IsArray()) {
    Error = "licenses.json has no \"components\" array";
    return false;
  }
  Out.clear();
  for (const Json& J : List->Items()) {
    if (!J.IsObject()) {
      Error = "a component is not an object";
      return false;
    }
    auto Str = [&](const char* K) {
      const Json* V = J.Find(K);
      return V && V->IsString() ? V->AsString() : std::string();
    };
    LicenseEntry E;
    E.Name = Str("name");
    E.Role = Str("role");
    E.License = Str("license");
    E.Url = Str("url");
    E.File = Str("file");
    E.Note = Str("note");
    const Json* B = J.Find("bundled");
    E.Bundled = B ? B->AsBool(true) : true;
    if (E.Name.empty() || E.License.empty()) {
      Error = "a component needs a name and a license";
      return false;
    }
    Out.push_back(std::move(E));
  }
  return true;
}

const std::vector<LicenseEntry>& EmbeddedLicenses() {
  static const std::vector<LicenseEntry> L = [] {
    std::vector<LicenseEntry> V;
    const Blob B = GetLicensesJson();
    std::string Err;
    ParseLicenses(std::string(reinterpret_cast<const char*>(B.Data), B.Size), V, Err);
    return V;
  }();
  return L;
}

bool LicenseTextAvailable(const LicenseEntry& E) { return !E.File.empty() && FindLicenseFile(E.File.c_str()).Data != nullptr; }

std::string LicenseText(const LicenseEntry& E) {
  if (E.File.empty())
    return {};
  const Blob B = FindLicenseFile(E.File.c_str());
  return B.Data ? std::string(reinterpret_cast<const char*>(B.Data), B.Size) : std::string();
}

std::string LicenseTextPath(const LicenseEntry& E) { return E.File.empty() ? std::string() : JoinPath(LicensesDir, E.File); }

bool EnsureLicensesWritten(Storage& S, const std::string& Dir, int& Written, std::string& Error) {
  Written = 0;
  auto Put = [&](const std::string& Path, const char* Data, size_t Size) {
    const std::string Want(Data, Size);
    std::string Have;
    if (S.ReadFile(Path, Have) && Have == Want)
      return true;
    if (!S.WriteFileAtomic(Path, Want, Error))
      return false;
    ++Written;
    return true;
  };
  const Blob J = GetLicensesJson();
  if (!Put(JoinPath(Dir, "licenses.json"), reinterpret_cast<const char*>(J.Data), J.Size))
    return false;
  for (size_t I = 0; I < LicenseFileCount(); ++I) {
    const Blob B = LicenseFileData(I);
    if (!Put(JoinPath(Dir, LicenseFileName(I)), reinterpret_cast<const char*>(B.Data), B.Size))
      return false;
  }
  return true;
}

} // namespace ui
