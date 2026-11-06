/*
 * DE1-SoC / CPulator Audio IRQ demo: selectable spectrum source.
 * SOURCE_DAC (default): waveform/FFT track DDS PCM written to DAC FIFO.
 * SOURCE_ADC: waveform/FFT show independent input (250 Hz sawtooth in CPulator).
 * Click [SOURCE] at lower-right to switch views.
 * This digital monitor does not physically route DAC analog output into ADC.
 * 
 * [안내] 현재 구현된 FFT 및 Fixed-Point(Q2.14) 스케일링/비트 수는 CPulator 시뮬레이션 환경용 임시 파라미터입니다.
 * 추후 실제 DE1-SoC 하드웨어 보드에서 Audio Codec Line-In 전압(Vpp) 및 Nios II FPU Custom Instruction 활용 여부를
 * 실측 확인한 후 최종 확정 및 최적화될 예정입니다.
 */
#include <stdint.h>

/* CPulator DE1-SoC Nios II 8 kHz build.
 * CODEC_CONFIG_MMIO=0: the simulator does NOT map the config core.
 * Set to 1 only for a physical system with a confirmed config core at 0xFF203000.
 */
#ifndef CODEC_CONFIG_MMIO
#define CODEC_CONFIG_MMIO 0
#endif
#define AUDIO_SAMPLE_RATE 8000u
#define AUDIO_CORE_WIDTH 32  /* Audio Core data width, distinct from WM8731 16-bit IWL */
#define ADC_RING_SIZE 1024u /* power of two; used for both ADC/DAC monitors */
#define ADC_RING_MASK (ADC_RING_SIZE-1u)
#define SOURCE_ADC 0
#define SOURCE_DAC 1
#define AUDIO_IRQ (1u << 6)
#define TIMER_IRQ (1u << 0)

/* IRQ vector in the .exceptions section (exception address 0x20).
 * Save all GPRs except sp, and restore ea/et exactly as Nios II requires.
 * The C function interrupt_handler() is defined below.
 */
void interrupt_handler(void);
__asm__(
    ".section .exceptions, \"ax\"\n"
    ".global the_exception\n"
    "the_exception:\n"
    "subi sp, sp, 128\n"
    "stw et, 96(sp)\n"
    "rdctl et, ipending\n"
    "beq et, r0, 1f\n"
    "subi ea, ea, 4\n"
    "1:\n"
    "stw r1, 4(sp)\n"
    "stw r2, 8(sp)\n"
    "stw r3, 12(sp)\n"
    "stw r4, 16(sp)\n"
    "stw r5, 20(sp)\n"
    "stw r6, 24(sp)\n"
    "stw r7, 28(sp)\n"
    "stw r8, 32(sp)\n"
    "stw r9, 36(sp)\n"
    "stw r10, 40(sp)\n"
    "stw r11, 44(sp)\n"
    "stw r12, 48(sp)\n"
    "stw r13, 52(sp)\n"
    "stw r14, 56(sp)\n"
    "stw r15, 60(sp)\n"
    "stw r16, 64(sp)\n"
    "stw r17, 68(sp)\n"
    "stw r18, 72(sp)\n"
    "stw r19, 76(sp)\n"
    "stw r20, 80(sp)\n"
    "stw r21, 84(sp)\n"
    "stw r22, 88(sp)\n"
    "stw r23, 92(sp)\n"
    "stw r25, 100(sp)\n"
    "stw r26, 104(sp)\n"
    "stw r28, 112(sp)\n"
    "stw r29, 116(sp)\n"
    "stw r30, 120(sp)\n"
    "stw r31, 124(sp)\n"
    "addi fp, sp, 128\n"
    "call interrupt_handler\n"
    "ldw r1, 4(sp)\n"
    "ldw r2, 8(sp)\n"
    "ldw r3, 12(sp)\n"
    "ldw r4, 16(sp)\n"
    "ldw r5, 20(sp)\n"
    "ldw r6, 24(sp)\n"
    "ldw r7, 28(sp)\n"
    "ldw r8, 32(sp)\n"
    "ldw r9, 36(sp)\n"
    "ldw r10, 40(sp)\n"
    "ldw r11, 44(sp)\n"
    "ldw r12, 48(sp)\n"
    "ldw r13, 52(sp)\n"
    "ldw r14, 56(sp)\n"
    "ldw r15, 60(sp)\n"
    "ldw r16, 64(sp)\n"
    "ldw r17, 68(sp)\n"
    "ldw r18, 72(sp)\n"
    "ldw r19, 76(sp)\n"
    "ldw r20, 80(sp)\n"
    "ldw r21, 84(sp)\n"
    "ldw r22, 88(sp)\n"
    "ldw r23, 92(sp)\n"
    "ldw et, 96(sp)\n"
    "ldw r25, 100(sp)\n"
    "ldw r26, 104(sp)\n"
    "ldw r28, 112(sp)\n"
    "ldw r29, 116(sp)\n"
    "ldw r30, 120(sp)\n"
    "ldw r31, 124(sp)\n"
    "addi sp, sp, 128\n"
    "eret\n"
    ".previous\n"
);


