# Pwnie Island 逆向教程：从符号验证到可恢复的 Tick 线程运行时

本文记录本项目在 **Pwn Adventure 3: Pwnie Island 32 位 Windows 教学客户端**上的完整实现过程。最终效果不是简单地“写一个地址”，而是建立一条有构建识别、签名门禁、本地玩家识别、游戏线程调用、请求确认和恢复路径的运行时研究链。

最终已经在本地实机完成：

1. 注入 32 位研究 DLL；
2. 自动捕获本地完整 `Player*`，读取生命值 100；
3. 从游戏自己的 `Actor::GetPosition` 获取坐标；
4. 通过 Tick 线程调用 `Actor::SetPosition` 向上移动 100，再传回原坐标并让正常物理稳定；
5. 启用 fly 参数和 `CanJump` 补丁，再关闭并恢复原始字节与原始移动值；
6. 在 Tick 线程调用原始冲刺成员函数并得到 3；
7. 关闭覆盖、卸载 Hook/DLL，确认共享映射消失、冲刺常量保持 3。

> 范围声明：这只是自己的本地教学游戏环境中的二进制研究。项目没有实现攻击功能、刷取/自动化、反作弊产品、反作弊绕过、隐藏或驱动。fly 也不是 noclip，更不构成服务端权限或服务端验证结论。

## 1. 先固定目标构建

RVA、签名字节和对象偏移都与具体构建绑定。本文验证的文件为：

```text
GameLogic.dll SHA-256
8CAEB44F70A4D5F88C957756F6387B7E1C55C8E72F97E09A5B726E9C784D9570

GameLogic.pdb SHA-256
41B78B15F205382180745FBCA0FFE2FCEB89E0D12A16D1283F5B240E29F96FEF
```

DLL 的 CodeView/RSDS 记录和 PDB 的 GUID/age 已核对一致，因此 Binary Ninja 中加载的符号与该 DLL 匹配。可以用 PowerShell 自己复核哈希：

```powershell
$gameDir = 'C:\Users\JiawenXu\Downloads\PwnAdventure3_Windows\PwnAdventure3\PwnAdventure3\Binaries\Win32'
Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $gameDir 'GameLogic.dll')
Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $gameDir 'GameLogic.pdb')
```

如果哈希不同，到此停止。应在新 DLL/PDB 上重新定位符号、重新检查反汇编和对象布局，而不是调整几个地址后冒险运行。

## 2. 用到的工具及各自作用

- **Binary Ninja Free**：加载 `GameLogic.dll.bndb` 和匹配 PDB，确认符号、函数入口、调用约定与字段访问。
- **GNU `objdump`/`file`（MSYS2 MINGW32）**：独立核对入口字节、PE 位数和导入表。
- **CMake + Ninja + MinGW i686**：构建 32 位外部控制台与 32 位注入 DLL。
- **MinHook**：安装 `Player::Tick` 与 `Player::GetSprintMultiplier` Hook。
- **PowerShell/任务与模块枚举**：确认真正的 32 位游戏进程、模块基址、产物哈希与 DLL 卸载状态。
- **Cheat Engine 7.7（可选）**：适合做未知值筛选或动态字段交叉验证；本次最终坐标链不依赖 CE 猜测出的绝对地址。

本次运行中，Binary Ninja 打开的数据库是 `GameLogic.dll.bndb`。游戏启动器和真正的客户端是两个进程；必须选择窗口标题为 `PwnAdventure3 (32-bit, PCD3D_SM5)` 的 `PwnAdventure3-Win32-Shipping.exe`，不要把启动器 PID 当成目标。

## 3. 静态逆向结果

### 3.1 RVA，而不是绝对地址

代码中保存的是 RVA，运行时地址总是：

```text
函数/数据 VA = GameLogic.dll 本次加载基址 + RVA
```

已验证表：

| 对象 | RVA/偏移 | 用途 |
|---|---:|---|
| `Actor::GetPosition` | `0x16F0` | 在 Tick 线程发布坐标 |
| `Actor::SetPosition` | `0x1C80` | 在 Tick 线程执行传送 |
| `Player::GetSprintMultiplier` | `0x13940` | 原始内部调用与返回值 Hook |
| sprint 常量 | `0x78B34` | `.rdata` 中的 `3.0f` |
| `Player::IsLocalPlayer` | `0x4FEF0` | 排除其他玩家/对象 |
| `Player::Tick` | `0x50730` | 稳定的本地游戏线程切入点 |
| `Player::CanJump` | `0x51680` | fly 实验的可恢复 5 字节补丁 |
| `IPlayer` 子对象 | 完整 `Player + 0x70` | 接口 this 指针调整 |
| health | 完整 `Player + 0x30` | 当前生命值 |
| walking speed | 完整 `Player + 0x190` | 移动字段 1 |
| jump speed | 完整 `Player + 0x194` | 移动字段 2 |
| jump hold time | 完整 `Player + 0x198` | 移动字段 3 |

