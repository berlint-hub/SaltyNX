#include <switch.h>
#include "ipc.h"
#include "display_refresh_rate.h"

#include <stdlib.h>
#include <dirent.h>

#define NANOPRINTF_IMPLEMENTATION
#include "useful.h"
#include "dmntcht.h"
#include <malloc.h>
#include "service.h"
#include "display_sync.h"
#include "hijack.h"

#define MODULE_SALTYNX 420
#define NVDISP_PANEL_GET_VENDOR_ID 0xC003021A


u32 __nx_fs_num_sessions = 1;

const size_t shmem_size = 0x1000;

struct NxFpsSharedBlock* nx_fps = 0;

u32 __nx_applet_type = AppletType_None;

struct MinMax {
    u8 min;
    u8 max;
};

Handle saltyport, saltyportDisplaySync, sdcard, injectserv;
static Thread displaySyncThread;
static u8 displaySyncThreadStack[0x6000] __attribute__((aligned(0x1000)));
static char g_heap[0x30000];
bool already_hijacking = false;
bool should_terminate = false;
u64 exception = 0x0;
SharedMemory _sharedMemory = {0};
size_t reservedSharedMemory = 0;
uint64_t clkVirtAddr = 0;
uint64_t dsiVirtAddr = 0;
bool displaySync = false;
bool displaySyncOutOfFocus60 = false;
bool displaySyncDocked = false;
bool displaySyncDockedOutOfFocus60 = false;
uint8_t refreshRate = 0;
s64 lastAppPID = -1;
bool isOLED = false;
bool isLite = false;
bool cheatCheck = false;
bool isDocked = false;
bool dontForce60InDocked = false;
bool matchLowestDocked = false;

#ifdef SWITCH
    #define systemtickfrequency 19200000
#elif OUNCE
    #define systemtickfrequency 31250000
#else 
    uint64_t systemtickfrequency = 0;
#endif
static_assert(systemtickfrequency != 0);

//This is done to save some space as they have no practical use in our case
void* __real___cxa_throw(void *thrown_exception, void *pvar, void (*dest)(void *));
void* __real__Unwind_Resume();
void* __real___gxx_personality_v0();
void __wrap___cxa_throw(void *thrown_exception, void *pvar, void (*dest)(void *)) {abort();}
void __wrap__Unwind_Resume() {}
void __wrap___gxx_personality_v0() {}

void __libnx_initheap(void)
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    fake_heap_start = &g_heap[0];
    fake_heap_end   = &g_heap[sizeof g_heap];
}

void __appInit(void)
{
    svcSleepThread(1*1000*1000*1000);
}

void __appExit(void)
{
    already_hijacking = false;
    fsdevUnmountAll();
    smExit();
    setsysExit();
    nvExit();
    pdmqryExit();
}

void ABORT_IF_FAILED(Result rc, uint8_t ID) {
    if (R_FAILED(rc)) {
        uint32_t* address = (uint32_t*)(0x7100000000 + ID);
        *address = rc;
    }
}

//Tweaks to nvInitialize so it will take less RAM
#define NVDRV_TMEM_SIZE (8 * 0x1000)
alignas(0x1000) char nvdrv_tmem_data[NVDRV_TMEM_SIZE];

Result __nx_nv_create_tmem(TransferMemory *t, u32 *out_size, Permission perm) {
    *out_size = NVDRV_TMEM_SIZE;
    return tmemCreateFromMemory(t, nvdrv_tmem_data, NVDRV_TMEM_SIZE, perm);
}

u64 TIDnow;
u64 BIDnow;

static bool isServiceRunning(const char *serviceName) {	
	Handle handle;	
	if (R_FAILED(smRegisterService(&handle, smEncodeName(serviceName), false, 1))) 
		return true;
	else {
		svcCloseHandle(handle);	
		smUnregisterService(smEncodeName(serviceName));
		return false;
	}
}

static bool isCheatsFolderInstalled() {
    char romfspath[0x40] = "";
    bool flag = false;

    npf_snprintf(romfspath, 0x40, "sdmc:/atmosphere/contents/%016lx/cheats", TIDnow);

    DIR* dir = opendir(romfspath);
    if (dir) {
        if (readdir(dir))
            flag = true;
        closedir(dir);
    }

    return flag;
}

static ptrdiff_t searchNxFpsSharedMemoryBlock(uintptr_t base) {
	ptrdiff_t search_offset = 0;
	while(search_offset < shmem_size) {
		uint32_t* MAGIC_shared = (uint32_t*)(base + search_offset);
		if (*MAGIC_shared == 0x465053) {
			return search_offset;
		}
		else search_offset += 4;
	}
	return -1;
}

__attribute__((noinline)) static Result isApplicationOutOfFocus(bool* outOfFocus) {
    static s32 last_total_entries = 0;
    static bool isOutOfFocus = false;
    s32 total_entries = 0;
    s32 start_entry_index = 0;
    s32 end_entry_index = 0;
    Result rc = pdmqryGetAvailablePlayEventRange(&total_entries, &start_entry_index, &end_entry_index);
    if (R_FAILED(rc)) return rc;
    if (total_entries == last_total_entries) {
        *outOfFocus = isOutOfFocus;
        return 0;
    }
    last_total_entries = total_entries;

    #define EVENT_COUNT 16
    PdmPlayEvent events[EVENT_COUNT];
    s32 out = 0;
    s32 start_entry = end_entry_index - (EVENT_COUNT-1);
    if (start_entry < 0) start_entry = 0;
    rc = pdmqryQueryPlayEvent(start_entry, events, EVENT_COUNT, &out);
    if (R_FAILED(rc)) return rc;
    if (out == 0) return 1;

    int itr = -1;
    for (int i = out-1; i >= 0; i--) {
        PdmPlayEvent* event = &events[i];
        if (event->play_event_type != PdmPlayEventType_Applet)
            continue;
        if (event->event_data.applet.applet_id != AppletId_application)
            continue;
        union {
            uint32_t parts[2];
            uint64_t full;
        } TID;
        TID.parts[0] = event->event_data.applet.program_id[1];
        TID.parts[1] = event->event_data.applet.program_id[0];

        if (TID.full != (TIDnow & ~0xFFF))
            continue;
        else {
            itr = i;
            break;
        }
    }
    if (itr == -1) return 1;

    bool isOut = events[itr].event_data.applet.event_type == PdmAppletEventType_OutOfFocus || events[itr].event_data.applet.event_type == PdmAppletEventType_OutOfFocus4;
    *outOfFocus = isOut;
    isOutOfFocus = isOut;
    return 0;
}

void updateNxFpsPointer() {
    uintptr_t sharedAddress = (uintptr_t)shmemGetAddr(&_sharedMemory);
    if (sharedAddress) {
        ptrdiff_t offset = searchNxFpsSharedMemoryBlock(sharedAddress);
        if (offset != -1) {
            nx_fps = (struct NxFpsSharedBlock*)(sharedAddress + offset);
        }
    }
}

void checkIfDmntChtNeedsToBeActive() {
    static bool dmntchtActive = false;
    if (!dmntchtActive) dmntchtActive = isServiceRunning("dmnt:cht");
    if (!dmntchtActive || !isCheatsFolderInstalled())
        cheatCheck = true;
    else {
        Handle debug_handle;
        if (R_SUCCEEDED(svcDebugActiveProcess(&debug_handle, lastAppPID))) {
            s32 thread_count;
            u64 threads[2];
            svcGetThreadList(&thread_count, threads, 2, debug_handle);
            svcCloseHandle(debug_handle);
            if (thread_count > 1) {
                cheatCheck = true;
                if (R_SUCCEEDED(dmntchtInitialize())) {
                    dmntchtForceOpenCheatProcess();
                    dmntchtExit();
                }
            }
        }
        else cheatCheck = true;
    }
}

