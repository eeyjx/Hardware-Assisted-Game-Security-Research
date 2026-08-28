# Hardware-Assisted Game Security Research

This repository archives the source code for an undergraduate thesis on game security, process-memory analysis, and visualization.

## Repository layout

| Directory | Description |
| --- | --- |
| [`projects/cs2-dumper-cpp-standalone`](projects/cs2-dumper-cpp-standalone/) | A standalone C++20 tool for analyzing Source 2 processes and exporting runtime structures. |
| [`projects/pwnie-island-research`](projects/pwnie-island-research/) | A local reverse-engineering research framework for the Pwn Adventure 3: Pwnie Island educational client. |
| [`projects/cs2-stage3-esp-radar`](projects/cs2-stage3-esp-radar/) | Source code, tests, and runtime assets for the CS2 Stage 3 ESP and fixed-map radar project. |

See the README in each project directory for build instructions, target environments, and limitations.

## Responsible-use notice

This repository is intended only for thesis work, offline experiments, educational research, and security analysis of software that the researcher owns or is explicitly authorized to examine. Do not use its process-memory reading, injection, hooking, or visualization features in online matches, to evade anti-cheat systems, against unauthorized systems, or in violation of software licenses or terms of service.

## Repository hygiene

The repository contains source code, tests, documentation, and required runtime assets. IDE caches, build directories, logs, backups, `node_modules`, and other reproducible artifacts are excluded from version control.
