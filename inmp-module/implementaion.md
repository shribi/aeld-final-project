# INMP441 Line Discipline + ALSA Capture Driver: Plan

## Goal
Expose an INMP441 microphone, connected through an ESP32 and a UART-USB bridge, as a standard ALSA capture card so that `arecord`, PipeWire and other tools can use it like normal hardware.

## Architecture

```
INMP441 --I2S--> ESP32 --Serial2 @ 921600--> UART-USB bridge --> /dev/ttyUSB0
                                                                      |
                                                          kernel line discipline
                                                          (frame parser)
                                                                      |
                                                          ALSA PCM capture device
                                                                      |
                                                          arecord / PipeWire
```

| Layer | Responsibility |
|---|---|
| Existing USB-serial driver | Moves bytes from the hardware to the tty |
| Line discipline (new) | Decodes frames: magic, seq, len, CRC-16, resync, zero-fill for lost packets |
| ALSA PCM (new) | Presents the stream as a sound card: format, rate, periods, xruns |

## Components

1. **Frame parser**
   - Kernel-independent C file (pure function: bytes in, PCM out)
   - Handles magic, sequence, length validation, CRC-16, resync by scanning for magic
   - Zero-fills gaps from lost packets, with a cap on gap size
   - Reused unchanged in a userspace test harness

2. **Line discipline module**
   - Registers a `tty_ldisc_ops` (initially `N_DEVELOPMENT`)
   - `open()` creates the ALSA card; `close()`/`hangup()` tears it down safely
   - `receive_buf()` feeds bytes to the parser

3. **ALSA capture device**
   - One card, one PCM capture stream, mono
   - `snd_pcm_hardware` advertises a single fixed format and rate
   - `trigger()` starts/stops delivery; `pointer()` reports the buffer position
   - `snd_pcm_period_elapsed()` called from the receive path; xruns reported, not hidden

4. **Userspace attach**
   - `ldattach` sets the baud rate and attaches the discipline to the port, so no in-kernel termios code or custom ioctls are needed

5. **Statistics**
   - Counters for lost packets, CRC errors, resyncs and overruns, exposed via sysfs or debugfs

6. **Packaging**
   - Out-of-tree module in a Buildroot package, loaded at boot
   - Init script or udev rule runs `ldattach` on the serial port

## Key Decisions

- **Kernel work ends in a real subsystem (ALSA)**, so the driver does something plain userspace cannot: it registers a standard sound card.
- **No private character device, kfifo, kthread, `write()` or port/baud ioctls.** ALSA owns buffering and device access; the tty layer pushes data to the line discipline.
- **Baud and raw mode are configured from userspace** (`ldattach`/`stty`), avoiding version-dependent kernel termios code.
- **Parser stays separate from kernel code** so it can be unit-tested in userspace.
- **Single fixed audio format.** Sample rate and width are chosen so the stream fits in the link: 921600 baud 8N1 is about 92 KB/s, so 16 kHz 16-bit mono (32 KB/s) fits comfortably, while 48 kHz 16-bit does not.
- **Version portability:** targets are the host 7.0 kernel and Buildroot `linux-custom` 6.1. API differences (ldisc callbacks, `class_create`) are isolated behind `LINUX_VERSION_CODE` guards.
- **Alternative considered:** a userspace daemon feeding `snd-aloop` or a PipeWire source. Rejected for this project because the goal is a driver with normal hardware behavior and kernel/ALSA experience.

## Risks

| Risk | Mitigation |
|---|---|
| `receive_buf` runs in a non-sleeping context | Spinlocks shared with `pointer()` and `trigger()`; no sleeping calls |
| Ldisc callback signatures differ across kernel versions | Single compatibility wrapper |
| Hangup or unplug during capture | Safe teardown with `snd_card_free_when_closed()` |
| Clock drift between ESP32 and host | Zero-fill on loss; report xruns |
| Corrupted `seq` or `len` | Validate before use; cap zero-fill size |
| Baud rate not supported by the bridge chip | Verify bridge supports 921600 reliably |

## Verification

- Parser unit tests in userspace using captured and synthetic frames
- Fake framed data over a pty (`socat`) attached with `ldattach` before using hardware
- `arecord -l` shows the card; `arecord` produces a valid WAV
- Lost-packet and CRC counters stay low with the real ESP32
- Hot-unplug during capture causes no oops and no hang
- Builds and loads on both the host kernel and the Buildroot 6.1 kernel