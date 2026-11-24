// SPDX-License-Identifier: MIT
#pragma once

// A small FTP server (RFC 959, with EPSV from RFC 2428 and SIZE, MDTM,
// REST, MLST and MLSD from RFC 3659) for moving files in and out of the
// guest root filesystem. File operations go through FtpFileSystem, which on
// Switch 2 calls the guest's own Linux syscall handlers, so paths longer
// than the SD card can hold work like they do for guest programs.
//
// Up to MaxSessions clients are connected at once (desktop clients open
// extra connections for transfers), in passive mode only. The server is
// polled from the caller's loop through Step(), so the launcher stays
// responsive between commands. Sessions are served one at a time;
// transfers run to completion inside Step() with a timeout on each data
// socket operation.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Switch2Guest {

struct FtpEntry {
  std::string Name;
  uint32_t Mode {}; // Linux st_mode
  uint64_t Links {};
  uint32_t User {};
  uint32_t Group {};
  uint64_t Size {};
  int64_t ModifySeconds {};
};

// Linux file operations on absolute guest paths. Errors are negative
// Linux errno values. The guest root filesystem has no user database, so
// there are no FTP accounts: the backend decides the identity it acts as.
class FtpFileSystem {
public:
  virtual ~FtpFileSystem() = default;
  // lstat semantics when NoFollow is set, stat otherwise.
  virtual int64_t Stat(const std::string& Path, FtpEntry& Entry, bool NoFollow) = 0;
  // Entries other than "." and "..", with lstat information.
  virtual int64_t List(const std::string& Path, std::vector<FtpEntry>& Entries) = 0;
  // Opens a regular file. Returns a handle.
  virtual int64_t OpenRead(const std::string& Path, uint64_t Offset) = 0;
  virtual int64_t OpenWrite(const std::string& Path, uint64_t Offset, bool Append) = 0;
  virtual int64_t Read(int64_t Handle, void* Buffer, size_t Count) = 0;
  virtual int64_t Write(int64_t Handle, const void* Buffer, size_t Count) = 0;
  virtual int64_t Close(int64_t Handle) = 0;
  // chdir semantics: a directory the user may search.
  virtual int64_t EnterDirectory(const std::string& Path) = 0;
  virtual int64_t MakeDirectory(const std::string& Path) = 0;
  virtual int64_t RemoveDirectory(const std::string& Path) = 0;
  virtual int64_t Remove(const std::string& Path) = 0;
  virtual int64_t Rename(const std::string& From, const std::string& To) = 0;
  virtual int64_t SetModifyTime(const std::string& Path, int64_t Seconds) = 0;
};

inline const char* LinuxErrorText(int64_t Error) {
  switch (-Error) {
  case 1: return "Operation not permitted";
  case 2: return "No such file or directory";
  case 5: return "Input/output error";
  case 13: return "Permission denied";
  case 17: return "File exists";
  case 18: return "Invalid cross-device link";
  case 20: return "Not a directory";
  case 21: return "Is a directory";
  case 22: return "Invalid argument";
  case 24: return "Too many open files";
  case 27: return "File too large";
  case 28: return "No space left on device";
  case 30: return "Read-only file system";
  case 36: return "File name too long";
  case 39: return "Directory not empty";
  case 40: return "Too many levels of symbolic links";
  default: return "Operation failed";
  }
}

class FtpServer {
public:
  static constexpr size_t MaxLine = 8192;
  static constexpr size_t MaxSessions = 8;
  static constexpr int DataTimeoutMs = 30000;

  FtpServer(FtpFileSystem& Files, std::string Password) : Files {Files}, Password {std::move(Password)} {}
  ~FtpServer() { Stop(); }

  // Listens on all interfaces. Returns 0 or a negative errno.
  // Address is in network byte order. Horizon's socket service only lets
  // homebrew listen on the console's own address (as ftpd does with
  // gethostid()), not INADDR_ANY. FailedCall names the call that failed.
  int Start(uint16_t Port, uint32_t Address4 = htonl(INADDR_ANY)) {
    FailedCall = "socket";
    Listener = socket(AF_INET, SOCK_STREAM, 0);
    if (Listener < 0) return -errno;
    const int Enable = 1;
    setsockopt(Listener, SOL_SOCKET, SO_REUSEADDR, &Enable, sizeof(Enable));
    sockaddr_in Address {};
    Address.sin_family = AF_INET;
    Address.sin_port = htons(Port);
    Address.sin_addr.s_addr = Address4;
    FailedCall = "bind";
    int Failed = bind(Listener, reinterpret_cast<sockaddr*>(&Address), sizeof(Address));
    if (!Failed) {
      FailedCall = "listen";
      Failed = listen(Listener, 2);
    }
    if (Failed) {
      const int Error = -errno;
      Stop();
      return Error;
    }
    FailedCall = "";
    return 0;
  }

