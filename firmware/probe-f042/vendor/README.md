# vendor/ — required CMSIS / startup / linker (not committed)

These come from ST's **STM32Cube_FW_F0** package and carry ST's license, so they
are git-ignored (see `../.gitignore`). They also cannot be auto-fetched —
st.com blocks non-browser clients. Add them by hand, then record the exact
version + checksums below so a full firmware build is reproducible.

## Files to place here
| Path under `vendor/` | From STM32Cube_FW_F0 | Purpose |
|---|---|---|
| `CMSIS/Include/` | `Drivers/CMSIS/Include/` | Cortex-M0 core headers |
| `CMSIS/Device/ST/STM32F0xx/Include/` | `Drivers/CMSIS/Device/ST/STM32F0xx/Include/` | `stm32f042x6.h` + family |
| `system_stm32f0xx.c` | `Drivers/CMSIS/Device/ST/STM32F0xx/Source/Templates/` | SystemInit |
| `startup_stm32f042x6.s` | `.../Source/Templates/gcc/` | vector table + reset |
| `STM32F042K6Tx_FLASH.ld` | a CubeIDE/CubeMX F042K6 project | 32 KB flash / 6 KB RAM map |

The linker script must reflect the datasheet memory map: **FLASH 32 KB @ 0x08000000,
RAM 6 KB @ 0x20000000** (stm32f042k6.pdf, §5 Memory mapping).

## Provenance (as currently staged — 2026-06-23)
GitHub's "Download ZIP" of STM32CubeF0 does **not** include git submodules, so the
CMSIS **device** headers (a `cmsis_device_f0` submodule) came through empty. They
were fetched separately from ST's official repo. Sources:

| File(s) | Source |
|---|---|
| `CMSIS/Include/*` (core), `startup_stm32f042x6.s`, `STM32F042K6Tx_FLASH.ld`, `system_stm32f0xx.c` | `STM32CubeF0-master.zip` (GitHub STMicroelectronics/STM32CubeF0); startup/linker from `Projects/STM32F042K6-Nucleo/.../SW4STM32/` |
| `CMSIS/Device/ST/STM32F0xx/Include/{stm32f0xx,stm32f042x6,system_stm32f0xx}.h` | `raw.githubusercontent.com/STMicroelectronics/cmsis_device_f0/master/Include/` |

```
sha256  stm32f042x6.h        : 764bbce122e8a62c0f31a798695a0f59f5fe1a98d7f2dc105d5e9444c9deb1ad
sha256  startup_stm32f042x6.s: 48814bbef78fc1897c6fb028a2c43cc13708eac4dd5382ea27f722ca0f74b993
sha256  STM32F042K6Tx_FLASH.ld: 6c0ac957839ad2b0e25c34190de184a728ed9f00461813fa2f98b5a53116b305
sha256  system_stm32f0xx.c   : 5f8362a08ec0d73df48fa51430fc8c12e817dd177990f2c37a57b1425a7f4221
```
Regenerate with: `sha256sum vendor/CMSIS/Device/ST/STM32F0xx/Include/stm32f042x6.h vendor/startup_stm32f042x6.s vendor/STM32F042K6Tx_FLASH.ld vendor/system_stm32f0xx.c`

Note: the device headers are from `cmsis_device_f0` master; if you need an exact
version match to the CubeF0 release, init the submodule instead.

## Local tweak
`STM32F042K6Tx_FLASH.ld` has `_Min_Heap_Size` set to `0x0` (was `0x200`): this
firmware uses no `malloc`, so reserving heap just wastes ~512 B of the 6 KB RAM.
Re-apply this if you replace the linker script from a fresh Cube export.
