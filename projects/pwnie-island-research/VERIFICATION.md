# Verification report

Verified on the local 32-bit Pwn Adventure 3 client using `GameLogic.dll`:

```text
SHA-256: 8CAEB44F70A4D5F88C957756F6387B7E1C55C8E72F97E09A5B726E9C784D9570
```

## Build

- `pwnie_external.exe`: PE32 Intel i386
- `pwnie_research.dll`: PE32 Intel i386
- No dynamic `libgcc`, `libstdc++`, or `libwinpthread` dependency
- DLL imports: `KERNEL32.dll`, `msvcrt.dll`, `USER32.dll`

## Runtime results

The repaired DLL initialized successfully and MinHook reported ready.

Protected sprint write:

```text
Before: 3 / 0x40400000
Write:  10 / 0x41200000 (read-back verified)
After:  3 / 0x40400000 (restored and read-back verified)
```

Player base correction:

```text
Incoming IPlayer*:       0x488F2CD8
Complete Player*:        0x488F2C68  (IPlayer - 0x70)
Complete Player + 0x30:  100
Old IPlayer + 0x30:      0
```

The first 0x400 bytes of the complete Player object produced 256 aligned
candidates. Candidate `+0x30` was `100`. Calling the original internal
`Player::GetSprintMultiplier()` with the required reverse `+0x70` adjustment
returned `3`.

Normal shutdown removed the PID-specific shared mapping; a later `status`
reported that injected DLL state was unavailable.

## Remaining limitation

Coordinate values are not proven to be three direct Player fields. The direct
offset and float-scan commands are research aids; implement the verified
engine-actor pointer path before treating coordinates as stable.
