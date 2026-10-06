/*==============================================================================
 *  文件: chat_server/main.cpp
 *  项目: C/S 架构双人聊天室 —— 服务端（Server）
 *  语言: C++23（使用 -std=c++23，禁止使用 C++26 特性）
 *  平台: Windows + MinGW-w64 (g++ 13 及以上) + CMake
 *------------------------------------------------------------------------------
 *  功能清单
 *    1. TCP Socket 监听，最多只允许 2 个客户端同时在线，第 3 个连接被拒绝。
 *    2. 转发两个客户端之间的文字消息。
 *    3. 转发任意大小的文件（分片流式转发：不把整个文件读进内存；服务端同时落盘）。
 *    4. 聊天记录持久化（默认二进制日志 history.bin；
 *       定义编译宏 CHAT_USE_SQLITE 后改用 SQLite 数据库 chat.db）。
 *    5. 进程启动时加载历史记录，客户端握手成功后自动下发全部历史。
 *    6. 客户端可用 /get <文件名> 从服务端重新下载历史文件（任意大小）。
 *------------------------------------------------------------------------------
 *  编译/运行
 *    cmake -S . -B build -G "MinGW Makefiles"
 *    cmake --build build -j
 *    ./build/chat_server.exe [端口] [数据目录]
 *============================================================================*/

/*------------------------------------------------------------------------------
 *  一、应用层协议（chat_server 与 chat_client 各有一份完全相同的定义，
 *      修改协议时必须同时修改两个工程，否则无法互通）
 *------------------------------------------------------------------------------
 *  传输方式: TCP 字节流，所有整数都是「大端序(Big-Endian / 网络字节序)」。
 *
 *  帧结构:  [1 字节 帧类型][4 字节 负载长度][负载 ...]
 *           发送方保证负载长度 <= kMaxFrameSize，接收方同样做检查，
 *           防止畸形数据导致内存爆炸。
 *
 *  ── 客户端 -> 服务端（上行，不带发送者信息，由服务端补齐） ──
 *    FT_HELLO      : [u16 名字长度][名字(UTF-8)]
 *    FT_TEXT       : [正文(UTF-8)]
 *    FT_FILE_BEGIN : [u32 传输ID][u64 文件总大小][u16 文件名长度][文件名(UTF-8)]
 *    FT_FILE_CHUNK : [u32 传输ID][原始字节...]
 *    FT_FILE_END   : [u32 传输ID]
 *    FT_FILE_ABORT : [u32 传输ID][原因(UTF-8)]
 *    FT_REQ_FILE   : [u16 文件名长度][文件名(UTF-8)]   // 请求下载服务端文件
 *    FT_BYE        : [空]
 *
 *  ── 服务端 -> 客户端（下行：每条消息都带「谁发的」「什么时间发的」，
 *                       这样历史记录回放时也能正确显示） ──
 *    FT_HELLO_ACK  : [u8 你的编号][u16 名字长度][名字(UTF-8)]
 *    FT_SYSTEM     : [u64 时间戳秒][正文(UTF-8)]
 *    FT_TEXT       : [u8 发送者编号][u64 时间戳秒][u16 名字长度][名字][正文]
 *    FT_FILE_BEGIN : [u8 发送者编号][u64 时间戳秒][u32 传输ID][u64 总大小][u16 文件名长度][文件名]
 *    FT_FILE_CHUNK : [u8 发送者编号][u32 传输ID][原始字节...]
 *    FT_FILE_END   : [u8 发送者编号][u32 传输ID]
 *    FT_FILE_ABORT : [u8 发送者编号][u32 传输ID][原因(UTF-8)]
 *    FT_FILE_NOTICE: [u8 发送者编号][u64 时间戳秒][u64 大小][u16 文件名长度][文件名]
 *                    // 文件收发完成后写进聊天记录的一条“通知”，会被历史回放
 *------------------------------------------------------------------------------
 *  为什么需要「帧」？TCP 是字节流，没有消息边界，必须由应用层自己约定边界。
 *  这里用「长度前缀」切分，才能正确区分“一条文字消息”和“一个文件分片”。
 *---------------------------------------------------------------------------*/

// ============================== 0. 头文件 ==============================
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN      // 只引入必要的 Windows 头，缩短编译时间
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX                 // 关闭 windows.h 中的 min/max 宏
#  endif
#  include <winsock2.h>              // Winsock2 必须出现在 windows.h 之前
#  include <ws2tcpip.h>
#  include <windows.h>               // SetConsoleOutputCP / MultiByteToWideChar
#else
#  error "本工程面向 Windows + MinGW 平台，请在 Windows 下编译。"
#endif

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifdef CHAT_USE_SQLITE
#  include <sqlite3.h>             // 可选扩展：SQLite 持久化
#endif

