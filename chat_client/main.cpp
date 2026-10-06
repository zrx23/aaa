/*==============================================================================
 *  文件: chat_client/main.cpp
 *  项目: C/S 架构双人聊天室 —— 客户端（Client）
 *  语言: C++23（使用 -std=c++23，禁止使用 C++26 特性）
 *  平台: Windows + MinGW-w64 (g++ 13 及以上) + CMake
 *------------------------------------------------------------------------------
 *  功能清单
 *    1. 使用 TCP Socket 连接服务端（纯命令行交互，无 GUI）。
 *    2. 发送/接收文字消息（一个接收线程 + 一个输入线程）。
 *    3. 发送任意大小的文件：/file <路径>（分片流式发送，不整块读入内存）。
 *    4. 接收对方发来的文件：自动保存到 downloads/ 目录。
 *    5. 连上后自动接收服务端下发的历史聊天记录。
 *    6. /get <文件名> 可从服务端重新下载历史文件（任意大小）。
 *------------------------------------------------------------------------------
 *  编译/运行
 *    cmake -S . -B build -G "MinGW Makefiles"
 *    cmake --build build -j
 *    ./build/chat_client.exe [服务器IP] [端口] [昵称]
 *============================================================================*/

/*------------------------------------------------------------------------------
 *  一、应用层协议（与 chat_server/main.cpp 完全一致，改一处必须改两处）
 *------------------------------------------------------------------------------
 *  帧结构:  [1 字节 帧类型][4 字节 负载长度][负载 ...]（长度为大端序）
 *
 *  ── 客户端 -> 服务端（上行） ──
 *    FT_HELLO      : [u16 名字长度][名字(UTF-8)]
 *    FT_TEXT       : [正文(UTF-8)]
 *    FT_FILE_BEGIN : [u32 传输ID][u64 文件总大小][u16 文件名长度][文件名(UTF-8)]
 *    FT_FILE_CHUNK : [u32 传输ID][原始字节...]
 *    FT_FILE_END   : [u32 传输ID]
 *    FT_FILE_ABORT : [u32 传输ID][原因(UTF-8)]
 *    FT_REQ_FILE   : [u16 文件名长度][文件名(UTF-8)]
 *    FT_BYE        : [空]
 *
 *  ── 服务端 -> 客户端（下行） ──
 *    FT_HELLO_ACK  : [u8 你的编号][u16 名字长度][名字(UTF-8)]
 *    FT_SYSTEM     : [u64 时间戳秒][正文(UTF-8)]
 *    FT_TEXT       : [u8 发送者编号][u64 时间戳秒][u16 名字长度][名字][正文]
 *    FT_FILE_BEGIN : [u8 发送者编号][u64 时间戳秒][u32 传输ID][u64 总大小][u16 文件名长度][文件名]
 *    FT_FILE_CHUNK : [u8 发送者编号][u32 传输ID][原始字节...]
 *    FT_FILE_END   : [u8 发送者编号][u32 传输ID]
 *    FT_FILE_ABORT : [u8 发送者编号][u32 传输ID][原因(UTF-8)]
 *    FT_FILE_NOTICE: [u8 发送者编号][u64 时间戳秒][u64 大小][u16 文件名长度][文件名]
 *---------------------------------------------------------------------------*/

// ============================== 0. 头文件 ==============================
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  error "本工程面向 Windows + MinGW 平台，请在 Windows 下编译。"
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// ============================== 1. 协议定义 ==============================
namespace proto {

constexpr uint16_t kDefaultPort  = 9080;                 // 默认端口（与服务端一致）
constexpr uint32_t kChunkSize    = 64u * 1024u;          // 文件分片 64 KiB
constexpr uint32_t kMaxFrameSize = 8u * 1024u * 1024u;   // 单个帧负载上限
constexpr int      kServerId     = 0;                    // 0 号 = 服务端

enum FrameType : uint8_t {
    FT_HELLO       = 1,
    FT_HELLO_ACK   = 2,
    FT_TEXT        = 3,
    FT_SYSTEM      = 4,
    FT_FILE_BEGIN  = 5,
    FT_FILE_CHUNK  = 6,
    FT_FILE_END    = 7,
    FT_FILE_ABORT  = 8,
    FT_FILE_NOTICE = 9,
    FT_REQ_FILE    = 10,
    FT_BYE         = 11,
};

struct Frame {
    uint8_t              type = 0;
    std::vector<uint8_t> payload;
};

// ------------------------- 1.1 写缓冲区的小工具 -------------------------
inline void putU8(std::vector<uint8_t>& b, uint8_t v) {
    b.push_back(v);
}
inline void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t((v >> 8) & 0xFF));
    b.push_back(uint8_t(v & 0xFF));
}
inline void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 3; i >= 0; --i) b.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}
inline void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 7; i >= 0; --i) b.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}
inline void putStr16(std::vector<uint8_t>& b, std::string_view s) {
    const size_t n = std::min<size_t>(s.size(), 0xFFFFu);
    putU16(b, uint16_t(n));
    b.insert(b.end(), s.begin(), s.begin() + std::ptrdiff_t(n));
}
inline void putRaw(std::vector<uint8_t>& b, const void* p, size_t n) {
    const auto* c = static_cast<const uint8_t*>(p);
    b.insert(b.end(), c, c + n);
}

