// SPDX-License-Identifier: MIT
// The host side of AF_INET: thin wrappers over the Horizon BSD socket
// service. Every function takes and returns BSD values (the translation
// to and from Linux lives in GuestInet.h), reports failure as a negative
// Linux errno taken from g_bsdErrno, and never blocks: host sockets are
// always nonblocking and one host thread runs every guest thread.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Switch2Guest::HostNet {

// Starts the socket service (4 BSD sessions) and nifm on the first call;
// later calls count. False only when the socket service failed to start.
// Shared by the FTP and rootfs setup screens and the guest runtime.
bool Acquire();
void Release();

// DNS servers of the current connection (network order, 0 for none). False without a connection.
bool DnsServers(uint32_t& Primary, uint32_t& Secondary);

using Address = std::array<uint8_t, 16>; // BSD sockaddr_in

struct PollEntry {
  int Fd;
  int16_t Events, Revents;
};

// Result values: a descriptor or byte count >= 0, or -(Linux errno).
int Socket(int BsdType, int Protocol);
int Close(int Fd);
int Bind(int Fd, const Address& To);
int Listen(int Fd, int Backlog);
int Connect(int Fd, const Address& To);
int Accept(int Fd, Address& Peer);
int64_t Send(int Fd, const void* Data, size_t Size, int BsdFlags, const Address* To);
int64_t Receive(int Fd, void* Data, size_t Size, int BsdFlags, Address* From, bool& HasFrom);
int GetName(int Fd, bool Peer, Address& Out);
int GetOption(int Fd, int Level, int Name, void* Out, size_t& Size);
int SetOption(int Fd, int Level, int Name, const void* Value, size_t Size);
int Shutdown(int Fd, int How);
int BytesPending(int Fd); // FIONREAD
// poll with the host's own bits and a timeout in milliseconds (0: do not wait).
int Poll(PollEntry* Entries, size_t Count, int TimeoutMs);

} // namespace Switch2Guest::HostNet
