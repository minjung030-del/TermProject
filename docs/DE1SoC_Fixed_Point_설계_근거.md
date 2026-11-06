# DE1-SoC 오디오 스펙트럼 분석 시스템의 Fixed-point 설계 근거

> **대상 구현:** `de1soc_cpulator_irq_dual_source.c` (CPulator / Nios II / Audio IRQ 기반, DAC PCM·ADC Input 선택)  
> **문서 목적:** 고정소수점 비트 배분, 16-bit PCM 사용 근거, FFT 연산 폭, 정규화, 정량 오차 및 Trade-off를 결과보고서에 설명하기 위한 설계 검토 자료  
> **구현 기준:** 2026-11-07 검토본  
> **검증 구분:** CPulator 화면으로 확인한 기능 동작과 Python으로 재현한 수치 실험을 구분해 기술함

## 1. 설계 개요 및 결론

본 프로젝트는 **Nios II 기반 DE1-SoC Computer**의 Audio Core에서 오디오 샘플을 읽거나 DDS(Direct Digital Synthesis)로 생성한 샘플을 DAC FIFO로 출력하고, 선택된 PCM 데이터에 대해 **64-point Radix-2 DIT FFT**를 수행하여 VGA에 파형 및 주파수 스펙트럼을 표시한다.

고정소수점 연산의 설계 목표는 (1) PCM 입력의 해상도 보존, (2) Butterfly 연산 과정의 오버플로 방지, (3) 소수 계수에 의한 FFT 오차 억제, (4) Nios II에서 실시간 처리 가능한 정수 연산 구성의 균형이다.

**현재 구현의 채택안**

| 연산 대상 | 저장 자료형 | 수치 해석 | 선택 이유 |
|---|---|---|---|
| ADC/DAC PCM | `int16_t` | Signed Q1.15 | ±1 Full-scale에 대응하는 16-bit 오디오 샘플 |
| FFT 실수부·허수부 | `int32_t` | Signed Q17.15 | 6개 FFT Stage의 비트 성장을 수용하면서 입력의 소수 해상도 유지 |
| Twiddle Factor | `int16_t` | Signed Q2.14 | ±1.0을 정확히 표현하면서 소수부 14비트 확보 |
| 복소 곱셈의 중간 연산 | `int64_t` | 곱셈 결과는 Q19.29에 해당 | `int32 × int16` 곱과 두 곱의 합·차에서 오버플로 방지 |
| DAC DDS 위상 | `uint32_t` | 32-bit 순환 위상 누산기 | 주파수 조절 시 위상 연속성 및 높은 위상 해상도 |

> **핵심 판단:** CODEC의 *16-bit PCM 워드 길이*와 FFT의 *내부 누산 데이터 폭*은 별개다. 입력은 16-bit로 유지하되 FFT 내부는 32-bit로 확장하고, Twiddle Factor는 16-bit Q2.14로 표현하는 것이 현재 시스템의 합리적인 절충안이다.

## 2. 실제 구현 범위와 하드웨어 조건

### 2.1 실행 환경

| 항목 | 현재 CPulator 구현 |
|---|---|
| CPU / 시스템 | Nios II, DE1-SoC Computer 가상 시스템 |
| 오디오 샘플링 주파수 | **8,000 samples/s** (`AUDIO_SAMPLE_RATE = 8000`) |
| Audio Core MMIO | `0xFF203040` |
| Audio IRQ | IRQ 6 |
| Timer IRQ | IRQ 0, 1 ms SysTick |
| Audio Core 샘플 레지스터 | 32-bit 데이터 경로로 처리 (`AUDIO_CORE_WIDTH = 32`) |
| 프로그램의 PCM 표현 | `int16_t`, Audio Core 32-bit 워드의 상위 16비트 사용 |
| FFT | 64-point, **125 Hz/bin**, 약 8 ms 분석 구간 |
| FFT 스펙트럼 | 0~31번 Bin 표시(0~3.875 kHz), Nyquist 4 kHz |
| VGA | 320×240, 시간 파형 + FFT 막대 그래프 |

CPulator의 **ADC Input** 모드에서는 실제 마이크가 아니라 시뮬레이터가 제공하는 32-sample 주기의 톱니파가 입력된다. 따라서 8 kHz에서 기본 주파수는 `8000/32 = 250 Hz`이다. **DAC PCM** 모드에서는 DDS가 DAC FIFO에 제출하는 *디지털 샘플*을 분석한다. 두 모드 간 전환은 신호 분석용 Ring Buffer를 바꾸는 기능이지, DAC의 아날로그 출력을 ADC에 물리적으로 되돌리는 루프백은 아니다.