// A client that derives its target from a *measured* frame cadence does not publish
// a clean cap. NetherSX2 publishes the rate it just measured, quantised to 5 Hz, so
// around a 50 FPS PAL game the target walks 45/50/55 for seconds at a time.
//
// Every SetDisplayRefreshRate is a real panel mode switch (PLLD + nvdisp), so
// following that walk is expensive: the picture hitches, the hitch changes the
// measured cadence, and the next target is wrong again. That feedback loop is what
// made display sync feel like it kept slowing down.
//
// Corrections are therefore asymmetric:
//   * a large change (new FPSLocker cap, docked switch) applies after 1.5 s;
//   * a small rise (45 -> 50) applies after 5 s, because it restores the panel to
//     the rate the app actually wants;
//   * a small drop (50 -> 45) applies only after 20 s, because a sudden few-Hz drop
//     is almost always one slow frame being measured, and dropping the panel costs a
//     mode switch plus a slower picture;
//   * a 1 Hz difference is ignored outright. Since 2.1.13 the panel can sit on any
//     integer rate, so the measured value would otherwise walk 48/49/50 forever.
#define REFRESH_RATE_SMALL_DELTA   5u
#define REFRESH_RATE_SMALL_UP_MS   5000ULL
#define REFRESH_RATE_SMALL_DOWN_MS 20000ULL
#define REFRESH_RATE_LARGE_MS      1500ULL
#define REFRESH_RATE_FINE_MS       4000ULL
#define REFRESH_RATE_DEADBAND_HZ   1u
#define SV_TICKS_PER_MS        19200ULL

// Display sync is not allowed to take the handheld panel below this. 45 Hz is the
// OLED panel's own floor and the hardware limit the sweep in 2.1.20 measured: every
// integer rate from 45 to 60 lands exactly, so there is no reason to keep the automatic
// path out of 45..49 any more. The window this sync may use is 45..60, one hertz apart.
//
// Rates the client cannot express - anything outside its 5 Hz table, and anything
// between whole hertz - come from the measured path, which is on by default since
// 2.1.26. The clamp below only catches the client's own quantised target.
#define DISPLAY_SYNC_MIN_RATE 45u

static uint32_t pendingRefreshRate = 0;
static u64 pendingRefreshSince = 0;
static u64 pendingRefreshRequired = 0;

// A drop is the one direction that can feed itself to death. The panel slows down, the
// compositor then paces the app at the new rate, the app measures that same rate and
// keeps publishing it, so nothing in the loop can tell a genuine 45 FPS game from a
// panel that dragged it to 45. It stays there until somebody sets the rate by hand.
//
// The way out uses information the loop already has: the highest rate the app ever
// asked for. If the app published 50 and we later sit at 45, then 45 is below what the
// app has demonstrated it can do, so it is worth one test. Walk the panel back up to
// that demonstrated rate and watch whether the app follows. If it does, the low
// reading was the panel's ceiling and the higher rate is kept. If it does not, the game
// really cannot fill the panel, the old rate is restored, and that rate is never tested
// again for this app.
//
// Only a rate the app actually asked for counts. The 60 Hz default that stands in for
// "no target yet" must not, or every launch would probe away from its real rate.
#define PROBE_HOLD_MS    10000ULL
#define PROBE_WINDOW_MS  12000ULL
// Fine mode has no way out of a lock on its own: the panel paces the app, the app
// measures the panel, and "the game runs at 45" and "the panel is holding the game at
// 45" produce the same number by construction. The only test that separates them is to
// raise the panel and watch whether the measurement follows. Wait fifteen seconds of
// agreement before suspecting it, then test 2 Hz up - two, not five, because one hertz
// is the whole point of this path. One hertz, because a 2 Hz excursion overshoots: at a
// 49 Hz panel the app lands on 50, the test wants 51, and the panel ends up one hertz
// off with the app shorting frames.
#define FINE_LOCK_SUSPECT_MS 15000ULL
#define FINE_LOCK_STEP       1u
// A successful test is followed by another one, so the panel climbs 45 -> 47 -> 49 -> 51
// until the app stops following. A failed one parks that rate for five minutes; the
// scene that could not do 52 at minute one may do it at minute six, and without a retry
// the panel would never find out. The test itself lasts six seconds, not twelve - the
// client publishes once a second and the average crosses 2 Hz in two or three samples.
#define FINE_RETRY_MS        300000ULL
#define FINE_RETRY_MAX_MS    1800000ULL
#define FINE_PROBE_WINDOW_MS 6000ULL
// The measurement wanders by half a hertz, so rounding it to a whole hertz flips
// between two neighbours every few seconds - and every flip is a panel mode switch.
// The requested rate therefore keeps a deadband of its own: it only moves when the
// measurement has clearly left it, not when it grazes the boundary. Upwards that
// band has to cover more than half a hertz, because the resting point of this whole
// loop is a panel at exactly the app's rate - the one place where the client's window
// flickers between "one more present than the panel" and "one fewer". A 50 Hz panel
// with 50 fps content read 51.00 on the device, and 0.55 Hz did not stop it.
#define FINE_TARGET_HYST     1.20f
// How far a rise may move the panel in one step. A measurement 15 hertz above the
// panel is either a scene change or a burst of double presents, and only a step this
// size is cheap enough to be wrong about.
#define FINE_RISE_STEP       2u
// Falling is the more dangerous direction: the published value sits a little under
// the panel rate whenever the app is being throttled rather than free-running. The
// client averages sixteen present intervals, so one late interval moves the number
// by a whole hertz - 15 x 20.4 ms plus a single 27 ms one reads 48.0, and 20.4 plus
// a doubled 40.8 ms reads 46.1. At a 49 Hz panel the log sat at 47.99 for minutes,
// which is a hertz of jitter, not a hertz of content. A fall therefore has to clear
// 1.8 Hz before it is believed: that follows a full two hertz scene drop, and
// ignores the noise the panel produces by simply holding the app back.
#define FINE_TARGET_FALL     1.80f
// How far above the panel the measurement has to sit before a rise is believed at all.
// The client publishes whole hertz (estimate_present_fps returns unsigned), so a 50 fps
// app in front of a 50 Hz panel reads exactly 50.00 almost all the time and 51.00 when
// one 16-interval window happens to catch only short intervals. That single hertz is
// the entire width of the feedback signal and it is not evidence of anything. Two
// hertz, held, is: under FIFO the app cannot present above the panel's rate, so a
// measurement two hertz clear of it means the app is not the thing being limited.
#define FINE_RISE_EVIDENCE   1.90f

static bool probeArmed = false;      // last applied change went below the demonstrated max
static uint32_t probeFromRate = 0;   // rate we left to test upward, 0 = not testing
static uint32_t probeTarget = 0;
static u64 probeStart = 0;
static u64 probeWindowMs = PROBE_WINDOW_MS;
static uint32_t probeFailedRate = 0;
static u64 probeFailedSince = 0;
static bool probeFromFine = false;   // this test came from the measured path
static uint32_t probeCount = 0;      // capped, so a game wobbling on the boundary
static const uint32_t probeLimit = 3; // cannot ping-pong the panel forever
static u64 sameRateSince = 0;
static u64 finePinnedSince = 0;
static bool minRateClampLogged = false;