// ============================================================================
// DE1-SoC Memory Map & Base Addresses (Altera University Program)
// ============================================================================
#define TIMER0_BASE          0xFF202000 // Interval Timer 0 (SysTick)
#define PS2_PORT1_BASE       0xFF200100 // PS/2 Controller 1 (Keyboard)
#define PS2_PORT2_BASE       0xFF200108 // PS/2 Controller 2 (Mouse in CPulator)
#define AUDIO_BASE           0xFF203040 // Audio Core (Line In/Out FIFO)
#define VGA_PIXEL_BUF_BASE   0x08000000 // FPGA On-chip Pixel Memory (320x240)
#define VGA_CHAR_BASE        0x09000000 // VGA Character Buffer Base (80x60)

#define HW_REGS(base)        ((volatile uint32_t *)(base))

#define VGA_WIDTH            320
#define VGA_HEIGHT           240

// 16-bit RGB565 Colors
#define COLOR_BLACK          0x0000
#define COLOR_WHITE          0xFFFF
#define COLOR_YELLOW         0xFFE0
#define COLOR_CYAN           0x07FF
#define COLOR_GREEN          0x07E0
#define COLOR_RED            0xF800
#define COLOR_DARK_GRAY      0x39E7
#define COLOR_GRID_GRAY      0x2104

// Scope Display Bounds
#define WAVE_LEFT            15
#define WAVE_RIGHT           235

// Split-View Layout Boundaries
#define TIME_TOP             14
#define TIME_BOTTOM          114
#define TIME_CENTER_Y        64

#define DIVIDER_Y            119

#define FFT_TOP              124
#define FFT_BOTTOM           228
#define FFT_BASELINE         224
#define FFT_BINS             32
#define FFT_BAR_WIDTH        4
#define FFT_BAR_PITCH        6
#define FFT_START_X          25

// ============================================================================
// Data Structures
// ============================================================================
typedef struct {
    int x;
    int y;
    int prev_x;
    int prev_y;
    uint8_t left_btn;
    uint8_t prev_left_btn; // For single-click edge detection
    uint8_t right_btn;
} mouse_state_t;

typedef struct {
    int v1_cursor_y;       // Current V1 Y
    int prev_v1_cursor_y;  // Previous V1 Y
    int trigger_level_y;   // Current Trigger Y
    int prev_trigger_y;    // Previous Trigger Y
    int is_dragging_v1;
    int is_dragging_trig;
} scope_state_t;

// ============================================================================
// Global State Variables
// ============================================================================
static volatile uint32_t g_jiffies = 0;
static volatile uint32_t g_adc_total = 0; /* monotonic producer count */
static volatile int16_t g_adc_ring[ADC_RING_SIZE]; /* physical/simulated ADC PCM */
static volatile uint32_t g_dac_total = 0; /* PCM samples submitted to output FIFO */
static volatile int16_t g_dac_ring[ADC_RING_SIZE]; /* digital DAC monitor (not ADC) */
static int g_source_mode = SOURCE_DAC; /* default: waveform follows DDS settings */
static volatile uint32_t g_adc_irq_count = 0;
static volatile uint32_t g_audio_irq_count = 0;
static volatile uint32_t g_dac_phase_inc = 0;
static volatile uint32_t g_dac_sent = 0;

static mouse_state_t     g_mouse;
static scope_state_t     g_scope;

// Generator Control States (Initial: 0)
static int g_freq_hz   = 0; // 0..3500 Hz (Step: 250 Hz)
static volatile int g_amp_level = 0; // 0..5 (Step: 1 level = 0.5V)

static uint32_t g_audio_phase = 0;
static uint8_t  g_ps2_packet[3];
static int      g_ps2_idx = 0;

static uint16_t g_mouse_bg[8][8];
static int      g_old_wave_y[WAVE_RIGHT - WAVE_LEFT + 1];
static int      g_old_fft_h[FFT_BINS];

// 32-step Integer Sine Lookup Table (Amp: +/- 35)
static const int8_t SINE_LUT[32] = {
      0,   7,  13,  19,  25,  29,  32,  35,
     35,  35,  32,  29,  25,  19,  13,   7,
      0,  -7, -13, -19, -25, -29, -32, -35,
    -35, -35, -32, -29, -25, -19, -13,  -7
};

// 8x8 Mouse Arrow Cursor
static const uint8_t MOUSE_ARROW[8][8] = {
    {1, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 0, 0, 0, 0, 0, 0},
    {1, 2, 1, 0, 0, 0, 0, 0},
    {1, 2, 2, 1, 0, 0, 0, 0},
    {1, 2, 2, 2, 1, 0, 0, 0},
    {1, 2, 1, 1, 0, 0, 0, 0},
    {1, 1, 0, 1, 1, 0, 0, 0},
    {1, 0, 0, 0, 1, 1, 0, 0}
};

/* Signed Q2.14 twiddle values: +/-1 is exactly representable. */
static const int16_t W_COS_Q14[32] = {
    16384, 16305, 16069, 15679, 15137, 14449, 13623, 12665,
    11585, 10394, 9102, 7723, 6270, 4756, 3196, 1606,
    0, -1606, -3196, -4756, -6270, -7723, -9102, -10394,
    -11585, -12665, -13623, -14449, -15137, -15679, -16069, -16305
};
static const int16_t W_SIN_Q14[32] = {
    0, 1606, 3196, 4756, 6270, 7723, 9102, 10394,
    11585, 12665, 13623, 14449, 15137, 15679, 16069, 16305,
    16384, 16305, 16069, 15679, 15137, 14449, 13623, 12665,
    11585, 10394, 9102, 7723, 6270, 4756, 3196, 1606
};

