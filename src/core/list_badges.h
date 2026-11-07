#pragma once
// Per-row badge tables for ListMenu (BadgeFn takes no argument): F<0> ..
// F<N-1>, one tiny instance per row index. The tables built with these are
// const arrays of function pointers -- read-only data (flash), no RAM.
#define MB_BADGE_ROWS5(F, B) F<(B) + 0>, F<(B) + 1>, F<(B) + 2>, F<(B) + 3>, F<(B) + 4>
#define MB_BADGE_ROWS10(F, B) MB_BADGE_ROWS5(F, B), MB_BADGE_ROWS5(F, (B) + 5)
#define MB_BADGE_ROWS40(F) MB_BADGE_ROWS10(F, 0), MB_BADGE_ROWS10(F, 10), MB_BADGE_ROWS10(F, 20), MB_BADGE_ROWS10(F, 30)
#define MB_BADGE_ROWS66(F) \
  MB_BADGE_ROWS40(F), MB_BADGE_ROWS10(F, 40), MB_BADGE_ROWS10(F, 50), MB_BADGE_ROWS5(F, 60), F<65>