// ------------------------- 1.2 读缓冲区的小工具 -------------------------
class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& d) : d_(d) {}

    bool u8(uint8_t& v) {
        if (pos_ + 1 > d_.size()) return false;
        v = d_[pos_++];
        return true;
    }
    bool u16(uint16_t& v) {
        if (pos_ + 2 > d_.size()) return false;
        v = uint16_t((uint16_t(d_[pos_]) << 8) | uint16_t(d_[pos_ + 1]));
        pos_ += 2;
        return true;
    }
    bool u32(uint32_t& v) {
        if (pos_ + 4 > d_.size()) return false;
        uint32_t r = 0;
        for (int i = 0; i < 4; ++i) r = (r << 8) | uint32_t(d_[pos_ + i]);
        pos_ += 4;
        v = r;
        return true;
    }
    bool u64(uint64_t& v) {
        if (pos_ + 8 > d_.size()) return false;
        uint64_t r = 0;
        for (int i = 0; i < 8; ++i) r = (r << 8) | uint64_t(d_[pos_ + i]);
        pos_ += 8;
        v = r;
        return true;
    }
    bool raw(void* dst, size_t n) {
        if (pos_ + n > d_.size()) return false;
        std::memcpy(dst, d_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    bool str16(std::string& s) {
        uint16_t n = 0;
        if (!u16(n)) return false;
        if (pos_ + n > d_.size()) return false;
        s.assign(reinterpret_cast<const char*>(d_.data() + pos_), n);
        pos_ += n;
        return true;
    }
    bool restString(std::string& s) {
        s.assign(reinterpret_cast<const char*>(d_.data() + pos_), d_.size() - pos_);
        pos_ = d_.size();
        return true;
    }
    // 只读访问剩余字节（不拷贝），用于接收文件分片
    const uint8_t* restPtr(size_t& n) const {
        n = d_.size() - pos_;
        return d_.data() + pos_;
    }

private:
    const std::vector<uint8_t>& d_;
    size_t                      pos_ = 0;
};

// ------------------------- 1.3 客户端组帧（上行帧） -------------------------
inline Frame makeHello(const std::string& name) {
    Frame f;
    f.type = FT_HELLO;
    putStr16(f.payload, name);
    return f;
}

inline Frame makeText(const std::string& text) {
    Frame f;
    f.type = FT_TEXT;
    putRaw(f.payload, text.data(), text.size());
    return f;
}

inline Frame makeFileBegin(uint32_t xferId, uint64_t size, const std::string& fileName) {
    Frame f;
    f.type = FT_FILE_BEGIN;
    putU32(f.payload, xferId);
    putU64(f.payload, size);
    putStr16(f.payload, fileName);
    return f;
}

inline Frame makeFileChunk(uint32_t xferId, const uint8_t* data, size_t n) {
    Frame f;
    f.type = FT_FILE_CHUNK;
    putU32(f.payload, xferId);
    putRaw(f.payload, data, n);
    return f;
}

inline Frame makeFileEnd(uint32_t xferId) {
    Frame f;
    f.type = FT_FILE_END;
    putU32(f.payload, xferId);
    return f;
}

inline Frame makeFileAbort(uint32_t xferId, const std::string& why) {
    Frame f;
    f.type = FT_FILE_ABORT;
    putU32(f.payload, xferId);
    putRaw(f.payload, why.data(), why.size());
    return f;
}

inline Frame makeReqFile(const std::string& name) {
    Frame f;
    f.type = FT_REQ_FILE;
    putStr16(f.payload, name);
    return f;
}

inline Frame makeBye() {
    Frame f;
    f.type = FT_BYE;
    return f;
}

// ------------------------- 1.4 客户端解帧（下行帧） -------------------------
inline bool parseHelloAck(const Frame& f, int& myId, std::string& name) {
    if (f.type != FT_HELLO_ACK) return false;
    Reader  r(f.payload);
    uint8_t id = 0;
    if (!r.u8(id) || !r.str16(name)) return false;
    myId = id;
    return true;
}

inline bool parseSystem(const Frame& f, uint64_t& ts, std::string& text) {
    if (f.type != FT_SYSTEM) return false;
    Reader r(f.payload);
    return r.u64(ts) && r.restString(text);
}

inline bool parseText(const Frame& f, int& sender, uint64_t& ts, std::string& who,
                      std::string& text) {
    if (f.type != FT_TEXT) return false;
    Reader  r(f.payload);
    uint8_t id = 0;
    if (!r.u8(id) || !r.u64(ts) || !r.str16(who) || !r.restString(text)) return false;
    sender = id;
    return true;
}

inline bool parseFileBegin(const Frame& f, int& sender, uint64_t& ts, uint32_t& xferId,
                           uint64_t& size, std::string& fileName) {
    if (f.type != FT_FILE_BEGIN) return false;
    Reader  r(f.payload);
    uint8_t id = 0;
    if (!r.u8(id) || !r.u64(ts) || !r.u32(xferId) || !r.u64(size) ||
        !r.str16(fileName))
        return false;
    sender = id;
    return true;
}

inline bool parseFileChunk(const Frame& f, int& sender, uint32_t& xferId,
                           const uint8_t*& data, size_t& n) {
    if (f.type != FT_FILE_CHUNK) return false;
    Reader  r(f.payload);
    uint8_t id = 0;
    if (!r.u8(id) || !r.u32(xferId)) return false;
    sender = id;
    data   = r.restPtr(n);
    return true;
}

inline bool parseFileEnd(const Frame& f, int& sender, uint32_t& xferId) {
    if (f.type != FT_FILE_END) return false;
    Reader  r(f.payload);
    uint8_t id = 0;
    if (!r.u8(id) || !r.u32(xferId)) return false;
    sender = id;
    return true;
}

inline bool parseFileAbort(const Frame& f, int& sender, uint32_t& xferId,
                           std::string& why) {
    if (f.type != FT_FILE_ABORT) return false;
    Reader  r(f.payload);
    uint8_t id = 0;
    if (!r.u8(id) || !r.u32(xferId) || !r.restString(why)) return false;
    sender = id;
    return true;
}

inline bool parseFileNotice(const Frame& f, int& sender, uint64_t& ts, uint64_t& size,
                            std::string& fileName) {
    if (f.type != FT_FILE_NOTICE) return false;
    Reader  r(f.payload);
    uint8_t id = 0;
    if (!r.u8(id) || !r.u64(ts) || !r.u64(size) || !r.str16(fileName)) return false;
    sender = id;
    return true;
}

}  // namespace proto
// ============================== 2. 通用小工具 ==============================

