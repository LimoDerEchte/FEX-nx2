// SPDX-License-Identifier: MIT
// Socket options for bulk transfers (FTP data connections, the RootFS
// download). Taken from OunceShell's ftpd (kurumuz/ounceshell,
// switch2/third_party/ftpd/ftp.c, ftp_set_socket_options): a 64 KiB
// SO_RCVBUF and SO_SNDBUF on every data socket plus TCP_NODELAY (we use
// 256 KiB, measured below). Its FTP
// server is much faster on the console than ours was without them.
// Set the options before connect() or listen(), so the TCP window
// they imply is in place from the handshake on (accepted sockets
// inherit them from the listener).
#pragma once

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace Switch2Guest {

// Requested SO_RCVBUF/SO_SNDBUF. Throughput is capped at about buffer / round-trip
// time. Console download probe (0.16.2, 100 Mbit line, rootfs.fex-emu.gg), 64 MiB
// each: 64 KiB 3.34 MiB/s, 256 KiB 11.92 MiB/s, 512 KiB 11.59, 1 MiB 11.62. 256 KiB
// fills the line; the socket service allows up to tcp_*_buf_max_size (1 MiB).
inline int TransferSocketBuffer = 262144;
// SO_RCVBUF as the socket reports it after the last TuneTransferSocket call (-1: unknown).
inline int LastTunedReceiveBuffer = -1;

// Returns false when the socket refused one of the options; callers keep
// going with the defaults then.
inline bool TuneTransferSocket(int Socket) {
  int Size = TransferSocketBuffer;
  bool Ok = setsockopt(Socket, SOL_SOCKET, SO_RCVBUF, &Size, sizeof(Size)) == 0;
  Ok = setsockopt(Socket, SOL_SOCKET, SO_SNDBUF, &Size, sizeof(Size)) == 0 && Ok;
  int Yes = 1;
  Ok = setsockopt(Socket, IPPROTO_TCP, TCP_NODELAY, &Yes, sizeof(Yes)) == 0 && Ok;
  int Actual = -1;
  socklen_t Length = sizeof(Actual);
  LastTunedReceiveBuffer = getsockopt(Socket, SOL_SOCKET, SO_RCVBUF, &Actual, &Length) == 0 ? Actual : -1;
  return Ok;
}

} // namespace Switch2Guest