// ---------------------------------------------------------------------------
// Rate sweep: answers "does this panel actually take 49 Hz?" without needing a
// client that can ask for it.
//
// Neither FPSLocker nor NetherSX2 can publish anything outside their 5 Hz table, so
// the only way to find out whether the panel locks at 48, 49, 51 or 52 is to drive it
// from here. Put the rates in
//
//     sdmc:/SaltySD/flags/test_rates.txt        45 46 47 48 49 50 51 52
//
// and reboot. While an application runs, the sysmodule steps through them five seconds
// apart, printing what the hardware reports back through the PLLD registers. Without
// the file nothing happens at all. Display sync is suspended for the duration so the
// two cannot fight over the panel, and the rate is returned to 60 Hz when the list
// runs out.
#define TEST_SWEEP_FILE    "sdmc:/SaltySD/flags/test_rates.txt"
#define TEST_SWEEP_STEP_MS 5000ULL
#define TEST_SWEEP_MAX     16

static uint32_t testSweepRates[TEST_SWEEP_MAX];
static uint32_t testSweepCount = 0;
static uint32_t testSweepIndex = 0;
static u64 testSweepNextStep = 0;
static bool testSweepDone = false;

// ---------------------------------------------------------------------------
// Fine mode: let the panel follow the rate the game is actually presenting at, to the
// hertz, instead of snapping to the client's 5 Hz table.
//
// The measured source is NX-FPS itself: its present hook writes an EMA of the frame
// interval into the shared block as FPSavg on every presented frame, for NVN, EGL and
// Vulkan. That is a real measurement from the game, independent of whatever the client
// publishes into FPSlocked, so this works with any game and not just NetherSX2.
//
// Opt-in, because it is the experiment: it allows rates the normal mode refuses.
//
//     sdmc:/SaltySD/flags/sync_fine.flag        (create, then reboot)
//
// The 1 Hz deadband has to go or nothing would ever move by one, and the 20 s drop is
// pointless once the target itself only moves by a hertz: a real one-hertz change is
// followed in 4 s, a large swing still gets the 1.5 s treatment.
#define FINE_SYNC_FILE "sdmc:/SaltySD/flags/sync_fine.flag"
#define FINE_SYNC_OFF_FILE "sdmc:/SaltySD/flags/sync_fine_off.flag"

// On by default since 2.1.26. Without it the panel can only ever sit on the client's
// 5 Hz table, which is not a smooth 45..60 window - and now that the floor is 45 there
// is nothing left for it to protect. The flag file is still honoured so an install that
// already has it keeps working, and a file next to it turns the path off again.
static bool fineSyncEnabled = true;

static void loadFineSyncFlag() {
    const bool off = file_or_directory_exists(FINE_SYNC_OFF_FILE);
    if (file_or_directory_exists(FINE_SYNC_FILE))
        SaltyNX_printf(APP_NAME ": fine sync enabled from " FINE_SYNC_FILE ", following the measured rate to the hertz\n");
    else if (off)
        SaltyNX_printf(APP_NAME ": fine sync disabled by " FINE_SYNC_OFF_FILE ", the panel follows the client target only\n");
    else
        SaltyNX_printf(APP_NAME ": fine sync enabled by default, following the measured rate to the hertz\n");
    fineSyncEnabled = !off;
}

// A client publishes FPSavg as a whole number, once a second, from a window of
// sixteen frames. One hitch - a stalled worker thread, a 2.6 s blocking read -
// inflates the mean interval enough to drag that number down by five hertz for a
// whole second, and the next second it is back. A one-hertz target cannot follow
// that, so the samples are gated to one per second, the ones that jump are
// refused, and the rest are averaged. Ten refusals in a row is not noise any
// more, that is a different app or a different scene, so the average restarts
// from the new value.
// How often the published value is read. This used to be 900 ms, which was chosen to
// match a client that publishes once a second - but it meant a value that arrived just
// after a sample waited up to 900 ms to be seen, on top of the client's own delay.
// Reading four times a second and letting the time constant below do the smoothing is
// the same filter with less latency, because a coefficient derived from the interval
// makes the wall-clock behaviour independent of the sampling rate. Sampling faster than
// the client publishes adds no information and no noise: the same value read twice
// simply moves the average by the smaller step it now deserves.
#define FINE_SAMPLE_MS     100ULL
// Time constant of the averaging filter, chosen so the wall-clock smoothing is exactly
// what it has always been. The old filter applied a fixed 0.35 once per 900 ms sample -
// it kept 0.65 of the error every 900 ms, which is not exp(-900/2571), the two differ.
// Reproducing that with nine 100 ms sub-steps needs (1 - alpha)^9 = 0.65, so alpha is
// 0.04674 and the constant is 2140 ms. Deriving alpha from the measured interval instead
// of hard-coding it is what lets the sampling rate change without the smoothing changing
// with it; a linear ramp stands in for 1 - exp(-dt/tau), which at these step sizes agrees
// to better than a tenth of a percent and costs no libm call.
#define FINE_TAU_MS      2140ULL
#define FINE_REJECT_HZ   2.0f
// How long a jump has to persist before it is believed. This is the single largest delay
// in the whole loop, and it was set to nine seconds for a reason that no longer holds:
// it was "ten samples in a row", which at the old 900 ms sampling really was nine
// seconds of protection against a hitch. Two seconds still outlasts any hitch - the
// client's own window is 320 ms and a stalled second is a handful of samples - and it
// takes a real scene change from thirteen seconds to six. Measured against dips of one
// to ten seconds, two seconds holds the same line as nine did: anything up to three
// seconds is ignored, anything from five seconds is followed.
#define FINE_REJECT_MS   2000ULL
// Unusable samples are a different case: 0 and 255 are not slowdowns, they are a client
// that has not published yet or an app that stopped presenting. Nine seconds of patience
// costs nothing when the alternative is dropping to the client's quantised path.
#define FINE_INVALID_MS  9000ULL

static float fineAvg = 0.0f;
static bool fineAvgValid = false;
static u64 fineSampleTick = 0;
static u64 fineRejectSince = 0;
static u64 fineInvalidSince = 0;
static float fineLastRaw = 0.0f;
// Highest smoothed value this app has shown. It is the only evidence that a higher
// rate is worth testing: while the panel holds the app down, the app never measures
// above the panel, so "the app is capable of more" cannot be observed at the moment
// it matters - only remembered from before the drop.
static float finePeak = 0.0f;
// The rate this path is currently asking the panel for, and the evidence counters the
// escape test keeps. fineEverMeasured: the app has published at least one usable
// value, so a later silence is a pause, not a missing publisher.
static uint32_t fineTarget = 0;
static bool fineEverMeasured = false;
static uint32_t probeFailCount = 0;
// Set while a fine probe has just been followed, cleared as soon as one is not. A
// climb is the one state in which the next probe is worth spending without asking for
// more evidence first - the previous one already proved the panel was the limiter.
static bool fineClimbing = false;

