<p align="center">
  <img src="res/logo_icon.png" alt="cim logo" width="180">
</p>

<h1 align="center">cim</h1>

<p align="center">
  Command Instant Messenger，一款运行在终端中的实时即时通讯工具。
</p>

![chat](README/chat.jpg)

## 主要功能

- **账号审核与登录**：新账号需要管理员批准后才能登录，密码不会以明文保存。
- **可选记住密码**：登录成功后可按用户选择保存密码并在下次启动时预填，但不会自动登录或直接进入聊天页。
- **一对一实时聊天**：选择联系人后即可发送消息，双方在线时消息会实时送达。
- **消息时间标记**：每条消息上方显示服务端确认的发送日期和时间。
- **在线状态同步**：联系人列表会显示用户当前处于在线或离线状态。
- **服务器连接设置**：可在客户端配置服务器的 IP、域名和端口，保存后自动重新连接。
- **加密传输**：聊天通道强制使用 WSS，并校验服务器证书及主机名。
- **联系人搜索**：通过用户名快速筛选联系人。
- **双栏聊天界面**：左侧管理联系人，右侧集中展示当前会话和消息输入框。
- **键盘优先操作**：支持方向键切换区域和联系人，按 Enter 发送消息。
- **自动重新连接**：网络连接中断后会自动尝试恢复，并在当前运行周期内恢复登录会话。
- **发送状态提示**：连接异常、认证失败、空消息和发送失败会在界面中显示提示。
- **本机服务管理**：服务端提供受密钥保护的本机命令，可审核注册、查看用户、修改密码和删除账号。

## 构建

构建 cim 需要：

- Git
- CMake 3.21 或更高版本
- 支持 C++20 的编译器
- SQLite3
- libsodium
- OpenSSL

获取源码后，先初始化项目依赖：

```bash
git submodule update --init --recursive
```

### Linux

Debian / Ubuntu 可以通过以下命令安装构建环境：

```bash
sudo apt update
sudo apt install build-essential cmake git libsqlite3-dev libsodium-dev libssl-dev
```

先按照下一节生成证书，再把 CA 公共证书路径传给 CMake：

```bash
cmake -B build -S . -DCIM_CA_CERT="$HOME/.local/share/cim/tls/ca.crt"
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
C:\vcpkg\vcpkg.exe install sqlite3:x64-windows libsodium:x64-windows openssl:x64-windows
```

通过 vcpkg 工具链配置并构建 Release 版本：

```powershell
cmake -B build -S . -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DCIM_CA_CERT=C:/secure/cim/ca.crt
cmake --build build --config Release --target all
```

构建产物位于：

```text
build/Release/cim.exe
build/Release/cim-ca.crt
build/Release/cim-server.exe
```

> Linux 构建已经验证。Windows 构建配置已适配 vcpkg，但尚未在 MSVC 环境中实际验证。Windows 上的证书可在可信的 Linux 管理机生成后传入构建。

## TLS 证书

项目使用私有 CA 签发服务器证书。客户端仅携带公开的 `ca.crt`，不得分发 `ca.key` 或 `server.key`。证书输出目录应位于源码仓库之外。

为服务器的实际域名和 IP 生成证书，`--dns` 和 `--ip` 均可重复：

```bash
./scripts/generate-tls-cert.sh \
  --output-dir "$HOME/.local/share/cim/tls" \
  --dns chat.example.com \
  --ip 203.0.113.10
```

如果客户端使用 `127.0.0.1` 连接，证书必须包含对应 SAN：

```bash
./scripts/generate-tls-cert.sh \
  --output-dir "$HOME/.local/share/cim/tls" \
  --dns localhost \
  --ip 127.0.0.1
```

脚本生成以下文件：

```text
ca.crt              客户端信任的 CA 公共证书
ca.key              CA 私钥，仅管理员保存
server.crt          服务器证书
server.key          服务器私钥
server-chain.crt    服务端使用的完整证书链
```

续期会保留原 CA，因此不需要重新分发客户端：

