# BZM Bitaxe 1002 ASIC validation phases

This document covers only the ASIC-side `CHAIN_4`, `SENSORS`, `CLOCKS`,
`BALANCED_RAMP`, and `RUNNING` phases. Power, fan, bridge lease, and local-arm
checks remain prerequisites owned by the production supervisor.

The implementation is split into:

- `bzm_bringup.c`: pure phase logic using injected register, clock, telemetry,
  and activation operations;
- `bzm_driver.c`: singleton serial/bridge adapter for production;
- `test_bzm_bringup.c`: hardware-free phase and failure-path tests.

Every phase returns `GOOD`, `BAD`, or `BLOCKED` plus a stable reason, ASIC ID,
register/PLL location, expected value, actual value, and completed-item count.
No result is inferred from a write succeeding: every configured register is read
back, and every sensor/clock phase requires newer telemetry from all four ASICs.

## Production call order

After the power supervisor has proven its own prerequisites, call:

1. `BZM_staged_initialize(state, &report)`
2. `BZM_staged_chain4(&report)`
3. `BZM_staged_sensors(&sensor_profile, &telemetry_policy, &report)`
4. `BZM_staged_clocks(&pll_profile, &telemetry_policy, &report)`
5. `BZM_staged_balanced_ramp(&telemetry_policy, &report)`
6. `BZM_staged_running(state, &telemetry_policy, &report)`

`BZM_staged_initialize` holds ASIC reset low and closes dispatch. `CHAIN_4`
pulses reset because an asserted reset cannot answer register commands, but
mining dispatch remains closed. Any later non-`GOOD` result immediately closes
dispatch, asserts reset low, and invalidates all earlier ASIC evidence.
`BZM_staged_hold_reset()` is the explicit abort/reset operation.

The production adapter renews and validates the bridge safety lease immediately
before the chain reset pulse and before every ASIC probe, register read/write,
and telemetry snapshot. Delays are split into chunks no longer than 250 ms with
a renewal before each chunk. A failed heartbeat, invalid status, non-controlled
state, or zero remaining lease becomes a sticky I/O failure and forces the
stage through the same fail-closed reset path. This keeps the bridge watchdog
alive while the synchronous stage holds the runtime lock.

Before `RUNNING`, register the production supervisor's live lease/interlock
predicate with `BZM_staged_set_dispatch_authorizer(callback, context)`. A null
callback is the default and means closed. The predicate is checked before a
dispatch and again for every engine write, so authorization expiring during a
236-engine dispatch stops the remaining writes and triggers the reactor's flush
path. The callback must be non-blocking and must not call back into the BZM
driver. `BZM_staged_poll(timeout_ms)` pumps telemetry/result frames for runtime
health monitoring without opening the dispatch gate.

## CHAIN_4

Configuration:

- expected ASIC count: exactly `4`;
- IDs in physical chain order: `0x0a`, `0x14`, `0x1e`, `0x28`;
- ASIC ID register: local/control offset `0x0b`;
- first write: `0x0000000a`, with hardware readback `0x0000010a`;
- following writes/readbacks: `0x00000114`, `0x0000011e`, and `0x00000128`.

`GOOD` means every unaddressed probe responded, every full 32-bit ID value read
back exactly, every assigned ID answered a NOOP, and a final unaddressed probe
returned no response. Expected report: `GOOD`, reason `none`, `completed_items=4`.

`BAD` includes a missing responder, an I/O/parser error, any readback mismatch,
an assigned-ID NOOP failure, or a fifth unaddressed responder. A timeout on the
final probe is expected evidence; a transport send/parser error is not treated
as proof that no fifth ASIC exists.

## SENSORS

Use `bzm_bringup_reference_sensor_profile()` for the Intel reference sequence:

