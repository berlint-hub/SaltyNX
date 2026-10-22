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
SaltyNX 2.1.5: got SD card.
SaltyNX: display-sync endpoint ready (NethFPS, 4 sessions)
SaltyNX: [serviceThreadDisplaySync] waiting for a client on port NethFPS
SaltyNX: [serviceThreadDisplaySync] client PID 141 connected
SaltyNX: [serviceThreadDisplaySync] session finished: 0xf601
```

`0xf601` is `KernelError_ConnectionClosed`: the client closed its session after the reply, which happens once per poll. Only the first few are logged, then only changes.

Failures are logged once per distinct code, so a silent log really means "nothing tried to connect". If `accept` reports `0xf201` (`KernelError_NotFound`) for a full second, the endpoint drops the port handle and registers the name again, logging `port NethFPS re-registered`.

Result codes a client sees when it connects to `NethFPS`:

| Code | Name | Meaning |
| --- | --- | --- |
| `0` | - | connected |
| `0xF201` | `KernelError_NotFound` | no such port: an older sysmodule is running |
| `0x10801` | `KernelError_OutOfResource` | the port exists but all `DISPLAY_SYNC_SESSIONS` are taken |

Note that `OutOfResource` on the *legacy* port is normal while a game runs, because Core owns that session. A client should always report the two connect attempts separately, otherwise a failing `NethFPS` attempt is indistinguishable from the expected busy legacy port.

Do not connect to this port from inside the sysmodule itself. A same-process loopback client (a boot-time self-test, for instance) leaves a session slot that never frees, after which every external client gets `OutOfResource` while `accept` only reports `NotFound`. Build 2.1.4 shipped such a self-test and broke the endpoint this way.

Reference client: `saltysd_display.c` in [NetherSX2_nx](https://github.com/berlint-hub/NetherSX2_nx).

### Verifying the endpoint

Right after boot, `sdmc:/SaltySD/saltynx.log` (written when `SaltySD/flags/log.flag` exists) has to contain the version line and these three lines from the sysmodule itself:

```
SaltyNX 2.1.4: got SD card.
SaltyNX: display-sync endpoint ready (NethFPS)
SaltyNX: display-sync self-test OK (rate 0, 0 before a game runs)
```

The self-test connects to the port at boot and performs one `GetDisplayRefreshRate` request through it, so a working log means the endpoint really serves requests. If the self-test fails, it prints the kernel result code instead.

When a client then connects, `SaltyNX: [serviceThreadDisplaySync] display-sync client connected` shows up. Failures are logged once per distinct code, so a silent log really means "nothing tried to connect".

Result codes a client sees when it connects to `NethFPS`:

| Code | Name | Meaning |
| --- | --- | --- |
| `0` | - | connected |
| `0xF201` | `KernelError_NotFound` | no such port: an older sysmodule is running, or the port could not be created (see the log) |
| `0x10801` | `KernelError_OutOfResource` | the port exists but another client holds its only session |

Note that `OutOfResource` on the *legacy* port is normal while a game runs, because Core owns that session. A client should always report the two connect attempts separately, otherwise a failing `NethFPS` attempt is indistinguishable from the expected busy legacy port.

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