```bash
./scripts/generate-tls-cert.sh \
  --output-dir "$HOME/.local/share/cim/tls" \
  --dns chat.example.com \
  --ip 203.0.113.10 \
  --renew
```

续期后需要重启 `cim-server` 才会加载新证书。新增域名或 IP 时，必须在续期命令中再次列出全部仍需保留的 SAN。

构建会把 CA 公共证书复制到 `cim` 可执行文件旁并命名为 `cim-ca.crt`。发布客户端时必须同时分发这两个文件。客户端 Settings 中可改用系统 CA 或指定其他 CA PEM，也可通过 `cim --ca-cert /path/to/ca.crt` 仅覆盖本次运行使用的 CA；这些方式都不会关闭证书与主机名校验。

从保存 `cim.db` 的工作目录启动服务端：

```bash
./build/cim-server \
  --tls-cert "$HOME/.local/share/cim/tls/server-chain.crt" \
  --tls-key "$HOME/.local/share/cim/tls/server.key"
```

证书应由运行服务端的非 root 专用账户生成和读取；不要仅为读取 root 所有的私钥而以 root 运行服务端。服务端没有证书参数时会拒绝启动。聊天端口 `9001` 仅接受 WSS；本机管理端口 `127.0.0.1:9002` 不暴露到网络，继续使用受 `cim-admin.key` 保护的本机 WebSocket。

## Docker 部署

`Dockerfile` 使用多阶段构建，并通过 `CIM_BUILD_CLIENT=OFF` 只构建服务端。运行阶段使用非 root 用户和只读根文件系统；镜像不包含证书、私钥、数据库或管理密钥。

安装 Docker Engine、Docker Compose、Git 和 OpenSSL 后，以有 Docker 访问权限的普通用户在仓库中运行，无需预先创建网络、目录或证书：

```bash
bash scripts/docker-up.sh
```

脚本自动初始化 Git 子模块，以当前用户的 UID/GID 运行容器，创建仓库下的 `docker-data/` 和 `tls/`，首次生成本地 CA 及服务端证书，然后构建镜像并后台启动。已有数据和证书会复用，不会自动覆盖或续期；不要使用 `sudo` 运行此脚本。

默认服务地址为 `wss://localhost:9001`，证书覆盖 `localhost`、`127.0.0.1` 和 `::1`。可直接指定启动脚本生成的 CA 运行客户端：

```bash
./build/cim --ca-cert ./tls/ca.crt
```

也可在客户端 Settings 中选择自定义 CA 并指向 `tls/ca.crt`，或在构建客户端时通过 `CIM_CA_CERT` 指定它。不要分发 `ca.key` 或 `server.key`。

远程部署时，应按前文证书生成说明为实际域名/IP 准备证书，并通过 `CIM_TLS_DIR` 指向其目录；本地默认证书不适用于任意远程地址。可通过 `CIM_DATA_DIR`、`CIM_UID`、`CIM_GID` 覆盖数据目录及容器身份，目录权限须与指定身份匹配。Compose 只挂载 `server-chain.crt` 和 `server.key`，不会把 CA 私钥放入容器。

查看状态及日志：

```bash
docker compose ps
docker compose logs -f cim-server
```

启动脚本会先校验 Compose 配置，再复用外部 bridge 网络 `dev-net`；新机器上不存在该网络时自动创建，Linux 桥接口命名为 `br-docker`，子网由 Docker 分配。已有网络若不是 bridge 驱动则报错停止，不会删除或重建；已有 bridge 网络即使接口名不同也会保留。脚本可从任意目录调用，项目目录固定为本仓库。

本配置面向可信的开发环境：加入 `dev-net` 的其他项目容器可以直接访问 `cim-server:9001`，不受宿主发布端口的限制；管理端口仍仅监听容器内的 `127.0.0.1:9002`。需要跨项目隔离的生产环境不应直接使用此共享网络配置。同一共享网络上的多个 CIM 实例需要避免服务 DNS 名称冲突。`docker compose down` 不会删除外部共享网络，启动脚本也不会清理其他网络。