### 2.2 WM8731 설정과 CPulator의 구분

첨부된 `altera_up_av_config_auto_init_ob_audio.v`는 WM8731 초기화 파라미터로 `AUD_DATA_FORMAT = 9'h001`을 정의한다. 이는 WM8731 R7 레지스터의 **16-bit, Left-Justified, Slave Mode** 구성에 해당한다. 또한 `AUD_SAMPLE_CTRL = 9'h002`가 정의되어 있으나, 실제 샘플링 주파수는 **MCLK와 해당 레지스터의 조합**으로 결정된다. 이 값 자체가 곧 8 kHz를 뜻하는 것은 아니다.

현재 CPulator 장치 목록에는 Audio/Video Configuration Core가 MMIO 장치로 표시되지 않아 `CODEC_CONFIG_MMIO = 0`으로 빌드한다. 따라서 **CPulator에서 CODEC 레지스터를 펌웨어로 다시 설정한 결과가 아닌, 시뮬레이터의 8 kHz Audio 모델에서 관찰한 결과**로 기재해야 한다. 실제 WM8731 하드웨어의 디지털 설정과 실측 성능을 혼동해서는 안 된다.

## 3. 16-bit PCM과 Q1.15 선택 근거

### 3.1 16-bit 부호 있는 정수 샘플

오디오 PCM 값은 다음 범위에서 해석한다.

$$
-32768\le x_{\mathrm{PCM}}\le 32767
$$

이를 Signed Q1.15(**부호를 포함한 정수부 1비트 + 소수부 15비트**)로 해석하면,

$$
x_{\mathrm{norm}}=\frac{x_{\mathrm{PCM}}}{2^{15}}
$$

$$
-1\le x_{\mathrm{norm}}\le1-2^{-15}
$$

**정규화 1 LSB**는 다음과 같다.

$$
\Delta_{\mathrm{PCM}}=2^{-15}\approx 3.05176\times10^{-5}
$$

| PCM 정수값 | Q1.15 해석값 | 의미 |
|---:|---:|---|
| -32768 | -1.000000 | 음의 Full-scale |
| -16384 | -0.500000 | 음의 절반 진폭 |
| 0 | 0.000000 | 기준점 |
| 16384 | +0.500000 | 양의 절반 진폭 |
| 32767 | +0.999969 | 양의 최대 진폭 |

프로그램에서는 매 샘플을 실수로 나누는 대신 원시 PCM 정수 코드를 그대로 유지한다. `Q1.15`라는 이름은 **스케일 해석 방법**을 의미하며, 실제 배열에 `float`나 소수점이 저장되는 것은 아니다.

```c
static inline int16_t audio_to_s16(uint32_t word) {
#if AUDIO_CORE_WIDTH == 32
    return (int16_t)(((int32_t)word) >> 16);
#else
    return (int16_t)(word & 0xFFFFu);
#endif
}
```

현재 구현의 위 코드는 **32-bit Audio Core 워드에서 상위 16비트를 취한다는 전제**다. 이는 WM8731의 R7=16-bit 설정과 구분되는, FPGA Audio Core/CPulator 데이터 경로의 표현 문제다.

### 3.2 왜 16-bit PCM인가?

1. WM8731은 16/20/24/32-bit 디지털 오디오 워드 길이를 지원하지만, 제공된 HDL 초기화 설정의 R7은 **16-bit**다.
2. CPulator 코드가 16-bit PCM을 `int16_t`로 저장하므로 샘플당 2바이트로 처리할 수 있다.
3. 이상적 16-bit 양자화기의 최대 정현파 SNR은 `6.02×16 + 1.76 ≈ 98.08 dB`다.
4. WM8731 데이터시트의 **3.3 V, 48 kHz, Line-in 0 dB gain, A-weighted** 조건에서 ADC SNR 전형값은 약 **90 dB**다. 이상적 16-bit 양자화 SNR과 비교하면 CODEC의 실제 아날로그 잡음도 중요한 제한 요인이라는 해석이 가능하다.