// ============================== 1. 协议定义 ==============================
namespace proto {

// ---- 常量 ----
constexpr uint16_t kDefaultPort  = 9080;                 // 默认监听端口
constexpr uint32_t kChunkSize    = 64u * 1024u;          // 文件分片大小：64 KiB
constexpr uint32_t kMaxFrameSize = 8u * 1024u * 1024u;   // 单个帧负载上限（防御性检查）
constexpr int      kServerId     = 0;                    // 编号 0 代表“服务端”自己
constexpr int      kMaxClients   = 2;                    // 需求：只允许 2 个客户端

// ---- 帧类型枚举 ----
enum FrameType : uint8_t {
    FT_HELLO       = 1,   // 客户端打招呼（携带昵称）
    FT_HELLO_ACK   = 2,   // 服务端分配编号并回执
    FT_TEXT        = 3,   // 文字消息
    FT_SYSTEM      = 4,   // 服务端系统提示
    FT_FILE_BEGIN  = 5,   // 文件开始
    FT_FILE_CHUNK  = 6,   // 文件分片
    FT_FILE_END    = 7,   // 文件结束
    FT_FILE_ABORT  = 8,   // 文件中断
    FT_FILE_NOTICE = 9,   // 文件通知（写入聊天记录）
    FT_REQ_FILE    = 10,  // 请求下载服务端保存的文件
    FT_BYE         = 11,  // 主动断开
};

// 一个完整的应用层帧
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
// 写入「2 字节长度 + 内容」的 UTF-8 字符串（长度超过 65535 字节会截断）
inline void putStr16(std::vector<uint8_t>& b, std::string_view s) {
    const size_t n = std::min<size_t>(s.size(), 0xFFFFu);
    putU16(b, uint16_t(n));
    b.insert(b.end(), s.begin(), s.begin() + std::ptrdiff_t(n));
}
// 写入一段原始字节
inline void putRaw(std::vector<uint8_t>& b, const void* p, size_t n) {
    const auto* c = static_cast<const uint8_t*>(p);
    b.insert(b.end(), c, c + n);
}

// ------------------------- 1.2 读缓冲区的小工具 -------------------------
// 带边界检查的读取器：任何越界读取都返回 false，避免因为非法数据崩溃。
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
    // 取走剩余的全部字节
    bool restRaw(std::vector<uint8_t>& out) {
        out.assign(d_.begin() + std::ptrdiff_t(pos_), d_.end());
        pos_ = d_.size();
        return true;
    }
    bool restString(std::string& s) {
        s.assign(reinterpret_cast<const char*>(d_.data() + pos_), d_.size() - pos_);
        pos_ = d_.size();
        return true;
    }
    // 只读地访问剩余字节（不拷贝），用于转发文件分片
    const uint8_t* restPtr(size_t& n) const {
        n = d_.size() - pos_;
        return d_.data() + pos_;
    }
    size_t remaining() const { return d_.size() - pos_; }

private:
    const std::vector<uint8_t>& d_;
    size_t                      pos_ = 0;
};

// ------------------------- 1.3 服务端使用的组帧函数 -------------------------
inline Frame makeHelloAck(int yourId, const std::string& name) {
    Frame f;
    f.type = FT_HELLO_ACK;
    putU8(f.payload, uint8_t(yourId));
    putStr16(f.payload, name);
    return f;
}

inline Frame makeSystem(uint64_t ts, const std::string& text) {
    Frame f;
    f.type = FT_SYSTEM;
    putU64(f.payload, ts);
    putRaw(f.payload, text.data(), text.size());
    return f;
}

inline Frame makeText(int senderId, uint64_t ts, const std::string& senderName,
                      const std::string& text) {
    Frame f;
    f.type = FT_TEXT;
    putU8(f.payload, uint8_t(senderId));
    putU64(f.payload, ts);
    putStr16(f.payload, senderName);
    putRaw(f.payload, text.data(), text.size());
    return f;
}

inline Frame makeFileBegin(int senderId, uint64_t ts, uint32_t xferId, uint64_t size,
                           const std::string& fileName) {
    Frame f;
    f.type = FT_FILE_BEGIN;
    putU8(f.payload, uint8_t(senderId));
    putU64(f.payload, ts);
    putU32(f.payload, xferId);
    putU64(f.payload, size);
    putStr16(f.payload, fileName);
    return f;
}

inline Frame makeFileChunk(int senderId, uint32_t xferId, const uint8_t* data, size_t n) {
    Frame f;
    f.type = FT_FILE_CHUNK;
    putU8(f.payload, uint8_t(senderId));
    putU32(f.payload, xferId);
    putRaw(f.payload, data, n);
    return f;
}

inline Frame makeFileEnd(int senderId, uint32_t xferId) {
    Frame f;
    f.type = FT_FILE_END;
    putU8(f.payload, uint8_t(senderId));
    putU32(f.payload, xferId);
    return f;
}

inline Frame makeFileAbort(int senderId, uint32_t xferId, const std::string& why) {
    Frame f;
    f.type = FT_FILE_ABORT;
    putU8(f.payload, uint8_t(senderId));
    putU32(f.payload, xferId);
    putRaw(f.payload, why.data(), why.size());
    return f;
}

inline Frame makeFileNotice(int senderId, uint64_t ts, uint64_t size,
                            const std::string& fileName) {
    Frame f;
    f.type = FT_FILE_NOTICE;
    putU8(f.payload, uint8_t(senderId));
    putU64(f.payload, ts);
    putU64(f.payload, size);
    putStr16(f.payload, fileName);
    return f;
}

// ------------------------- 1.4 服务端使用的解帧函数（上行帧） -------------------------
inline bool parseHello(const Frame& f, std::string& name) {
    if (f.type != FT_HELLO) return false;
    Reader r(f.payload);
    return r.str16(name);
}

inline bool parseFileBegin(const Frame& f, uint32_t& xferId, uint64_t& size,
                           std::string& name) {
    if (f.type != FT_FILE_BEGIN) return false;
    Reader r(f.payload);
    return r.u32(xferId) && r.u64(size) && r.str16(name);
}

inline bool parseFileChunk(const Frame& f, uint32_t& xferId, const uint8_t*& data,
                           size_t& n) {
    if (f.type != FT_FILE_CHUNK) return false;
    Reader r(f.payload);
    if (!r.u32(xferId)) return false;
    data = r.restPtr(n);
    return true;
}

inline bool parseFileEnd(const Frame& f, uint32_t& xferId) {
    if (f.type != FT_FILE_END) return false;
    Reader r(f.payload);
    return r.u32(xferId);
}

inline bool parseReqFile(const Frame& f, std::string& name) {
    if (f.type != FT_REQ_FILE) return false;
    Reader r(f.payload);
    return r.str16(name);
}

}  // namespace proto
// ============================== 2. 通用小工具 ==============================

// UTF-8 字符串 -> std::filesystem::path
// 说明：libstdc++ 在 Windows 上把 path 的原生字符类型定义为 wchar_t，
//       如果直接用 std::string 构造 path，会走 "C" locale 的窄字符转换，
//       导致中文文件名转换失败。这里显式做 UTF-8 -> UTF-16 转换即可。
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

// std::filesystem::path -> UTF-8 字符串（用于打印和协议传输）
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