// UTF-8 字符串 -> std::filesystem::path（Windows 下走 UTF-16，中文文件名不乱码）
std::filesystem::path u8path(const std::string& utf8) {
    if (utf8.empty()) return {};
    if constexpr (std::is_same_v<std::filesystem::path::value_type, wchar_t>) {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()),
                                            nullptr, 0);
        if (n <= 0) return {};
        std::wstring w(size_t(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), w.data(), n);
        return std::filesystem::path(w);
    } else {
        return std::filesystem::path(utf8);
    }
}

// std::filesystem::path -> UTF-8 字符串
std::string pathToU8(const std::filesystem::path& p) {
    if constexpr (std::is_same_v<std::filesystem::path::value_type, wchar_t>) {
        const std::wstring w = p.wstring();
        if (w.empty()) return {};
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string s(size_t(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n,
                              nullptr, nullptr);
        return s;
    } else {
        return p.string();
    }
}

// ==================== 2.1 Windows 原生时间 API ====================
//  本工程完全不使用 <chrono> / clock_gettime / time()，只依赖 Win32 API，
//  因此不会出现「无法定位程序输入点 clock_gettime64」这类错误。
//  1601-01-01 到 1970-01-01 之间的秒数（FILETIME 与 Unix 时间戳的固定差值）
constexpr uint64_t kEpochDeltaSeconds = 11644473600ULL;

// 当前时间：Unix 时间戳（秒）
uint64_t nowSeconds() {
    FILETIME       ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000000ULL - kEpochDeltaSeconds;
}

// Unix 时间戳 -> 本地 SYSTEMTIME
SYSTEMTIME localTimeOf(uint64_t unixSec) {
    ULARGE_INTEGER u{};
    u.QuadPart = (unixSec + kEpochDeltaSeconds) * 10000000ULL;
    FILETIME   utc{};
    utc.dwLowDateTime  = u.LowPart;
    utc.dwHighDateTime = u.HighPart;
    FILETIME   local{};
    SYSTEMTIME st{};
    ::FileTimeToLocalFileTime(&utc, &local);   // UTC -> 本地时间
    ::FileTimeToSystemTime(&local, &st);       // 文件时间 -> 年月日时分秒
    return st;
}

// 时间戳 -> "HH:MM:SS"（聊天窗口里只需要时分秒）
std::string briefStampOf(uint64_t sec) {
    const SYSTEMTIME st = localTimeOf(sec);
    char             buf[16]{};
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ==================== 2.2 Windows 原生线程 / 互斥 ====================
//  不使用 std::thread / std::mutex：它们会依赖 MinGW 的 POSIX 线程库
//  libwinpthread-1.dll（正是 clock_gettime64 报错的根源），
//  这里直接使用 Win32 的 CreateThread / CRITICAL_SECTION。

// 临界区（互斥锁），对应标准库的 std::mutex
class CriticalSection {
public:
    CriticalSection() { ::InitializeCriticalSection(&cs_); }
    ~CriticalSection() { ::DeleteCriticalSection(&cs_); }
    CriticalSection(const CriticalSection&)            = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;
    CRITICAL_SECTION* native() { return &cs_; }

private:
    CRITICAL_SECTION cs_{};
};

// RAII 加锁守卫，对应 std::lock_guard
class CriticalGuard {
public:
    explicit CriticalGuard(CriticalSection& cs) : cs_(&cs) {
        ::EnterCriticalSection(cs_->native());
    }
    ~CriticalGuard() { ::LeaveCriticalSection(cs_->native()); }
    CriticalGuard(const CriticalGuard&)            = delete;
    CriticalGuard& operator=(const CriticalGuard&) = delete;

private:
    CriticalSection* cs_;
};

// 线程，对应 std::thread（基于 CreateThread）
class WinThread {
public:
    WinThread() = default;
    ~WinThread() { join(); }                // 析构时自动等待，避免线程句柄泄漏
    WinThread(const WinThread&)            = delete;
    WinThread& operator=(const WinThread&) = delete;

    template <typename Fn>
    bool start(Fn fn) {
        auto* task = new std::function<void()>(std::move(fn));
        handle_    = ::CreateThread(nullptr, 0, &WinThread::entry, task, 0, nullptr);
        if (!handle_) {                     // 线程创建失败
            delete task;
            return false;
        }
        return true;
    }

    void join() {
        if (!handle_) return;
        ::WaitForSingleObject(handle_, INFINITE);
        ::CloseHandle(handle_);
        handle_ = nullptr;
    }

    bool joinable() const { return handle_ != nullptr; }

private:
    static DWORD WINAPI entry(LPVOID param) {
        auto* task = static_cast<std::function<void()>*>(param);
        (*task)();
        delete task;
        return 0;
    }
    HANDLE handle_ = nullptr;
};

// 高精度计时器（QueryPerformanceCounter）：统计文件收发耗时与平均速度
class Stopwatch {
public:
    Stopwatch() { restart(); }

    void restart() {
        LARGE_INTEGER f{};
        freq_ = (::QueryPerformanceFrequency(&f) && f.QuadPart > 0) ? f.QuadPart : 1;
        LARGE_INTEGER c{};
        ::QueryPerformanceCounter(&c);
        start_ = c.QuadPart;
    }

    double seconds() const {
        LARGE_INTEGER c{};
        ::QueryPerformanceCounter(&c);
        return double(c.QuadPart - start_) / double(freq_);
    }

private:
    long long freq_  = 1;
    long long start_ = 0;
};

// 去掉末尾的 '\r' '\n'
std::string chomp(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

// 只保留文件名，替换 Windows 非法字符，避免 ../ 路径穿越
std::string sanitizeFileName(const std::string& raw) {
    const std::filesystem::path p    = u8path(raw);
    std::string                 name = pathToU8(p.filename());
    if (name.empty() || name == "." || name == "..") name = "unnamed";
    for (char& ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            ch = '_';
        }
    }
    return name;
}

// 在 dir 下找一个不冲突的路径：a.txt -> a_1.txt -> a_2.txt ...
std::filesystem::path uniquePath(const std::filesystem::path& dir,
                                 const std::string& name) {
    std::error_code       ec;
    std::filesystem::path p = dir / u8path(name);
    if (!std::filesystem::exists(p, ec)) return p;
    const std::string stem = pathToU8(p.stem());
    const std::string ext  = pathToU8(p.extension());
    for (int i = 1; i < 100000; ++i) {
        const std::filesystem::path candidate =
            dir / u8path(stem + "_" + std::to_string(i) + ext);
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
    return p;
}

// 人类可读的大小
std::string humanSize(uint64_t bytes) {
    static const char* unit[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double             v      = double(bytes);
    int                i      = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        ++i;
    }
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), (i == 0 ? "%.0f %s" : "%.2f %s"), v, unit[i]);
    return buf;
}

// 统一的输出：主线程（读键盘）和接收线程（读网络）都会打印，必须加锁
CriticalSection g_outMutex;
void printLine(const std::string& s) {
    CriticalGuard lock(g_outMutex);
    std::cout << s << std::endl;
}
void printPrompt() {
    CriticalGuard lock(g_outMutex);
    std::cout << "> " << std::flush;
}

// ============================== 3. Socket 收发基础封装 ==============================

// 循环发送，直到发完（解决 TCP 半包问题）
bool sendAll(SOCKET s, const void* data, size_t len) {
    const auto* p         = static_cast<const uint8_t*>(data);
    size_t      sentTotal = 0;
    while (sentTotal < len) {
        const size_t remain = len - sentTotal;
        const int    want   = int(std::min<size_t>(remain, 1u << 20));
        const int    n = ::send(s, reinterpret_cast<const char*>(p + sentTotal), want, 0);
        if (n <= 0) return false;
        sentTotal += size_t(n);
    }
    return true;
}

// 精确读取 len 个字节
bool recvExact(SOCKET s, void* data, size_t len) {
    auto*  p   = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < len) {
        const size_t remain = len - got;
        const int    want   = int(std::min<size_t>(remain, 1u << 20));
        const int    n      = ::recv(s, reinterpret_cast<char*>(p + got), want, 0);
        if (n == 0) return false;
        if (n == SOCKET_ERROR) return false;
        got += size_t(n);
    }
    return true;
}