// ============================================================================
// VGA Low-Level Driver
// ============================================================================
static inline void vga_draw_pixel(int x, int y, uint16_t color) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        volatile uint16_t *pixel_addr = (volatile uint16_t *)(VGA_PIXEL_BUF_BASE + ((y << 10) + (x << 1)));
        *pixel_addr = color;
    }
}

static inline uint16_t vga_get_pixel(int x, int y) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT) {
        volatile uint16_t *pixel_addr = (volatile uint16_t *)(VGA_PIXEL_BUF_BASE + ((y << 10) + (x << 1)));
        return *pixel_addr;
    }
    return COLOR_BLACK;
}

static inline uint16_t get_background_color(int x, int y) {
    if (x < WAVE_LEFT || x > WAVE_RIGHT || y < TIME_TOP || y > FFT_BOTTOM) {
        return (x >= 240) ? COLOR_DARK_GRAY : COLOR_BLACK;
    }
    if (y == DIVIDER_Y) return COLOR_WHITE;
    if (y == FFT_BASELINE && x >= WAVE_LEFT && x <= WAVE_RIGHT) return COLOR_WHITE;

    if (y >= TIME_TOP && y <= TIME_BOTTOM) {
        if (((y - TIME_TOP) % 25 == 0) && ((x - WAVE_LEFT) % 27 == 0)) {
            return COLOR_GRID_GRAY;
        }
    }
    if (y >= FFT_TOP && y <= FFT_BOTTOM) {
        if ((y % 20 == 0) && ((x - FFT_START_X) % 36 == 0)) {
            return COLOR_GRID_GRAY;
        }
    }
    return COLOR_BLACK;
}

static void vga_draw_text(int col, int row, const char *str) {
    volatile char *char_buf = (volatile char *)VGA_CHAR_BASE;
    while (*str) {
        if (col >= 0 && col < 80 && row >= 0 && row < 60) {
            *(char_buf + ((row << 7) + col)) = *str;
        }
        col++;
        str++;
    }
}

static void vga_save_mouse_bg(int x, int y) {
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            g_mouse_bg[r][c] = vga_get_pixel(x + c, y + r);
        }
    }
}

static void vga_restore_mouse_bg(int x, int y) {
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (MOUSE_ARROW[r][c] != 0) {
                vga_draw_pixel(x + c, y + r, g_mouse_bg[r][c]);
            }
        }
    }
}

static void vga_draw_mouse_pointer(int x, int y) {
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            uint8_t pixel_type = MOUSE_ARROW[r][c];
            if (pixel_type == 1) {
                vga_draw_pixel(x + c, y + r, COLOR_BLACK);
            } else if (pixel_type == 2) {
                vga_draw_pixel(x + c, y + r, COLOR_WHITE);
            }
        }
    }
}

static void draw_box(int x1, int y1, int x2, int y2, uint16_t color) {
    for (int x = x1; x <= x2; x++) {
        vga_draw_pixel(x, y1, color);
        vga_draw_pixel(x, y2, color);
    }
    for (int y = y1; y <= y2; y++) {
        vga_draw_pixel(x1, y, color);
        vga_draw_pixel(x2, y, color);
    }
}

// ============================================================================
// Fixed-Point 64-Point FFT Engine (CPulator 시뮬레이션용 임시 구현)
// [주의] 본 FFT 비트 분할(Q2.14) 및 스케일링 파라미터는 시뮬레이션 검증용 임시 설정이며,
// 추후 실제 DE1-SoC 보드에서 Codec 파라미터 실측 및 FPU 사용 여부 검토 후 최종 결정 예정입니다.
// ============================================================================
static void fft_bit_reverse(int32_t *real, int32_t *imag) {
    int j = 0;
    for (int i = 0; i < 63; i++) {
        if (i < j) {
            int32_t tr = real[i]; real[i] = real[j]; real[j] = tr;
            int32_t ti = imag[i]; imag[i] = imag[j]; imag[j] = ti;
        }
        int k = 32;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }
}

/* FFT stage data are 32-bit raw PCM codes (15 fractional bits); twiddles Q2.14.
 * Output scale equals unnormalized DFT, no per-stage right shift.
 * 64*32768 < 2^31 so stage growth fits in int32_t.
 */
static void fixed_fft_64(int32_t *real, int32_t *imag) {
    fft_bit_reverse(real, imag);
    for (int len = 2; len <= 64; len <<= 1) {
        int half = len >> 1, step = 64 / len;
        for (int i = 0; i < 64; i += len) {
            for (int j = 0; j < half; ++j) {
                int k = j * step;
                int32_t wr = W_COS_Q14[k];
                int32_t wi = W_SIN_Q14[k];
                int32_t xr = real[i + j + half], xi = imag[i + j + half];
                int32_t ur = real[i + j], ui = imag[i + j];
                /* 64-bit intermediates avoid overflow from additions and products. */
                int32_t tr = (int32_t)(((int64_t)xr * wr + (int64_t)xi * wi) >> 14);
                int32_t ti = (int32_t)(((int64_t)xi * wr - (int64_t)xr * wi) >> 14);
                real[i + j]        = ur + tr;
                imag[i + j]        = ui + ti;
                real[i + j + half] = ur - tr;
                imag[i + j + half] = ui - ti;
            }
        }
    }
}