// 当前时间：Unix 时间戳（秒）。
// GetSystemTimeAsFileTime 返回自 1601-01-01(UTC) 起的 100 纳秒计数，
// 除以 10^7 得到秒数，再减去固定偏移即可换算成 Unix 时间戳。
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

// 时间戳 -> "YYYY-MM-DD HH:MM:SS"
std::string stampOf(uint64_t sec) {
    const SYSTEMTIME st = localTimeOf(sec);
    char             buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 只取 "HH:MM:SS" 部分（日志里更简洁）
std::string briefStampOf(uint64_t sec) {
    const SYSTEMTIME st = localTimeOf(sec);
    char             buf[16]{};
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 去掉字符串结尾的 '\r' '\n'（Windows 下 std::getline 会留下 '\r'）
std::string chomp(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

// 只保留文件名部分，并替换 Windows 下的非法字符，同时防止 ../ 之类的路径穿越
std::string sanitizeFileName(const std::string& raw) {
    const std::filesystem::path p = u8path(raw);
    std::string                 name = pathToU8(p.filename());
    if (name.empty() || name == "." || name == "..") name = "unnamed";
    for (char& ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            ch = '_';
        }
    }
    if (name.size() > 120) name = name.substr(name.size() - 120);   // 防止超长文件名
    return name;
}

// 在目录 dir 下为 name 找一个不冲突的路径：a.txt -> a_1.txt -> a_2.txt ...
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
    return p;   // 极端情况：直接覆盖
}

// 人类可读的文件大小，例如 1.50 MB
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

// 高精度计时器（QueryPerformanceCounter）：统计文件传输耗时与平均速度
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

// 线程安全的日志输出（用临界区代替 CriticalSection）
CriticalSection g_logMutex;
void logLine(const std::string& s) {
    CriticalGuard guard(g_logMutex);
    std::cout << "[" << briefStampOf(nowSeconds()) << "] " << s << std::endl;
}

// ============================== 3. Socket 收发基础封装 ==============================

// 把 len 个字节全部写完（TCP 存在“半包”问题，必须循环发送）
bool sendAll(SOCKET s, const void* data, size_t len) {
    const auto* p         = static_cast<const uint8_t*>(data);
    size_t      sentTotal = 0;
    while (sentTotal < len) {
        const size_t remain = len - sentTotal;
        const int    want   = int(std::min<size_t>(remain, 1u << 20));   // 单次最多 1 MiB
        const int    n = ::send(s, reinterpret_cast<const char*>(p + sentTotal), want, 0);
        if (n <= 0) return false;                                        // 出错或对端关闭
        sentTotal += size_t(n);
    }
    return true;
}

// 精确读取 len 个字节（同样要循环，直到读满或对端断开）
bool recvExact(SOCKET s, void* data, size_t len) {
    auto*  p   = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < len) {
        const size_t remain = len - got;
        const int    want   = int(std::min<size_t>(remain, 1u << 20));
        const int    n      = ::recv(s, reinterpret_cast<char*>(p + got), want, 0);
        if (n == 0) return false;      // 对端正常关闭
        if (n == SOCKET_ERROR) return false;
        got += size_t(n);
    }
    return true;
}

// 发送一个完整的帧：帧头 5 字节 + 负载
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

// 接收一个完整的帧
bool recvFrame(SOCKET s, proto::Frame& f) {
    uint8_t hdr[5];
    if (!recvExact(s, hdr, sizeof(hdr))) return false;
    const uint32_t len = (uint32_t(hdr[1]) << 24) | (uint32_t(hdr[2]) << 16) |
                         (uint32_t(hdr[3]) << 8) | uint32_t(hdr[4]);
    if (len > proto::kMaxFrameSize) {      // 协议非法，直接断开
        logLine("收到非法帧（长度超限），断开该连接");
        return false;
    }
    f.type = hdr[0];
    f.payload.assign(len, 0);
    if (len != 0 && !recvExact(s, f.payload.data(), len)) return false;
    return true;
}
// ============================== 4. 聊天记录持久化 ==============================
//
//  抽象接口：把「保存一条记录」和「读出全部记录」隔离开，
//  这样默认的文件实现和可选的 SQLite 实现可以互换。
//
class IHistoryStore {
public:
    virtual ~IHistoryStore() = default;
    virtual bool                      open() = 0;                      // 打开/创建存储
    virtual void                      append(const proto::Frame& f) = 0; // 追加一条记录
    virtual std::vector<proto::Frame> loadAll() = 0;                   // 读出全部记录
    virtual const char*               backendName() const = 0;         // 后端名字（打印用）
};

// 把帧写进 std::ostream（与网络帧格式完全一致，方便复用）
bool writeFrameToStream(std::ostream& os, const proto::Frame& f) {
    uint8_t hdr[5];
    hdr[0] = f.type;
    hdr[1] = uint8_t((f.payload.size() >> 24) & 0xFF);
    hdr[2] = uint8_t((f.payload.size() >> 16) & 0xFF);
    hdr[3] = uint8_t((f.payload.size() >> 8) & 0xFF);
    hdr[4] = uint8_t(f.payload.size() & 0xFF);
    os.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    if (!f.payload.empty())
        os.write(reinterpret_cast<const char*>(f.payload.data()),
                 std::streamsize(f.payload.size()));
    return os.good();
}

// 从 std::istream 读一个帧；文件读完返回 false
bool readFrameFromStream(std::istream& is, proto::Frame& f) {
    uint8_t hdr[5];
    is.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (is.gcount() != std::streamsize(sizeof(hdr))) return false;
    const uint32_t len = (uint32_t(hdr[1]) << 24) | (uint32_t(hdr[2]) << 16) |
                         (uint32_t(hdr[3]) << 8) | uint32_t(hdr[4]);
    if (len > proto::kMaxFrameSize) return false;
    f.type = hdr[0];
    f.payload.assign(len, 0);
    if (len != 0) {
        is.read(reinterpret_cast<char*>(f.payload.data()), std::streamsize(len));
        if (is.gcount() != std::streamsize(len)) return false;
    }
    return true;
}