// 发送一帧：[1 字节类型][4 字节负载长度][负载]
bool sendFrame(SOCKET s, const proto::Frame& f) {
    if (f.payload.size() > proto::kMaxFrameSize) return false;
    uint8_t hdr[5];
    hdr[0] = f.type;
    hdr[1] = uint8_t((f.payload.size() >> 24) & 0xFF);
    hdr[2] = uint8_t((f.payload.size() >> 16) & 0xFF);
    hdr[3] = uint8_t((f.payload.size() >> 8) & 0xFF);
    hdr[4] = uint8_t(f.payload.size() & 0xFF);
    if (!sendAll(s, hdr, sizeof(hdr))) return false;
    if (!f.payload.empty() && !sendAll(s, f.payload.data(), f.payload.size())) return false;
    return true;
}

// 接收一帧
bool recvFrame(SOCKET s, proto::Frame& f) {
    uint8_t hdr[5];
    if (!recvExact(s, hdr, sizeof(hdr))) return false;
    const uint32_t len = (uint32_t(hdr[1]) << 24) | (uint32_t(hdr[2]) << 16) |
                         (uint32_t(hdr[3]) << 8) | uint32_t(hdr[4]);
    if (len > proto::kMaxFrameSize) return false;
    f.type = hdr[0];
    f.payload.assign(len, 0);
    if (len != 0 && !recvExact(s, f.payload.data(), len)) return false;
    return true;
}
// ============================== 4. 客户端主体 ==============================

