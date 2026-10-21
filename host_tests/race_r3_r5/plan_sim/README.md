# Acceptance plan script simulation (test only, NOT hardware)

These files run the Bash blocks of
`hardware_test/RACE_R3_R5_ACCEPTANCE_PLAN_R7.3.md` against a simulated
esptool and `pio` (`fake_pio.py`) and three simulated 4 MB devices.
- Every simulated call is logged, so each case can show whether
  `write_flash` or `pio run` was called.
- It does not model esptool's bootloader-header update, or any real
  hardware, USB or timing.

```sh
BUILD_DIR=<dir holding bootloader.bin and partitions.bin from `pio run -e morse_buddy`> \
  ./setup_sim.sh /tmp/plan-sim
bash /tmp/plan-sim/run_sim.sh      # prints each case; ends with "N cases, 0 unexpected"
```

Results: `../evidence/plan_r7.3_simulation_results.txt` (29 cases, 0
unexpected).

**Serial monitor check** (plan 0.6). The plan's monitor block is run
against a pseudo-terminal that sends three `[RACE][HEAP]` lines. Run it from
a directory outside the project:

```sh
mkdir -p /tmp/mon-out && cd /tmp/mon-out && \
  python3 <this dir>/pty_mon.py <repo> /tmp/mon-out /tmp/plan-sim/monitor.sh
```

It checks that the session log lands in `OUT`, that the environment's
`monitor_speed` is used, and that nothing is written into the project. Its
output is part of the same results file.