static void fineSampleMeasurement() {
    if (!fineSyncEnabled || isDocked || !nx_fps) {
        fineAvgValid = false;
        fineRejectSince = 0;
        fineInvalidSince = 0;
        return;
    }
    const u64 now = svcGetSystemTick();
    const float avg = nx_fps->FPSavg;
    const u64 sinceMs = fineSampleTick ? (now - fineSampleTick) / SV_TICKS_PER_MS : FINE_SAMPLE_MS;
    if (fineSampleTick && sinceMs < FINE_SAMPLE_MS)
        return;
    fineSampleTick = now;
    fineLastRaw = avg;
    // FPSavg is 255 in NX-FPS before the first frame, 0 before a client
    // publishes anything, and lands in 14..29 for a second or two after a
    // multi-second hitch - the client measures frame intervals, and one stall
    // dominates a sixteen frame window. Discarding the whole average over one
    // such sample is exactly what the jump rejection was meant to prevent: the
    // panel would fall back to the client's 5 Hz path, floor and probe
    // included, and climb to 55. So a single unusable sample is skipped and
    // only a sustained absence of any usable value gives up the average.
    if (!(avg >= 30.0f && avg <= 240.0f)) {
        if (fineAvgValid) {
            if (fineInvalidSince == 0) {
                fineInvalidSince = now;
                return;
            }
            if (now - fineInvalidSince < FINE_INVALID_MS * SV_TICKS_PER_MS)
                return;
        }
        fineAvgValid = false;
        fineAvg = 0.0f;
        fineRejectSince = 0;
        fineInvalidSince = 0;
        return;
    }
    fineInvalidSince = 0;
    fineEverMeasured = true;
    if (!fineAvgValid) {
        fineAvg = avg;
        fineAvgValid = true;
        fineRejectSince = 0;
        return;
    }
    if (avg < fineAvg - FINE_REJECT_HZ || avg > fineAvg + FINE_REJECT_HZ) {
        if (fineRejectSince == 0)
            fineRejectSince = now;
        else if (now - fineRejectSince >= FINE_REJECT_MS * SV_TICKS_PER_MS) {
            fineAvg = avg;
            fineRejectSince = 0;
        }
        return;
    }
    fineRejectSince = 0;
    float alpha = (float)sinceMs / (float)FINE_TAU_MS;
    if (alpha > 1.0f) alpha = 1.0f;
    if (alpha < 0.002f) alpha = 0.002f;
    fineAvg += (avg - fineAvg) * alpha;
    if (fineAvg > finePeak)
        finePeak = fineAvg;
}

// Returns 0 when there is nothing trustworthy to follow.
static uint32_t fineMeasuredTarget() {
    if (!fineAvgValid) {
        // Keep fineTarget. It is the anchor the deadband below compares against, and
        // clearing it here was how an overshoot survived: one unusable sample, then the
        // average restarts from a fresh number and whatever that number rounds to wins
        // outright. The panel is still where it was; the next sample can still be
        // measured against it.
        return 0;
    }
    const float avg = fineAvg;
    uint32_t target = (uint32_t)(avg + 0.5f);
    if (target < HandheldModeRefreshRateAllowed.min)
        target = HandheldModeRefreshRateAllowed.min;
    else if (target > HandheldModeRefreshRateAllowed.max)
        target = HandheldModeRefreshRateAllowed.max;
    // Stay where we are while the measurement is still grazing the rate we already
    // asked for. Without this a smoothed 48.9 rounds to 49, drops the panel, and the
    // next sample rounds back to 50 - a mode switch every few seconds that looks
    // exactly like the judder it was supposed to remove.
    if (fineTarget && target != fineTarget) {
        const float delta = avg - (float)fineTarget;
        if (delta < 0.0f) {
            if (delta > -FINE_TARGET_FALL)
                target = fineTarget;
        }
        else if (delta < FINE_TARGET_HYST) {
            target = fineTarget;
        }
    }
    fineTarget = target;
    return target;
}

static void loadTestSweep() {
    FILE* f = fopen(TEST_SWEEP_FILE, "r");
    if (!f)
        return;
    char buf[128] = {0};
    const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    uint32_t value = 0;
    bool any = false;
    for (size_t i = 0; i <= got; i++) {
        const char c = (i < got) ? buf[i] : ' ';
        if (c >= '0' && c <= '9') {
            if (value <= 1000)
                value = value * 10 + (uint32_t)(c - '0');
            any = true;
        }
        else if (any) {
            if (value >= 1 && value <= 240 && testSweepCount < TEST_SWEEP_MAX)
                testSweepRates[testSweepCount++] = value;
            value = 0;
            any = false;
        }
    }
    if (!testSweepCount)
        return;
    SaltyNX_printf(APP_NAME ": sweep: %u rate(s) from " TEST_SWEEP_FILE ", %u s apart\n",
                   testSweepCount, (u32)(TEST_SWEEP_STEP_MS / 1000));
}

// Returns true when the sweep owns the panel this tick.
static bool runTestSweep() {
    // Hands the panel back to the normal logic once the list has been walked, so the
    // sweep cannot leave display sync suspended for the rest of the session.
    if (!testSweepCount || isDocked || testSweepDone)
        return false;
    const u64 now = svcGetSystemTick();
    if (testSweepNextStep && now < testSweepNextStep)
        return true;

    if (testSweepIndex >= testSweepCount) {
        testSweepDone = true;
        uint32_t achieved = 0;
        SetDisplayRefreshRate(60);
        if (GetDisplayRefreshRate(&achieved, true))
            SaltyNX_printf(APP_NAME ": sweep: done, back to 60 Hz (active %u Hz)\n", achieved);
        else
            SaltyNX_printf(APP_NAME ": sweep: done, back to 60 Hz\n");
        return true;
    }

    const uint32_t want = testSweepRates[testSweepIndex++];
    uint32_t before = 0;
    GetDisplayRefreshRate(&before, true);
    const bool setOk = SetDisplayRefreshRate(want);
    uint32_t after = 0;
    const bool readOk = GetDisplayRefreshRate(&after, true);
    if (readOk)
        SaltyNX_printf(APP_NAME ": sweep: %u Hz requested from %u Hz, active %u Hz%s\n",
                       want, before, after,
                       (after == want) ? "" : ((setOk) ? "  <-- did not land" : "  <-- refused"));
    else
        SaltyNX_printf(APP_NAME ": sweep: %u Hz requested from %u Hz, readback failed (set %s)\n",
                       want, before, setOk ? "ok" : "refused");
    testSweepNextStep = now + TEST_SWEEP_STEP_MS * SV_TICKS_PER_MS;
    return true;
}

void cleanAppData() {
    lastAppPID = -1;
    nx_fps = 0;
    cheatCheck = false;
    pendingRefreshRate = 0;
    pendingRefreshRequired = 0;
    probeArmed = false;
    probeFromRate = 0;
    probeTarget = 0;
    probeFailedRate = 0;
    probeFailedSince = 0;
    probeFromFine = false;
    probeWindowMs = PROBE_WINDOW_MS;
    probeCount = 0;
    sameRateSince = 0;
    finePinnedSince = 0;
    finePeak = 0.0f;
    fineTarget = 0;
    fineEverMeasured = false;
    probeFailCount = 0;
    fineClimbing = false;
    fineRejectSince = 0;
    fineInvalidSince = 0;
    fineLastRaw = 0.0f;
    minRateClampLogged = false;
    testSweepIndex = 0;
    testSweepNextStep = 0;
    testSweepDone = false;
    uintptr_t shmem = (uintptr_t)shmemGetAddr(&_sharedMemory);
    if (shmem) {
        memset((void*)(shmem+4), 0, shmem_size-4);
    }
    if ((!isDocked && displaySync) || (isDocked && displaySyncDocked)) {
        uint32_t temp_refreshRate = 0;
        if (GetDisplayRefreshRate(&temp_refreshRate, true) && temp_refreshRate != 60)
            SetDisplayRefreshRate(60);
        refreshRate = 0;
    }
    if (openedFilesAmount) {
        for (size_t i = 0; i < openedFilesAmount; i++) {
            fclose(openedFilesArray[i].file);
        }
        memset(openedFilesArray, 0, sizeof(openedFilesArray));
        openedFilesAmount = 0;
    }
    if (openedDirsAmount) {
        for (size_t i = 0; i < openedDirsAmount; i++) {
            closedir(openedDirsArray[i].dir);
        }
        memset(openedDirsArray, 0, sizeof(openedDirsArray));
        openedDirsAmount = 0;
    }
}

