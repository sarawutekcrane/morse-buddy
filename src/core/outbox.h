#pragma once
// Local Outbox retry (Addendum section 7.9, Phase 2 section 8). Registers
// its own AppService; scans for PENDING_OUTBOX records and republishes
// them once the owning group is connected, clearing the flag atomically
// on success.

namespace Outbox {

// Optional immediate retry (e.g. right after a group reconnects); the
// background AppService tick already does this periodically on its own.
void tryFlushNow();

}  // namespace Outbox