// ---- 4.1 默认实现：二进制日志文件（不需要任何第三方库） ----
class FileHistoryStore final : public IHistoryStore {
public:
    explicit FileHistoryStore(std::filesystem::path file) : file_(std::move(file)) {}

    bool open() override {
        std::error_code ec;
        if (const auto dir = file_.parent_path(); !dir.empty())
            std::filesystem::create_directories(dir, ec);
        out_.open(file_, std::ios::binary | std::ios::app);   // 追加模式，不破坏旧记录
        return out_.is_open();
    }

    void append(const proto::Frame& f) override {
        CriticalGuard lock(m_);
        if (!out_.is_open()) return;
        writeFrameToStream(out_, f);
        out_.flush();          // 立刻落盘，避免异常退出丢记录
    }

    std::vector<proto::Frame> loadAll() override {
        std::vector<proto::Frame> v;
        std::ifstream             in(file_, std::ios::binary);
        if (!in.is_open()) return v;
        proto::Frame f;
        while (readFrameFromStream(in, f)) v.push_back(std::move(f));
        return v;
    }

    const char* backendName() const override { return "二进制日志 (server_data/history.bin)"; }

private:
    std::filesystem::path file_;
    std::ofstream         out_;
    CriticalSection            m_;
};

#ifdef CHAT_USE_SQLITE
// ---- 4.2 可选扩展：SQLite 数据库 ----
//  表结构 messages(id, kind, sender, ts, content, file_size)
//    kind = 3 表示文字消息（content = 正文）
//    kind = 9 表示文件通知（content = 文件名，file_size = 文件大小）
class SqliteHistoryStore final : public IHistoryStore {
public:
    explicit SqliteHistoryStore(std::filesystem::path db) : dbPath_(std::move(db)) {}

    ~SqliteHistoryStore() override {
        if (db_) sqlite3_close(db_);
    }

    bool open() override {
        std::error_code ec;
        if (const auto dir = dbPath_.parent_path(); !dir.empty())
            std::filesystem::create_directories(dir, ec);

        if (sqlite3_open(pathToU8(dbPath_).c_str(), &db_) != SQLITE_OK) {
            logLine(std::string("打开 SQLite 数据库失败: ") +
                    (db_ ? sqlite3_errmsg(db_) : "unknown"));
            return false;
        }
        exec("PRAGMA journal_mode=WAL;");      // WAL 模式：写入更快、更抗崩溃
        exec("PRAGMA synchronous=NORMAL;");
        exec("CREATE TABLE IF NOT EXISTS messages("
             "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
             "  kind      INTEGER NOT NULL,"      // 3=文字 9=文件通知
             "  sender    INTEGER NOT NULL,"      // 发送者编号
             "  ts        INTEGER NOT NULL,"      // Unix 时间戳（秒）
             "  content   TEXT    NOT NULL,"      // 正文 或 文件名
             "  file_size INTEGER NOT NULL DEFAULT 0);");
        return true;
    }

    void append(const proto::Frame& f) override {
        int         sender  = 0;
        uint64_t    ts      = 0;
        uint64_t    fsize   = 0;
        std::string content;

        // 只把「文字消息」和「文件通知」写进数据库
        if (f.type == proto::FT_TEXT) {
            proto::Reader r(f.payload);
            uint8_t       sid = 0;
            std::string   name;
            if (!r.u8(sid) || !r.u64(ts) || !r.str16(name) || !r.restString(content)) return;
            sender = sid;
        } else if (f.type == proto::FT_FILE_NOTICE) {
            proto::Reader r(f.payload);
            uint8_t       sid = 0;
            if (!r.u8(sid) || !r.u64(ts) || !r.u64(fsize) || !r.str16(content)) return;
            sender = sid;
        } else {
            return;      // 其它类型不入库
        }

        static const char* kSql =
            "INSERT INTO messages(kind,sender,ts,content,file_size) VALUES(?,?,?,?,?);";
        CriticalGuard lock(m_);
        sqlite3_stmt*               stmt = nullptr;
        if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return;
        sqlite3_bind_int(stmt, 1, int(f.type));
        sqlite3_bind_int(stmt, 2, sender);
        sqlite3_bind_int64(stmt, 3, sqlite3_int64(ts));
        sqlite3_bind_text(stmt, 4, content.c_str(), int(content.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, sqlite3_int64(fsize));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    std::vector<proto::Frame> loadAll() override {
        std::vector<proto::Frame> out;
        static const char*        kSql =
            "SELECT kind,sender,ts,content,file_size FROM messages ORDER BY id ASC;";
        CriticalGuard lock(m_);
        sqlite3_stmt*               stmt = nullptr;
        if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) return out;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int         kind   = sqlite3_column_int(stmt, 0);
            const int         sender = sqlite3_column_int(stmt, 1);
            const uint64_t    ts     = uint64_t(sqlite3_column_int64(stmt, 2));
            const char*       raw    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            const std::string content = raw ? raw : "";
            const uint64_t    fsize   = uint64_t(sqlite3_column_int64(stmt, 4));

            if (kind == proto::FT_TEXT) {
                out.push_back(proto::makeText(sender, ts, "", content));
            } else if (kind == proto::FT_FILE_NOTICE) {
                out.push_back(proto::makeFileNotice(sender, ts, fsize, content));
            }
        }
        sqlite3_finalize(stmt);
        return out;
    }

    const char* backendName() const override { return "SQLite (server_data/chat.db)"; }

private:
    void exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            logLine(std::string("SQLite 执行失败: ") + (err ? err : "?"));
            sqlite3_free(err);
        }
    }

    std::filesystem::path dbPath_;
    sqlite3*              db_ = nullptr;
    CriticalSection            m_;
};
#endif  // CHAT_USE_SQLITE
// ============================== 5. 服务端主体 ==============================
class ChatServer {
public:
    ChatServer(uint16_t port, std::filesystem::path dataDir)
        : port_(port), dataDir_(std::move(dataDir)) {}