// How long to wait before testing the same rate again. Every consecutive failure
// doubles it, so a rate that is genuinely impossible stops costing an excursion at
// all after a handful of tries, while a scene change still gets retried within
// minutes. A success resets the count: the panel is following the app again.
static u64 fineRetryWindowMs() {
    u64 ms = FINE_RETRY_MS;
    for (uint32_t i = 1; i < probeFailCount && ms < FINE_RETRY_MAX_MS; i++)
        ms *= 2;
    return ms > FINE_RETRY_MAX_MS ? FINE_RETRY_MAX_MS : ms;
}

void updateRefreshRateForApp() {
    if (runTestSweep()) {
        pendingRefreshRate = 0;
    }
    else if ((!isDocked && displaySync) || (isDocked && displaySyncDocked)) {
        uint32_t temp_refreshRate = 0;
        GetDisplayRefreshRate(&temp_refreshRate, true);
        uint32_t requestedRate = nx_fps ? (isDocked ? nx_fps->FPSlockedDocked : nx_fps->FPSlocked) : 0;
        fineSampleMeasurement();
        uint32_t fineRate = (fineSyncEnabled && !isDocked) ? fineMeasuredTarget() : 0;
        // The panel is never raised on a one hertz measurement. Under FIFO an app
        // cannot present faster than the panel, so anything it publishes while the
        // panel is holding it is that panel's rate plus noise - and a panel one hertz
        // above the content repeats a frame every second, which is a hitch the loop
        // would have introduced itself. Two hertz of the panel's own rate is the only
        // evidence that the app wants to go faster, and once the panel does move, the
        // climb continues one hertz at a time through the probe above.
        if (fineRate && temp_refreshRate && fineRate > temp_refreshRate &&
            fineAvg < (float)temp_refreshRate + FINE_RISE_EVIDENCE)
            fineRate = temp_refreshRate;
        if (fineSyncEnabled && !isDocked) {
            // Fine mode is only as good as the measurement behind it. NX-FPS installs
            // its present hook by rewriting the game's import table, so an app that
            // resolves vkQueuePresentKHR at runtime - any Vulkan wrapper - never calls
            // the hooked copy and FPSavg stays 0 forever. That is indistinguishable
            // from "the panel just did not move" unless it is said out loud.
            static u64 lastFineLog = 0;
            static u64 fineNoDataSince = 0;
            static bool fineNoDataLogged = false;
            const u64 nowFine = svcGetSystemTick();
            const uint32_t measured = fineMeasuredTarget();
            if (measured) {
                fineNoDataSince = 0;
                fineNoDataLogged = false;
            }
            else if (nx_fps && !fineEverMeasured) {
                if (fineNoDataSince == 0)
                    fineNoDataSince = nowFine;
                else if (!fineNoDataLogged && nowFine - fineNoDataSince >= 30000ULL * SV_TICKS_PER_MS) {
                    fineNoDataLogged = true;
                    const uint32_t avg_x100 = (uint32_t)(nx_fps->FPSavg * 100.0f);
                    SaltyNX_printf(APP_NAME ": fine: no measurement after 30 s (FPS=%u FPSavg=%u.%02u) - "
                                   "nothing is publishing FPSavg for this app, "
                                   "falling back to the client target\n",
                                   (u32)nx_fps->FPS, avg_x100 / 100, avg_x100 % 100);
                }
            }
            if (nowFine - lastFineLog >= 30000ULL * SV_TICKS_PER_MS) {
                lastFineLog = nowFine;
                const uint32_t avg_x100 = nx_fps ? (uint32_t)(nx_fps->FPSavg * 100.0f) : 0;
                const uint32_t smooth_x100 = (uint32_t)(fineAvg * 100.0f);
                SaltyNX_printf(APP_NAME ": fine: block=%s FPS=%u FPSavg=%u.%02u smooth=%u.%02u target=%u panel=%u\n",
                               nx_fps ? "found" : "missing",
                               nx_fps ? (u32)nx_fps->FPS : 0,
                               avg_x100 / 100, avg_x100 % 100,
                               smooth_x100 / 100, smooth_x100 % 100,
                               fineRate, temp_refreshRate);
            }
        }
        uint32_t check_refresh_rate = fineRate ? fineRate : (requestedRate ? requestedRate : refreshRate);
        if (check_refresh_rate == 0)
            check_refresh_rate = 60;
        // The 50 Hz floor exists to stop the loop from dragging itself down through the
        // client's quantised target. Fine mode follows a real measurement instead, so it
        // is allowed the panel's own minimum.
        if (!fineRate && !isDocked && requestedRate && check_refresh_rate < DISPLAY_SYNC_MIN_RATE) {
            if (!minRateClampLogged) {
                minRateClampLogged = true;
                SaltyNX_printf(APP_NAME ": display-sync: ignoring the %u Hz target, this sync never goes below %u Hz\n",
                               check_refresh_rate, DISPLAY_SYNC_MIN_RATE);
            }
            check_refresh_rate = DISPLAY_SYNC_MIN_RATE;
        }
        if (check_refresh_rate != 60 && nx_fps && nx_fps->forceOriginalRefreshRate && (!isDocked || (isDocked && !dontForce60InDocked))) {
            check_refresh_rate = 60;
        }
        if (check_refresh_rate != 60 && ((isDocked && displaySyncDockedOutOfFocus60) || (!isDocked && displaySyncOutOfFocus60))) {
            bool isOutOfFocus = true;
            if (R_SUCCEEDED(isApplicationOutOfFocus(&isOutOfFocus)) && isOutOfFocus) {
                check_refresh_rate = 60;
            }
        }

        const u64 now = svcGetSystemTick();

        if (probeFromRate) {
            // Testing upward: hold the panel still and watch what the app publishes.
            // The target only counts as followed if it comes back to the rate being
            // tested - "it moved up somehow" is not the same thing, and 2.1.19 read a
            // target of 55 at 60 Hz as proof that 50 had been the panel's ceiling.
            // The client path quantises to 5 Hz, so one hertz of slack there; the
            // measured path does not, and a panel one hertz above what the app can fill
            // is a judder the test itself created.
            // The fine test has to read the measurement, not the target. The target is
            // deliberately frozen by FINE_TARGET_HYST, which is 1.2 Hz wide, and a fine
            // probe moves one hertz - so a probe could never be ratified through the
            // target, the panel fell back six seconds later, and the only direction that
            // ever stuck was down. On the device that showed up as 45 -> 46 -> 45 -> 46
            // forever after a single dip, never once recovering to 50.
            const bool followed = probeFromFine
                                ? (fineAvgValid && fineAvg + 0.5f >= (float)probeTarget)
                                : (check_refresh_rate + 1u >= probeTarget);
            if (followed) {
                uint32_t achieved = 0;
                if (GetDisplayRefreshRate(&achieved, true))
                    SaltyNX_printf(APP_NAME ": display-sync: %u Hz was not a lock, target followed to %u Hz (active %u Hz)\n",
                                   probeFromRate, check_refresh_rate, achieved);
                else
                    SaltyNX_printf(APP_NAME ": display-sync: %u Hz was not a lock, target followed to %u Hz\n",
                                   probeFromRate, check_refresh_rate);
                // A fine probe that the app followed is the only evidence that can move
                // this loop upward while the panel sets the pace, so the rate it proved
                // becomes the anchor the deadband compares against. Without that the
                // deadband immediately argues for the rate we just left and the gain is
                // undone in four seconds.
                if (probeFromFine) {
                    fineTarget = probeTarget;
                    fineClimbing = true;
                }
                probeFromRate = 0;
                probeArmed = false;
                probeFromFine = false;
                probeTarget = 0;
                probeFailCount = 0;
                sameRateSince = 0;
                pendingRefreshRate = 0;
            }
            else if (check_refresh_rate < probeFromRate || now - probeStart >= probeWindowMs * SV_TICKS_PER_MS) {
                uint32_t achieved = 0;
                SetDisplayRefreshRate(probeFromRate);
                probeFailedRate = probeFromRate;
                probeFailedSince = now;
                if (probeFailCount < 8)
                    probeFailCount++;
                if (probeFromFine)
                    fineClimbing = false;
                if (GetDisplayRefreshRate(&achieved, true))
                    SaltyNX_printf(APP_NAME ": display-sync: %u Hz is real, back from the test (target %u Hz, active %u Hz)\n",
                                   probeFromRate, check_refresh_rate, achieved);
                else
                    SaltyNX_printf(APP_NAME ": display-sync: %u Hz is real, back from the test (target %u Hz)\n",
                                   probeFromRate, check_refresh_rate);
                probeFromRate = 0;
                probeArmed = false;
                probeFromFine = false;
                probeTarget = 0;
                sameRateSince = now;
                pendingRefreshRate = 0;
            }
            else {
                pendingRefreshRate = 0;
            }
        }
        else if (temp_refreshRate != check_refresh_rate) {
            const uint32_t delta = (check_refresh_rate > temp_refreshRate)
                                 ? (check_refresh_rate - temp_refreshRate)
                                 : (temp_refreshRate - check_refresh_rate);
            sameRateSince = 0;
            if (delta <= (fineRate ? 0u : REFRESH_RATE_DEADBAND_HZ)) {
                // One hertz is below what the eye resolves and costs a full mode
                // switch, so it is always measurement noise, never an intent.
                pendingRefreshRate = 0;
            }
            else if (check_refresh_rate != pendingRefreshRate) {
                // Only a large *drop* is worth acting on quickly: that is a game
                // switching from 60 to a PAL cap, or a loading screen ending. A large
                // rise gets the slow treatment, because a client can publish 60 for one
                // sample right after a gameplay stretch and the panel would visibly jump
                // to 60 and straight back.
                u64 requiredMs;
                if (fineRate)
                    requiredMs = (check_refresh_rate < temp_refreshRate &&
                                  delta > REFRESH_RATE_SMALL_DELTA)
                               ? REFRESH_RATE_LARGE_MS
                               : REFRESH_RATE_FINE_MS;
                else if (check_refresh_rate > temp_refreshRate)
                    requiredMs = REFRESH_RATE_SMALL_UP_MS;
                else if (delta <= REFRESH_RATE_SMALL_DELTA)
                    requiredMs = REFRESH_RATE_SMALL_DOWN_MS;
                else
                    requiredMs = REFRESH_RATE_LARGE_MS;
                pendingRefreshRate = check_refresh_rate;
                pendingRefreshSince = now;
                pendingRefreshRequired = requiredMs * SV_TICKS_PER_MS;
            }
            else if (now - pendingRefreshSince >= pendingRefreshRequired) {
                const u32 heldMs = (u32)((now - pendingRefreshSince) / SV_TICKS_PER_MS);
                uint32_t achievedRate = 0;
                // Fine mode never jumps. On the device a single measurement took the
                // panel from 45 to 60 Hz after 1.5 s - fifteen mode switches' worth of
                // change on the strength of a number that had been 45.09 fourteen
                // seconds earlier. Two hertz a step means a wrong reading costs one
                // step and falls straight back, while a real scene change still gets
                // there, one hold at a time.
                uint32_t applyRate = check_refresh_rate;
                if (fineRate && applyRate > temp_refreshRate + FINE_RISE_STEP)
                    applyRate = temp_refreshRate + FINE_RISE_STEP;
                SetDisplayRefreshRate(applyRate);
                // The panel quantises and clamps (25 Hz is served as 50 Hz on OLED),
                // so report what the hardware ended up at, not what we asked for.
                if (GetDisplayRefreshRate(&achievedRate, true))
                    SaltyNX_printf(APP_NAME ": display-sync: refresh rate %u -> %u Hz requested (target held %u ms, active %u Hz%s)\n",
                                   temp_refreshRate, applyRate, heldMs, achievedRate,
                                   fineRate ? ", measured" : "");
                else
                    SaltyNX_printf(APP_NAME ": display-sync: refresh rate %u -> %u Hz requested (target held %u ms%s)\n",
                                   temp_refreshRate, applyRate, heldMs,
                                   fineRate ? ", measured" : "");
                pendingRefreshRate = 0;
                // A drop to the floor is the only one worth second-guessing: below it
                // there is no room left to move down, and the panel holding the app
                // down looks exactly like a slow game. Arm one test, and keep the
                // excursion small - a 50 -> 60 jump is far more visible than 50 -> 55.
                // Only a *small* drop can be the lock-in case: the panel sits just above
                // the floor, the client measures the panel instead of the game, and the
                // two converge. 60 -> 50 is the client asking for 50, not a lock-in.
                probeArmed = !fineRate &&
                             (temp_refreshRate - check_refresh_rate <= REFRESH_RATE_SMALL_DELTA) &&
                             (check_refresh_rate == DISPLAY_SYNC_MIN_RATE) &&
                             (temp_refreshRate <= HandheldModeRefreshRateAllowed.max) &&
                             (temp_refreshRate != probeFailedRate) &&
                             (probeCount < probeLimit);
                if (probeArmed) {
                    uint32_t up = DISPLAY_SYNC_MIN_RATE + REFRESH_RATE_SMALL_DELTA;
                    if (up > temp_refreshRate)
                        up = temp_refreshRate;
                    if (up > HandheldModeRefreshRateAllowed.max)
                        up = HandheldModeRefreshRateAllowed.max;
                    probeTarget = up;
                    probeFromFine = false;
                    probeWindowMs = PROBE_WINDOW_MS;
                }
            }
        }
        else {
            pendingRefreshRate = 0;
            // Ambiguous only when the smoothed measurement sits on the panel rate: a
            // 42 next to a 45 Hz panel is the app being the limiter and there is
            // nothing to prove. Agreement for fifteen seconds is the lock shape.
            //
            // And only when the app has demonstrated, earlier in this session, that it
            // can go above the rate it is stuck on. Without that the test would fire on
            // every correctly running game - a title sitting happily at 50 would be
            // pushed to 51 every fifteen seconds forever, which is a defect of its own.
            const float agree = fineAvg - (float)temp_refreshRate;
            const bool measurementOnPanel = fineAvgValid && agree < 0.6f && agree > -0.6f;
            // Three reasons to spend an excursion, and no others:
            //  - the app was measured above this rate earlier in the session, so the
            //    panel is probably what is holding it back;
            //  - the panel is sitting on the floor, which is a clamp rather than a
            //    content-derived rate - nothing else can ever lift it off 45, so it is
            //    owed a test even when nothing has been demonstrated;
            //  - a climb is already in progress, so the previous probe proved the point
            //    and the next hertz is worth trying.
            const bool worthTesting = finePeak >= (float)temp_refreshRate + 1.0f ||
                                      temp_refreshRate <= DISPLAY_SYNC_MIN_RATE ||
                                      fineClimbing;
            if (fineRate && measurementOnPanel && worthTesting &&
                temp_refreshRate < HandheldModeRefreshRateAllowed.max && !probeFromRate) {
                if (finePinnedSince == 0)
                    finePinnedSince = now;
                const bool parked = (temp_refreshRate == probeFailedRate) &&
                                    (now - probeFailedSince < fineRetryWindowMs() * SV_TICKS_PER_MS);
                // A floor probe and a continuing climb do not wait out the fifteen
                // seconds: in both cases the reason is already established, and waiting
                // only slows the recovery from a dip that landed on the clamp.
                const bool reasonKnown = fineClimbing || temp_refreshRate <= DISPLAY_SYNC_MIN_RATE;
                if (!probeArmed && !parked &&
                    (reasonKnown || now - finePinnedSince >= FINE_LOCK_SUSPECT_MS * SV_TICKS_PER_MS))
                    probeArmed = true;
                if (probeArmed && probeTarget == 0) {
                    uint32_t up = temp_refreshRate + FINE_LOCK_STEP;
                    if (up > HandheldModeRefreshRateAllowed.max)
                        up = HandheldModeRefreshRateAllowed.max;
                    probeTarget = up;
                    probeFromFine = true;
                    probeWindowMs = FINE_PROBE_WINDOW_MS;
                }
            }
            else if (!measurementOnPanel) {
                // Do not end the climb here. Right after a probe the panel has moved but
                // the average has not caught up yet, so the two disagree for a few
                // seconds by construction. A climb ends when a probe fails or when the
                // loop moves the panel down, both of which are handled where they happen.
                finePinnedSince = 0;
            }
            if (probeArmed && probeTarget > check_refresh_rate) {
                if (sameRateSince == 0)
                    sameRateSince = now;
                else if (now - sameRateSince >= (probeFromFine ? 0ULL : PROBE_HOLD_MS) * SV_TICKS_PER_MS) {
                    uint32_t up = (probeTarget > HandheldModeRefreshRateAllowed.max)
                                ? HandheldModeRefreshRateAllowed.max
                                : probeTarget;
                    probeArmed = false;
                    probeFromRate = check_refresh_rate;
                    probeStart = now;
                    sameRateSince = 0;
                    if (!probeFromFine)
                        probeCount++;
                    SetDisplayRefreshRate(up);
                    // Announce a candidate once. Re-trying the same rate is a routine
                    // retry, and logging it every minute was most of the log.
                    if (!probeFromFine || probeFailCount == 0)
                        SaltyNX_printf(APP_NAME ": display-sync: testing %u Hz against %u Hz, which the app held before\n",
                                       check_refresh_rate, up);
                }
            }
            else {
                sameRateSince = 0;
            }
        }
    }
    if (!isDocked && nx_fps && nx_fps->FPSlocked > HandheldModeRefreshRateAllowed.max) {
        nx_fps->FPSlocked = HandheldModeRefreshRateAllowed.max;
        refreshRate = HandheldModeRefreshRateAllowed.max;
    }
    else if (isDocked && nx_fps) {
        uint8_t highestrr = getDockedHighestRefreshRateAllowed();
        if (nx_fps->FPSlockedDocked > highestrr) {
            nx_fps->FPSlockedDocked = highestrr;
            refreshRate = highestrr;
        }
    }
}

