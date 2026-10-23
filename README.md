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

Each of those calls is a real change of the panel rate - two register writes, cheap to perform, but an event all the same, and the frame it lands on is wrong. The picture hitches, the hitch perturbs the measured cadence, and the mismatch feeds itself, so the game feels like it keeps slowing down.

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
| target deadband | 1 Hz | on the *output*: rises past 1.2 Hz and only on 2 Hz of evidence, falls only past 1.8 Hz |
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

Fine mode cannot tell the difference by looking, so it stops looking and starts testing. When the measurement has sat on the panel rate for eight seconds and the panel is below its maximum, the panel goes up one hertz. Then:

| what happens next | what it means | what the sync does |
| --- | --- | --- |
| the measurement follows within six seconds | the panel was the limiter | stay, and test again eight seconds later |
| the measurement does not follow | the app really runs at that rate | back down, park that rate - five minutes, doubling on each further failure up to half an hour |

There is also a deadband on the requested rate itself, separate from the one on the measurement, and it is not symmetric. Rounding a value that wanders by half a hertz flips between two neighbours every few seconds, and every flip is a panel rate change: a game running at 49.5 spent a session alternating 48 -> 49 -> 48 about every forty seconds. So the requested rate only rises once the measurement is more than 1.2 Hz above it - wide, because the resting point of the whole loop is a panel at exactly the app's rate, the one place where the client's sixteen-frame window flickers between one present more than the panel and one fewer. Falling is held to a wider margin still, 1.8 Hz, because a value that runs under the panel rate is normal - the app is being throttled by the panel rather than free-running, and its own count sags accordingly. The client averages sixteen present intervals, so one late interval moves its number by a whole hertz: at a 49 Hz panel the log sat on 47.99 for minutes. Following that sag downwards is what made the panel flap; it now stays put unless the app has clearly dropped two hertz, and a scene that really does fall is still followed.

The fine target may not climb past the panel on a one hertz measurement. The published value is effectively whole hertz - NetherSX2's `estimate_present_fps()` returns an integer - so a 50 fps app in front of a 50 Hz panel reads exactly 50.00 nearly all the time, and 51.00 when one sixteen-interval window catches only short intervals. That single hertz is the entire width of the feedback signal. Under FIFO the app cannot present faster than the panel anyway, so a measurement two hertz clear of the panel is the only evidence that the panel is what is holding it back; a rise now needs that, and the one-hertz climb still happens through the probe above, which is unchanged.

The anchor the deadband compares against also survives a lost measurement. It used to be cleared after ten unusable samples, so the average restarted from a fresh number and whatever that number rounded to won outright - with no deadband to argue with it. That is how a panel sitting correctly at 50 Hz ended up at 51 for a minute and a half at a time.

### Native games, and how close this really gets to VRR

Nothing in fine mode is emulator-specific. It follows whatever publishes `FPSavg` into the NX-FPS block, and for a native game that is NX-FPS itself, hooking the game's own present path. The emulator needed a handoff only because its Vulkan wrapper resolves `vkQueuePresentKHR` at runtime, so NX-FPS's hook is never called; a native game has no such problem. Turn on display sync and it works, with no flag files and no per-game setup.

There was one thing wrong with that, and it mattered for almost every native title.

**A 30 fps game used to be sent to 45 Hz.** The floor is a floor, so content below it was clamped to it, and 30 fps against 45 Hz is a 1.5:1 ratio - one frame shown for two refreshes and the next for one, forever. That is a hitch on *every frame*, which is worse than not syncing at all. Sixty hertz shows each frame for exactly two refreshes; that is the correct answer and it is the one a VRR display reaches for when it cannot match the content.

The target for sub-floor content is now a whole multiple of the content rate, taken from a snapped rate rather than the raw measurement - a 30 fps game measures 28.6 during a busy scene and 2 x 28.6 is 57 Hz, which is a 1.9:1 ratio and worse than the floor it replaced. Games lock to a handful of rates, so the value is snapped within two hertz first and the multiple is stable without any hysteresis of its own.

