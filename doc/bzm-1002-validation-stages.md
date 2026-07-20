# Bitaxe 1002 staged validation operator runbook

This runbook is for the BZM Bitaxe 1002 staged firmware. The Bonanza ASIC rail
is disabled by default, and the runtime target is deliberately ephemeral: it is
not stored in NVS, every reboot starts by proving `OFF_SAFE`, and the Bonanza
boot path does not start the legacy power manager, ASIC initialization, BAP, or
mining tasks. No stage is energized automatically at boot.

Validation is monotonic within one request. A target of stage `N` reruns stages
`0..N` in order using fresh evidence; a prerequisite cannot be skipped and a
previous `GOOD` never satisfies a new run. A request above the image's compiled
ceiling is `BLOCKED`, never silently clamped.

## Current release boundary

Stages 0 through 7 are implemented. Powered production admission is
fail-closed unless one of two safety contracts is explicitly selected:

- Independent-cutoff mode requires the bridge's independent VCORE, fan-tach,
  and trip-monitor capability evidence.
- Board-managed mode requires
  `CONFIG_BZM_1002_BOARD_MANAGED_SAFETY=y`. This deliberately accepts the
  on-board ESP, RP2040 bridge, TPS546, fan, and ASIC monitors as the safety
  authority. It does not weaken any live check: the TPS command must remain
  exactly 2.800 V, measured VOUT and TPS status must stay safe, the bridge
  lease/output readback must remain controlled, fan tach and ASIC telemetry
  must stay valid, and every stop or fault must prove physical `OFF_SAFE`.
- The present bridge still honestly reports `BAD_CAPABILITY_GAP` under its
  independent-hardware capability definition. Do not forge those bits;
  board-managed acceptance is an explicit ESP policy choice instead.
- An attended lab image may alternatively use the ESP-only shutdown override
  with a current-limited supply and an operator at the device.
- Stage 6 is implemented behind `CONFIG_BZM_1002_STAGE6_BALANCED_RAMP`. It
  follows the BIRDS reference behavior: activate the higher-voltage stack
  first, acknowledge that engine, then immediately activate the other stack.
  The UART has no atomic pair latch, so the explicit safety bound is one engine
  of transient skew per ASIC pair. Each batch pauses TDM with the true
  all-ASIC address `0xff`, proves `0xfec8` on ASICs `0x0a`, `0x14`, `0x1e`,
  and `0x28`, and later
  proves `0xfec9` plus fresh telemetry after resume. Every engine must read
  back busy plus writable configuration `0x04`; hardware-owned active bit
  `0x10` is allowed. Engine programming occurs only inside a clean parser
  window. An isolated unchecksummed TDM telemetry anomaly is retried while
  activation is frozen, an ASIC trip is immediate, and a continuous anomaly
  fails at the configured confirmation count. The final barrier rejects any
  sentinel result or parser fault. This remains engineering evidence until
  qualified across production devices.
- ASIC telemetry voltage conversion and limits require device-capture
  calibration. TDM byte 7 bit 7 is the documented combined PLL0/PLL1 lock bit;
  bits 5 and 6 are reserved, so DLLs are instead kept disabled and verified by
  direct control-register readback. Until the provisional voltage envelopes
  and clock behavior are qualified across production devices, stage 4 and
  stage 5 results are engineering evidence, not production qualification.
- A panic, watchdog, brownout, physical reset, or allocation abort cannot run
  an ESP software safe-off handler first. Board-managed production therefore
  relies on the RP2040 bridge's fixed lease to return 5 V/reset to safe state
  and on the TPS/board defaults to keep VCORE off across restart. The ESP must
  re-prove `OFF_SAFE` before it accepts another powered request.

## Safety gates

Powered stages have independent compile-time, admission, policy, and live
runtime gates:

1. `CONFIG_BZM_1002_MAX_STAGE` must include the requested stage.
2. `CONFIG_BZM_1002_POWERED_VALIDATION` must be compiled into the image.
3. The POST must contain the exact confirmation string
   `ENERGIZE_BZM_1002`.
4. The operator must provide the configured local arm while the device is
   already `GOOD/OFF_SAFE`. The USB method requires
   `bzm-arm ENERGIZE_BZM_1002` on the ESP's local USB Serial/JTAG console and
   consumes one RAM-only token. Images without it sample BOOT directly. An
   HTTP client cannot create either local arm.
5. A nonzero lease no longer than the compiled maximum must be supplied.
6. Production policy requires either fresh bridge evidence for all three
   independent safety capabilities or the explicit board-managed-safety
   contract. The attended-lab override is rejected in production.
7. The live supervisor, bridge heartbeat, fan, TPS, ASIC telemetry, trip, clock,
   and dispatch gates remain authoritative after admission.

Any `BAD` stage forces reset, bridge 5 V, and the TPS rail off. If shutdown
cannot be physically verified, the state becomes `SHUTDOWN_UNVERIFIED` and no
new start is allowed. A build toggle never overrides a live interlock.

## ESP firmware configuration

Configure with `idf.py menuconfig` under **Bitaxe Configuration -> Bitaxe 1002
Bonanza staged validation**, or set the corresponding sdkconfig values. These
are all BZM staged-validation build controls:

