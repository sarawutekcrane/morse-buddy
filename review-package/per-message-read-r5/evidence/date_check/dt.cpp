#include <cstdio>
#include <cstring>
#include <ctime>
#include "core/history_dates.h"
namespace MessageStore { const ConversationIndexEntry* getIndexEntry(uint16_t) { return nullptr; } }
int main() {
  int bad = 0; char a[32], b[32];
  for (uint32_t ts = HistoryDates::kTrustedFrom; ts < HistoryDates::kTrustedUntil - 86400; ts += 86400 / 4 + 37) {
    time_t t = ts + 7 * 3600; struct tm g; gmtime_r(&t, &g);
    snprintf(a, sizeof a, "%02d/%02d/%04d", g.tm_mday, g.tm_mon + 1, g.tm_year + 1900);
    HistoryDates::formatDay(HistoryDates::thaiDay(ts), b, sizeof b);
    if (strcmp(a, b)) { if (bad++ < 5) printf("ts %u: %s vs %s\n", ts, a, b); }
  }
  HistoryDates::formatDay(HistoryDates::thaiDay(1793898000u), b, sizeof b); printf("1793898000 -> %s\n", b);
  HistoryDates::formatDay(HistoryDates::thaiDay(1793897999u), b, sizeof b); printf("1793897999 -> %s\n", b);
  HistoryDates::formatDay(HistoryDates::thaiDay(0), b, sizeof b); printf("0 -> %s\n", b);
  HistoryDates::formatDay(HistoryDates::thaiDay(1577836799u), b, sizeof b); printf("1577836799 -> %s\n", b);
  HistoryDates::formatDay(HistoryDates::thaiDay(1577836800u), b, sizeof b); printf("1577836800 -> %s\n", b);
  HistoryDates::formatDay(HistoryDates::thaiDay(4102444799u), b, sizeof b); printf("4102444799 -> %s\n", b);
  HistoryDates::formatDay(HistoryDates::thaiDay(4102444800u), b, sizeof b); printf("4102444800 -> %s\n", b);
  printf("%d mismatches over 2020..2099 against gmtime(+7h)\n", bad);
}