| content | panel | ratio |
|---|---|---|
| 60 fps | 60 Hz | 1:1 |
| 50 fps | 50 Hz | 1:1 |
| 47 fps | 47 Hz | 1:1 |
| 30 fps | **60 Hz** | 2:1, every frame twice |
| 25 fps | 50 Hz | 2:1 |
| 24 fps | 48 Hz | 2:1 |
| 20 fps | 60 Hz | 3:1 |
| 15 fps | 45 Hz | 3:1 |
| 43 fps | 45 Hz | 1.05:1 |
| 40 fps | 45 Hz | 1.125:1 |

The last two rows are where the range runs out. Between 30 and 45 fps there is no usable multiple - twice the content is above the ceiling and the content itself is below the floor - so the floor is the best ratio on offer. That is still far better than the plain clamp was: 43 fps against 45 Hz is 1.05:1, one frame in twenty repeated, not half of them.

**This is not VRR, and until 2.1.37 this document described the difference wrongly.** The earlier text here said every rate change was a full mode switch - PLLD and nvdisp reprogrammed, the panel blanked, tens of milliseconds, plainly visible - and used that to justify the four second hold. Reading `SetDisplayRefreshRate()` rather than trusting the description showed a different picture. In handheld it takes the `setPLLDHandheldRefreshRate()` path, which writes one word to `PLLD_BASE` and one to `PLLD_MISC` and returns. No mode set, no blanking, no ioctl to wait on. The four second hold was protecting against a cost that did not exist on this path.

What is true is narrower and more interesting. The panel re-locks on a frame boundary, so a change costs about one frame - not nothing, but not a visible pause either. And the change is an *event*, not a continuum: one constant replaces another and stays there until the next event. A VRR panel varies the interval of every scanline; this can only pick the next constant. That is the real distance from VRR, and it is a distance in resolution, not in cost.

So a 60 fps game sits at 60 and never moves. A 30 fps game sits at 60 with perfect 2:1 cadence. A game whose rate genuinely wanders gets a panel that follows it, hertz by hertz, arriving in one step instead of crawling. What it does not get is a panel that tracks frame by frame.

### Nothing waits any more: direct tracking

Everything before 2.1.37 stacked delay on delay, and each layer was individually defensible. `FINE_TAU_MS` was a 2140 ms average. `REFRESH_RATE_FINE_MS` was a four second hold before the decision could be acted on. `FINE_RISE_STEP` capped a move at two hertz, so a 60 -> 47 fall took seven writes. In series they made a real scene change take 6.6 seconds to reach the panel.

All three are gone.

**The average was the worst of them, because it was averaging something already averaged.** `FPSavg` is not a raw frame counter. For a native game NX-FPS computes a ten frame exponential average and publishes it on every frame - about 167 ms of window at 60 fps. For NetherSX2 it is a printed-once-a-second value over the emulator's own present window. Putting a 2140 ms time constant on top of a one second average meant the loop was responding to a value that described a scene two seconds gone. It was there to stop one bad sample from reaching the decision, and `FINE_REJECT_MS` already does that earlier and more sharply: anything more than two hertz from the current value is refused until it has persisted. The extra stage bought no protection at all and cost two seconds of every response. Samples are now taken as published, and the filtering happens in `fineMeasuredTarget`, where a difference is judged - a decision is a much better place to be conservative than a measurement is.

**The hold is now a rate limit on the actuator, not a delay in the decision.** Instead of requiring a change to survive a timer before it is allowed, the loop allows at most one write every 700 ms and otherwise acts on the next tick, which is ten milliseconds away. The difference matters: a timer adds latency to every change including the real ones, while a limit only costs anything when changes are rapid - which is exactly when damping is wanted. The 700 ms is not a compromise either; the fastest publisher here is once a second, so a change can never be justified more often than that.