本次运行基址是 `0x69B10000`，因此例如 sprint 常量地址是：

```text
0x69B10000 + 0x78B34 = 0x69B88B34
```

下次启动时 ASLR 很可能改变基址；不要复制本次 VA。

### 3.2 为什么旧代码会得到错误的 Player

`GetSprintMultiplier`/`IsLocalPlayer` 这类接口函数接收的是 `IPlayer*`，而 `Player::Tick` 和 Actor 方法接收完整 `Player*`。本构建中的关系是：

```text
IPlayer* = complete Player* + 0x70
complete Player* = IPlayer* - 0x70
```

旧流程把进入冲刺函数的 ECX 直接当成完整 Player，导致字段整体错位。早期验证中：

```text
IPlayer + 0x30             -> 0
(IPlayer - 0x70) + 0x30    -> 100
```

新流程 Hook `Player::Tick`；其 ECX 天然是完整对象。要验证本地身份时，先计算 `player + 0x70` 再调用 `IsLocalPlayer`。这样捕获不再依赖玩家先按一次冲刺键。

### 3.3 函数签名门禁

在创建任何 Hook 或补丁前，DLL 会核对下列入口：

| RVA | 期望入口/规则 |
|---:|---|
| `0x16F0` | `55 8B EC 8B 49 0C` |
| `0x1C80` | `55 8B EC 8B 55 08` |
| `0x4FEF0` | `33 C0 39 81 48 01` |
| `0x50730` | `55 8B EC 83 E4 C0` |
| `0x51680` | `8B 49 9C 85 C9` |
| `0x13940` | 首字节 `D9 05`，第 7 字节 `C3`，中间绝对操作数必须指向 `base + 0x78B34` |

任一检查失败，`signatures_verified` 不会变成 1，Hook 不会启用。这不是完整的密码学构建认证，所以哈希检查仍然是第一道门；它的作用是在误用 RVA 时快速失败。

## 4. 最终运行时架构

```text
pwnie_external.exe
  |-- 枚举目标 PID 与 GameLogic 基址
  |-- LoadLibraryW 注入 pwnie_research.dll
  |-- 打开 Local\\PwnieIslandResearchState_<PID>
  |-- 写命令邮箱 / 读取确认与遥测
  |
  +--> pwnie_research.dll 生命周期线程
         |-- 验证签名
         |-- MinHook: Player::Tick + GetSprintMultiplier
         |-- 等待 shutdown；卸载前恢复、停 Hook、等活动调用归零
         |
         +--> 游戏自己的 Player::Tick 线程
                |-- IsLocalPlayer(player + 0x70)
                |-- 发布 Player、health、movement
                |-- 处理 fly 与内部 sprint 请求
                |-- 调用原始 Player::Tick
                |-- 处理 teleport 请求
                +-- 调用 GetPosition 并发布结果
```

关键设计是：外部控制台不直接从自己的线程调用游戏成员函数，DLL 生命周期线程也不做这些调用。它们只向共享内存写请求；下一次已确认本地玩家的 Tick 才执行调用。

### 4.1 传送邮箱

外部端把请求号同时当作轻量 seqlock，按如下事务顺序写入：

1. 检查坐标是有限浮点数且位于 `[-200000, 200000]`；
2. 确认 Hook、本地 Player 与最近 Tick 有效，且没有未完成请求；
3. 用比较交换把当前偶数请求号变为奇数，奇数表示生产者正在写三元组；
4. 写 `teleport_x/y/z` 并执行内存屏障；
5. 发布下一个偶数请求号（旧偶数 `+2`）；
6. 最多等待 2 秒，直到 `teleport_completed_id` 等于该偶数请求号。

Tick 忽略零值、奇数写入态和已完成值；在调用原始 `Player::Tick` 后复制最新偶数请求，确认请求号在复制过程中未变化，再调用：

```cpp
Actor::SetPosition(completePlayer, &requestedVector);
```

