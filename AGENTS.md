# AGENTS.md — 双人聊天室（chat_server / chat_client）

本文件面向在本仓库中继续工作的 AI 编码助手（以及人），说明工程约束、构建方式与代码约定。

## 1. 项目概述

一个 C/S 架构的双人命令行聊天室：

- `chat_server/`：服务端独立 CMake 工程，负责监听连接、管理会话（只允许 2 个客户端）、
  转发消息、持久化聊天记录、给新登录的客户端回放历史记录。
- `chat_client/`：客户端独立 CMake 工程，负责连接服务端、收发文字、收发文件、下载历史文件。

## 2. 硬性约束（不可违反）

1. **语言标准**：C++23，禁止 C++26。`CMakeLists.txt` 中固定
   `CMAKE_CXX_STANDARD 23` + `CMAKE_CXX_STANDARD_REQUIRED ON` + `CMAKE_CXX_EXTENSIONS OFF`。
2. **编译环境**：MinGW-w64（g++ 13+），Windows 平台。
3. **构建方式**：CMake（`-G "MinGW Makefiles"`）。
4. **工程组织**：服务端与客户端必须是**两个独立工程**，分别位于 `chat_server/` 与
   `chat_client/`，各自拥有独立的 `CMakeLists.txt`，各自单独构建。
   **禁止**把两者合并成一个工程、禁止共用头文件/源码目录。
5. **无第三方依赖**（可选扩展除外）：只依赖 Windows 系统库 `ws2_32`。

## 3. 时间与线程约定（重要）

**禁止使用** `std::chrono`、`std::this_thread::sleep_for`、`std::thread`、`std::mutex`
以及任何 `clock_gettime` / `nanosleep` 系列调用。

原因：在 MinGW 上它们会依赖 POSIX 线程库 `libwinpthread-1.dll`，
一旦运行时被 `PATH` 中其它软件自带的旧版 DLL 抢先加载，就会出现
`无法定位程序输入点 clock_gettime64` 的崩溃。

统一改用 Windows 原生 API（两个 `main.cpp` 中各有一份相同的实现）：

| 用途 | 使用 | 替代 |
|------|------|------|
| 当前时间 | `GetSystemTimeAsFileTime` + `FileTimeToLocalFileTime` / `FileTimeToSystemTime` | `std::chrono::system_clock` / `time()` |
| 高精度计时、测速 | `QueryPerformanceCounter` / `QueryPerformanceFrequency`（`Stopwatch` 类） | `std::chrono::steady_clock` |
| 线程 | `CreateThread`（`WinThread` 类） | `std::thread` |
| 互斥 | `CRITICAL_SECTION`（`CriticalSection` + `CriticalGuard`） | `std::mutex` / `std::lock_guard` |
| 睡眠 | `::Sleep(ms)` | `std::this_thread::sleep_for` |
| 日志 / 时钟戳格式化 | `std::snprintf` | `std::strftime` |

两个 `CMakeLists.txt` 都必须保留：

```cmake
add_compile_options(-D_WIN32 -DWIN32)
```

并且**不要**引入 `find_package(Threads)` / `Threads::Threads`（即不链接 POSIX 线程库）。
构建后由 CMake 自动把 MinGW 自带的 `libwinpthread-1.dll` 复制到 exe 同目录，
避免加载到 `PATH` 中的旧版 DLL。

## 4. 通信协议约定

协议定义在**两个** `main.cpp` 中（各自一份，刻意不共享头文件）。
**修改协议时必须同时修改两端，否则无法互通。**

帧格式：`[1 字节类型][4 字节大端负载长度][负载]`

上行（客户端 → 服务端）：`FT_HELLO`、`FT_TEXT`、`FT_FILE_BEGIN`、`FT_FILE_CHUNK`、
`FT_FILE_END`、`FT_FILE_ABORT`、`FT_REQ_FILE`、`FT_BYE`。

下行（服务端 → 客户端）：`FT_HELLO_ACK`、`FT_SYSTEM`、`FT_TEXT`、
`FT_FILE_BEGIN`、`FT_FILE_CHUNK`、`FT_FILE_END`、`FT_FILE_ABORT`、`FT_FILE_NOTICE`。

要点：

- 上行帧不带发送者信息，由服务端补齐编号/昵称/时间戳后再转发与落库。
- 文件按 64 KiB 分片流式收发，`u64` 描述大小，支持任意大小文件，不整块读入内存。
- 一次文件传输用 `(发送者编号, 传输ID)` 唯一标识，可与其他传输交错。
- 所有字符串都是 UTF-8；路径在 Windows 上经 `u8path()` / `pathToU8()` 做 UTF-8 ↔ UTF-16 转换。
- 文件名必须经过 `sanitizeFileName()` 消毒（去目录、替换非法字符），防止路径穿越。

## 5. 构建

```bat
cd chat_server
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

cd ..\chat_client
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

可选（服务端）SQLite 持久化：

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCHAT_USE_SQLITE=ON
```

## 6. 运行与验证

```bat
:: 1) 服务端
chat_server.exe 9080 server_data
:: 2) 两个客户端
chat_client.exe 127.0.0.1 9080 Alice
chat_client.exe 127.0.0.1 9080 Bob
```

验证清单：

1. 客户端 B 后连接时，能自动打印历史聊天记录（`=== 历史聊天记录开始 ===`）。
2. 双方文字消息互达；第 3 个客户端被拒绝并收到“服务器已满”提示。
3. `/file 大文件` 能完整传过去，两端 `certutil -hashfile <文件> MD5` 一致。
4. 服务端 `server_data/files/` 保存了文件；客户端 `/get 文件名` 能重新下载。
5. 重启服务端后，历史记录仍然存在。
6. 构建产物导入表检查（应无输出）：

   ```bat
   objdump -p chat_server.exe | findstr /i "clock_gettime nanosleep"
   ```

## 7. 代码风格

- 全部注释使用中文，按“协议 / 工具 / 存储 / 主体”分节，函数前写清“做什么、为什么”。
- 每个源文件顶部都要有文件头注释（功能清单、编译运行方式）。
- 不使用 `std::format` / `std::print` / `std::expected` 等太新的库设施，保证 MinGW 13+ 可编译。
- 发送/接收必须走 `sendAll` / `recvExact`（循环处理 TCP 半包），禁止裸 `send` / `recv`。

## 8. 平台与依赖声明

- 目标平台：Windows（使用 Winsock2、`CreateThread`、`CRITICAL_SECTION` 等 Win32 API）。
- 依赖：MinGW-w64（g++ ≥ 13）、CMake ≥ 3.20；链接系统库 `ws2_32`。
- 可选依赖：SQLite3（仅 `-DCHAT_USE_SQLITE=ON` 时需要）。
- 非 Windows 平台无法编译（源码中有 `#error` 保护）。
