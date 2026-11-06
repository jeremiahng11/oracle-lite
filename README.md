# Oracle Lite

Voice notes, meeting minutes and web answers on an ESP32-S3 e-paper device. Setup Wi-Fi AP: `Oracle Lite`, password `Ora-XXXXXXXX` (shown on screen).
The HTTPS trust store `certs/oracle_ca_bundle.pem` now carries 17 common public roots (Google Trust Services, Let's Encrypt, DigiCert, GlobalSign, SSL.com, Sectigo/USERTrust, Amazon) so a CA rotation at api.openai.com does not break Ask.

Firmware version: see `version.txt` . Each `pio run -t upload` raises the rc number for the next build (`scripts/version.py`).

## Hardware

Waveshare ESP32-S3-ePaper-1.54 with 8 MB flash, 8 MB OPI PSRAM and a microSD card.
Use the supported 8/8 MB board revision. Older 4/2 MB boards are unsupported.

## Build and upload

Open this entire folder in VS Code with the PlatformIO extension. The project
pins Arduino-ESP32 3.2.0, the PioArduino platform and its library dependencies.
The first build downloads these dependencies and requires an internet connection.

With the PlatformIO command-line tools installed, run from this folder:

```sh
pio run -e oracle_lite
pio run -e oracle_lite -t upload
pio device monitor -b 115200
```

Connect the board by USB before uploading. If automatic bootloader entry fails,
hold BOOT while resetting the board, then release BOOT and retry the upload.
The build output is `.pio/build/oracle_lite/firmware.bin`.

This is a PlatformIO project. Opening only the `.ino` in Arduino IDE does not
apply the pinned platform, partition layout or embedded certificate bundle.

## Setup and use

Upload the firmware with PlatformIO (`pio run -t upload`). On first start, hold
both buttons to open the Web Companion (Wi-Fi `Oracle Lite`) and enter your
Wi-Fi details and your own OpenAI API key in Setup. The supplied source
contains no personal credentials. Settings are stored on the device.

Hold the upper button to record a memory; release it to save. Hold the lower
button to ask a question. Press the lower button during a memory recording to
add a Moment. Double-press the upper button for the daily briefing or the lower
button for the weekly briefing. Hold both buttons for three seconds to open
or close the Web Companion. Offline memories stay on the SD card for later
processing.

Recordings and transcripts are stored on the SD card under `/notes`. Back up
that directory before experimenting with storage formats. Keep the local portal
on a trusted private network.

## Make changes

- `oracle_lite/oracle_lite.ino`: startup, button actions and application flow.
- `oracle_lite/config.h`: board pins, timing and firmware defaults.
- `oracle_lite/src/app/`: recording, storage, networking, settings and interface.
- `oracle_lite/src/audio/`, `codec_board/`, `esp_codec_dev/`: audio hardware support.
- `oracle_lite/src/display/`, `i2c_bsp/`, `power/`: display and board support.
- `platformio.ini`: pinned build settings, libraries and version identifier.
- `partitions_oracle_8MB.csv`: flash layout.
- `certs/oracle_ca_bundle.pem`: embedded certificates for HTTPS connections.

Keep the partition layout and memory settings unless your hardware changes
require a migration. Version identifiers are set in `platformio.ini`, with
fallbacks in `oracle_lite/config.h`. Normal uploads preserve device settings;
full-chip erases and partition changes may not.

The firmware uses OpenAI services for its online features. Those features need
Wi-Fi and your own API key and may incur API usage charges. Audio recording
works offline. See `THIRD_PARTY.md` for bundled component notices.