단, 두 SNR 수치는 동일한 측정 정의가 아니다. 특히 **실제 보드의 ENOB, 전체 시스템 SNR, 전압 측정 정확도를 직접 측정한 값으로 제시해서는 안 된다.** 또한 Audio Core 레지스터가 32-bit인 경우 CODEC IWL을 16-bit로 설정했다는 이유만으로 MMIO 전송량이 자동으로 절반이 되는 것도 아니다.

## 4. FFT 누산기: Q17.15 및 32-bit 선택 근거

### 4.1 Radix-2 FFT의 비트 성장

64-point FFT의 Stage 수는 다음과 같다.

$$
N=64,\qquad \log_2N=6
$$

Butterfly의 덧셈·뺄셈은 각 Stage에서 데이터의 크기를 키울 수 있다. 순방향 DFT를 `1/N`으로 정규화하지 않는 경우 이상적인 DFT 출력의 절댓값에 대한 기본 상한은,

$$
|X[k]|\le\sum_{n=0}^{63}|x[n]|\le64\cdot\max_n|x[n]|
$$

이다. 입력을 Full-scale의 16-bit PCM 코드로 생각하면,

$$
64\times32768=\boxed{2,097,152}
$$

이 되어 **int16 범위(-32768~32767)를 초과**한다. 따라서 **FFT 입력과 내부 누산기를 모두 16-bit로 유지하는 설계는 일반적인 Full-scale ADC 입력에 안전하지 않다.**

반면 `int32_t`의 범위는 약 ±21억이므로 위 크기보다 훨씬 넓다. 이는 **이상적인 DFT의 크기 상한**이며, 실제 정수 구현에는 Twiddle 계수 양자화와 시프트 절삭 오차가 더해진다. 따라서 최종 판단은 수치 검증도 함께 수행해야 한다.

### 4.2 Q17.15의 의미

Signed Q17.15는 **부호 포함 정수부 17비트 + 소수부 15비트 = 32비트**로 정의한다.

$$
-65536\le x_{\mathrm{FFT}}<65536,\qquad \mathrm{LSB}=2^{-15}
$$

입력 16-bit PCM 정수값을 `int32_t`로 옮길 때 소수부 스케일은 변경하지 않는다.

```c
int32_t fft_real[64];
int32_t fft_imag[64];

fft_real[i] = samples[221 - 64 + i];
fft_imag[i] = 0;
```

이렇게 하면 **소수부 15비트의 해석을 유지하면서, 정수부에 FFT 성장에 필요한 여유 비트를 제공**할 수 있다. 별도의 입력 선행 우시프트에 따른 유효비트 손실도 발생하지 않는다.

### 4.3 왜 각 Stage에서 `>>1`하지 않는가?

16-bit 고정소수점 FFT에서는 각 Stage마다 결과를 우시프트하여 전체적으로 `1/64` 스케일을 적용할 수 있다. 이 방식은 오버플로를 줄이지만, 작은 신호에서 연속적인 반올림·절삭 오차가 누적될 수 있다.

현재는 `int32_t`가 충분한 동적 범위를 제공하므로 **Butterfly Stage마다 강제 우시프트를 적용하지 않는다.** 수학적인 순방향 FFT에 대응하는 **비정규화 스펙트럼**을 계산한 뒤, VGA 표시 단계에서 별도로 스케일을 적용한다.

> `Twiddle 곱셈 후 >>14`는 **계수의 Q14 소수 스케일 복구**이고, `FFT 출력 /64`나 `각 Stage >>1`과 같은 **FFT 진폭 정규화**가 아니다.

## 5. Twiddle Factor: Q2.14 선택 근거

### 5.1 복소 계수의 표현 범위

Radix-2 FFT의 회전 인자는,

$$
W_{64}^{k}=e^{-j2\pi k/64}=\cos\theta-j\sin\theta
$$

이며 실수부·허수부는 `[-1,1]` 범위다. `int16_t` Signed Q2.14(부호 포함 정수부 2비트 + 소수부 14비트)로 저장하면,

$$
W_{\mathrm{Q14}}=\operatorname{round}(W\cdot2^{14})
$$

$$
+1.0\leftrightarrow16384,\quad0.5\leftrightarrow8192,\quad-1.0\leftrightarrow-16384
$$

가 된다. Q2.14의 LSB는,

$$
\Delta_W=2^{-14}\approx6.10352\times10^{-5}
$$