| Kconfig symbol | Allowed/default | Operator meaning |
|---|---|---|
| `CONFIG_BZM_1002_MAX_STAGE` | `0..7`, default `0` | Absolute image ceiling. Compile a separate image for each newly authorized ceiling. |
| `CONFIG_BZM_1002_POWERED_VALIDATION` | `y/n`, default `n` | Includes stages that can energize VCORE. Required for targets `2..7`; has no effect without the other gates. |
| `CONFIG_BZM_1002_LAB_VALIDATION` | `y/n`, default `n` | `y` selects attended-lab policy; `n` selects production policy. |
| `CONFIG_BZM_1002_ALLOW_ESP_ONLY_KILL_IN_LAB` | `y/n`, default `n`, depends on lab mode | Temporary lab-only escape hatch when the three independent bridge safety capabilities are absent. Production policy always rejects it. |
| `CONFIG_BZM_1002_BOARD_MANAGED_SAFETY` | `y/n`, default `n`, depends on powered mode | Explicitly accepts the on-board ESP/RP2040/TPS safety contract in production. The 2.800 V TPS command, measured VOUT, TPS status, bridge lease/readback, fan, telemetry, and safe-off checks remain mandatory. |
| `CONFIG_BZM_1002_USB_SERIAL_ARM` | `y/n`, default `n`, depends on powered mode and USB Serial/JTAG console | Replaces BOOT holding with a local `bzm-arm ENERGIZE_BZM_1002` command in lab or production policy. The arm is RAM-only, short-lived, and consumed exactly once. |
| `CONFIG_BZM_1002_USB_SERIAL_ARM_WINDOW_SECONDS` | `1..60`, default `30` | Maximum delay from a valid local serial command to the single powered POST that consumes it. |
| `CONFIG_BZM_1002_STAGE6_BALANCED_RAMP` | `y/n`, default `n`, depends on powered mode and `MAX_STAGE>=6` | Enables the reference-style bounded-skew sentinel ramp. Leave off for stages 0..5 and in the checked-in production defaults. |
| `CONFIG_BZM_1002_STAGE7_MINING` | `y/n`, default `n`, depends on powered mode, Stage 6, and `MAX_STAGE>=7` | Opens leased real-work validation only after stages 0..6 are `GOOD`. Leave off until Stage 6 device evidence is accepted. |
| `CONFIG_BZM_1002_STAGE7_PROOF_TIMEOUT_SECONDS` | `1..300`, default `15` | Maximum time after mining dispatch opens to prove a full four-ASIC dispatch and locally valid result. After proof, each new bounded result-recovery episode receives the same independent timeout. An initial or recovery timeout is `BAD` and forces safe-off. |
| `CONFIG_BZM_1002_STAGE7_MIN_VALID_RESULTS` | `1..100`, default `1` | Required current-generation results that map successfully and pass local work/nonce validation. |
| `CONFIG_BZM_1002_STAGE7_MAX_LOCAL_REJECTIONS` | `0..100`, default `1` | Maximum consecutive current-generation results that may fail local work/nonce validation. A later locally valid nonce resets the streak and proves recovery; the lifetime total remains visible. The next consecutive rejection is `BAD`. |
| `CONFIG_BZM_1002_STAGE7_ALLOW_MAPPING_RECOVERY` | `y/n`, default `n` | Allows bounded recovery when an unchecksummed nonce-like frame has corrupt ASIC/engine/sequence attribution. A complete dispatch plus a locally verified nonce establishes proof even if a bounded recovery is pending at that sampling instant; that recovery then gets its own timeout. The completed stage remains retained during later bounded recovery while pending counters stay visible. |
| `CONFIG_BZM_1002_STAGE7_MAX_MAPPING_REJECTIONS` | `1..100`, default `2` | Maximum consecutive unattributable nonce-like frames when mapping recovery is enabled. A later locally valid nonce resets the streak while the lifetime total remains visible. Exceeding the streak or failing to recover before timeout is `BAD`. |
| `CONFIG_BZM_1002_STAGE7_MIN_NONCE_DIFFICULTY` | `1..1024`, default `1` | Minimum locally calculated result difficulty counted as Stage-7 proof. A mapped current-work result below this floor is a local validation rejection; exceeding the rejection allowance is `BAD`. |
| `CONFIG_BZM_1002_STAGE7_LEAD_ZEROS` | `32..64`, default `36` | ASIC-side result filter. `36` reduces result UART traffic by about 4x versus `34` while retaining expected difficulty near 16. Good is locally verified proof before timeout; bad is timeout or below-floor/corrupt results beyond the rejection allowance. |
| `CONFIG_BZM_1002_STAGE7_DISPATCH_GAP_US` | `0..2000`, default `250` | After each 4-ASIC logical-engine write, service the bridge's fixed safety lease when 250 ms is due, wait for TX completion, leave this bounded idle gap, and drain TDM RX. Good is a complete 236/944 dispatch with the bridge watchdog continuously controlled; a lease or checkpoint failure is `BAD`. |
| `CONFIG_BZM_1002_STAGE7_ALLOW_PARSER_REALIGN` | `y/n`, default `n` | Enables BIRDS-style byte-at-a-time resynchronization during RUNNING. A bounded discard burst is nonfatal only after a valid frame resumes and clean windows prove stability. |
| `CONFIG_BZM_1002_STAGE7_PARSER_REALIGN_MAX_DISCARDS` | `1..256`, default `32` | Maximum discarded bytes in one recoverable burst. More is continuous corruption and `BAD`. |
| `CONFIG_BZM_1002_STAGE7_PARSER_REALIGN_CLEAN_WINDOWS` | `1..10`, default `2` | Consecutive 500 ms observations with no new discard, no partial buffered frame, and a valid frame after the last discard. Complete frames waiting in the consumer queue do not prevent recovery. |
| `CONFIG_BZM_1002_STAGE7_PARSER_REALIGN_MAX_WINDOWS` | `2..20`, default `6` | Maximum observations allowed to complete one realignment; default is approximately three seconds. |
| `CONFIG_BZM_1002_STAGE7_PARSER_REALIGN_MAX_EVENTS` | `1..20`, default `2` | Maximum observations with new discarded bytes inside one unresolved realignment episode. A completed recovery resets the count; exceeding it before recovery is continuous corruption and `BAD`. |
| `CONFIG_BZM_1002_FAN_MIN_RPM` | `1..30000`, default `1000` | Provisional minimum fresh tach result for `CONTROLS` and live monitoring. Qualify for the exact fan. |
| `CONFIG_BZM_1002_MAX_LEASE_SECONDS` | `1..300`, default `30` | Upper bound for every powered validation/`RUNNING` lease. |
| `CONFIG_BZM_1002_SAFE_OFF_VCORE_MV` | `0..1000`, default `250` | Maximum fresh TPS `READ_VOUT` for a `GOOD` safe-off proof. Qualify against the production discharge network. |
| `CONFIG_BZM_1002_TELEMETRY_MAX_AGE_MS` | `100..5000`, default `1000` | Maximum ASIC telemetry age; samples must also postdate the current stage's writes. |
| `CONFIG_BZM_1002_SENSOR_TDM_GAP_COUNT` | `0..63`, default `0` | Programs sensor TDM interval register `0x2d`. At 5 Mbaud with 100 slots of 127 bits, `0` is about 394 reports/s/ASIC and `63` is about 6.2 reports/s/ASIC. Larger values reduce return-path traffic; `GOOD` still requires fresh telemetry inside `TELEMETRY_MAX_AGE_MS`. |
| `CONFIG_BZM_1002_TEMP_MIN_C` | `-100..100`, default `-20` | Provisional inclusive lower engineering envelope for fresh ASIC telemetry. |
| `CONFIG_BZM_1002_TEMP_MAX_C` | `1..150`, default `105` | Provisional inclusive upper engineering envelope for fresh ASIC telemetry. |
| `CONFIG_BZM_1002_STACK_MV_MIN` | `0..2000`, default `300` | Provisional lower bound applied to CH0 (bottom stack) and CH1 (top stack). |
| `CONFIG_BZM_1002_STACK_MV_MAX` | `1..2000`, default `800` | Provisional upper bound applied to CH0 (bottom stack) and CH1 (top stack). |
| `CONFIG_BZM_1002_INTERSTACK_DIFF_ABS_MAX_MV` | `1..200`, default `50` | Maximum absolute CH2 differential between top-stack VSS and bottom-stack VDD; CH2 is expected near zero. |
| `CONFIG_BZM_1002_CH2_CONFIRM_SAMPLES` | `1..10`, default `3` | Consecutive fresh CH2 excursions required for shutdown, including a voltage-fault bit in that same unchecksummed frame. `1` restores immediate shutdown. Recovery resets the count. Trips and unrelated invalid data remain immediate. |
| `CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES` | `1..10`, default `1` | Consecutive fresh clears of the live unchecksummed combined PLL0/PLL1 telemetry bit required for shutdown. Recovery resets the per-ASIC count. The default remains fail-fast; the current hardware qualification image uses `3`. Initial Stage-5 PLL register and direct lock validation is always immediate. |
| `CONFIG_BZM_1002_STACK_MAX_SPREAD_MV` | `1..1000`, default `100` | Maximum per-ASIC `abs(CH0-CH1)` stack spread. Larger imbalance is `BAD`. |
| `CONFIG_BZM_BRIDGE_UPDATE_EXPERIMENTAL` | `y/n`, default `n` | Separately exposes the experimental bridge firmware updater; it is not a validation stage. |
| `CONFIG_GPIO_BUTTON_BOOT` | GPIO number, default `0` | Fallback physical arm input when the USB serial arm is not compiled. |