/* Cheap magnitude for display and peak detection (not an RMS/calibrated V). */
static inline uint32_t get_magnitude(int32_t r, int32_t i) {
    uint32_t a = (uint32_t)(r < 0 ? -r : r);
    uint32_t b = (uint32_t)(i < 0 ? -i : i);
    uint32_t hi = (a > b) ? a : b;
    uint32_t lo = (a > b) ? b : a;
    return hi + (lo >> 1);
}

/* Audio core FIFO register layout: WSLC[31:24], WSRC[23:16],
 * RALC[15:8], RARC[7:0]. The core is 32-bit PCM in this build.
 */
static inline int16_t audio_to_s16(uint32_t word) {
#if AUDIO_CORE_WIDTH == 32
    return (int16_t)(((int32_t)word) >> 16);
#else
    return (int16_t)(word & 0xFFFFu);
#endif
}
static inline uint32_t audio_from_s16(int16_t s) {
#if AUDIO_CORE_WIDTH == 32
    return (uint32_t)((int32_t)s * 65536);
#else
    return (uint32_t)(uint16_t)s;
#endif
}

/* Called only from IRQ context or once at boot before global interrupts. */
static void audio_isr(void) {
    volatile uint32_t *audio = HW_REGS(AUDIO_BASE);
    g_audio_irq_count++;

    /* Drain all stereo input samples, and retain the left channel for scope/FFT. */
    uint32_t fs = audio[1];
    uint32_t n_l = (fs >> 8) & 0xFFu;
    uint32_t n_r = fs & 0xFFu;
    uint32_t n = (n_l < n_r) ? n_l : n_r;
    while (n--) {
        int16_t left  = audio_to_s16(audio[2]);
        (void)audio[3]; /* Right-channel FIFO must also be drained. */
        g_adc_ring[g_adc_total & ADC_RING_MASK] = left;
        g_adc_total++;
        g_adc_irq_count++;
    }

    /* Refill paired output FIFOs to full, clearing the write IRQ level. */
    fs = audio[1];
    uint32_t w_l = (fs >> 24) & 0xFFu;
    uint32_t w_r = (fs >> 16) & 0xFFu;
    uint32_t w = (w_l < w_r) ? w_l : w_r;
    while (w--) {
        int16_t s = 0;
        uint32_t inc = g_dac_phase_inc;
        if (inc != 0 && g_amp_level > 0) {
            g_audio_phase += inc;
            uint32_t idx = (g_audio_phase >> 27) & 31u;
            int val = ((int)SINE_LUT[idx] * g_amp_level) / 5;
            s = (int16_t)(val * 850); /* abs max = 29,750 < 32,768 */
        }
        /* Digital monitor of precisely the PCM sample written to the DAC FIFO.
         * This is NOT a measurement of the analog Line-out signal. */
        g_dac_ring[g_dac_total & ADC_RING_MASK] = s;
        g_dac_total++;
        uint32_t d = audio_from_s16(s);
        audio[2] = d;
        audio[3] = d;
        g_dac_sent++;
    }
}

/* CPUlator's Device List omits 0xFF203000, so leave this OFF there.
 * On hardware with the AV Config MMIO present, write WM8731 registers here.
 * R8 is deliberately preserved: codec clocks determine the sample rate.
 */
static int codec_write_reg(unsigned reg, unsigned val) {
#if CODEC_CONFIG_MMIO
    volatile uint32_t *cfg = HW_REGS(0xFF203000u);
    uint32_t timeout = 1000000u;
    while (((cfg[1] & 2u) == 0u) && --timeout) {}
    if (!timeout) return -1;
    cfg[0] = 0u;              /* DEV = 0: on-board audio */
    cfg[2] = reg & 0x7Fu;    /* WM8731 address */
    cfg[3] = val & 0x1FFu;   /* 9-bit register payload */
    timeout = 1000000u;
    while (((cfg[1] & 2u) == 0u) && --timeout) {}
    if (!timeout || (cfg[1] & 1u)) return -1; /* ACK=0 is success */
    return 0;
#else
    (void)reg; (void)val;
    return 0; /* Not available in CPUlator; no MMIO transaction performed */
#endif
}
static int codec_init(void) {
    /* For a real board with MMIO configuration available:
     * R0/1 0dB line gain; R4 DAC selected, bypass/sidetone off;
     * R5 ADC HPF on, DAC deemphasis off. Do NOT change R7/R8 here.
     */
    if (codec_write_reg(0, 0x017)) return -1;
    if (codec_write_reg(1, 0x017)) return -1;
    if (codec_write_reg(4, 0x012)) return -1;
    if (codec_write_reg(5, 0x000)) return -1;
    return 0;
}

static void audio_init(void) {
    volatile uint32_t *a = HW_REGS(AUDIO_BASE);
    a[0] = 0x0Cu; /* clear both input/output FIFOs */
    a[0] = 0x00u; /* clear is level controlled: release */
    audio_isr();  /* prime output FIFOs before enabling IRQ */
    a[0] = 0x03u; /* RE=1, WE=1 */
}

