# 运行验证报告 — 2026-08-12

## 结论

本次在本地 32 位 Pwn Adventure 3 教学客户端上完成了端到端验证：构建识别、32 位静态运行库产物、DLL 注入、签名门禁、Tick Hook、本地完整 Player 捕获、位置读取、可逆传送、可逆 fly、游戏线程内部冲刺调用和干净卸载全部通过。

验证结束状态是干净的：传送已回到原坐标，移动字段已恢复，`CanJump` 补丁已移除，冲刺覆盖关闭，PID 专属共享映射不可用，研究 DLL 已卸载，sprint 常量仍为 3。

本报告只证明下面列出的本地客户端行为。未实现或验证攻击功能、刷取/自动化、反作弊产品、反作弊绕过、noclip 或服务端权限。

## 1. 目标身份

```text
真正游戏进程:
  PID: 12160
  Image: PwnAdventure3-Win32-Shipping.exe
  Window: PwnAdventure3 (32-bit, PCD3D_SM5)

启动器进程:
  PID: 8684
```

测试明确选择 PID 12160，而非启动器。该会话中：

```text
GameLogic.dll base: 0x69B10000
GameLogic.dll size: 671744 bytes
```

目标文件哈希：

```text
GameLogic.dll
SHA-256  8CAEB44F70A4D5F88C957756F6387B7E1C55C8E72F97E09A5B726E9C784D9570

GameLogic.pdb
SHA-256  41B78B15F205382180745FBCA0FFE2FCEB89E0D12A16D1283F5B240E29F96FEF
```

DLL 的 CodeView/RSDS GUID/age 与该 PDB 匹配。Binary Ninja 打开的 `GameLogic.dll.bndb` 使用的是这组符号。

说明：PID、模块基址、模块句柄和 Player 指针是单次运行值，受重启与 ASLR 影响；哈希、RVA 和经验证的布局才是该构建的持久证据。

## 2. 构建产物

验证文件：

```text
C:\Users\JiawenXu\Downloads\test\test2\build\pwnie_external.exe
C:\Users\JiawenXu\Downloads\test\test2\build\pwnie_research.dll
```

本次验证文件的 SHA-256：

```text
pwnie_external.exe
D8044FD67F7CDA8E79EFBD7FF62F37D453BA1197D37B71F24D22E7D5637DC9D6

pwnie_research.dll
FE96B54606359DC2EF4BBCF0B6A7B4CA06492EEC8A5B59CE35D3D90A7111A51B
```

若重新构建，编译器元数据可能使产物哈希变化；此处哈希用于标识实际参与本次运行验证的二进制。

`file` 结果：

```text
pwnie_external.exe: PE32 executable for MS Windows 4.00 (console), Intel i386, 17 sections
pwnie_research.dll: PE32 executable for MS Windows 4.00 (DLL), Intel i386, 17 sections
```

研究 DLL 导入：

```text
KERNEL32.dll
msvcrt.dll
USER32.dll
```

未导入 `libgcc_s_dw2-1.dll`、`libstdc++-6.dll` 或 `libwinpthread-1.dll`，证明 MinGW 运行库已按项目配置静态链接。这消除了此前目标进程找不到 MinGW 依赖、导致 `LoadLibraryW returned NULL` 的主要风险。

复核命令：

```bash
cd /c/Users/JiawenXu/Downloads/test/test2
file build/pwnie_external.exe build/pwnie_research.dll
objdump -p build/pwnie_research.dll | grep 'DLL Name'
```

## 3. 构建绑定的静态证据

| 目标 | RVA | 注入前签名门禁 |
|---|---:|---|
| `Actor::GetPosition` | `0x000016F0` | `55 8B EC 8B 49 0C` |
| `Actor::SetPosition` | `0x00001C80` | `55 8B EC 8B 55 08` |
| `Player::GetSprintMultiplier` | `0x00013940` | `D9 05 <base+0x78B34> C3` |
| sprint 数据 | `0x00078B34` | 原值 `3.0f / 0x40400000` |
| `Player::IsLocalPlayer` | `0x0004FEF0` | `33 C0 39 81 48 01` |
| `Player::Tick` | `0x00050730` | `55 8B EC 83 E4 C0` |
| `Player::CanJump` | `0x00051680` | `8B 49 9C 85 C9` |

