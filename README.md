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

   Plain AF packets and PFT-fragmented (PF) packets are both accepted.

   **Favourites:** the star next to the address saves it as a favourite, with the ensemble's name as the label (edit it as you like); on a saved address the star is gold, and clicking it lets you rename or remove the favourite. The arrow lists the favourites: click one to switch to it, which also resumes the station you last played there. Hover a favourite and click × to remove it.
2. The services appear on the left once the FIC has been received. Click one to play it. The ensemble header shows the EId and ECC, and the country's flag on the right. The country comes from the ECC plus the first hex digit of the EId (ETSI TS 101 756), and the European area and Australia are covered.
3. On the right are the slideshow (320×240), the dynamic label and any DL Plus items (title, artist, ...). Hover the slide for its name and click-through URL. Right-click it to copy the image to the clipboard. On Linux this needs `xclip` (X11) or `wl-copy` (Wayland).

**Ensemble information.** The ⓘ button under the flag opens a window with everything the FIC says about the multiplex:

- **Capacity units:** a bar of the 864 CUs, with the sub-channels in order of their start address and the free capacity hatched, plus a table giving each sub-channel's CU range, protection, bit rate and services. Hover a segment or a row to highlight it.
- **Ensemble:** labels, EId, ECC, local time offset, date and time (FIG 0/10), transmission mode, CIF count, change and alarm flags, configuration count (FIG 0/7), announcement clusters, the EDI frame counter and timestamp, and how often each FIG has been received.
- **Services:** every programme and data service with its labels, SId, country, programme type, announcements, and each component's type, sub-channel, CU range, protection, language and user applications (Slideshow, SPI, ...).

Hovering a service in the list shows its details, including its own ECC and country. Services from abroad often carry an ECC that differs from the ensemble's (FIG 0/9 extended field).

**Announcements.** If the playing service takes part in announcements (FIG 0/18, e.g. road traffic), a small pill next to its programme type names the types it supports. While an announcement is signalled for it (FIG 0/19), the pill turns green and shows the active type. EDIplayer only shows this; it doesn't switch to the announcement's sub-channel.

### Recording

Below the service name are two record buttons. Each shows its running time while recording, and clicking it again stops it. Both can run at once.

- **raw:** the audio exactly as broadcast. DAB (MP2) services give an `.mp2` file. DAB+ gives an `.aac` file in LOAS/LATM framing, because DAB+ uses 960-sample AAC frames, which ADTS can't signal. The file carries the full AudioSpecificConfig, so faad2-based decoders play it exactly. ffmpeg (and so VLC and mpv) identifies it correctly but, as of ffmpeg 6.1, doesn't implement SBR with 960-sample frames, so HE-AAC recordings play from those as the narrower-bandwidth AAC core only.
- **wav:** the decoded audio (what you hear, before the volume control) as 32-bit float stereo WAV at the service's sample rate. Float keeps the peaks that loud broadcasts decode to just above full scale; 16-bit would clip them.

Files go to `EDIplayer_audio/` next to the program, named `<date>_<time>_<station>`, e.g. `2026-10-26_12-21-39_SWR3.wav`. A recording stops when you change service or stop receiving. The WAV header is kept up to date while recording, so the file is usable even if the program is closed abruptly.

### Station logos (SPI)

If the multiplex carries SPI (Service and Programme Information, ETSI TS 102 818), EDIplayer decodes it in the background for the whole ensemble:

- Each service's **32×32 logo** appears in the square to the left of its name.
- The **320×240 logo** fills the slideshow area until the station sends a slideshow image of its own.
- A small **SPI** line under the ensemble name shows the carousel loading (`SPI 12 / 33`). Carousels are slow, so a full load can take several minutes.

- If the SPI carries **programme information**, a small **EPG** button appears on that line. It opens the programme guide: the stations one above the other and their programmes running across the day, with a line at the current time and what's on now highlighted. Hover a programme for its description; click a station to play it; the arrows change the day. Times are shown in your computer's time zone. Schedules that ended more than a day ago are left out (some broadcasters keep sending old ones).

All received files are saved to `EDIplayer_SPI/<ECC><EId>/` next to the program, e.g. `EDIplayer_SPI/E1CFCB/`. On Linux, if that folder isn't writable, they go under `~/.config/ediplayer/` instead. The folder also holds `logos.txt`, which records which logo belongs to which service, so the next time you connect to that ensemble the logos appear straight away.