| Register | Offset | Expected value |
| --- | ---: | ---: |
| UART TDM control | `0x07` | `0x0000fec9` (127-bit slot, 100 slots, enabled) |
| Slow clock divider | `0x08` | `0x00000002` (25 MHz from 50 MHz) |
| TDM transmit delay | `0x09` | `0x00000001` |
| UART thermal/voltage packet enable | `0x0a` | `0x0000000f` |
| Thermal and voltage clock divider | `0x3d` | `0x00000108` (both divide by 8 from 50 MHz) |
| DTS reset/powerdown | `0x2e` | `0x00000100` |
| Sensor TDM gap | `0x2d` | `CONFIG_BZM_1002_SENSOR_TDM_GAP_COUNT` (`0..63`; default `0`) |
| DTS configuration | `0x2f` | `0x00000000` (12-bit conversion mode) |
| Thermal/voltage threshold counts | `0x3c` | `0x000a000a` |
| Thermal tune/trip | `0x30` | `0x8001 | (2650 << 1)` (115 C reference trip) |
| Bandgap low nibble | `0x45` | read/modify/write to `0x3` |
| First-ASIC I/O drive strength | `0x51` | `0x44464444` (BIRDS UART integrity setting) |
| Voltage sensor reset/powerdown | `0x3e` | `0x00000100` |
| Voltage sensor configuration | `0x3f` | `0x81000000` (14-bit VM mode, gap 8) |
| Voltage sensor control | `0x40` | CH0/CH1 code `7561` (500 mV reference shutdown), enable bit set |

The caller must also provide a versioned telemetry policy with qualified minimum
and maximum temperature and CH0/CH1 stack-voltage values, a maximum absolute
CH2 inter-stack differential, a maximum permitted `abs(CH0-CH1)` spread, and a
nonzero maximum sample age. The policy also contains a CH2 consecutive-sample
confirmation count in the range `1..10`; production defaults to `3`. CH2
measures top-stack VSS relative to bottom-stack VDD and is expected near zero;
it is not a third stack voltage.

The 100-slot TDM frame is intentional: the wire IDs select slots `10`, `20`,
`30`, and `40`, leaving ten-slot timing separation between ASIC responses. A
four-slot frame would allocate only slots `0..3` and none of these ASICs could
transmit. The 127-bit slot accommodates a ten-byte telemetry frame at 5 Mbaud.
TDM is enabled only after every other sensor control has read back on all four
ASICs. One write to the all-ASIC address `0xff` starts all four schedules from
the same epoch, preserving the intended ten-slot timing separation. Stage 4
programs the configurable sensor interval gap on every ASIC. At the reference
5 Mbaud cadence, gap `0` is approximately 394 telemetry frames per second per
ASIC, while gap `63` is approximately 6.2 per second per ASIC; the runtime
freshness limit remains authoritative. Stage 4
first writes and reads back BIRDS I/O drive control `0x44464444` on ASIC
`0x0a`; the reference uses this setting to prevent UART unknown messages.
Stage 4 does not inject register reads after TDM starts: a fresh post-start
telemetry packet from every spaced ASIC is the functional enable proof. It may
absorb a bounded discarded-byte burst from that activation transition, but
then requires ten consecutive clean 100 ms parser windows before `SENSORS` can
be `GOOD`; any other parser error or failure to settle is `BAD`.

`GOOD` means every register above read back exactly on ASICs `0x0a`, `0x14`,
`0x1e`, and `0x28`, and
each ASIC supplied a post-configuration, fresh, valid, in-bounds telemetry
sample with no thermal/voltage fault or trip. Expected report:
`GOOD`, reason `none`, `completed_items=4`.

`BAD` includes an I/O failure, one mismatched register, missing telemetry,
telemetry older than the final configuration write, an expired sample, a fault
or trip, any non-finite value, temperature/CH0/CH1 outside the supplied bounds,
or a CH2 excursion that persists for the configured number of consecutive fresh
frames. A finite CH2 excursion, including a voltage-fault bit in that same
unchecksummed frame, is `PENDING`, not `BAD`: a fresh in-range
frame resets that ASIC's count, while the default third consecutive excursion
becomes `BAD`. Repeated reads of the same timestamp never advance the count.
Setting the count to `1` restores immediate CH2 shutdown. Missing snapshot/time
support is `BLOCKED` because the phase cannot collect the required evidence.

## CLOCKS

Use `bzm_bringup_pll_800_profile()`. No other clock profile is accepted in this
production phase:

- reference clock: `50 MHz`;
- reference divider: `2`;
- post1 divider: `1`;
- post2 divider: `1`;
- feedback divider: `128` (`0x80`);
- post-divider register: `0x1242`;
- resulting PLL frequency: `800 MHz`.