  const char* FailedCall = "";

  uint16_t Port() const {
    sockaddr_in Address {};
    socklen_t Length = sizeof(Address);
    if (Listener < 0 || getsockname(Listener, reinterpret_cast<sockaddr*>(&Address), &Length)) return 0;
    return ntohs(Address.sin_port);
  }

  void Stop() {
    for (auto& Connection : Sessions) {
      Serve(Connection, [&] { CloseClient(); });
    }
    Sessions.clear();
    CloseFD(Listener);
  }

  size_t Connections() const { return Sessions.size(); }
  // Receives each command (passwords masked) and each reply line.
  std::function<void(const std::string&)> Trace;
  const std::string& Status() const { return LastStatus; }

  // Waits up to TimeoutMs for a connection or a command and handles it.
  // Returns true when something happened that the caller may want to show.
  bool Step(int TimeoutMs) {
    if (Listener < 0) return false;
    std::vector<pollfd> Events {{Listener, POLLIN, 0}};
    for (const auto& Connection : Sessions) Events.push_back({Connection.Client, POLLIN, 0});
    if (poll(Events.data(), Events.size(), TimeoutMs) <= 0) return false;
    bool Changed = false;
    // Existing sessions first, so a client that hung up is gone before a
    // reconnect is counted against MaxSessions.
    for (size_t Index = 0; Index < Sessions.size(); ++Index) {
      if (Events[Index + 1].revents & (POLLIN | POLLHUP | POLLERR)) {
        Serve(Sessions[Index], [&] { Changed |= Receive(); });
      }
    }
    const auto Before = Sessions.size();
    std::erase_if(Sessions, [](const Session& Connection) { return Connection.Client < 0; });
    if (Sessions.size() != Before) LastStatus = Describe("Client disconnected");
    if (Events[0].revents & POLLIN) {
      const int Incoming = accept(Listener, nullptr, nullptr);
      if (Incoming >= 0 && Sessions.size() >= MaxSessions) {
        SendAll(Incoming, "421 Too many connections.\r\n");
        close(Incoming);
      } else if (Incoming >= 0) {
        Sessions.emplace_back();
        Sessions.back().Client = Incoming;
        Serve(Sessions.back(), [&] { Reply(220, "FEX Switch 2 guest filesystem"); });
        std::erase_if(Sessions, [](const Session& Connection) { return Connection.Client < 0; });
        LastStatus = Describe("Client connected");
        Changed = true;
      }
    }
    return Changed;
  }

private:
  FtpFileSystem& Files;
  std::string Password;
  int Listener {-1};
  std::string LastStatus {"Waiting for a client"};

  // State of one control connection.
  struct Session {
    int Client {-1};
    int Passive {-1};
    std::string Input;
    std::string User;
    bool LoggedIn {};
    std::string Directory {"/"};
    std::string RenameFrom;
    uint64_t Restart {};
  };
  std::vector<Session> Sessions;

  // The session being served. Command handling below works on these
  // fields; Serve() swaps a session's state in and back out around it.
  int Client {-1};
  int Passive {-1};
  std::string Input;
  std::string User;
  bool LoggedIn {};
  std::string Directory {"/"};
  std::string RenameFrom;
  uint64_t Restart {};

  template<typename Work>
  void Serve(Session& Connection, Work&& Body) {
    auto Exchange = [&] {
      std::swap(Client, Connection.Client);
      std::swap(Passive, Connection.Passive);
      std::swap(Input, Connection.Input);
      std::swap(User, Connection.User);
      std::swap(LoggedIn, Connection.LoggedIn);
      std::swap(Directory, Connection.Directory);
      std::swap(RenameFrom, Connection.RenameFrom);
      std::swap(Restart, Connection.Restart);
    };
    Exchange();
    Body();
    Exchange();
  }

