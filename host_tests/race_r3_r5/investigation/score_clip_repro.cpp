// INVESTIGATION REPRO (host only, not production). Defect 2: a Race score of
// "1" (and 10-19) is not shown beside the name on hardware.
//
// Uses the REAL FreeSans9pt7b glyph table (Adafruit GFX Library Fonts/) and
// replicates, line for line:
//   - Adafruit_GFX::charBounds()/getTextBounds() (library 1.12.6, the version
//     the evaluated firmware build resolved), textsize 1, wrap off
//     (display.cpp init() calls setTextWrap(false));
//   - display.cpp textWidth() (= w) and measureFitWidth() (= x1 + w), both
//     measured at (0, 50) for the PRIMARY font;
//   - display.cpp printLine()'s clip loop (drop the last char while
//     measureFitWidth(buf) > kScreenWidth - x);
//   - race.cpp drawRoomRow()'s score placement
//     (scoreX = kScreenWidth - 2 - textWidth(score)).
// Build: g++ -std=gnu++17 -I "<Adafruit GFX Library dir>" score_clip_repro.cpp
#include <cstdint>
#include <cstdio>
#include <cstring>
#define PROGMEM
#include <gfxfont.h>
#include <Fonts/FreeSans9pt7b.h>

static const int16_t kScreenWidth = 240;  // display.h (landscape 240x135)

static void charBounds(unsigned char c, int16_t* x, int16_t* y, int16_t* minx, int16_t* miny, int16_t* maxx, int16_t* maxy) {
  const GFXfont* f = &FreeSans9pt7b;
  if (c == '\n' || c == '\r') return;  // not used here
  if (c < f->first || c > f->last) return;
  const GFXglyph* g = &f->glyph[c - f->first];
  int16_t x1 = *x + g->xOffset, y1 = *y + g->yOffset, x2 = x1 + g->width - 1, y2 = y1 + g->height - 1;
  if (x1 < *minx) *minx = x1;
  if (y1 < *miny) *miny = y1;
  if (x2 > *maxx) *maxx = x2;
  if (y2 > *maxy) *maxy = y2;
  *x += g->xAdvance;
}
static void getTextBounds(const char* s, int16_t x, int16_t y, int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h) {
  int16_t minx = 0x7FFF, miny = 0x7FFF, maxx = -1, maxy = -1;
  *x1 = x; *y1 = y; *w = *h = 0;
  for (; *s; s++) charBounds(static_cast<unsigned char>(*s), &x, &y, &minx, &miny, &maxx, &maxy);
  if (maxx >= minx) { *x1 = minx; *w = maxx - minx + 1; }
  if (maxy >= miny) { *y1 = miny; *h = maxy - miny + 1; }
}
static int16_t textWidth(const char* t) { int16_t x1, y1; uint16_t w, h; getTextBounds(t, 0, 50, &x1, &y1, &w, &h); return w; }
static int16_t measureFitWidth(const char* t) { int16_t x1, y1; uint16_t w, h; getTextBounds(t, 0, 50, &x1, &y1, &w, &h); return x1 + w; }
// display.cpp printLine(): what is actually printed at x.
static void printLineClip(int16_t x, const char* text, char* out, size_t cap) {
  strncpy(out, text, cap - 1); out[cap - 1] = '\0';
  int16_t maxWidth = kScreenWidth - x;
  if (maxWidth > 0) { while (out[0] != '\0' && measureFitWidth(out) > maxWidth) out[strlen(out) - 1] = '\0'; }
  else out[0] = '\0';
}

int main() {
  int wrong = 0;
  printf("score  textWidth  x1(bearing)  scoreX  maxWidth  fit  printed\n");
  const unsigned scores[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 19, 20, 21, 99, 100, 199, 1000};
  for (unsigned s : scores) {
    char score[8]; snprintf(score, sizeof(score), "%u", s);
    int16_t scoreX = static_cast<int16_t>(kScreenWidth - 2 - textWidth(score));  // race.cpp drawRoomRow()
    char printed[8]; printLineClip(scoreX, score, printed, sizeof(printed));
    int16_t x1, y1; uint16_t w, h; getTextBounds(score, 0, 50, &x1, &y1, &w, &h);
    bool ok = strcmp(printed, score) == 0;
    if (!ok) wrong++;
    printf("%5s  %9d  %11d  %6d  %8d  %3d  \"%s\"%s\n", score, textWidth(score), x1, scoreX, kScreenWidth - scoreX,
           measureFitWidth(score), printed, ok ? "" : "   <-- WRONG");
  }
  printf("%d of %zu scores are not printed as their value\n", wrong, sizeof(scores) / sizeof(scores[0]));
  // PROPOSED placement (not in the firmware): align by the ink's right edge,
  // scoreX = kScreenWidth - 2 - measureFitWidth(score), so printLine()'s clip
  // (which uses measureFitWidth) can never remove a digit.
  int wrongProposed = 0;
  for (unsigned s : scores) {
    char score[8]; snprintf(score, sizeof(score), "%u", s);
    int16_t scoreX = static_cast<int16_t>(kScreenWidth - 2 - measureFitWidth(score));
    char printed[8]; printLineClip(scoreX, score, printed, sizeof(printed));
    if (strcmp(printed, score) != 0 || scoreX + measureFitWidth(score) != kScreenWidth - 2) wrongProposed++;
  }
  printf("proposed placement: %d of %zu scores wrong; every score's last ink column at x = %d\n", wrongProposed,
         sizeof(scores) / sizeof(scores[0]), kScreenWidth - 3);
  return wrong == 0 ? 0 : 1;
}
