import json, time, urllib.request, sys
IP = sys.argv[1]
def st():
    return json.loads(urllib.request.urlopen(f"http://{IP}/api/status", timeout=8).read())["dbg"]
def cmd(q): urllib.request.urlopen(f"http://{IP}/{q}", timeout=8).read()
time.sleep(1)
for led, stat in ((1,1),(1,0),(2,1),(2,0)):
    a = st()
    cmd(f"test?addr=2&led={led}&stat={stat}")
    time.sleep(2.5)
    b = st()
    print(led, stat, {k: b[k]-a[k] for k in ("err","lost","full","oth","sfail")}, "lec", b["lec"], "led", b["led"], "sec", b["sec"])