The checked-in `sdkconfig.defaults` is the safe image: maximum stage `0`,
powered validation off, lab mode off, lab override off, and bridge updater off.
It may prove only `OFF_SAFE`.

### Recommended image progression

Build and archive a distinct ESP binary and sdkconfig for every ceiling. Do not
raise more than one ceiling between hardware reviews.

```text
# Safe default image
CONFIG_BZM_1002_MAX_STAGE=0
# CONFIG_BZM_1002_POWERED_VALIDATION is not set
# CONFIG_BZM_1002_LAB_VALIDATION is not set
# CONFIG_BZM_1002_ALLOW_ESP_ONLY_KILL_IN_LAB is not set
# CONFIG_BZM_1002_BOARD_MANAGED_SAFETY is not set
# CONFIG_BZM_1002_USB_SERIAL_ARM is not set
# CONFIG_BZM_1002_STAGE6_BALANCED_RAMP is not set
# CONFIG_BZM_1002_STAGE7_MINING is not set
# CONFIG_BZM_BRIDGE_UPDATE_EXPERIMENTAL is not set

# Controls-only image
CONFIG_BZM_1002_MAX_STAGE=1
# CONFIG_BZM_1002_POWERED_VALIDATION is not set
# CONFIG_BZM_1002_LAB_VALIDATION is not set
# CONFIG_BZM_1002_ALLOW_ESP_ONLY_KILL_IN_LAB is not set

# Attended lab image for powered stage N
CONFIG_BZM_1002_MAX_STAGE=N
CONFIG_BZM_1002_POWERED_VALIDATION=y
CONFIG_BZM_1002_LAB_VALIDATION=y
CONFIG_BZM_1002_ALLOW_ESP_ONLY_KILL_IN_LAB=y
# CONFIG_BZM_1002_BOARD_MANAGED_SAFETY is not set
CONFIG_BZM_1002_USB_SERIAL_ARM=y
CONFIG_BZM_1002_USB_SERIAL_ARM_WINDOW_SECONDS=30
# Enable only in the separately archived Stage-6-or-later image:
# CONFIG_BZM_1002_STAGE6_BALANCED_RAMP=y
# Enable only in the separately archived Stage-7 image:
# CONFIG_BZM_1002_STAGE7_MINING=y
# CONFIG_BZM_1002_STAGE7_DISPATCH_GAP_US=250

# Production board-managed Stage-7 image
CONFIG_BZM_1002_MAX_STAGE=7
CONFIG_BZM_1002_POWERED_VALIDATION=y
# CONFIG_BZM_1002_LAB_VALIDATION is not set
# CONFIG_BZM_1002_ALLOW_ESP_ONLY_KILL_IN_LAB is not set
CONFIG_BZM_1002_BOARD_MANAGED_SAFETY=y
CONFIG_BZM_1002_USB_SERIAL_ARM=y
CONFIG_BZM_1002_USB_SERIAL_ARM_WINDOW_SECONDS=30
CONFIG_BZM_1002_STAGE6_BALANCED_RAMP=y
CONFIG_BZM_1002_STAGE7_MINING=y
CONFIG_BZM_1002_STAGE7_MAX_MAPPING_REJECTIONS=32
CONFIG_BZM_1002_STAGE7_DISPATCH_GAP_US=250
CONFIG_BZM_1002_STAGE7_PARSER_REALIGN_MAX_WINDOWS=20
CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES=3
```

For a lab image, replace `N` with one literal integer before building; `N` is
not valid Kconfig syntax. Keep the checked-in threshold defaults only as initial
engineering values. Record the qualified values beside each archived binary.

## Bridge firmware configuration

The ESP `CONTROLS` stage requires bridge protocol `1.1` and a bridge safety
stage of at least `1`.

```sh
# Stage 0: safe boot/status only; insufficient for ESP CONTROLS
BONANZA_BRIDGE_SAFETY_STAGE=0 cargo build --release

# Stage 1: arm/heartbeat lease for controls and attended-lab work
BONANZA_BRIDGE_SAFETY_STAGE=1 \
BONANZA_BRIDGE_LEASE_TIMEOUT_MS=2000 \
cargo build --release

# Stage 2: lease plus command-task trip latching
BONANZA_BRIDGE_SAFETY_STAGE=2 \
BONANZA_BRIDGE_LEASE_TIMEOUT_MS=2000 \
cargo build --release
```

`BONANZA_BRIDGE_SAFETY_STAGE` accepts only `0`, `1`, or `2` and defaults to
`0`. `BONANZA_BRIDGE_LEASE_TIMEOUT_MS` accepts `250..60000` milliseconds and
defaults to `2000`. Set `BONANZA_BRIDGE_FW_VERSION` only when a printable ASCII
release identifier is required; it does not change safety behavior.

A stage-2 bridge reports expected capabilities `0x000f` and production verdict
`BAD_CAPABILITY_GAP`, with the independent VCORE cutoff, autonomous tach
interlock, and independent trip-monitor bits clear. That remains useful and
honest diagnostic evidence. Never set capability bits merely to pass ESP
policy; use the explicit board-managed-safety option when that is the accepted
production contract.

## HTTP API contract

The validation routes are registered only for the BZM/Bonanza board. They use
the existing AxeOS network authorization policy. In the examples, replace
`bitaxe.local` with the device IP or hostname:

```sh
export BZM_URL=http://bitaxe.local
```

Read current configuration, bridge evidence, lease, fault, report, and all
stage definitions:

```sh
curl -sS "$BZM_URL/api/system/bzm/validation"
```

Start a run with `POST /api/system/bzm/validation`. The JSON object must have
exactly these four keys; unknown, duplicate, missing, fractional, or
out-of-range values are rejected. The body limit is 384 bytes and `confirm` is
limited to 32 bytes:

```json
{
  "targetStage": 0,
  "hold": false,
  "leaseSeconds": 0,
  "confirm": ""
}
```

- `targetStage` is an integer `0..7` and must not exceed the compiled ceiling.
- `hold` leaves a successful powered target energized in `HOLDING`; `false`
  runs a final safe-off before returning. It has no holding effect below stage
  2.
- `leaseSeconds` must be `0` for stages 0 and 1. It must be `1` through
  `configuration.maximumLeaseSeconds` for powered stages.
- `confirm` must be empty for the examples at stages 0 and 1 and must exactly
  equal `ENERGIZE_BZM_1002` for stages 2 through 7.
- `configuration.armMethod` identifies `USB_SERIAL_JTAG` or `BOOT_BUTTON`.
  For USB arm builds, require `localArm.active=true` before the powered POST;
  a successful admission immediately changes it back to false.

For a USB serial arm build, connect the ESP's native USB port, open its
Serial/JTAG console, and issue exactly:

```text
bzm-arm ENERGIZE_BZM_1002
GOOD: BZM powered validation armed once for 30000 ms
```

