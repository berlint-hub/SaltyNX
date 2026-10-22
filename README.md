# SaltyNX

Sysmodule for injecting custom codes into retail Nintendo Switch games, by default it injects "Core" which includes many useful features known as NX-FPS (tracking game performance statistics, unlocking FPS) and ReverseNX-RT (switching between docked and handheld graphics profiles). Sysmodule also manages custom refresh rates in both handheld and docked modes.

This sysmodule targets more common approach, so basic compatibility with all games is a priority. If you want to inject your own code for specific game I recommend much more powerful code injection framework - [exlaunch](https://github.com/shadowninja108/exlaunch).

Original sysmodule was created by: https://github.com/shinyquagsire23

This fork includes many new features (aforementioned NX-FPS and ReverseNX-RT), compatibility fixes, and beside plugins support also supports patches for easy way to edit common functions.

![GitHub all releases](https://img.shields.io/github/downloads/masagrator/SaltyNX/total?style=for-the-badge)
---

> [!CAUTION]
> It is required to have FW installed at least 10.0.0 version
> 
> No technical support for anything else than stock Atmosphere.
>
> Using it in online games such as Splatoon 3 can lead to ban as it injects custom code, be aware of that.

> [!IMPORTANT]
> Known issues:
> - Cheats using HEAP+offset address instead of MAIN+offset address chain may not work properly.

Tools utilizing SaltyNX:
- [FPSLocker](https://github.com/masagrator/FPSLocker)
- [ReverseNX-RT](https://github.com/masagrator/ReverseNX-RT)
- [Status Monitor Overlay](https://github.com/masagrator/Status-Monitor-Overlay)
- And more

For additional functions you need [SaltyNX-Tool](https://github.com/masagrator/SaltyNX-Tool)

Patches pattern:
- filename is a function symbol with filetype `.asm64` for 64-bit games, `.asm32` for 32-bit games,
- inside file write with hex editor instructions that you want to put into this function,
- put this file either to `SaltySD/patches` to make it work for every game, or to `SaltySD/patches/*titleid*` to make it work for specific game.

Examples of such patches can be find [HERE](https://github.com/masagrator/SaltyNX-Patches).

# Display sync from homebrew

Homebrew that wants to drive SaltyNX display sync (an emulator keeping the panel at the rate it presents at, for example) can use a second IPC port named `NethFPS`. It is served by its own thread and lives next to the legacy `SaltySD` port, which accepts one client at a time - inside a hijacked process that slot is used by SaltyNX Core while it bootstraps, so a second client running there can get `OutOfResource` when it tries to use `SaltySD` directly.

The port accepts only these commands, with the same wire format as `SaltySD`:

| Command | ID | Reply |
| --- | --- | --- |
| `GetDisplayRefreshRate` | 10 | `magic`, `result`, current refresh rate |
| `GetSharedMemoryHandle` | 7 | Handle of the 0x1000 B shared memory block |

Anything else is answered with `0xEE01`. A client that already speaks `SaltySD` needs no other change than the port name.

After mapping the shared block, a client finds the NX-FPS/FPSLocker block by the `0x465053` magic (it sits at the start of the block) and writes `FPSlocked`, which is byte 10 of that block. SaltyNX then sets the panel to the matching refresh rate while display sync is enabled (`sdmc:/SaltySD/flags/displaysync.flag` or the `SetDisplaySync` command), exactly like FPSLocker does. Note that SaltyNX does not inject Core into homebrew applications, so this only works for homebrew installed and launched as a title.

Reference client: `saltysd_display.c` in [NetherSX2_nx](https://github.com/berlint-hub/NetherSX2_nx).

### Verifying the endpoint

`sdmc:/SaltySD/saltynx.log` (written when `SaltySD/flags/log.flag` exists) shows the endpoint coming up and every client it serves:

```
SaltyNX 2.1.6: got SD card.
SaltyNX: display-sync endpoint ready (NethFPS, 4 sessions, handle 0x...)
SaltyNX: [serviceThreadDisplaySync] waiting for a client on port NethFPS (handle 0x...)
SaltyNX: [serviceThreadDisplaySync] client session #1 accepted
SaltyNX: [serviceThreadDisplaySync] client PID 141 connected
SaltyNX: [serviceThreadDisplaySync] served first command 0xa
SaltyNX: [serviceThreadDisplaySync] session finished: 0xf601
```

`0xf601` is `KernelError_ConnectionClosed`: the client closed its session after the reply, which happens once per poll. Only the first few sessions are logged, then every 60th one, so a client polling once per second does not hammer the SD card.

If no client connects at all, the endpoint says so once per 30 seconds (`no client on port NethFPS yet (endpoint idle on handle 0x...)`). That line proves the worker thread is alive and the port handle is valid, so an idle endpoint can be told apart from a broken one.

The worker waits on the port handle instead of polling `svcAcceptSession`, because of how the kernel actually behaves:

- `svcAcceptSession` returns `KernelError_NotFound` (`0xF201`) whenever no client is queued. That is the normal idle answer, not a failure, and the endpoint never treats it as one. Builds 2.1.4 and 2.1.5 added "recovery" for it, which is what broke the endpoint on device.
- A server port handle is signalled as soon as a client session is queued, so waiting on it is enough.
- The name `NethFPS` in the kernel name list holds its own reference to the client port. Closing the server handle does **not** remove the name: a following `svcManageNamedPort` fails with `KernelError_InvalidState` (`0xFA01`) and clients keep connecting to a port nobody accepts on. The port is therefore created once at boot and never closed while SaltyNX runs. To really remove the name (only done when the worker thread cannot start), `svcManageNamedPort` has to be called with `maxSessions == 0`.

Result codes a client can see when it connects to `NethFPS`:

| Code | Name | Meaning |
| --- | --- | --- |
| `0` | - | connected |
| `0xF201` | `KernelError_NotFound` | no such port: an older sysmodule is running |
| `0xE01` | `KernelError_OutOfSessions` | the port exists but all `DISPLAY_SYNC_SESSIONS` are already accepted and not closed |
| `0x10801` | `KernelError_LimitReached` (libnx: `OutOfResource`) | the name lookup succeeded, but the client process could not reserve a session from its own resource limit: the endpoint is fine, the client is out of session slots |
| `0x10601` | `KernelError_PortClosed` | the port is still named but its server side is gone (a crashed/restarted sysmodule) |

### A client can lose its command id in the raw IPC header

The raw payload of an IPC request does not start at `TLS+0x20`; it starts right after the header (`TLS+0x14` for a request that carries a PID descriptor), and the client declares its length in the second control word. If a client writes its payload at `TLS+0x20` but declares less than `0x0C+16` bytes, the kernel copies only the magic word and the command id never reaches the server.

NetherSX2 664af43 does exactly that for `GetSharedMemoryHandle` (it passes `16` where its own layout needs `32`). The symptom is unmistakable: `GetDisplayRefreshRate` is served normally, while the shared-memory request shows up as an unknown command built from leftover bytes of the previous reply, so the client never maps the NX-FPS block and `active` stays `0`.

Since 2.1.9 the endpoint tolerates it: a request whose raw payload is too short to contain a command id, but which starts with the `SFCI` magic, is treated as `GetSharedMemoryHandle` - the only other command this endpoint serves. The first such request is logged once:

```
[serviceThreadDisplaySync] client raw size 16 B hides the command id, treating it as GetSharedMemoryHandle
```

A client that declares the right size never hits this path.

Both display-sync log lines carry the sysmodule version, so a log can never be attributed to the wrong build again:

```
SaltyNX 2.1.10: display-sync endpoint ready (NethFPS, 4 sessions, handle 0x...)
[serviceThreadDisplaySync] client PID 147 connected (SaltyNX 2.1.10)
```

The version in `saltynx.log` must match the `NethFPS` endpoint you are testing - `saltynx_proc.nsp` (the sysmodule) and `saltynx_core.elf` are two separate files and are easy to update unevenly.

### The hijacked process may only allow one session

Some titles SaltyNX hijacks are created with a session limit of **one** (`LimitableResource_SessionCountMax`). SaltyNX Core occupies that single slot while it bootstraps, so the app living in the same process cannot open *any* IPC session of its own and gets `0x10801` (`KernelError_OutOfResource`) on every connect - even though the endpoint it targets has free sessions. This is what NetherSX2 hit.

Since 2.1.8 Core raises the limit of the process it lives in before it connects to `SaltySD`:

```
SaltyNX Core 2.1.8: session limit 1 -> 32
SaltyNX Core: probe[after-init] pid=144 nethfps=0x0 saltySD=0xe01 sessions=2/32 limitrc=0x0
SaltyNX Core: probe[after-deinit] nethfps=0x0
```

`svcSetResourceLimitLimitValue` only refuses values below the current usage, so raising is always allowed, and every process starts with its own limit object, so no other process is affected. A process with a normal limit is not touched at all.

`saltySD=0xe01` in the probe is expected: the legacy port has a single session slot and Core holds it at that point.

### Diagnosing a client that never connects

2.1.7 makes SaltyNX Core (which runs inside the same process as the embedded app) print one line to `sdmc:/SaltySD/saltynx_core.log` right after it connects to `SaltySD`, and one more after it releases that session:

```
SaltyNX Core: probe[after-init] pid=139 nethfps=0x0 saltySD=0xe01 sessions=1/1 limitrc=0x0
SaltyNX Core: probe[after-deinit] nethfps=0x0
```

- `nethfps=0x0` means the endpoint is reachable from this very process, so a client failure in the same process is not a port problem. `0x10801` there means the process itself cannot allocate another session at that moment.
- `saltySD=0xe01` is the expected answer while Core holds the only session of the legacy port (`KernelError_OutOfSessions`): it calibrates what "port full" looks like on the running kernel.
- `sessions=<current>/<max>` is this process' session accounting (`LimitableResource_SessionCountMax`). If `max` is 1, only one session can exist in the hijacked process at a time, and a client that connects while Core is alive cannot get one.

The `0x10801` case is the important one for diagnostics: the kernel looks the port name up *before* it reserves a session, so this code proves the endpoint exists and is reachable. Nothing on the SaltyNX side can fix it - the client process has to free session slots (or retry) itself.

Note that a session error on the *legacy* `SaltySD` port is normal while a game runs, because SaltyNX Core owns that session. A client should always report the two connect attempts separately, otherwise a failing `NethFPS` attempt is indistinguishable from the expected busy legacy port.

Do not connect to this port from inside the sysmodule itself. A same-process loopback client (a boot-time self-test, for instance) leaves a session slot that never frees, after which external clients fail while the server side looks idle. Build 2.1.4 shipped such a self-test and broke the endpoint this way.

# How to install release:

Download newest version from [RELEASES](https://github.com/masagrator/SaltyNX/releases), look for .zip file with name starting "SaltyNX", unpack it.

Put `atmosphere` and `SaltySD` folders unpacked from zip archive to root of your sdcard.

Remember to restart Switch. 

> [!IMPORTANT]
> In case of any issues after updating already existing SaltyNX setup, remove whole SaltySD folder and put fresh one from release.

---

# Exceptions

<details> 

  <summary>List of titles not compatible only with external plugins, they support all other SaltyNX features</summary>

| Title | Why? |
| ------------- | ------------- |
| Azure Striker Gunvolt: Striker Pack | 32-bit games don't support plugins |
| Baldur's Gate and Baldur's Gate II: Enhanced Editions | 32-bit games don't support plugins |
| CelDamage HD | 32-bit games don't support plugins |
| DEADLY PREMONITION Origins | 32-bit games don't support plugins |
| Death Road to Canada | 32-bit games don't support plugins |
| Dies irae Amantes amentes For Nintendo Switch | 32-bit games don't support plugins |
| Goat Simulator | 32-bit games don't support plugins |
| Gothic | 32-bit games don't support plugins |
| Grandia Collection | Only launcher is 64-bit, actual games are 32-bit, so plugins are not supported |
| LIMBO | 32-bit games don't support plugins |
| Luigi's Mansions 2 HD | 32-bit games don't support plugins |
| Mario Kart 8 Deluxe (1.0.0-3.0.3) | 32-bit games don't support plugins |
| Megadimension Neptunia VII | 32-bit games don't support plugins |
| Moero Chronicle Hyper | 32-bit games don't support plugins |
| Moero Crystal H | 32-bit games don't support plugins |
| Monster Hunter Generations Ultimate | 32-bit games don't support plugins |
| Monster Hunter XX Nintendo Switch Ver. | 32-bit games don't support plugins |
| New Super Mario Bros. U Deluxe | 32-bit games don't support plugins |
| Ni no Kuni: Wrath of the White Witch | 32-bit games don't support plugins |
| Pikmin 3 Deluxe | 32-bit games don't support plugins |
| Planescape: Torment and Icewind Dale | 32-bit games don't support plugins |
| Radiant Silvergun | 32-bit games don't support plugins |
| Sherlock Holmes and The Hound of The Baskervilles | 32-bit games don't support plugins |
| Tokyo Mirage Session #FE Encore | 32-bit games don't support plugins |
| Valkyria Chronicles | 32-bit games don't support plugins |
| World of Goo | 32-bit games don't support plugins |
| YouTube | Unknown |
| 超次次元ゲイム ネプテューヌRe;Birth1 | 32-bit games don't support plugins |
| 超次次元ゲイム ネプテューヌRe;Birth2 SISTERS GENERATION | 32-bit games don't support plugins |
| 神次次元ゲイム ネプテューヌRe;Birth3 V CENTURY | 32-bit games don't support plugins |

</details>

<details>
  <summary>List of games not compatible with SaltyNX at all</summary>

| Title | Why? |
| ------------- | ------------- |
| Witcher 3 GOTY (version 3.2) | heap related |
</details>
Titles in exceptions.txt are treated as part of Blacklist, you can find it in root of repo. SaltyNX reads it from SaltySD folder. Normal titleid blocks only loading plugins, `X` at the beginning of titleid means that SaltyNX won't inject Core at all. `R` at the beginning of titleid means that SaltyNX won't inject Core at all if romfs mod for this game is installed. 32-bit games are ignored by default for plugins.

---

# Thanks to
- `Cooler3D` for sharing code with me how he was changing handheld display refresh rate in his tools that were first publicly available tools allowing this on HOS. I have used that as basis to make my own function.

# Other
`Claude` was used to:
 - cleanup `libnx32_min` from unused files and add files to make it independent from newlib shipped with DevkitARM which was breaking PIE compilation
 - Fix `bootstrap32` code to generate properly position independent ELF
 - `core32` and `bootstrap32` now relocate themselves instead of sysmodule doing that for them (fixing an issue with pointers needed relocating in read only sections)
 - Add support for showing logo via shader
 - Implement my design for triple buffer workaround targeting memory hardcoded double buffer games
