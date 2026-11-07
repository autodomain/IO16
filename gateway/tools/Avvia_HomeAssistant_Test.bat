@echo off
title IO16 - Test Dashboard Home Assistant
echo ============================================================
echo   IO16 Home Assistant Test Dashboard (Porta 8123)
echo ============================================================
echo.
echo Connessione al broker MQTT 192.168.1.51...
start http://localhost:8123
python C:\Users\Carmelo\Desktop\mqtt_gateway\tools\ha_test_dashboard.py
pause