  std::string Describe(const char* Event) const {
    if (Sessions.empty()) return std::string(Event) + ", none connected";
    return std::string(Event) + ", " + std::to_string(Sessions.size()) + " connected";
  }

  // Reads from the served session's control connection and runs every
  // complete command. Returns true when a command was handled.
  bool Receive() {
    char Buffer[4096];
    const auto Received = recv(Client, Buffer, sizeof(Buffer), 0);
    if (Received <= 0) {
      CloseClient();
      return true;
    }
    Input.append(Buffer, Received);
    bool Handled = false;
    size_t End;
    while (Client >= 0 && (End = Input.find('\n')) != std::string::npos) {
      std::string Line = Input.substr(0, End);
      Input.erase(0, End + 1);
      if (!Line.empty() && Line.back() == '\r') Line.pop_back();
      Handle(Line);
      Handled = true;
    }
    if (Client >= 0 && Input.size() > MaxLine) {
      Reply(500, "Command line too long.");
      CloseClient();
    }
    return Handled;
  }

  static void CloseFD(int& FD) {
    if (FD >= 0) close(FD);
    FD = -1;
  }

  void CloseClient() {
    CloseFD(Passive);
    CloseFD(Client);
    LoggedIn = false;
    RenameFrom.clear();
    Restart = 0;
  }

  static bool WaitFor(int FD, short Event) {
    pollfd Wait {FD, Event, 0};
    return poll(&Wait, 1, DataTimeoutMs) > 0 && (Wait.revents & (Event | POLLHUP));
  }

  static bool SendAll(int FD, std::string_view Data) {
    while (!Data.empty()) {
      if (!WaitFor(FD, POLLOUT)) return false;
      const auto Sent = send(FD, Data.data(), Data.size(), 0);
      if (Sent < 0 && errno == EINTR) continue;
      if (Sent <= 0) return false;
      Data.remove_prefix(Sent);
    }
    return true;
  }

  void Reply(int Code, std::string_view Text) {
    std::string Line = std::to_string(Code) + " " + std::string(Text) + "\r\n";
    if (Trace) Trace("FTP < " + Line.substr(0, Line.size() - 2));
    if (!SendAll(Client, Line)) CloseClient();
  }

  void ReplyError(int64_t Error) { Reply(550, LinuxErrorText(Error)); }

  // Joins an argument with the working directory. The guest resolves ".."
  // and symlinks itself; only the working directory is kept normalized.
  std::string Resolve(std::string_view Argument) const {
    if (Argument.empty()) return Directory;
    if (Argument.front() == '/') return std::string(Argument);
    return Directory + (Directory.ends_with('/') ? "" : "/") + std::string(Argument);
  }

  static std::string Normalize(std::string_view Path) {
    std::vector<std::string_view> Parts;
    while (!Path.empty()) {
      const auto Slash = Path.find('/');
      const auto Part = Path.substr(0, Slash);
      Path = Slash == Path.npos ? std::string_view {} : Path.substr(Slash + 1);
      if (Part.empty() || Part == ".") continue;
      if (Part == "..") {
        if (!Parts.empty()) Parts.pop_back();
      } else {
        Parts.push_back(Part);
      }
    }
    std::string Result;
    for (const auto Part : Parts) Result.append("/").append(Part);
    return Result.empty() ? "/" : Result;
  }

  static std::string Quote(std::string_view Path) {
    std::string Result = "\"";
    for (const char Character : Path) {
      Result += Character;
      if (Character == '"') Result += '"';
    }
    return Result + "\"";
  }

  static std::string TimeText(int64_t Seconds, const char* Format) {
    const time_t Time = Seconds;
    tm Parts {};
    gmtime_r(&Time, &Parts);
    char Text[32];
    std::strftime(Text, sizeof(Text), Format, &Parts);
    return Text;
  }