// 正在接收中的一个文件
struct IncomingFile {
    std::ofstream out;                 // 写文件流
    uint64_t      expected = 0;        // 对方声明的总大小
    uint64_t      got      = 0;        // 已收到字节数
    std::string   name;                // 文件名（已消毒）
    std::string   diskPath;            // 本地保存路径
    Stopwatch     watch;               // 本次接收耗时统计（QueryPerformanceCounter）
};

// 用 (发送者编号, 传输ID) 作为一次文件传输的唯一标识。
// 这样即使「对方在给我发文件」的同时「服务端在给我下发历史文件」，也不会混淆。
using FileKey   = std::pair<int, uint32_t>;
using FileTable = std::map<FileKey, IncomingFile>;

class ChatClient {
public:
    ChatClient(std::string host, uint16_t port, std::string nickname,
               std::filesystem::path downloadDir)
        : host_(std::move(host)),
          port_(port),
          nickname_(std::move(nickname)),
          downloadDir_(std::move(downloadDir)) {}

    ~ChatClient() { shutdownClient(); }

    bool connectToServer();     // 连接 + 握手
    void run();                 // 命令行交互主循环
    void shutdownClient();      // 断开并回收接收线程

private:
    bool sendFrameSafe(const proto::Frame& f);                 // 加锁发送
    void readerLoop();                                         // 接收线程
    void dispatch(const proto::Frame& f, FileTable& files);    // 处理一个下行帧
    bool sendFile(const std::string& path);                    // 发送本地文件
    void printHelp() const;
    static std::string senderLabel(int id);                    // "服务器" / "用户N"

    std::string            host_;
    uint16_t               port_;
    std::string            nickname_;
    std::filesystem::path  downloadDir_;
    std::atomic<SOCKET>    sock_{INVALID_SOCKET};
    std::atomic<bool>      running_{false};
    WinThread              reader_;
    CriticalSection        sendMutex_;               // 串行化对同一个 socket 的写
    std::atomic<int>       myId_{0};
    std::atomic<uint32_t>  nextXfer_{1};             // 本地文件传输 ID 计数器
};

std::string ChatClient::senderLabel(int id) {
    if (id == proto::kServerId) return "服务器";
    return "用户" + std::to_string(id);
}