对象布局：

```text
complete Player* + 0x70 = IPlayer*
complete Player* + 0x30 = health
complete Player* + 0x190 = walking speed
complete Player* + 0x194 = jump speed
complete Player* + 0x198 = jump hold time
```

在本次基址 `0x69B10000` 下，运行地址映射为：

| 目标 | 本次 VA |
|---|---:|
| `Actor::GetPosition` | `0x69B116F0` |
| `Actor::SetPosition` | `0x69B11C80` |
| `GetSprintMultiplier` | `0x69B23940` |
| sprint 数据 | `0x69B88B34` |
| `IsLocalPlayer` | `0x69B5FEF0` |
| `Player::Tick` | `0x69B60730` |
| `Player::CanJump` | `0x69B61680` |

这些 VA 只用于解释该次证据，代码实际使用模块基址加 RVA。

## 4. 注入和初始化

注入前状态是干净的：目标未加载 `pwnie_research.dll`，PID 专属共享映射不存在，外部控制台以 sprint 地址 `0x69B88B34` 读得 3。

执行：

```text
inject C:\Users\JiawenXu\Downloads\test\test2\build\pwnie_research.dll
```

结果：

```text
Injected module handle: 0x68050000
Research DLL initialized. Signature checks passed; Tick and sprint hooks are ready.
```

初始化验收：

```text
Hook ready: 1
Initialization error: 0
Signatures verified: 1
Tick hook ready: 1
Sprint hook ready: 1
```

结论：目标构建签名全部通过，两个 MinHook 已创建并启用，共享状态协议版本 4 可读。

## 5. 本地 Player 和遥测

Tick Hook 自动过滤本地玩家，无需冲刺触发。结果：

```text
Complete local Player*: 0x472C3E30
IPlayer* (derived):      0x472C3EA0
Health address:          0x472C3E60
Health:                  100
```

基线位置：

```text
X = -53753.8203
Y = -56582.9297
Z = 1110.97229
```

基线移动字段：

```text
walking speed  = 200
jump speed     = 420
jump hold time = 0.2
```

结论：完整对象调整正确，health、位置和移动字段均在本地 Tick 中持续发布；所有 float 有限且符合基线预期。

## 6. 可逆传送验证

### 6.1 向上移动

命令：

```text
teleport-up 100
```

确认：

```text
Teleport request 2 completed on the game thread.
Result = (-53753.8203, -56582.9297, 1210.95898)
teleport_request_id   = 2
teleport_completed_id = 2
teleport_succeeded    = 1
```

目标 Z 是基线加 100。实际发布 Z 为 `1210.95898`，与目标相差来自后续正常游戏 Tick/物理更新；X/Y 保持不变。请求号兼作 seqlock：奇数 1 是生产者写入态，偶数 2 才是完整发布请求。完成号只在 `Actor::SetPosition` 调用、调用后位置发布和 ±2 单位回读验证后写入。

### 6.2 回滚

命令：

```text
teleport -53753.8203 -56582.9297 1110.97229
```

确认：

```text
Teleport request 4 completed on the game thread.
Immediate Tick result = (-53753.8203, -56582.9297, 1110.85913)
Settled result        = (-53753.8203, -56582.9297, 1110.97595)
teleport_request_id   = 4
teleport_completed_id = 4
teleport_succeeded    = 1
```

结论：`Actor::SetPosition` 的完整 Player this 指针和 `Vector3*` 参数正确，事务邮箱确认生效。回传后普通重力/碰撞 Tick 继续更新 Z；稳定值与原始基线只相差约 `0.00366`，远小于传送确认的 ±2 单位容差。

