import os
import shutil

def create_bundle():
    base_dir = r"c:\Users\Carmelo\Desktop\pjon_client_8btn_8led"
    hex_1284p = os.path.join(base_dir, r".pio\build\atmega1284p\firmware.hex")
    hex_324pb = os.path.join(base_dir, r".pio\build\atmega324pb\firmware.hex")

    if not os.path.exists(hex_1284p) or not os.path.exists(hex_324pb):
        print("Compilazione in corso per entrambi i microcontrollori...")
        os.system(f"cd /d {base_dir} && pio run -e atmega1284p -e atmega324pb")

    with open(hex_1284p, "r") as f:
        data_1284p = f.read().strip()

    with open(hex_324pb, "r") as f:
        data_324pb = f.read().strip()

    bundle_content = (
        ";=== IO16 UNIVERSAL FIRMWARE BUNDLE ===\n"
        "; Compatibile sia con ATmega1284P che con ATmega324PB\n\n"
        ";=== TARGET:atmega1284p ===\n"
        + data_1284p + "\n\n"
        ";=== TARGET:atmega324pb ===\n"
        + data_324pb + "\n"
    )

    out_client = os.path.join(base_dir, "firmware_io16_universal.hex")
    out_desktop = r"c:\Users\Carmelo\Desktop\firmware_io16_universal.hex"

    with open(out_client, "w") as f:
        f.write(bundle_content)

    with open(out_desktop, "w") as f:
        f.write(bundle_content)

    print(f"[OK] Creato file universale: {out_client}")
    print(f"[OK] Creato file universale sul Desktop: {out_desktop}")
    print(f"     - Dimensione bundle: {len(bundle_content)} byte")
    print(f"     - Supporta ATmega1284P ({len(data_1284p)} byte HEX)")
    print(f"     - Supporta ATmega324PB ({len(data_324pb)} byte HEX)")

if __name__ == "__main__":
    create_bundle()

