#ifndef DISPLAY_SYNC_H
#define DISPLAY_SYNC_H

#include <switch.h>

// Dedicated IPC port for display-sync clients inside the hijacked process. The name
// must match the client, which has it compiled in as "NethFPS".
#define DISPLAY_SYNC_PORT_NAME "NethFPS"
// One session is enough for the polling client, but a stray connection attempt (or a
// client that keeps its session open) must not lock everybody else out.
#define DISPLAY_SYNC_SESSIONS 4

extern Handle saltyportDisplaySync;

void serviceThreadDisplaySync(void* portHandle);

#endif
