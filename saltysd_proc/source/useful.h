#ifndef USEFUL_H
#define USEFUL_H

#include <switch.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "nanoprintf.h"
#ifdef __cplusplus
	#include <cctype>
#else
	#include <ctype.h>
#endif

struct resolutionCalls {
	uint16_t width;
	uint16_t height;
	uint16_t calls;
};

struct NxFpsSharedBlock {
	uint32_t MAGIC;
	uint8_t FPS;
	float FPSavg;
	bool pluginActive;
	uint8_t FPSlocked;
	uint8_t FPSmode;
	uint8_t ZeroSync;
	uint8_t patchApplied;
	uint8_t API;
	uint32_t FPSticks[10];
	uint8_t Buffers;
	uint8_t SetBuffers;
	uint8_t ActiveBuffers;
	uint8_t SetActiveBuffers;
	union {
		struct {
			bool handheld: 1;
			bool docked: 1;
			unsigned int reserved: 6;
		} NX_PACKED ds;
		uint8_t general;
	} displaySync;
	struct resolutionCalls renderCalls[8];
	struct resolutionCalls viewportCalls[8];
	bool forceOriginalRefreshRate;
	bool dontForce60InDocked;
	bool forceSuspend;
	uint8_t currentRefreshRate;
	float readSpeedPerSecond;
	uint8_t FPSlockedDocked;
	uint64_t frameNumber;
	int8_t expectedSetBuffers;
} NX_PACKED;

static_assert(sizeof(struct NxFpsSharedBlock) == 174);

#ifdef SWITCH
    #define systemtickfrequency 19200000
#elif OUNCE
    #define systemtickfrequency 31250000
#else
	extern uint64_t systemtickfrequency;
#endif

static inline void remove_spaces(char* str_trimmed, const char* str_untrimmed)
{
  while (str_untrimmed[0] != '\0')
  {
    if(!isspace((int)str_untrimmed[0]))
    {
      str_trimmed[0] = str_untrimmed[0];
      str_trimmed++;
    }
    str_untrimmed++;
  }
  str_trimmed[0] = '\0';
}

static inline bool file_or_directory_exists(const char *filename)
{
    struct stat buffer;
    return stat(filename, &buffer) == 0 ? true : false;
}

// Logging is opt-in, and it comes in two sizes:
//
//   sdmc:/SaltySD/flags/log.flag        decisions, warnings and boot information
//   sdmc:/SaltySD/flags/log.full.flag   everything, including one line per IPC call
//
// Neither file: silent. The second flag exists because the full trace writes a line for
// every fopen/fread/memcpy the game makes through SaltyNX - that is thousands of writes
// to the same SD card the game streams its ISO from, which is measurable in the frame
// pacing it is supposed to be diagnosing.
//
// The flags are sampled once a minute rather than once per line: the stat() itself was
// per line before, and it lands on the same card.
#define SALTYNX_LOG_SILENT  0
#define SALTYNX_LOG_MINIMAL 1
#define SALTYNX_LOG_FULL    2

static inline int SaltyNX_LogLevel(void)
{
	static int cached = -1;
	static uint64_t lastCheck = 0;
	const uint64_t now = svcGetSystemTick();
	if (cached >= 0 && (now - lastCheck) < (60ULL * systemtickfrequency))
		return cached;
	lastCheck = now;
	if (file_or_directory_exists("sdmc:/SaltySD/flags/log.full.flag")) cached = SALTYNX_LOG_FULL;
	else if (file_or_directory_exists("sdmc:/SaltySD/flags/log.flag")) cached = SALTYNX_LOG_MINIMAL;
	else cached = SALTYNX_LOG_SILENT;
	return cached;
}

static inline void SaltyNX_vprintf(int level, const char* format, ...)
{
	if (SaltyNX_LogLevel() < level) return;
	
	char buffer[256];

	va_list args;
	va_start(args, format);
	npf_vsnprintf(buffer, 256, format, args);
	va_end(args);
	
	svcOutputDebugString(buffer, strlen(buffer));
	
	static bool previous_line_had_endline = false;
	FILE* f = fopen("sdmc:/SaltySD/saltynx.log", "ab");
	if (f)
	{
		static uint64_t tick = 0;
		if (!tick) {
			tick = svcGetSystemTick();
		}
		else if (previous_line_had_endline) {
			char timer[] = "[244444444:24:24] ";
			uint64_t deltaTick = svcGetSystemTick() - tick;
			uint64_t deltaSeconds = deltaTick / systemtickfrequency;
			npf_snprintf(timer, sizeof(timer), "[%02ld:%02ld:%02ld] ", (deltaSeconds/3600) % 1000000000, ((deltaSeconds/60) % 60), deltaSeconds % 60);
			fwrite(timer, strlen(timer), 1, f);
		}
		if (buffer[strlen(buffer)-1] == '\n') {
			previous_line_had_endline = true;
		}
		else previous_line_had_endline = false;
		fwrite(buffer, strlen(buffer), 1, f);
		fclose(f);
	}
}

// Ordinary lines: shown with log.flag.
#define SaltyNX_printf(...)      SaltyNX_vprintf(SALTYNX_LOG_MINIMAL, __VA_ARGS__)
// Trace lines: only with log.full.flag.
#define SaltyNX_printf_trace(...) SaltyNX_vprintf(SALTYNX_LOG_FULL, __VA_ARGS__)


#define debug_log(...) \
	{char log_buf[0x200]; npf_snprintf(log_buf, 0x200, __VA_ARGS__); \
	svcOutputDebugString(log_buf, strlen(log_buf));}
	
#endif // USEFUL_H
