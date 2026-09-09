@echo off
REM ============================================================
REM  ESP32-AirPlay2 - 一键烧录（固件 + 网页 SPIFFS）
REM  使用前：开发板用 UART 口连接电脑（USB Host 占用原生 USB 口）
REM ============================================================
cd /d "%~dp0"
set "PIO=%USERPROFILE%\.platformio\penv\Scripts\platformio.exe"
if not exist "%PIO%" set "PIO=pio"
"%PIO%" run -t upload -t uploadfs -e esp32s3-usbhost
if %errorlevel%==0 (
  echo.
  echo [OK] 烧录完成。开发板上电后请连接热点 esp32-airplay2 配网。
)
pause
