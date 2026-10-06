# ============================================================================
#  chat_server/CMakeLists.txt
#  双人聊天室 —— 服务端构建脚本
#  平台: Windows + MinGW-w64 (g++ 13+)      语言: C++23（禁用 C++26）
# ----------------------------------------------------------------------------
#  常用命令:
#    cmake -S . -B build -G "MinGW Makefiles"                       # 配置(Release)
#    cmake --build build -j                                         # 编译
#    cmake -S . -B build -G "MinGW Makefiles" -DCHAT_USE_SQLITE=ON  # 开启 SQLite
# ============================================================================

cmake_minimum_required(VERSION 3.20)

project(chat_server
        VERSION 1.0.0
        DESCRIPTION "双人 TCP 聊天室服务端"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
#  1. C++ 标准：固定使用 C++23，禁止 C++26
#     CMAKE_CXX_EXTENSIONS=OFF -> 生成 -std=c++23（而不是 gnu++23），
#     保证不使用 GNU 扩展，也不会“自动升级”到 C++26。
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认 Release；可用 -DCMAKE_BUILD_TYPE=Debug 覆盖
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

if(NOT WIN32)
  message(FATAL_ERROR "本项目目标平台是 Windows + MinGW，请在 Windows 上构建。")
endif()

add_executable(chat_server main.cpp)

# ---------------------------------------------------------------------------
#  2. 编译选项
#     -D_WIN32 -DWIN32 : 明确声明 Windows 平台（要求：不使用 POSIX 时间库）
#     -Wall -Wextra    : 打开常用警告
#     -O2              : Release 下优化
#     说明：源码里所有时间/线程都改用 Windows 原生 API
#           （GetSystemTimeAsFileTime / QueryPerformanceCounter / CreateThread /
#             CRITICAL_SECTION / Sleep），不再使用 clock_gettime 系列函数，
#           也不使用 std::chrono / std::thread / std::mutex，
#           因此不会再出现「无法定位程序输入点 clock_gettime64」的错误。
# ---------------------------------------------------------------------------
add_compile_options(-D_WIN32 -DWIN32)
target_compile_options(chat_server PRIVATE -Wall -Wextra)
target_compile_options(chat_server PRIVATE $<$<CONFIG:Release>:-O2>)

# 让编译器按 UTF-8 处理源码，避免源码中的中文注释在不同代码页下乱码
if(MINGW)
  target_compile_options(chat_server PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# ---------------------------------------------------------------------------
#  3. 依赖库
#     只链接 ws2_32（Windows Socket 2）。
#     不链接 POSIX 线程库 / POSIX 时间库（libpthread、clock_gettime、nanosleep），
#     所以这里也不再 find_package(Threads)。
# ---------------------------------------------------------------------------
target_link_libraries(chat_server PRIVATE ws2_32)

# ---------------------------------------------------------------------------
#  4. 可选项：静态链接 MinGW 的 libgcc / libstdc++
#     打开后 exe 不再依赖 libstdc++-6.dll / libgcc_s_seh-1.dll，
#     只需要和 exe 放在一起的 libwinpthread-1.dll（或者用 -static 全静态链接，
#     但部分 MinGW 发行版缺少相应的静态库，全静态会链接失败）。
#     若本机工具链报错，用 -DCHAT_STATIC_RUNTIME=OFF 关闭即可。
# ---------------------------------------------------------------------------
option(CHAT_STATIC_RUNTIME "静态链接 libgcc / libstdc++" ON)
if(MINGW AND CHAT_STATIC_RUNTIME)
  target_link_options(chat_server PRIVATE -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
#  5. 可选扩展：使用 SQLite 保存聊天记录
#     cmake -S . -B build -G "MinGW Makefiles" -DCHAT_USE_SQLITE=ON
#     需要 sqlite3 开发库，例如 MSYS2:
#       pacman -S mingw-w64-x86_64-sqlite3
#     定义后 main.cpp 中第 4.2 节的 SqliteHistoryStore 分支才参与编译。
# ---------------------------------------------------------------------------
option(CHAT_USE_SQLITE "用 SQLite 保存聊天记录（需要 sqlite3 库）" OFF)
if(CHAT_USE_SQLITE)
  find_package(SQLite3 REQUIRED)
  target_link_libraries(chat_server PRIVATE SQLite::SQLite3)
  target_compile_definitions(chat_server PRIVATE CHAT_USE_SQLITE=1)
  message(STATUS "已启用 SQLite 聊天记录后端: ${SQLite3_VERSION}")
endif()

# ---------------------------------------------------------------------------
#  6. 配置完成后的提示
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
#  7. 运行时 DLL：把 MinGW 自带的 libwinpthread-1.dll 复制到 exe 旁边。
#     Windows 加载 DLL 时先搜索「exe 所在目录」，再搜索 PATH；
#     把正确的 DLL 放到 exe 旁边，就不会被系统里别的软件
#     （比如 STM32CubeProgrammer、Anaconda、旧版 Dev-Cpp 等）带来的
#     旧 libwinpthread-1.dll 抢先加载。
# ---------------------------------------------------------------------------
if(MINGW)
  get_filename_component(_chat_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_file(CHAT_WINPTHREAD_DLL
            NAMES libwinpthread-1.dll
            PATHS "${_chat_mingw_bin}"
            NO_DEFAULT_PATH)
  if(CHAT_WINPTHREAD_DLL)
    add_custom_command(TARGET chat_server POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CHAT_WINPTHREAD_DLL}"
              "$<TARGET_FILE_DIR:chat_server>/libwinpthread-1.dll"
      COMMENT "复制 libwinpthread-1.dll 到可执行文件目录")
    message(STATUS "运行时 DLL 已就绪: ${CHAT_WINPTHREAD_DLL}")
  else()
    message(WARNING "未找到 libwinpthread-1.dll，请手动复制到 chat_server.exe 同目录。")
  endif()
endif()

message(STATUS "chat_server 使用的 C++ 标准: C++${CMAKE_CXX_STANDARD}")
message(STATUS "生成的可执行文件: chat_server.exe（位于构建目录中）")