// ============================================================================
// PS/2 Mouse Driver
// ============================================================================
static void ps2_init(void) {
    volatile uint32_t *p1 = HW_REGS(PS2_PORT1_BASE);
    volatile uint32_t *p2 = HW_REGS(PS2_PORT2_BASE);
    while (*p1 & 0x8000) { (void)*p1; }
    while (*p2 & 0x8000) { (void)*p2; }
    g_ps2_idx = 0;
}

static void ps2_poll_mouse(void) {
    volatile uint32_t *ports[2] = {
        HW_REGS(PS2_PORT2_BASE),
        HW_REGS(PS2_PORT1_BASE)
    };

    for (int p = 0; p < 2; p++) {
        volatile uint32_t *ps2 = ports[p];

        while (1) {
            uint32_t reg_val = *ps2;
            if (!(reg_val & 0x8000)) break;

            uint8_t byte = (uint8_t)(reg_val & 0xFF);
            if (g_ps2_idx == 0 && !(byte & 0x08)) continue;

            g_ps2_packet[g_ps2_idx++] = byte;

            if (g_ps2_idx >= 3) {
                g_ps2_idx = 0;

                uint8_t status = g_ps2_packet[0];
                int8_t  dx     = (int8_t)g_ps2_packet[1];
                int8_t  dy     = (int8_t)g_ps2_packet[2];

                g_mouse.left_btn  = (status & 0x01) ? 1 : 0;
                g_mouse.right_btn = (status & 0x02) ? 1 : 0;

                int new_x = g_mouse.x + dx;
                int new_y = g_mouse.y + dy;

                if (new_x < 0) new_x = 0;
                if (new_x > VGA_WIDTH - 8) new_x = VGA_WIDTH - 8;
                if (new_y < 0) new_y = 0;
                if (new_y > VGA_HEIGHT - 8) new_y = VGA_HEIGHT - 8;

                g_mouse.x = new_x;
                g_mouse.y = new_y;
            }
        }
    }
}

// ============================================================================
// Timer Driver (SysTick)
// ============================================================================
static void timer_init(void) {
    volatile uint32_t *timer = HW_REGS(TIMER0_BASE);
    const uint32_t ticks_per_ms = 100000u; /* timer clock 100MHz (100,000 cycles = 1ms) */
    timer[1] = 0; /* stop before programming */
    timer[0] = 0; /* clear pending timeout */
    timer[2] = (ticks_per_ms - 1u) & 0xFFFFu;
    timer[3] = (ticks_per_ms - 1u) >> 16;
    timer[1] = 0x7u; /* START | CONT | ITO */
}
static void timer_isr(void) {
    HW_REGS(TIMER0_BASE)[0] = 0u; /* clear TO source first */
    g_jiffies++;
}
void interrupt_handler(void) {
    uint32_t pending = __builtin_rdctl(4); /* ipending */
    if (pending & TIMER_IRQ) timer_isr();
    if (pending & AUDIO_IRQ) audio_isr();
}

// ============================================================================
// Oscilloscope GUI & Interaction Manager
// ============================================================================
static void gui_init(void) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_draw_pixel(x, y, (x >= 240) ? COLOR_DARK_GRAY : COLOR_BLACK);
        }
    }

    // 외곽 프레임
    for (int x = WAVE_LEFT; x <= WAVE_RIGHT; x++) {
        vga_draw_pixel(x, TIME_TOP, COLOR_WHITE);
        vga_draw_pixel(x, DIVIDER_Y, COLOR_WHITE);
        vga_draw_pixel(x, FFT_BOTTOM, COLOR_WHITE);
    }
    for (int y = TIME_TOP; y <= FFT_BOTTOM; y++) {
        vga_draw_pixel(WAVE_LEFT, y, COLOR_WHITE);
        vga_draw_pixel(WAVE_RIGHT, y, COLOR_WHITE);
    }

    // FFT 베이스라인
    for (int x = WAVE_LEFT + 1; x < WAVE_RIGHT; x++) {
        vga_draw_pixel(x, FFT_BASELINE, COLOR_WHITE);
    }

    // 버튼 박스 테두리 (CYAN)
    draw_box(246, 14, 276, 23, COLOR_CYAN); // [+F]
    draw_box(284, 14, 314, 23, COLOR_CYAN); // [-F]
    draw_box(246, 26, 276, 35, COLOR_CYAN); // [+A]
    draw_box(284, 26, 314, 35, COLOR_CYAN); // [-A]

    // 상단 툴바
    vga_draw_text(1, 1, "[RUN] [DAC PCM  ]  8kS/s  FFT64  Span:4kHz");

    // 우측 메뉴 텍스트
    vga_draw_text(61, 2,  "=== AUDIO GEN ===");
    vga_draw_text(63, 4,  "[+F]");
    vga_draw_text(72, 4,  "[-F]");
    vga_draw_text(63, 7,  "[+A]");
    vga_draw_text(72, 7,  "[-A]");
    vga_draw_text(61, 10, "Freq: 0.00 kHz");
    vga_draw_text(61, 12, "Amp : 0.00 V");
    vga_draw_text(61, 15, "=== SPECTRUM ===");
    vga_draw_text(61, 17, "Peak: 0.00 kHz");
    vga_draw_text(61, 19, "Span: 4.00 kHz");
    vga_draw_text(61, 22, "=== STATUS ===");
    vga_draw_text(61, 24, "Mode: DAC PCM  ");
    vga_draw_text(61, 26, "DAC : Active");

    /* Toggle between generated DAC PCM and independently sampled ADC PCM. */
    draw_box(246, 208, 314, 221, COLOR_CYAN);
    vga_draw_text(63, 53, "[SOURCE]");

    for (int i = 0; i <= (WAVE_RIGHT - WAVE_LEFT); i++) {
        g_old_wave_y[i] = TIME_CENTER_Y;
    }
    for (int b = 0; b < FFT_BINS; b++) {
        g_old_fft_h[b] = 0;
    }

    vga_save_mouse_bg(g_mouse.x, g_mouse.y);
    vga_draw_mouse_pointer(g_mouse.x, g_mouse.y);
}

