# DE1-SoC CPulator: DAC/ADC Source Switch (IRQ-based)

## Why the display never followed the generator

CPulator's input side (ADC) is an independent generated **250 Hz sawtooth** for the **8 kHz** Audio device. DAC sound is a separate stream. The previous code correctly analyzed ADC data, so changing the DDS generator did not change the scope or FFT.

## Revised behavior

- **Default: `DAC PCM`**. The Audio IRQ stores the signed `int16_t` sample actually submitted to **both DAC FIFO channels** in `g_dac_ring[]`. The VGA scope and 64-point Q2.14-coefficient FFT analyze this ring. The graph tracks DDS frequency/amplitude changes.
- **Click `[SOURCE]` at bottom-right** to switch to **`ADC Input`**. The Audio IRQ continues collecting actual Audio Core input into `g_adc_ring[]`. In CPulator, this shows its independent 250 Hz sawtooth and harmonics.
- Click `[SOURCE]` again to go back to `DAC PCM`.

Both streams are captured **inside the Audio IRQ**, without moving audio FIFO servicing back into the superloop. Timer IRQ 0 still increments `g_jiffies`; Audio IRQ 6 handles input/output FIFO traffic. FFT/VGA rendering still runs in the main loop.

## Signal path

```
DDS generator -> DAC FIFO -> speaker
        | 
        +-> g_dac_ring (digital signal monitor) -> VGA/FFT [default]

CPulator ADC (250 Hz sawtooth) -> ADC FIFO -> g_adc_ring -> VGA/FFT [alternate]
```

**Important:** `DAC PCM` is a **digital monitor of samples submitted to the DAC FIFO**, not analog capture of WM8731 Line-out. To measure real analog DAC output on the hardware board, physically connect Line-out to Line-in with appropriate levels, then choose `ADC Input`. The simulator does not implement that physical feedback automatically.

## Build/run

Use the same Nios II project / ELF loading procedure that already worked for `de1soc_cpulator_irq_adc_fft.c`. Replace its source with `de1soc_cpulator_irq_dual_source.c`. `CODEC_CONFIG_MMIO` remains **0** for CPulator because the Device List does not expose the Configuration Core.

The source has passed host GCC C syntax checking, **not** Nios II cross-compilation or live CPulator execution in this environment.

## Expected indicators

- `DAC PCM`, 500 Hz: yellow sine-like waveform; FFT peak close to **0.50 kHz**.
- Increase amplitude: yellow waveform grows in height; green peak bar grows.
- `ADC Input` at any DDS setting: independent sawtooth, FFT peak **0.25 kHz** and harmonics.
- FFT is **64 points, 8 kHz**, so bin spacing is **125 Hz**. All GUI frequency steps are multiples of 250 Hz, and align with bins in this test.

This preserves both ADC data acquisition (from the previous request) and real-time DDS visualization.