  // YYYYMMDDHHMMSS in UTC, as used by MDTM and MFMT.
  static bool ParseTime(std::string_view Text, int64_t& Seconds) {
    int Values[6] {};
    static constexpr int Widths[6] {4, 2, 2, 2, 2, 2};
    for (int Field = 0, Position = 0; Field < 6; Position += Widths[Field++]) {
      for (int Digit = 0; Digit < Widths[Field]; ++Digit) {
        const char Character = Text[Position + Digit];
        if (Character < '0' || Character > '9') return false;
        Values[Field] = Values[Field] * 10 + (Character - '0');
      }
    }
    const auto [Year, Month, Day, Hour, Minute, Second] = Values;
    if (Month < 1 || Month > 12 || Day < 1 || Day > 31 || Hour > 23 || Minute > 59 || Second > 60) return false;
    // Days from the civil date (Howard Hinnant's algorithm).
    const int64_t ShiftedYear = Year - (Month <= 2);
    const int64_t Era = ShiftedYear / 400;
    const int64_t YearOfEra = ShiftedYear - Era * 400;
    const int64_t DayOfYear = (153 * (Month + (Month > 2 ? -3 : 9)) + 2) / 5 + Day - 1;
    const int64_t DayOfEra = YearOfEra * 365 + YearOfEra / 4 - YearOfEra / 100 + DayOfYear;
    const int64_t Days = Era * 146097 + DayOfEra - 719468;
    Seconds = Days * 86400 + Hour * 3600 + Minute * 60 + Second;
    return true;
  }

  // The date columns of `ls -l`, which clients parse by field count: three
  // fields, the time of day within six months of now, else the year.
  static std::string ListTime(int64_t Seconds) {
    constexpr int64_t SixMonths = 15778476;
    const int64_t Now = time(nullptr);
    const bool Recent = Seconds > Now - SixMonths && Seconds < Now + SixMonths;
    return TimeText(Seconds, Recent ? "%b %e %H:%M" : "%b %e  %Y");
  }

  static std::string ListLine(const FtpEntry& Entry) {
    static constexpr char Types[] = "?pc?d?b?-?l?s???";
    std::string Mode(10, '-');
    Mode[0] = Types[(Entry.Mode >> 12) & 15];
    constexpr char Bits[] = "rwxrwxrwx";
    for (int Index = 0; Index < 9; ++Index) {
      if (Entry.Mode & (0400 >> Index)) Mode[1 + Index] = Bits[Index];
    }
    if (Entry.Mode & 04000) Mode[3] = Mode[3] == 'x' ? 's' : 'S';
    if (Entry.Mode & 02000) Mode[6] = Mode[6] == 'x' ? 's' : 'S';
    if (Entry.Mode & 01000) Mode[9] = Mode[9] == 'x' ? 't' : 'T';
    char Prefix[128];
    std::snprintf(Prefix, sizeof(Prefix), "%s %3llu %-8u %-8u %12llu %s ", Mode.c_str(),
                  static_cast<unsigned long long>(Entry.Links), Entry.User, Entry.Group,
                  static_cast<unsigned long long>(Entry.Size), ListTime(Entry.ModifySeconds).c_str());
    return Prefix + Entry.Name + "\r\n";
  }

  static std::string Facts(const FtpEntry& Entry, std::string_view Type) {
    char Text[192];
    const bool Directory = (Entry.Mode & 0170000) == 0040000;
    std::snprintf(Text, sizeof(Text), "type=%.*s;size=%llu;modify=%s;UNIX.mode=0%o;UNIX.uid=%u;UNIX.gid=%u; ",
                  static_cast<int>(Type.size()), Type.data(), static_cast<unsigned long long>(Directory ? 0 : Entry.Size),
                  TimeText(Entry.ModifySeconds, "%Y%m%d%H%M%S").c_str(), Entry.Mode & 07777, Entry.User, Entry.Group);
    return Text;
  }

  static std::string_view FactType(const FtpEntry& Entry) {
    switch (Entry.Mode & 0170000) {
    case 0040000: return "dir";
    case 0100000: return "file";
    case 0120000: return "OS.unix=symlink";
    default: return "OS.unix=special";
    }
  }

