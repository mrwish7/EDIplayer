# EDIplayer

A desktop player for a full DAB multiplex received as **EDI** (ETSI TS 102 693) over UDP or TCP. It reads the service list from the FIC, plays any DAB+ or DAB (MP2) service, and shows that service's **Dynamic Label** (with **DL Plus** tags) and **MOT Slideshow**.

Runs on Windows and Linux (Dear ImGui + GLFW + OpenGL 3, miniaudio for sound).

## Using it

1. Enter the EDI source in the top bar and press play:

   | Source | Meaning |
   |--------|---------|
   | `tcp://host:port` | connect to an EDI TCP server |
   | `udp://port` | receive unicast UDP on that port |
   | `udp://239.1.2.3:5004` | join a multicast group |
   | `udp://239.1.2.3:5004?iface=192.168.1.10` | join on a specific interface |

   Plain AF packets and PFT-fragmented (PF) packets are both accepted. The arrow next to the address opens the recent sources.
2. The services appear on the left once the FIC has been received. Click one to play it.
3. On the right are the slideshow (320×240), the dynamic label and any DL Plus items (title, artist, ...). Hover the slide for its name and click-through URL.

The source, volume, last service and recent sources are saved (`EDIplayer.ini` next to the exe on Windows, `~/.config/ediplayer/settings.ini` on Linux). On the next start the last service resumes automatically. **Log** shows what the receiver and decoders report.

## Building

Everything is vendored in `third_party/`, so no downloads are needed. You need CMake ≥ 3.16 and a C++20 compiler.

**Windows** (Visual Studio 2019 or later):

```
cmake -S . -B build-win -G "Visual Studio 16 2019" -A x64
cmake --build build-win --config Release
```

**Linux**: install the X11/OpenGL headers (`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev` on Debian/Ubuntu). ALSA/PulseAudio are loaded at run time, so no audio dev packages are needed.

```
cmake -S . -B build-linux
cmake --build build-linux -j
```

This builds `EDIplayer` and `edi-probe`.

### edi-probe

A command line test tool for the same engine, useful without a display or sound card:

```
edi-probe tcp://host:port                         # list the services
edi-probe tcp://host:port --sid D3A3 --seconds 30 --wav out.wav --slides dir
```

## How it works

```
EDI (UDP/TCP) ─► PFT reassembly ─► AF / TAG parsing (deti, est)
                                      │ FIC              │ MST of the selected sub-channel
                                      ▼                  ▼
                           service list (FIG 0/2,   DAB+: superframe sync ─► AUs ─► libfaad (AAC 960, SBR, PS)
                           1/0, 1/1)                DAB:  MP2 frames ─────────────► libmad
                                                         │ PAD                       │ PCM
                                                         ▼                           ▼
                                             DLS + DL Plus, MOT slides   jitter buffer ─► miniaudio
```

| Directory | Contents |
|-----------|----------|
| `src/edi/` | EDI/PFT/TAG parsing, FIC, DAB+ superframes, PAD, DLS, MOT, in C (from **edi-meta-forwarder**) |
| `src/decoders/` | libfaad (HE-AAC v2) and libmad (Layer II) ports (from **UecpBridge**, with the UECP and MP3 parts removed) |
| `src/core/` | network source, player engine, decoder wrappers, audio output, log |
| `src/gui/` | the Dear ImGui front end (style from **dab2edi**) |
| `tools/` | `edi-probe` |

Changes to the reused code:

- `audio.c` also hands each AAC access unit (or MP2 frame) to the decoder, not just its PAD.
- `fic.c` records each service's primary component type (DAB/DAB+), keeps labels up to date, and trims label spaces.
- The libfaad port's long IMDCT didn't accept the 1920-point transform used by the 960-sample frames of DAB+; `imdct_long()` now maps it onto its table slot, as `mdct_init()` already did.

**Audio timing.** The sound card's clock and the multiplexer's clock differ slightly. Decoded audio goes through a FIFO that starts at 400 ms. Playback speed is then nudged by up to ±0.2 % (with interpolation) to hold the FIFO near 600 ms. After an underrun it refills, and above 1.5 s it drops back.

## Limitations

- **PFT with Reed-Solomon FEC**: all fragments must arrive; FEC isn't used to recover lost ones.
- **DAB+ Reed-Solomon** isn't applied (EDI is normally error-free). AUs failing their CRC are replaced by silence.
- **MP2 at 24 kHz** (half sample rate) isn't detected, only 48 kHz.
- **Slideshow in MOT directory mode** and packet-mode data services aren't supported. Only audio services are listed.
- Some stations rotate their service label. The list shows the latest label but keeps the position of the first one seen, so rows don't jump.

## Licences

Third-party code: Dear ImGui (MIT), GLFW (zlib), miniaudio (public domain / MIT-0), stb_image (public domain / MIT), FAAD2 (GPL-2.0-or-later), libmad (GPL-2.0-or-later). Because of FAAD2 and libmad, the program as a whole is GPL.