**And the step cap is gone, because the thing it protected against does not exist.** Two hertz at a time was right when each step was believed to be an expensive mode switch. It is two register writes, so a 60 -> 47 fall is one write, not seven, and the evidence that produced the target - persistence through the jump rejection and the deadband - was already required.

| | 2.1.36 | 2.1.37 |
|---|---|---|
| estimator | 2140 ms average | as published |
| decision to action | 4 s hold | one tick, rate-limited to 1 per 700 ms |
| a 60 -> 47 fall | 7 writes | 1 write |
| **total, scene change to panel** | **6.6 s** | **~3.1 s** |

The remaining three seconds are `FINE_REJECT_MS` and nothing else. That is the deliberate part, and it is the one place where being fast would be a mistake - see the asymmetry below. It is now the whole latency budget rather than one of four terms in it.

**One thing still cannot be tracked, and it is worth being clear about.** Under vsync the panel paces the app, so an app held at 47 measures 47 whether it could do 50 or not - the signal that would justify a rise does not exist in the measurement. Rising therefore cannot be inferred, only tested: after eight seconds of the measurement agreeing with the panel, the loop raises it one hertz for six seconds and watches. If the app follows, the panel stays; if not, that rate is parked. Eight seconds instead of fifteen is the only change here, and it makes the discovery faster without changing how often a wrong guess is retried.

### "NX-FPS is not running" has two different causes

FPSLocker's message looks the same for two faults that have nothing in common, and the difference decides who can fix it.

`pluginActive` is the field the overlay gates its entire menu on - no plugin, no buffer selector, no target list. It is set in exactly one place, `NX-FPS-Common.hpp` inside `PostFrame()`, which runs when NX-FPS's patched present path fires. It is not set by the sysmodule and not by any client.

So:

| what the status line says | what actually happened | whose problem |
|---|---|---|
| `block=missing` | the Core never initialised in that process - no `0x465053` anywhere in shared memory | install/load: `saltynx_core.elf`, `saltynx_bootstrap.elf` |
| `block=found plugin=0` | the Core initialised and NX-FPS's present hook never fired for this title | the game's rendering path, i.e. nothing to fix here |
| `block=found plugin=1` | working | - |

Since 2.1.39 the 30 second status line carries `plugin=`, and the 30 second "no measurement" line says which of the two it is in words. Before that both read as "the plugin is not running".

Worth knowing while comparing titles: NetherSX2 reports the first state as *working*. It publishes into the same block, but it finds the block rather than creating it - the Core still has to run and build it. So an emulator that measures correctly proves the Core loaded, and says nothing about whether NX-FPS's own present hook fires.

### Who does what: SaltyNX, the Core, and FPSLocker

These three are constantly mistaken for alternatives. They are a stack, and the confusion costs people a whole evening every time, because the missing piece is the one nobody installs separately.

| piece | what it is | what it actually does |
|---|---|---|
| **SaltyNX** (`exefs.nsp`, a sysmodule) | the mechanism | hijacks the game's process, owns the shared memory, serves the IPC, and performs the rate change itself (`SetDisplayRefreshRate`, two PLLD register writes) |
| **the Core** (`saltynx_core.elf`, injected into the game) | the instrumentation | creates the NX-FPS block, counts presents, publishes `FPS` / `FPSavg`, implements the buffer and FPS-target settings |
| **FPSLocker** (a Tesla overlay) | the user interface | reads that block, writes `FPSlocked` / `Buffers` into it, and calls the SaltyNX IPC |

The line worth remembering: **the overlay has no measuring code of its own.** FPSLocker finds the block by the same magic, `0x465053`, that this sysmodule scans its shared memory for, and it refuses to draw its real menu until `pluginActive` is set. No Core, no block, no `pluginActive` - and the overlay correctly shows "NX-FPS is not running" instead of a menu.

That is the state the log shows. `fine: block=missing` is this sysmodule looking for `0x465053` in its own shared memory and not finding it, which means no Core ever initialised inside the game. Fine mode then has nothing to follow, and FPSLocker has nothing to display. The two symptoms are the same cause, seen from two processes.

