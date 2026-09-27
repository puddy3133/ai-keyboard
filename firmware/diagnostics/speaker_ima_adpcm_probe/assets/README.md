# EasyInput EIAD startup probe

`easyinput_boot_probe_eiad.h` is a self-contained, project-owned diagnostic
asset. It is separate from the WaytoAGI factory boot sound. Firmware builds do
not need an audio encoder or the original WAV at build time.

Frozen provenance:

- source asset: project-owned `dingdongji_ding.wav` diagnostic sound;
- source SHA-256: `a7ad2114cfe07718c7de3e881fdcc4f20a981d36828892868195e2dec94842c4`;
- source format: PCM16-LE, stereo, 32,000 Hz, 8,913 frames;
- result: EIAD v1, 48 kHz mono, 13,369 samples, 6,872 bytes;
- encoded SHA-256: `c483ead293fba5321e5b22e7cfff699e56b3db8d7c157ec4b23eba8b8bd2de42`;
- decoded PCM SHA-256: `7f13f1e2b55daaef6b189b09c40576dbb17c809726c7f1661782399fe9491729`.

The deterministic downmix, resample and IMA-ADPCM output are frozen by
`host_test/ima_adpcm_decoder_tests.cpp`.