이다. 계수를 가장 가까운 정수로 반올림한다면 **계수 단위 양자화의 최대 절대오차는 `0.5 LSB = 2^-15 ≈ 3.05176×10^-5`**이다.

### 5.2 Q8 및 Q1.15와 비교

| 형식 | 총 16-bit 배분 | +1.0 표현 | 소수 간격 | 판단 |
|---|---|---|---:|---|
| Q8 | 부호 1 + 정수 7 + 소수 8 | 가능 | 1/256 ≈ 0.00390625 | 정수부 여유가 과도하고 계수 정밀도가 낮음 |
| **Q2.14** | 부호 1 + 정수 1 + 소수 14 | **정확히 가능** | 1/16384 ≈ 0.00006104 | **채택**: 범위·해상도 균형 |
| Q1.15 | 부호 1 + 소수 15 | **정확히 불가** | 1/32768 ≈ 0.00003052 | +1.0 특수 처리 또는 포화 필요 |

Q2.14는 Q8보다 소수부가 6비트 많아 **계수 양자화 간격이 64배 촘촘**하다. 다만 *최종 FFT 출력 오차가 반드시 64배 줄어드는 것은 아니다*. 반복 시프트의 절삭, 입력 파형의 LUT 오차 및 연산 순서도 출력 오차에 영향을 준다.

### 5.3 실제 코드의 복소 곱셈

```c
int32_t tr = (int32_t)(
    ((int64_t)xr * wr + (int64_t)xi * wi) >> 14
);
int32_t ti = (int32_t)(
    ((int64_t)xi * wr - (int64_t)xr * wi) >> 14
);

real[i + j]        = ur + tr;
imag[i + j]        = ui + ti;
real[i + j + half] = ur - tr;
imag[i + j + half] = ui - ti;
```

곱셈의 스케일은 다음과 같다.

$$
Q17.15\times Q2.14\rightarrow Q19.29
$$

따라서 결과를 `>>14`하여 다시 Q17.15 스케일로 돌린다. 곱셈 자체 및 두 곱의 합·차는 32-bit를 넘을 수 있으므로 **`int64_t`로 계산한 뒤 시프트**한다.

오른쪽 시프트 과정에서 하위 14비트가 버려져 오차가 발생한다. 음수의 산술 우시프트는 현재 사용한 Nios II GCC 같은 구현의 동작을 전제로 하며, C 언어 표준상 완전한 이식성이 보장되는 연산은 아니라는 점도 유의한다. 정밀한 수치 기준이 요구되면 부호를 고려한 **대칭 반올림(round-to-nearest)**으로 개선할 수 있다.

## 6. FFT Magnitude 및 VGA 표시 정규화

### 6.1 Magnitude는 정확한 제곱근이 아닌 근사값

일반적인 복소 FFT 크기는,

$$
|X[k]|=\sqrt{\mathrm{Re}(X[k])^2+\mathrm{Im}(X[k])^2}
$$

이다. 현재 코드는 Nios II의 연산 부담을 줄이기 위해 다음 근사식을 사용한다.

$$
|X[k]|_{\mathrm{approx}}=\max(|\mathrm{Re}|,|\mathrm{Im}|)
+\frac12\min(|\mathrm{Re}|,|\mathrm{Im}|)
$$

```c
uint32_t hi = (a > b) ? a : b;
uint32_t lo = (a > b) ? b : a;
return hi + (lo >> 1);
```

이 근사식은 연속 실수에서 정확한 크기에 비해 최대,

$$
\sqrt{1+0.5^2}-1\approx\boxed{11.803\%}
$$

까지 과대평가할 수 있다(정수 `>>1`의 절삭 효과는 별도). 따라서 **Peak Bin 탐지용으로는 실용적**이지만, **정밀 전압·진폭 측정에는 추가 오차가 존재**한다.

### 6.2 VGA 막대 정규화는 FFT 내부 정규화와 다름

현재 렌더링 코드는,

```c
int new_h = (int)(((uint64_t)mag * 70u) / (32u * 32768u));
if (new_h > 70) new_h = 70;
```

을 사용한다. **64-point, Bin에 정확히 정렬된 Full-scale 정현파**에서 양의 주파수 Bin 크기는 대략 `N × 32768 / 2 = 32 × 32768`이므로 이를 **70픽셀**로 대응시킨다.

$$
H[k]=\operatorname{clip}_{[0,70]}
\left(\left\lfloor\frac{|X[k]|_{\mathrm{approx}}\cdot70}{32\cdot32768}\right\rfloor\right)
$$