  // Listens for one data connection on the control connection's local
  // address. Lets the system choose the port, and if that is refused
  // walks a fixed unprivileged range instead.
  int OpenPassive(sockaddr_in& Address) {
    CloseFD(Passive);
    socklen_t Length = sizeof(Address);
    if (getsockname(Client, reinterpret_cast<sockaddr*>(&Address), &Length)) return -errno;
    int Error = 0;
    for (uint32_t Attempt = 0; Attempt <= 64; ++Attempt) {
      Address.sin_port = htons(Attempt ? static_cast<uint16_t>(50000 + (NextDataPort++ % 1000)) : 0);
      Passive = socket(AF_INET, SOCK_STREAM, 0);
      if (Passive < 0) return -errno;
      Length = sizeof(Address);
      if (!bind(Passive, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) && !listen(Passive, 1) &&
          !getsockname(Passive, reinterpret_cast<sockaddr*>(&Address), &Length)) {
        return 0;
      }
      Error = -errno;
      CloseFD(Passive);
    }
    return Error;
  }

  uint32_t NextDataPort {};

  // Accepts the data connection announced by PASV or EPSV.
  int AcceptData() {
    if (Passive < 0) {
      Reply(425, "Use PASV or EPSV first.");
      return -1;
    }
    Reply(150, "Opening data connection.");
    int Data = -1;
    if (WaitFor(Passive, POLLIN)) Data = accept(Passive, nullptr, nullptr);
    CloseFD(Passive);
    if (Data < 0) Reply(425, "Data connection was not opened.");
    return Data;
  }

  void SendListing(std::string_view Argument, char Format) {
    // Clients pass ls options such as "-la"; the listing is always long.
    while (Format == 'L' && Argument.starts_with('-')) {
      const auto Space = Argument.find(' ');
      Argument = Space == Argument.npos ? std::string_view {} : Argument.substr(Space + 1);
    }
    const auto Path = Resolve(Argument);
    FtpEntry Self {};
    auto Error = Files.Stat(Path, Self, false);
    std::vector<FtpEntry> Entries;
    if (!Error && (Self.Mode & 0170000) == 0040000) Error = Files.List(Path, Entries);
    else if (!Error && Format == 'M') Error = -20;
    else if (!Error) {
      Self.Name = std::string(Argument);
      Entries.push_back(Self);
    }
    if (Error) {
      CloseFD(Passive);
      ReplyError(Error);
      return;
    }
    const int Data = AcceptData();
    if (Data < 0) return;
    std::string Text;
    bool Sent = true;
    for (const auto& Entry : Entries) {
      if (Format == 'N') Text += Entry.Name + "\r\n";
      else if (Format == 'M') Text += Facts(Entry, FactType(Entry)) + Entry.Name + "\r\n";
      else Text += ListLine(Entry);
      if (Text.size() > 65536) {
        Sent = Sent && SendAll(Data, Text);
        Text.clear();
      }
    }
    Sent = Sent && SendAll(Data, Text);
    close(Data);
    Reply(Sent ? 226 : 426, Sent ? "Transfer complete." : "Transfer aborted.");
  }

  void Retrieve(const std::string& Path) {
    const auto Offset = std::exchange(Restart, 0);
    FtpEntry Entry {};
    auto Error = Files.Stat(Path, Entry, false);
    if (!Error && (Entry.Mode & 0170000) != 0100000) Error = (Entry.Mode & 0170000) == 0040000 ? -21 : -22;
    const auto Handle = Error ? Error : Files.OpenRead(Path, Offset);
    if (Handle < 0) {
      CloseFD(Passive);
      ReplyError(Handle);
      return;
    }
    const int Data = AcceptData();
    if (Data < 0) {
      Files.Close(Handle);
      return;
    }
    std::vector<char> Buffer(256 * 1024);
    int64_t Result = 0;
    bool Sent = true;
    while (Sent && (Result = Files.Read(Handle, Buffer.data(), Buffer.size())) > 0) {
      Sent = SendAll(Data, {Buffer.data(), static_cast<size_t>(Result)});
    }
    Files.Close(Handle);
    close(Data);
    if (Result < 0) Reply(451, LinuxErrorText(Result));
    else Reply(Sent ? 226 : 426, Sent ? "Transfer complete." : "Transfer aborted.");
  }

