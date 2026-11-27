// SPDX-License-Identifier: MIT
#pragma once

namespace Switch2Guest {
enum class DirectoryRenameKind { Replace, Exchange, ExchangeWithVirtual, Remove, Create };
enum class DirectoryRecovery {
  Finish,
  RestoreSource,
  RestoreDestination,
  RestoreBoth,
  RemoveBackup,
  CompleteExchange,
  Invalid,
};

// Old, New and Backup describe native directory presence after validating
// the entry types. A virtual destination has no native directory at New
// until the source directory is moved over it.
constexpr DirectoryRecovery PlanDirectoryRecovery(DirectoryRenameKind Kind, bool Committed,
                                                  bool Old, bool New, bool Backup) {
  using Plan = DirectoryRecovery;
  if (Kind == DirectoryRenameKind::Create) {
    if (Backup || (Old && New)) return Plan::Invalid;
    if (Committed) return Old ? Plan::RestoreDestination : New ? Plan::Finish : Plan::Invalid;
    return New ? Plan::Invalid : Old ? Plan::RemoveBackup : Plan::Finish;
  }
  if (Kind == DirectoryRenameKind::Remove && Committed) {
    if (Old || Backup) return Plan::Invalid;
    return New ? Plan::RemoveBackup : Plan::Finish;
  }
  if (Kind == DirectoryRenameKind::ExchangeWithVirtual || Kind == DirectoryRenameKind::Remove) {
    if (Backup) return Plan::Invalid;
    if (!Old && New) return Committed ? Plan::Finish : Plan::RestoreSource;
    return !Committed && Old && !New ? Plan::Finish : Plan::Invalid;
  }

  if (!Committed) {
    if (!Backup) return Old && New ? Plan::Finish : Plan::Invalid;
    if (!Old && New) return Plan::RestoreBoth;
    return Old && !New ? Plan::RestoreDestination : Plan::Invalid;
  }
  if (!New) return Plan::Invalid;
  if (Kind == DirectoryRenameKind::Exchange) {
    if (!Old && Backup) return Plan::CompleteExchange;
    return Old && !Backup ? Plan::Finish : Plan::Invalid;
  }
  if (Old) return Plan::Invalid;
  return Backup ? Plan::RemoveBackup : Plan::Finish;
}
} // namespace Switch2Guest