    ~ChatServer() { shutdown(); }

    bool start();       // 初始化 Winsock / 存储 / 监听
    void run();         // 主循环：accept + 回收线程
    void shutdown();    // 关闭监听、断开客户端、等待线程结束

    static std::atomic<bool> s_stopRequested;   // Ctrl+C 时置位

private:
    // 一个已连接的客户端
    struct Client {
        std::atomic<SOCKET> sock{INVALID_SOCKET};
        int                 id = 0;                 // 编号：1 或 2
        std::string         name;                   // 昵称
        std::atomic<bool>   online{false};          // 是否已完成 HELLO 握手
        std::atomic<bool>   done{false};            // 工作线程是否已结束（可回收）
        CriticalSection          sendMutex;              // 串行化“对同一个 socket 的写”
        WinThread           worker;

        // ---- 以下字段只被它自己的工作线程访问，无需加锁 ----
        bool                  uploading  = false;   // 是否正在接收该客户端上传的文件
        uint32_t              upXferId   = 0;       // 当前传输 ID
        uint64_t              upSize     = 0;       // 发送方声明的总大小
        uint64_t              upReceived = 0;       // 实际已收到字节数
        std::string           upName;               // 落盘后的文件名
        std::filesystem::path upDiskPath;           // 落盘完整路径
        std::ofstream         upFile;               // 落盘文件流
        Stopwatch             upWatch;               // 本次传输的耗时统计（QPC）
    };
    using ClientPtr = std::shared_ptr<Client>;

    void worker(ClientPtr c);                                     // 客户端工作线程
    void handleFrame(const ClientPtr& c, const proto::Frame& f);  // 处理一个上行帧

    bool        sendTo(int id, const proto::Frame& f);            // 给指定编号发一帧
    void        relayToPeer(int fromId, const proto::Frame& f);   // 转发给“另一个人”
    void        broadcast(const proto::Frame& f, int exceptId);   // 广播（可排除某编号）
    void        sendHistoryTo(const ClientPtr& c);                // 下发历史记录
    void        persist(const proto::Frame& f);                   // 写入聊天记录
    void        abortUpload(const ClientPtr& c);                  // 中断并删除半截文件
    void        streamFileToClient(const ClientPtr& c, const std::string& name);
    void        reapFinishedClients();                            // 回收已结束的连接
    std::string onlineUserList();                                 // "1=张三, 2=李四"

    uint16_t                       port_;
    std::filesystem::path          dataDir_;      // 数据目录（默认 server_data/）
    std::filesystem::path          filesDir_;     // 收到的文件（server_data/files/）
    SOCKET                         listenSock_ = INVALID_SOCKET;
    std::unique_ptr<IHistoryStore> history_;      // 聊天记录存储
    std::vector<proto::Frame>      historyCache_; // 启动时加载的历史（内存副本，便于回放）
    CriticalSection                     mtx_;          // 保护 clients_ 与 historyCache_
    std::vector<ClientPtr>         clients_;
    std::atomic<bool>              running_{false};
    std::atomic<uint32_t>          nextServerXfer_{1};   // 服务端自己发文件时用的传输 ID
};

std::atomic<bool> ChatServer::s_stopRequested{false};

// ------------------------------ 5.1 启动 ------------------------------
bool ChatServer::start() {
    // (1) 初始化 Winsock
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        logLine("WSAStartup 失败：无法初始化 Windows Socket");
        return false;
    }

    // (2) 准备数据目录
    std::error_code ec;
    if (dataDir_.empty()) dataDir_ = u8path("server_data");
    filesDir_ = dataDir_ / u8path("files");
    std::filesystem::create_directories(filesDir_, ec);

    // (3) 打开聊天记录存储，并把历史加载到内存
#ifdef CHAT_USE_SQLITE
    history_ = std::make_unique<SqliteHistoryStore>(dataDir_ / u8path("chat.db"));
#else
    history_ = std::make_unique<FileHistoryStore>(dataDir_ / u8path("history.bin"));
#endif
    if (!history_->open()) {
        logLine("打开聊天记录存储失败");
        return false;
    }
    historyCache_ = history_->loadAll();

    // (4) 创建监听 socket 并绑定端口
    listenSock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock_ == INVALID_SOCKET) {
        logLine("创建监听 socket 失败");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 监听本机所有网卡
    addr.sin_port        = htons(port_);

    if (::bind(listenSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        logLine("bind 失败（端口可能被占用）：" + std::to_string(port_));
        return false;
    }
    if (::listen(listenSock_, 8) == SOCKET_ERROR) {
        logLine("listen 失败");
        return false;
    }

    running_ = true;
    logLine("服务端启动成功，监听端口 " + std::to_string(port_) + "，最多允许 " +
            std::to_string(proto::kMaxClients) + " 个客户端同时在线");
    logLine(std::string("聊天记录后端：") + history_->backendName() + "，已加载历史记录 " +
            std::to_string(historyCache_.size()) + " 条");
    logLine("文件保存目录：" + pathToU8(filesDir_));
    return true;
}

// ------------------------------ 5.2 主循环 ------------------------------
void ChatServer::run() {
    while (running_) {
        if (s_stopRequested.load()) break;   // 收到 Ctrl+C

        // 用 select 等待新连接，超时 500ms，这样能及时响应退出请求
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSock_, &readSet);
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 500 * 1000;

        const int rc = ::select(0, &readSet, nullptr, nullptr, &tv);
        if (rc == SOCKET_ERROR) {
            if (!running_) break;
            ::Sleep(50);
            continue;
        }
        if (rc == 0) {              // 超时：顺手回收已经结束的连接
            reapFinishedClients();
            continue;
        }

        sockaddr_in peer{};
        int         peerLen = sizeof(peer);
        const SOCKET s = ::accept(listenSock_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (s == INVALID_SOCKET) continue;

        reapFinishedClients();      // 保证名额计数准确

        // ---- 判断聊天室是否已经满员（最多 2 个客户端）----
        bool full = false;
        {
            CriticalGuard lock(mtx_);
            full = clients_.size() >= size_t(proto::kMaxClients);
        }
        if (full) {
            logLine("有人尝试连接，但聊天室已满，已拒绝");
            sendFrame(s, proto::makeSystem(
                             nowSeconds(),
                             "服务器已满：本聊天室只允许 2 个客户端同时在线，连接被拒绝。"));
            // 先半关闭「写」方向，再用一小段时间把对方可能发来的数据读干净，最后关闭。
            // 否则直接 closesocket 会因为后续到达的数据产生 RST，
            // 把刚刚发出去的提示冲掉，客户端就看不到“服务器已满”了。
            ::shutdown(s, SD_SEND);
            DWORD drainTimeout = 500;
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&drainTimeout),
                         sizeof(drainTimeout));
            char dummy[512];
            int  n     = 0;
            int  guard = 0;
            do {
                n = ::recv(s, dummy, sizeof(dummy), 0);
            } while (n > 0 && ++guard < 64);
            ::closesocket(s);
            continue;
        }

        // ---- 接纳这个连接 ----
        auto c = std::make_shared<Client>();
        {
            CriticalGuard lock(mtx_);
            c->id = int(clients_.size()) + 1;   // 1 号、2 号
            clients_.push_back(c);
        }
        c->sock = s;

        // 握手阶段设置 10 秒接收超时：
        // 防止有人连上却不发 HELLO，白白占用一个名额。
        DWORD timeoutMs = 10000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        logLine("客户端编号 " + std::to_string(c->id) + " 已接入，等待握手…");
        c->worker.start([this, c] { worker(c); });
    }
}

