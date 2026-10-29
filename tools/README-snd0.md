# The hover music (sce_sys/snd0.at9)

Made with BlackBearReloaded's PS5 AT9 Converter
(https://github.com/manoma080/ps5-at9-converter, GPL-3.0, Python + numpy), the
encoder PSGFX uses on real PS5s:

    python -I ps5_at9.py -i CrackdownTheme.mp3 -o snd0.at9 --duration 60 \
        --fade-in 0.5 --fade-out 4

It writes the file the way Sony's `at9tool -e -br 192 -wholeloop` does: 192 kb/s,
48 kHz stereo, fmt/fact/smpl/data, a whole-track loop, -28 LUFS (param.json's
`loudnessSnd0`), under the 2 MiB limit.

`tools/at9enc.py` (from the Max Payne 3 port) is not to be used for it: the PS5
played its files garbled, sped up and cut short. Sony's decoder rejects a
superframe whose unused tail bytes aren't 0x01 (at9enc wrote zeros), wants the
gradient's start unit at least 1 (at9enc wrote 0) and precisions at or under 15.
FFmpeg and LibAtrac9 check none of that, which is why they decoded it fine. Above
192 kb/s the PS5 plays nothing.