The Core is not a separate download - it is built from this repository and must be present as `sdmc:/SaltySD/saltynx_core.elf`, next to `sdmc:/SaltySD/saltynx_bootstrap.elf`, and both must come from the same release as the `.nsp`. Installing only `exefs.nsp`, which is the natural thing to do when the only file that changes between builds is the size of the sysmodule, leaves the Core at whatever version was there before - or absent on a first install. Since 2.1.38 the sysmodule says which of the two it found at boot:

```
SaltyNX: saltynx_core.elf present, 197592 B
SaltyNX: saltynx_bootstrap.elf is MISSING - games will start without SaltyNX and nothing will measure a frame rate
```

One more line in that log needs translating, because it looks alarming and is not. `no client on port NethFPS yet` is about the *emulator*: NetherSX2 connects to a private endpoint of this fork and publishes its own `FPSavg` there. A native game never connects to it, so the line is expected background noise in a native-game log and has nothing to do with NX-FPS.

### The target in the overlay, and the bug it exposed

FPSLocker's target field (`FPSlocked`) used to be ignored entirely once fine mode had measured anything, for a good reason: on the client path it is quantised onto the 5 Hz ladder, so following it turned a 47 into a 45 and moved the panel a whole step on one bad sample.

That left no way to say "I want this capped at 50", which is a reasonable thing to want. So the field is read again, as a **ceiling** rather than a source of truth: fine mode follows the measured rate and simply never goes above what was set. Zero means nothing was set and the whole window is available.

The cap is applied *after* the deadband, and that ordering is the whole point. The deadband anchors on the last target, so a panel that reached 60 before a target of 50 was set would compute "60 is where I already am, the measurement is 60, nothing to do" and sit there forever - the target would be ignored for exactly as long as the content stayed above it. A ceiling is not a preference to be smoothed.

Two deliberate exceptions:

- **Sub-floor content ignores the ceiling.** Below 45 fps the target is a repeat factor, not a tracked rate, and the only good factor is the one that divides the content evenly. 30 fps at 60 Hz shows every frame exactly twice; 30 fps at 45 Hz shows one frame once and the next twice, forever. Capping the multiple would produce a strictly worse picture, so it does not.
- **A display-sync client suppresses it for the session.** NetherSX2 writes `FPSlocked` from its own measurement, quantised - around a 47 fps scene it publishes 45. Honouring that as a ceiling would pin the panel at 45 and undo the entire reason fine mode exists, which is escaping that ladder. A client connected to this fork's private endpoint is exactly such a writer, so the cap is off for that session and the measurement speaks for itself. The log says so once.

Worth knowing: FPSLocker steps its target by five, so 45, 50, 55 and 60 are reachable from the overlay and 47/48/49 are not. The sysmodule itself honours any value in the window, so a client that writes 47 gets 47 - it is the overlay's ladder that cannot express it.

### A feature that never worked, and why

2.1.36 taught the loop to send sub-floor content to a whole multiple of its rate - 30 fps to 60 Hz instead of clamping to 45. The arithmetic was right and it was dead on arrival.

The rise gate above it refuses any target higher than the panel unless the measurement has first climbed `FINE_RISE_EVIDENCE` above the panel:

```c
if (fineRate > temp_refreshRate && fineAvg < temp_refreshRate + FINE_RISE_EVIDENCE)
    fineRate = temp_refreshRate;
```

At 30 fps on a 45 Hz panel the target is 60, the measurement is 30, and `30 < 45 + 1.9` is true on every sample for as long as the game runs. So the panel sat at 45 with a 1.5:1 ratio - a hitch on every frame - and the branch could never lift it, because the app can never measure above the panel it is stuck behind under vsync.

The gate is correct and stays; it just must not apply to a repeat factor. A 1:1 target above the panel is a claim that the app wants a higher rate, and that claim needs evidence. A multiple is derived *from* the panel being wrong in the first place, so demanding that the app first measure above the panel is demanding proof of the thing being fixed.

