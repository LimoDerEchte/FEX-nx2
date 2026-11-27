// SPDX-License-Identifier: MIT
#include "RootfsSelection.h"

#include "Json.h"

namespace ui {

RootfsSelection RootfsSelection::Load(Storage& S, const SettingsPaths& P, const RootfsIndex& Index) {
  RootfsSelection Sel;
  std::string Text, Err;
  Json J;
  if (S.ReadFile(P.UiJson, Text) && Json::Parse(Text, J, Err) && J.IsObject()) {
    if (const Json* V = J.Find("rootfs_base"))
      if (V->IsString())
        Sel.Base = V->AsString();
    if (const Json* V = J.Find("rootfs_extras"))
      Sel.Extras = V->AsBool(true);
    // Desktops and stores are recorded as chosen but only "none" and no store can be installed today.
    if (const Json* V = J.Find("rootfs_desktop"))
      if (V->IsString() && V->AsString() == "none")
        Sel.Desktop = "none";
  }
  bool Known = false;
  for (const RootfsImage& I : Index.Images)
    if (I.Selectable() && KeyOf(I) == Sel.Base)
      Known = true;
  if (!Known) {
    Sel.Base.clear();
    for (const RootfsImage& I : Index.Images)
      if (I.Selectable()) {
        Sel.Base = KeyOf(I);
        break;
      }
  }
  return Sel;
}

bool RootfsSelection::Save(Storage& S, const SettingsPaths& P, std::string& Error) const {
  Json J = Json::Object();
  std::string Text, Err;
  if (S.ReadFile(P.UiJson, Text)) {
    Json Old;
    if (Json::Parse(Text, Old, Err) && Old.IsObject())
      J = std::move(Old);
    else
      S.WriteFileAtomic(P.UiJson + ".bad", Text, Err);
  }
  if (!J.Find("version"))
    J.Set("version", Json::Int(1));
  J.Set("rootfs_base", Json::String(Base));
  J.Set("rootfs_desktop", Json::String(Desktop));
  Json Stores = Json::Array();
  for (const std::string& St : this->Stores)
    Stores.Push(Json::String(St));
  J.Set("rootfs_stores", std::move(Stores));
  J.Set("rootfs_extras", Json::Bool(Extras));
  if (!S.WriteFileAtomic(P.UiJson, J.Dump(), Err)) {
    Error = "ui.json: " + Err;
    return false;
  }
  return true;
}

} // namespace ui
