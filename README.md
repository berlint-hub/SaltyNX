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

### Logging is opt-in and has two sizes

SaltyNX wrote a line to the SD card for every IPC call the game made - every `fopen`, `fread`, `memcpy`, every directory scan - plus a `stat()` per line to see whether logging was even enabled. During gameplay that is thousands of writes to the same card the game streams its ISO from, which shows up in the frame pacing the log is meant to diagnose, and it makes the log itself unreadable.

Since 2.1.19:

| flag file | what is written |
| --- | --- |
| neither | nothing |
| `sdmc:/SaltySD/flags/log.flag` | decisions, warnings, boot information, NX-FPS/ReverseNX lines, one line every 60 s for the display-sync endpoint |
| `sdmc:/SaltySD/flags/log.full.flag` | everything, including one line per IPC call |

The ordinary flag now gives a log that fits on a screen for a whole session. The full flag is still there for the "why did this client not connect" case; it just has to be asked for.

```
sdmc:/SaltySD/flags/log.flag        -> SaltyNX 2.1.19: display-sync endpoint ready ...
                                       display-sync: refresh rate 60 -> 50 Hz requested (target held 5003 ms, active 50 Hz)
```

The flags are sampled once a minute instead of once per line, so creating or deleting one takes effect within a minute rather than instantly - the stat() that used to check it per line was itself part of the problem.

`sdmc:/SaltySD/saltynx_core.log` is gated by the same `log.flag` and is unaffected; the core writes a few dozen lines per launch, not thousands.

### Do not follow a jittery target every second

`updateRefreshRateForApp()` follows `NxFpsSharedBlock::FPSlocked` and calls `SetDisplayRefreshRate()` as soon as the value differs. That is correct for FPSLocker, where the value is a stable user choice, but a client that derives the target from a *measured* frame cadence (NetherSX2 does) wobbles by a few Hz every second - 45/50/55 around a 50 FPS game.

Each of those calls is a real panel mode switch. The picture hitches, the hitch perturbs the measured cadence, and the mismatch feeds itself, so the game feels like it keeps slowing down.

Since 2.1.11 no correction is applied immediately. Since 2.1.12 the wait depends on the direction, because a flat hysteresis only slows a feedback loop down instead of breaking it - a 5 s window on both directions turned five switches in nine seconds into two switches in eleven seconds, still 45 before 50:

| correction | wait before the panel follows |
| --- | --- |
| large drop, e.g. 60 -> 50 when a PAL game starts | 1.5 s |
| any rise, e.g. 45 -> 50 or 50 -> 60 | 5 s |
| small drop, e.g. 50 -> 45 | 20 s |

Only a large drop is allowed to act quickly. A large *rise* gets the slow treatment because a client can publish 60 for a single sample right after a gameplay stretch, and the panel would visibly jump to 60 and straight back.

A small drop is almost always one slow frame being measured, never a new intent, and it is the expensive direction - the panel drops a mode and the picture gets slower. A small rise restores the rate the app actually wants, so it is allowed to happen quickly. Applied switches report both the requested and the effective rate, because the panel clamps (25 Hz is served as 50 Hz):

```
SaltyNX: display-sync: refresh rate 50 -> 45 Hz requested (target held 20004 ms, active 45 Hz)
```

Note that this only stops the flapping. Smooth pacing still needs the *client* to present in lockstep with the panel: with Vsync off (`swap interval 0`, immediate present) the app renders slightly faster or slower than the panel and one frame per few seconds is dropped or repeated. `present_fps` close to `display_rate` in the client diagnostic is not proof of lockstep - a free-running 50 FPS loop on a 45 Hz panel also reports ~50.

### Fine mode: follow the measured rate to the hertz

The 2.1.20 sweep proved the panel takes every integer rate from 45 to 52 exactly, one hertz apart. That makes a real 1 Hz sync possible - but not through the client, which quantises its target to 5 Hz. The sysmodule already has a measured source of its own: `FPSavg` in the shared block, written on every presented frame by NX-FPS, or by the client itself when the NX-FPS hook does not run in that process.