이는 **VGA 시각화를 위한 고정 스케일**이다. 출력 스펙트럼의 물리 단위를 V 또는 Vrms로 보정한 것은 아니며, 클리핑이나 Magnitude 근사 오차 때문에 막대 높이와 실제 아날로그 진폭이 정확히 일치하지는 않는다.

### 6.3 주파수 Peak와 진폭 정확도는 서로 다른 문제

$$
\Delta f=\frac{f_s}{N}=\frac{8000}{64}=\boxed{125\ \mathrm{Hz/bin}}
$$

현재 DDS의 설정 간격은 250 Hz이므로 **설정 주파수가 FFT Bin에 정렬되는 시험 조건**이다. 따라서 주파수 Peak가 정확히 표시되어도 *Q14 계수의 오차가 0이거나 임의 주파수에서 분해능이 충분하다*는 뜻은 아니다.

ADC 입력의 임의 주파수는 Bin 사이에 놓일 수 있으며 현재 버전에는 Window 함수가 없어 **Spectral Leakage**가 발생한다. 필요 시 Hann Window 및 Peak Interpolation 적용을 검토한다.

## 7. 정량 검증: 동일 입력에 대한 Q8·Q2.14 비교

### 7.1 시험 방법

현재 C 코드의 32-entry 정현파 LUT, 진폭 계산, 32-bit DDS 위상 누산 방식을 동일하게 모사한 뒤, 생성된 **실제 16-bit PCM 정수 샘플 64개**를 다음 세 방식으로 분석했다.

- **Reference:** 동일한 PCM 배열에 대한 NumPy `float64` FFT
- **Q8 비교군:** 최초 코드의 Q8 Twiddle 테이블을 사용하고 **나머지 계산 경로는 현재 코드와 같은 32-bit FFT 데이터 / 64-bit 중간 연산**으로 통일
- **Q2.14 채택군:** 현재 `W_COS_Q14`, `W_SIN_Q14` 테이블 및 `>>14` 연산 사용

검증 범위는 **14개 주파수(250~3500 Hz, 250 Hz 간격) × 진폭 5단계 × 시작 위상 32개 = 총 2,240개 입력 조합**이다.

> Q8과 Q2.14를 공정하게 비교하기 위해 FFT의 누산 자료형 및 입력 PCM은 같게 유지했다. 따라서 Q8 비교군은 **초기 소스 전체의 동작을 그대로 재실행한 결과가 아니라, Twiddle 표현 정밀도 차이를 비교하기 위한 재구성**이다.

주요 Bin의 복소 결과 오차는 다음과 같이 정의하였다.

$$
\varepsilon_{\mathrm{peak}}=\frac{
|X_{\mathrm{fixed}}[k_{\mathrm{ref}}]-X_{\mathrm{float64}}[k_{\mathrm{ref}}]|
}{|X_{\mathrm{float64}}[k_{\mathrm{ref}}]|}\times100\%
$$

### 7.2 검증 결과

| 지표 | Q8 비교군 | **현재 Q2.14** |
|---|---:|---:|
| Twiddle 계수의 실제 최대 절대오차 | 약 0.003997 | **약 0.00002988** |
| 주요 Bin의 최대 복소 상대오차 | 약 **0.14143%** | **약 0.00509%** |
| 주요 Bin의 평균 복소 상대오차 | 약 0.09629% | **약 0.00127%** |
| 주요 Bin의 최대 복소 절대오차(PCM 코드 단위) | 약 1,312.30 | **약 21.33** |
| Peak Bin 오검출 수 | 0 / 2,240 | **0 / 2,240** |
| Butterfly 연산 중 최대 실수부·허수부 절댓값 | 956,782 | **956,823** |

**해석:** 시험한 DDS 설정에서는 Q8도 Peak Bin을 정확히 검출했다. 따라서 Q2.14를 사용한 목적은 단순한 Peak 위치 개선보다 **복소 FFT 계산 결과의 정밀도 향상**에 있다. Q2.14는 해당 테스트에서 최대 복소 상대오차를 `0.14143% → 0.00509%`로 낮췄다.

**결과의 적용 범위 및 주의사항:**

