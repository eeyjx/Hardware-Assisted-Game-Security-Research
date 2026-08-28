# Pwnie Island 本地逆向研究框架

这是一个仅针对 **Pwn Adventure 3: Pwnie Island 32 位 Windows 教学客户端**的研究项目。它把已经通过 PDB、反汇编和运行时实验验证的对象关系，封装成一个外部控制台与一个注入 DLL。

当前版本已实现并实机验证：

- 运行时定位 `GameLogic.dll`，所有地址均按 `模块基址 + RVA` 计算；
- 在启用 Hook 前校验六处函数签名字节，不匹配即停止初始化；
- 用 `Player::Tick` 自动识别本地完整 `Player*`，无需先按一次冲刺键；
- 在游戏 Tick 线程读取生命值、位置和移动参数；
- 通过 Tick 邮箱调用 `Actor::SetPosition`，实现可确认、可恢复的传送；
- 临时修改 `Player::CanJump` 与三个已验证移动字段，实现实验性 fly，并在关闭或卸载时恢复；
- Hook `Player::GetSprintMultiplier()`，支持返回值覆盖和在 Tick 线程调用原函数；
- 对 `.rdata` 冲刺常量执行临时页保护修改、回读验证和保护恢复；
- 通过按目标 PID 命名的共享内存向外部控制台发布状态；
- 保留未知值扫描器，扫描范围只包括可写 `MEM_PRIVATE` 页面。

它没有实现攻击功能、反作弊分析/绕过、隐藏、驱动或通用注入器。只应在自己的本地教学游戏环境中使用。

## 文件

- `external_tool.cpp`：32 位外部控制台、注入、共享状态客户端与内存扫描器。
- `research_dll.cpp`：Tick/冲刺 Hook、位置邮箱、fly 状态机与安全卸载。
- `shared_state.hpp`：已验证 RVA、对象偏移、错误码与共享协议（当前版本 4）。
- `FINAL_TUTORIAL_ZH.md`：从二进制识别到实机验证的完整中文步骤。
- `VERIFICATION_20260812.md`：2026-08-12 的可审计运行记录。
- `CMakeLists.txt`：32 位 CMake/Ninja 构建配置，MinGW 运行库静态链接。
- `CMakePresets.json`：VS Code/PowerShell 可直接选择的 MinGW32 Release 预设。

## 已验证的目标构建

```text
GameLogic.dll SHA-256:
8CAEB44F70A4D5F88C957756F6387B7E1C55C8E72F97E09A5B726E9C784D9570

GameLogic.pdb SHA-256:
41B78B15F205382180745FBCA0FFE2FCEB89E0D12A16D1283F5B240E29F96FEF
```

DLL 的 CodeView/RSDS 标识与 PDB 的 GUID/age 已核对匹配。不同 `GameLogic.dll` 构建必须重新验证所有 RVA、签名和对象布局，不能直接复用这里的数值。

## 构建

打开 **MSYS2 MINGW32**，不要使用 UCRT64/MINGW64：

```bash
pacman -S --needed git mingw-w64-i686-toolchain mingw-w64-i686-cmake mingw-w64-i686-ninja
cd /c/Users/JiawenXu/Downloads/test/test2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

也可以在普通 PowerShell 或 VS Code 终端使用项目自带的预设（仍需先将
`C:\msys64\mingw32\bin` 与 `C:\msys64\usr\bin` 放入本次终端的 `PATH`）：

```powershell
$env:PATH = 'C:\msys64\mingw32\bin;C:\msys64\usr\bin;' + $env:PATH
cmake --preset mingw32-release
cmake --build --preset mingw32-release
```

产物：

```text
build/pwnie_external.exe
build/pwnie_research.dll
```

两者必须是 `PE32 Intel 80386`。CMake 会拒绝 64 位配置。MinGW 运行库被静态链接，注入 DLL 不依赖目标进程去寻找 `libgcc_s_dw2-1.dll`、`libstdc++-6.dll` 或 `libwinpthread-1.dll`。

## 最短运行流程

先启动本地游戏并进入可移动的世界，再在 Windows 终端中运行：

```powershell
cd C:\Users\JiawenXu\Downloads\test\test2\build
.\pwnie_external.exe
```

在 `pwnie>` 提示符中依次输入：

```text
read-sprint
inject C:\Users\JiawenXu\Downloads\test\test2\build\pwnie_research.dll
status
player
position
movement
call-internal-sprint
```

只有当 `status` 同时显示下列状态时才继续修改实验：

```text
Hook ready: 1
Initialization error: 0
Signatures verified: 1
Tick hook ready: 1
Sprint hook ready: 1
```

可逆的传送验证：

```text
position
teleport-up 100
position
teleport <原始X> <原始Y> <原始Z>
position
```

可逆的 fly 验证：

```text
movement
fly status
fly on
fly status
fly off
fly status
```

结束时必须正常卸载：

```text
hook-sprint off
fly off
shutdown-dll
status
read-sprint
quit
```

卸载后 `status` 应显示 `Injected DLL state: not available`，冲刺常量应仍为 `3`。`End` 键也可请求 DLL 清理和卸载，但控制台命令更容易观察恢复过程。

## 主要命令

```text
status
read-sprint
write-sprint <float>
inject <pwnie_research.dll 路径>
player
capture-player
position
teleport <x> <y> <z>
teleport-up <delta>
movement
fly status|on|off
fly speed <10..20000>
fly hold <0.2..999999>
fly walk <0..20000>
call-internal-sprint
hook-sprint on <(0,1000]>
hook-sprint off
shutdown-dll
```

输入 `help` 可查看扫描器等全部命令。旧的 `set-coords`/`clear-coords` 已弃用；位置不再依赖猜测的 Player 内直接坐标偏移，而是在 Tick 线程调用已验证的 Actor 方法。

## 关键 RVA 与布局

| 符号/字段 | RVA 或偏移 |
|---|---:|
| `Actor::GetPosition` | `0x16F0` |
| `Actor::SetPosition` | `0x1C80` |
| `Player::GetSprintMultiplier` | `0x13940` |
| 冲刺常量 | `0x78B34` |
| `Player::IsLocalPlayer` | `0x4FEF0` |
| `Player::Tick` | `0x50730` |
| `Player::CanJump` | `0x51680` |
| 完整 `Player*` 到 `IPlayer*` | `+0x70` |
| 生命值 | `Player + 0x30` |
| walking speed | `Player + 0x190` |
| jump speed | `Player + 0x194` |
| jump hold time | `Player + 0x198` |

## 重要限制

- 地址、字段和函数签名只对上面的目标哈希成立；ASLR 使每次模块基址变化。
- fly 是 `CanJump` + 移动参数实验，不是 noclip，也不是服务器端权限或服务端验证结论。
- `CanJump` 目前仍是经过签名门禁、回读与恢复保护的 5 字节全局代码补丁；写入不是指令级原子操作，并可能短暂影响同进程的其他 Player。只应在本地教学会话中短时启用，并在结束前确认补丁状态为 0。
- 传送和 fly 的验证只证明本地客户端调用链与状态改变；不声称绕过网络/服务器检查。
- 不要把该框架用于不属于你的进程、在线对局或无授权系统。
- 修改前先记下原值；用 `fly off`、原坐标回传和 `shutdown-dll` 完成恢复。

详细原理、错误码、完整复现实验和实测输出见 [FINAL_TUTORIAL_ZH.md](FINAL_TUTORIAL_ZH.md) 与 [VERIFICATION_20260812.md](VERIFICATION_20260812.md)。