static void update_source_text(void) {
    if (g_source_mode == SOURCE_DAC) {
        vga_draw_text(1, 1, "[RUN] [DAC PCM  ]  8kS/s  FFT64  Span:4kHz");
        vga_draw_text(61, 24, "Mode: DAC PCM  ");
    } else {
        vga_draw_text(1, 1, "[RUN] [ADC INPUT]  8kS/s  FFT64  Span:4kHz");
        vga_draw_text(61, 24, "Mode: ADC Input");
    }
}

static void update_hud_text(int max_bin, uint32_t max_mag, uint32_t sample_total) {
    char buf[16];

    // Freq 텍스트 갱신 (0.00 ~ 3.50 kHz)
    int f_khz100 = g_freq_hz / 10;
    buf[0] = 'F'; buf[1] = 'r'; buf[2] = 'e'; buf[3] = 'q'; buf[4] = ':'; buf[5] = ' ';
    buf[6] = '0' + (f_khz100 / 100);
    buf[7] = '.';
    buf[8] = '0' + ((f_khz100 / 10) % 10);
    buf[9] = '0' + (f_khz100 % 10);
    buf[10] = ' '; buf[11] = 'k'; buf[12] = 'H'; buf[13] = 'z'; buf[14] = '\0';
    vga_draw_text(61, 10, buf);

    // Amp 텍스트 갱신 (0.00 ~ 2.50 V)
    int a_v100 = g_amp_level * 50;
    buf[0] = 'A'; buf[1] = 'm'; buf[2] = 'p'; buf[3] = ' '; buf[4] = ':'; buf[5] = ' ';
    buf[6] = '0' + (a_v100 / 100);
    buf[7] = '.';
    buf[8] = '0' + ((a_v100 / 10) % 10);
    buf[9] = '0' + (a_v100 % 10);
    buf[10] = ' '; buf[11] = 'V'; buf[12] = ' '; buf[13] = ' '; buf[14] = '\0';
    vga_draw_text(61, 12, buf);

    // Peak 텍스트 갱신
    if (sample_total >= 64u && max_mag > 500u) {
        int peak_hz = max_bin * 125;
        int p_khz100 = peak_hz / 10;
        buf[0] = 'P'; buf[1] = 'e'; buf[2] = 'a'; buf[3] = 'k'; buf[4] = ':'; buf[5] = ' ';
        buf[6] = '0' + (p_khz100 / 100);
        buf[7] = '.';
        buf[8] = '0' + ((p_khz100 / 10) % 10);
        buf[9] = '0' + (p_khz100 % 10);
        buf[10] = ' '; buf[11] = 'k'; buf[12] = 'H'; buf[13] = 'z'; buf[14] = '\0';
        vga_draw_text(61, 17, buf);
    } else {
        vga_draw_text(61, 17, "Peak: 0.00 kHz");
    }
}

