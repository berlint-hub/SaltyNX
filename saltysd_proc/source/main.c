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
#define REFRESH_RATE_DEADBAND_HZ   1u
#define SV_TICKS_PER_MS        19200ULL

// Display sync is not allowed to take the handheld panel below this. The client
// derives its target from a measured cadence, so a slow scene publishes 45 - and once
// the panel sits at 45 the loop cannot tell that from a game that really runs at 45,
// because by then it does. 50/55/60 is the whole window this sync may use.
//
// A rate set by hand (FPSLocker, the legacy SetDisplayRefreshRate path) is not derived
// from a measurement and is left alone, so a 45 FPS game can still be pinned to 45
// deliberately - it just cannot drift there on its own.
#define DISPLAY_SYNC_MIN_RATE 50u

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

static bool probeArmed = false;      // last applied change went below the demonstrated max
static uint32_t probeFromRate = 0;   // rate we left to test upward, 0 = not testing
static uint32_t probeTarget = 0;
static u64 probeStart = 0;
static uint32_t probeFailedRate = 0;
static uint32_t probeCount = 0;      // capped, so a game wobbling on the boundary
static const uint32_t probeLimit = 3; // cannot ping-pong the panel forever
static u64 sameRateSince = 0;
static bool minRateClampLogged = false;

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
    probeCount = 0;
    sameRateSince = 0;
    minRateClampLogged = false;
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

void updateRefreshRateForApp() {
    if ((!isDocked && displaySync) || (isDocked && displaySyncDocked)) {
        uint32_t temp_refreshRate = 0;
        GetDisplayRefreshRate(&temp_refreshRate, true);
        uint32_t requestedRate = nx_fps ? (isDocked ? nx_fps->FPSlockedDocked : nx_fps->FPSlocked) : 0;
        uint32_t check_refresh_rate = requestedRate ? requestedRate : refreshRate;
        if (check_refresh_rate == 0)
            check_refresh_rate = 60;
        if (!isDocked && requestedRate && check_refresh_rate < DISPLAY_SYNC_MIN_RATE) {
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
            if (check_refresh_rate > probeFromRate) {
                uint32_t achieved = 0;
                if (GetDisplayRefreshRate(&achieved, true))
                    SaltyNX_printf(APP_NAME ": display-sync: %u Hz was locked in, target followed to %u Hz (active %u Hz)\n",
                                   probeFromRate, check_refresh_rate, achieved);
                else
                    SaltyNX_printf(APP_NAME ": display-sync: %u Hz was locked in, target followed to %u Hz\n",
                                   probeFromRate, check_refresh_rate);
                probeFromRate = 0;
                probeArmed = false;
                sameRateSince = 0;
                pendingRefreshRate = 0;
            }
            else if (check_refresh_rate < probeFromRate || now - probeStart >= PROBE_WINDOW_MS * SV_TICKS_PER_MS) {
                uint32_t achieved = 0;
                SetDisplayRefreshRate(probeFromRate);
                probeFailedRate = probeFromRate;
                if (GetDisplayRefreshRate(&achieved, true))
                    SaltyNX_printf(APP_NAME ": display-sync: %u Hz is real, back from the test (target %u Hz, active %u Hz)\n",
                                   probeFromRate, check_refresh_rate, achieved);
                else
                    SaltyNX_printf(APP_NAME ": display-sync: %u Hz is real, back from the test (target %u Hz)\n",
                                   probeFromRate, check_refresh_rate);
                probeFromRate = 0;
                probeArmed = false;
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
            if (delta <= REFRESH_RATE_DEADBAND_HZ) {
                // One hertz is below what the eye resolves and costs a full mode
                // switch, so it is always measurement noise, never an intent.
                pendingRefreshRate = 0;
            }
            else if (check_refresh_rate != pendingRefreshRate) {
                u64 requiredMs = REFRESH_RATE_LARGE_MS;
                if (delta <= REFRESH_RATE_SMALL_DELTA)
                    requiredMs = (check_refresh_rate > temp_refreshRate)
                                 ? REFRESH_RATE_SMALL_UP_MS
                                 : REFRESH_RATE_SMALL_DOWN_MS;
                pendingRefreshRate = check_refresh_rate;
                pendingRefreshSince = now;
                pendingRefreshRequired = requiredMs * SV_TICKS_PER_MS;
            }
            else if (now - pendingRefreshSince >= pendingRefreshRequired) {
                const u32 heldMs = (u32)((now - pendingRefreshSince) / SV_TICKS_PER_MS);
                uint32_t achievedRate = 0;
                SetDisplayRefreshRate(check_refresh_rate);
                // The panel quantises and clamps (25 Hz is served as 50 Hz on OLED),
                // so report what the hardware ended up at, not what we asked for.
                if (GetDisplayRefreshRate(&achievedRate, true))
                    SaltyNX_printf(APP_NAME ": display-sync: refresh rate %u -> %u Hz requested (target held %u ms, active %u Hz)\n",
                                   temp_refreshRate, check_refresh_rate, heldMs, achievedRate);
                else
                    SaltyNX_printf(APP_NAME ": display-sync: refresh rate %u -> %u Hz requested (target held %u ms)\n",
                                   temp_refreshRate, check_refresh_rate, heldMs);
                pendingRefreshRate = 0;
                // A drop to the floor is the only one worth second-guessing: below it
                // there is no room left to move down, and the panel holding the app
                // down looks exactly like a slow game. Arm one test back to the rate we
                // just left; a drop between 60/55/50 above the floor is a real one.
                probeArmed = (check_refresh_rate < temp_refreshRate) &&
                             (check_refresh_rate == DISPLAY_SYNC_MIN_RATE) &&
                             (temp_refreshRate <= HandheldModeRefreshRateAllowed.max) &&
                             (temp_refreshRate != probeFailedRate) &&
                             (probeCount < probeLimit);
                if (probeArmed)
                    probeTarget = temp_refreshRate;
            }
        }
        else {
            pendingRefreshRate = 0;
            if (probeArmed && probeTarget > check_refresh_rate) {
                if (sameRateSince == 0)
                    sameRateSince = now;
                else if (now - sameRateSince >= PROBE_HOLD_MS * SV_TICKS_PER_MS) {
                    uint32_t up = (probeTarget > HandheldModeRefreshRateAllowed.max)
                                ? HandheldModeRefreshRateAllowed.max
                                : probeTarget;
                    probeArmed = false;
                    probeFromRate = check_refresh_rate;
                    probeStart = now;
                    sameRateSince = 0;
                    probeCount++;
                    SetDisplayRefreshRate(up);
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
