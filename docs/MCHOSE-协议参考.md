# MCHOSE HID configuration protocol

Recovered from the M HUB web driver bundle (`mchose.com.cn`, chunk
`purify.es-Dc-oxOSy.js`, deobfuscated in `protocol/`). Covers the MCHOSE mice
that enumerate under vendor `0x5253` (RealTek-assigned) and the newer `0x3837`,
which the driver treats identically: L7 / L7 Pro / L7 Ultra, M7, A7, K7.

This describes the L7 family. The G7 speaks a different protocol, described in
[G7 family](#g7-family) at the end.

Verified device here: **MCHOSE L7 Pro**, USB `5253:1020` on the dongle,
wireless device ID `00b0` (176, which is what the driver's own model table
lists for this mouse), firmware 5.12.2.1, 26000 DPI sensor, LOD 1 or 2 mm.
Everything below was exercised against that mouse on 2026-08-28: the frame,
the inversion, and every offset in the config block. The six DPI stages came
back as 400 / 800 / 1600 / 3200 / 6400 / 26000, the factory preset, which is
what pins the layout.

## Interface

Three USB interfaces. Configuration lives on the third one, the vendor
collection: usage pages `0xFF01` (65281) and `0xFF0B` (65291). On this machine
that is `/dev/hidraw14`; never hardcode the number, match the report descriptor.

Two report IDs carry the whole protocol:

| Report | Payload | Use |
|---|---|---|
| `0x11` | 20 bytes | short commands (rate, LOD, sleep, DPI, profile, RGB) |
| `0x12` | 64 bytes | long commands (full config, per-axis DPI, macros) |

Both are **feature reports**: `HIDIOCSFEATURE` to write, `HIDIOCGFEATURE` to
read back. Output reports are not used by this protocol.

## Frame

Request payload, before inversion:

```
byte 0    command
byte 1..  arguments, per schema below
rest      zero
```

Then **every payload byte is inverted (`^ 0xFF`)**, padding included, so an
all-zero tail goes on the wire as `FF FF FF ...`.

Response, from `HIDIOCGFEATURE` on the same report ID:

```
byte 0    report ID          (not inverted)
byte 1    command ^ 0xFF     (echo, use it to match the reply)
byte 2..  payload ^ 0xFF
```

There is no checksum. The driver retries a read up to 6 times when the echoed
command does not match, and up to 5 times on a busy device.

## Read commands

| Report | Cmd | Fields |
|---|---|---|
| `0x11` | `0x03` | `bond:u8`, `vid:u16`, `pid:u16`, `connect:u8`, `game:u8` |
| `0x11` | `0x04` | `versionLength:u8`, `version:char[versionLength]` |
| `0x11` | `0x06` | `vid:u16`, `pid:u16`, `fwVersion:u32`, `connectMode:bit3`, `connectStatus:bit1`, `_:bit4`, `batteryLevel:u8`, `chargeStatus:u8` |
| `0x11` | `0x1b` | RGB: `enable`, `id`, `useIndependent`, `brightness`, `lightSetting`, `closeTime`, `speed`, `openRandomColor`, `red`, `green`, `blue`, `red2`, `green2`, `blue2`, `changeDirect` |
| `0x12` | `0x67` | full config, see below |
| `0x12` | `0x65` / `0x77` / `0x63` | macro body / macro data / macro name |

All multi-byte integers are little-endian.

### `0x12 0x67`, the full config

```
profileIndex   u8
gDpiIndex      bit4     active DPI stage on the 2.4 GHz link
gRateIndex     bit4     report rate index on the 2.4 GHz link
usbDpiIndex    bit4     active DPI stage when wired
usbRateIndex   bit4     report rate index when wired
reserved       u8
dpi0..dpi5     u16 x6   the six DPI stages, raw DPI (e.g. 1600), not encoded
dpiSum         u8       how many stages are enabled
sensor         u8
keyDebounce    u8       ms
sleep          u8       minutes
button1..6     bit4 index + bit4 type + u24 value, per button
reserved1..5   u8 x5
rotateVal      u8
val            u8
dpiVal0..5     u16 x6   Y-axis DPI when the stages are not square
```

Nibble order on the wire is `usbRateIndex, usbDpiIndex, gRateIndex, gDpiIndex`
(the write schema is authoritative; the read parser lists them the other way
round because it reads the low nibble first).

## Write commands

| Report | Cmd | Schema |
|---|---|---|
| `0x11` | `0x02` | `game:u8`, the game mode. Reads back at `0x11 0x03` byte 6. |
| `0x11` | `0x0A` | `sleepStatus:u8`, `sleep:u8` |
| `0x11` | `0x0B` | `data:u16` |
| `0x11` | `0x2B` | RGB, same 15 fields as read `0x1b` |
| `0x11` | `0x40` | `usbDpiIndex:u8`, `gDpiIndex:u8`, `reserved:u8`, `dpi0..5:u16`, `sum:u8` |
| `0x11` | `0x41` | `usbRate:u8`, `freeRate:u8` |
| `0x11` | `0x42` | `lod:u8`, `ripple:u8`, `line:u8`, `motionSync:u8`, `_:u8`, `_:u8`, `gameMode:u8`, `rotateOpen:u8`, `rotateVal:u8` |
| `0x11` | `0x43` | `time:u8` |
| `0x11` | `0x58` | `profileIndex:u8` |
| `0x12` | `0x40` | `0x11 0x40` plus `diff:u8` and `dpiVal0..5:u16` (per-axis DPI) |
| `0x12` | `0x52` | `buttonIndex:u8`, `_:u8`, `buttonType:u8`, `buttonValue:u24` |
| `0x12` | `0x55` | macro chunk: `buttonIndex`, `moreData`, `offset:u16`, `length` |
| `0x12` | `0x57` | the whole config, same layout as read `0x12 0x67` |
| `0x12` | `0x2D` | RGB gradient |

DPI is written as the **raw value** in a `u16`, little-endian. There is no
`dpi/50 - 1` encoding here; that belongs to the other MCHOSE protocol family
(templated frames with a sum checksum, used by the devices with a screen).

## Report rate index

`usbRate` / `freeRate` index into the model's own list. For an 8K model:

```
0 -> 125 Hz   1 -> 500 Hz   2 -> 1000 Hz
3 -> 2000 Hz  4 -> 4000 Hz  5 -> 8000 Hz
```

A 1K-only link stops at index 2. The L7 Pro is 8K on the dongle and wired, 1K
on Bluetooth.

## LOD

`lod` in `0x11 0x42` is an index into the model's list. L7 Pro: `0 -> 1 mm`,
`1 -> 2 mm`. The 42000 DPI models start at 0.7 mm instead.

## Model table

From the same bundle, per model: the DPI ceiling, the LOD list, and the rate
list per link. L7 Pro: `pids ["176", "4129", "4128"]`, `dpiMax 26000`,
`lod [1mm, 2mm]`, rate `[8K, 1K, 8K]`.

## What the hardware taught us

Six things the bundle does not say, found by driving the real mouse.

**Game mode has its own command.** `0x11 0x02 <mode>` is what moves the `game`
byte in `0x11 0x03`. The `gameMode` field inside `0x11 0x42` is something else
and leaves that byte alone, so do not use it.

**`0x11 0x42` is not write-only after all.** Its ripple, angle-snap and motion
sync arguments land in the config block's `sensor` byte (body offset 17) as
bits 2, 3 and 4. Bit 0 belongs to the mouse and is always set. So the flags can
be read back, even though there is no read command for them.

**They do not clear the same way they set.** Sending `0x11 0x42` with zeroes
leaves the bits standing, and the mouse rejects a config-block write that
clears `sensor` outright: it answers with the byte it had. Turning one of them
off needs more than the obvious inverse, which is why this tool still keeps the
last written set in its own state file.

**Wired is body[1], wireless is body[2].** The vendor's read parser and write
schema disagree; setting the rate over the dongle moved only body[2], which
settles it.

**Two bytes of the config block belong to the mouse.** Offset 3, the one the
vendor schema calls `reserved`, flips between `0x00` and `0x02` on its own
whenever the active DPI stage changes, and bit 0 of `sensor` (offset 17) is
always set. Comparing a read-back byte for byte against what was written
therefore reports a failure on writes that landed perfectly.

**A config write needs time to commit.** Reading the block straight back after
`0x12 0x57` returns the old contents often enough to look like a failed write.
Poll for a few hundred milliseconds instead.

**The mouse can be talked at too fast.** Applying a preset first took fifteen
exchanges inside a second and choked the config channel every time. A 25 ms
floor between transactions, process-wide, makes it reliable; without it no
amount of retrying helps.

**The long-report channel can wedge.** `0x12 0x67` starts answering 63 zero
bytes while `0x11 0x03` and `0x11 0x06` keep working normally, so it reads like
a wiped mouse when nothing was lost. Re-selecting the profile with
`0x11 0x58` brings it straight back.

## G7 family

The MCHOSE G7 shares nothing with the protocol above except the vendor usage
page number. USB `a8a4:2255` is the mouse on its cable and `a8a5:2255` is its
2.4 GHz receiver. Both were read and written on 2026-10-15; the mouse reported
firmware 2.2.7 on both links (the receiver's own build is from Dec 2024, the
mouse's from Nov 2025).

The M HUB bundle only lists the G7. The protocol is in the vendor's separate
G series web app at `mchose.com.cn/gmouse/` (see `protocol/README.md`), and was
confirmed against the hardware, including the offsets below.

### Interface

The cable shows three USB interfaces and the receiver four. The protocol is on
the one whose report descriptor starts `06 01 FF 09 10`: usage page `0xFF01`,
usage `0x10`, one 64-byte input and one 64-byte output report, **no report ID,
no feature reports**. The other interfaces are the mouse, the keyboard
interface of the receiver and vendor collections M HUB does not use. Match the
descriptor, never the `hidrawN` number.

### Transport

Write a 64-byte output report, read a 64-byte input report. On Linux a hidraw
`write()` of a device without report IDs takes a leading zero byte in front of
the 64 bytes and drops it before the report goes out. Replies arrive within a
fraction of a second; the vendor app waits 500 ms and this tool one second.

A request starts with `0x55`, its reply with `0xAA`, and byte 1 is the command
in both, so a reply is matched on `AA <command>`. The mouse also sends reports
nobody asked for, for example `AA FA ...` after a report rate change; skip
anything that does not match.

All offsets below are in the 64-byte frame, counted from zero.

### Commands

| Request | Meaning |
|---|---|
| `55 03` | firmware version: ASCII digits at reply bytes 23, 24 and 25, `2 2 7` is 2.2.7 |
| `55 30 A5 0B 2E 01 01 01 00 00` | battery: byte 8 is the percent, byte 9 is non-zero while charging |
| `55 0E A5 0B 30 01 01 01 00 00` | read the settings block |
| `55 0F AE 0A 30 01 01 01 00 00 ...` | write the settings block, bytes 10 and on as in the read reply |
| `55 08 A5 0B 20 00 ...` / `55 09 A5 22 20 ...` | read and write the five buttons, not used here |
| `06 0C ...` | macros, not used here |
| `55 ED` | the vendor app asks this to see whether the receiver has a mouse; both links answered all zeros, so its meaning is not known |

Bytes 5 to 9 of the read and write headers are the vendor app's constants,
copied as they are.

### The settings block

| Byte | Field |
|---|---|
| 10 | report rate, index plus one: `1` 125 Hz, `2` 250, `3` 500, `4` 1000 |
| 11 | how many DPI stages are enabled |
| 12 | active stage, index plus one |
| 13..24 | six DPI values, `u16` little-endian, raw DPI |
| 48 | scroll direction, `0` forward and `1` reverse |
| 49 | lift-off distance, `1` or `2` (mm) |
| 50 | sensor flags, bit 0 is "line correction", that is angle snapping |
| 51 | key debounce, ms (the vendor app offers 0 to 20) |
| 52 | sleep, minutes, `0` never (the app offers 0 to 100) |
| 53 | highspeed mode, bit 0 |
| 54 | not written by the vendor app, the cable reports `64` here |
| 55 | sensor angle, signed byte |

The vendor app offers 200 to 12000 DPI. Bytes 25 to 47 and 56 to 63 are zero on
both links.

### What the hardware taught us

**The enabled stages are the first N.** Byte 11 is a count, not a mask. With a
count of 3 the stages 0 to 2 are enabled and the three after them keep their
values without being used, so changing DPI values must not change the count.
Setting the active stage to a stage that is not enabled makes no sense.

**The cable and the receiver keep separate copies of the settings.** A write
to one link leaves the other alone: after the cable was set to three enabled
stages with new DPI values, the receiver still had one stage and its old
values, and writes to the receiver never moved the cable. The mouse does copy
settings between them at some point, though. Later the receiver's stages 1 and 2
read `1000` and `2000` like the cable's instead of `1600` and `2400`, while its
count stayed 1. When that happens is not known, so never assume the two links
agree: read the link you are about to write to, and back them up separately.

**The cable reports a few bytes the receiver does not.** The cable returns
`FF` at byte 49 (lift-off) and byte 53, and `64` at byte 54; the receiver
returns `01`, `00` and `00`. So build a write from the reply you just read and
change only the field you mean to, never from zeros.

**The reply checksum is the mouse's job.** Byte 3 of a reply is the sum of
bytes 4 to 53 modulo 256 for the settings block, and bytes 4 to 9 for the
battery. A write goes out with the vendor's constant at byte 3 and the mouse
accepts it; on the next read it has recomputed the byte. The reply to a write
is an echo of the frame, which says nothing about whether the settings were
kept, so read them back.

**A report rate change makes the mouse drop the next request.** The mouse
announces the change with an `AA FA ...` report, and the read sent right behind
the write got no answer. Waiting 150 ms after the write was enough in testing, and the
next read after that is answered normally.

**Writing the same settings back changes nothing.** Verified on both links.
Writing one field changes that byte and byte 3 only. DPI, report rate, scroll
direction, angle snapping, debounce and sleep were changed and changed back on
both links, lift-off on the receiver, and the active stage on the cable.

### Not verified

When the mouse copies its settings from the cable to the receiver. Lift-off on
the cable, where byte 49 is `FF` and there is nothing sensible to return to. The active stage on the receiver, which has one stage enabled. What
`55 ED` answers with the mouse off. Bytes 53, 54 and 55, which this
tool never changes. The ripple (bit 4) and
motion sync (bit 5) flags that one of the vendor app's pages offers in byte 50.