// ------------------------------ 4.1 连接与握手 ------------------------------
bool ChatClient::connectToServer() {
    // (1) 初始化 Winsock
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printLine("WSAStartup 失败：无法初始化 Windows Socket");
        return false;
    }

    // (2) 解析地址（支持 IP 和主机名）
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo*         res     = nullptr;
    const std::string portStr = std::to_string(port_);
    if (::getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        printLine("无法解析服务器地址：" + host_);
        return false;
    }

    // (3) 逐个尝试连接
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, p->ai_addr, int(p->ai_addrlen)) == 0) break;
        ::closesocket(s);
        s = INVALID_SOCKET;
    }
    ::freeaddrinfo(res);

    if (s == INVALID_SOCKET) {
        printLine("连接服务器失败：" + host_ + ":" + portStr + "（服务端是否已启动？）");
        return false;
    }
    sock_ = s;
    printLine("已建立 TCP 连接：" + host_ + ":" + portStr);

    // (4) 发送 HELLO，上报昵称
    const bool helloSent = sendFrameSafe(proto::makeHello(nickname_));

    // (5) 不论 HELLO 是否发送成功，都先把服务端的回应读完：
    //     服务端拒绝连接（聊天室已满）时会先发提示再关闭，
    //     此时 send 可能失败，但它发来的提示仍然能读到。
    FileTable    files;                    // 握手期间一般没有文件，占位用
    proto::Frame f;
    while (recvFrame(sock_.load(), f)) {
        dispatch(f, files);
        if (f.type == proto::FT_HELLO_ACK) {
            running_ = true;
            reader_.start([this] { readerLoop(); });
            return true;
        }
    }
    if (!helloSent) printLine("握手数据发送失败（服务端可能已经关闭了连接）。");
    printLine("服务端拒绝或中断了本次连接（例如聊天室已满）。");
    shutdownClient();
    return false;
}

// ------------------------------ 4.2 接收线程 ------------------------------
void ChatClient::readerLoop() {
    const SOCKET sock = sock_.load();
    FileTable    files;                      // 本线程私有的“正在接收的文件”表

    while (running_) {
        proto::Frame f;
        if (!recvFrame(sock, f)) break;      // 服务端断开或网络出错
        dispatch(f, files);
    }

    // 关闭所有没接收完的文件
    for (auto& entry : files) {
        IncomingFile& in = entry.second;
        if (in.out.is_open()) in.out.close();
        printLine("文件接收未完成：" + in.name);
    }
    files.clear();

    if (running_.exchange(false)) {
        printLine("与服务器的连接已断开，请按 Ctrl+C 退出。");
    }
}
// ------------------------------ 4.3 处理下行帧 ------------------------------
void ChatClient::dispatch(const proto::Frame& f, FileTable& files) {
    switch (f.type) {
    // ---------- 握手回执 ----------
    case proto::FT_HELLO_ACK: {
        int         id = 0;
        std::string name;
        if (!proto::parseHelloAck(f, id, name)) return;
        myId_ = id;
        printLine("连接成功：我是 " + std::to_string(id) + " 号用户，昵称 " + name);
        break;
    }

    // ---------- 系统提示 / 历史记录分隔线 ----------
    case proto::FT_SYSTEM: {
        uint64_t    ts = 0;
        std::string text;
        if (!proto::parseSystem(f, ts, text)) return;
        printLine("[" + briefStampOf(ts) + "] 系统: " + text);
        break;
    }

    // ---------- 文字消息 ----------
    case proto::FT_TEXT: {
        int         sender = 0;
        uint64_t    ts     = 0;
        std::string who, text;
        if (!proto::parseText(f, sender, ts, who, text)) return;
        if (who.empty()) who = senderLabel(sender);
        printLine("[" + briefStampOf(ts) + "] " + who + ": " + text);
        break;
    }

    // ---------- 有人开始给我发文件：创建本地文件 ----------
    case proto::FT_FILE_BEGIN: {
        int         sender = 0;
        uint64_t    ts = 0, size = 0;
        uint32_t    xferId = 0;
        std::string fname;
        if (!proto::parseFileBegin(f, sender, ts, xferId, size, fname)) return;

        fname = sanitizeFileName(fname);
        IncomingFile in;
        in.expected = size;
        in.name     = fname;
        in.diskPath = pathToU8(uniquePath(downloadDir_, fname));
        in.out.open(u8path(in.diskPath), std::ios::binary | std::ios::trunc);
        if (!in.out.is_open()) {
            printLine("无法创建本地文件（将忽略该文件）：" + in.diskPath);
            return;                                  // 后续分片因为找不到 key 会被丢弃
        }
        printLine("[" + briefStampOf(ts) + "] " + senderLabel(sender) + " 正在发送文件：" +
                  fname + "（" + humanSize(size) + "）");
        in.watch.restart();                        // 开始统计接收耗时
        files[FileKey{sender, xferId}] = std::move(in);
        break;
    }

    // ---------- 文件分片：直接写入本地文件 ----------
    case proto::FT_FILE_CHUNK: {
        int            sender = 0;
        uint32_t       xferId = 0;
        const uint8_t* data   = nullptr;
        size_t         n      = 0;
        if (!proto::parseFileChunk(f, sender, xferId, data, n)) return;
        const auto it = files.find(FileKey{sender, xferId});
        if (it == files.end()) return;               // 这个传输被忽略了
        IncomingFile& in = it->second;
        if (n > 0) in.out.write(reinterpret_cast<const char*>(data), std::streamsize(n));

        const uint64_t before = in.got;
        in.got += n;
        // 每 4 MiB 打印一次进度
        if (before / (4u * 1024u * 1024u) != in.got / (4u * 1024u * 1024u)) {
            printLine("接收 " + in.name + " 进度：" + humanSize(in.got) + " / " +
                      humanSize(in.expected));
        }
        break;
    }

    // ---------- 文件结束 ----------
    case proto::FT_FILE_END: {
        int      sender = 0;
        uint32_t xferId = 0;
        if (!proto::parseFileEnd(f, sender, xferId)) return;
        const auto it = files.find(FileKey{sender, xferId});
        if (it == files.end()) return;
        IncomingFile& in = it->second;
        if (in.out.is_open()) in.out.close();
        const double used = in.watch.seconds();               // 高精度耗时（秒）
        const double mbps =
            used > 0.0001 ? double(in.got) / 1024.0 / 1024.0 / used : 0.0;
        char speed[64]{};
        std::snprintf(speed, sizeof(speed), "，耗时 %.2f 秒，平均 %.2f MB/s", used, mbps);
        printLine("文件接收完成：" + in.diskPath + "（" + humanSize(in.got) + "）" + speed);
        files.erase(it);
        break;
    }

    // ---------- 文件中断 ----------
    case proto::FT_FILE_ABORT: {
        int         sender = 0;
        uint32_t    xferId = 0;
        std::string why;
        if (!proto::parseFileAbort(f, sender, xferId, why)) return;
        const auto it = files.find(FileKey{sender, xferId});
        if (it == files.end()) return;
        IncomingFile& in = it->second;
        if (in.out.is_open()) in.out.close();
        std::error_code ec;
        std::filesystem::remove(u8path(in.diskPath), ec);   // 删除残缺文件
        printLine("文件传输被中断（" + why + "）：" + in.name);
        files.erase(it);
        break;
    }

    // ---------- 文件通知（历史记录里也会出现） ----------
    case proto::FT_FILE_NOTICE: {
        int         sender = 0;
        uint64_t    ts = 0, size = 0;
        std::string fname;
        if (!proto::parseFileNotice(f, sender, ts, size, fname)) return;
        printLine("[" + briefStampOf(ts) + "] " + senderLabel(sender) + " 发送了文件：" +
                  fname + "（" + humanSize(size) + "）  可用 /get " + fname +
                  " 从服务端重新下载");
        break;
    }

    default:
        break;
    }
}

