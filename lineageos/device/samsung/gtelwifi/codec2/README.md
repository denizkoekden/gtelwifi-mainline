# SC7730 VSP Codec2 bridge

Experimental Android 17 SimpleC2Component and AIDL ComponentStore. The native
VSP probe and in-process Codec2 client have validated the implementation,
byte-exact High/B-frame output, EOS, flush, restart and resolution changes.
The decoder is registered system-wide and selected automatically by MediaCodec
and a real Media3/ExoPlayer 1.9.1 client. The measured 720p comparison has no
dropped frames and substantially lower total CPU cost than software decoding;
1080p AVC presentation still drops frames. Jellyfin 720p DirectPlay now survives
an eight-minute run and forward/backward seeks with a CABAC-padding parser fix;
its physical sight check and renderer drop count remain open. VP8 native contracts
pass at 720p/1080p, including hidden frames, crop, resize, EOS, flush/restart and
cross-codec session exclusion. VP8 720p playback and seek pass with no renderer drops. MPEG4 Simple Profile
through 720p and H263 Baseline through 704x576 also pass real Media3 playback.
MPEG4 ASP/B pictures and H263+ remain unsupported. The new bundle still needs
a separately authorized persistent installation. See `docs/handoff-vsp-codec2-protokoll.md` for evidence.

`TARGET_GTELWIFI_VSP=true` installs the service and vendor codec XML. Default
is false, preserving software codecs. The private AVC/VP8/MPEG4/H263 libraries are supplied by
`tools/vsp/extract-vsp.sh` and the existing private build-host staging process;
no implementation or downloaded vendor header belongs in this directory.
The ABI declarations apply only to the hash checked by that script.

Build targets: `libcodec2_sprd_avcdec`, `c2-sprd-avc-test`,
`libcodec2_sprd_vp8dec`, `c2-sprd-vp8-test`,
`libcodec2_sprd_m4vdec`, `c2-sprd-mpeg4-test`, `c2-sprd-h263-test`,
`android.hardware.media.c2-sprd-service`. For an in-process test put the test
executable, component DSO and privately supplied AVC DSO in one temporary
Android directory. Use matching platform Codec2 dependencies, not a mixture
of newly rebuilt framework libraries and arbitrary older libraries.

The test consumes the length-framed Annex-B AU format used by `tools/vsp`:

```
c2-sprd-avc-test input.au 120 [output.nv12]
```

It checks returned work, frame count, EOS, duplicate output and 64-bit timestamp
identity. An optional dump is in display order for comparison with ffmpeg.
`VSP_C2_ROUNDS=3` repeats with flush and stop/start, removing repeated SPS/PPS
so the AVC component must replay cached CSD. VP8 instead replays the real keyframe.
`VSP_C2_DATA_EOS=1` puts EOS on the
last data work instead of adding a separate empty work. No audio is involved.

The driver exposes privileged raw DMA programming, so only one service decoder
is permitted across all four codecs, and the service requires SYS_RAWIO. A common
DSO owns the process session lease; failed component destruction cannot release
another codec's lease. The node is mediacodec:media
0660 when the optional service is installed. This bring-up build is SELinux
permissive; an enforcing policy/sandbox audit remains necessary before treating
it as a production service. Applications never receive the device fd or DMA
addresses. The component checks SPS geometry before any slice submission and
accepts only progressive 8-bit 4:2:0 Baseline/Main/High through level 4.1.
VP8 accepts profile 0 with even dimensions up to 1920x1080 and retains hidden
reference pictures without inventing presentation timestamps. MPEG4/H263 headers are checked before DMA; untested advanced tools are rejected.
All codecs require
tightly packed contiguous NV12 from the minigbm allocator.

DPB references and pending display output retain C2 graphic blocks and their
imports. UnwrapNativeCodec2GrallocHandle returns a non-owning wrapper: delete
it without closing its fds. Import retirement happens after the legacy call
returns and releases the engine. CPU pixel copies are avoided. Idle open
legacy fds permit runtime and system suspend with the matching VSP kernel.

The AVC/VP8 XML maximum is 1920x1088 for the coded 16-line blocks of a
1920x1080 picture. A limit of 1080 in the XML incorrectly made Android report
a maximum of 1072 visible lines. The bitstream guards still bound visible
height to 1080; coded allocation and native capability use 1088.