**On by default since 2.1.26.** The window is the whole 45..60 Hz, one hertz apart, and the measured path is what makes 45-49 reachable at all.

```
sdmc:/SaltySD/flags/sync_fine.flag        (optional - it is on without this)
sdmc:/SaltySD/flags/sync_fine_off.flag    (create to fall back to the client target)
```

```
SaltyNX: fine sync enabled by default, following the measured rate to the hertz
SaltyNX: display-sync: refresh rate 50 -> 49 Hz requested (target held 4008 ms, active 49 Hz, measured)
```

Everything else still applies, with two exceptions:

| | normal mode | fine mode |
| --- | --- | --- |
| target source | client's `FPSlocked` (5 Hz table) | NX-FPS `FPSavg` (measured, 1 Hz) |
| floor | 45 Hz (the panel's own minimum) | 45 Hz (the panel's own minimum) |
| deadband | 1 Hz | 0 - a one hertz change is the point |
| rise / drop / large | 5 s / 20 s / 1.5 s | 4 s / 4 s / 1.5 s |
| measurement | client's number, used as is | one sample a second, jumping ones refused, the rest smoothed |
| target deadband | 1 Hz | on the *output*: rises past 0.55 Hz, falls only past 1.8 Hz |
| lock-in test | armed on a drop to the floor | disabled (the loop is the point) |

Values outside 30..240 fps are ignored: `FPSavg` starts at 255 in NX-FPS and is zero before the first presented frame, so "not measuring yet" must not move the panel.

A client publishes that number as a whole hertz, once a second, from a window of sixteen frames. A single hitch - a stalled worker thread, a disc read that blocked for 2.6 s - inflates the mean interval enough to report five hertz low for one second and then snap back. Fine mode cannot build a one-hertz decision on that, so it takes one sample a second, refuses any that jump more than 2 Hz, and averages the rest with a 0.35 coefficient (about three seconds). Ten refusals in a row is not noise any more - that is a new scene or a new app - so the average restarts from the new value instead of sitting on a stale one. That is also what stops a lock from surviving a scene change.

Samples the client cannot produce at all - `FPSavg` of 0 before it publishes anything, or 14..29 for a second after a multi-second stall - are skipped rather than allowed to wipe the average. Wiping it was a real misbehaviour: the target fell back to the client's 5 Hz path, floor and probe included, and the panel then followed a number that had nothing to do with the scene. Giving up the average now takes ten unusable samples in a row, about nine seconds.

Fine mode is only as good as the measurement behind it, and that is the open question. `FPSavg` is written from NX-FPS's present hook, which is installed by rewriting the game's import table - a game that resolves `vkQueuePresentKHR` at runtime, the way a Vulkan wrapper does, never calls the hooked copy. When that happens the shared field keeps its initial 255 and fine mode silently does nothing, which looks exactly like "the panel just did not want to move".

Since 2.1.22 the answer is printed while the flag is present (every ten seconds, every thirty from 2.1.23):

```
SaltyNX: fine: block=found FPS=0 FPSavg=50.00 smooth=50.13 target=50 panel=50
SaltyNX: fine: block=found FPS=0 FPSavg=0.00 smooth=0.00 target=0 panel=50
```

`FPSavg=0.00` means the hook never ran and fine mode has no measurement to follow. `target=0` is the consequence - the normal path takes over, floor and all. Since 2.1.23 that is also said once, instead of leaving it to be inferred (thirty seconds in, from 2.1.25):

```
SaltyNX: fine: no measurement after 30 s (FPS=0 FPSavg=0.00) - nothing is publishing FPSavg for this app, falling back to the client target
```