void updateRefreshRate() {
    static bool wasLastDocked = false;
    if ((wasLastDocked && !isDocked && !displaySync) || (!wasLastDocked && isDocked && !displaySyncDocked)) {
        uint32_t temp_refreshRate = 0;
        if (GetDisplayRefreshRate(&temp_refreshRate, true) && temp_refreshRate != 60) {
            SetDisplayRefreshRate(60);
            refreshRate = 0;
        }
    }
    wasLastDocked = isDocked;

    if (isDocked && !displaySyncDocked && nx_fps && nx_fps->FPSlockedDocked > 60) {
        uint32_t temp_refreshRate = 0;
        GetDisplayRefreshRate(&temp_refreshRate, true);
        if (temp_refreshRate <= 60) {
            nx_fps->FPSlockedDocked = 60;
        }
    }
    
    uint32_t crr = 0;
    GetDisplayRefreshRate(&crr, true);
    if (isOLED) correctOledGamma(crr);

    if (nx_fps) nx_fps -> dontForce60InDocked = dontForce60InDocked;
}

void Initialize() {
    #if !defined(SWITCH) && !defined(OUNCE)
	    systemtickfrequency = armGetSystemTickFreq();
    #endif
    ABORT_IF_FAILED(smInitialize(), 1);
    ABORT_IF_FAILED(fsInitialize(), 2);
    ABORT_IF_FAILED(fsdevMountSdmc(), 3);
    SaltyNX_printf(APP_NAME " " APP_VERSION ": got SD card.\n");
    // Say which logging mode this boot is in, so a log never leaves it ambiguous why
    // it is short. Only printed when logging is on at all.
    if (SaltyNX_LogLevel() != SALTYNX_LOG_SILENT)
        SaltyNX_printf(APP_NAME ": logging is opt-in: log.flag writes decisions, log.full.flag adds the IPC trace\n");
    loadTestSweep();
    loadFineSyncFlag();

    
    ABORT_IF_FAILED(setsysInitialize(), 10);

    SetSysFirmwareVersion fw;
    Result rc = setsysGetFirmwareVersion(&fw);
    if (R_SUCCEEDED(rc)) {
        hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
    }
    else {
        SaltyNX_printf(APP_NAME ": Couldn't retrieve Firmware Version! rc: 0x%x.\n", rc);
    }

    SetSysProductModel model;
    if (R_SUCCEEDED(setsysGetProductModel(&model))) {
        if (model == SetSysProductModel_Aula) {
            SaltyNX_printf(APP_NAME ": Detected OLED model. Locking minimum refresh rate to 45 Hz.\n");
            isOLED = true;
            HandheldModeRefreshRateAllowed.min = 45;
        }
        else if (model == SetSysProductModel_Hoag) {
            isLite = true;
            SaltyNX_printf(APP_NAME ": Detected Lite model. Docked refresh rate will be blocked.\n");
        }
    }
    
    ABORT_IF_FAILED(nvInitialize(), 6);
    u32 fd = 0;
    if (R_FAILED(nvOpen(&fd, "/dev/nvdisp-disp0"))) {
        SaltyNX_printf(APP_NAME ": Couldn't open /dev/nvdisp-disp0! Can't check if using Retro Remake display.\n");
    }
    else {
        struct vendorID {
            u8 vendor;
            u8 model;
            u8 board;
        };
        struct vendorID _vendorid = {0};
        Result nvrc = nvIoctl(fd, NVDISP_PANEL_GET_VENDOR_ID, &_vendorid);
        nvClose(fd);
        if (R_SUCCEEDED(nvrc)) {
            if (_vendorid.vendor == 0xE0 || _vendorid.vendor == 0xE1) {
                isRetroSUPER = true;
            }
            if (_vendorid.vendor == 0x20 && _vendorid.model == 0x94 && _vendorid.board == 0x10) {
                isPossiblySpoofedRetro = true;
            }
        }
    }
    if (!isLite) {
        if (file_or_directory_exists("sdmc:/SaltySD/plugins/FPSLocker/ExtDisplays") == false) {
            mkdir("sdmc:/SaltySD/plugins", 69);
            mkdir("sdmc:/SaltySD/plugins/FPSLocker", 420);
            mkdir("sdmc:/SaltySD/plugins/FPSLocker/ExtDisplays", 2137);
        }
    }
    ABORT_IF_FAILED(ldrDmntInitialize(), 7);
    Service* ldrDmntSrv = ldrDmntGetServiceSession();
    Service ldrDmntClone;
    serviceClone(ldrDmntSrv, &ldrDmntClone);
    serviceClose(ldrDmntSrv);
    memcpy(ldrDmntSrv, &ldrDmntClone, sizeof(Service));

    ABORT_IF_FAILED(pdmqryInitialize(), 8);
    Service* pdmqrySrv = pdmqryGetServiceSession();
    Service pdmqryClone;
    serviceClone(pdmqrySrv, &pdmqryClone);
    serviceClose(pdmqrySrv);
    memcpy(pdmqrySrv, &pdmqryClone, sizeof(Service));

    if (file_or_directory_exists("sdmc:/SaltySD/flags/displaysync.flag")) {
        displaySync = true;
    }
    if (file_or_directory_exists("sdmc:/SaltySD/flags/displaysyncdocked.flag")) {
        displaySyncDocked = true;
    }
    if (file_or_directory_exists("sdmc:/SaltySD/flags/displaysync_outoffocus.flag")) {
        displaySyncOutOfFocus60 = true;
    }

    // Start our port
    // For some reason, we only have one session maximum (0 reslimit handle related?)	
    svcManageNamedPort(&saltyport, "SaltySD", 1);
    // Second port for display-sync clients living inside the hijacked process. SaltySD
    // accepts one client at a time and Core uses that slot while it bootstraps, so an
    // app running in the same process can get OutOfResource there.
    Result displaySyncPortRc = svcManageNamedPort(&saltyportDisplaySync, DISPLAY_SYNC_PORT_NAME, DISPLAY_SYNC_SESSIONS);
    if (R_FAILED(displaySyncPortRc))
        SaltyNX_printf(APP_NAME ": failed to create the display-sync port: 0x%x\n", displaySyncPortRc);
    svcManageNamedPort(&injectserv, "InjectServ", 1);

    uint64_t dummy = 0;
    rc = svcQueryMemoryMapping(&clkVirtAddr, &dummy, 0x60006000, 0x1000);
    if (R_FAILED(rc)) {
        SaltyNX_printf(APP_NAME ": Retrieving virtual address for 0x60006000 failed. RC: 0x%x.\n", rc);
        clkVirtAddr = 0;
    }
    if (isOLED) {
        Result rc = svcQueryMemoryMapping(&dsiVirtAddr, &dummy, 0x54300000, 0x40000);
        if (R_FAILED(rc)) {
            SaltyNX_printf(APP_NAME ": Retrieving virtual address for 0x54300000 failed. RC: 0x%x.\n", rc);
            dsiVirtAddr = 0;
        }
    }
    shmemCreate(&_sharedMemory, shmem_size, Perm_Rw, Perm_Rw);
    shmemMap(&_sharedMemory);
    memset(shmemGetAddr(&_sharedMemory), 0, shmem_size);

    if (R_SUCCEEDED(displaySyncPortRc)) {
        Result threadRc = threadCreate(&displaySyncThread, serviceThreadDisplaySync,
                                       (void*)(uintptr_t)saltyportDisplaySync,
                                       displaySyncThreadStack, sizeof(displaySyncThreadStack),
                                       0x3B, -2);
        if (R_SUCCEEDED(threadRc))
            threadRc = threadStart(&displaySyncThread);

        if (R_FAILED(threadRc)) {
            SaltyNX_printf(APP_NAME ": display-sync thread failed to start: 0x%x\n", threadRc);
            // Nobody would accept on this port, so a client would connect and then wait
            // forever instead of falling back to the legacy port. Closing our handle is
            // not enough - the name keeps its own reference to the client port - so the
            // name itself has to be deleted with maxSessions == 0.
            Handle removed = INVALID_HANDLE;
            Result removeRc = svcManageNamedPort(&removed, DISPLAY_SYNC_PORT_NAME, 0);
            if (R_FAILED(removeRc))
                SaltyNX_printf(APP_NAME ": failed to unregister port %s: 0x%x\n",
                               DISPLAY_SYNC_PORT_NAME, removeRc);
            svcCloseHandle(saltyportDisplaySync);
            saltyportDisplaySync = INVALID_HANDLE;
        }
        else {
            SaltyNX_printf(APP_NAME " " APP_VERSION ": display-sync endpoint ready (%s, %d sessions, handle 0x%x)\n",
                           DISPLAY_SYNC_PORT_NAME, DISPLAY_SYNC_SESSIONS, saltyportDisplaySync);
            SaltyNX_printf(APP_NAME ": display-sync: integer refresh rates enabled (%u..%u Hz, 1 Hz deadband)\n",
                           HandheldModeRefreshRateAllowed.min, HandheldModeRefreshRateAllowed.max);
        }
    }
}

