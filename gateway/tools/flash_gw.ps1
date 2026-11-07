param([string]$Hex = "c:\Users\Carmelo\Desktop\mqtt_gateway\.pio\build\atmega1284\firmware.hex", [string]$Part = "m1284p")
$d = "C:\Users\Carmelo\.platformio\packages\tool-avrdude"
& "$d\avrdude.exe" -C "$d\avrdude.conf" -c usbasp -p $Part -U "flash:w:${Hex}:i"

