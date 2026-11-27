// SPDX-License-Identifier: MIT
#include "RootfsIndex.h"

#include <algorithm>
#include <string_view>

#include "Json.h"
#include "assets/Assets.h"

namespace ui {
namespace {

bool IsHex64(const std::string& Hash) {
  if (Hash.empty() || Hash.size() > 16)
    return false;
  return std::all_of(Hash.begin(), Hash.end(), [](char C) { return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F'); });
}

std::string StringField(const Json& Object, const char* Key) {
  const Json* Value = Object.Find(Key);
  return Value && Value->IsString() ? Value->AsString() : std::string();
}

uint64_t NumberField(const Json& Object, const char* Key) {
  const Json* Value = Object.Find(Key);
  return Value && Value->IsNumber() && Value->AsInt() > 0 ? uint64_t(Value->AsInt()) : 0;
}

// The date folder of ".../Ubuntu_24_04/2026-08-11/Ubuntu_24_04.sqsh", or "".
std::string DateFolder(const std::string& Url) {
  const size_t End = Url.rfind('/');
  if (End == std::string::npos || End == 0)
    return {};
  const size_t Start = Url.rfind('/', End - 1);
  if (Start == std::string::npos)
    return {};
  const std::string Part = Url.substr(Start + 1, End - Start - 1);
  const bool Date = Part.size() == 10 && Part[4] == '-' && Part[7] == '-';
  return Date ? Part : std::string();
}

} // namespace

const RootfsImage* RootfsIndex::Find(const std::string& DistroMatch, const std::string& DistroVersion) const {
  for (const RootfsImage& Image : Images)
    if (Image.DistroMatch == DistroMatch && Image.DistroVersion == DistroVersion && Image.Type == "squashfs")
      return &Image;
  return nullptr;
}

std::string HttpUrl(const std::string& Url) {
  constexpr const char* Https = "https://";
  if (Url.compare(0, 8, Https) == 0)
    return "http://" + Url.substr(8);
  return Url;
}

std::string UrlFileName(const std::string& Url) {
  const size_t Slash = Url.rfind('/');
  return Slash == std::string::npos ? Url : Url.substr(Slash + 1);
}

bool ParseRootfsIndex(const std::string& JsonText, RootfsIndex& Out, std::string& Error) {
  Out = {};
  Json Root;
  if (!Json::Parse(JsonText, Root, Error))
    return false;
  const Json* V1 = Root.Find("v1");
  if (!Root.IsObject() || !V1 || !V1->IsObject()) {
    Error = "not a RootFS index (no \"v1\" object)";
    return false;
  }
  for (const auto& [Key, Entry] : V1->Members()) {
    if (!Entry.IsObject())
      continue;
    RootfsImage Image;
    Image.Key = Key;
    Image.DistroMatch = StringField(Entry, "DistroMatch");
    Image.DistroVersion = StringField(Entry, "DistroVersion");
    Image.Url = HttpUrl(StringField(Entry, "URL"));
    Image.Hash = StringField(Entry, "Hash");
    Image.Type = StringField(Entry, "Type");
    Image.Name = StringField(Entry, "Name");
    if (Image.Name.empty())
      Image.Name = Key;
    Image.FileName = UrlFileName(Image.Url);
    Image.DownloadBytes = NumberField(Entry, "DownloadSize");
    Image.InstalledBytes = NumberField(Entry, "InstalledSize");
    Image.ExtraDownloadBytes = NumberField(Entry, "ExtraDownloadSize");
    if (const Json* Extras = Entry.Find("Extras"); Extras && Extras->IsArray())
      for (const Json& Item : Extras->Items())
        if (Item.IsString())
          Image.Extras.push_back(Item.AsString());
    if (const Json* Available = Entry.Find("Available"); Available && Available->IsBool())
      Image.Available = Available->AsBool(true);
    Image.Reason = StringField(Entry, "Reason");
    if (Image.Available) {
      // A usable entry needs a URL and a valid hash; others are skipped.
      if (Image.Url.empty() || Image.DistroMatch.empty() || !IsHex64(Image.Hash))
        continue;
    } else if (Image.Reason.empty()) {
      Image.Reason = "Not available yet";
    }
    Out.Images.push_back(std::move(Image));
  }
  if (Out.Images.empty()) {
    Error = "the index lists no images";
    return false;
  }
  return true;
}

const RootfsIndex& BuiltInRootfsIndex() {
  static const RootfsIndex Index = [] {
    RootfsIndex Parsed;
    const Blob Text = GetRootfsIndexJson();
    std::string Error;
    ParseRootfsIndex(std::string(reinterpret_cast<const char*>(Text.Data), Text.Size), Parsed, Error);
    return Parsed;
  }();
  return Index;
}

namespace {
// "http://host/x" -> "host"; empty for anything else.
std::string UrlHost(const std::string& Url) {
  constexpr std::string_view Http = "http://";
  if (Url.compare(0, Http.size(), Http) != 0) return {};
  const size_t End = Url.find('/', Http.size());
  return Url.substr(Http.size(), End == std::string::npos ? std::string::npos : End - Http.size());
}
} // namespace

bool FindNewer(const RootfsImage& Current, const RootfsIndex& Live, RootfsImage& Out) {
  const RootfsImage* Best = nullptr;
  const std::string CurrentHost = UrlHost(Current.Url);
  const std::string CurrentDate = DateFolder(Current.Url);
  for (const RootfsImage& Candidate : Live.Images) {
    if (Candidate.DistroMatch != Current.DistroMatch || Candidate.DistroVersion != Current.DistroVersion || Candidate.Type != "squashfs" ||
        !Candidate.Selectable() || Candidate.Url == Current.Url || Candidate.Hash == Current.Hash)
      continue;
    // The live index comes over plain HTTP: only offer images from the host the built-in entry trusts.
    if (CurrentHost.empty() || UrlHost(Candidate.Url) != CurrentHost)
      continue;
    const std::string Date = DateFolder(Candidate.Url);
    if (Date.empty() || Date <= CurrentDate)
      continue;
    if (!Best || Date > DateFolder(Best->Url))
      Best = &Candidate;
  }
  if (!Best)
    return false;
  Out = *Best;
  Out.Name = Current.Name;
  Out.DownloadBytes = 0;  // learned from the server
  Out.InstalledBytes = Current.InstalledBytes;
  Out.ExtraDownloadBytes = Current.ExtraDownloadBytes;
  Out.Extras = Current.Extras;
  return true;
}

IndexChoice ChooseIndex(const std::string& LiveText) {
  IndexChoice Choice;
  Choice.Index = BuiltInRootfsIndex();
  if (LiveText.empty()) {
    Choice.Note = "Using the built-in index";
    return Choice;
  }
  RootfsIndex Live;
  std::string Error;
  if (!ParseRootfsIndex(LiveText, Live, Error)) {
    Choice.Note = "The online index could not be read (" + Error + "); using the built-in index";
    return Choice;
  }
  Choice.LiveUsed = true;
  Choice.Note = "Using the built-in index; no newer image online";
  std::vector<RootfsImage> Updated;
  for (const RootfsImage& Built : Choice.Index.Images) {
    RootfsImage Newer;
    if (Built.Selectable() && FindNewer(Built, Live, Newer)) {
      Choice.Note = "A newer " + Built.Name + " image is available (online index over plain HTTP: not authenticated)";
      Updated.push_back(std::move(Newer));
    }
  }
  // A newer entry replaces the built-in one of the same distro and version.
  for (RootfsImage& Newer : Updated)
    for (RootfsImage& Built : Choice.Index.Images)
      if (Built.DistroMatch == Newer.DistroMatch && Built.DistroVersion == Newer.DistroVersion && Built.Type == "squashfs")
        Built = Newer;
  return Choice;
}

} // namespace ui
