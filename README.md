<p align="center">
  <img src="res/logo_icon.png" alt="cim logo" width="180">
</p>

<h1 align="center">cim</h1>

<p align="center">
  Command Instant Messenger，一款运行在终端中的实时即时通讯工具。
</p>

![chat](README/chat.jpg)

## 主要功能

- **账号注册与登录**：可以直接创建账号或使用已有账号登录，密码不会以明文保存。
- **一对一实时聊天**：选择联系人后即可发送消息，双方在线时消息会实时送达。
- **在线状态同步**：联系人列表会显示用户当前处于在线或离线状态。
- **联系人搜索**：通过用户名快速筛选联系人。
- **双栏聊天界面**：左侧管理联系人，右侧集中展示当前会话和消息输入框。
- **键盘优先操作**：支持方向键切换区域和联系人，按 Enter 发送消息。
- **自动重新连接**：网络连接中断后会自动尝试恢复，并在当前运行周期内恢复登录会话。
- **发送状态提示**：连接异常、认证失败、空消息和发送失败会在界面中显示提示。

## 构建

构建 cim 需要：

- Git
- CMake 3.21 或更高版本
- 支持 C++20 的编译器
- SQLite3
- libsodium

获取源码后，先初始化项目依赖：

```bash
git submodule update --init --recursive
```

### Linux

Debian / Ubuntu 可以通过以下命令安装构建环境：

```bash
sudo apt update
sudo apt install build-essential cmake git libsqlite3-dev libsodium-dev
```

配置并构建：

```bash
cmake -B build -S .
cmake --build build --target all
```

构建产物位于：

```text
build/cim
build/cim-server
```

### Windows

Windows 需要 Visual Studio 2022 的“使用 C++ 的桌面开发”组件，并使用
[vcpkg](https://github.com/microsoft/vcpkg) 安装系统依赖：

```powershell
C:\vcpkg\vcpkg.exe install sqlite3:x64-windows libsodium:x64-windows
```

通过 vcpkg 工具链配置并构建 Release 版本：

```powershell
cmake -B build -S . -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --target all
```

构建产物位于：

```text
build/Release/cim.exe
build/Release/cim-server.exe
```

> Linux 构建已经验证。Windows 构建配置已适配 vcpkg，但尚未在 MSVC 环境中实际验证。

## 使用体验

1. 打开 cim 后选择登录或创建账号。
2. 登录成功后，从左侧联系人列表选择聊天对象。
3. 在右侧输入消息，按 Enter 发送。
4. 使用左右方向键在联系人区域和聊天区域之间切换。
5. 使用搜索框快速定位联系人，并通过在线状态判断对方是否可以即时接收消息。

## 当前范围

- 目前支持在线用户之间的一对一实时消息。
- 聊天消息不会保存为历史记录。
- 对方离线时不会保存或补发消息。
- 群聊、文件传输、图片消息和消息撤回尚未提供。