`fine:` now carries `cap=`, so a capped panel is never a mystery:

```
fine: block=found plugin=1 FPS=30 FPSavg=30.00 meas=30.00 target=60 panel=60 peak=60.0 cap=0
```

### FPSLocker, or this? They answer different questions

FPSLocker is an overlay that sets a target. This is a loop that follows one. The distinction sounds academic until it meets a game whose frame rate wanders, which is most of them.

| | FPSLocker | fine mode |
|---|---|---|
| granularity in handheld | **5 Hz** - 15/20/25/30/35/40/45/50/55/60 | **1 Hz**, every integer 45..60 |
| open or closed loop | open - writes a value and trusts it | closed - measures, then decides |
| per game | yes, saved profile | follows whatever is running |
| needs a UI | yes, a Tesla overlay | no |
| can express 47 | no | yes |

The ladder is not a limitation anyone chose on a whim: `AllowedFPSTargets[32] = {15, 20, 25, 30, 35, 40, 45, 50, 55, 60}` in FPSLocker's `source/main.cpp`. The values this subsystem has spent weeks debugging - 47, 48, 49, 51 - are not on it. A 50 fps game pinned to 50 is fine; a 47 fps scene has nowhere to go except 45, and a panel 5 Hz below the content is a repeated frame every 200 ms.

The two are wired to be mutually exclusive, and FPSLocker already knows it. Its sync toggle calls `SaltySD_SetDisplaySync`, which sets `displaySync` here, which gates the entire automatic path - fine mode included. Turn sync off in FPSLocker and the rate selector is in charge; turn it on and this subsystem is. Both directions work with this fork: the IPC command ids match FPSLocker's `include/SaltyNX.h` exactly (0 end, 6 check-shared, 7 shared-handle, 10 get-rate, 11 set-rate, 12 set-sync, 13..19 the docked and policy calls), and nothing in FPSLocker checks a version string.

**Use FPSLocker when you know the answer.** A game that runs at a rock solid 30 and looks best at 60, a title you want pinned at 55 to cut power, anything where you have decided. 5 Hz granularity is fine when the content is not near a boundary.

**Use fine mode when you do not.** Content that wanders between 50 and 60 depending on the scene, or a PAL title whose real rate is 50 but whose measurement is 49.6. That is where 5 Hz becomes a visible mode switch and 1 Hz does not.

**If fine mode keeps disappointing you, FPSLocker plus sync off is a legitimate answer.** It is a mature tool, the rates are stable because nothing is adapting, and you get per-game profiles. It will not find 50 on its own and it cannot sit at 47, but nothing about it oscillates.

### Two masters, one control

With vsync on, the panel paces the emulator: set 47 Hz and the emulator presents 47 frames. That is the whole reason the fine path exists, and it has a consequence that is easy to miss - **the measurement can never show that the app wants to go faster**, because the app is being shown at the rate the panel is already running. `FPSavg` reports what the panel did, not what the content could do.

Everything below followed from getting that wrong.

**The evidence gate was unobtainable.** Until 2.1.35 a probe was armed only when `finePeak` - the highest measurement of the session - was at least one hertz above the current panel. Under pacing that can never happen. A panel that dropped to 47 after a scene dip stayed at 47 for the rest of the session: the deadband refuses a rise of one hertz, and the gate demanded evidence that measuring could not produce. Every settled rate above the floor was a dead end.

The excursion is now the instrument, and the only one. One hertz up for six seconds answers the question directly - does the app follow? - and the parking backoff bounds the cost of a wrong guess: five minutes, doubling to half an hour. Measured against the shipped logic, a panel reset to 47 with a 50 fps app behind it never moves; with this change it climbs 47 -> 48 -> 49 -> 50 in about twenty seconds.

**The two writers to `FPSlocked`.** Fine mode used to fall back to the client's `FPSlocked` whenever a single sample was unusable, and the client derives that value by rounding its own measurement onto a 15/20/25/30/35/40/45/50/55/60 ladder. A 47 therefore became a 45: one bad sample dropped the panel a whole ladder step, and the loop spent the next minute climbing back to 47. Once fine mode has measured anything it is now the authority, and an unusable sample holds the current panel instead - the panel is already where this path put it, so holding it is the honest answer.