// ------------------------------ 5.3 单个客户端的工作线程 ------------------------------
void ChatServer::worker(ClientPtr c) {
    const SOCKET sock = c->sock.load();

    // (1) 等待 HELLO，读取昵称
    proto::Frame hello;
    std::string  name;
    if (!recvFrame(sock, hello) || !proto::parseHello(hello, name) || name.empty()) {
        logLine("客户端编号 " + std::to_string(c->id) + " 握手失败，断开连接");
        c->done = true;                     // 交给主线程回收（含关闭 socket）
        return;
    }
    name = chomp(std::move(name)).substr(0, 64);   // 昵称限长，防止超长数据

    // 握手结束，恢复正常阻塞模式（不再有接收超时）
    DWORD timeoutMs = 0;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    c->name   = name;
    c->online = true;
    logLine("客户端 " + std::to_string(c->id) + " 上线，昵称：" + name);

    // (2) 回执：告诉客户端“你是几号”
    if (!sendTo(c->id, proto::makeHelloAck(c->id, name))) {
        c->online = false;
        c->done   = true;
        return;
    }

    // (3) 自动下发历史聊天记录（需求 4）
    sendHistoryTo(c);

    // (4) 告诉新客户端当前在线成员
    sendTo(c->id, proto::makeSystem(
                      nowSeconds(),
                      "当前在线用户：" + onlineUserList() + "（你已成功连接服务器）"));

    // (5) 通知对方有人上线
    broadcast(proto::makeSystem(nowSeconds(), name + " 加入了聊天室"), c->id);

    // (6) 主循环：不断读取该客户端发来的帧
    while (running_ && !c->done && !s_stopRequested.load()) {
        proto::Frame f;
        if (!recvFrame(sock, f)) break;          // 对端断开或出错
        handleFrame(c, f);
    }

    // (7) 收尾
    abortUpload(c);                              // 半截文件要删掉
    broadcast(proto::makeSystem(nowSeconds(), name + " 离开了聊天室"), c->id);
    logLine("客户端 " + std::to_string(c->id) + "（" + name + "）已断开");
    c->online = false;
    c->done   = true;                            // 通知主线程回收
}