**Confirmed on the device, both ways.** Before the client patch NetherSX2 reported `block=found FPS=0 FPSavg=0.00` for a whole session. NX-FPS rewrites the game's import table, and a Vulkan wrapper resolves `vkQueuePresentKHR` through `vk_icdGetInstanceProcAddr` instead, so the hooked copy is never called. The NX-FPS fields are therefore free real estate in that process, and the client can fill them: writing its own measured rate into `FPSavg` (offset 5 of the packed block) is a three-line change on the client side and makes fine mode work unchanged. With that patch in place the same log reads `FPSavg=50.00 target=50`, the panel moves 60 -> 50 Hz in 1556 ms with the `, measured` suffix, and then stays there: over fifteen minutes of Gran Turismo 3 the one-second samples wandered between 44 and 51 while the panel never left 50.

This is the experiment, not a default. It can drop the panel below 50, which is exactly what the normal mode forbids, and it follows a measurement that includes the panel's own influence - the loop that made 2.1.16 necessary still exists here, it is just being asked to do something useful instead of something harmful.

### Testing whether the panel takes a rate the clients cannot ask for

Neither FPSLocker nor NetherSX2 can publish anything outside their own 5 Hz table, so "does this panel lock at 49 Hz?" cannot be answered from the client side. The sysmodule can answer it itself: put a list of rates in

```
sdmc:/SaltySD/flags/test_rates.txt        45 46 47 48 49 50 51 52
```

and reboot. While an application is running, SaltyNX steps through the list five seconds apart, printing what the PLLD registers report back:

```
SaltyNX: sweep: 8 rate(s) from sdmc:/SaltySD/flags/test_rates.txt, 5 s apart
SaltyNX: sweep: 48 Hz requested from 47 Hz, active 48 Hz
SaltyNX: sweep: 51 Hz requested from 50 Hz, active 51 Hz
SaltyNX: sweep: 49 Hz requested from 48 Hz, active 48 Hz  <-- did not land
SaltyNX: sweep: done, back to 60 Hz (active 60 Hz)
```

`active` is read back from the PLLD, so it says what the panel is really doing. `did not land` means the panel accepted the write but reports a different rate; `refused` means `SetDisplayRefreshRate()` itself returned false. Without the file nothing happens, so it cannot be left on by accident and it cannot fight FPSLocker.

Display sync is suspended for the duration of the sweep and resumes afterwards, and the sweep is skipped in docked mode where a different set of modes applies.

What this does **not** test: the sync loop itself at 1 Hz granularity. That needs a client that publishes 48/49/51/52, and NetherSX2 quantises to 5 Hz. The sweep proves the panel side of the question - whether the hardware and the PLLD solver can hit those rates at all.

### The handheld window is 45..60, one hertz apart

2.1.18 clamped the automatic path to 50/55/60 to avoid the feedback loop described below: a slow scene publishes 45, the panel drops to 45, the compositor then paces the app at 45, and from that point a genuine 45 FPS game and a panel holding the app down are the same thing. Refusing to enter that state was cheaper than escaping it.

2.1.26 removes the clamp, because the loop now has an exit. The window is the panel's own 45..60, one hertz apart, and everything in it is reachable:

```c
#define DISPLAY_SYNC_MIN_RATE 45u
```

Only the client's own quantised target is clamped - a client that publishes 25 gets the floor:

```
SaltyNX: display-sync: ignoring the 25 Hz target, this sync never goes below 45 Hz
```

A rate set by hand - FPSLocker, or the legacy `SetDisplayRefreshRate` path NetherSX2 uses at startup - was never clamped and still is not. Docked mode keeps its own allowlist.

Consequence worth knowing: a game that genuinely cannot fill 45 FPS gets a 45 Hz panel and the judder that implies.

### A drop can lock itself in - and now gets out

A drop is the only direction that can become permanent. The panel slows down, the compositor then paces the app at the new rate, the app measures that same rate and publishes it, and the loop has no way to tell a game that genuinely runs at 45 from a panel that dragged it down to 45. By the time the value is measured it is genuinely true, so the rate stays until it is set by hand. That is the failure users hit as "sync locked my game at 45 FPS and I have to turn it off and set 50 myself".

Fine mode cannot tell the difference by looking, so it stops looking and starts testing. When the smoothed measurement has sat on the panel rate for fifteen seconds, the panel is below its maximum, **and the app has demonstrated earlier in this session that it can go faster than the rate it is stuck on**, the panel goes up one hertz. Then:

| what happens next | what it means | what the sync does |
| --- | --- | --- |
| the measurement follows within six seconds | the panel was the limiter | stay, and test again fifteen seconds later |
| the measurement does not follow | the app really runs at that rate | back down, park that rate - five minutes, doubling on each further failure up to half an hour |

There is also a deadband on the requested rate itself, separate from the one on the measurement, and it is not symmetric. Rounding a smoothed value that wanders by half a hertz flips between two neighbours every few seconds, and every flip is a panel mode switch: a game running at 49.5 spent a session alternating 48 -> 49 -> 48 about every forty seconds. So the requested rate only rises once the measurement is more than 0.55 Hz above it. Falling is held to a wider margin, 1.8 Hz, because a value that runs under the panel rate is normal - the app is being throttled by the panel rather than free-running, and its own count sags accordingly. The client averages sixteen present intervals, so one late interval moves its number by a whole hertz: at a 49 Hz panel the log sat on 47.99 for minutes. Following that sag downwards is what made the panel flap; it now stays put unless the app has clearly dropped two hertz, and a scene that really does fall is still followed.

The climb is 45 -> 46 -> 47 ... until a test fails, so a panel that was holding a 50 FPS game at 45 recovers all five hertz on its own, about ninety seconds end to end.

That last condition is what keeps the test out of the way of a game that is simply running correctly. A title sitting at 50 Hz that has never measured above 50 has nothing to prove and is never tested - otherwise it would be pushed to 51 every fifteen seconds forever, which would be a defect of its own. Only the drop is ambiguous, and only a drop that follows a higher rate leaves the evidence behind.

A game that genuinely runs at 45 after having run at 50 loses six seconds of every five minutes to a one-hertz excursion, which is the price of never being stuck.

```
SaltyNX: display-sync: testing 45 Hz against 46 Hz, which the app held before
SaltyNX: display-sync: 45 Hz was not a lock, target followed to 46 Hz (active 46 Hz)
```

or

```
SaltyNX: display-sync: testing 45 Hz against 46 Hz, which the app held before
SaltyNX: display-sync: 45 Hz is real, back from the test (target 45 Hz, active 45 Hz)
```

The client path keeps its own test - one excursion of five hertz, armed only when a change lands on the floor - because a 5 Hz ladder cannot express anything finer.

A rise cannot get stuck that way, because the app can always correct it down. So the way out is to walk the panel back up once and watch what the app does:

- only a *small* drop to the 50 Hz floor arms one test - 50 -> 45 is the lock-in shape, while 60 -> 50 is simply the client asking for 50;
- after 10 s with nothing changing, the panel is moved back to the rate it just left, for up to 12 s;
- if the published target follows upward, the low reading was the panel's ceiling and the higher rate is kept;
- if it does not, the game really cannot fill the panel, the old rate is restored and that rate is never tested again for this app.

```
SaltyNX: display-sync: testing 50 Hz against 55 Hz, which the app held before
SaltyNX: display-sync: 50 Hz was not a lock, target followed to 55 Hz (active 55 Hz)
```

or

```
SaltyNX: display-sync: 50 Hz is real, back from the test (target 50 Hz, active 50 Hz)
```

At most three tests run per session, so a game that wobbles on the boundary cannot ping-pong the panel forever. The test returns to the rate the panel was actually on, not to the highest rate ever seen. The 60 Hz a client publishes before the game starts would otherwise send the panel to 60 for the whole test. A game that never dropped from a higher rate is never tested, and a rate that failed once is never tried again in the same session.

### Any integer refresh rate, not just multiples of five

The handheld panel is driven at 50 Hz for PAL titles and 60 Hz otherwise, and both are multiples of five, so this matters less than it sounds - but the 5 Hz ladder was never a hardware limit and the code no longer pretends it is. Nothing asks for an in-between rate today; the solver is there so the arithmetic is right if something ever does.

