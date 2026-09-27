# Third-party notices

EasyInput Maker contains project-owned material and may use third-party components. The root `LICENSE` applies only to project-owned material that does not carry a different license notice. It does not replace, narrow, or override third-party licenses.

For project-owned material, the copyright and licensing entity is 深圳物启万相人工智能有限公司, the original author is CY-CHENYUE, and EasyInput Maker is a WaytoAGI community project. The `Required Notice:` lines in the root `LICENSE` must be preserved with redistributed copies.

## Initial firmware dependency inventory

| Component | Intended form | Version / basis | License | Required handling |
| --- | --- | --- | --- | --- |
| ESP-IDF | Build framework, not vendored as a complete copy | 5.5.5 | Apache-2.0 with separately licensed bundled components | Use the official distribution and preserve notices required by the components actually redistributed |
| ESP HID safety adapter | Modified source distributed in `components/esp_hid/` | Derived from ESP-IDF 5.5.5 | Apache-2.0 | Keep SPDX and copyright headers, the component `LICENSE`, upstream provenance, and prominent modification notices |
| `espressif/esp_tinyusb` | Managed build dependency | 1.7.6~2 | Apache-2.0 | Keep the dependency locked and include Apache-2.0 notices when redistribution requires them |
| `espressif/tinyusb` | Managed transitive dependency | 0.21.0~1 | MIT | Preserve the TinyUSB copyright and MIT permission notice |
| `espressif/esp_audio_codec` | Optional managed dependency for the Ogg/Opus diagnostic build | 2.5.0 | Espressif Modified MIT License (`LicenseRef-Espressif-Modified-MIT`) | Use only with Espressif Systems products and preserve its copyright and license notice |

## README visual asset inventory

| Asset | Intended form | Version / basis | License | Required handling |
| --- | --- | --- | --- | --- |
| WeChat official account QR image | README image at `assets/readme/wechat-qr.jpg` | Copied unmodified from `CY-CHENYUE/project-flow-cy` commit `7d3ad181f65e034b7b45cff916f15cfd8fc7db74`; SHA-256 `9daa1588d879a2572aefd7dcd7327208b5bb8110f2a7f6e9a9e6e7b1615d16e3` | GPL-3.0-or-later | Preserve the CY-CHENYUE copyright, source provenance, this notice, and the GPL-3.0-or-later license text |

`project-flow-cy` QR asset notice: Copyright (c) 2026 CY-CHENYUE. This asset is licensed under GPL-3.0-or-later. When copying, modifying, or distributing the asset or derivative versions, keep this notice and the license statement.

The dependency inventory must be updated whenever a component, version, copied source file, generated asset, font, image, audio file, model, or other redistributable material is added or removed.

## License texts

- `LICENSES/Apache-2.0.txt` — Apache License 2.0.
- `LICENSES/GPL-3.0-or-later.txt` — license for the separately licensed README QR image.
- `LICENSES/MIT-TinyUSB.txt` — TinyUSB MIT license and copyright notice.
- `LICENSES/LicenseRef-Espressif-Modified-MIT.txt` — license for the optional Espressif audio codec component.

## Project assets

The checked-in `waytoagi.eiad` factory prompt and the independent EIAD/Ogg diagnostic fixtures are project-owned audio assets. They are distributed under the root PolyForm Noncommercial license with the required WaytoAGI, CY-CHENYUE and 深圳物启万相人工智能有限公司 notices. Their source fingerprints, conversion metadata and encoded fingerprints are recorded in the corresponding `assets/README.md` files.

Xiph libopus and libopusenc were used only to produce the fixed diagnostic Ogg file; they are not vendored or linked into the default firmware. The optional ESP-IDF Ogg/Opus diagnostic uses the separately licensed Espressif audio codec component listed above.

Audio, images, fonts, models, diagrams, and other non-code assets may be published only when their ownership and redistribution terms are documented. An asset with unknown or undocumented provenance is excluded from release.

## Source references

- PolyForm Noncommercial 1.0.0: <https://polyformproject.org/licenses/noncommercial/1.0.0>
- ESP Component Registry, `esp_tinyusb` 1.7.6~2: <https://components.espressif.com/components/espressif/esp_tinyusb/versions/1.7.6~2>
- ESP Component Registry, `tinyusb` 0.21.0~1: <https://components.espressif.com/components/espressif/tinyusb/versions/0.21.0~1>
- ESP Component Registry, `esp_audio_codec` 2.5.0: <https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.5.0>
- `project-flow-cy` QR image source: <https://github.com/CY-CHENYUE/project-flow-cy/blob/7d3ad181f65e034b7b45cff916f15cfd8fc7db74/assets/wechat-qr.jpg>
