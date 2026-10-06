# 双人聊天室（C/S 架构 · C++23 · MinGW · CMake）

[![build](https://github.com/OWNER/REPO/actions/workflows/build.yml/badge.svg)](https://github.com/OWNER/REPO/actions/workflows/build.yml)
![C++](https://img.shields.io/badge/C%2B%2B-23-blue)
![toolchain](https://img.shields.io/badge/toolchain-MinGW--w64-orange)
![platform](https://img.shields.io/badge/platform-Windows-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

> 把上面的 `OWNER/REPO` 换成你自己的 GitHub 用户名/仓库名，CI 徽章就会自动显示构建状态。

一个命令行版的 TCP 双人聊天室：**服务端与客户端是两个完全独立的 CMake 工程**，
分别位于 `chat_server/` 与 `chat_client/`，各自拥有 `CMakeLists.txt`，可以单独编译、单独运行。

## 功能特性

- **双人会话**：服务端只允许 2 个客户端同时在线，第 3 个连接会被明确拒绝。
- **即时文字聊天**：任意一端输入回车，另一端立即收到。
- **任意类型/大小文件传输**：64 KiB 分片流式收发，不把整个文件读进内存。
- **会话记录持久化**：文字与文件元信息（文件名、大小、发送者、时间）全部落盘。
- **历史记录回放**：服务端启动即读取历史文件；客户端登录后自动收到并显示全部历史。
- **历史文件再下载**：客户端可用 `/get <文件名>` 从服务端重新拉取之前传输过的文件。
- **可选 SQLite 后端**：`-DCHAT_USE_SQLITE=ON` 时改用 SQLite 数据库保存聊天记录。
- **稳定的分帧协议**：长度前缀分帧 + 半包处理 + 发送端加锁，杜绝粘包/错帧。

## 目录结构

```
.
├── .github/workflows/build.yml     # GitHub Actions：MinGW + CMake 自动构建
├── chat_server/                    # 工程一：服务端（独立 CMake 工程）
│   ├── CMakeLists.txt
│   └── main.cpp
├── chat_client/                    # 工程二：客户端（独立 CMake 工程）
│   ├── CMakeLists.txt
│   └── main.cpp
├── AGENTS.md                       # AI Agent 协作说明（工程约束 / 代码约定）
├── LICENSE                         # MIT
└── README.md
```

两个工程之间**没有任何共享文件**（通信协议在各自的 `main.cpp` 里各写一份），
因此可以分别拷贝、分别构建，互不影响。

## 依赖声明

| 项目 | 说明 |
|------|------|
| 目标平台 | Windows（使用 Winsock2 与 Win32 线程/时间 API） |
| 编译器 | MinGW-w64，g++ **13 及以上**（C++23） |
| 构建工具 | CMake **3.20 及以上**，`-G "MinGW Makefiles"` |
| 系统库 | `ws2_32`（Windows Socket 2） |
| 可选依赖 | SQLite3（仅 `-DCHAT_USE_SQLITE=ON` 时需要） |

> 源码中所有时间、线程、互斥都使用 Windows 原生 API
> （`GetSystemTimeAsFileTime` / `QueryPerformanceCounter` / `CreateThread` / `CRITICAL_SECTION`），
> 不使用 `std::chrono` / `std::thread` / `std::mutex`，因此产物不依赖
> `clock_gettime` 系列符号，不会出现“无法定位程序输入点 clock_gettime64”的错误。

## 快速开始

### 1. 编译

```bat
:: 服务端
cd chat_server
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

:: 客户端
cd ..\chat_client
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

如果 `g++` 不在 `PATH` 中，显式指定工具链（注意路径用正斜杠）：

```bat
cmake -S . -B build -G "MinGW Makefiles" ^
      -DCMAKE_CXX_COMPILER=C:/mingw64/bin/g++.exe ^
      -DCMAKE_MAKE_PROGRAM=C:/mingw64/bin/mingw32-make.exe
```

可选：服务端使用 SQLite 保存聊天记录

```bat
:: MSYS2 安装依赖：pacman -S mingw-w64-x86_64-sqlite3
cmake -S . -B build -G "MinGW Makefiles" -DCHAT_USE_SQLITE=ON
cmake --build build -j
```

### 2. 运行

```bat
:: 窗口 1：服务端（端口 9080，数据目录 server_data）
chat_server.exe

:: 窗口 2 / 3：两个客户端
chat_client.exe 127.0.0.1 9080 Alice
chat_client.exe 127.0.0.1 9080 Bob
```

命令行参数：

```
chat_server.exe [端口] [数据目录]
chat_client.exe [服务器IP] [端口] [昵称] [下载目录]
```

客户端命令：

| 命令 | 作用 |
|------|------|
| `你好` | 直接输入文字，回车发送 |
| `/file D:\big.bin` | 给对方发送文件（任意类型、任意大小） |
| `/get big.bin` | 从服务端重新下载已保存的历史文件 |
| `/help` | 显示帮助 |
| `/sleep 1000` | 等待 1 秒（写自动化测试脚本时有用） |
| `/quit` | 退出聊天室 |

### 3. 验证

1. Alice 先发几条消息，再启动 Bob：Bob 登录后会立即打印
   `=== 历史聊天记录开始 ===` 和全部历史消息。
2. Alice 执行 `/file D:\大文件.bin`，Bob 的 `downloads/` 出现同名文件，
   两端 `certutil -hashfile <文件> MD5` 结果一致。
3. 服务端 `server_data/files/` 中留有该文件；Bob 用 `/get 文件名` 可再次下载。
4. 启动第 3 个客户端：会收到“服务器已满”提示并断开。
5. 关掉服务端再重启，客户端连上后仍能看到之前的聊天记录。

## 通信协议

帧结构：`[1 字节类型][4 字节大端负载长度][负载]`

| 方向 | 帧 | 说明 |
|------|----|------|
| C→S | `FT_HELLO` | 上报昵称 |
| C→S | `FT_TEXT` | 文字消息 |
| C→S | `FT_FILE_BEGIN` / `FT_FILE_CHUNK` / `FT_FILE_END` / `FT_FILE_ABORT` | 文件分片传输 |
| C→S | `FT_REQ_FILE` | 请求下载服务端文件 |
| S→C | `FT_HELLO_ACK` | 分配编号 |
| S→C | `FT_SYSTEM` / `FT_TEXT` / `FT_FILE_*` | 系统提示、聊天内容、文件转发 |
| S→C | `FT_FILE_NOTICE` | 文件元信息（写入历史记录并回放） |

上行帧不带发送者信息，由服务端补齐编号、昵称、时间戳后再转发与落盘，
这样历史记录回放时也能还原“谁、在什么时候、说了什么 / 发了什么文件”。

## 持续集成（GitHub Actions）

`.github/workflows/build.yml` 会在 push / PR 时自动：

1. 用 MSYS2 安装 MinGW-w64（g++ / cmake / make / sqlite3）；
2. 分别构建 `chat_server`、`chat_server + SQLite`、`chat_client` 三个组合；
3. 用 `objdump -p` 做回归检查：产物若重新引入 `clock_gettime` / `nanosleep` 依赖则直接失败；
4. 把生成的可执行文件作为 artifact 上传。

## AI Agent 辅助开发说明

本项目的开发过程使用了 AI Agent（Codex）辅助。按课程要求，仓库中一并提供了
[`AGENTS.md`](AGENTS.md)：其中记录了工程硬性约束、通信协议约定、
Win32 时间/线程替换规则与验证清单；与 Agent 的完整对话上下文随作业一并提交。

## 许可证

本项目基于 [MIT License](LICENSE) 开源。
请把 `LICENSE` 中的 `Copyright (c) 2026 <请替换为你的名字>` 改成你自己的名字。
