# dadamachines TBD-16 — USB MSC Firmware

USB Mass Storage Class firmware for the **[dadamachines TBD-16](https://dadamachines.com)** audio DSP platform. Exposes the on-board SD card as a USB drive for file management and firmware updates — no need to open the device.

This is a fork of [ctag-fh-kiel/tbd-usb-msc](https://github.com/ctag-fh-kiel/tbd-usb-msc), adapted for the TBD-16 (ESP32-P4) hardware.

## What It Does

This firmware runs in the **OTA1 slot** of the TBD-16 as a "housekeeping" partition:

- **SD card over USB** — Mounts the SD card as a USB Mass Storage device so a host PC can read/write files directly
- **Auto-reboot** — When the host unmounts/ejects the drive, the device reboots back into the main firmware (OTA0)
- **SPI command API** — Accepts commands from the RP2350 sequencer bridge over SPI
- **ESP32-C6 OTA** — Flashes ESP-Hosted slave firmware to the on-board ESP32-C6 WiFi module from a file on the SD card

## Target Hardware

| Target | Status |
|--------|--------|
| ESP32-P4 (TBD-16) | **Primary** — actively developed and tested |
| ESP32-S3 | Upstream support (not tested with TBD-16 hardware) |

### Rev 3.x SDMMC strategy

ESP32-P4 Rev 3.1 testing found card-dependent initialization and sustained-write
failures when a card remained in a marginal non-UHS 4-bit mode. The Rev 3.x
helper therefore:

1. restores the SD I/O rail to 3.3 V and power-cycles the card;
2. attempts tuned UHS-I SDR50 at 100 MHz, 4-bit, phase 2 with DDR disabled;
3. validates the actual negotiated mode and releases slot-0 state before retry;
4. falls back to High Speed 1-bit, phase 0 when UHS cannot be established.

Every successful initialization logs real frequency, configured limit, bus
width, UHS/DDR state, and OCR. A successful compile is not sufficient release
evidence: new builds require sustained archive extraction, checksum verification,
multiple cold starts, and multiple production card models before release.

On macOS, close MSC access with `sync`, then an explicit successful
`diskutil unmount` of the volume, and only then optionally eject the whole disk.
Never eject a still-mounted volume.

## Building

Requires [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32p4/get-started/index.html).

```bash
source ~/esp/esp-idf/export.sh
./build.sh
```

The build system automatically applies patches from `patches/` to ESP-IDF at configure time.

## Flashing

The MSC firmware is flashed to the OTA1 partition by the [TBD-16 Firmware Update Tool](https://github.com/dadamachines/dada-tbd-tui) or the [browser-based flasher](https://dadamachines.github.io/ctag-tbd/flash/10_stable_channel.html). For manual flashing with `esptool.py`, use `--chip esp32p4`.

## Project Structure

```
main/
  tusb_msc_main.c      USB MSC device setup and console commands
  spi_api.c/h           SPI slave command interface (RP2350 bridge)
  ota_c6_sdcard.c/h     ESP32-C6 firmware update from SD card
  custom_sdmmc_cmd.c    SDMMC command wrappers with retry/error handling
  Kconfig.projbuild     Menuconfig options (storage media, pin config)
patches/                ESP-IDF patches applied at build time
```

## Related Repositories

- [dadamachines/ctag-tbd](https://github.com/dadamachines/ctag-tbd) — Main TBD-16 firmware (DSP engine, web UI, plugin system)
- [dadamachines/dada-tbd-tui](https://github.com/dadamachines/dada-tbd-tui) — Terminal-based firmware update tool
- [ctag-fh-kiel/ctag-tbd](https://github.com/ctag-fh-kiel/ctag-tbd) — Upstream CTAG TBD project

## Acknowledgements

**CTAG TBD** was created by [Robert Manzke](https://github.com/ctag-fh-kiel/ctag-tbd) at the [Creative Technologies Arbeitsgruppe](https://www.creative-technologies.de/), Kiel University of Applied Sciences.

The TBD-16 adaptation is led by [dadamachines](https://dadamachines.com).

## Funding

This project is partially funded through the [NGI0 Commons Fund](https://nlnet.nl/commonsfund), established by [NLnet](https://nlnet.nl/) with financial support from the European Commission's [Next Generation Internet](https://ngi.eu/) programme, under grant agreement No [101135429](https://cordis.europa.eu/project/id/101135429).

[<img src="https://nlnet.nl/logo/banner-320x120.png" alt="NLnet" width="160">](https://nlnet.nl/project/TBD-DSP-Toolkit/)

## License

This firmware is licensed under the [GNU Lesser General Public License (LGPL 3.0)](https://www.gnu.org/licenses/lgpl-3.0.txt).

Upstream code from [ctag-fh-kiel/tbd-usb-msc](https://github.com/ctag-fh-kiel/tbd-usb-msc) is licensed under [Unlicense / CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/).

© 2025–2026 [Johannes Elias Lohbihler](https://dadamachines.com) for dadamachines. (TBD-16 adaptation)

See [LICENSE](LICENSE) for details.

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

## Example Output

After the flashing you should see the output at idf monitor:

```
I (329) app_start: Starting scheduler on CPU0
I (334) app_start: Starting scheduler on CPU1
I (334) main_task: Started on CPU0
I (344) main_task: Calling app_main()
I (344) gpio: GPIO[4]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0
I (354) example_main: Initializing storage...
I (364) example_main: Initializing wear levelling
I (374) example_main: Mount storage...
I (374) example_main:
ls command output:
.fseventsd
_pic.jpg
.__pic.jpg
README.MD
I (384) example_main: USB MSC initialization
I (384) tusb_desc:
┌─────────────────────────────────┐
│  USB Device Descriptor Summary  │
├───────────────────┬─────────────┤
│bDeviceClass       │ 239         │
├───────────────────┼─────────────┤
│bDeviceSubClass    │ 2           │
├───────────────────┼─────────────┤
│bDeviceProtocol    │ 1           │
├───────────────────┼─────────────┤
│bMaxPacketSize0    │ 64          │
├───────────────────┼─────────────┤
│idVendor           │ 0x303a      │
├───────────────────┼─────────────┤
│idProduct          │ 0x4002      │
├───────────────────┼─────────────┤
│bcdDevice          │ 0x100       │
├───────────────────┼─────────────┤
│iManufacturer      │ 0x1         │
├───────────────────┼─────────────┤
│iProduct           │ 0x2         │
├───────────────────┼─────────────┤
│iSerialNumber      │ 0x3         │
├───────────────────┼─────────────┤
│bNumConfigurations │ 0x1         │
└───────────────────┴─────────────┘
I (554) TinyUSB: TinyUSB Driver installed
I (564) example_main: USB MSC initialization DONE

Type 'help' to get the list of commands.
Use UP/DOWN arrows to navigate through command history.
Press TAB when typing command name to auto-complete.
I (724) main_task: Returned from app_main()
esp32s3> 
esp32s3> help
help 
  Print the list of registered commands

read 
  read BASE_PATH/README.MD and print its contents

write 
  create file BASE_PATH/README.MD if it does not exist

size 
  show storage size and sector size

expose 
  Expose Storage to Host

status 
  Status of storage exposure over USB

exit 
  exit from application

esp32s3> 
esp32s3> read
E (80054) example_main: storage exposed over USB. Application can't read from storage.
Command returned non-zero error code: 0xffffffff (ESP_FAIL)
esp32s3> write
E (83134) example_main: storage exposed over USB. Application can't write to storage.
Command returned non-zero error code: 0xffffffff (ESP_FAIL)
esp32s3> size
E (85354) example_main: storage exposed over USB. Application can't access storage
Command returned non-zero error code: 0xffffffff (ESP_FAIL)
esp32s3> status
storage exposed over USB: Yes
esp32s3> expose
E (108344) example_main: storage is already exposed
Command returned non-zero error code: 0xffffffff (ESP_FAIL)
esp32s3> 
esp32s3> 
esp32s3> read
Mass Storage Devices are one of the most common USB devices. It use Mass Storage Class (MSC) that allow access to their internal data storage.
In this example, ESP chip will be recognised by host (PC) as Mass Storage Device.
Upon connection to USB host (PC), the example application will initialize the storage module and then the storage will be seen as removable device on PC.
esp32s3> write
esp32s3> size
Storage Capacity 0MB
esp32s3> status
storage exposed over USB: No
esp32s3> expose
I (181224) example_main: Unmount storage...
esp32s3> status
storage exposed over USB: Yes
esp32s3> 
esp32s3>
```
