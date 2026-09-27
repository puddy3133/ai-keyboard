# Ogg/Opus diagnostic fixture

`easyinput_boot_probe.ogg` is a deterministic, diagnostic-only fixture derived
from the project-owned `dingdongji_ding.wav` test sound. It is separate from
the WaytoAGI factory boot sound and is compiled only when both speaker
diagnostic flags are enabled.

- normalized source: 48,000 Hz mono PCM16, 13,369 logical samples;
- encoder: Xiph `libopus 1.6.1` with `libopusenc 0.3`;
- container: Ogg Opus, mapping family 0, 15 audio packets;
- file size: 1,734 bytes;
- SHA-256: `d49de654f1b72f05a835299801950df202688f7adf70bbfac11d2dbb48941411`.

The checked-in file lets the optional decoder diagnostic build without a local
encoder dependency.
