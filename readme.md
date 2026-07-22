# ESP-Miner-Bonanza

ESP-Miner-Bonanza is open-source ESP32-S3 firmware and an AxeOS web interface for the Intel BZM2-based Bitaxe Bonanza. It currently targets board revision 1002: the quad-BZM2 [`bitaxeorg/bitaxeBonanza` 1002x hardware design](https://github.com/bitaxeorg/bitaxeBonanza/tree/1002x).

This is a Bonanza-specific fork of [ESP-Miner](https://github.com/bitaxeorg/ESP-Miner). It adds the BZM2 driver, RP2040 bridge protocol, fixed-profile startup, runtime safety supervision, and Bonanza health telemetry needed by the 1002x design. The four BZM2 ASICs run at the fixed production profile of 800 MHz with a 2.8 V board rail; frequency and voltage tuning are intentionally disabled.

The 1002x hardware is still described as a prototype by its hardware repository. Treat both the hardware and this firmware as active development work, and use the matching Bonanza ESP32 and RP2040 bridge firmware.

## AxeOS feature support

This table inventories the features exposed by the current AxeOS web UI. **Supported** means the feature is implemented for board 1002, **Partial** means only the listed Bonanza path works, and **Not supported** identifies an AxeOS control or data source that is not yet connected to the Bonanza runtime.

| AxeOS area | Feature | Status | Bonanza support |
| --- | --- | --- | --- |
| Dashboard | Bonanza Miner Health | Supported | Shows lifecycle state, pool/work health, BZM2 and engine counts, fixed clock and rail settings, measured rail voltage, board temperature, fan speed/RPM, bridge firmware and protocol compatibility, faults, and transport/result diagnostics. |
| Dashboard | Hashrate | Supported | Current, 1-minute, 10-minute, and 1-hour rates, error percentage, and four-ASIC/hash-domain detail are populated. |
| Dashboard | Shares and difficulty | Supported | Accepted and rejected shares, rejection reasons, pool difficulty, best/session difficulty, and block probability are populated. |
| Dashboard | Pool, block, and coinbase details | Supported | Shows active/fallback pool state, work age, response timing, block header data, version signals, and decoded coinbase outputs when supplied by the pool. |
| Dashboard | Hashrate-register heatmap | Supported | Reports all four BZM2 ASICs and their active engines through the generic hashrate monitor. |
| Dashboard | Block-found notification | Supported | The notification and dismiss action use the normal AxeOS path. |
| Dashboard | Widget layout and sensitive-data hiding | Supported | Widget visibility/order and the privacy toggle are browser-side AxeOS features. |
| Dashboard | Historical charts | Partial | Hashrate, Wi-Fi, and heap data are available. Generic power, temperature, voltage, and fan chart series are not yet populated from Bonanza telemetry. |
| Dashboard | Power and efficiency cards | Not supported | The Bonanza controller does not currently publish generic input power/current or expected-hashrate telemetry. |
| Dashboard | Legacy Heat and Fan cards | Partial | Accurate board temperature and fan readings are shown in Bonanza Miner Health, but the legacy cards still consume generic telemetry fields that are not wired to the Bonanza controller. |
| Top bar | Pause and resume mining | Not supported | The API changes the AxeOS paused flag, but it does not yet stop or restart the Bonanza safety controller and BZM2 work dispatcher. |
| Top bar | Restart | Supported | Restart and configuration restarts first move the Bonanza hardware to a verified safe-off state. |
| Scoreboard | Highest-difficulty shares | Supported | The result pipeline records and displays the top shares and their job, nonce, time, extranonce, and version-bit details. |
| Swarm | Discovery and device list | Supported | Subnet scan, manual add/remove, mDNS/IP access, refresh, filtering, sorting, grid/list views, and family identification support Bonanza devices. |
| Swarm | Metrics and remote actions | Partial | Hashrate, shares, best difficulty, uptime, pool difficulty, version, remote settings, restart, and identify work. Generic power/temperature totals and pause/resume have the same limitations noted above. |
| Logs | Live and downloaded logs | Supported | Real-time WebSocket logs, filtering, scroll pause/resume, clear, and download are available. |
| System | Device and runtime information | Supported | Shows Bonanza model/board/ASIC identity, uptime/reset reason, network state, CPU/heap usage, and firmware, AxeOS, and ESP-IDF versions. Bridge details are shown in Bonanza Miner Health. |
| System | Identify device | Supported | Triggers the normal on-device identify display. |
| Pool | Stratum V1 | Supported | Primary/fallback pools, suggested difficulty, extranonce subscribe, no TLS/system CA/custom CA, and coinbase decoding are implemented. |
| Pool | Stratum V2 | Supported | Primary/fallback pools, Standard and Extended Channels, optional authority key validation, encrypted transport, and Extended Channel coinbase decoding are implemented. |
| Pool | Automatic fallback and recovery | Supported | The protocol coordinator switches pools after bounded failures, stops requesting work when all configured pools are unavailable, and probes for recovery. |
| Network | Wi-Fi and hostname configuration | Supported | Wi-Fi scan, SSID/password changes, hostname changes, and restart are available. |
| Network | Setup access point/captive setup | Supported | The AxeOS AP onboarding route is available when station setup is required. |
| Network | mDNS and AxeOS discovery | Supported | Publishes HTTP and AxeOS DNS-SD records and supports `.local` access and Swarm discovery. |
| Theme | Appearance | Supported | Dark, light, and white themes plus custom accent colors are available and persisted. |
| Settings | ASIC frequency and voltage | Not supported | Board 1002 is deliberately locked to 800 MHz and a 2.8 V board rail; overclock mode does not expose tuning controls. |
| Settings | Automatic/manual fan control | Not supported | Bonanza safety requires the bridge-controlled fan at 100%; target temperature, minimum fan, and manual fan controls are not applied. |
| Settings | Overheat-mode reset | Not supported | Bonanza uses its dedicated fail-closed safety supervisor. A latched Bonanza fault requires inspection and restart rather than the generic overheat reset flow. |
| Settings | Display configuration | Supported | Display type, rotation, color inversion, and display timeout use the normal AxeOS display path. |
| Settings | Statistics/data logging | Partial | Logging and retention work, but generic power, temperature, voltage, and fan samples are not yet backed by Bonanza telemetry. |
| Update | Manual ESP firmware OTA | Supported | Uploading `esp-miner.bin` is guarded by a verified Bonanza safe-off transition before flash and restart. |
| Update | Manual AxeOS OTA | Supported | Uploading `www.bin`, progress reporting, and recovery mode are available. |
| Update | Latest-release lookup/download | Not supported | AxeOS still queries the upstream ESP-Miner release feed, which is not Bonanza-aware and must not be used as a source of Bonanza firmware. |
| Other | Bitcoin whitepaper | Supported | The bundled whitepaper link is a static AxeOS feature. |
| API | REST and live WebSocket API | Partial | System, ASIC, statistics, scoreboard, logs, settings, restart, identify, and OTA paths are available. Pause/resume and generic Bonanza power/heat/fan telemetry retain the limitations above. |

## Community
The upstream ESP-Miner firmware and AxeOS are maintained by OSMU, which hosts a [discussion forum](https://osmu.xyz).

Only flash images built for board 1002 from this repository. Generic binaries from the upstream ESP-Miner release page do not contain the complete Bonanza firmware and bridge contract.

## Bitaxetool
Bitaxetool is a command-line Python tool for flashing a Bitaxe and updating its configuration.

**Bitaxetool requires Python 3.4 or later and pip.**

Install bitaxetool from pip. pip is included with Python 3.4 but if you need to install it check <https://pip.pypa.io/en/stable/installation/>

```
pip install --upgrade bitaxetool
```
Bitaxetool includes the libraries needed to flash binaries to the Bitaxe Bonanza hardware.

**Notes**
 - The bitaxetool does not work properly with esptool v5.x.x, esptool v4.9.0 or earlier is required.
 - Bitaxetool v0.6.1 - locked to using esptool v4.9.0

```
pip install bitaxetool==0.6.1
```

- Flash a factory image to reset the miner to factory settings. The image must be built specifically for board 1002:

```
bitaxetool --firmware ./esp-miner-factory-1002-<version>.bin
```
- Flash just the NVS config to a bitaxe:

```
bitaxetool --config ./config-1002.cvs
```
- Flash both a factory image _and_ a config to your Bitaxe: note the settings in the config file will overwrite the config already baked into the factory image:

```
bitaxetool --config ./config-1002.cvs --firmware ./esp-miner-factory-1002-<version>.bin
```

## AxeOS API
The ESP-Miner-Bonanza UI is called AxeOS and provides an API to expose actions and information.

For more details take a look at [`main/http_server/openapi.yaml`](./main/http_server/openapi.yaml).

Available API endpoints:
  
**GET**

* `/api/system/info` Get system information
* `/api/system/asic` Get ASIC settings information
* `/api/system/statistics` Get system statistics (data logging should be activated)
* `/api/system/statistics/dashboard` Get system statistics for dashboard
* `/api/system/scoreboard` Get top 20 highest difficulty shares
* `/api/system/wifi/scan` Scan for available Wi-Fi networks
* `/api/system/logs` Download system logs

**POST**

* `/api/system/restart` Restart the system
* `/api/system/identify` Identify the device
* `/api/system/OTA` Update system firmware
* `/api/system/OTAWWW` Update AxeOS

**PATCH**

* `/api/system` Update system settings

**WEBSOCKETS**

* `/api/ws` Text stream log
* `/api/ws/live` JSONp stream of partial system info updates

### API examples in `curl` (works with IP addresses or .local hostnames):

```bash
# Get system information
curl http://YOUR-BITAXE-IP/api/system/info

# Get ASIC settings information
curl http://YOUR-BITAXE-IP/api/system/asic

# Get system statistics
curl http://YOUR-BITAXE-IP/api/system/statistics

# Get dashboard statistics
curl http://YOUR-BITAXE-IP/api/system/statistics/dashboard

# Get available Wi-Fi networks
curl http://YOUR-BITAXE-IP/api/system/wifi/scan

# Download system logs
curl http://YOUR-BITAXE-IP/api/system/logs


# Restart the system
curl -X POST http://YOUR-BITAXE-IP/api/system/restart

# Let the device say Hi!
curl -X POST http://YOUR-BITAXE-IP/api/system/identify

# Update system firmware
curl -X POST \
     -H "Content-Type: application/octet-stream" \
     --data-binary "@esp-miner.bin" \
     http://YOUR-BITAXE-IP/api/system/OTA

# Update AxeOS
curl -X POST \
     -H "Content-Type: application/octet-stream" \
     --data-binary "@www.bin" \
     http://YOUR-BITAXE-IP/api/system/OTAWWW


# Update a supported system setting
curl -X PATCH http://YOUR-BITAXE-IP/api/system \
     -H "Content-Type: application/json" \
     -d '{"statsFrequency": 60}'

# Stream logs
websocat ws://YOUR-BITAXE-IP/api/ws

# Stream Info API
websocat ws://YOUR-BITAXE-IP/api/ws/live
```

## mDNS Support

ESP-Miner-Bonanza includes comprehensive mDNS (multicast DNS) support for seamless network discovery and device accessibility. This feature enables automatic device discovery on local networks without requiring manual IP address configuration.

### Features

- **Automatic mDNS Initialization**: Device automatically registers with mDNS/Bonjour/Avahi services on network connection
- **Dynamic Hostname Registration**: Device hostname is registered as `<hostname>.local` (e.g., `bitaxe.local`)
- **Service Advertisement**: HTTP service is advertised as `_http._tcp` on port 80
- **AxeOS Subtype**: Advertises `_axeos._sub._http._tcp` for targeted DNS-SD discovery of AxeOS devices
- **Device TXT Records**: Includes board version, family, ASIC model, ASIC count, and firmware version as DNS-SD TXT records
- **Dynamic Hostname Updates**: mDNS hostname updates automatically when device hostname is changed via web interface
- **Hostname Normalization**: Automatically strips `.local` suffix when setting hostnames to prevent duplicate registrations
- **CORS Support**: Enhanced CORS handling to allow requests from mDNS hostnames
- **Hostname Conflict Resolution**: Automatically detects and resolves hostname conflicts by appending MAC address suffix when needed
- **Enhanced Swarm Discovery**: Swarm mode supports both IP addresses and .local hostnames for seamless network management

### Network Discovery

Once connected to your local network, the device becomes discoverable through:

```bash
# Using avahi-browse (Linux)
avahi-browse _http._tcp

# Discover AxeOS devices specifically
avahi-browse _axeos._sub._http._tcp

# Using dns-sd (macOS)
dns-sd -B _http._tcp

# Discover AxeOS devices with TXT records
dns-sd -B _axeos._sub._http._tcp

# Direct access
http://<hostname>.local
```

### Configuration

- **Default Hostname**: `bitaxe` (configurable via web interface)
- **Service Type**: `_http._tcp`
- **Subtype**: `_axeos._sub._http._tcp`
- **Port**: `80`
- **Instance Name**: `Bitaxe <family> <board> (<mac_suffix>)` (e.g., `Bitaxe Bonanza 1002 (A1B2)`)
- **TXT Records**: `board`, `family`, `asic`, `asic_count`, `fw_version`

### Hostname Conflict Resolution

If multiple devices attempt to use the same hostname, ESP-Miner-Bonanza automatically resolves conflicts by appending a MAC address-derived suffix (e.g., `bitaxe-12ab` if `bitaxe` is taken). This ensures unique network identification without manual intervention.

### Benefits

- **Zero-Configuration Discovery**: Devices automatically appear in network browsers
- **Cross-Platform Compatibility**: Works with Windows, macOS, Linux, and mobile devices
- **No IP Address Required**: Access devices using human-readable names
- **Automatic Resolution**: DNS resolution happens transparently in the background
- **Zero-Configuration Swarm Management**: Automatic device discovery and management without IP configuration
- **Enhanced Cross-Platform Compatibility**: Improved support across different network environments and discovery protocols

## Administration

The firmware hosts a small web server on port 80 for administrative purposes. Once the Bitaxe device is connected to the local network, the admin web front end may be accessed via a web browser connected to the same network at `http://<IP>`, replacing `IP` with the LAN IP address of the Bitaxe device, or `http://bitaxe.local`, provided your network supports mDNS.

### Recovery

In the event that the admin web front end is inaccessible, for example because of an unsuccessful firmware update (`www.bin`), a recovery page can be accessed at `http://<IP>/recovery`.

### Fixed ASIC and fan settings

Board 1002 runs a safety-locked profile: four BZM2 ASICs at 800 MHz, a 2.8 V board rail, and 100% bridge-controlled fan speed. The AxeOS `?oc` mode does not unlock frequency or voltage fields for Bonanza, and the generic automatic/manual fan controls are not applied.

## Development using ESP-Miner-Bonanza/devcontainer

This configuration allows you to edit locally and compile the source code using a docker container so you don't have to install the ESP-IDF toolchain and other supporting software on your computer to compile the firmware.

### Prerequisites

- Docker server

### Local PC Setup

These instructions will assume an installation to your home directory.
```
cd ~
git clone --recursive https://github.com/johnny9/esp-miner-bonanza.git
cd esp-miner-bonanza
git checkout <the branch you want>
git submodule update --init --recursive
# The next step builds the docker container that will compile the source code
# This will take several minutes to finish
docker build -t espminer-bonanza-build .devcontainer
```
### Building

```
cd ~/esp-miner-bonanza
docker run --rm -it -v $PWD:/workspace espminer-bonanza-build /bin/bash
git config --global --add safe.directory /workspace    # set git permissions or build will fail; only done once
cd /workspace
idf.py build
```	
Once the build is done exit out of the docker session and flash the new firmware.

## Development

### Prerequisites

- Install the ESP-IDF toolchain from https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/
- Install nodejs/npm from https://nodejs.org/en/download
- (Optional) Install the ESP-IDF extension for VSCode from https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension

### Cloning

This project uses git submodules (e.g. libsecp256k1). Clone with `--recursive`:
```
git clone --recursive https://github.com/johnny9/esp-miner-bonanza.git
```

If you already have a checkout, initialize the submodules with:
```
git submodule update --init --recursive
```

### Building

At the root of the repository, run:
```
idf.py build && ./merge_bin.sh ./esp-miner-merged.bin
```

Note: the merge_bin.sh script is a custom script that merges the bootloader, partition table, and the application binary into a single file.

Note: configure VS Code and ESP-IDF for the ESP32-S3 target used by board 1002.

### Flashing

With the bitaxe connected to your computer via USB, run:

```
bitaxetool --config ./config-1002.cvs --firmware ./esp-miner-merged.bin
```

Do not substitute a configuration for another Bitaxe model; this fork currently targets board 1002.

**Notes:** 
  - If you are developing within a dev container, you will need to run the bitaxetool command from outside the container. Otherwise, you will get an error about the device not being found.
  - Some Bitaxe versions can't directly connect to a USB-C port. If yours is affected use a USB-A adapter as a workaround. More about it [here](https://github.com/bitaxeorg/bitaxeGamma/issues/37).
  - Only ESP32-S3-WROOM-1 module type N16R8 (16MB Flash, 8MB Octal SPI PSRAM) is supported. This model number should be visible on the ESP32 module. Other module types without PSRAM or with Quad SPI PSRAM will not work with the normal firmware. More about it [here](https://github.com/bitaxeorg/ESP-Miner/issues/826).

### Wi-Fi routers

There are some Wi-Fi routers that will block mining, ASUS Wi-Fi routers & some TP-Link Wi-Fi routers for example.
If you find that your not able to mine / have no hash rate you will need to check the Wi-Fi routers settings and disable the following;

1/ AiProtection

2/ IoT 

If your Wi-Fi router has both of these options you might have to disable them both.

If your still having problems here, check other settings within the Wi-Fi router and the bitaxe device, this includes the URL for
the Stratum Host and Stratum Port.

## Attributions

The display font is Portfolio 6x8 from https://int10h.org/oldschool-pc-fonts/ by VileR.