`FPSavg` stays the instrument, but only for *downward* moves. A value below the panel is real evidence that the content is slower than the panel. A value equal to the panel proves nothing.

### Why the panel used to sink and stay sunk

Reported on 2.1.31 as "turn on handheld display sync and it drops to 45 Hz and stays there". It was a real defect, and the mechanism is worth writing down because everything in it looks correct in isolation.

A fine probe moves the panel **one** hertz (`FINE_LOCK_STEP`). The target deadband is **1.2** hertz (`FINE_TARGET_HYST`). The probe's success test asked whether the *target* had reached the tested rate - and the target is exactly the value the deadband freezes. So a settled measurement at 45.0 next to a 46 Hz panel produced a target of 45 (the rise was 1.0 Hz, inside the 1.2 Hz band), the test read 45 against a target of 46, declared the rate real, and put the panel back four seconds later. Every upward probe was a no-op by construction.

That makes the loop one-way. Downward moves are self-confirming: once the panel drops below the app, the client follows the panel, reports the panel's rate back, and that number keeps agreeing with the new rate. Nothing in the measurement can ever argue upward again, because the probe - the only channel that can - was structurally unable to succeed. One dip, and the panel walked to the floor.

Simulated against the shipped logic with a display-synced client and one 25 second menu dip, that is 64 panel mode switches in twelve minutes, ending at 49 Hz and flickering 45 <-> 46. The device shows the same shape in the 2.1.26 log (`45 Hz was not a lock, target followed to 46 Hz`, then back).

Three changes:

- The fine test reads the **measurement**, not the target: the app counts as having followed when the smoothed value reaches the tested rate. The target is deliberately frozen; it cannot also be the verdict.
- A followed probe **ratifies the anchor** (`fineTarget = probeTarget`), so the deadband cannot immediately argue for the rate we just left and undo the gain in four seconds.
- A climb is a state. While one is in progress the next hertz is worth trying without waiting for fresh evidence, and it ends when a probe fails or the panel moves down.

The floor also earns a test of its own. 45 Hz is a clamp, not a content-derived rate, so nothing else can ever lift the panel off it; a panel sitting on the floor is now owed an upward test even if the app has never demonstrated headroom. The parking backoff still applies, so a title that genuinely runs at 43 pays one six second excursion every five minutes, doubling to half an hour.

The same simulation with these changes: 9 switches and settled at 50 Hz.

### Why the sampling rate was never the slow part

The control loop samples a shared block. Raising that rate is free and it was raised - 900 ms to 250 ms in 2.1.33, to 100 ms in 2.1.34 - but it buys about half a second, and it is worth being clear about why.

A 50 to 45 fps scene change, measured end to end against the actual code: **13.7 s** before the panel moved on 2.1.33. Of that, the sampling interval contributed 0.15 s and the client's own once-a-second publish contributed 0.5 s. The rest was the jump rejection and the hold - two persistence filters that were both tuned long.

| delay | 2.1.33 | 2.1.34 |
|---|---|---|
| client publishes | 1 s | 1 s (client side) |
| sampling phase | up to 250 ms | up to 100 ms |
| jump rejection | 9 s | 2 s |
| hold before the move | 4 s | 4 s |
| **total** | **13.7 s** | **6.6 s** |

The nine second rejection was `FINE_REJECT_MAX = 10` samples, which at the old 900 ms sampling genuinely was nine seconds of protection against a hitch. Against dips of one to ten seconds the two second version holds exactly the same line - anything up to three seconds is ignored, anything from five seconds is followed - because the hold is a second filter in series and a dip that ends during the hold cancels the move.

Going to ten publishes a second from the client would not help further: its sixteen-frame window is 320 ms, so at 10 Hz only 31% of each window is new data and the rest is the same measurement read again. Four a second is the point where a fresh window arrives at least three quarters new. `nethersx2-fpsavg-v3-handoff.md` asks for four.