随后立即用 `Actor::GetPosition` 发布调用后位置；只有三个轴与请求值都相差不超过 2 个单位时才把 `teleport_succeeded` 置 1，最后才写完成号。因此控制台不会混合两个生产者的坐标，也不会把上一 Tick 的旧位置误报成成功结果。邮箱是单槽单未决请求模型，不是多命令队列。

位置发布本身也使用独立的 `position_sequence` seqlock：Tick 写三元组前把序号变成奇数，完整写入后变回偶数；控制台只接受前后序号相同的偶数快照。`teleport-up` 因而不会把两个 Tick 的 X/Y/Z 混成一个目标。

### 4.2 fly 状态机

`fly on` 的默认实验值是：

```text
walking speed  = 800
jump speed     = 999
jump hold time = 99999
```

首次启用时，Tick 线程先保存当前玩家的三个原始字段，再确认 `CanJump` 当前 5 字节仍是：

```text
8B 49 9C 85 C9
```

然后临时改变代码页保护，写入：

```text
B0 01 C3 90 90    ; mov al,1 / ret / nop / nop
```

写后回读并刷新指令缓存。`fly off` 会在同一个仍经 Tick 验证的 Player 上恢复保存的移动值和原始 5 字节；如果切图导致 Player 对象已经变化，则丢弃旧对象快照，绝不向可能已经析构或被复用的旧地址回写，再为新本地 Player 建立快照。卸载流程还会进行最后一次恢复尝试。因此必须观察：

```text
CanJump patch active: 0
Movement snapshot valid: 0
Fly error: 0
```

fly 只是让 `CanJump` 返回真并提高跳跃/移动字段。它没有移除碰撞，不等于真正的自由三轴飞行或 noclip。

当前 `CanJump` 路径仍采用 5 字节全局代码补丁。虽然实现会校验原字节、临时修改页面保护、回读、刷新指令缓存并在失败/关闭/卸载时恢复，但多字节写入本身不是 CPU 指令级原子操作，而且生效期间可能影响同进程的其他 Player。它适合作为这个本地教学客户端中的短时实验，不应被当成通用或零风险的运行时补丁方案；更严格的后续版本可改为按本地 `IPlayer*` 过滤的 detour。

### 4.3 冲刺 Hook 与内部调用

`GetSprintMultiplier` 接收 `IPlayer*`。Hook 只在 `IsLocalPlayer(iplayer)` 为真且覆盖开关已启用时返回用户值；否则调用原函数。

`call-internal-sprint` 也不会从控制台线程直接跨进程调用。控制台置请求位，下一次本地 Tick 用：

```text
complete Player + 0x70 -> IPlayer*
original GetSprintMultiplier(IPlayer*) -> result
```

再增加完成计数并发布结果。

## 5. 解决过的两个主要故障

### 5.1 `LoadLibraryW returned NULL in the target`

常见原因是位数不匹配或 MinGW 运行库无法在游戏进程的 DLL 搜索路径中找到。本项目的解决方式是：

- 确保编译器目标为 `i686-w64-mingw32`；
- CMake 在 `MINGW` 下添加 `-static -static-libgcc -static-libstdc++`；
- 用 `objdump -p` 确认不存在动态 `libgcc`、`libstdc++`、`libwinpthread` 依赖。

不要把 64 位 DLL 注入 32 位进程。

### 5.2 `write-sprint` 返回 Windows Error 998

998 是 `ERROR_NOACCESS`。地址能读出 `3.0f`，但 sprint 常量位于只读 `.rdata`，直接 `WriteProcessMemory` 会失败。

修复后的顺序是：

1. `VirtualQueryEx` 确认页面；
2. `VirtualProtectEx` 临时改为可写；
3. `WriteProcessMemory`；
4. 验证写入字节数；
5. 回读并比较新值；
6. 无论写入结果如何，都恢复原页保护。

这允许教学实验验证常量，但仍应立即写回原值 3。冲刺 Hook 通常比长期修改 `.rdata` 更容易恢复。

## 6. 从零构建

### 6.1 安装 32 位工具链

打开 **MSYS2 MINGW32** 终端：

```bash
pacman -S --needed git mingw-w64-i686-toolchain mingw-w64-i686-cmake mingw-w64-i686-ninja
which cmake
which ninja
which gcc
which g++
g++ -dumpmachine
```

最后一条必须输出：

```text
i686-w64-mingw32
```

注意 `cmake` 的拼写，不是 `camke`。如果把反斜杠用于续行，它必须是行尾最后一个字符；最安全的是直接使用上面的一行安装命令。

### 6.2 配置与编译