  void Store(const std::string& Path, bool Append) {
    const auto Offset = std::exchange(Restart, 0);
    const auto Handle = Files.OpenWrite(Path, Offset, Append);
    if (Handle < 0) {
      CloseFD(Passive);
      ReplyError(Handle);
      return;
    }
    const int Data = AcceptData();
    if (Data < 0) {
      Files.Close(Handle);
      return;
    }
    std::vector<char> Buffer(256 * 1024);
    int64_t Error = 0;
    bool Complete = false;
    while (!Error) {
      if (!WaitFor(Data, POLLIN)) break;
      const auto Received = recv(Data, Buffer.data(), Buffer.size(), 0);
      if (Received < 0 && errno == EINTR) continue;
      if (Received <= 0) {
        Complete = Received == 0;
        break;
      }
      for (size_t Done = 0; Done < static_cast<size_t>(Received) && !Error;) {
        const auto Written = Files.Write(Handle, Buffer.data() + Done, Received - Done);
        if (Written <= 0) Error = Written ? Written : -5;
        else Done += Written;
      }
    }
    const auto Closed = Files.Close(Handle);
    close(Data);
    if (!Error) Error = Closed;
    if (Error) Reply(Error == -28 ? 452 : 451, LinuxErrorText(Error));
    else Reply(Complete ? 226 : 426, Complete ? "Transfer complete." : "Transfer aborted.");
  }