Good output is the `GOOD` line plus `localArm.active=true` in the validation
GET response. Bad output begins with `BAD`, or the GET reports inactive/zero
remaining time. The command is rejected unless the runtime is fault-free,
unowned, de-energized, and has a fresh `GOOD` final safe-off proof. Wrong
confirmation, expiry, successful consumption, stop, fault clear, maintenance,
and reboot all leave no usable token.

Admission and hardware outcome are separate. HTTP `200` plus
`operation.accepted=true` means only that the runtime admission gate accepted
the request. The operator must also require:

```text
report.overallName == "GOOD"
report.stages[targetStage].statusName == "GOOD"
report.stages[targetStage].codeName == "STAGE_OK"
```

For a non-held run, also require `report.stateName == "OFF_SAFE"`,
`report.energized == false`, and `report.finalSafeOff.statusName == "GOOD"`.
For a held powered run, require `report.stateName == "HOLDING"`,
`lease.active == true`, and continue monitoring every live field. A bridge
production-policy rejection can therefore return an admitted HTTP operation
whose `report.overallName` is `BLOCKED`; never treat admission alone as a pass.

Admission errors use:

- HTTP `400`: `INVALID_STAGE` or malformed/excess JSON;
- HTTP `409`: `BUSY` or `FAULT_LATCHED`;
- HTTP `412`: `BUILD_CEILING`, `POWER_NOT_COMPILED`,
  `CONFIRMATION_REQUIRED`, `LOCAL_ARM_REQUIRED`, `LEASE_REQUIRED`, or
  `LEASE_TOO_LONG`;
- HTTP `503`: the BZM runtime is unavailable.

### Lease heartbeat

Only a currently held validation/mining owner can renew a lease, and renewal
must arrive before expiry. Each accepted call replaces the deadline with the
requested interval, bounded by the compiled maximum:

```sh
curl -sS -X POST \
  -H 'Content-Type: application/json' \
  --data '{"leaseSeconds":10}' \
  "$BZM_URL/api/system/bzm/validation/heartbeat"
```

Require HTTP `200`, `operation.accepted=true`, and a fresh positive
`lease.remainingMs`. Send heartbeats comfortably inside the interval, for
example every 3 seconds for a 10-second lease. HTTP `409` means the session is
not held, already expired, faulted, or otherwise cannot renew. Supervisor lease
expiry forces safe-off; do not use expiry as a normal stop procedure.

### Stop and fault clear

Stop is always the normal end of a held run:

```sh
curl -sS -X POST \
  -H 'Content-Type: application/json' \
  --data '{"reason":"stage 3 evidence captured"}' \
  "$BZM_URL/api/system/bzm/validation/stop"
```

The optional `reason` is at most 96 bytes; `{}` uses `operator stop`. Require
HTTP `200`, `operation.accepted=true`, `report.stateName == "OFF_SAFE"`,
`report.energized == false`, and `report.finalSafeOff.statusName == "GOOD"`.
HTTP `500` means shutdown could not be verified; remove input power using the
qualified external method and investigate before another run.
HTTP `409` means an exclusive bridge update, ESP OTA, factory operation, or
restart owns the safe-off boundary. A generic stop cannot revoke that owner;
wait for the operation's matching release path.

After a `BAD` run, first verify the physical outputs and reported safe-off
evidence, correct the cause, then clear the latched fault with an empty object:

```sh
curl -sS -X POST \
  -H 'Content-Type: application/json' \
  --data '{}' \
  "$BZM_URL/api/system/bzm/fault/clear"
```

Fault clear internally requires a new `OFF_SAFE` proof and a successful bridge
fault clear. Require HTTP `200`, `operation.accepted=true`,
`fault.latched == false`, and `report.stateName == "OFF_SAFE"`. HTTP `409`
means there was no clearable fault or safe-off/bridge proof failed. Clearing a
fault never resumes validation or mining.

## Stage-by-stage procedure

Before every run, save the GET response and confirm the exact ESP and bridge
build identifiers. For lab work, connect the required instruments. In
board-managed production, first verify `OFF_SAFE GOOD`, the fixed 2.800 V
configuration, and the bridge lease/output readback. Increase
`CONFIG_BZM_1002_MAX_STAGE` by one only after the preceding stage's evidence
has been reviewed.