```bash
cd /c/Users/JiawenXu/Downloads/test/test2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

如果曾用 64 位生成器配置这个 build 目录，应创建新的 32 位构建目录；不要混用缓存。编译成功后：

```bash
file build/pwnie_external.exe
file build/pwnie_research.dll
objdump -p build/pwnie_research.dll | grep 'DLL Name'
```

预期两者都是 `PE32 executable, Intel 80386`。DLL 导入应只有系统 DLL；不应出现：

```text
libgcc_s_dw2-1.dll
libstdc++-6.dll
libwinpthread-1.dll
```

## 7. 一步一步运行并验证

### 7.1 建立干净基线

1. 启动本地 Pwn Adventure 3 客户端并进入可以移动的世界。
2. 确认目标是 `PwnAdventure3-Win32-Shipping.exe` 的 32 位游戏窗口。
3. 在 PowerShell 启动外部工具：

```powershell
cd C:\Users\JiawenXu\Downloads\test\test2\build
.\pwnie_external.exe
```

4. 先读取未修改的冲刺常量：

```text
pwnie> read-sprint
```

预期 float 是 `3`，原始位模式为 `0x40400000`。

### 7.2 注入并检查门禁

```text
pwnie> inject C:\Users\JiawenXu\Downloads\test\test2\build\pwnie_research.dll
pwnie> status
```

必须同时看到：

```text
Hook ready: 1
Initialization error: 0
Signatures verified: 1
Tick hook ready: 1
Sprint hook ready: 1
```

如果这些条件未满足，不要执行传送或 fly。等待游戏恢复 Tick；如果仍未就绪，参见第 9 节错误表。

### 7.3 确认本地玩家与遥测

```text
pwnie> player
pwnie> position
pwnie> movement
```

Tick 会自动捕获，不需要按冲刺。若世界重载后对象变化，可以：

```text
pwnie> capture-player
```

它清空旧遥测并重新武装；下一次有效本地 Tick 会自动填入新对象。

### 7.4 做一次可逆传送

第一步必须把 `position` 输出完整抄下来。本文实测基线为：

```text
(-53753.8203, -56582.9297, 1110.97229)
```

然后：

```text
pwnie> teleport-up 100
```

最终加固版的首个已发布请求号是 2（奇数 1 只表示写入态）。实测请求 2 在游戏线程完成，位置成为：

```text
(-53753.8203, -56582.9297, 1210.95898)
```

Z 没有恰好停在 `+100.00000`，是因为后续 Tick 的正常物理更新；这正是为什么要读取调用后的实际坐标，而不是假定写入值永久不变。

立即恢复原位置：

```text
pwnie> teleport -53753.8203 -56582.9297 1110.97229
```

实测请求 4 完成；紧随其后的 Tick 读到 Z=`1110.85913`，正常落地物理稳定后为 `1110.97595`，与记录基线 `1110.97229` 相差约 `0.00366`。自己的运行中必须使用自己刚记录的坐标，不要复制本文的坐标；它只属于这一次会话。

### 7.5 做一次可逆 fly 验证

先记录原值：

```text
pwnie> movement
pwnie> fly status
```

实测原值：

```text
walking speed  = 200
jump speed     = 420
jump hold time = 0.2
```

启用并检查：

```text
pwnie> fly on
pwnie> fly status
```

实测状态：

```text
walking speed              = 800
jump speed                 = 999
jump hold time             = 99999
CanJump patch active       = 1
Movement snapshot valid    = 1
Fly update count           = 3
Fly error                  = 0
```

可以在本地短暂用正常移动/跳跃输入观察效果。完成后立刻关闭：

```text
pwnie> fly off
pwnie> fly status
```

实测恢复：

```text
walking speed              = 200
jump speed                 = 420
jump hold time             = 0.2
CanJump patch active       = 0
Movement snapshot valid    = 0
Fly restore count          = 1
Fly error                  = 0
```

只有看到原值、补丁和快照都恢复，才算测试完成。

### 7.6 验证原始内部调用与可选 Hook

```text
pwnie> call-internal-sprint
```

实测输出：

```text
Original Player::GetSprintMultiplier() returned 3
```

若要短暂验证 Hook 返回值：

```text
pwnie> hook-sprint on 10
```

在游戏里触发本地玩家冲刺后，再关闭：

```text
pwnie> hook-sprint off
```

这不会改变 `.rdata` 中原始常量。若教学目标是验证受保护常量写入，也可执行：

```text
pwnie> write-sprint 10
pwnie> read-sprint
pwnie> write-sprint 3
pwnie> read-sprint
```

最后一次必须恢复到 `3 / 0x40400000`。

### 7.7 正常卸载与最终审计

```text
pwnie> hook-sprint off
pwnie> fly off
pwnie> shutdown-dll
```

等待一小段时间，再输入：

```text
pwnie> status
pwnie> read-sprint
```

本次实测：

- PID 专属共享映射已不可用；
- `pwnie_research.dll` 已从目标模块列表卸载；
- sprint 常量仍是 3；
- fly 原值和 `CanJump` 字节已在卸载前恢复。

正常顺序是：先关闭 fly/冲刺覆盖，恢复运行时修改，设置 shutdown，请求 DLL 禁用 Hook，等待活动 Hook 调用数归零，移除 Hook、反初始化 MinHook，最后 `FreeLibraryAndExitThread`。不能在 Hook 仍可能执行时卸载其代码页。

生命周期还采用 fail-closed 策略：MinHook 批量启用若部分失败，必须先证明所有 Hook 已禁用才允许卸载；fly 原值/字节若无法证明恢复，DLL 会保持驻留并报告错误，而不是带着未知补丁状态消失。Hook 禁用后还留出稳定期并等待活动调用归零。超过 2 秒没有本地 Tick 时，公开的 Player 指针和 health/position/movement 有效位会失效，避免菜单或世界切换后继续把旧快照当成当前对象。

## 8. 本次实机证据摘要

以下值是 2026-08-12 单次会话证据，不是下次运行应硬编码的地址：

| 项目 | 实测值 |
|---|---|
| 目标 PID | `12160` |
| `GameLogic.dll` 基址 | `0x69B10000` |
| 注入模块句柄 | `0x68050000` |
| 完整本地 `Player*` | `0x472C3E30` |
| health | `100` |
| 初始位置 | `(-53753.8203, -56582.9297, 1110.97229)` |
| `teleport-up 100` 后 | `(-53753.8203, -56582.9297, 1210.95898)` |
| 传送确认 | 请求 2 完成；请求 4 回传原目标，物理稳定后 Z=`1110.97595`（距基线约 `0.00366`） |
| 初始移动参数 | `200 / 420 / 0.2` |
| fly 参数 | `800 / 999 / 99999` |
| fly 启用状态 | patch 1，snapshot 1，updates 3，error 0 |
| fly 关闭状态 | patch 0，snapshot 0，restores 1，error 0，原值恢复 |
| 内部 sprint 结果 | `3` |
| 卸载后 | 映射不可用、DLL 已卸载、常量 `3` |

完整核验清单见 `VERIFICATION_20260812.md`。

## 9. 错误码和排查

### 9.1 初始化错误 `init_error`

| 值 | 含义 | 应做什么 |
|---:|---|---|
| 0 | 成功 | 仍需确认全部 ready 位为 1 |
| 1 | 找不到 `GameLogic.dll` | 先进入游戏世界；确认目标进程正确 |
| 3 | MinHook 初始化失败 | 卸载旧 DLL、重启客户端后重试 |
| 4 | 创建 Tick Hook 失败 | 检查构建/签名，确认没有残留 Hook |
| 5 | 创建 sprint Hook 失败 | 同上；不要继续运行时命令 |
| 6 | 启用 Hook 失败 | DLL 会清理已创建 Hook；重启并核查环境 |
| 7 | 禁用 Hook 失败 | DLL 不会冒险卸载；退出游戏以完成清理 |
| 20 | `GetPosition` 签名不符 | 目标构建或 RVA 不匹配 |
| 21 | `SetPosition` 签名不符 | 目标构建或 RVA 不匹配 |
| 22 | `IsLocalPlayer` 签名不符 | 目标构建或 RVA 不匹配 |
| 23 | `Player::Tick` 签名不符 | 目标构建或 RVA 不匹配 |
| 24 | `Player::CanJump` 签名不符 | 目标构建、RVA 或代码已被其他工具改动 |
| 25 | sprint 函数/操作数不符 | 函数或数据 RVA 不匹配 |

出现 20–25 时不要删除检查或强行 Hook，应回到哈希、PDB 和反汇编重新验证。

### 9.2 fly 错误 `fly_error`

| 值 | 含义 | 恢复动作 |
|---:|---|---|
| 0 | 无错误 | 正常 |
| 1 | `CanJump` 当前字节不符合预期 | `fly off`；停用其他修改该处的工具 |
| 2 | 修改/恢复代码页或字节验证失败 | `fly off`；若仍非零则关闭游戏完成恢复 |
| 3 | 无法读取原始移动值 | 不启用；等待有效本地 Tick/世界状态 |
| 4 | 无法写入移动值 | 状态机会尝试恢复补丁与快照 |
| 5 | 配置不是有限值或超出范围 | 改用控制台允许范围内的值 |
| 6 | 恢复原始状态失败 | 不继续测试；关闭客户端以丢弃进程内修改 |

### 9.3 其他常见输出

- `LoadLibraryW returned NULL in the target`：优先检查 PE 位数和动态 MinGW 依赖。
- `Error 998`：目标页不可写；使用项目的受保护写入路径，不要绕过恢复步骤。
- `Teleport ... timed out after 2 seconds`：游戏暂停、未进入世界或 Tick 不再运行；不要重复高速提交。
- `Research DLL state is unavailable`：尚未注入、协议版本不匹配，或 DLL 已正常卸载。
- `Hook ready: 0`：初始化尚未完成或正在关闭；不要发运行时修改命令。

## 10. 防守侧能力映射（仅分析，未实现反作弊）

下表是从已经实现的本地研究动作推导出的防守观测面，不包含隐藏或绕过方法。项目**没有实现反作弊产品，也没有实现攻击、刷取或农场自动化**。

| 本地研究能力 | 防守侧可观测信号 | 建议检测/缓解 |
|---|---|---|
| `LoadLibraryW` 注入研究 DLL | 进程模块列表出现非发布模块；映像路径/签名异常；远程线程与模块加载事件相关 | 维护允许模块清单与代码签名策略；记录模块加载来源；将高风险事件组合评分而非仅凭单一模块名封禁 |
| Hook `Player::Tick` / `GetSprintMultiplier` | 函数入口字节或代码页哈希改变；跳转目标落到非游戏模块 | 周期性校验关键函数代码完整性；验证可执行页来源；异常时安全终止敏感会话并保留证据 |
| 修改 `Player::CanJump` 前 5 字节 | `8B 49 9C 85 C9` 变为 `B0 01 C3 90 90`；代码页保护短暂变化 | 校验精确字节/函数哈希；把跳跃授权放在服务器状态机中，而不是只相信客户端布尔结果 |
| 提高 walking/jump/hold 字段 | 速度、加速度、滞空时间或连续跳跃超出角色/装备/状态允许包络 | 服务器按时间积分验证运动包络；采用容差、延迟补偿和分级响应，避免网络抖动造成误报 |
| `SetPosition` 传送 | 单位时间位移、垂直位移或路径穿越不可能；位置与服务器碰撞/权限状态不一致 | 服务器保持位置权威；限制每 Tick 最大位移；检查可达路径、碰撞、传送授权令牌与冷却时间 |

真正的防守验证应在隔离测试环境中测量误报/漏报，并以服务端权威规则为主。仅扫描某个工具名或固定 DLL 名既脆弱，也不能证明行为违规。

## 11. 备份与恢复点

项目中保留了两个只读式人工恢复点：

```text
backup_before_codex_20260806
backup_before_tick_runtime_20260812
```

前者保存早期修复前后的研究基线，后者保存加入 Tick 线程位置/fly 运行时之前的版本。不要在测试期间覆盖游戏原始 `GameLogic.dll`/PDB；本项目的修改发生在目标进程内存中，正常卸载或结束游戏进程后不会写回磁盘模块。

## 12. 验收清单

- [ ] 目标 DLL/PDB 哈希与本文一致。
- [ ] EXE/DLL 都是 PE32 i386，DLL 没有动态 MinGW 运行库依赖。
- [ ] 注入后五个门禁状态为 `1/0/1/1/1`。
- [ ] Tick 自动得到本地完整 Player，health 合理。
- [ ] `position` 连续更新，movement 为有限值。
- [ ] 传送请求有匹配完成号，并已回传原坐标。
- [ ] fly on 时补丁/快照为 1，fly off 后均为 0 且原值恢复。
- [ ] 内部 sprint 调用在 Tick 中完成并返回 3。
- [ ] 冲刺覆盖已关闭；受保护常量若测试过已恢复为 3。
- [ ] `shutdown-dll` 后共享映射不可用、模块已卸载。
- [ ] 没有把本次 PID、模块基址、Player 地址或坐标写成永久常量。

满足以上各项，才算完成了“静态证据 → 构建门禁 → 游戏线程调用 → 可逆实测 → 干净卸载”的最终效果。
