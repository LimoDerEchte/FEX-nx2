// SPDX-License-Identifier: MIT
// The license list (generated from the embedded licenses.json, so it cannot drift from what ships) and the first-run write of
// the embedded texts to sdmc:/fex/licenses/. The NRO is the only file the user copies; this puts the texts on the card.
#pragma once

#include <string>
#include <vector>

#include "Storage.h"

namespace ui {

inline constexpr const char* LicensesDir = "sdmc:/fex/licenses";

struct LicenseEntry {
  std::string Name, Role, License, Url, File, Note;
  bool Bundled = true;
};

bool ParseLicenses(const std::string& JsonText, std::vector<LicenseEntry>& Out, std::string& Error);
const std::vector<LicenseEntry>& EmbeddedLicenses();       // parsed once from the embedded licenses.json
bool LicenseTextAvailable(const LicenseEntry& E);          // the embedded text of E.File exists
std::string LicenseText(const LicenseEntry& E);            // empty when none
std::string LicenseTextPath(const LicenseEntry& E);        // where the launcher wrote it: sdmc:/fex/licenses/<file>

// Writes licenses.json and every embedded text into Dir when missing or different. Written gets the number of files written.
bool EnsureLicensesWritten(Storage& S, const std::string& Dir, int& Written, std::string& Error);

} // namespace ui