| Stage | Image and request | Expected `GOOD` evidence | `BAD` or `BLOCKED` evidence |
|---|---|---|---|
| `0 OFF_SAFE` | Safe default image (`MAX_STAGE=0`, powered off). POST target `0`, `hold=false`, lease `0`, confirm empty. | Reset asserted; bridge 5 V off; TPS enable and `OPERATION` off; fresh PGOOD low; VCORE at or below `SAFE_OFF_VCORE_MV`; fan full; final safe-off `GOOD`. | Any energized/readback contradiction, failed shutdown command, stale sample, PGOOD high, or discharge timeout is `BAD/SAFE_OFF_FAILED`. Runtime unavailable is `BLOCKED` operationally. |
| `1 CONTROLS` | `MAX_STAGE=1`, powered off; bridge protocol 1.1 safety stage >=1. POST target `1`, `hold=false`, lease `0`, confirm empty. | Bridge info/status coherent; arm plus heartbeat proven; reset/5 V/VCORE remain off; fan commanded 100%; fresh tach >= `FAN_MIN_RPM`; bridge disarms to safe output readback. | Incompatible bridge is `BLOCKED/NOT_IMPLEMENTED`; missing watchdog, arm/heartbeat failure, trip/fault, unsafe output, fan command failure, or low/stale tach is `BAD`. |
| `2 POWER_RAIL` | `MAX_STAGE=2`, powered on, bridge stage >=1, and either the independent or explicit board-managed production policy (or attended-lab override). Use only the fixed 2.8 V stacked-rail board profile. Issue the configured local arm; use a nonzero lease and exact confirmation. | Exact TPS profile, including the encoded `VOUT_COMMAND` word for 2.800 V, a 2.95 V command ceiling, and alert selectors, reads back; decoded command is within the PMBus representation tolerance; fixed rail is within 2.65..2.95 V; PGOOD/OPERATION valid; fresh VIN/VOUT/IOUT/VR temperature safe; faults clear; reset asserted and bridge 5 V off. Every later powered sample repeats the exact raw command-word comparison. | Production without either accepted safety contract is `BLOCKED/INDEPENDENT_KILL_REQUIRED`. Any request other than off or 2.8 V, a changed raw/non-finite `VOUT_COMMAND`, TPS/profile mismatch, status fault, stale/out-of-range power evidence, unexpected bridge outputs, or PGOOD timeout is `BAD`; the live command mismatch is reported as `TPS_VOUT_COMMAND` and forces safe-off. |
| `3 CHAIN_4` | `MAX_STAGE=3` with the same powered policy. Issue the configured local arm; use a bounded lease and exact confirmation. | Controlled 5 V/reset sequence finds exactly four independent ASICs at spaced TDM IDs `0x0a`, `0x14`, `0x1e`, `0x28`; full ID/control readbacks and NOOPs pass; no fifth responder; bridge readback shows leased 5 V on, reset released, fan full. | Zero, partial, duplicate, extra, misaddressed, later-unresponsive ASIC, parser/I/O error, or incoherent bridge state is `BAD`. |
| `4 SENSORS` | `MAX_STAGE=4`; configure qualified temperature, CH0/CH1 stack bounds, CH2 absolute differential, CH0/CH1 spread, and max age. Use the stage-3 powered procedure. | Reference sensor/trip registers read back on ASICs `0x0a`, `0x14`, `0x1e`, `0x28`; first-ASIC BIRDS UART drive control reads `0x44464444`; one all-ASIC write programs TDM control `0xfec9` (127-bit slot, 100 slots, enabled) and starts their schedules from a shared epoch; fresh post-write packets from all four ASIC IDs functionally prove the enable without injecting register replies into the active schedule; slow-clock divider `2`, delay `1`, and packet enable `0x0f` read back before activation; every ASIC supplies a post-configuration fresh valid temperature, CH0/CH1 stack voltages within bounds, CH2 near zero within its absolute limit, and CH0/CH1 spread within its limit; no fault/trip. A bounded activation discard burst must be followed by ten consecutive clean 100 ms parser windows. One finite CH2 excursion, including the co-occurring voltage-fault bit from the same unchecksummed frame, may recover before the configured consecutive count. | Missing or pre-configuration telemetry, stale/otherwise-invalid/NaN/out-of-range data, drive-control or other readback mismatch, failed synchronized TDM start, a parser that cannot produce the full clean one-second interval, any non-discard parser error, a trip, a voltage fault without a CH2 excursion, a CH2 anomaly reaching the configured consecutive count, or CH0/CH1 imbalance is `BAD`; missing snapshot/time capability is `BLOCKED`. Treat results as lab evidence until voltage scaling is calibrated from captures. |
| `5 CLOCKS` | `MAX_STAGE=5`; qualified 50 MHz reference, exact 800 MHz PLL profile, stage-4 telemetry policy, and live PLL-lock confirmation count. Use the powered procedure. | PLL0/PLL1 on all four ASICs have symmetric divider readbacks and direct enabled+lock evidence; DLL0/DLL1 controls `0x59`/`0x61` read back disabled (`0`); TDM is read back disabled as `0xfec8` during register checks, then re-enabled as `0xfec9` and proven by fresh sensor packets; TDM/UART controls remain `2`/`1`/`0x0f`/configured sensor gap/`0x108`; TDM byte 7 bit 7 reports combined PLL0/PLL1 lock; the hard dispatch gate remains closed. During later live monitoring, one isolated clear of that unchecksummed bit may recover on a newer locked sample before the configured consecutive count. There is no documented active-engine-count register. | Any initial direct divider, DLL, TDM-control, or PLL lock mismatch/timeout is immediately `BAD`. During live monitoring, combined PLL telemetry clears reaching the configured consecutive fresh-sample count, an open dispatch gate, missing TDM evidence, or degraded telemetry is `BAD`. Reserved telemetry bits 5 and 6 are not interpreted as DLL locks. |
| `6 BALANCED_RAMP` | `MAX_STAGE=6`, `STAGE6_BALANCED_RAMP=y`, powered procedure, deterministic sentinel work, and a qualified consecutive telemetry confirmation count. | Exactly 236 valid engines/ASIC, 944 total, 118/stack, and all 472 ASIC/pair commits acknowledged. Each ASIC engine domain is reset once. Result reporting reads back disabled on all four ASICs before the first TDM pause, preventing raw result packets while the framed transport is off. Each pair starts on the higher-voltage stack and completes the other before returning, so transient skew is at most one engine. TDM is broadcast-paused with address `0xff`, reads `0xfec8` on all ASICs, and resumes as `0xfec9`; engine writes occur only inside clean parser windows. Every engine reads back busy and writable config `0x04`, allowing hardware-owned active bit `0x10`; reserved coordinates stay untouched. Every batch has a safe pre-activation snapshot and newer post-commit safe/locked telemetry. A single TDM-frame anomaly may recover on a fresh sample before the confirmation count; an ASIC trip is immediate. The final TX/parser barrier sees zero sentinel results. Report is `GOOD`, reason `none`, `expected=472`, `actual=472`, `completed=472`. | Toggle off is `BLOCKED/NOT_IMPLEMENTED` with `balanced_pair_unavailable` before activation. Result-report control or TDM-control mismatch, engine-reset failure, partial pair, missing busy/config acknowledgement, channel imbalance beyond `STACK_MAX_SPREAD_MV`, invalid/reserved coordinate, stale telemetry, an immediate trip or anomaly that reaches the consecutive confirmation count, engine-window parser error, escaped sentinel result, or lease loss is `BAD` and forces safe-off. |
| `7 RUNNING` | `MAX_STAGE=7`, `STAGE6_BALANCED_RAMP=y`, `STAGE7_MINING=y`, fixed 2.8 V/800 MHz profile, bounded proof timeout, minimum valid-result count, mapping-recovery toggle/count, maximum local-rejection count, local difficulty floor, configured sensor interval, and inter-engine dispatch gap. | Stages 0..6 `GOOD`; TDM-framed result reporting reads back enabled on all four ASICs; post-enable telemetry is fresh, safe, and locked; a bounded entry barrier absorbs only discarded-byte transition residue and then requires one full second with no parser errors or queued/partial results before dispatch opens; every four-ASIC logical-engine write services the bridge safety lease at a maximum 250 ms cadence, completes TX, leaves the configured bounded bridge-drain gap, and drains RX before the next engine; leased supervisor owns `MINING`; `runningEvidence.statusName=GOOD`; at least one complete batch reports 236 logical/944 chip-engine writes; the configured number of current-generation results map and verify locally at or above the difficulty floor; bounded mapping/local recovery may already be pending when that genuine proof is established and then receives its own timeout; after initial proof, the stage remains complete during bounded recovery and lifetime totals/current pending streaks remain visible; any runtime discard burst is accepted only when realignment is enabled and stays within all byte/window/event bounds; live health remains `GOOD`. | Toggle off is `BLOCKED/NOT_IMPLEMENTED`. Result-report readback failure, bridge-lease or transport-checkpoint failure, a disabled/out-of-bounds/continuous parser corruption event, any non-discard parser error, early/partial/failed dispatch, escaped sentinel, mapping recovery disabled or above its count, no valid mapped nonce before the initial proof deadline, failure to recover before an independent recovery timeout, local work/nonce rejections above the configured bound, or a live safety fault is `BAD`, closes dispatch, and forces safe-off. Pool share acceptance is useful extra evidence but is not required because it depends on external pool difficulty/availability. |

Exact unpowered requests:

```sh
# Stage 0
curl -sS -X POST -H 'Content-Type: application/json' \
  --data '{"targetStage":0,"hold":false,"leaseSeconds":0,"confirm":""}' \
  "$BZM_URL/api/system/bzm/validation"

# Stage 1
curl -sS -X POST -H 'Content-Type: application/json' \
  --data '{"targetStage":1,"hold":false,"leaseSeconds":0,"confirm":""}' \
  "$BZM_URL/api/system/bzm/validation"
```

For target `N` from 2 through 7, verify the applicable independent or
board-managed safety contract. On a USB-arm image, issue
`bzm-arm ENERGIZE_BZM_1002` locally and confirm
`localArm.active=true`; otherwise hold BOOT on the already-running device.
Then issue the request before the local-arm window expires:

```sh
# Replace N with the one compiled and authorized target, for example 2.
curl -sS -X POST -H 'Content-Type: application/json' \
  --data '{"targetStage":N,"hold":true,"leaseSeconds":10,"confirm":"ENERGIZE_BZM_1002"}' \
  "$BZM_URL/api/system/bzm/validation"
```

JSON does not permit the symbolic value `N`; replace it with one integer. If
only a transient measurement is needed, use `hold:false`; the firmware performs
the final safe-off before returning. For `hold:true`, start heartbeat calls
immediately and always end with the stop endpoint.

### Full Stratum mining soak

There is no stage after `7 RUNNING`. Extend Stage 7 after its proof to validate
continued pool job rotation, repeated complete dispatches, nonce recovery, and
all live interlocks. For the connected validation device, the bounded harness
uses the wired host source address, issues the one-shot USB serial arm, renews
the lease, and always calls stop in a `finally` path:

```sh
python3 tools/run_bzm_stage7_device.py \
  --lease-seconds 300 \
  --post-proof-seconds 120
```

For this qualification run, compile the image with
`CONFIG_BZM_1002_MAX_LEASE_SECONDS=300`. The requested lease must cover staged
bring-up, the synchronous start response, expected control-plane reconnects,
and the post-proof observation. This does not relax the bridge's local
sub-second heartbeat or any electrical/thermal/clock/trip check. Restore the
checked-in 30-second default for images that do not require the longer bounded
soak.

Good output has all of the following:

- `runningEvidence.complete=true`, `statusName=GOOD`, at least one complete
  `236` logical / `944` chip-write dispatch, and an increasing
  `locallyValidResults` count.
- `health.statusName=GOOD`, no latched fault, bridge state `CONTROLLED` with
  runtime verdict `GOOD_CONTROLLED`, 5 V enabled and reset released while
  mining, and a live lease. A good health sample includes the fixed
  `VOUT_COMMAND=2.800 V` check,
  measured VOUT/PGOOD/TPS status, fan tach, ASIC telemetry, clocks, trips, and
  parser/recovery bounds.
- Corruption counters may increase only while their current consecutive
  streaks and parser episode stay within the configured bounds, and later
  valid nonces/clean windows visibly prove recovery. Parser clean windows
  require no partial buffered frame, but complete results waiting in the
  consumer queue are allowed. Likewise, an isolated
  live combined-PLL telemetry clear may be pending only below
  `pllLockConfirmSamples`; a newer locked sample must reset it.
- The final `STOP` is HTTP `200`, has no fault, retains completed running
  evidence, and reports `finalSafeOff.statusName=GOOD`, bridge 5 V off, and
  ASIC reset asserted.

Bad output is any missing initial proof, expired recovery deadline, over-limit
mapping/local/parser streak, non-finite or changed TPS command, unsafe measured
VOUT or TPS status, stale/unsafe telemetry, fan/trip fault, continuous PLL-lock
clears reaching the configured count, bridge lease
loss, partial dispatch, latched fault, or an unverified final safe-off. The
harness exits nonzero for failed Stage-7 proof or failed final safe-off.

## Reading results

Every stage reports exactly one status:

- `GOOD`: current evidence satisfies the whole stage contract.
- `BAD`: the hardware check ran and violated the contract. This latches a
  supervisor fault and triggers safe-off.
- `BLOCKED`: policy or a missing qualified capability prevented the check.
- `SKIPPED`: a lower stage was not `GOOD`.
- `NOT_RUN` or `RUNNING`: no terminal result exists yet.

Stable report codes include `STAGE_OK`, `STAGE_FAILED`,
`INVALID_CONFIGURATION`, `BUILD_CEILING`, `NOT_IMPLEMENTED`,
`POWER_NOT_COMPILED`, `LOCAL_ARM_REQUIRED`,
`INDEPENDENT_KILL_REQUIRED`, `POWER_LEASE_REQUIRED`,
`PREREQUISITE_FAILED`, and `SAFE_OFF_FAILED`. Preserve the complete response,
especially `runId`, each stage's `detail`, bridge capabilities/evidence,
production/runtime verdicts, fan RPM, lease, fault, and final safe-off.

## Stage 7 hardware observations

The 2026-07-20 Bitaxe 1002 runs used the fixed 2.8 V rail, 800 MHz PLL
profile, 250 us dispatch gap, and 36-bit ASIC result filter. With sensor TDM
gap `0`, the original bridge sampling point produced a 20-byte runtime discard
delta. Moving the bridge RX sample from 13 to 12 PIO cycles after the start
edge (the exact 1.5-bit center) reduced that delta to 18 bytes but did not
eliminate the burst. The centered run still completed all 236 logical / 944
chip-engine writes and returned locally valid difficulties `120.5`, `19.5`,
`38.4`, and `90.2`, followed by corrupt difficulty-zero results. This shows
that centering improved but did not solve return-path integrity, while the
nonce transform and four-ASIC mining path did produce real valid work.

The BIRDS parser recovers an unknown TDM header by dropping one byte and
searching again. The optional Stage 7 realignment policy now matches that
behavior without hiding the evidence: discarded-byte totals remain monotonic
and logged, only discarded bytes may change, a valid frame must follow the
last discard, clean windows must complete, and byte/window/event limits turn
continuous corruption into `BAD`. The event limit applies to new discard
bursts inside one unresolved episode; it resets after a completed realignment,
so later independently recovered bursts do not become a lifetime failure. The
sensor interval trial uses gap `63` at
the same 36-bit result threshold so its effect can be compared directly with
the gap-`0` centered run.

The first gap-`63` hardware pass reduced the Stage 4 observation from 2,553
frames with 9 discarded bytes to 97 frames with zero discarded bytes. All four
telemetry samples were still fresh at approximately 79 ms. That pass safely
returned to OFF_SAFE before mining because the complete Stage 0 through 6
sequence consumed the original 30-second supervisor lease; the attended Stage
7 trial therefore uses a 60-second lab lease while the checked-in safe default
remains 30 seconds.

With the lab lease extended to 60 seconds, the next gap-`63` run completed one
236-logical/944-chip-engine dispatch and returned three locally valid nonces at
difficulties `63.7`, `21.5`, and `188.2`. Four mapped frames failed local hash
validation and two nonce-like frames could not be attributed to the current
engine/sequence generation. The parser itself remained aligned at the Stage 4
checkpoint (`97` frames, zero discarded bytes). The then-current evidence
policy treated any attribution rejection as immediately fatal, so it forced a
verified safe-off despite the real mining proof. Attribution recovery is now a
separate default-off policy: the lab setting permits at most eight consecutive
such frames and requires a later locally valid nonce after the last rejection.
This does not excuse an over-limit burst, an invalid local hash streak above
its independent limit, or a run that cannot re-establish genuine proof.

The next gap-`63` run made four complete dispatches (944 logical / 3,776
chip-engine writes), mapped 34 results, and locally verified nine nonces while
recording 24 locally invalid results and 12 attribution rejects. The rejects
were not one continuous outage: the largest visible burst was seven frames,
followed by a valid difficulty-`28.2` nonce; other valid examples included
`61.5`, `19.8`, `30.3`, and `28.9`. Corrupt frames carried implausible fields
such as status `0xc`, sequence `253..255`, or time `80..109`, whereas valid
frames resumed with status `0x8` and bounded time/sequence values. The parser
also detected a nine-byte discard burst and entered its bounded realignment
state. A lifetime-total mapping ceiling of eight incorrectly stopped this
otherwise recovering stream, so both attribution and local-validation limits
are now consecutive streaks reset only by a later locally verified nonce;
lifetime totals, current streaks, and pending recovery remain exposed in the
API.

