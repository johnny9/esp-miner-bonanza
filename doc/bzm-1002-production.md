# Bonanza Bitaxe 1002 production support

Board 1002 uses a fixed, automatic production profile. After Wi-Fi and the
normal Stratum queue are ready, the ASIC driver transitions through:

```text
SAFE_OFF -> STARTING -> MINING -> FAULT or MAINTENANCE
```

There is no manual arm, numeric startup level, operator heartbeat, or remote
bring-up action. The controller first proves safe-off, verifies a compatible
bridge, enables and verifies the fixed 2.800 V TPS546 profile, discovers four
ASICs, configures telemetry and both 800 MHz PLLs, activates 944 engines with
bounded stack skew, and then starts the normal ESP-Miner work, result,
statistics, and Stratum tasks.

## Work scheduling

Each of the 236 logical engines receives an independent header/extranonce and
uses the full 32-bit nonce domain. Only the four ASIC instances of that one
logical engine divide the nonce range. Per-engine current and previous
generations remain attributable through enhanced-mode sequence identifiers.
The timestamp budget is 60, matching the BIRDS production scheduling model.
The ASIC-facing PIO link remains at the qualified 5 Mbaud rate. The separate
raw bridge-to-ESP UART runs at 2 Mbaud, more than twelve times its 160 kbit/s
measured receive payload budget, to provide board-level signal margin without
reducing ASIC work coverage. Cumulative PIO FIFO and DMA-ring overflow counters
are read through the bridge control protocol.

## Safety and recovery

The RP2040 bridge owns an independent short output lease. ESP-Miner services
that lease during startup, work programming, and continuous monitoring. A
persistent bridge, fan, TPS, telemetry, PLL, parser, attribution, or dispatch
fault closes work dispatch and proves safe-off. ESP/AxeOS OTA, bridge firmware
updates, and restart acquire the same verified-safe maintenance boundary before
proceeding.

Parser byte realignment and result-attribution recovery are bounded production
behaviors. Their byte, event, rejection, and timeout thresholds are the only
Bonanza recovery settings exposed in Kconfig. Voltage and frequency are not
tunable.

## AxeOS status

The normal `/api/system/info` response includes optional `asicHealth` data.
AxeOS renders lifecycle and state age, current pool/work age, warm-up and
average hashrate, four-ASIC and 944-engine status, the locked clock and voltage,
measured VOUT, temperature and fan telemetry, bridge compatibility, recovery
counters, and the last persistent fault with a safe next action.

Qualification snapshots, raw templates, midstates, per-engine traces, and
manual startup controls are intentionally absent from production firmware.
The production bridge updater accepts a separately supplied raw RP2040 image,
programs and read-back verifies it over the onboard SWD connection, resets the
bridge, and confirms its reported version before completing.

## Bridge compatibility

| ESP-Miner Bonanza controller | Required bridge protocol | Result |
| --- | --- | --- |
| Production MVO | major 1, minor 3 or newer compatible minor | Mining allowed with raw RX bytes and the RX-stats command |
| Production MVO | protocol missing, major mismatch, lease/trip policy missing, or required control path missing | Safe-off with an incompatible-bridge fault |

Before VCORE can be energized, the bridge must report the fixed trip-latch
policy plus coherent lease, reset, 5 V, full-fan, sampled-trip, and fault
state. The ESP then independently verifies live fan tach, the TPS command,
PGOOD, measured VCORE, power status, and temperature. Legacy bridge capability
fields for hardware-independent cutoff and tach interlock remain truthful but
are not treated as implemented on board 1002.

## Verification

Run the host/QEMU tests, frontend tests, and complete ESP-IDF build before an
OTA. A device regression must then verify automatic mining, independent work
across all logical engines, pool job rotation, live AxeOS updates, bounded
parser recovery, and verified safe-off before restart or maintenance.