### How fast the loop reacts

Until 2.1.37 the answer was "four and a half seconds, in two stages". Now there is one stage that matters:

| stage | period | where |
|---|---|---|
| client publishes `FPSavg` | 1 s | `saltysd_display_sync_poll()`, `s_next_query_tick` |
| sysmodule reads it | 100 ms | `FINE_SAMPLE_MS` |
| a jump has to persist | 3 s | `FINE_REJECT_MS` |
| panel is moved | one tick later, at most one write per 700 ms | `FINE_CHANGE_MIN_MS` |

The 2140 ms averaging filter is gone. `FPSavg` is already an average over the publisher's own window, so that filter was a second averaging stage stacked on the first - it made a 50 -> 45 scene take 4.78 s to reach ninety percent, and the only thing it bought was protection from single bad samples, which the jump rejection already provides. The accounting is in "Nothing waits any more" above.

The 100 ms sampling interval, the jump-rejection hold and the unusable-sample grace are all durations rather than sample counts. That matters because they were counts: ten samples was ten seconds at 900 ms and would have been a fifth of that at 250 ms, so raising the rate without converting them would have quietly made the filter many times twitchier.

The 700 ms between writes is not a debounce. A debounce adds latency to every change including the real ones; this only costs anything when changes are rapid, which is exactly when damping is wanted. It is also doing a second job: the sample arriving right after a write was measured *during* the transition, so deciding on it turns one event into two writes in opposite directions.

**The three second rejection is not timidity, it is the shape of the problem.** A fall is cheap to make and expensive to undo. Once the panel drops to 45 the compositor paces the app there, the app then measures 45, and nothing in the measurement can tell "the game runs at 45" apart from "the panel is holding it at 45". The only way back up is a probe: eight seconds of agreement, then a six second test per hertz, so about thirty seconds to recover five. A fall caused by a two second stutter therefore costs far more than three seconds of extra latency on every genuine change. Dips of up to three seconds are ignored; a scene that actually changed is followed three seconds later.

**And a single bad sample is no longer enough to start that fall.** The last three values the publisher actually produced are kept in a ring - only changes go in, so nine of every ten reads contribute nothing - and the median of those three is what the loop acts on. One dip among three cannot win the median, which removes the exact case that matters: a lone low sample moving a correct panel, and costing half a minute to undo. A change that persists replaces the whole ring within two publishes, so it costs about a second on a real scene change and nothing on noise.

What is left is the client's cadence, and nothing on this side can beat it. Its ring holds sixteen present intervals, 320 ms at 50 Hz, and publishes once a second, so it measures three times more than it reports. `nethersx2-fpsavg-v3-handoff.md` drops that to 250 ms; the sysmodule follows whatever cadence arrives, so that takes effect without a matching release here. A native game is limited in the same way by whatever NX-FPS is doing.

Fine mode also used to step towards its target, two hertz at a time, and that is gone too: a 60 -> 47 fall is one write now, not seven. The evidence that produced the target - persistence through the jump rejection, then the deadband - was already required, so the cap was only paying for a doubt the earlier stages had removed. On 2.1.29 a single measurement took the panel from 45 to 60 Hz after 1.5 seconds, with the value still reading 45.09 fourteen seconds earlier; that specific failure is now impossible for a different reason, because a fifteen hertz jump is refused for three seconds before it is believed at all.

The climb is 45 -> 46 -> 47 ... until a test fails, so a panel that was holding a 50 FPS game at 45 recovers all five hertz on its own, about ninety seconds end to end.

That last condition is what keeps the test out of the way of a game that is simply running correctly. A title sitting at 50 Hz that has never measured above 50 has nothing to prove and is never tested - otherwise it would be pushed to 51 every eight seconds forever, which would be a defect of its own. Only the drop is ambiguous, and only a drop that follows a higher rate leaves the evidence behind.

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