// ------------------------------ 5.4 处理一个上行帧 ------------------------------
void ChatServer::handleFrame(const ClientPtr& c, const proto::Frame& f) {
    switch (f.type) {
    // ---------- 文字消息：补齐“谁发的 + 时间”，转发并入库 ----------
    case proto::FT_TEXT: {
        std::string text(reinterpret_cast<const char*>(f.payload.data()),
                         f.payload.size());
        text = chomp(std::move(text));
        if (text.empty()) return;
        if (text.size() > 16u * 1024u) text.resize(16u * 1024u);   // 单条上限 16 KiB

        const uint64_t ts = nowSeconds();
        proto::Frame   out = proto::makeText(c->id, ts, c->name, text);
        persist(out);                 // 先落盘，保证历史不丢
        relayToPeer(c->id, out);      // 再转发给对方
        logLine("[" + std::to_string(c->id) + " " + c->name + "] " + text);
        break;
    }

    // ---------- 文件开始：打开落盘文件，并通知对方准备接收 ----------
    case proto::FT_FILE_BEGIN: {
        uint32_t    xferId = 0;
        uint64_t    size   = 0;
        std::string name;
        if (!proto::parseFileBegin(f, xferId, size, name)) return;

        if (c->uploading) abortUpload(c);          // 上一次没传完，先清理

        name          = sanitizeFileName(name);
        c->upDiskPath = uniquePath(filesDir_, name);
        c->upFile.open(c->upDiskPath, std::ios::binary | std::ios::trunc);
        if (!c->upFile.is_open()) {
            logLine("无法创建文件：" + pathToU8(c->upDiskPath));
            sendTo(c->id,
                   proto::makeSystem(nowSeconds(), "服务端保存文件失败，传输被取消。"));
            relayToPeer(c->id, proto::makeFileAbort(c->id, xferId, "服务端保存失败"));
            return;
        }

        c->uploading  = true;
        c->upXferId   = xferId;
        c->upSize     = size;
        c->upReceived = 0;
        c->upName     = pathToU8(c->upDiskPath.filename());
        c->upWatch.restart();               // 用 QueryPerformanceCounter 开始计时

        logLine("收到文件传输请求：" + c->upName + "（" + humanSize(size) + "），来自 " +
                c->name);
        sendTo(c->id, proto::makeSystem(nowSeconds(), "开始发送文件 " + c->upName + "（" +
                                                         humanSize(size) + "）…"));
        relayToPeer(c->id,
                    proto::makeFileBegin(c->id, nowSeconds(), xferId, size, c->upName));
        break;
    }

    // ---------- 文件分片：写盘 + 转发（流式处理，不缓存整个文件） ----------
    case proto::FT_FILE_CHUNK: {
        uint32_t       xferId = 0;
        const uint8_t* data   = nullptr;
        size_t         n      = 0;
        if (!proto::parseFileChunk(f, xferId, data, n)) return;
        if (!c->uploading || xferId != c->upXferId) return;   // 不属于当前传输，忽略

        if (c->upFile.is_open() && n > 0)
            c->upFile.write(reinterpret_cast<const char*>(data), std::streamsize(n));

        const uint64_t before = c->upReceived;
        c->upReceived += n;

        // 每写完 2 MiB 打印一次进度
        if (before / (2u * 1024u * 1024u) != c->upReceived / (2u * 1024u * 1024u)) {
            logLine("接收 " + c->upName + " 进度：" + humanSize(c->upReceived) + " / " +
                    humanSize(c->upSize));
        }

        relayToPeer(c->id, proto::makeFileChunk(c->id, xferId, data, n));
        break;
    }

    // ---------- 文件结束：关闭文件、转发结束帧、广播文件通知 ----------
    case proto::FT_FILE_END: {
        uint32_t xferId = 0;
        if (!proto::parseFileEnd(f, xferId)) return;
        if (!c->uploading || xferId != c->upXferId) return;

        if (c->upFile.is_open()) c->upFile.close();
        const std::string stored  = c->upName;
        const uint64_t    gotSize = c->upReceived;
        c->uploading              = false;

        relayToPeer(c->id, proto::makeFileEnd(c->id, xferId));

        // 生成“文件通知”：既写入聊天历史，也广播给双方（kServerId 表示不排除任何人）
        const proto::Frame notice =
            proto::makeFileNotice(c->id, nowSeconds(), gotSize, stored);
        persist(notice);
        broadcast(notice, proto::kServerId);
        const double used = c->upWatch.seconds();          // 高精度耗时（秒）
        const double mbps = used > 0.0001
                                ? double(gotSize) / 1024.0 / 1024.0 / used
                                : 0.0;
        char speed[64]{};
        std::snprintf(speed, sizeof(speed), "，耗时 %.2f 秒，平均 %.2f MB/s", used, mbps);
        logLine("文件接收完成：" + stored + "（" + humanSize(gotSize) + "），保存于 " +
                pathToU8(filesDir_) + speed);
        break;
    }

    // ---------- 文件中断 ----------
    case proto::FT_FILE_ABORT: {
        uint32_t    xferId = 0;
        std::string why;
        proto::Reader r(f.payload);
        if (r.u32(xferId)) r.restString(why);
        if (c->uploading && xferId == c->upXferId) {
            if (c->upFile.is_open()) c->upFile.close();
            std::error_code ec;
            std::filesystem::remove(c->upDiskPath, ec);   // 删除半截文件
            c->uploading = false;
            relayToPeer(c->id, proto::makeFileAbort(c->id, xferId, "发送方中断"));
            logLine("文件传输被中断并清理：" + c->upName);
        }
        break;
    }

    // ---------- 请求下载服务端保存的文件 ----------
    case proto::FT_REQ_FILE: {
        std::string name;
        if (!proto::parseReqFile(f, name)) return;
        streamFileToClient(c, sanitizeFileName(name));
        break;
    }

    // ---------- 主动断开 ----------
    case proto::FT_BYE:
        c->done = true;      // 让工作线程退出，随后由主线程回收
        break;

    default:
        logLine("忽略未知帧类型：" + std::to_string(int(f.type)));
        break;
    }
}
// ------------------------------ 5.5 发送 / 转发 ------------------------------
bool ChatServer::sendTo(int id, const proto::Frame& f) {
    ClientPtr target;
    {
        CriticalGuard lock(mtx_);
        for (auto& c : clients_) {
            if (c->id == id) {
                target = c;
                break;
            }
        }
    }
    if (!target) return false;
    const SOCKET s = target->sock.load();
    if (s == INVALID_SOCKET) return false;

    // 多个线程可能同时给同一个人发数据（对方转发的消息 + 系统通知 + 文件分片），
    // 必须用互斥锁串行化，否则帧头/负载会交错，接收端解析就会错乱。
    CriticalGuard lock(target->sendMutex);
    return sendFrame(s, f);
}

void ChatServer::relayToPeer(int fromId, const proto::Frame& f) {
    ClientPtr peer;
    {
        CriticalGuard lock(mtx_);
        for (auto& c : clients_) {
            if (c->id != fromId && c->online.load()) {
                peer = c;
                break;
            }
        }
    }
    // 对方还没握手完成时消息只进历史记录，等它握手时会通过历史回放拿到。
    if (!peer) return;

    const SOCKET s = peer->sock.load();
    if (s == INVALID_SOCKET) return;
    CriticalGuard lock(peer->sendMutex);
    if (!sendFrame(s, f)) logLine("转发失败（对方可能已断开）");
}

void ChatServer::broadcast(const proto::Frame& f, int exceptId) {
    std::vector<ClientPtr> snapshot;
    {
        CriticalGuard lock(mtx_);
        snapshot = clients_;
    }
    for (auto& c : snapshot) {
        if (!c->online.load() || c->id == exceptId) continue;
        const SOCKET s = c->sock.load();
        if (s == INVALID_SOCKET) continue;
        CriticalGuard lock(c->sendMutex);
        sendFrame(s, f);
    }
}