A following gap-`63` run confirmed the streak model but exposed an unrelated
safety-scheduling problem. It completed four dispatches (3,776 chip-engine
writes), mapped 73 frames, locally verified 15 nonces, and recovered all 11
attribution rejects. The ESP safety task nevertheless waited behind the ASIC
reactor lock long enough for the bridge's fixed two-second lease to expire.
The bridge latched safe-off exactly as designed; VCORE fell to approximately
`0.04 V`, PGOOD and `OPERATION` cleared, 5 V turned off, reset asserted, and an
explicit fault clear re-proved `OFF_SAFE GOOD`. The bridge watchdog was not
lengthened. Instead, the dispatch lock holder now services it at most every
250 ms from the per-engine checkpoint, using the live mining-dispatch lease
rather than the already-completed bring-up execution deadline.

The corrected checkpoint run kept the bridge status `GOOD_CONTROLLED` with
`leaseRemainingMs=2000` throughout eight complete dispatches (1,888 logical /
7,552 chip-engine writes). It mapped 138 results and locally verified 33
nonces while recording 105 locally invalid results and 29 attribution rejects.
The stream repeatedly returned to genuine proof: one clean snapshot showed 30
valid nonces, 27 lifetime attribution rejects, and both current streaks at
zero. Examples included difficulties `338.6`, `152.4`, `100.7`, `48.8`, and
`21.4`. No bridge, TPS, fan, clock, trip, dispatch, or parser safety fault
occurred, and final safe-off was `GOOD`.

This comparison answers the telemetry-cadence question precisely: increasing
the interval gap from `0` to `63` reduced Stage-4 traffic from roughly 2,553
to 97 observed frames and eliminated the Stage-4 discard burst in repeated
runs, so it materially improves baseline framing. It does not eliminate
Stage-7 result corruption under sustained dispatch; result frames still show
recoverable status/sequence/time damage. Therefore the slower cadence stays a
useful lab setting, not a claim that the bridge transport is fully corrected.
Once a complete dispatch and real nonce proof have made Stage 7 `GOOD`, a new
within-limit corruption episode no longer revokes completion: it is reported
as bounded recovery and receives a fresh proof timeout. Exceeding a streak,
failing to recover before that timeout, or any independent safety fault still
forces safe-off.

A subsequent gap-`63` run completed 14 dispatches (3,304 logical / 13,216
chip-engine writes), mapped 153 results, and locally verified 53 nonces. Both
the 117 lifetime attribution rejects and 100 lifetime local-validation rejects
had recovered to current streak zero. Valid examples included difficulties
`147.3`, `79.1`, `298.6`, and `67.9`; the bridge lease stayed controlled. A
later 27-byte parser discard burst was inside the configured 32-byte bound and
entered realignment with zero of two clean windows, but the old third-event-
per-lease ceiling made it immediately `BAD`. Earlier parser episodes had
already recovered, so that ceiling measured run lifetime rather than whether
corruption was continuous. The policy now resets its event count after every
proved realignment while still failing on excessive bytes, too many new
discard observations inside one unresolved episode, failure to produce a
valid post-discard frame and clean windows, recovery timeout, or any unrelated
parser counter. This run safely returned to `OFF_SAFE GOOD`.

The first run with that parser fix kept the parser clean and the bridge
controlled, but exposed the same lifetime-versus-current mistake in the
initial mining-proof lifecycle. At the 15-second deadline it had completed
nine dispatches (2,124 logical / 8,496 chip-engine writes), mapped 111
results, and locally verified 26 nonces. The only pending conditions were
bounded current streaks of two of eight attribution rejects and five of 32
local rejects, yet the monitor still required a perfectly clear sampling
instant before declaring initial proof. It therefore latched `MAPPING` even
though genuine proof and repeated recovery were present, then returned to
`OFF_SAFE GOOD`. Initial proof now means what the hardware evidence actually
requires: at least one complete four-ASIC dispatch and a locally verified
nonce inside the initial deadline. If mapping or local recovery is already
pending at that instant, Stage 7 becomes `GOOD` and that bounded episode gets
its own 15-second recovery deadline. Disabled recovery, an over-limit streak,
late initial proof, an expired recovery deadline, or an independent safety
fault remains `BAD`.

The final gap-`63` qualification run proved that behavior on the device. Stage
7 became `GOOD` after two dispatches and nine valid nonces while a one-of-32
local-rejection streak remained explicitly visible. It stayed `GOOD` for the
requested eight-second post-proof observation and finished with eight
dispatches (1,888 logical / 7,552 chip-engine writes), 139 mapped results, 27
locally verified nonces, 28 lifetime attribution rejects with current streak
zero, and 111 lifetime local rejects with current streak three. Valid examples
included difficulties `73.5`, `90.5`, `74.5`, `65.3`, and `54.3`. A nine-byte
discard burst arrived just before the intentional stop and correctly reported
bounded parser recovery (`9/32` bytes, one of two in-episode bursts, zero of
two clean windows) without revoking completed proof. The stop did not wait to
observe that particular episode's two clean windows, but earlier episodes and
the parser state-machine tests prove the recovery transition; the still-
pending state remained visible rather than being hidden. No bridge, TPS, fan,
clock, trip, dispatch, or lease fault occurred, and the harness exited after
`OFF_SAFE GOOD` with bridge 5 V off and ASIC reset asserted.

The first board-managed production soak exposed two representation/transport
details without changing the locked voltage. PMBus ULINEAR16 represents the
2.800 V command as 2.798828 V at this exponent (1.953125 mV per least
significant bit), so a 1 mV decoded-value comparison incorrectly rejected the
correct command. Runtime monitoring now compares the raw `VOUT_COMMAND` word
exactly against the configured profile and uses a 2 mV tolerance only for its
decoded diagnostic value. With that correction, Stage 7 reached `GOOD` after
two dispatches and six locally valid results, then ran for 20.9 seconds to 12
dispatches (2,832 logical / 11,328 chip-engine writes), 192 mapped frames, and
41 locally valid nonces. One subsequent unchecksummed telemetry frame from
ASIC `0x0a` carried a clear combined PLL bit even though Stage-5 direct PLL
readbacks had passed and all other health checks remained good. The fail-fast
live policy safely stopped at `OFF_SAFE GOOD`. The qualification image now
requires three consecutive fresh clears of that one live bit; an intervening
locked sample resets the count. Initial PLL validation, TPS command/status,
trip, bridge, fan, and all other safety failures remain immediate.

The next production attempt proved the new PLL policy was not the limiting
factor. It completed three dispatches (708 logical / 2,832 chip-engine writes)
and locally verified eight nonces, including difficulty `731.9`, with live
health still `GOOD`. It then accumulated ten consecutive unattributable
nonce-like frames against the qualification image's limit of eight and safely
stopped at `OFF_SAFE GOOD`. This is result-attribution corruption rather than
a rail, clock, trip, or bridge fault, and genuine work had already resumed
multiple times in prior captures. The production qualification value is now
32 consecutive mapping rejects, while the independent 15-second recovery
deadline still requires a later locally valid nonce. The checked-in safe
default remains two and mapping recovery remains default-off.

