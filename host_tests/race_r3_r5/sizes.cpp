// HOST TEST (not production): RAM sizes of the race engine structures.
#include <cstdio>
#define private public
#include "core/race_protocol.h"
#undef private
using namespace RaceProtocol;
int main() {
  Engine e;
  printf("sizeof(RoomState)=%zu sizeof(Entry)=%zu sizeof(Engine)=%zu\n", sizeof(RoomState), sizeof(Entry), sizeof(Engine));
  printf("  room_=%zu buf_=%zu rec_(MemberRecord)=%zu commit_=%zu disc_=%zu replyRate_=%zu reconcile_=%zu p_=%zu\n",
         sizeof(e.room_), sizeof(e.buf_), sizeof(e.rec_), sizeof(e.commit_), sizeof(e.disc_), sizeof(e.replyRate_),
         sizeof(e.reconcile_), sizeof(e.p_));
  return 0;
}
