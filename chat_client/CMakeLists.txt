# ============================================================================
#  chat_client/CMakeLists.txt
#  双人聊天室 —— 客户端构建脚本
#  平台: Windows + MinGW-w64 (g++ 13+)      语言: C++23（禁用 C++26）
# ----------------------------------------------------------------------------
#  常用命令:
#    cmake -S . -B build -G "MinGW Makefiles"     # 配置
#    cmake --build build -j                       # 编译
# ============================================================================

cmake_minimum_required(VERSION 3.20)

project(chat_client
        VERSION 1.0.0
        DESCRIPTION "双人 TCP 聊天室客户端"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
#  1. C++ 标准：固定 C++23（-std=c++23，不使用 C++26）
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

add_executable(chat_client main.cpp)

# ---------------------------------------------------------------------------
#  2. 编译选项
#     -D_WIN32 -DWIN32 : 明确声明 Windows 平台（要求：不使用 POSIX 时间库）
#     说明：源码里所有时间/线程都改用 Windows 原生 API
#           （GetSystemTimeAsFileTime / QueryPerformanceCounter / CreateThread /
#             CRITICAL_SECTION / Sleep），不再使用 clock_gettime 系列函数，
#           也不使用 std::chrono / std::thread / std::mutex，
#           因此不会再出现「无法定位程序输入点 clock_gettime64」的错误。
# ---------------------------------------------------------------------------
add_compile_options(-D_WIN32 -DWIN32)
target_compile_options(chat_client PRIVATE -Wall -Wextra)
target_compile_options(chat_client PRIVATE $<$<CONFIG:Release>:-O2>)

# 源码按 UTF-8 处理
if(MINGW)
  target_compile_options(chat_client PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()

# ---------------------------------------------------------------------------
#  3. 依赖库：只链接 ws2_32（Windows Socket 2）
#     不链接 POSIX 线程库 / POSIX 时间库（libpthread、clock_gettime、nanosleep），
#     所以这里不再 find_package(Threads)。
#     客户端不需要 SQLite：聊天记录由服务端保存。
# ---------------------------------------------------------------------------
target_link_libraries(chat_client PRIVATE ws2_32)

# ---------------------------------------------------------------------------
#  4. 可选项：静态链接 MinGW 的 libgcc / libstdc++
#     打开后 exe 只依赖 libwinpthread-1.dll（MinGW 自带的线程运行库）。
#     若本机工具链报错，用 -DCHAT_STATIC_RUNTIME=OFF 关闭即可。
# ---------------------------------------------------------------------------
option(CHAT_STATIC_RUNTIME "静态链接 libgcc / libstdc++" ON)
if(MINGW AND CHAT_STATIC_RUNTIME)
  target_link_options(chat_client PRIVATE -static-libgcc -static-libstdc++)
endif()

message(STATUS "chat_client 使用的 C++ 标准: C++${CMAKE_CXX_STANDARD}")
message(STATUS "生成的可执行文件: chat_client.exe（位于构建目录中）")

# ---------------------------------------------------------------------------
#  5. 运行时 DLL：把 MinGW 自带的 libwinpthread-1.dll 复制到 exe 旁边。
#     Windows 加载 DLL 时先搜索「exe 所在目录」，再搜索 PATH；
#     把正确的 DLL 放到 exe 旁边，就不会被系统里其它软件的旧版
#     libwinpthread-1.dll 抢先加载（这是 clock_gettime64 报错的常见诱因）。
# ---------------------------------------------------------------------------
if(MINGW)
  get_filename_component(_chat_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_file(CHAT_WINPTHREAD_DLL
            NAMES libwinpthread-1.dll
            PATHS "${_chat_mingw_bin}"
            NO_DEFAULT_PATH)
  if(CHAT_WINPTHREAD_DLL)
    add_custom_command(TARGET chat_client POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CHAT_WINPTHREAD_DLL}"
              "$<TARGET_FILE_DIR:chat_client>/libwinpthread-1.dll"
      COMMENT "复制 libwinpthread-1.dll 到可执行文件目录")
    message(STATUS "运行时 DLL 已就绪: ${CHAT_WINPTHREAD_DLL}")
  else()
    message(WARNING "未找到 libwinpthread-1.dll，请手动复制到 chat_client.exe 同目录。")
  endif()
endif()