static void handle_gui_interaction(void) {
    // 단일 클릭 에지 판별
    int click = (!g_mouse.prev_left_btn && g_mouse.left_btn);

    if (click) {
        // [+F] 버튼 클릭 (246..276, 14..23)
        if (g_mouse.x >= 246 && g_mouse.x <= 276 && g_mouse.y >= 14 && g_mouse.y <= 23) {
            if (g_freq_hz < 3500) g_freq_hz += 250;
        }
        // [-F] 버튼 클릭 (284..314, 14..23)
        else if (g_mouse.x >= 284 && g_mouse.x <= 314 && g_mouse.y >= 14 && g_mouse.y <= 23) {
            if (g_freq_hz >= 250) g_freq_hz -= 250;
            else g_freq_hz = 0;
        }
        // [+A] 버튼 클릭 (246..276, 26..35)
        else if (g_mouse.x >= 246 && g_mouse.x <= 276 && g_mouse.y >= 26 && g_mouse.y <= 35) {
            if (g_amp_level < 5) g_amp_level++;
        }
        // [-A] 버튼 클릭 (284..314, 26..35)
        else if (g_mouse.x >= 284 && g_mouse.x <= 314 && g_mouse.y >= 26 && g_mouse.y <= 35) {
            if (g_amp_level > 0) g_amp_level--;
        }
        /* Bottom-right [SOURCE] button, works in CPulator and on board. */
        else if (g_mouse.x >= 246 && g_mouse.x <= 314 &&
                 g_mouse.y >= 208 && g_mouse.y <= 221) {
            g_source_mode = (g_source_mode == SOURCE_DAC) ? SOURCE_ADC : SOURCE_DAC;
            update_source_text();
        }
    }

    // 마우스 드래그 인터랙션 (<T>, V1 커서)
    if (g_mouse.left_btn) {
        if (!g_scope.is_dragging_v1 && !g_scope.is_dragging_trig) {
            if (g_mouse.x >= WAVE_RIGHT - 12 && g_mouse.x <= WAVE_RIGHT + 12 &&
                g_mouse.y >= g_scope.trigger_level_y - 8 && g_mouse.y <= g_scope.trigger_level_y + 8) {
                g_scope.is_dragging_trig = 1;
            } else if (g_mouse.x >= WAVE_LEFT && g_mouse.x <= WAVE_RIGHT &&
                       g_mouse.y >= g_scope.v1_cursor_y - 6 && g_mouse.y <= g_scope.v1_cursor_y + 6) {
                g_scope.is_dragging_v1 = 1;
            }
        }

        if (g_scope.is_dragging_trig) {
            if (g_mouse.y >= TIME_TOP + 4 && g_mouse.y <= TIME_BOTTOM - 4) {
                g_scope.trigger_level_y = g_mouse.y;
            }
        } else if (g_scope.is_dragging_v1) {
            if (g_mouse.y >= TIME_TOP + 4 && g_mouse.y <= TIME_BOTTOM - 4) {
                g_scope.v1_cursor_y = g_mouse.y;
            }
        }
    } else {
        g_scope.is_dragging_trig = 0;
        g_scope.is_dragging_v1   = 0;
    }

    g_mouse.prev_left_btn = g_mouse.left_btn;
}

static void render_frame(uint32_t frame_count) {
    (void)frame_count;
    /* Copy a coherent 221-sample snapshot of the SELECTED stream.
     * IRQs are briefly masked only for this memory copy.
     * ADC input and DAC output are fundamentally separate in CPulator. */
    int16_t samples[WAVE_RIGHT - WAVE_LEFT + 1];
    const uint32_t count = (uint32_t)(WAVE_RIGHT - WAVE_LEFT + 1);
    uint32_t saved_status = __builtin_rdctl(0);
    __builtin_wrctl(0, saved_status & ~1u);
    const volatile int16_t *src_ring = (g_source_mode == SOURCE_DAC) ? g_dac_ring : g_adc_ring;
    uint32_t total = (g_source_mode == SOURCE_DAC) ? g_dac_total : g_adc_total;
    for (uint32_t j = 0; j < count; ++j) {
        uint32_t age = count - 1u - j;
        samples[j] = (total > age) ? src_ring[(total - 1u - age) & ADC_RING_MASK] : 0;
    }
    __builtin_wrctl(0, saved_status);

    vga_restore_mouse_bg(g_mouse.prev_x, g_mouse.prev_y);

    // 1. 이전 커서 소거
    if (g_scope.prev_v1_cursor_y != g_scope.v1_cursor_y) {
        for (int x = WAVE_LEFT + 1; x < WAVE_RIGHT; x += 4) {
            vga_draw_pixel(x, g_scope.prev_v1_cursor_y, get_background_color(x, g_scope.prev_v1_cursor_y));
        }
    }
    if (g_scope.prev_trigger_y != g_scope.trigger_level_y) {
        for (int x = WAVE_LEFT + 1; x < WAVE_RIGHT; x += 8) {
            vga_draw_pixel(x, g_scope.prev_trigger_y, get_background_color(x, g_scope.prev_trigger_y));
        }
        for (int dx = 1; dx <= 5; dx++) {
            for (int dy = -3; dy <= 3; dy++) {
                vga_draw_pixel(WAVE_RIGHT + dx, g_scope.prev_trigger_y + dy, COLOR_DARK_GRAY);
            }
        }
    }

    // 2. Selected buffered PCM stream -> time waveform and FFT input.
    int32_t fft_real[64];
    int32_t fft_imag[64];
    for (int i = 0; i <= (WAVE_RIGHT - WAVE_LEFT); ++i) {
        int x = WAVE_LEFT + i;
        vga_draw_pixel(x, g_old_wave_y[i], get_background_color(x, g_old_wave_y[i]));

        int32_t pcm = samples[i];
        int y = TIME_CENTER_Y - (int)(pcm * 38 / 32768);
        if (y < TIME_TOP + 1) y = TIME_TOP + 1;
        if (y > TIME_BOTTOM - 1) y = TIME_BOTTOM - 1;
        vga_draw_pixel(x, y, COLOR_YELLOW);
        g_old_wave_y[i] = y;

        if (i < 64) {
            /* Take the newest 64 selected PCM codes; FFT accum is int32. */
            fft_real[i] = samples[(WAVE_RIGHT - WAVE_LEFT + 1) - 64 + i];
            fft_imag[i] = 0;
        }
    }

    // 3. 64-point FFT 연산 및 주파수 스펙트럼 바 갱신
    fixed_fft_64(fft_real, fft_imag);

    int max_bin = 1;
    uint32_t max_mag = 0;

    for (int b = 0; b < FFT_BINS; b++) {
        uint32_t mag = 0;
        if (total >= 64u) {
            mag = get_magnitude(fft_real[b], fft_imag[b]);
            if (b > 0 && mag > max_mag) {
                max_mag = mag;
                max_bin = b;
            }
        }

        /* Amplitude-scaled display: full-scale bin-centered sinusoid
         * has unnormalized FFT magnitude N*32768/2 at its positive bin.
         * Clip height to 70 pixels; does not hide signal-level differences.
         */
        int new_h = (int)(((uint64_t)mag * 70u) / (32u * 32768u));
        if (new_h > 70) new_h = 70;

        int x_pos = FFT_START_X + (b * FFT_BAR_PITCH);

        // 이전 막대 선택적 소거
        if (g_old_fft_h[b] > new_h) {
            for (int dy = new_h + 1; dy <= g_old_fft_h[b]; dy++) {
                for (int dx = 0; dx < FFT_BAR_WIDTH; dx++) {
                    vga_draw_pixel(x_pos + dx, FFT_BASELINE - dy, get_background_color(x_pos + dx, FFT_BASELINE - dy));
                }
            }
        }
        // 새 막대 그리기 (형광 녹색)
        else if (new_h > g_old_fft_h[b]) {
            for (int dy = g_old_fft_h[b] + 1; dy <= new_h; dy++) {
                for (int dx = 0; dx < FFT_BAR_WIDTH; dx++) {
                    vga_draw_pixel(x_pos + dx, FFT_BASELINE - dy, COLOR_GREEN);
                }
            }
        }
        g_old_fft_h[b] = new_h;
    }

    // 4. OSD 텍스트 갱신 (Freq, Amp, Peak)
    update_hud_text(max_bin, max_mag, total);

    // 5. V1 전압 커서 & 트리거 마커 그리기
    for (int x = WAVE_LEFT + 1; x < WAVE_RIGHT; x += 4) {
        vga_draw_pixel(x, g_scope.v1_cursor_y, COLOR_CYAN);
    }
    g_scope.prev_v1_cursor_y = g_scope.v1_cursor_y;

    for (int x = WAVE_LEFT + 1; x < WAVE_RIGHT; x += 8) {
        vga_draw_pixel(x, g_scope.trigger_level_y, COLOR_RED);
    }
    vga_draw_pixel(WAVE_RIGHT + 1, g_scope.trigger_level_y,     COLOR_RED);
    vga_draw_pixel(WAVE_RIGHT + 2, g_scope.trigger_level_y - 1, COLOR_RED);
    vga_draw_pixel(WAVE_RIGHT + 2, g_scope.trigger_level_y + 1, COLOR_RED);
    vga_draw_pixel(WAVE_RIGHT + 3, g_scope.trigger_level_y - 2, COLOR_RED);
    vga_draw_pixel(WAVE_RIGHT + 3, g_scope.trigger_level_y + 2, COLOR_RED);
    g_scope.prev_trigger_y = g_scope.trigger_level_y;

    // 6. 마우스 커서 렌더링
    vga_save_mouse_bg(g_mouse.x, g_mouse.y);
    vga_draw_mouse_pointer(g_mouse.x, g_mouse.y);

    g_mouse.prev_x = g_mouse.x;
    g_mouse.prev_y = g_mouse.y;
}

