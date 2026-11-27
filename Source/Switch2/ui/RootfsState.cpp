// SPDX-License-Identifier: MIT
#include "RootfsState.h"

#include "Json.h"

namespace ui {

RootfsInfo DetectRootfs(Storage& S) {
  RootfsInfo Info;
  uint64_t Total = 0;
  if (!S.FreeSpace("sdmc:/", Info.FreeBytes, Total))
    Info.FreeBytes = 0;
  std::string Text, Error;
  Json Marker;
  if (S.ReadFile(RootfsMarkerPath, Text) && Json::Parse(Text, Marker, Error) && Marker.IsObject()) {
    const Json* On = Marker.Find("installed");
    if (On && On->AsBool()) {
      Info.Installed = true;
      const Json* Name = Marker.Find("name");
      const Json* Image = Marker.Find("image");
      Info.Name = Name && Name->IsString() && !Name->AsString().empty() ? Name->AsString() : Image && Image->IsString() ? Image->AsString() : "RootFS";
      return Info;
    }
  }
  // No marker: a tree that has the standard folders (the developer bundle's Debian RootFS) counts as installed.
  if (S.IsDir(std::string(RootfsFolder) + "/bin") || S.IsDir(std::string(RootfsFolder) + "/usr")) {
    Info.Installed = true;
    Info.Name = "RootFS";
  }
  return Info;
}

} // namespace ui