  void Handle(const std::string& Line) {
    const auto Space = Line.find(' ');
    std::string Command = Line.substr(0, Space);
    for (auto& Character : Command) Character = static_cast<char>(std::toupper(static_cast<unsigned char>(Character)));
    const std::string Argument = Space == Line.npos ? "" : Line.substr(Space + 1);
    if (Trace) Trace("FTP > " + (Command == "PASS" ? std::string("PASS ****") : Line));
    if (Command != "RNTO" && Command != "RNFR") RenameFrom.clear();
    if (Command != "REST" && Command != "RETR" && Command != "STOR" && Command != "APPE") Restart = 0;

    if (Command == "USER") {
      User = Argument;
      LoggedIn = false;
      return Reply(331, "Any user name works; enter the password shown on the console.");
    }
    if (Command == "PASS") {
      if (User.empty()) return Reply(503, "Send USER first.");
      if (Argument != Password) {
        LastStatus = "Rejected a wrong password";
        return Reply(530, "Login incorrect.");
      }
      // Any user name is accepted; only the password shown on the console
      // grants access.
      LoggedIn = true;
      LastStatus = "Client logged in";
      return Reply(230, "Logged in.");
    }
    if (Command == "QUIT") {
      Reply(221, "Goodbye.");
      CloseClient();
      return;
    }
    if (Command == "NOOP") return Reply(200, "OK.");
    if (Command == "SYST") return Reply(215, "UNIX Type: L8");
    if (Command == "FEAT") {
      return Reply(211, "-Features:\r\n EPSV\r\n MDTM\r\n MFMT\r\n MLST type*;size*;modify*;UNIX.mode*;UNIX.uid*;UNIX.gid*;\r\n"
                        " PASV\r\n REST STREAM\r\n SIZE\r\n UTF8\r\n211 End");
    }
    if (Command == "OPTS") return Reply(Argument == "UTF8 ON" || Argument == "utf8 on" ? 200 : 501, "OPTS");
    if (!LoggedIn) return Reply(530, "Log in first.");

    if (Command == "TYPE") return Reply(Argument == "I" || Argument == "A" || Argument == "L 8" ? 200 : 504, "Type set; transfers are binary.");
    if (Command == "MODE") return Reply(Argument == "S" ? 200 : 504, "Mode");
    if (Command == "STRU") return Reply(Argument == "F" ? 200 : 504, "Structure");
    if (Command == "PWD" || Command == "XPWD") return Reply(257, Quote(Directory) + " is the current directory.");
    if (Command == "CWD" || Command == "XCWD" || Command == "CDUP" || Command == "XCUP") {
      const auto Target = Command.ends_with("UP") ? Resolve("..") : Resolve(Argument);
      if (const auto Error = Files.EnterDirectory(Target)) return ReplyError(Error);
      Directory = Normalize(Target);
      return Reply(250, "Directory changed.");
    }
    if (Command == "PASV") {
      sockaddr_in Address {};
      if (const auto Error = OpenPassive(Address)) return Reply(425, LinuxErrorText(Error));
      const auto Host = ntohl(Address.sin_addr.s_addr);
      const auto Port = ntohs(Address.sin_port);
      char Text[96];
      std::snprintf(Text, sizeof(Text), "Entering Passive Mode (%u,%u,%u,%u,%u,%u).", Host >> 24, (Host >> 16) & 255,
                    (Host >> 8) & 255, Host & 255, Port >> 8, Port & 255);
      return Reply(227, Text);
    }
    if (Command == "EPSV") {
      if (!Argument.empty() && Argument != "1" && Argument != "ALL" && Argument != "all") return Reply(522, "Use IPv4 (1).");
      sockaddr_in Address {};
      if (const auto Error = OpenPassive(Address)) return Reply(425, LinuxErrorText(Error));
      return Reply(229, "Entering Extended Passive Mode (|||" + std::to_string(ntohs(Address.sin_port)) + "|).");
    }
    if (Command == "PORT" || Command == "EPRT") return Reply(502, "Active mode is not supported; use passive mode.");
    if (Command == "LIST") return SendListing(Argument, 'L');
    if (Command == "NLST") return SendListing(Argument, 'N');
    if (Command == "MLSD") return SendListing(Argument, 'M');
    if (Command == "MLST") {
      const auto Path = Resolve(Argument);
      FtpEntry Entry {};
      if (const auto Error = Files.Stat(Path, Entry, true)) return ReplyError(Error);
      return Reply(250, "-Listing\r\n " + Facts(Entry, FactType(Entry)) + Path + "\r\n250 End");
    }
    if (Command == "REST") {
      char* End {};
      errno = 0;
      const auto Value = std::strtoull(Argument.c_str(), &End, 10);
      if (Argument.empty() || *End || errno || Value > INT64_MAX) return Reply(501, "Invalid offset.");
      Restart = Value;
      return Reply(350, "Restarting at " + Argument + ".");
    }
    if (Command == "RETR") return Retrieve(Resolve(Argument));
    if (Command == "STOR" || Command == "APPE") return Store(Resolve(Argument), Command == "APPE");
    if (Command == "SIZE" || Command == "MDTM") {
      FtpEntry Entry {};
      auto Error = Files.Stat(Resolve(Argument), Entry, false);
      if (!Error && (Entry.Mode & 0170000) != 0100000) Error = -21;
      if (Error) return ReplyError(Error);
      return Reply(213, Command == "SIZE" ? std::to_string(Entry.Size) : TimeText(Entry.ModifySeconds, "%Y%m%d%H%M%S"));
    }
    if (Command == "MFMT") {
      const auto Separator = Argument.find(' ');
      int64_t Seconds {};
      if (Separator != 14 || !ParseTime(std::string_view(Argument).substr(0, 14), Seconds)) return Reply(501, "Use MFMT YYYYMMDDHHMMSS path.");
      if (const auto Error = Files.SetModifyTime(Resolve(Argument.substr(15)), Seconds)) return ReplyError(Error);
      return Reply(213, "Modify=" + Argument.substr(0, 14) + "; " + Argument.substr(15));
    }
    if (Command == "DELE") {
      if (const auto Error = Files.Remove(Resolve(Argument))) return ReplyError(Error);
      return Reply(250, "Deleted.");
    }
    if (Command == "MKD" || Command == "XMKD") {
      const auto Path = Resolve(Argument);
      if (const auto Error = Files.MakeDirectory(Path)) return ReplyError(Error);
      return Reply(257, Quote(Normalize(Path)) + " created.");
    }
    if (Command == "RMD" || Command == "XRMD") {
      if (const auto Error = Files.RemoveDirectory(Resolve(Argument))) return ReplyError(Error);
      return Reply(250, "Removed.");
    }
    if (Command == "RNFR") {
      FtpEntry Entry {};
      const auto Path = Resolve(Argument);
      if (const auto Error = Files.Stat(Path, Entry, true)) return ReplyError(Error);
      RenameFrom = Path;
      return Reply(350, "Ready for RNTO.");
    }
    if (Command == "RNTO") {
      if (RenameFrom.empty()) return Reply(503, "Send RNFR first.");
      const auto From = std::exchange(RenameFrom, {});
      if (const auto Error = Files.Rename(From, Resolve(Argument))) return ReplyError(Error);
      return Reply(250, "Renamed.");
    }
    if (Command == "ABOR") return Reply(226, "No transfer in progress.");
    if (Command == "ALLO") return Reply(202, "No allocation needed.");
    Reply(502, "Command not implemented.");
  }
};

} // namespace Switch2Guest