The same sequence is applied to PLL0 (`FBDIV 0x11`, `POSTDIV 0x10`, `ENABLE
0x12`) and PLL1 (`0x1b`, `0x1a`, `0x1c`) on every ASIC. Both PLLs are disabled,
programmed with identical divider values, enabled, read back, and polled until
their enable and lock bits (`value & 0x5 == 0x5`) are both set.

Before clock programming, the stage disables TDM on all four ASICs and reads
back TDM control `0xfec8`, preventing register responses from colliding with
unsolicited sensor packets. After clock programming it re-reads the remaining
sensor path controls on every ASIC: slow-clock divider `2`, TDM delay `1`, UART
packet enable `0x0f`, TDM gap `0`, and sensor clock divider `0x108`. DLL0 and
DLL1 control 5 registers (`0x59` and `0x61`) are explicitly written and read
back as `0`. TDM is then re-enabled as `0xfec9`; fresh telemetry from every ASIC
is the behavioral proof that the enable took effect.
There is no documented active-engine-count register. Engine inactivity is
therefore enforced by the hard mining dispatch gate remaining closed, not
claimed from an invented register readback.

`GOOD` means all 16 divider readbacks (two registers, two PLLs, four ASICs) are
exact, all eight PLLs report enabled+locked, all DLL controls and sensor/TDM
control readbacks are exact, the dispatch gate remains closed, and all four
ASICs provide fresh post-clock telemetry with TDM byte 7 bit 7 set. That bit is
the documented combined `(PLL0 lock & PLL1 lock)` indication; bits 5 and 6 are
reserved and are not DLL-lock evidence. Expected report:
`GOOD`, reason `none`, `actual=800`, `completed_items=4`.

`BAD` includes an asymmetric divider readback, either PLL failing to lock,
nonzero DLL control readback, or missing/stale/unsafe/combined-unlocked
telemetry. A requested
frequency or divider profile different from the exact values above is
`BAD/invalid_argument` before any register write.

## BALANCED_RAMP

The topology gate independently proves:

- exactly `236` usable engines;
- exactly `118` engines in each voltage stack;
- exactly `118` deterministic bottom/top pairs;
- unique physical IDs and valid coordinates for every compact engine ID.

The phase then requires two injected adapter capabilities:

1. a bounded sequential commit that starts on the higher-voltage stack,
   acknowledges it, then completes the other stack before returning; and
2. a dispatch barrier that stays closed until all `118 * 4 = 472` pair commits
   are acknowledged.

After each batch consisting of one balanced pair committed across all four
ASICs, the phase captures a timestamp and requires a new, fresh, in-bounds,
clock-locked telemetry sample from all four ASICs. Samples captured before the
four commits fail even when they are otherwise fresh. A qualified channel
spread limit is part of that policy. All telemetry and timing capabilities are
validated before the first activation.

Before each four-ASIC batch, the production adapter freezes activation and
requires a safe snapshot from all four ASICs. Because the TDM status packet is
unchecksummed, a non-trip anomaly may recover only through a fresh sample and
only before `ch2_confirm_samples` is reached. An asserted ASIC trip fails
immediately. The accepted snapshot is frozen for the four commits, so a later
TDM frame cannot replace the evidence while the batch is being programmed.

The BZM UART protocol provides serialized per-engine writes and no pair latch.
The implemented adapter therefore states and enforces the real hardware bound:
at most the first member of one pair can be active without its mate. It uses
the BIRDS higher-voltage-stack-first rule, queues deterministic 64-leading-zero
sentinel work as current and pending work, and requires a dword status readback
showing busy plus writable enhanced-mode config `0x04` before continuing. The
hardware-owned active bit `0x10` may also be present, producing the observed
`0x14` readback. A bounded three-attempt status read tolerates a transient UART
timeout but not a missing busy/config acknowledgement. A failure on the second
member is `BAD/balanced_pair_commit`; the staged wrapper immediately asserts
reset and leaves dispatch closed. The implementation is compiled only with
`CONFIG_BZM_1002_STAGE6_BALANCED_RAMP`; otherwise it returns
`BLOCKED/balanced_pair_unavailable` without activating an engine.

