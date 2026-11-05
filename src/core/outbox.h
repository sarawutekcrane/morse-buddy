#pragma once
// Local Outbox retry (Addendum section 7.9, Phase 2 section 8). Registers
// its own AppService: a record written pending is queued directly, and an
// incremental pass of header-only reads (once per boot after connecting,
// and after a change made outside the store) finds the ones left pending on
// flash. Queued records are republished once the owning group is
// connected, the flag cleared on success.

namespace Outbox {

// Optional immediate retry (e.g. right after a group reconnects); the
// background AppService tick already does this periodically on its own.
void tryFlushNow();

}  // namespace Outbox
