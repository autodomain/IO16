import urllib.request
import json
import time
import os

def parse_intel_hex(filepath):
    bin_data = bytearray(131072)
    max_len = 0
    ext = 0
    with open(filepath, 'r') as f:
        for line in f:
            l = line.strip()
            if not l.startswith(':'): continue
            length = int(l[1:3], 16)
            addr = int(l[3:7], 16) + ext
            rectype = int(l[7:9], 16)
            if rectype == 0:
                for i in range(length):
                    bin_data[addr + i] = int(l[9+i*2:11+i*2], 16)
                if addr + length > max_len:
                    max_len = addr + length
            elif rectype == 4:
                ext = int(l[9:13], 16) << 16
    return bytes(bin_data[:max_len])

def flash_node(node_id, bus_id, hex_file):
    print("=== Starting OTA Flash for Node", node_id, "(Bus", str(bus_id) + ") with", hex_file, "===")
    firmware = parse_intel_hex(hex_file)
    print("Parsed", len(firmware), "bytes of code.")
    
    # 1. Start OTA
    url_start = "http://192.168.1.66/api/ota_start?node=" + str(node_id) + "&bus=" + str(bus_id)
    req = urllib.request.urlopen(url_start, timeout=6)
    resp = json.loads(req.read().decode('utf-8'))
    if resp.get('status') != 'ok':
        raise Exception("OTA Start failed: " + str(resp))
    print("Connected! Target MCU:", resp.get("mcu"))
    
    # 2. Write pages (128 bytes for 324PB, 256 bytes for 1284P)
    page_size = 128 if resp.get("mcu") == "atmega324pb" else 256
    tot = len(firmware)
    for off in range(0, tot, page_size):
        chunk = firmware[off:off+page_size]
        if len(chunk) < page_size:
            chunk = chunk + b'\xFF' * (page_size - len(chunk))
        word_addr = off >> 1
        ok = False
        for retry in range(3):
            url_page = "http://192.168.1.66/api/ota_page?addr=" + str(word_addr)
            p_req = urllib.request.Request(url_page, data=chunk, headers={'Content-Type': 'application/octet-stream'})
            try:
                p_resp = json.loads(urllib.request.urlopen(p_req, timeout=5).read().decode('utf-8'))
                if p_resp.get('status') == 'ok':
                    ok = True
                    break
            except Exception as e:
                time.sleep(0.05)
        if not ok:
            urllib.request.urlopen('http://192.168.1.66/api/ota_abort', timeout=2)
            raise Exception("Failed writing page at word " + hex(word_addr))
        pct = int(((off + page_size) / tot) * 100)
        print("\rFlashing: " + str(min(pct, 100)) + "% (" + str(min(off+page_size, tot)) + "/" + str(tot) + " bytes)", end='', flush=True)
    print()
    
    # 3. End OTA
    urllib.request.urlopen('http://192.168.1.66/api/ota_end', timeout=5)
    print("=== Node", node_id, "updated and rebooted successfully! ===\n")

if __name__ == "__main__":
    try:
        # Node 10 is the ATmega1284p with LM75 (was reading DIP 2+4+4=10 instead of 2+4=6)
        print("--- Flashing ATmega1284P board (currently responding as Node 10) ---")
        try:
            flash_node(10, 1, 'C:/Users/Carmelo/Desktop/pjon_client_8btn_8led/.pio/build/atmega1284p/firmware.hex')
        except Exception as e1:
            print("Node 10 start failed, trying Node 6...", e1)
            flash_node(6, 1, 'C:/Users/Carmelo/Desktop/pjon_client_8btn_8led/.pio/build/atmega1284p/firmware.hex')
        
        time.sleep(4)
        print("\n--- Flashing ATmega324PB board (Node 24) ---")
        flash_node(24, 1, 'C:/Users/Carmelo/Desktop/pjon_client_8btn_8led/.pio/build/atmega324pb/firmware.hex')
        print("\nALL NODES SUCCESSFULLY FLASHED VIA OTA!")
    except Exception as e:
        print("\nOTA ERROR:", e)