`setPLLDHandheldRefreshRate()` used to do this:

```c
uint8_t base_refreshRate = new_refreshRate - (new_refreshRate % 5);
uint32_t DIVN = (4 * base_refreshRate) / 10;
```

That truncation is a leftover, not a hardware limit. The PLLD supplies the panel pixel clock, and the refresh rate is that clock divided by `DSI_CLOCK_HZ / 60`:

```
pixelClock = 9375 * (4096 * (2 * DIVN + 1) + SDM_DIN) / (4 * DIVM * (1 << DIVP))
refresh    = pixelClock / (DSI_CLOCK_HZ / 60)
```

With `DIVM = 1` and `DIVP = 1` that reduces to an exact integer equation:

```
refresh * 3328 == 8192 * DIVN + 4096 + SDM_DIN
```

so any integer rate has a solution: take the nearest `DIVN` and let the sigma-delta modulator absorb the remainder. One divider step is `8192 / 3328 = 2.46 Hz` and the modulator covers a full step (±4096), so nothing in 40..60 Hz is out of reach.

Since 2.1.13 `plldSolveForRate()` does that, and the whole ladder is exact:

| rate | DIVN | SDM_DIN |
| --- | --- | --- |
| 45 | 18 | −1792 |
| 46 | 18 | +1536 |
| 47 | 19 | −3328 |
| 48 | 19 | **0** |
| 49 | 19 | +3328 |
| 50 | 20 | −1536 |

The old code turned a 48 Hz request into `DIVN = 18` (45 Hz) with a `+8192` correction, which is half a step past the modulator's range, so the panel silently stayed at 45 and the caller got `true` back.

Two related changes in the same version:

- the readback rounds instead of truncating (`(pixelClock + DSI/120) / (DSI/60)`), so a rate never reads back one hertz low;
- a target below the panel minimum now clamps to `HandheldModeRefreshRateAllowed.min` instead of jumping to 60 Hz, so 43 FPS content on an OLED gets 45 Hz rather than 60.

Because 1 Hz granularity is now possible, `updateRefreshRateForApp()` ignores a 1 Hz difference outright - it is always measurement noise and never worth a mode switch.

Still untouched: `correctOledGamma()` only knows 45/50/55/60. 50 and 55 share the same ELVSS values, so an in-between rate simply keeps what was last applied; nothing is restored.

### A client can lose its command id in the raw IPC header

The raw payload of an IPC request does not start at `TLS+0x20`; it starts right after the header (`TLS+0x14` for a request that carries a PID descriptor), and the client declares its length in the second control word. If a client writes its payload at `TLS+0x20` but declares less than `0x0C+16` bytes, the kernel copies only the magic word and the command id never reaches the server.

NetherSX2 664af43 does exactly that for `GetSharedMemoryHandle` (it passes `16` where its own layout needs `32`). The symptom is unmistakable: `GetDisplayRefreshRate` is served normally, while the shared-memory request shows up as an unknown command built from leftover bytes of the previous reply, so the client never maps the NX-FPS block and `active` stays `0`.

Since 2.1.9 the endpoint tolerates it: a request whose raw payload is too short to contain a command id, but which contains the `SFCI` magic, is treated as `GetSharedMemoryHandle` - the only other command this endpoint serves. The first such request is logged once:

```
[serviceThreadDisplaySync] client raw size 16 B hides the command id, treating it as GetSharedMemoryHandle
```

The 2.1.9 version of that check still looked for the magic in word 0, which is where it sits only for the short request. The 32 B `GetDisplayRefreshRate` request carries a stale `SFCO` magic from the previous reply in word 0 and the real pair in word 3, so every rate query was answered with an error and logged as `unsupported command 0xffffffffffffffff` - it looked like a client that never asks for the rate, while the diagnostics showed the rate changing all along (the client reads it from the shared block instead).

Since 2.1.12 the whole copied window is scanned for the magic:

```
[serviceThreadDisplaySync] unsupported command 0x... (magic missing, raw 32 B)
```

A client that declares the right size never hits the short path.

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