脚本接受额外的 `docker compose up` 参数，例如 `bash scripts/docker-up.sh --wait`。`CIM_BUILD_NETWORK` 可选设置构建网络，默认为 `default`，不影响容器运行时的共享 bridge 网络。

主机端口默认为 `9001`，可以在启动前通过 `CIM_PORT` 修改。管理端口 `9002` 不会映射到宿主机，管理命令应在容器内部执行：

```bash
docker compose exec cim-server /app/cim-server pending
docker compose exec cim-server /app/cim-server users
docker compose exec cim-server /app/cim-server approve <username>
```

`cim.db` 和 `cim-admin.key` 保存在 `CIM_DATA_DIR`。迁移已有服务时，应先停止原服务，再把这两个文件复制到数据目录并保持所有者为 `CIM_UID:CIM_GID`。不要同时运行共享同一个数据库的本机服务和容器服务。

证书续期后重启容器：

```bash
docker compose restart cim-server
```

容器收到 `SIGTERM` 后会停止接受连接、关闭 WebSocket 并正常退出。健康检查通过容器内部的本机管理通道执行 `cim-server users`。

## 服务端管理

管理命令要求 `cim-server` 正在运行，并且必须在启动服务端时使用的同一工作目录执行。服务端首次启动时会在该目录生成 `cim-admin.key`，管理通道仅监听 `127.0.0.1:9002`。

查看待审核注册申请：

```bash
./build/cim-server pending
```

批准注册申请：

```bash
./build/cim-server approve <username>
```

拒绝注册申请：

```bash
./build/cim-server reject <username>
```

拒绝时需要先输入用户名确认，再填写不能为空的拒绝原因。拒绝原因会持久保存，因此客户端离线或服务端重启后仍能显示；同名用户重新申请时会清除旧原因。

客户端提交注册后不会立即登录。`cim` 保持运行时会立即收到批准或拒绝通知，网络短暂重连后也会继续监听审核结果。申请被批准后，用户需要使用注册时填写的密码手动登录；被拒绝后可以重新提交申请。待审核申请保留 7 天，服务端最多同时保存 1000 条申请。

列出注册用户、创建时间和实时在线状态：

```bash
./build/cim-server users
```

修改用户密码：

```bash
./build/cim-server passwd <username>
```

新密码会通过隐藏输入读取。修改成功后，该用户的历史会话将失效，在线连接也会立即断开。

删除用户：

```bash
./build/cim-server delete <username>
```

删除前需要再次输入用户名确认。用户被删除后，其历史会话将一并清除，在线连接也会立即断开。

查看管理命令帮助：

```bash
./build/cim-server help
```

## 使用体验

1. 确保 `cim-ca.crt` 位于客户端可执行文件旁，再打开 cim；可以通过 `Server settings` 配置服务器地址、端口和 CA 来源。
2. 选择登录或提交账号注册申请；新账号需要等待服务端管理员批准。登录页可选择 `Remember password`，下次启动只会预填账号和密码，不会自动登录。
3. 登录成功后，从左侧联系人列表选择聊天对象。
4. 在右侧输入消息，按 Enter 发送。
5. 使用左右方向键在联系人区域和聊天区域之间切换。
6. 使用搜索框快速定位联系人，并通过在线状态判断对方是否可以即时接收消息。
7. 使用聊天页顶部的 `SIGN OUT` 撤销当前 session 并返回登录页。

## 当前范围

- 目前支持在线用户之间的一对一实时消息。
- 聊天消息不会保存为历史记录。
- 对方离线时不会保存或补发消息。
- 登录 session 使用 30 天滚动有效期；改密、删号或主动退出会立即撤销对应 session。
- 启用 `Remember password` 会把密码保存在本机客户端配置中；Unix 配置文件权限强制为 `0600`，仍应仅在受信任设备上启用。
- 客户端到服务器使用 TLS 加密，但聊天不是端到端加密，服务端仍可读取在线转发的消息。
- 群聊、文件传输、图片消息和消息撤回尚未提供。
