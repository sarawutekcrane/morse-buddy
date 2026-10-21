#!/usr/bin/env python3
# SIMULATOR (test only): fake `pio` + esptool for exercising the plan's scripts. Every call is logged.
import sys, os, shutil
SIM=os.environ["SIM"]; FW=os.environ["SIMFW"]
a=sys.argv[1:]
open(os.path.join(SIM,"calls.log"),"a").write(" ".join(a)+"\n")
def img(port): return os.path.join(SIM,"dev",os.path.basename(port)+".img")
def mac(port): return open(os.path.join(SIM,"dev",os.path.basename(port)+".mac")).read().strip()
def rd(port,off,n):
    with open(img(port),"rb") as f: f.seek(off); return f.read(n)
def wr(port,off,data):
    with open(img(port),"r+b") as f: f.seek(off); f.write(data)
def pairs(e):
    out=[]; i=0
    while i<len(e):
        if e[i]=="-z": i+=1; continue
        if e[i] in("--flash_mode","--flash_freq","--flash_size"): i+=2; continue
        out.append((int(e[i],0),e[i+1])); i+=2
    return out
if a[:2]==["pkg","exec"]:
    e=a[a.index("--")+2:]
    port=e[e.index("--port")+1]
    if not os.path.exists(img(port)): print("Could not open port",port); sys.exit(2)
    g=("--chip","--port","--baud","--before","--after")
    e=[x for i,x in enumerate(e) if not (x in g or (i>0 and e[i-1] in g))]
    cmd=e[0]
    if cmd=="flash_id": print("Detected flash size: 4MB")
    elif cmd=="read_mac": print("esptool.py v4.11.0\nMAC: "+mac(port)+"\nHard resetting via RTS pin...")
    elif cmd=="read_flash":
        off,n,out=int(e[1],0),int(e[2],0),e[3]; open(out,"wb").write(rd(port,off,n))
    elif cmd=="verify_flash":
        for off,f in pairs(e[1:]):
            d=open(f,"rb").read()
            if rd(port,off,len(d))!=d: print("Verify failed at",hex(off)); sys.exit(2)
        print("-- verify OK (digest matched)")
    elif cmd=="write_flash":
        if os.environ.get("SIM_WRITE_FAIL"): print("A fatal error occurred: simulated write failure"); sys.exit(2)
        for off,f in pairs(e[1:]):
            d=bytearray(open(f,"rb").read())
            if os.environ.get("SIM_CORRUPT") and off==0x10000: d[5]^=0xff   # silent corruption: only verify can catch it
            wr(port,off,bytes(d)); print("Hash of data verified.")
    else: print("unsupported",cmd); sys.exit(2)
    sys.exit(0)
if a[0]=="run":
    if os.environ.get("SIM_NO_RUN"): print("SIMULATOR: pio run called during a no-build step"); sys.exit(99)
    if "-t" in a and a[a.index("-t")+1]=="clean": shutil.rmtree(".pio",ignore_errors=True); print("[SUCCESS] clean"); sys.exit(0)
    if "-t" in a: print("SIMULATOR: unexpected target"); sys.exit(98)
    B=".pio/build/morse_buddy"
    if os.environ.get("SIM_BUILD_FAIL"): print("src/core/race.cpp:1: error: simulated"); print("[FAILED]"); sys.exit(1)
    os.makedirs(B,exist_ok=True)
    for n in ("bootloader.bin","partitions.bin"): shutil.copy(os.path.join(FW,n),B+"/"+n)
    open(B+"/firmware.bin","wb").write(b"FW:"+open("src/core/race.cpp","rb").read()[:64]+b"\0"*1000)
    print("RAM:   [====      ]  37.9% (used 124220 bytes from 327680 bytes)\nFlash: [========  ]  82.6% (used 1191437 bytes from 1441792 bytes)")
    print("Dependency Graph\n|-- MQTT @ 2.5.2\nBuilding .pio/build/morse_buddy/firmware.bin")
    print("[SUCCESS] Took 1.00 seconds"); sys.exit(0)
print("unsupported",a); sys.exit(2)