// ------------------------------ 4.4 发送一帧 ------------------------------
bool ChatClient::sendFrameSafe(const proto::Frame& f) {
    const SOCKET s = sock_.load();
    if (s == INVALID_SOCKET) return false;
    CriticalGuard lock(sendMutex_);
    return sendFrame(s, f);
}
// ------------------------------ 4.5 发送文件（任意大小） ------------------------------
bool ChatClient::sendFile(const std::string& rawPath) {
    const std::filesystem::path path = u8path(rawPath);
    std::error_code             ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        printLine("找不到文件：" + rawPath);
        return false;
    }
    const uint64_t size = uint64_t(std::filesystem::file_size(path, ec));

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        printLine("无法打开文件：" + rawPath);
        return false;
    }

    const uint32_t    xferId = nextXfer_.fetch_add(1);
    const std::string fname  = pathToU8(path.filename());

    printLine("开始发送文件：" + fname + "（" + humanSize(size) + "）");
    Stopwatch watch;                                  // QueryPerformanceCounter 计时
    if (!sendFrameSafe(proto::makeFileBegin(xferId, size, fname))) {
        printLine("发送文件失败（连接已断开）");
        return false;
    }

    std::vector<uint8_t> buf(proto::kChunkSize);
    uint64_t             sent       = 0;
    uint64_t             lastReport = 0;
    bool                 ok         = true;

    while (in && running_) {
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        if (!sendFrameSafe(proto::makeFileChunk(xferId, buf.data(), size_t(n)))) {
            ok = false;
            break;
        }
        sent += uint64_t(n);
        if (sent - lastReport >= 4u * 1024u * 1024u) {   // 每 4 MiB 报告一次进度
            lastReport = sent;
            printLine("发送 " + fname + " 进度：" + humanSize(sent) + " / " +
                      humanSize(size));
        }
    }

    if (!ok || in.bad()) {
        printLine("发送文件出错，已通知服务端中断传输：" + fname);
        sendFrameSafe(proto::makeFileAbort(xferId, "读取本地文件失败"));
        return false;
    }

    sendFrameSafe(proto::makeFileEnd(xferId));
    const double used = watch.seconds();              // 高精度耗时（秒）
    const double mbps = used > 0.0001 ? double(sent) / 1024.0 / 1024.0 / used : 0.0;
    char speed[64]{};
    std::snprintf(speed, sizeof(speed), "，耗时 %.2f 秒，平均 %.2f MB/s", used, mbps);
    printLine("文件发送完成：" + fname + "（" + humanSize(sent) + "）" + speed);
    return true;
}

