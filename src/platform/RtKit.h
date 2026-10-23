// SPDX-License-Identifier: GPL-3.0-or-later
// Realtime scheduling without root, through RealtimeKit on the system bus.
#pragma once

#include <string>

namespace pd {

// Asks rtkit to put the calling thread on SCHED_FIFO. `priority` is clamped to
// what rtkit allows. Returns false with a reason if it could not.
bool makeCurrentThreadRealtime(int priority, std::string& err);

}  // namespace pd
