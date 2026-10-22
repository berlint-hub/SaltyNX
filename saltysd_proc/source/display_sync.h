#ifndef DISPLAY_SYNC_H
#define DISPLAY_SYNC_H

#include <switch.h>

// Dedicated IPC port for display-sync clients (f.e. NetherSX2). The name must
// match the client, which has it compiled in as "NethFPS".
#define DISPLAY_SYNC_PORT_NAME "NethFPS"

// The kernel keeps one session slot per accepted client session, so a client
// that reconnects once per second only ever uses one slot at a time. A few extra
// slots make sure a client that does not close its session cannot lock out the
// next connection attempt.
#define DISPLAY_SYNC_SESSIONS 4

// Wait on the port handle instead of polling svcAcceptSession. The port is
// signalled as soon as a client session is queued, so an idle endpoint costs
// nothing and never logs the normal "nobody connected" NotFound answer.
#define DISPLAY_SYNC_WAIT_NS (100ull * 1000 * 1000)

// How many idle waits (100 ms each) pass before the endpoint says once that it
// is still alive and still has not seen a client. 300 * 100 ms = 30 s.
#define DISPLAY_SYNC_IDLE_WAITS 300

extern Handle saltyportDisplaySync;

void serviceThreadDisplaySync(void* portHandle);

#endif
