# Hardware-Assisted Game Security Research

本科毕业设计代码归档，包含三个面向游戏安全、进程内存分析与可视化的研究项目。

## Repository layout

| Directory | Description |
| --- | --- |
| [`projects/cs2-dumper-cpp-standalone`](projects/cs2-dumper-cpp-standalone/) | 独立的 C++20 Source 2 进程分析与结构导出工具。 |
| [`projects/pwnie-island-research`](projects/pwnie-island-research/) | 针对 Pwn Adventure 3: Pwnie Island 教学客户端的本地逆向研究框架。 |
| [`projects/cs2-stage3-esp-radar`](projects/cs2-stage3-esp-radar/) | CS2 Stage 3 ESP 与本地固定地图 Radar 的源码、测试和运行资源。 |

各子项目的构建方法、目标环境和限制见对应目录中的 README。

## Responsible-use notice

本仓库仅用于毕业设计、离线实验、教学研究以及对本人拥有或已获明确授权的软件进行安全分析。请勿将其中的进程内存读取、注入、Hook 或可视化功能用于在线对局、规避反作弊、未授权系统或任何违反软件许可与服务条款的场景。

## Repository hygiene

仓库只保留源码、测试、文档和必要的运行资源。IDE 缓存、构建目录、日志、备份、`node_modules` 与其他可再生成产物不纳入版本控制。