## 7. 可逆 fly 验证

### 7.1 启用前

```text
movement = 200 / 420 / 0.2
CanJump patch active = 0
Movement snapshot valid = 0
Fly error = 0
```

### 7.2 启用

命令：

```text
fly on
fly status
```

结果：

```text
Fly enabled = 1
movement = 800 / 999 / 99999
CanJump patch active = 1
Movement snapshot valid = 1
Fly update count = 3
Fly error = 0
```

补丁转换：

```text
before: 8B 49 9C 85 C9
active: B0 01 C3 90 90
```

结论：原值快照已建立，三个移动字段在本地 Tick 中生效，`CanJump` 补丁写入和回读验证通过。

### 7.3 关闭与恢复

命令：

```text
fly off
fly status
```

结果：

```text
Fly enabled = 0
movement = 200 / 420 / 0.2
CanJump patch active = 0
Movement snapshot valid = 0
Fly restore count = 1
Fly error = 0
```

结论：原始字段与原始 5 字节均恢复，快照已清除。fly 只验证本地 `CanJump` 和移动参数路径，不代表 noclip 或服务器认可。

## 8. sprint 验证

在 Tick 线程排队调用：

```text
call-internal-sprint
```

结果：

```text
Original Player::GetSprintMultiplier() returned 3
```

这验证了完整 `Player + 0x70 -> IPlayer*` 调整和原始 `__thiscall` 调用路径。

早期受保护写入的回归结果也已验证：

```text
Before: 3  / 0x40400000
Write:  10 / 0x41200000, read-back verified
After:  3  / 0x40400000, restored and read-back verified
```

此前直接写 `.rdata` 曾返回 Windows Error 998（`ERROR_NOACCESS`）。当前实现通过临时页保护、写入、回读和恢复保护解决。最终会话保持常量为 3。

## 9. 干净卸载验证

执行顺序：

```text
hook-sprint off
fly off
shutdown-dll
```

卸载路径先关闭两个覆盖，等待/强制恢复 fly，随后禁用 Hook、等待活动 Hook 调用数归零、移除 Hook、反初始化 MinHook并退出 DLL 工作线程。

卸载后证据：

```text
status -> Injected DLL state: not available
目标模块列表 -> pwnie_research.dll 不存在
read-sprint -> 3 / 0x40400000
```

结论：共享映射已关闭、研究模块已卸载、运行时补丁无残留、原始数据值保持不变。

最终生命周期加固重建后又执行了一次烟雾测试：向当前基线坐标提交 no-op 传送，请求 2 成功确认；fly on 显示配置值与补丁生效，fly off 恢复基线值和原始字节；随后正常卸载，共享状态消失且 sprint 仍为 3。该烟雾测试对应上面记录的最终 DLL 哈希。

## 10. 失败判据

下列任一情况均应视为未通过，而不是继续尝试修改：

- 目标 DLL/PDB 哈希不符；
- 产物不是 PE32 i386；
- 存在动态 MinGW 运行库导入；
- `init_error != 0` 或任一 ready/signature 位不符合预期；
- Player/health/坐标/移动遥测无效或非有限值；
- 传送请求号没有对应完成号；
- fly off 后补丁或快照仍为 1，或原值未恢复；
- `fly_error != 0`；
- shutdown 后共享映射或模块仍存在；
- sprint 常量不是 3。

遇到签名错误 20–25 时应重新做构建识别和静态分析，不能删除签名检查。遇到 fly 恢复错误 6 时应停止实验并关闭游戏进程，以丢弃所有仅存在于内存中的修改。

## 11. 恢复点

源代码恢复目录：

```text
C:\Users\JiawenXu\Downloads\test\test2\backup_before_codex_20260806
C:\Users\JiawenXu\Downloads\test\test2\backup_before_tick_runtime_20260812
```

原始 `GameLogic.dll`/PDB 未被修改。项目中的运行时变更只作用于当前目标进程，按上述流程卸载或结束客户端进程后消失。
