# Morse Buddy — Hardware Test Result Record (Blank Template)

Copy this block once per test case executed from `MASTER_HARDWARE_TEST_CHECKLIST.md`.
Fill in every field. Leave "Serial evidence" and heap fields blank only if
the test genuinely produced none (state why in Notes) — do not omit them.

```
Test ID:            
Phase:               
Feature:             
Required devices:    
Preconditions:       

Procedure:
  1. 
  2. 
  3. 

Expected result:     

Observed result:     

Serial evidence:     

Heap before:          bytes
Min heap:             bytes
Heap after:           bytes

PASS/FAIL:           
Notes:               
```

## Field definitions

- **Test ID** — exact ID from the master checklist (e.g. `HW-BOOT-002`,
  `HW-OTA-12-005`, `HW-OTAFX-09`). One record per ID per run.
- **Phase** — which implementation phase the test validates (0–5).
- **Feature** — short name of the feature under test (e.g. "Private Radio
  claim/negotiate", "OTA manifest parse — bad path").
- **Required devices** — number of physical boards needed for this specific
  test, and their role if relevant (e.g. "2 — caller + callee").
- **Preconditions** — firmware build, NVS/settings state, network state,
  and any prior test that must have passed first.
- **Procedure** — the exact steps performed, numbered, as actually carried
  out (copy from the checklist; add any deviation).
- **Expected result** — copied from the checklist entry for this Test ID.
- **Observed result** — what actually happened, in your own words.
- **Serial evidence** — relevant log lines copied verbatim from the serial
  monitor (timestamps included if available). Required for any OTA,
  rollback, persistence, or failure-path test; optional but recommended
  elsewhere.
- **Heap before / Min heap / Heap after** — from `ESP.getFreeHeap()` /
  `ESP.getMinFreeHeap()` readings bracketing the test, where the checklist
  entry calls for a heap measurement (Section 21 and any OTA/leak test).
  Leave as `N/A` for tests that don't call for heap measurement.
- **PASS/FAIL** — record one of `PASS`, `FAIL`, or `BLOCKED` (BLOCKED =
  could not be executed, e.g. missing hardware precondition — explain in
  Notes). Never record PASS without an actual physical observation backing
  it.
- **Notes** — anything unexpected, environmental factors, deviations from
  procedure, or follow-up needed. If FAIL or BLOCKED, explain the cause
  here if known.