int main(int argc, char *argv[])
{   

    Initialize();
    
    // Main service loop
    u64* pids = malloc(0x200 * sizeof(u64));
    u64 max = 0;
    while (1)
    {
        s32 num;
        static s32 init_num = 0;
        svcGetProcessList(&num, pids, 0x200);
        if (!init_num) init_num = num;
        u64 old_max = max;
        for (int i = init_num; i < num; i++)
        {
            if (pids[i] > max)
            {
                max = pids[i];
            }
        }
        
        if (lastAppPID != -1) {
            if (!nx_fps) updateNxFpsPointer();
            if (!cheatCheck) checkIfDmntChtNeedsToBeActive();
            bool found = false;
            for (int i = num - 1; lastAppPID <= pids[i]; i--)
            {
                if (pids[i] == lastAppPID)
                {	
                    found = true;
                    break;
                }
            }
            if (!found) cleanAppData();
            else updateRefreshRateForApp();
        }

        updateRefreshRate();

        // Detected new PID
        if (max != old_max && max > 0x80)
        {
            hijack_pid(max);
        }
        
        // If someone is waiting for us, handle them.
        if (R_SUCCEEDED(svcWaitSynchronizationSingle(saltyport, 9000000)))
        {
            serviceThread();
        }
        if (R_SUCCEEDED(svcWaitSynchronizationSingle(injectserv, 1000000))) {
            Handle sesja;
            svcAcceptSession(&sesja, injectserv);
            svcCloseHandle(sesja);
        }
    }
    free(pids);

    return 0;
}