- 이 수치는 **코드 기반 수치 시뮬레이션**이며, CPulator 또는 실제 보드에서 수치 오차를 직접 계측한 값이 아니다.
- 동일한 정수 PCM을 기준값에도 사용했으므로 **DDS LUT 파형 자체의 정현파 근사 오차**, ADC의 아날로그 잡음/왜곡, DAC 아날로그 출력 오차는 이 비교에 포함되지 않는다.
- 표시용 `get_magnitude()`의 최대 11.8% 수준 근사 오차도 위 **복소 FFT 상대오차**와 별개다.
- 임의 입력(잡음, 다중 톤, Bin 비정렬 주파수, Full-scale 충격파 등)에 대한 전 범위 정확성을 보장하는 실험은 아니다.

## 8. 메모리·연산량·실시간성 Trade-off

| 설계 대안 | 장점 | 단점 / 위험 | 판단 |
|---|---|---|---|
| FFT 데이터 `int16_t` | 메모리 절약, 작은 데이터 폭 | Full-scale 입력에서 6-Stage FFT 오버플로 위험 | 미채택 |
| **FFT 데이터 `int32_t`** | 중간 비트 성장 수용, 불필요한 Stage 시프트 없음 | FFT 배열 메모리 2배, 연산 비용 증가 | **채택** |
| FFT Stage마다 1비트 시프트 | 16-bit 범위 유지 가능 | 소신호 유효비트 손실, 1/64 스케일 추적 필요 | 대안 |
| Q8 Twiddle | 작은 계수 정밀도로 구현 단순 | 회전 인자 양자화 오차 상대적으로 큼 | 미채택 |
| **Q2.14 Twiddle** | ±1.0 정확 표현, 충분한 계수 해상도 | 64-bit 중간 연산 처리 부담 | **채택** |
| Q1.15 Twiddle | 계수 소수부 1비트 추가 | +1.0의 정확한 표현 불가 | 미채택 |
| Magnitude 제곱근 | 정확한 복소 크기 계산 | 연산 부담 증가 | 미채택 |
| **`max + min/2`** | 정수 비교·시프트로 계산 | 최대 약 11.8% 크기 오차 | **시각화 목적 채택** |

### 8.1 연산 수 및 메모리

64-point Radix-2 FFT의 Butterfly 횟수는,

$$
\frac{N}{2}\log_2N=\frac{64}{2}\times6=\boxed{192\ \mathrm{butterflies}}
$$

이다. 현재 `real[64]`, `imag[64]`는 각각 `int32_t`이므로 **FFT 배열 자체는 512바이트**를 사용한다. 같은 배열을 `int16_t`로 사용하면 256바이트다. Twiddle 배열 두 개는 합계 128바이트이며, ADC/DAC Ring Buffer 두 개는 `1024×2×2 = 4096바이트`다(별도 GUI 상태 등 제외).

Nios II에서의 64-bit 곱셈·시프트 구현은 CPU 구성과 컴파일러 옵션에 따라 실행시간이 크게 달라질 수 있다. **C 코드에서 이론적으로 오버플로를 피했다는 사실만으로 실시간 처리성이 증명되지는 않는다.** FIFO Overflow/Underflow 카운터, Audio IRQ 실행시간, `fixed_fft_64()` 실행시간 및 VGA Frame 갱신시간을 실제 실행 환경에서 확인해야 한다.

### 8.2 현재 버전에서 아직 수행하지 않은 검증

1. **수치 견고성:** 최대·최소 PCM, DC, 교번 최대치, 여러 정현파 혼합, Bin 비정렬 주파수에 대한 오버플로 및 오차 테스트
2. **스펙트럼 정확도:** Hann Window 적용 전후의 Leakage, Peak 추정 오차 비교
3. **진폭 계측:** 정확한 Magnitude (`sqrt`)와 근사식의 출력 비교; 보정된 dBFS 표시
4. **실시간성:** Nios II에서 IRQ 서비스 시간, FFT 계산시간, VGA Render 시간과 FIFO 상태 측정
5. **실제 보드:** WM8731 오디오 클록/워드 길이/아날로그 Gain 확인 및 Line-in·Line-out 계측

## 9. 보고서에 바로 사용할 설계 근거 문단

