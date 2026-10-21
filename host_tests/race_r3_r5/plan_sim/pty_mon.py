#!/usr/bin/env python3
# TEST ONLY: runs the plan monitor block (argv[3]) on a pseudo-terminal that emits heap lines.
# Usage: python3 pty_mon.py REPO OUTDIR MONITOR_SCRIPT   (run from OUTDIR, outside the project)
import os, pty, subprocess, sys, time, threading
repo, cwd = sys.argv[1], sys.argv[2]
master, slave = pty.openpty()
port = os.ttyname(slave)
cmd = ["bash", sys.argv[3]]
os.environ.update(OUT=cwd, DEV="A", REPO=repo, PORT=port)
print("cmd:", " ".join(cmd), "cwd:", cwd)
# the monitor wants a terminal on stdin; give it its own pty
im, isl = pty.openpty()
p = subprocess.Popen(cmd, cwd=cwd, stdin=isl, stdout=isl, stderr=isl, start_new_session=True)
buf=[]
def rd():
    while True:
        try: d=os.read(im,4096)
        except OSError: break
        if not d: break
        buf.append(d)
threading.Thread(target=rd, daemon=True).start()
def feed():
    time.sleep(6)
    for i in range(3):
        os.write(master, b"[RACE][HEAP] periodic uptime_s=%d free=150000 min_since_boot=140000 largest_block=110000 (bytes, internal 8-bit)\r\n" % (60 * (i + 1)))
        time.sleep(0.5)
threading.Thread(target=feed, daemon=True).start()
time.sleep(12)
os.write(im, b"\x03")  # Ctrl+C, as a person would
time.sleep(2); p.terminate()
p.wait(timeout=10); out=b"".join(buf).decode(errors="replace")
print(out[-1500:])
print("exit:", p.returncode)