The source, volume, last service and favourites are saved (`EDIplayer.ini` next to the exe on Windows, `~/.config/ediplayer/settings.ini` on Linux). On the next start the last service resumes automatically. **Log** shows what the receiver and decoders report.

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

**GitHub Actions** (`.github/workflows/build.yml`) builds both platforms on every push, and the builds can be downloaded from the run's page. Pushing a version tag also publishes them as a release:

```
git tag v1.0.0
git push origin v1.0.0
```

### edi-probe

A command line test tool for the same engine, useful without a display or sound card:

```
edi-probe tcp://host:port                         # list the services
edi-probe tcp://host:port --sid D3A3 --seconds 30 --wav out.wav --slides dir
edi-probe tcp://host:port --sid D3A3 --seconds 30 --record dir    # raw + wav into dir/EDIplayer_audio
edi-probe tcp://host:port --spi 300 --spi-dir dir                 # follow the SPI carousel
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
| `src/core/` | network source, player engine, decoder wrappers, audio output, SPI (`spi.cpp`), log |
| `src/gui/` | the Dear ImGui front end (style from **dab2edi**) |
| `tools/` | `edi-probe`; `gen_icon.py` and `gen_flags.py` regenerate the embedded icon and flags |
| `res/` | app icon (`icon.ico`, PNGs) and the Windows resource file |

Changes to the reused code:

- `audio.c` also hands each AAC access unit (or MP2 frame) to the decoder, not just its PAD.
- `fic.c` records each service's primary component type (DAB/DAB+), keeps labels up to date, and trims label spaces.
- The libfaad port's long IMDCT didn't accept the 1920-point transform used by the 960-sample frames of DAB+; `imdct_long()` now maps it onto its table slot, as `mdct_init()` already did.

**SPI.** These FIGs locate the SPI components:

| FIG | Gives |
|-----|-------|
| 0/13 | which service components carry user application 7 (SPI) |
| 0/8, 0/2 | (SId, SCIdS) → service component ID |
| 0/3 | component ID → sub-channel and packet address |
| 0/9 | the ECC, for the folder name |

From there:

1. **Packet mode:** packets are CRC-checked and reassembled into MSC data groups.
2. **MOT directory mode:** the directory and object bodies are collected into a carousel.
3. **Service Information:** the binary-encoded SI (ETSI TS 102 371, with the tag tables from AbracaDABra) gives each service's `<multimedia>` logos, which are matched to the received images by ContentName.

**Audio timing.** The sound card's clock and the multiplexer's clock differ slightly. Decoded audio goes through a FIFO that starts at 400 ms. Playback speed is then nudged by up to ±0.2 % to hold the FIFO near 600 ms, using a 32-tap Kaiser-windowed sinc interpolator, whose error is about 100 dB below the music. Plain linear interpolation was only about 30 dB below, which was audible as grit on loud, bright material. After an underrun it refills, and above 1.5 s it drops back.

**Headroom.** Loud stations decode to peaks of up to about +4 dB over full scale, in both AAC and MP2. The whole path is float (decoders → FIFO → sound card), so those peaks reach the system mixer intact, where the system volume normally brings them back into range, the same as in other players. Converting to 16 bits in the decoders would hard-clip them instead.

## Limitations

- **PFT with Reed-Solomon FEC**: all fragments must arrive; FEC isn't used to recover lost ones.
- **DAB+ Reed-Solomon** isn't applied (EDI is normally error-free). AUs failing their CRC are replaced by silence.
- **MP2 at 24 kHz** (half sample rate) isn't detected, only 48 kHz.
- **Slideshow in MOT directory mode** and packet-mode data services aren't supported. Only audio services are listed.
- **SPI:** the programme guide shows programmes, not programme groups or the parts of a programme (programme events). The SI file is saved in its binary broadcast form, not as XML. gzip-compressed SI objects and compressed MOT directories aren't decoded. SPI carried in a service's X-PAD isn't decoded, only packet-mode SPI.
- Some stations rotate their service label. The list shows the latest label but keeps the position of the first one seen, so rows don't jump.

## Licences

Third-party code: AbracaDABra's SPI binary tag tables (MIT, Petr Kopecký), country flags from flagcdn.com (public domain, embedded by `tools/gen_flags.py`), Dear ImGui (MIT), GLFW (zlib), miniaudio (public domain / MIT-0), stb_image (public domain / MIT), FAAD2 (GPL-2.0-or-later), libmad (GPL-2.0-or-later). Because of FAAD2 and libmad, the program as a whole is GPL.