Each ASIC's engine domain is soft-reset before its first pair. For every pair
index, the adapter reaches a UART idle gap, broadcasts TDM pause through the
true all-ASIC address `0xff`, and reads `0xfec8` back from ASICs `0x0a`,
`0x14`, `0x1e`, and `0x28`.
After the four commits it broadcasts resume as `0xfec9` and requires fresh TDM
telemetry. The unaddressed discovery value `0xfa` is never used as the
all-ASIC broadcast.

The final barrier also proves all `944` engines and all `472` ASIC/pair commits
were acknowledged and drains UART TX. Every engine-programming window requires
zero buffered bytes and no change in discard, unexpected-header,
rejected/dropped-result, unmatched-readback, or telemetry-decode counters.
Only discard bytes caused outside those windows by controlled TDM transitions
may be rebaselined. The final barrier still rejects buffered data, queued
sentinel results, or any other parser fault.

The first physical Bitaxe 1002 Stage-6 validation on 2026-07-19 completed all
`472` commits with `GOOD`, including recovery from isolated TDM telemetry
outliers, followed by a `GOOD` final safe-off proof. This is device evidence,
not production qualification; production remains blocked until the independent
bridge safety capabilities are available.

## RUNNING

`RUNNING` requires a previously `GOOD` balanced ramp, revalidates the 236-engine
topology, and requires another fresh, safe, locked telemetry sample from every
ASIC. Only after that check does the production adapter initialize the mining
reactor and set its dispatch gate.

Real work is separately gated by `CONFIG_BZM_1002_STAGE7_MINING`. Once the
leased `MINING` owner opens dispatch, one complete dispatch must cover all 236
logical engines across all four ASICs (`944` chip-engine writes), and the
configured number of current-generation results must map and meet the compiled
local difficulty floor before the initial proof deadline. That genuine proof
establishes `GOOD` even if bounded corruption recovery is pending at the same
sampling instant; the pending episode then receives its own recovery timeout.
The per-engine
checkpoint also services the bridge's fixed two-second safety lease at a
maximum 250 ms cadence, because a complete dispatch owns the reactor lock long
enough to starve an external monitor. A failed dispatch or bridge renewal,
disabled or over-limit recovery, initial proof timeout, or a bounded episode
that cannot recover within its own fresh timeout changes the evidence to
`BAD`, closes dispatch, latches the fault, and forces safe-off. Once initial
proof is `GOOD`, a within-limit result-corruption or parser-realignment episode
does not revoke completion while that recovery window runs; lifetime totals,
current streaks, and pending flags stay visible. Independently recovered parser
episodes do not accumulate toward a lifetime ceiling: the configured parser
event limit instead bounds new discard bursts inside one unresolved episode.
Pool acceptance is recorded
separately but is not the hardware proof because it depends on external pool
difficulty and availability.

If the Stage-6 toggle is off or its validation is not `GOOD`, expected output is
`BLOCKED/prerequisite`. If the Stage-7 toggle is off, expected output is
`BLOCKED/NOT_IMPLEMENTED`. Any path where
`BZM_staged_dispatch_enabled()` becomes true before all prerequisites are
`GOOD` is a test failure and a release blocker.

## Tests

Run the full firmware suite with:

```sh
ESP_QEMU_BUILD_DIR=.cache/qemu-test-build tools/run_qemu_tests.sh
```

The bring-up tests cover exact profiles, four-ID discovery and fifth-ASIC
rejection, per-ASIC sensor readbacks, pre-configuration telemetry rejection,
fresh/safe telemetry, symmetric PLL programming and lock failure, rejection of
non-800 MHz profiles, sensor/TDM reproof after clocks, channel-spread rejection,
fail-closed toggle/capability detection, higher-voltage-first ordering, a
one-engine maximum partial-pair failure, status/config acknowledgement,
deterministic sentinel programming, post-commit telemetry after every one of
the 118 balanced batches, all 472 balanced commits plus clean parser barrier,
bridge lease status/chunking, and the final RUNNING telemetry gate.
Stage-7 evidence tests additionally cover pending-to-good progression, full
944 chip-engine dispatch accounting, first-write/partial-dispatch flushing,
mapping and local-validation rejection, counter rollback, invalid
configuration, and proof timeout.