> 본 프로젝트에서는 WM8731의 16-bit PCM 데이터 형식과 Nios II 기반 Audio Core의 처리 구조를 고려하여 64-point Radix-2 DIT FFT를 고정소수점 연산으로 구현하였다. 입력 PCM은 Signed Q1.15로 해석하고, FFT의 실수부 및 허수부는 Stage별 덧셈·뺄셈에 따른 비트 성장을 수용하기 위해 32-bit Q17.15로 저장하였다. 이에 따라 Butterfly 연산에서 추가적인 Stage별 강제 스케일링 없이 입력의 소수부 해상도를 유지하였다. 복소 회전 인자인 Twiddle Factor는 범위가 ±1인 특성을 고려해 16-bit Q2.14로 표현하여 ±1.0을 정확히 저장하고 14-bit 소수부 정밀도를 확보하였다. 곱셈과 가산의 중간 계산에는 `int64_t`를 사용하여 오버플로를 방지하고, 곱셈 결과를 14비트 산술 우시프트하여 FFT 데이터 스케일로 복귀시켰다.
>
> DDS의 250~3500 Hz, 진폭 5단계 및 시작 위상 32개 조합으로 구성된 총 2,240개 소프트웨어 수치 시험에서, 동일 PCM 입력의 `float64` FFT와 비교한 주요 주파수 Bin의 최대 복소 상대오차는 Q2.14 방식에서 약 0.00509%였다. 동일한 내부 데이터 폭을 사용하는 Q8 비교군의 약 0.14143%보다 작았으며, 두 방식 모두 해당 시험에서 Peak Bin 오검출은 없었다. 따라서 현재 구현의 Q2.14 채택은 주파수 Peak 검출 자체보다 FFT 복소 연산의 계수 양자화 오차를 낮추기 위한 선택이다. 한편 VGA의 스펙트럼 크기는 연산량을 고려해 제곱근 대신 `max + min/2` 근사식으로 계산하므로, 해당 표시값은 정밀한 아날로그 전압 계측값이 아닌 시각화용 크기 정보로 해석해야 한다.

## 10. 요약

- **16-bit:** WM8731의 설정된 오디오 워드 길이 및 현재 펌웨어 PCM 저장 형식과 부합한다.
- **Q1.15:** 입력을 부동소수점으로 바꾸지 않고 `[-1, 1)` Full-scale로 해석할 수 있다.
- **32-bit Q17.15:** 64-point FFT에서 발생하는 내부 비트 성장을 수용하여 16-bit 누산 오버플로를 방지한다.
- **16-bit Q2.14:** ±1.0을 정확히 표현하며 Q8보다 64배 촘촘한 회전 인자 양자화 간격을 제공한다.
- **64-bit 곱셈 중간값:** Q17.15×Q2.14 연산의 중간 범위를 수용하고 `>>14`로 스케일을 복원한다.
- **비정규화 FFT + VGA 정규화:** FFT 정확도와 출력 화면 스케일링을 분리한다.
- **제약:** 64-point/8 kHz에 따른 125 Hz Bin 간격, Window 부재, 근사 Magnitude 오차, 실시간 성능 미계측.

---

## 참고 자료 및 코드 위치

1. **펌웨어:** `de1soc_cpulator_irq_dual_source.c`
   - `AUDIO_SAMPLE_RATE`, `AUDIO_CORE_WIDTH`, `ADC_RING_SIZE`, `SOURCE_ADC/DAC`
   - `W_COS_Q14`, `W_SIN_Q14`: 고정소수점 Twiddle 상수
   - `fixed_fft_64()`: 64-point Radix-2 DIT Butterfly 및 `int64_t` 중간 연산
   - `get_magnitude()`: Magnitude 근사
   - `audio_to_s16()`, `audio_from_s16()`: Audio Core 워드/PCM 변환
   - `audio_isr()`: ADC 샘플 수집 및 DDS DAC PCM 생성
   - `render_frame()`: 최신 64샘플 FFT, 스펙트럼 높이 변환
2. **CODEC HDL 초기값:** `altera_up_av_config_auto_init_ob_audio.v` — `AUD_DATA_FORMAT`, `AUD_SAMPLE_CTRL` 파라미터.
3. **CODEC 데이터시트:** `WM8731.PDF` — 1쪽 지원 워드 길이, 7쪽 ADC SNR 조건, Audio Interface 및 Register Map 관련 장.
4. **CPulator 테스트 관찰:** DAC PCM 모드에서 750 Hz Peak, ADC Input 모드에서 250 Hz Sawtooth 및 고조파 확인. 이 관찰은 **기능 검증**이며 위의 **FFT 정량오차 실험**과 별개의 증거다.