// 把内存里的历史记录原样回放给新上线的客户端
void ChatServer::sendHistoryTo(const ClientPtr& c) {
    std::vector<proto::Frame> snapshot;
    {
        CriticalGuard lock(mtx_);
        snapshot = historyCache_;
    }
    if (snapshot.empty()) {
        sendTo(c->id, proto::makeSystem(nowSeconds(), "=== 暂无历史聊天记录 ==="));
        return;
    }
    sendTo(c->id, proto::makeSystem(nowSeconds(),
                                    "=== 历史聊天记录开始（共 " +
                                        std::to_string(snapshot.size()) + " 条）==="));
    for (const auto& f : snapshot) {
        if (!sendTo(c->id, f)) return;      // 发送失败说明客户端已断开
    }
    sendTo(c->id, proto::makeSystem(nowSeconds(), "=== 历史聊天记录结束 ==="));
}

void ChatServer::persist(const proto::Frame& f) {
    if (!history_) return;
    history_->append(f);                    // 落盘
    CriticalGuard lock(mtx_);
    historyCache_.push_back(f);             // 同时更新内存副本（供后来者回放）
}

void ChatServer::abortUpload(const ClientPtr& c) {
    if (!c->uploading) return;
    if (c->upFile.is_open()) c->upFile.close();
    std::error_code ec;
    std::filesystem::remove(c->upDiskPath, ec);            // 删除不完整的文件
    relayToPeer(c->id, proto::makeFileAbort(c->id, c->upXferId, "连接中断"));
    logLine("上传中断，已删除不完整文件：" + c->upName);
    c->uploading = false;
}

// 把服务端磁盘上的文件发给请求者（同样分片发送，支持任意大小）
void ChatServer::streamFileToClient(const ClientPtr& c, const std::string& name) {
    const std::filesystem::path path = filesDir_ / u8path(name);
    std::error_code             ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        sendTo(c->id, proto::makeSystem(nowSeconds(), "服务端没有该文件：" + name));
        return;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        sendTo(c->id, proto::makeSystem(nowSeconds(), "服务端打开文件失败：" + name));
        return;
    }
    const uint64_t size   = uint64_t(std::filesystem::file_size(path, ec));
    const uint32_t xferId = nextServerXfer_.fetch_add(1);

    sendTo(c->id,
           proto::makeFileBegin(proto::kServerId, nowSeconds(), xferId, size, name));
    std::vector<uint8_t> buf(proto::kChunkSize);
    uint64_t             sent = 0;
    while (running_ && !c->done && in) {
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        if (!sendTo(c->id,
                    proto::makeFileChunk(proto::kServerId, xferId, buf.data(), size_t(n)))) {
            return;                                   // 对方已断开
        }
        sent += uint64_t(n);
    }
    sendTo(c->id, proto::makeFileEnd(proto::kServerId, xferId));
    logLine("已向客户端 " + std::to_string(c->id) + " 下发文件 " + name + "（" +
            humanSize(sent) + "）");
}

std::string ChatServer::onlineUserList() {
    CriticalGuard lock(mtx_);
    std::string                 s;
    for (auto& c : clients_) {
        if (!c->online.load()) continue;
        if (!s.empty()) s += ", ";
        s += std::to_string(c->id) + "=" + c->name;
    }
    return s.empty() ? "（无）" : s;
}

// 回收已经结束的连接：join 线程、关闭 socket、从列表里删除
void ChatServer::reapFinishedClients() {
    std::vector<ClientPtr> dead;
    {
        CriticalGuard lock(mtx_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            if ((*it)->done.load()) {
                dead.push_back(*it);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& c : dead) {
        if (c->worker.joinable()) c->worker.join();     // 此时工作线程一定已退出
        const SOCKET s = c->sock.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            ::shutdown(s, SD_BOTH);
            ::closesocket(s);
        }
        logLine("连接资源已释放：编号 " + std::to_string(c->id));
    }
}

// ------------------------------ 5.6 关闭 ------------------------------
void ChatServer::shutdown() {
    if (!running_.exchange(false)) return;      // 只执行一次

    logLine("正在关闭服务端…");

    // (1) 关闭监听 socket：让 select 立刻返回
    if (listenSock_ != INVALID_SOCKET) {
        ::closesocket(listenSock_);
        listenSock_ = INVALID_SOCKET;
    }

    // (2) 断开所有客户端，唤醒阻塞在 recv 上的工作线程
    std::vector<ClientPtr> snapshot;
    {
        CriticalGuard lock(mtx_);
        snapshot = clients_;
    }
    for (auto& c : snapshot) {
        c->online = false;
        const SOCKET s = c->sock.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            ::shutdown(s, SD_BOTH);
            ::closesocket(s);
        }
    }

    // (3) 等待所有工作线程退出
    for (auto& c : snapshot) {
        if (c->worker.joinable()) c->worker.join();
    }
    {
        CriticalGuard lock(mtx_);
        clients_.clear();
    }

    ::WSACleanup();
    logLine("服务端已停止。");
}

// ============================== 6. 程序入口 ==============================
namespace {
// Ctrl+C / SIGTERM 处理：只置标志位，真正的清理动作留给主线程做
void onSignal(int) {
    ChatServer::s_stopRequested = true;
}
}  // namespace

int main(int argc, char** argv) {
    // 让控制台按 UTF-8 显示中文
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // 命令行参数：chat_server.exe [端口] [数据目录]
    uint16_t              port    = proto::kDefaultPort;
    std::filesystem::path dataDir = u8path("server_data");
    if (argc >= 2) {
        const int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535) port = uint16_t(p);
    }
    if (argc >= 3) dataDir = u8path(argv[2]);

    std::cout << "===============================================\n"
              << "  双人聊天室 服务端  (C++23 / MinGW / TCP)\n"
              << "  用法: chat_server.exe [端口] [数据目录]\n"
              << "  当前端口: " << port << "   数据目录: " << pathToU8(dataDir) << "\n"
              << "  按 Ctrl+C 退出\n"
              << "===============================================" << std::endl;

    ChatServer server(port, dataDir);
    if (!server.start()) {
        server.shutdown();
        return 1;
    }
    server.run();        // 阻塞在这里，直到 Ctrl+C
    server.shutdown();
    return 0;
}
