#!/usr/bin/env python3
"""Autotest gateway PJON <-> IO16 via HTTP API del gateway.
Uso: python tools/autotest.py [ip] [node_id]
Stampa un report JSON-like e termina con exit code 0 se tutti i test passano.
"""
import json, sys, time, urllib.request

GW_MAC = "aa-ed-ab-fe-aa-f2"


def discover_ip():
    """Trova l'IP del gateway (DHCP) via tabella ARP, dopo un ping-sweep."""
    import re, subprocess
    def arp():
        out = subprocess.run(["arp", "-a"], capture_output=True, text=True).stdout.lower()
        m = re.search(r"(\d+\.\d+\.\d+\.\d+)\s+" + GW_MAC, out)
        return m.group(1) if m else None
    ip = arp()
    if ip:
        return ip
    subprocess.run(["powershell", "-Command",
                    "1..254 | ForEach-Object { Start-Job { param($i) Test-Connection -Count 1 -Quiet -TimeoutSeconds 1 $i } -ArgumentList \"192.168.1.$_\" } | Wait-Job -Timeout 20 | Out-Null; Get-Job | Remove-Job -Force"],
                   capture_output=True)
    return arp() or "192.168.1.100"


IP = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] != "auto" else discover_ip()
NODE = int(sys.argv[2]) if len(sys.argv) > 2 else 2
BASE = f"http://{IP}"
print("Gateway IP:", IP, "| nodo test:", NODE)


def get(path, timeout=6):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def status():
    return json.loads(get("/api/status"))


def node(st, nid):
    for n in st.get("nds", []):
        if n["i"] == nid:
            return n
    return {}


results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}")


def main():
    t0 = status()
    check("HTTP /api/status raggiungibile", True, f"prx={t0['prx']} ptx={t0['ptx']}")
    if "dbg" in t0:
        print("DBG", json.dumps(t0["dbg"]))

    # 1. Traffico a riposo: nessuna raffica
    time.sleep(10)
    t1 = status()
    idle = t1["prx"] - t0["prx"]
    check("Traffico a riposo (10s) <= 3 pacchetti", idle <= 3, f"delta_rx={idle}")

    # 2. Ping nodo
    a = status()
    get("/pingall")
    time.sleep(2)
    b = status()
    n_a, n_b = node(a, NODE), node(b, NODE)
    d = n_b.get("r", 0) - n_a.get("r", 0)
    check(f"Ping nodo {NODE}: risposta singola (K)", 1 <= d <= 2, f"delta_node_rx={d}")
    check(f"Nodo {NODE} online dopo ping", n_b.get("o") == 1)

    # 3. Test LED: comando singolo -> 1 TX, errori non crescono
    for led, stat in ((1, 1), (1, 0), (0, 1), (0, 0)):
        a = status()
        get(f"/test?addr={NODE}&led={led}&stat={stat}")
        time.sleep(1.5)
        b = status()
        dtx = b["ptx"] - a["ptx"]
        drx = b["prx"] - a["prx"]
        check(f"Test LED led={led} stat={stat}: 1 TX", dtx == 1, f"dtx={dtx} drx={drx}")
        if "dbg" in b and "dbg" in a:
            de = b["dbg"].get("err", 0) - a["dbg"].get("err", 0)
            check(f"  errori PJON durante invio = 0", de == 0, f"derr={de}")

    # 4. Stress: 20 comandi consecutivi
    a = status()
    for i in range(20):
        get(f"/test?addr={NODE}&led={(i%8)+1}&stat={i%2}")
        time.sleep(0.15)
    time.sleep(1)
    b = status()
    check("Stress 20 comandi: gateway vivo", True, f"dtx={b['ptx']-a['ptx']}")
    get(f"/test?addr={NODE}&led=0&stat=0")

    # 5. Pagina principale
    html = get("/")
    check("Pagina principale caricata", "Test Rapido" in html, f"len={len(html)}")

    # 6. Latenza HTTP
    lat = []
    for _ in range(10):
        s = time.time()
        get("/api/status")
        lat.append((time.time() - s) * 1000)
    check("Latenza HTTP media < 300ms", sum(lat) / len(lat) < 300, f"avg={sum(lat)/len(lat):.0f}ms max={max(lat):.0f}ms")

    fails = [r for r in results if not r[1]]
    print(f"\nRISULTATO: {len(results)-len(fails)}/{len(results)} test OK")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()

