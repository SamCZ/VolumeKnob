#pragma once

#include <stdint.h>

/** Serial link to the Windows companion (see docs/protocol.md). */
void linkInit();
/** Read and apply incoming lines; updates g_model. Call every loop iteration. */
void linkPoll();

void linkSendMaster(int volume, bool muted);
void linkSendApp(int id, int volume, bool muted);
