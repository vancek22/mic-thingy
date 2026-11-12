# Virtual microphone driver (Windows) - status and design

**Status: not built.** The kernel driver itself needs the Windows Driver Kit, MSVC and a virtual
machine in test-signing mode (a bug in a kernel driver is a blue screen). None of that exists on
the development machine, so no driver code has been written or run. What exists is everything that
can be built and tested without it:

| Piece | Where | State |
|---|---|---|
| Shared-memory ring protocol (header, writer, reference consumer) | `src/audio/vmic_shm.[ch]` | done, tested (`tests/test_app.c`: order across wraps, silence on underrun, drop and count on overrun) |
| VB-Cable path | `src/app/session.c` | unchanged, the default and fully supported |
| Kernel driver (PortCls / WaveRT miniport, one capture endpoint) | `driver/windows/` (does not exist yet) | not started |
| `vmic_windows.c` backend (map the section, write frames, drift control) | `src/audio/` | not started |
| Installer / uninstaller | - | not started |

## Decision (2026-11-12)

Test-signing only. No EV certificate is bought, so Windows 11 loads the driver only with
`bcdedit /set testsigning on` (watermark on the desktop; some anti-cheat software refuses to run).
The driver is therefore an opt-in for people who accept that; VB-Cable stays the default.

## Design

* Driver: start from Microsoft's MIT-licensed *Simple Audio Sample* (SYSVAD), strip everything
  except one capture-only WaveRT endpoint named "mic thingy microphone" (48 kHz mono/stereo,
  16-bit and float). Record the origin in `THIRD_PARTY_NOTICES.md`.
* Data path: the driver creates a section holding a `VfVmicShared` header + ring (see
  `vmic_shm.h`); the application maps it through a custom IOCTL on the device interface. The
  application is the only writer of `write_pos`, the driver's timer DPC the only writer of
  `read_pos`. No locks on either side. Empty ring = silence + underrun counter, full ring = the
  application drops a block + overrun counter.
* Clock drift: the application keeps the ring level near a target with the same drop/repeat-one-
  sample logic as `VfSink`; the driver exposes its position counter.
* Safety: the IOCTL accepts one client at a time, validates every size, maps the section for
  data only, and never dereferences user pointers outside `__try` / probe-and-lock.
* Expected gain: the cable device, its buffer and one WASAPI buffer pair disappear from the
  path. The measured VB-Cable + Windows part of the loop is ~50-55 ms (CLAUDE.md); how much of it
  a driver removes has to be measured with `vf-latency` once it exists.

## Stages (from docs/PLAN_PERFORMANCE.md, section 7)

* D0 environment: Windows 11 VM, test signing, WDK, WinDbg, Driver Verifier; build and install the
  unmodified sample.
* D1 minimal capture endpoint that outputs silence / a test tone.
* D2 data channel: section + IOCTL + `tools/vf_drvtest.c`; `vf-record` + `vf-eval match` must show
  r >= 0.9999 and zero dropouts for 10 minutes.
* D3 `vmic_windows.c` backend, automatic choice driver / VB-Cable, latency comparison.
* D4 robustness: sleep/resume, format changes, two readers, killing the application, 20 install /
  uninstall cycles, Driver Verifier throughout.
* D5 installer with an explicit consent dialog and uninstall; test-signing instructions only.