// ============================================================================
// Main Entry Point
// ============================================================================
int main(void) {
    /* Keep PIE disabled during peripheral and GUI initialization. */
    __builtin_wrctl(0, 0u);
    __builtin_wrctl(3, 0u);

    g_mouse.x = 160; g_mouse.y = 120;
    g_mouse.prev_x = 160; g_mouse.prev_y = 120;
    g_scope.v1_cursor_y = 40;
    g_scope.prev_v1_cursor_y = 40;
    g_scope.trigger_level_y = 64;
    g_scope.prev_trigger_y = 64;
    g_freq_hz = 0;
    g_amp_level = 0;
    g_source_mode = SOURCE_DAC;

    /* CPUlator has no config MMIO: codec_init() is a safe no-op there. */
    int codec_ok = codec_init();
    (void)codec_ok;
    ps2_init();
    gui_init();
    update_source_text();
    timer_init();
    audio_init();

    __builtin_wrctl(3, TIMER_IRQ | AUDIO_IRQ); /* ienable bits 0,6 */
    __builtin_wrctl(0, 1u);                    /* status.PIE = 1 */

    uint32_t last_render_tick = 0;
    uint32_t frame_counter = 0;
    int prev_freq = -1;

    while (1) {
        ps2_poll_mouse();
        handle_gui_interaction();
        /* Calculate DDS tuning word only when user changes frequency. */
        if (prev_freq != g_freq_hz) {
            prev_freq = g_freq_hz;
            g_dac_phase_inc = (uint32_t)(((uint64_t)(uint32_t)g_freq_hz << 32) / AUDIO_SAMPLE_RATE);
        }
        uint32_t now = g_jiffies;
        if ((uint32_t)(now - last_render_tick) >= 33u) {
            last_render_tick = now;
            render_frame(frame_counter++);
        }
        /* Audio and timer are served by the shared Nios II exception vector. */
    }
}
