# Spotify record LED

A 64x64 HUB75 LED panel on an ESP32-S3 that shows a spinning vinyl record with the album
cover of whatever is playing on Spotify. A rotary knob dims the panel.

## Hardware
- Waveshare ESP32-S3 (16 MB PSRAM, octal flash) + 64x64 HUB75 panel
- HUB75 pins: R1=4 G1=5 B1=6 R2=7 G2=15 B2=16 A=18 B=8 C=3 D=42 E=9 LAT=40 OE=2 CLK=41
- Rotary knob (relative, +/-) on GPIO 45 / 46
- Panel on its own 5 V supply; the panel clock is **10 MHz** (20 MHz made it glitch red)

## Build and flash
ESP-IDF 6.1. `export.ps1` can pick the wrong Python on Windows; `tools/idf-env.ps1` sets the
environment by hand (edit the paths to match your install):

```powershell
. .\tools\idf-env.ps1
python $env:IDF_PATH\tools\idf.py -p COM4 flash
```

## How the first-time setup works
No Wi-Fi or Spotify details are compiled in. When the board has nothing saved, can't join its
Wi-Fi within 45 s of booting, or Spotify rejects the saved token, it opens the open hotspot
**LED-DISC-SETUP** with a captive web form (Wi-Fi name, password, Spotify code). Settings are
stored in flash (NVS) and survive power loss.

The Spotify code comes from the helper page in [`spotify-helper/`](spotify-helper/) (PKCE login
in the browser, public Client ID only, no client secret anywhere).

Developer shortcut: `python tools/spotify_auth.py` writes `main/secrets.h` with default
credentials. It is git-ignored. Delete it before building a unit you give away, and erase saved
settings with `esptool --port COM4 erase-region 0x9000 0x6000`.

## Layout
- `main/main.cpp` - panel setup, knob, brightness ladder, render loop
- `main/disc.cpp` - vinyl renderer (grooves, rotating sheen, spinning cover label)
- `main/net.cpp` - Wi-Fi, Spotify token refresh, now-playing polling, album-art download/decode
- `main/portal.cpp`, `main/config.cpp`, `main/screen.cpp` - setup hotspot, saved settings, setup screen
