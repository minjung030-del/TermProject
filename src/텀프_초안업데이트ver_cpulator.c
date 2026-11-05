#include <stdint.h>

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
static mouse_state_t     g_mouse;
static scope_state_t     g_scope;

// Generator Control States (Initial: 0)
static int g_freq_hz   = 0; // 0..3500 Hz (Step: 250 Hz)
static int g_amp_level = 0; // 0..5 (Step: 1 level = 0.5V)

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

// Q8 Fixed-Point Twiddle Factors for 64-point FFT
static const int16_t W_COS_Q8[32] = {
    256, 254, 251, 246, 236, 225, 213, 198,
    181, 163, 142, 120,  98,  74,  50,  25,
      0, -25, -50, -74, -98,-120,-142,-163,
   -181,-198,-213,-225,-236,-246,-251,-254
};
static const int16_t W_SIN_Q8[32] = {
      0,  25,  50,  74,  98, 120, 142, 163,
    181, 198, 213, 225, 236, 246, 251, 254,
    256, 254, 251, 246, 236, 225, 213, 198,
    181, 163, 142, 120,  98,  74,  50,  25
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
// Fixed-Point 64-Point FFT Engine
// ============================================================================
static void fft_bit_reverse(int16_t *real, int16_t *imag) {
    int j = 0;
    for (int i = 0; i < 63; i++) {
        if (i < j) {
            int16_t tr = real[i]; real[i] = real[j]; real[j] = tr;
            int16_t ti = imag[i]; imag[i] = imag[j]; imag[j] = ti;
        }
        int k = 32;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }
}

static void fixed_fft_64(int16_t *real, int16_t *imag) {
    fft_bit_reverse(real, imag);

    for (int len = 2; len <= 64; len <<= 1) {
        int half = len >> 1;
        int step = 64 / len;

        for (int i = 0; i < 64; i += len) {
            int k = 0;
            for (int j = 0; j < half; j++) {
                int32_t wr = W_COS_Q8[k];
                int32_t wi = W_SIN_Q8[k];

                int32_t tr = (real[i + j + half] * wr + imag[i + j + half] * wi) >> 8;
                int32_t ti = (imag[i + j + half] * wr - real[i + j + half] * wi) >> 8;

                real[i + j + half] = real[i + j] - tr;
                imag[i + j + half] = imag[i + j] - ti;
                real[i + j] += tr;
                imag[i + j] += ti;

                k += step;
            }
        }
    }
}

static inline uint16_t get_magnitude(int16_t r, int16_t i) {
    if (r < 0) r = -r;
    if (i < 0) i = -i;
    int16_t max = (r > i) ? r : i;
    int16_t min = (r > i) ? i : r;
    return (uint16_t)(max + (min >> 1));
}

// ============================================================================
// Audio Core Driver (DDS Out & Loopback In FIFO Draining)
// ============================================================================
static void audio_service(void) {
    volatile uint32_t *audio = HW_REGS(AUDIO_BASE);
    uint32_t fifospace = audio[1]; // [31:24] WSRC, [23:16] WSLC, [15:8] RARC, [7:0] RALC
    uint8_t wslc = (fifospace >> 16) & 0xFF;
    uint8_t wsrc = (fifospace >> 24) & 0xFF;
    uint8_t ralc = (fifospace >> 0) & 0xFF;

    // 1. Line-In FIFO 배출 (오버플로우 방지 및 수신 버퍼 비우기)
    while (ralc > 0) {
        (void)audio[2]; // Left Data Read
        (void)audio[3]; // Right Data Read
        ralc--;
    }

    // 2. Line-Out FIFO 송신 (DDS 신호 발생)
    int samples_to_send = (wslc < wsrc) ? wslc : wsrc;
    if (samples_to_send > 16) samples_to_send = 16;

    for (int i = 0; i < samples_to_send; i++) {
        int16_t dac_val = 0;
        if (g_freq_hz > 0 && g_amp_level > 0) {
            // DDS 위상 누적: 8000Hz 기준
            g_audio_phase += (uint32_t)g_freq_hz * 536871ULL;
            uint8_t lut_idx = (g_audio_phase >> 27) & 0x1F;
            int16_t sample_val = (SINE_LUT[lut_idx] * g_amp_level) / 5;
            dac_val = sample_val * 850; // 16-bit Full Scale 확장
        }

        audio[2] = (uint32_t)(dac_val & 0xFFFF);
        audio[3] = (uint32_t)(dac_val & 0xFFFF);
    }
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
    timer[0] = 0; // TO(Timeout) 비트 클리어
    // 100MHz 시스템 클럭 기준 1ms SysTick 주기: 100,000 사이클 = 0x186A0
    timer[2] = 0x186A0 & 0xFFFF;         // Periodl: 0x86A0
    timer[3] = (0x186A0 >> 16) & 0xFFFF; // Periodh: 0x0001
    timer[1] = 0x06; // START=1, CONT=1, ITO=0 (시뮬레이터용 Polling 구동)
}

static inline void timer_poll(void) {
    volatile uint32_t *timer = HW_REGS(TIMER0_BASE);
    if (timer[0] & 0x01) {
        timer[0] = 0;
        g_jiffies++;
    }
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
    vga_draw_text(1, 1, "[RUN]  [LINE LOOPBACK]  CH1: 500mV/div  Span: 4.0kHz");

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
    vga_draw_text(61, 24, "Mode: Line-Loop");
    vga_draw_text(61, 26, "DAC : Active");

    for (int i = 0; i <= (WAVE_RIGHT - WAVE_LEFT); i++) {
        g_old_wave_y[i] = TIME_CENTER_Y;
    }
    for (int b = 0; b < FFT_BINS; b++) {
        g_old_fft_h[b] = 0;
    }

    vga_save_mouse_bg(g_mouse.x, g_mouse.y);
    vga_draw_mouse_pointer(g_mouse.x, g_mouse.y);
}

static void update_hud_text(int max_bin, uint16_t max_mag) {
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
    if (g_amp_level > 0 && g_freq_hz > 0 && max_mag > 10) {
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

    // 2. 상단 시간축 파형 렌더링 & FFT 샘플 버퍼 구성
    int16_t fft_real[64];
    int16_t fft_imag[64];
    int step = (g_freq_hz / 250); // 8000Hz 기준 250Hz 단위 비례 스텝

    for (int i = 0; i <= (WAVE_RIGHT - WAVE_LEFT); i++) {
        int x = WAVE_LEFT + i;
        vga_draw_pixel(x, g_old_wave_y[i], get_background_color(x, g_old_wave_y[i]));

        int wave_val = 0;
        if (g_freq_hz > 0 && g_amp_level > 0) {
            int phase = ((i * step) + (frame_count << 1)) & 0x1F;
            wave_val = (SINE_LUT[phase] * g_amp_level) / 5;
        }

        int y = TIME_CENTER_Y - wave_val;
        if (y < TIME_TOP + 1) y = TIME_TOP + 1;
        if (y > TIME_BOTTOM - 1) y = TIME_BOTTOM - 1;

        vga_draw_pixel(x, y, COLOR_YELLOW);
        g_old_wave_y[i] = y;

        if (i < 64) {
            fft_real[i] = wave_val << 3;
            fft_imag[i] = 0;
        }
    }

    // 3. 64-point FFT 연산 및 주파수 스펙트럼 바 갱신
    fixed_fft_64(fft_real, fft_imag);

    int max_bin = 1;
    uint16_t max_mag = 0;

    for (int b = 0; b < FFT_BINS; b++) {
        uint16_t mag = 0;
        if (g_freq_hz > 0 && g_amp_level > 0) {
            mag = get_magnitude(fft_real[b], fft_imag[b]);
            if (b > 0 && mag > max_mag) {
                max_mag = mag;
                max_bin = b;
            }
        }

        int new_h = mag >> 4;
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
    update_hud_text(max_bin, max_mag);

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
    g_mouse.x = 160;
    g_mouse.y = 120;
    g_mouse.prev_x = 160;
    g_mouse.prev_y = 120;
    g_mouse.left_btn = 0;
    g_mouse.prev_left_btn = 0;
    g_mouse.right_btn = 0;

    g_scope.v1_cursor_y = 40;
    g_scope.prev_v1_cursor_y = 40;
    g_scope.trigger_level_y = 64;
    g_scope.prev_trigger_y = 64;
    g_scope.is_dragging_v1 = 0;
    g_scope.is_dragging_trig = 0;

    // 초깃값 0 (정지/무음 상태)
    g_freq_hz   = 0;
    g_amp_level = 0;

    timer_init();
    ps2_init();
    gui_init();

    uint32_t last_render_tick = 0;
    uint32_t frame_counter = 0;

    while (1) {
        timer_poll();
        ps2_poll_mouse();
        audio_service(); // Line Out 샘플 송신 & Line In FIFO 배출
        handle_gui_interaction();

        // 30 FPS 화면 갱신 (33ms 주기)
        if ((uint32_t)(g_jiffies - last_render_tick) >= 33) {
            last_render_tick = g_jiffies;
            render_frame(frame_counter++);
        }
    }

    return 0;
}