// ------------------------------ 4.6 帮助 ------------------------------
void ChatClient::printHelp() const {
    printLine("---------------- 使用说明 ----------------");
    printLine("  直接输入文字并回车   -> 发送聊天消息");
    printLine("  /file <文件路径>     -> 给对方发文件（支持任意大小）");
    printLine("  /get  <文件名>       -> 从服务端下载已保存的文件");
    printLine("  /help                -> 显示本帮助");
    printLine("  /sleep <毫秒>        -> 等待若干毫秒（写测试脚本时有用）");
    printLine("  /quit                -> 退出聊天室");
    printLine("------------------------------------------");
}

// ------------------------------ 4.7 主交互循环 ------------------------------
void ChatClient::run() {
    printHelp();
    printLine("输入内容后回车即可发送，输入 /help 查看命令。");
    printPrompt();

    std::string line;
    while (running_ && std::getline(std::cin, line)) {
        line = chomp(std::move(line));

        if (!line.empty()) {
            if (line[0] != '/') {
                // ---- 普通文字消息 ----
                if (sendFrameSafe(proto::makeText(line))) {
                    printLine("[" + briefStampOf(nowSeconds()) + "] 我: " + line);
                } else {
                    printLine("发送失败：与服务器的连接已断开");
                }
            } else {
                // ---- 命令处理 ----
                const size_t      sp  = line.find(' ');
                const std::string cmd = (sp == std::string::npos) ? line : line.substr(0, sp);
                std::string       arg = (sp == std::string::npos)
                                            ? std::string()
                                            : chomp(line.substr(sp + 1));
                // 允许 /file "C:\path with space\a.bin" 这种带引号的写法
                if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"')
                    arg = arg.substr(1, arg.size() - 2);

                if (cmd == "/quit" || cmd == "/exit") {
                    printLine("正在退出…");
                    break;
                } else if (cmd == "/file") {
                    if (arg.empty()) printLine("用法：/file <文件路径>");
                    else sendFile(arg);
                } else if (cmd == "/get") {
                    if (arg.empty()) {
                        printLine("用法：/get <文件名>");
                    } else if (sendFrameSafe(proto::makeReqFile(arg))) {
                        printLine("已向服务端请求文件：" + arg);
                    } else {
                        printLine("请求失败：与服务器的连接已断开");
                    }
                } else if (cmd == "/help") {
                    printHelp();
                } else if (cmd == "/sleep") {
                    const long ms = arg.empty() ? 1000 : std::atol(arg.c_str());
                    if (ms > 0) ::Sleep(DWORD(ms > 60000 ? 60000 : ms));   // Win32 原生睡眠
                } else {
                    printLine("未知命令：" + cmd + "（输入 /help 查看帮助）");
                }
            }
        }
        printPrompt();
    }

    shutdownClient();
}

// ------------------------------ 4.8 断开 ------------------------------
void ChatClient::shutdownClient() {
    if (running_.exchange(false)) {
        sendFrameSafe(proto::makeBye());        // 礼貌地通知服务端（失败也无所谓）
    }
    const SOCKET s = sock_.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        ::shutdown(s, SD_BOTH);                 // 先 shutdown，让阻塞的 recv 立即返回
        ::closesocket(s);
    }
    if (reader_.joinable()) reader_.join();
    ::WSACleanup();
}

// ============================== 5. 程序入口 ==============================
int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);              // 控制台按 UTF-8 显示中文
    ::SetConsoleCP(CP_UTF8);

    // 命令行参数：chat_client.exe [服务器IP] [端口] [昵称] [下载目录]
    std::string           host = "127.0.0.1";
    uint16_t              port = proto::kDefaultPort;
    std::string           nick = "用户" + std::to_string(::GetCurrentProcessId());
    std::filesystem::path downloadDir = u8path("downloads");

    if (argc >= 2) host = argv[1];
    if (argc >= 3) {
        const int p = std::atoi(argv[2]);
        if (p > 0 && p <= 65535) port = uint16_t(p);
    }
    if (argc >= 4) nick = argv[3];
    if (argc >= 5) downloadDir = u8path(argv[4]);

    std::error_code ec;
    std::filesystem::create_directories(downloadDir, ec);   // 创建下载目录

    std::cout << "===============================================\n"
              << "  双人聊天室 客户端  (C++23 / MinGW / TCP)\n"
              << "  用法: chat_client.exe [服务器IP] [端口] [昵称] [下载目录]\n"
              << "  服务器: " << host << ":" << port << "\n"
              << "  昵称: " << nick << "\n"
              << "  文件下载目录: " << pathToU8(downloadDir) << "\n"
              << "===============================================" << std::endl;

    ChatClient client(host, port, nick, downloadDir);
    if (!client.connectToServer()) {
        client.shutdownClient();
        std::cout << "按回车键退出…" << std::endl;
        std::cin.get();
        return 1;
    }
    client.run();
    return 0;
}