The first run with the 32-frame attribution allowance proved both recovery
changes in live mining. A 17-frame attribution streak recovered to zero, and
an isolated combined PLL clear on ASIC `0x1e` was reported at `1/3`; the next
fresh locked sample reset it without a clock fault. The run reached 16
dispatches (3,776 logical / 15,104 chip-engine writes), 254 mapped frames, and
55 locally valid nonces. It then safely stopped because parser realignment had
used all six observation windows: two bounded 9-byte discard bursts occurred
late in the same episode, leaving only one window after the second burst and
no opportunity to prove the required two clean windows. The qualification
image now permits 20 observations (approximately ten seconds), but still
allows at most 32 discarded bytes, two new discard events per unresolved
episode, and requires two clean windows. The checked-in fail-closed default
remains six observations.

The subsequent 20-window run reached Stage 7 `GOOD` and continued mining
through 52 dispatches (12,272 logical / 49,088 chip-engine writes), 733 mapped
frames, and 187 locally valid nonces. It safely stopped after a third 9-byte
discard observation exceeded the two-event limit in one unresolved parser
episode. The capture also exposed why the episode never completed: valid
frames and locally valid nonces continued, but the recovery proof required the
application result queue to become empty. That queue contains complete frames
waiting for the consumer and is not evidence of parser byte misalignment.
Runtime realignment now requires a byte boundary, a valid emitted frame after
the last discard, and the configured clean windows, but it does not require
the complete-result queue to drain. A partial buffered frame, a new discard,
an unrelated parser error, excessive bytes/events/windows, or an expired
recovery deadline remains `BAD`. Stage-entry settling retains its stricter
empty-queue requirement.

The first device run with that correction visibly realigned a 9-byte discard
burst after a valid frame and two clean windows while normal result processing
continued. It completed 17 dispatches (4,012 logical / 16,048 chip-engine
writes), mapped 286 frames, and locally validated 66 nonces before the
60-second external validation lease expired. The synchronous start response
was delayed through a device Wi-Fi reconnect, so the harness could not send
its first heartbeat before expiry. The dispatch gate closed at the deadline,
the next zero-engine dispatch was reported as transport status 4, and the
board returned to `OFF_SAFE GOOD`. TPS, bridge, fan, PLL, trip, telemetry, and
parser health were all still `GOOD`; this was an operator control-plane lease
expiry, not an ASIC-link or electrical failure. The qualification image uses
a 300-second operator lease for the bounded two-minute mining soak. The bridge
safety heartbeat remains local and sub-second, all live safety checks remain
unchanged, and the checked-in maximum-lease default remains 30 seconds.

The final 300-second-lease qualification completed the requested 120 seconds
after Stage-7 proof and stopped normally after 153.9 seconds of live evidence.
It finished with 89 complete dispatches (21,004 logical / 84,016 chip-engine
writes), 1,405 mapped results, 330 locally valid nonces, 557 lifetime mapping
rejects, 1,075 lifetime local rejects, and zero dispatch failures. At stop the
current mapping streak was zero and the current local streak was seven of 32;
completed proof remained `GOOD` under its bounded recovery deadline. Multiple
9-byte parser episodes visibly advanced through zero, one, and two clean
windows and returned to `RUNNING runtime health checks are GOOD`. The pool
accepted observed submissions at difficulties `2,116.8` and `7,206.8`, which
provides end-to-end Stratum evidence beyond local nonce verification. No TPS,
VOUT command/readback, bridge, fan, trip, telemetry, PLL, dispatch, or lease
fault occurred. The explicit stop returned HTTP 200 with no latched fault,
`OFF_SAFE GOOD`, bridge 5 V off, ASIC reset asserted, TPS off/discharged, and
bridge lease zero. An independent post-run GET confirmed the same state.

## Bridge updater maintenance boundary

Bridge update is not a startup or validation stage. Its firmware/status routes
exist only when `CONFIG_BZM_BRIDGE_UPDATE_EXPERIMENTAL=y` on a supported BZM
board:

- `POST /api/system/bridge/firmware`
- `GET /api/system/bridge/firmware/status`

The updater must acquire exclusive `BRIDGE_UPDATE` ownership only after a new
`OFF_SAFE` result is `GOOD`. Missing maintenance hooks fail closed before SWD.
The update path pauses mining, keeps the fan at 100%, excludes validation and
other maintenance owners, and re-proves safe-off when it releases ownership.
It never automatically resumes mining or restores an energized target. Keep
the updater disabled in production until signed persistent A/B staging and
automatic last-known-good recovery are implemented and qualified.

A bridge that cannot return a valid safety-status schema cannot produce a
`GOOD` `OFF_SAFE` proof and therefore cannot enter this in-band updater path.
Recover pre-safety-schema firmware with a separate offline/SWD procedure under
physical power isolation; do not weaken `OFF_SAFE` into a legacy-success path.

The ordinary ESP firmware and AxeOS WWW OTA routes use the same boundary with
exclusive `ESP_OTA` ownership on board 1002. Either upload is rejected unless
a new `OFF_SAFE` proof succeeds. Failed uploads release ownership through
another safe-off; a successful WWW upload does the same before responding, and
a successful ESP firmware upload keeps the device safe and exclusive until it
reboots. A bridge without the safety-status schema cannot use these in-band
update paths and requires the offline recovery procedure above.

`POST /api/system/restart` also closes dispatch and preempts an ordinary held
validation/mining lease only after a fresh safe-off proof. It then holds
exclusive `ESP_RESTART` ownership through reboot. The request returns HTTP
`409` and does not reboot if safe-off fails or any updater/factory maintenance
owner is active. Fatal Stratum allocation restart paths use the same guard;
they may not reset a powered BZM without first obtaining verified safe-off
ownership.

## Unit and integration coverage

Before flashing any image, build the production firmware and run the full
hardware-free firmware suite:

```sh
idf.py -B build build
idf.py -B .cache/qemu-test-build -C test-ci build
ESP_QEMU_BUILD_DIR=.cache/qemu-test-build tools/run_qemu_tests.sh
python3 -m unittest tools/test_validate_bitaxe_1002_config.py
python3 tools/validate_bitaxe_1002_config.py
```

The QEMU suite covers stage metadata and policy gates, exact runtime admission,
supervisor ownership/lease/fault/safe-off behavior, bridge framing and strict
safety-status decoding, TPS profile verification, exact four-chip discovery,
sensor freshness and bounds, symmetric 800 MHz PLL programming and failures,
topology holes/balance, bounded-skew ramp ordering/acknowledgement/fail-closed
behavior, dispatch closure, and
bridge-updater/OTA maintenance and guarded restart behavior. Run the bridge's
host tests separately:

```sh
cd ../bonanza-bridge-fw
cargo test --lib --target x86_64-unknown-linux-gnu
```

Unit tests establish code-path correctness; they do not qualify rail polarity,
cutoff latency, fan performance, ASIC telemetry scaling, or PLL status
semantics. Those require recorded measurements on the exact production board.
