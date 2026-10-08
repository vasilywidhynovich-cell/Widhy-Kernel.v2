/* Widhy OS v2 - kernel 64-bit, grafis 640x480 ala TempleOS
 * Fitur: framebuffer, font, konsol teks, IDT, PIC, timer 100Hz,
 *        keyboard (+panah, Ctrl, ESC), mouse PS/2 + pointer,
 *        disk ATA + filesystem WFS2 (persistent, bertingkat/folder),
 *        perintah: pwd cd mkdir rmdir ls cat write append cp mv rm nano
 *        REGISTER TOMBOL: tiap tombol punya id unik -> aksi berbeda
 *        WIDHY COMP v2: kompiler bahasa sendiri
 *          - variabel int/char, array, fungsi, if/else, while, for
 *          - switch/case/default, break, continue, return
 *          - literal karakter 'a' '\n'
 *          - builtin: print print_int print_char beep getkey input exit
 *                    poll cls putat putintat cursor delay key_down
 *                    gfx_clear gfx_rect gfx_frame gfx_text gfx_int
 *                    mouse_x mouse_y mouse_btn mouse_hide mouse_show
 *                    panel_save panel_restore
 *                    ui_button ui_button_hit
 *                    gfx_buf gfx_flip pal_set pal_reset
 *          - #include "file.wc" (rekursif, maks 3 level)
 *          wcc <sumber> [keluaran]  -> kompilasi -> simpan ke DISK
 *          wrun [berkas]            -> muat dari DISK lalu jalankan
 *          mario                    -> game grafis (pixel) v3
 *          paint [nama.wpg]         -> MS Paint-like, simpan .wpg
 *
 * DESAIN PENTING: handler interrupt TIDAK menggambar apa-apa.
 *
 * Mario (mario.wc) sudah ditanam langsung di berkas ini (mario_src).
 */
#include <stdint.h>
#include <time.h>
#include "speaker.h"
#include "rtc.h"
#define PIT_FREQ 1193180
#define TICK_HZ  100

#define WC_CODE      0x100000
#define WC_DATA      0x110000
#define WC_CODE_MAX  0x10000
#define WC_DATA_MAX  0x10000

/* ============================================================
 * 1. PORT I/O
 * ============================================================ */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(uint16_t port, uint16_t val){
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t r;
    __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

void speaker_play(uint32_t freq) {
    if (freq == 0) { speaker_stop(); return; }
    uint32_t div = PIT_FREQ / freq;
    if (div > 0xFFFF) div = 0xFFFF;
    if (div < 1) div = 1;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)((div >> 8) & 0xFF));
    uint8_t tmp = inb(0x61);
    if ((tmp & 3) != 3) outb(0x61, tmp | 3);
}
void speaker_stop(void) {
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp & 0xFC);
}
static void delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 100000; j++) {
            __asm__ __volatile__("nop");
        }
    }
}
void speaker_beep(uint32_t freq, uint32_t ms) {
    speaker_play(freq);
    delay_ms(ms);
    speaker_stop();
    delay_ms(20);
}

/* ============================================================
 * 2. GRAFIS
 * ============================================================ */
static volatile uint8_t *fb;
static volatile uint8_t *gd;      /* target gambar: fb atau back buffer */
#define BACKBUF 0x140000
static int pitch, width, height;
static const uint8_t *font = (const uint8_t *)0x60000;

#define CW 8
#define CH 16

enum { BLACK, BLUE, GREEN, CYAN, RED, PURPLE, BROWN, LGRAY,
       DGRAY, LBLUE, LGREEN, LCYAN, LRED, LPURPLE, YELLOW, WHITE };

static void set_palette(void) {
    static const uint8_t pal[16][3] = {
        { 0, 0, 0},{ 0, 0,42},{ 0,42, 0},{ 0,42,42},
        {42, 0, 0},{42, 0,42},{42,21, 0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},
        {63,21,21},{63,21,63},{63,63,21},{63,63,63}
    };
    for (int i = 0; i < 16; i++) {
        outb(0x3C8, i);
        outb(0x3C9, pal[i][0]);
        outb(0x3C9, pal[i][1]);
        outb(0x3C9, pal[i][2]);
    }
}
static void gfx_init(void) {
    uint8_t *mi = (uint8_t *)0x8000;
    pitch  = *(uint16_t *)(mi + 0x10);
    width  = *(uint16_t *)(mi + 0x12);
    height = *(uint16_t *)(mi + 0x14);
    fb     = (volatile uint8_t *)(uint64_t)*(uint32_t *)(mi + 0x28);
    gd     = fb;
    set_palette();
}
static void fill_rect(int x, int y, int w, int h, uint8_t color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            gd[(y + j) * pitch + x + i] = color;
}
static void draw_glyph(int px, int py, uint8_t c, uint8_t fg, uint8_t bg) {
    const uint8_t *g = font + c * CH;
    for (int r = 0; r < CH; r++) {
        uint8_t bits = g[r];
        volatile uint8_t *row = gd + (py + r) * pitch + px;
        for (int b = 0; b < CW; b++)
            row[b] = (bits & (0x80 >> b)) ? fg : bg;
    }
}
static void draw_str(int col, int row, const char *s, uint8_t fg, uint8_t bg) {
    while (*s) draw_glyph((col++) * CW, row * CH, *s++, fg, bg);
}

/* ============================================================
 * 3. KONSOL TEKS
 * ============================================================ */
static int con_x = 0, con_y = 1;
static uint8_t con_fg = BLACK, con_bg = LGREEN;

static void con_color(uint8_t fg, uint8_t bg) { con_fg = fg; con_bg = bg; }

static volatile int mouse_x = 320, mouse_y = 240, mouse_btn = 0, mouse_dirty = 1;

#define BTN_MAX  8
#define BTN_W    100
#define BTN_H    28
#define BTN_X0   520
#define BTN_Y0   48
#define BTN_GAP  8

struct button {
    uint16_t id;
    int      x, y;
    char     label[12];
    uint8_t  used;
    uint8_t  down;
};
static struct button btns[BTN_MAX];
static int btn_pressed = -1;

static int btn_find(uint16_t id) {
    for (int i = 0; i < BTN_MAX; i++)
        if (btns[i].used && btns[i].id == id) return i;
    return -1;
}
static int btn_hit(int slot, int x, int y) {
    struct button *b = &btns[slot];
    return x >= b->x && x < b->x + BTN_W && y >= b->y && y < b->y + BTN_H;
}
static void btn_draw_one(int slot) {
    struct button *b = &btns[slot];
    if (!b->used) return;
    uint8_t face = b->down ? DGRAY : LGRAY;
    uint8_t ink  = b->down ? WHITE : BLACK;
    fill_rect(b->x, b->y, BTN_W, BTN_H, BLACK);
    fill_rect(b->x + 2, b->y + 2, BTN_W - 4, BTN_H - 4, face);
    int len = 0; while (b->label[len]) len++;
    int px = b->x + (BTN_W - len * CW) / 2;
    int py = b->y + (BTN_H - CH) / 2 + (b->down ? 1 : 0);
    for (int i = 0; i < len; i++)
        draw_glyph(px + i * CW, py, (uint8_t)b->label[i], ink, face);
}
static void btn_draw_all(void) { for (int i = 0; i < BTN_MAX; i++) btn_draw_one(i); }
static void btn_erase_all(void) {
    for (int i = 0; i < BTN_MAX; i++)
        if (btns[i].used) fill_rect(btns[i].x, btns[i].y, BTN_W, BTN_H, con_bg);
}
static int btn_register(uint16_t id, const char *label) {
    if (id == 0 || btn_find(id) >= 0) return -1;
    for (int i = 0; i < BTN_MAX; i++) {
        if (!btns[i].used) {
            btns[i].id   = id;
            btns[i].x    = BTN_X0;
            btns[i].y    = BTN_Y0 + i * (BTN_H + BTN_GAP);
            btns[i].down = 0;
            int n = 0;
            while (label[n] && n < 11) { btns[i].label[n] = label[n]; n++; }
            btns[i].label[n] = 0;
            btns[i].used = 1;
            btn_draw_one(i);
            return i;
        }
    }
    return -2;
}
static int btn_unregister(uint16_t id) {
    int s = btn_find(id);
    if (s < 0) return -1;
    fill_rect(btns[s].x, btns[s].y, BTN_W, BTN_H, con_bg);
    btns[s].used = 0;
    if (btn_pressed == s) btn_pressed = -1;
    return 0;
}
static void btn_clear_all(void) {
    for (int i = 0; i < BTN_MAX; i++) { btns[i].used = 0; btns[i].down = 0; }
    btn_pressed = -1;
}
static uint16_t btn_mouse_update(void) {
    int left = mouse_btn & 1;
    uint16_t clicked = 0;
    if (btn_pressed < 0) {
        if (!left) return 0;
        for (int i = 0; i < BTN_MAX; i++)
            if (btns[i].used && btn_hit(i, mouse_x, mouse_y)) {
                btn_pressed = i;
                btns[i].down = 1;
                btn_draw_one(i);
                break;
            }
        return 0;
    }
    int s = btn_pressed;
    int inside = btn_hit(s, mouse_x, mouse_y);
    int nd = left && inside;
    if (nd != btns[s].down) { btns[s].down = nd; btn_draw_one(s); }
    if (!left) {
        if (inside) clicked = btns[s].id;
        btns[s].down = 0;
        btn_draw_one(s);
        btn_pressed = -1;
    }
    return clicked;
}
static void scroll(void) {
    btn_erase_all();
    for (int y = CH; y < height - CH; y++) {
        volatile uint64_t *dst = (volatile uint64_t *)(fb + y * pitch);
        volatile uint64_t *src = (volatile uint64_t *)(fb + (y + CH) * pitch);
        for (int i = 0; i < width / 8; i++) dst[i] = src[i];
    }
    fill_rect(0, height - CH, width, CH, con_bg);
}
static void con_putc(char c) {
    int cols = width / CW, rows = height / CH;
    if (c == '\n') { con_x = 0; con_y++; }
    else if (c == '\b') {
        if (con_x > 0) {
            con_x--;
            draw_glyph(con_x * CW, con_y * CH, ' ', con_fg, con_bg);
        }
    } else if (c == '\t') {
        for (int i = 0; i < 4; i++) con_putc(' ');
        return;
    } else {
        draw_glyph(con_x * CW, con_y * CH, (uint8_t)c, con_fg, con_bg);
        if (++con_x >= cols) { con_x = 0; con_y++; }
    }
    if (con_y >= rows) { scroll(); con_y = rows - 1; }
}
static void con_puts(const char *s) { while (*s) con_putc(*s++); }
static void con_hex(uint64_t n) {
    const char *d = "0123456789ABCDEF";
    con_puts("0x");
    int started = 0;
    for (int sh = 60; sh >= 0; sh -= 4) {
        int v = (n >> sh) & 0xF;
        if (v || started || sh == 0) { con_putc(d[v]); started = 1; }
    }
}
static void text_cursor(int on) {
    fill_rect(con_x * CW, con_y * CH + CH - 2, CW, 2, on ? con_fg : con_bg);
}

/* ============================================================
 * 4. MOUSE POINTER
 * ============================================================ */
#define CUR_W 12
#define CUR_H 16
static const char *cursor_art[CUR_H] = {
    "X           ","XX          ","X.X         ","X..X        ",
    "X...X       ","X....X      ","X.....X     ","X......X    ",
    "X.......X   ","X........X  ","X.....XXXXX ","X..X..X     ",
    "X.X X..X    ","XX  X..X    ","X    X..X   ","     XXXX   ",
};
static uint8_t mouse_bg[CUR_W * CUR_H];
static int mouse_drawn = 0, mouse_px, mouse_py;

static void mouse_hide(void) {
    if (!mouse_drawn) return;
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++) {
            int x = mouse_px + i, y = mouse_py + j;
            if (x < width && y < height)
                fb[y * pitch + x] = mouse_bg[j * CUR_W + i];
        }
    mouse_drawn = 0;
}
static void mouse_show(void) {
    mouse_px = mouse_x; mouse_py = mouse_y;
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++) {
            int x = mouse_px + i, y = mouse_py + j;
            if (x >= width || y >= height) continue;
            mouse_bg[j * CUR_W + i] = fb[y * pitch + x];
            char p = cursor_art[j][i];
            if (p == 'X')      fb[y * pitch + x] = BLACK;
            else if (p == '.') fb[y * pitch + x] = WHITE;
        }
    mouse_drawn = 1;
}
static void draw_num3(int col, int val, uint8_t fg, uint8_t bg) {
    char s[4] = { '0' + (val / 100) % 10, '0' + (val / 10) % 10, '0' + val % 10, 0 };
    draw_str(col, 0, s, fg, bg);
}
static void draw_title_bar(void) {
    int cols = width / CW;
    fill_rect(0, 0, width, CH, BLUE);
    draw_str(1, 0, "Widhy OS v2", WHITE, BLUE);
    int c = cols - 22;
    draw_str(c, 0, "X:", YELLOW, BLUE);
    draw_str(c + 6, 0, "Y:", YELLOW, BLUE);
    draw_str(c + 16, 0, "L", LGRAY, BLUE);
    draw_str(c + 18, 0, "R", LGRAY, BLUE);
}
static void update_title_status(void) {
    int c = (width / CW) - 22;
    draw_num3(c + 2, mouse_x, WHITE, BLUE);
    draw_num3(c + 8, mouse_y, WHITE, BLUE);
    draw_str(c + 16, 0, "L", (mouse_btn & 1) ? LGREEN : LGRAY, BLUE);
    draw_str(c + 18, 0, "R", (mouse_btn & 2) ? LGREEN : LGRAY, BLUE);
}

/* ============================================================
 * 5. IDT
 * ============================================================ */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));
struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry   idt[256];
static struct idt_pointer idtp;
extern uint64_t isr_stub_table[];

static void set_idt_gate(int n, uint64_t handler) {
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = 0x18;
    idt[n].ist         = 0;
    idt[n].flags       = 0x8E;
    idt[n].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[n].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[n].zero        = 0;
}
static void idt_init(void) {
    for (int i = 0; i < 48; i++) set_idt_gate(i, isr_stub_table[i]);
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtp));
}

/* ============================================================
 * 6. PIC + PIT
 * ============================================================ */
static void pic_init(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
}
static void pit_init(void) {
    uint32_t d = PIT_FREQ / TICK_HZ;
    outb(0x43, 0x36);
    outb(0x40, d & 0xFF);
    outb(0x40, (d >> 8) & 0xFF);
}

/* ============================================================
 * 7. KEYBOARD
 * ============================================================ */
enum { KEY_UP = 0x80, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
       KEY_HOME, KEY_END, KEY_DEL, KEY_PGUP, KEY_PGDN };

static const char scancode_normal[58] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
    'z','x','c','v','b','n','m',',','.','/', 0,'*', 0,' '
};
static const char scancode_shift[58] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0,'|',
    'Z','X','C','V','B','N','M','<','>','?', 0,'*', 0,' '
};
static volatile int shift_pressed = 0;
static volatile int ctrl_pressed = 0;
static int kb_ext = 0;
static volatile uint8_t key_buf[256];
static volatile uint8_t key_head = 0, key_tail = 0;
static volatile uint8_t scancode_down[128] = {0};

static void keyboard_handler(void) {
    uint8_t sc = inb(0x60);
    if (sc == 0xE0) { kb_ext = 1; return; }
    int ext = kb_ext;
    kb_ext = 0;

    if (sc & 0x80) scancode_down[sc & 0x7F] = 0;
    else           scancode_down[sc & 0x7F] = 1;

    if (sc == 0x1D) { ctrl_pressed = 1; return; }
    if (sc == 0x9D) { ctrl_pressed = 0; return; }
    if (!ext) {
        if (sc == 0x2A || sc == 0x36) { shift_pressed = 1; return; }
        if (sc == 0xAA || sc == 0xB6) { shift_pressed = 0; return; }
    }
    if (sc & 0x80) return;
    if (ext) {
        uint8_t k = 0;
        switch (sc) {
            case 0x48: k = KEY_UP;    break;
            case 0x50: k = KEY_DOWN;  break;
            case 0x4B: k = KEY_LEFT;  break;
            case 0x4D: k = KEY_RIGHT; break;
            case 0x47: k = KEY_HOME;  break;
            case 0x4F: k = KEY_END;   break;
            case 0x53: k = KEY_DEL;   break;
            case 0x49: k = KEY_PGUP;  break;
            case 0x51: k = KEY_PGDN;  break;
        }
        if (k) key_buf[key_head++] = k;
        return;
    }
    if (sc < 58) {
        char c = shift_pressed ? scancode_shift[sc] : scancode_normal[sc];
        if (!c) return;
        if (ctrl_pressed) {
            if (c >= 'a' && c <= 'z')      c = c - 'a' + 1;
            else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
            else return;
        }
        key_buf[key_head++] = (uint8_t)c;
    }
}

/* ============================================================
 * 8. MOUSE PS/2
 * ============================================================ */
static void ps2_wait_write(void) { int t = 100000; while (t-- && (inb(0x64) & 2)); }
static void ps2_wait_read(void)  { int t = 100000; while (t-- && !(inb(0x64) & 1)); }
static void mouse_write(uint8_t v) {
    ps2_wait_write(); outb(0x64, 0xD4);
    ps2_wait_write(); outb(0x60, v);
}
static uint8_t mouse_read(void) { ps2_wait_read(); return inb(0x60); }
static void mouse_init(void) {
    while (inb(0x64) & 1) inb(0x60);
    ps2_wait_write(); outb(0x64, 0xA8);
    ps2_wait_write(); outb(0x64, 0x20);
    ps2_wait_read();
    uint8_t cfg = inb(0x60);
    cfg |= 2; cfg &= ~0x20;
    ps2_wait_write(); outb(0x64, 0x60);
    ps2_wait_write(); outb(0x60, cfg);
    mouse_write(0xF6); mouse_read();
    mouse_write(0xF4); mouse_read();
}
static uint8_t m_packet[3];
static int m_cycle = 0;
static void mouse_handler(void) {
    uint8_t b = inb(0x60);
    if (m_cycle == 0 && !(b & 0x08)) return;
    m_packet[m_cycle++] = b;
    if (m_cycle < 3) return;
    m_cycle = 0;
    uint8_t st = m_packet[0];
    if (st & 0xC0) return;
    int dx = m_packet[1] - ((st << 4) & 0x100);
    int dy = m_packet[2] - ((st << 3) & 0x100);
    int nx = mouse_x + dx, ny = mouse_y - dy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > width  - 1) nx = width  - 1;
    if (ny > height - 1) ny = height - 1;
    mouse_x = nx; mouse_y = ny;
    mouse_btn = st & 7;
    mouse_dirty = 1;
}
static int strncmp_(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (uint8_t)a[i] - (uint8_t)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

/* ============================================================
 * 9. DISPATCHER INTERRUPT
 * ============================================================ */
static volatile uint64_t ticks = 0;
void interrupt_dispatch(uint64_t vector) {
    if (vector < 32) {
        gd = fb;
        con_color(WHITE, RED);
        con_puts("\n*** EXCEPTION ");
        con_hex(vector);
        con_puts(" ***");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (vector == 32) ticks++;
    else if (vector == 33) keyboard_handler();
    else if (vector == 44) mouse_handler();
    if (vector >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
static int str_equal(const char *a, const char *b){
  while (*a && *b){ if (*a != *b) return 0; a++; b++; }
  return *a == *b;
}

/* ============================================================
 * 9a. RTC
 * ============================================================ */
static int gmt_offset = 7;
static uint8_t cmos_read(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static int     cmos_updating(void)    { return cmos_read(0x0A) & 0x80; }
static uint8_t bcd_to_bin(uint8_t v)  { return (v & 0x0F) + (v >> 4) * 10; }

void rtc_read(rtc_time_t *t) {
    for (int g = 0; g < 100000 && cmos_updating(); g++);
    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hr   = cmos_read(0x04);
    uint8_t day  = cmos_read(0x07);
    uint8_t mon  = cmos_read(0x08);
    uint8_t yr   = cmos_read(0x09);
    uint8_t regB = cmos_read(0x0B);
    if (!(regB & 0x04)) {
        sec = bcd_to_bin(sec); min = bcd_to_bin(min);
        day = bcd_to_bin(day); mon = bcd_to_bin(mon); yr  = bcd_to_bin(yr);
        uint8_t pm = hr & 0x80;
        hr = bcd_to_bin(hr & 0x7F) | pm;
    }
    if (!(regB & 0x02)) {
        uint8_t pm = hr & 0x80;
        hr &= 0x7F;
        if (pm && hr < 12) hr += 12;
        if (!pm && hr == 12) hr = 0;
    }
    t->second = sec;  t->minute = min;  t->hour = hr;
    t->day = day;     t->month = mon;   t->year = 2000 + yr;
}
static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }
static int days_in_month(int m, int y) {
    static const uint8_t d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && is_leap(y)) return 29;
    return d[m - 1];
}
void rtc_read_local(rtc_time_t *t) {
    rtc_read(t);
    int h = t->hour + gmt_offset;
    int day = t->day, mon = t->month, yr = t->year;
    while (h >= 24) {
        h -= 24;
        if (++day > days_in_month(mon, yr)) { day = 1; if (++mon > 12) { mon = 1; yr++; } }
    }
    while (h < 0) {
        h += 24;
        if (--day < 1) { if (--mon < 1) { mon = 12; yr--; } day = days_in_month(mon, yr); }
    }
    t->hour  = (uint8_t)h;
    t->day   = (uint8_t)day;
    t->month = (uint8_t)mon;
    t->year  = (uint16_t)yr;
}
void rtc_set_gmt(int offset) {
    if (offset < -12) offset = -12;
    if (offset > 14)  offset = 14;
    gmt_offset = offset;
}
int rtc_get_gmt(void) { return gmt_offset; }

static void rtc_fmt(const rtc_time_t *t, char *o) {
    o[0] = '0' + t->day / 10;   o[1] = '0' + t->day % 10;   o[2] = '/';
    o[3] = '0' + t->month / 10; o[4] = '0' + t->month % 10; o[5] = '/';
    o[6] = '0' + (t->year / 1000) % 10; o[7] = '0' + (t->year / 100) % 10;
    o[8] = '0' + (t->year / 10) % 10;   o[9] = '0' + t->year % 10;
    o[10] = ' ';
    o[11] = '0' + t->hour / 10;   o[12] = '0' + t->hour % 10;   o[13] = ':';
    o[14] = '0' + t->minute / 10; o[15] = '0' + t->minute % 10; o[16] = ':';
    o[17] = '0' + t->second / 10; o[18] = '0' + t->second % 10; o[19] = 0;
}
static uint64_t clock_last = 0;
static int clock_ok = 0;
static char clock_str[20];
static void clock_tick(void) {
    if (clock_ok && ticks - clock_last < TICK_HZ / 2) return;
    clock_last = ticks;
    clock_ok = 1;
    rtc_time_t t;
    char s[20];
    rtc_read_local(&t);
    rtc_fmt(&t, s);
    int same = 1;
    for (int i = 0; i < 20; i++) if (s[i] != clock_str[i]) same = 0;
    if (same) return;
    for (int i = 0; i < 20; i++) clock_str[i] = s[i];
    mouse_hide();
    draw_str(30, 0, s, WHITE, BLUE);
    mouse_show();
}
const char *words[] = {
    "apple","banana","kernel","computer","iron","widhy","dragon",
    "rocket","space","linux","bill gates","dry","bummer","sex"
};
#define WORD_COUNT (sizeof(words) / sizeof(words[0]))
unsigned long seed = 123456789;
unsigned long random(void) { seed = seed * 1103515245 + 12345; return seed; }
void random_word(void) {
    unsigned long index = random() % WORD_COUNT;
    con_puts(words[index]);
    con_puts("\n");
}
static void shutdown(void){
   speaker_beep(200, 800);
   con_puts("Shutting down...\n");
   outw(0x604, 0x2000);
   outw(0xB004, 0x2000);
   outw(0x4004, 0x3400);
   for (;;) __asm__ volatile ("cli; hlt");
}
void widhy_output(char out[]){ con_puts(out); con_puts("\n"); }

/* ============================================================
 * 9b. DISK ATA
 * ============================================================ */
#define ATA_IO    0x1F0
#define ATA_CTRL  0x3F6
#define ATA_DRIVE 0xF0
static int ata_ok = 0;
static void ata_delay(void) { for (int i = 0; i < 4; i++) inb(ATA_CTRL); }
static int ata_wait(int need_drq) {
    for (int t = 0; t < 1000000; t++) {
        uint8_t s = inb(ATA_IO + 7);
        if (s & 0x21) return -1;
        if (!(s & 0x80)) {
            if (!need_drq) return 0;
            if (s & 0x08) return 0;
        }
    }
    return -1;
}
static void ata_init(void) {
    ata_ok = 0;
    outb(ATA_CTRL, 2);
    outb(ATA_IO + 6, ATA_DRIVE);
    ata_delay();
    outb(ATA_IO + 2, 0); outb(ATA_IO + 3, 0);
    outb(ATA_IO + 4, 0); outb(ATA_IO + 5, 0);
    outb(ATA_IO + 7, 0xEC);
    if (inb(ATA_IO + 7) == 0) return;
    for (int t = 0; t < 1000000 && (inb(ATA_IO + 7) & 0x80); t++);
    if (inb(ATA_IO + 4) || inb(ATA_IO + 5)) return;
    if (ata_wait(1) < 0) return;
    for (int i = 0; i < 256; i++) inw(ATA_IO);
    ata_ok = 1;
}
static void ata_setup(uint32_t lba, uint8_t cmd) {
    outb(ATA_IO + 6, ATA_DRIVE | ((lba >> 24) & 0x0F));
    ata_delay();
    outb(ATA_IO + 2, 1);
    outb(ATA_IO + 3, lba & 0xFF);
    outb(ATA_IO + 4, (lba >> 8) & 0xFF);
    outb(ATA_IO + 5, (lba >> 16) & 0xFF);
    outb(ATA_IO + 7, cmd);
}
static int ata_read(uint32_t lba, void *buf) {
    if (!ata_ok) return -1;
    uint16_t *p = (uint16_t *)buf;
    ata_setup(lba, 0x20);
    if (ata_wait(1) < 0) return -1;
    for (int i = 0; i < 256; i++) p[i] = inw(ATA_IO);
    return 0;
}
static int ata_write(uint32_t lba, const void *buf) {
    if (!ata_ok) return -1;
    const uint16_t *p = (const uint16_t *)buf;
    ata_setup(lba, 0x30);
    if (ata_wait(1) < 0) return -1;
    for (int i = 0; i < 256; i++) outw(ATA_IO, p[i]);
    outb(ATA_IO + 7, 0xE7);
    return ata_wait(0);
}

/* ============================================================
 * 9c. FILESYSTEM WFS2
 * ============================================================ */
#define FS_MAGIC         0x32534657u
#define FS_MAX_FILES     64
#define FS_DIR_LBA       1
#define FS_DIR_SECTORS   8
#define FS_DATA_LBA      16
#define FS_SLOT_SECTORS  32
#define FS_MAX_SIZE      (FS_SLOT_SECTORS * 512)
#define ROOT             (-1)

struct fs_entry {
    char     name[32];
    uint32_t size;
    uint8_t  used;
    uint8_t  is_dir;
    uint8_t  parent;
    uint8_t  pad[25];
} __attribute__((packed));

static struct fs_entry fs_dir[FS_MAX_FILES];
static uint8_t filebuf[FS_MAX_SIZE];
static uint8_t secbuf[512];
static int fs_ready = 0;
static int cwd = ROOT;

static void fs_zero(void *p, uint32_t n) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}
static void str_copy(char *d, const char *s) {
    int i = 0;
    while (s[i] && i < 31) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void con_dec(uint32_t n) {
    char b[11]; int i = 10; b[i] = 0;
    if (!n) { con_putc('0'); return; }
    while (n) { b[--i] = '0' + n % 10; n /= 10; }
    con_puts(b + i);
}
static int fs_sync_dir(void) {
    for (int s = 0; s < FS_DIR_SECTORS; s++)
        if (ata_write(FS_DIR_LBA + s, (uint8_t *)fs_dir + s * 512) < 0) return -1;
    return 0;
}
static void fs_mount(void) {
    fs_ready = 0; cwd = ROOT;
    if (!ata_ok) return;
    if (ata_read(0, secbuf) < 0) return;
    if (*(uint32_t *)secbuf != FS_MAGIC) return;
    for (int s = 0; s < FS_DIR_SECTORS; s++)
        if (ata_read(FS_DIR_LBA + s, (uint8_t *)fs_dir + s * 512) < 0) return;
    fs_ready = 1;
}
static int fs_format(void) {
    if (!ata_ok) return -1;
    fs_zero(secbuf, 512);
    *(uint32_t *)secbuf = FS_MAGIC;
    if (ata_write(0, secbuf) < 0) return -1;
    fs_zero(fs_dir, sizeof(fs_dir));
    if (fs_sync_dir() < 0) return -1;
    fs_ready = 1; cwd = ROOT;
    return 0;
}
static int fs_check(void) {
    if (!ata_ok)   { con_puts("Disk tidak ditemukan.\n"); return 0; }
    if (!fs_ready) { con_puts("Disk belum diformat. Ketik: format yes\n"); return 0; }
    return 1;
}
static int fs_name_ok(const char *n) {
    int l = 0;
    while (n[l]) { if (n[l] <= ' ' || n[l] > '~') return 0; l++; }
    return l > 0 && l < 32;
}
static int ent_parent(int i) { return fs_dir[i].parent == 0xFF ? ROOT : fs_dir[i].parent; }
static int fs_child(int dir, const char *name) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (fs_dir[i].used && ent_parent(i) == dir && str_equal(fs_dir[i].name, name)) return i;
    return -1;
}
static int fs_has_children(int dir) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (fs_dir[i].used && ent_parent(i) == dir) return 1;
    return 0;
}
static int is_inside(int d, int anc) {
    int n = 0;
    while (d != ROOT && n++ < 64) { if (d == anc) return 1; d = ent_parent(d); }
    return 0;
}
static int path_split(const char *p, int *dir, char *leaf) {
    int d = cwd;
    leaf[0] = 0;
    if (*p == '/') { d = ROOT; while (*p == '/') p++; }
    while (*p) {
        char comp[32];
        int n = 0;
        while (*p && *p != '/') { if (n >= 31) return -1; comp[n++] = *p++; }
        comp[n] = 0;
        while (*p == '/') p++;
        int last = (*p == 0);
        if (str_equal(comp, ".")) continue;
        if (str_equal(comp, "..")) { if (d != ROOT) d = ent_parent(d); continue; }
        if (last) { str_copy(leaf, comp); break; }
        int c = fs_child(d, comp);
        if (c < 0 || !fs_dir[c].is_dir) return -1;
        d = c;
    }
    *dir = d;
    return 0;
}
static int path_resolve(const char *p, int *idx) {
    int d; char leaf[32];
    if (path_split(p, &d, leaf) < 0) return -1;
    if (!leaf[0]) { *idx = d; return 0; }
    int c = fs_child(d, leaf);
    if (c < 0) return -1;
    *idx = c;
    return 0;
}
static void dir_path(int d, char *buf, int max) {
    int chain[32]; int n = 0, o = 0;
    while (d != ROOT && n < 32) { chain[n++] = d; d = ent_parent(d); }
    if (!n) { buf[0] = '/'; buf[1] = 0; return; }
    while (n-- > 0) {
        if (o < max - 1) buf[o++] = '/';
        for (const char *s = fs_dir[chain[n]].name; *s && o < max - 1; s++) buf[o++] = *s;
    }
    buf[o] = 0;
}
static int fs_alloc(int dir, const char *name, int is_dir) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_dir[i].used) {
            fs_zero(&fs_dir[i], sizeof(struct fs_entry));
            str_copy(fs_dir[i].name, name);
            fs_dir[i].used   = 1;
            fs_dir[i].is_dir = is_dir ? 1 : 0;
            fs_dir[i].parent = (dir == ROOT) ? 0xFF : (uint8_t)dir;
            return i;
        }
    }
    return -1;
}
static int fs_load(int idx) {
    uint32_t secs = (fs_dir[idx].size + 511) / 512;
    uint32_t base = FS_DATA_LBA + idx * FS_SLOT_SECTORS;
    if (fs_dir[idx].size > FS_MAX_SIZE) return -1;
    for (uint32_t s = 0; s < secs; s++)
        if (ata_read(base + s, filebuf + s * 512) < 0) return -1;
    return 0;
}
static int fs_store(int idx) {
    uint32_t size = fs_dir[idx].size;
    uint32_t secs = (size + 511) / 512;
    if (size > FS_MAX_SIZE) return -1;
    for (uint32_t i = size; i < secs * 512; i++) filebuf[i] = 0;
    uint32_t base = FS_DATA_LBA + idx * FS_SLOT_SECTORS;
    for (uint32_t s = 0; s < secs; s++)
        if (ata_write(base + s, filebuf + s * 512) < 0) return -1;
    return 0;
}
static int fs_write_file(int dir, const char *name, uint32_t size) {
    int idx = fs_child(dir, name);
    if (idx >= 0 && fs_dir[idx].is_dir) return -2;
    if (idx < 0) {
        idx = fs_alloc(dir, name, 0);
        if (idx < 0) return -3;
    }
    fs_dir[idx].size = size;
    if (fs_store(idx) < 0 || fs_sync_dir() < 0) return -1;
    return 0;
}
static void fs_put(int dir, const char *name, const char *text, int append) {
    if (!fs_name_ok(name)) { con_puts("Nama file tidak valid.\n"); return; }
    int idx = fs_child(dir, name);
    if (idx >= 0 && fs_dir[idx].is_dir) { con_puts("Itu direktori.\n"); return; }
    uint32_t len = 0;
    if (idx >= 0 && append) {
        if (fs_load(idx) < 0) { con_puts("Gagal membaca disk.\n"); return; }
        len = fs_dir[idx].size;
    }
    uint32_t tl = 0; while (text[tl]) tl++;
    if (len + tl + 1 > FS_MAX_SIZE) { con_puts("File terlalu besar.\n"); return; }
    for (uint32_t i = 0; i < tl; i++) filebuf[len + i] = text[i];
    len += tl;
    filebuf[len++] = '\n';
    if (idx < 0) {
        idx = fs_alloc(dir, name, 0);
        if (idx < 0) { con_puts("Tabel penuh.\n"); return; }
    }
    fs_dir[idx].size = len;
    if (fs_store(idx) < 0 || fs_sync_dir() < 0) con_puts("Gagal menulis ke disk!\n");
    else con_puts("Tersimpan.\n");
}
static int next_arg(const char **pp, char *out, int max) {
    const char *p = *pp;
    int n = 0;
    while (*p == ' ') p++;
    if (!*p) { *pp = p; out[0] = 0; return 0; }
    while (*p && *p != ' ') { if (n < max - 1) out[n++] = *p; p++; }
    out[n] = 0;
    *pp = p;
    return 1;
}
static int sb_str(char *d, int n, int max, const char *s) {
    while (*s && n < max - 1) d[n++] = *s++;
    d[n] = 0;
    return n;
}
static int sb_dec(char *d, int n, int max, uint32_t v) {
    char t[11]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = '0' + v % 10; v /= 10; }
    while (k && n < max - 1) d[n++] = t[--k];
    d[n] = 0;
    return n;
}
static void pad_puts(const char *s, int w) {
    int n = 0;
    while (*s && n < w) { con_putc(*s++); n++; }
    while (n++ < w) con_putc(' ');
}
static void pad_dec(uint32_t n, int w) {
    char b[11]; int i = 10; b[i] = 0;
    if (!n) b[--i] = '0';
    while (n) { b[--i] = '0' + n % 10; n /= 10; }
    for (int k = 10 - i; k < w; k++) con_putc(' ');
    con_puts(b + i);
}
static void ls_dash(int n) { while (n--) con_putc('-'); }
static void ls_sep(void) {
    con_putc('+'); ls_dash(34);
    con_putc('+'); ls_dash(7);
    con_putc('+'); ls_dash(12);
    con_puts("+\n");
}
static void ls_head(void) {
    ls_sep();
    con_puts("| "); pad_puts("NAMA", 32);
    con_puts(" | "); pad_puts("JENIS", 5);
    con_puts(" | "); pad_puts("UKURAN", 10);
    con_puts(" |\n");
    ls_sep();
}
static void ls_row(int i) {
    int dir = fs_dir[i].is_dir;
    uint8_t of = con_fg;
    int n = 0;
    const char *s = fs_dir[i].name;
    con_puts("| ");
    if (dir) con_fg = BLUE;
    while (*s && n < 31) { con_putc(*s++); n++; }
    if (dir) { con_putc('/'); n++; }
    con_fg = of;
    while (n++ < 32) con_putc(' ');
    con_puts(" | ");
    pad_puts(dir ? "DIR" : "FILE", 5);
    con_puts(" | ");
    if (dir) { for (int k = 0; k < 9; k++) con_putc(' '); con_putc('-'); }
    else { pad_dec(fs_dir[i].size, 8); con_puts(" B"); }
    con_puts(" |\n");
}
static void fs_rm_tree(int idx) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (fs_dir[i].used && ent_parent(i) == idx) fs_rm_tree(i);
    fs_zero(&fs_dir[idx], sizeof(struct fs_entry));
}
static void cmd_ls(const char *path) {
    int dir = cwd;
    if (path[0] && path_resolve(path, &dir) < 0) { con_puts("ls: path tidak ada.\n"); return; }
    ls_head();
    int n = 0;
    if (dir != ROOT && !fs_dir[dir].is_dir) { ls_row(dir); n = 1; }
    else {
        for (int i = 0; i < FS_MAX_FILES; i++)
            if (fs_dir[i].used && ent_parent(i) == dir) { ls_row(i); n++; }
    }
    if (!n) con_puts("| (kosong)                         |       |            |\n");
    ls_sep();
    con_dec(n); con_puts(" item\n");
}
static void cmd_mkdir(const char *p) {
    int d; char leaf[32];
    if (path_split(p, &d, leaf) < 0 || !leaf[0]) { con_puts("mkdir: path tidak valid.\n"); return; }
    if (!fs_name_ok(leaf)) { con_puts("mkdir: nama tidak valid.\n"); return; }
    if (fs_child(d, leaf) >= 0) { con_puts("mkdir: sudah ada.\n"); return; }
    if (fs_alloc(d, leaf, 1) < 0) { con_puts("Tabel penuh.\n"); return; }
    if (fs_sync_dir() < 0) con_puts("Gagal menulis ke disk!\n");
    else con_puts("Direktori dibuat.\n");
}
static void cmd_rmdir(const char *p) {
    int i;
    if (path_resolve(p, &i) < 0 || i == ROOT) { con_puts("rmdir: tidak ada.\n"); return; }
    if (!fs_dir[i].is_dir) { con_puts("rmdir: itu file, pakai rm.\n"); return; }
    if (fs_has_children(i)) { con_puts("rmdir: direktori tidak kosong.\n"); return; }
    if (is_inside(cwd, i)) { con_puts("rmdir: direktori sedang dipakai.\n"); return; }
    fs_zero(&fs_dir[i], sizeof(struct fs_entry));
    if (fs_sync_dir() < 0) con_puts("Gagal menulis ke disk!\n");
    else con_puts("Dihapus.\n");
}
static void cmd_rm(const char *p, int recursive) {
    int i;
    if (path_resolve(p, &i) < 0 || i == ROOT) { con_puts("rm: tidak ada.\n"); return; }
    if (fs_dir[i].is_dir) {
        if (!recursive) { con_puts("rm: itu direktori. Pakai rm -r.\n"); return; }
        if (is_inside(cwd, i)) { con_puts("rm: direktori sedang dipakai.\n"); return; }
        fs_rm_tree(i);
    } else {
        fs_zero(&fs_dir[i], sizeof(struct fs_entry));
    }
    if (fs_sync_dir() < 0) con_puts("Gagal menulis ke disk!\n");
    else con_puts("Dihapus.\n");
}
static int resolve_dest(const char *dst, const char *srcname, int *tdir, char *tname) {
    int d; char leaf[32];
    if (path_split(dst, &d, leaf) < 0) return -1;
    if (!leaf[0]) { *tdir = d; str_copy(tname, srcname); return 0; }
    int c = fs_child(d, leaf);
    if (c >= 0 && fs_dir[c].is_dir) { *tdir = c; str_copy(tname, srcname); return 0; }
    *tdir = d;
    str_copy(tname, leaf);
    return 0;
}
static void cmd_cp(const char *src, const char *dst) {
    int s, td, t;
    char tn[32];
    if (path_resolve(src, &s) < 0 || s == ROOT) { con_puts("cp: sumber tidak ada.\n"); return; }
    if (fs_dir[s].is_dir) { con_puts("cp: hanya file.\n"); return; }
    if (resolve_dest(dst, fs_dir[s].name, &td, tn) < 0) { con_puts("cp: tujuan tidak valid.\n"); return; }
    if (!fs_name_ok(tn)) { con_puts("cp: nama tujuan tidak valid.\n"); return; }
    t = fs_child(td, tn);
    if (t == s) { con_puts("cp: sumber dan tujuan sama.\n"); return; }
    if (t >= 0 && fs_dir[t].is_dir) { con_puts("cp: tujuan direktori.\n"); return; }
    if (fs_load(s) < 0) { con_puts("Gagal membaca disk.\n"); return; }
    uint32_t size = fs_dir[s].size;
    if (t < 0) {
        t = fs_alloc(td, tn, 0);
        if (t < 0) { con_puts("Tabel penuh.\n"); return; }
    }
    fs_dir[t].size = size;
    if (fs_store(t) < 0 || fs_sync_dir() < 0) con_puts("Gagal menulis ke disk!\n");
    else con_puts("Disalin.\n");
}
static void cmd_mv(const char *src, const char *dst) {
    int s, td;
    char tn[32];
    if (path_resolve(src, &s) < 0 || s == ROOT) { con_puts("mv: sumber tidak ada.\n"); return; }
    if (resolve_dest(dst, fs_dir[s].name, &td, tn) < 0) { con_puts("mv: tujuan tidak valid.\n"); return; }
    if (!fs_name_ok(tn)) { con_puts("mv: nama tujuan tidak valid.\n"); return; }
    if (fs_child(td, tn) >= 0) { con_puts("mv: tujuan sudah ada.\n"); return; }
    if (fs_dir[s].is_dir && is_inside(td, s)) { con_puts("mv: tidak bisa ke dalam dirinya.\n"); return; }
    fs_zero(fs_dir[s].name, 32);
    str_copy(fs_dir[s].name, tn);
    fs_dir[s].parent = (td == ROOT) ? 0xFF : (uint8_t)td;
    if (fs_sync_dir() < 0) con_puts("Gagal menulis ke disk!\n");
    else con_puts("Dipindahkan.\n");
}
static void cmd_cat(const char *p) {
    int i;
    if (path_resolve(p, &i) < 0 || i == ROOT) { con_puts("cat: file tidak ada.\n"); return; }
    if (fs_dir[i].is_dir) { con_puts("cat: itu direktori.\n"); return; }
    if (fs_load(i) < 0) { con_puts("Gagal membaca disk.\n"); return; }
    uint32_t size = fs_dir[i].size;
    for (uint32_t k = 0; k < size; k++) {
        char c = filebuf[k];
        if (c == '\n' || (c >= 32 && c < 127)) con_putc(c);
    }
    if (size && filebuf[size - 1] != '\n') con_putc('\n');
}

/* ============================================================
 * 9d. NANO EDITOR
 * ============================================================ */
static int  ed_len, ed_pos, ed_top, ed_left, ed_mod;
static int  ed_dir, ed_idx;
static char ed_name[32];
static char ed_path[320];
static char ed_clip[512];
static int  ed_clip_len = 0;

static int wait_key(void) {
    for (;;) {
        clock_tick();
        if (key_tail != key_head) return key_buf[key_tail++];
        if (mouse_dirty) {
            mouse_dirty = 0;
            mouse_hide();
            update_title_status();
            mouse_show();
        }
        __asm__ volatile ("cli");
        if (key_tail == key_head && !mouse_dirty)
            __asm__ volatile ("sti; hlt");
        else
            __asm__ volatile ("sti");
    }
}
static int ed_key(void) { int k = wait_key(); mouse_hide(); return k; }
static void ed_puts(int col, int row, const char *s, uint8_t fg, uint8_t bg, int limit) {
    while (*s && col < limit) draw_glyph((col++) * CW, row * CH, (uint8_t)*s++, fg, bg);
}
static int ed_ls(int pos) { while (pos > 0 && filebuf[pos - 1] != '\n') pos--; return pos; }
static int ed_le(int pos) { while (pos < ed_len && filebuf[pos] != '\n') pos++; return pos; }
static int ed_insert(char c) {
    if (ed_len >= FS_MAX_SIZE) return -1;
    for (int i = ed_len; i > ed_pos; i--) filebuf[i] = filebuf[i - 1];
    filebuf[ed_pos++] = (uint8_t)c;
    ed_len++;
    ed_mod = 1;
    return 0;
}
static void ed_delete_range(int from, int to) {
    int n = to - from;
    for (int i = to; i < ed_len; i++) filebuf[i - n] = filebuf[i];
    ed_len -= n;
    if (ed_pos >= to) ed_pos -= n;
    else if (ed_pos > from) ed_pos = from;
    ed_mod = 1;
}
static void ed_up(void) {
    int s = ed_ls(ed_pos);
    if (s == 0) return;
    int col = ed_pos - s;
    int ps = ed_ls(s - 1);
    int plen = (s - 1) - ps;
    ed_pos = ps + (col < plen ? col : plen);
}
static void ed_down(void) {
    int e = ed_le(ed_pos);
    if (e >= ed_len) return;
    int col = ed_pos - ed_ls(ed_pos);
    int ns = e + 1;
    int nlen = ed_le(ns) - ns;
    ed_pos = ns + (col < nlen ? col : nlen);
}
static void ed_cell(int col, int row, const char *key, const char *label) {
    ed_puts(col, row, key, BLACK, LGRAY, col + 3);
    ed_puts(col + 3, row, label, BLACK, WHITE, col + 16);
}
static void ed_bars(void) {
    int rows = height / CH;
    fill_rect(0, (rows - 2) * CH, width, 2 * CH, WHITE);
    ed_cell(0,  rows - 2, "^G", "Bantuan");
    ed_cell(16, rows - 2, "^O", "Simpan");
    ed_cell(32, rows - 2, "^X", "Keluar");
    ed_cell(48, rows - 2, "^K", "Potong");
    ed_cell(64, rows - 2, "^U", "Tempel");
    ed_cell(0,  rows - 1, "^C", "Posisi");
    ed_cell(16, rows - 1, "^A", "Awal");
    ed_cell(32, rows - 1, "^E", "Akhir");
    ed_cell(48, rows - 1, "^Y", "Hal-Atas");
    ed_cell(64, rows - 1, "^V", "Hal-Bawah");
}
static void ed_status(const char *msg) {
    int rows = height / CH, cols = width / CW;
    fill_rect(0, (rows - 3) * CH, width, CH, WHITE);
    if (!msg) return;
    char b[110];
    int n = sb_str(b, 0, sizeof b, "[ ");
    n = sb_str(b, n, sizeof b, msg);
    sb_str(b, n, sizeof b, " ]");
    ed_puts(1, rows - 3, b, BLACK, LGRAY, cols);
}
static void ed_draw(void) {
    int cols = width / CW, rows = height / CH, trows = rows - 5;
    int line = 0;
    for (int i = 0; i < ed_pos; i++) if (filebuf[i] == '\n') line++;
    int col = ed_pos - ed_ls(ed_pos);
    if (line < ed_top) ed_top = line;
    if (line >= ed_top + trows) ed_top = line - trows + 1;
    if (col < ed_left) ed_left = col;
    if (col >= ed_left + cols) ed_left = col - cols + 1;
    fill_rect(0, CH, width, CH, LGRAY);
    ed_puts(1, 1, "Widhy nano", BLACK, LGRAY, cols);
    ed_puts(13, 1, ed_path, BLACK, LGRAY, cols - 31);
    if (ed_mod) ed_puts(cols - 30, 1, "[Diubah]", RED, LGRAY, cols);
    char b[40];
    int n = sb_str(b, 0, sizeof b, "Baris ");
    n = sb_dec(b, n, sizeof b, line + 1);
    n = sb_str(b, n, sizeof b, " Kol ");
    n = sb_dec(b, n, sizeof b, col + 1);
    ed_puts(cols - n - 1, 1, b, BLACK, LGRAY, cols);
    int p = 0;
    for (int l = 0; l < ed_top && p < ed_len; l++) {
        p = ed_le(p);
        if (p < ed_len) p++;
    }
    for (int r = 0; r < trows; r++) {
        int have = (p <= ed_len);
        int e = have ? ed_le(p) : 0;
        for (int c = 0; c < cols; c++) {
            int idx = p + ed_left + c;
            uint8_t ch = ' ';
            if (have && idx < e) {
                ch = filebuf[idx];
                if (ch < 32 || ch > 126) ch = '.';
            }
            draw_glyph(c * CW, (2 + r) * CH, ch, BLACK, WHITE);
        }
        if (have) p = (e < ed_len) ? e + 1 : ed_len + 1;
    }
    uint8_t cc = ' ';
    if (ed_pos < ed_len && filebuf[ed_pos] != '\n') {
        cc = filebuf[ed_pos];
        if (cc < 32 || cc > 126) cc = '.';
    }
    draw_glyph((col - ed_left) * CW, (2 + line - ed_top) * CH, cc, WHITE, BLACK);
}
static void ed_help(void) {
    int rows = height / CH, cols = width / CW;
    static const char *t[] = {
        "WIDHY NANO - BANTUAN","",
        "Ctrl+O          Simpan berkas",
        "Ctrl+X          Keluar",
        "Ctrl+K          Potong baris",
        "Ctrl+U          Tempel",
        "Ctrl+A / Home   Awal baris",
        "Ctrl+E / End    Akhir baris",
        "Ctrl+Y / PgUp   Naik halaman",
        "Ctrl+V / PgDn   Turun halaman",
        "Ctrl+C          Posisi kursor",
        "Panah           Gerakkan kursor",
        "Backspace/Del   Hapus karakter","",
        "Tekan tombol apa saja untuk kembali...",
    };
    fill_rect(0, 2 * CH, width, (rows - 5) * CH, WHITE);
    for (int i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++)
        ed_puts(2, 3 + i, t[i], i == 0 ? BLUE : BLACK, WHITE, cols);
    ed_key();
}
static int ed_save(void) {
    if (ed_idx < 0) {
        ed_idx = fs_alloc(ed_dir, ed_name, 0);
        if (ed_idx < 0) { ed_status("Tabel penuh"); return -1; }
    }
    fs_dir[ed_idx].size = ed_len;
    if (fs_store(ed_idx) < 0 || fs_sync_dir() < 0) {
        ed_status("Gagal menulis ke disk!");
        return -1;
    }
    ed_mod = 0;
    char b[60];
    int n = sb_str(b, 0, sizeof b, "Tersimpan: ");
    n = sb_dec(b, n, sizeof b, ed_len);
    sb_str(b, n, sizeof b, " byte");
    ed_status(b);
    return 0;
}
static void editor_run(void) {
    int quit = 0;
    mouse_hide();
    fill_rect(0, CH, width, height - CH, WHITE);
    ed_bars();
    ed_status("Widhy nano - tekan Ctrl+G untuk bantuan");
    while (!quit) {
        ed_draw();
        mouse_show();
        int k = ed_key();
        ed_status(0);
        switch (k) {
        case KEY_UP:    ed_up(); break;
        case KEY_DOWN:  ed_down(); break;
        case KEY_LEFT:  if (ed_pos > 0) ed_pos--; break;
        case KEY_RIGHT: if (ed_pos < ed_len) ed_pos++; break;
        case KEY_HOME:
        case 1:         ed_pos = ed_ls(ed_pos); break;
        case KEY_END:
        case 5:         ed_pos = ed_le(ed_pos); break;
        case KEY_PGUP:
        case 25:        for (int i = 0; i < 20; i++) ed_up(); break;
        case KEY_PGDN:
        case 22:        for (int i = 0; i < 20; i++) ed_down(); break;
        case KEY_DEL:   if (ed_pos < ed_len) ed_delete_range(ed_pos, ed_pos + 1); break;
        case '\b':      if (ed_pos > 0) ed_delete_range(ed_pos - 1, ed_pos); break;
        case '\n':
            if (ed_insert('\n') < 0) ed_status("Penuh");
            break;
        case '\t':
            for (int i = 0; i < 4; i++)
                if (ed_insert(' ') < 0) { ed_status("Penuh"); break; }
            break;
        case 15:        ed_save(); break;
        case 7:         ed_help(); break;
        case 11: {
            int s = ed_ls(ed_pos), e = ed_le(ed_pos);
            if (e < ed_len) e++;
            if (e - s > (int)sizeof(ed_clip)) { ed_status("Baris terlalu panjang"); break; }
            if (e == s) { ed_status("Tidak ada yang dipotong"); break; }
            for (int i = s; i < e; i++) ed_clip[i - s] = (char)filebuf[i];
            ed_clip_len = e - s;
            ed_delete_range(s, e);
            ed_pos = s;
            ed_status("Baris dipotong");
            break;
        }
        case 21:
            if (!ed_clip_len) { ed_status("Papan klip kosong"); break; }
            for (int i = 0; i < ed_clip_len; i++)
                if (ed_insert(ed_clip[i]) < 0) { ed_status("Penuh"); break; }
            break;
        case 3: {
            int line = 1, total = 1;
            for (int i = 0; i < ed_pos; i++) if (filebuf[i] == '\n') line++;
            for (int i = 0; i < ed_len; i++) if (filebuf[i] == '\n') total++;
            char b[100];
            int n = sb_str(b, 0, sizeof b, "Baris ");
            n = sb_dec(b, n, sizeof b, line);
            n = sb_str(b, n, sizeof b, "/");
            n = sb_dec(b, n, sizeof b, total);
            n = sb_str(b, n, sizeof b, ", Kol ");
            n = sb_dec(b, n, sizeof b, ed_pos - ed_ls(ed_pos) + 1);
            n = sb_str(b, n, sizeof b, ", Kar ");
            n = sb_dec(b, n, sizeof b, ed_pos);
            n = sb_str(b, n, sizeof b, "/");
            sb_dec(b, n, sizeof b, ed_len);
            ed_status(b);
            break;
        }
        case 24:
            if (!ed_mod) { quit = 1; break; }
            ed_status("Simpan? (Y/N/Ctrl+C batal)");
            for (;;) {
                int a = ed_key();
                if (a == 'y' || a == 'Y') { if (ed_save() == 0) quit = 1; break; }
                if (a == 'n' || a == 'N') { quit = 1; break; }
                if (a == 3) { ed_status("Dibatalkan"); break; }
            }
            break;
        default:
            if (k >= 32 && k < 127)
                if (ed_insert((char)k) < 0) ed_status("Penuh");
            break;
        }
    }
    mouse_hide();
    fill_rect(0, CH, width, height - CH, con_bg);
    con_x = 0;
    con_y = 1;
}
static void editor_open(const char *path) {
    int d; char leaf[32];
    if (path_split(path, &d, leaf) < 0 || !leaf[0] || !fs_name_ok(leaf)) {
        con_puts("nano: path tidak valid.\n");
        return;
    }
    int idx = fs_child(d, leaf);
    if (idx >= 0 && fs_dir[idx].is_dir) { con_puts("nano: itu direktori.\n"); return; }
    ed_len = 0;
    if (idx >= 0) {
        if (fs_load(idx) < 0) { con_puts("Gagal membaca disk.\n"); return; }
        ed_len = fs_dir[idx].size;
        if (ed_len > FS_MAX_SIZE) ed_len = FS_MAX_SIZE;
    }
    ed_dir = d; ed_idx = idx;
    str_copy(ed_name, leaf);
    ed_pos = ed_top = ed_left = ed_mod = 0;
    char tmp[300];
    dir_path(d, tmp, sizeof tmp);
    int n = sb_str(ed_path, 0, sizeof ed_path, tmp);
    if (n > 1) n = sb_str(ed_path, n, sizeof ed_path, "/");
    sb_str(ed_path, n, sizeof ed_path, leaf);
    editor_run();
}

/* ============================================================
 * 9e. PENERJEMAH PERINTAH FILESYSTEM
 * ============================================================ */
static const char *fs_cmds[] = {
    "ls", "pwd", "cd", "mkdir", "rmdir", "rm", "cp", "mv",
    "cat", "write", "append", "nano", "edit"
};
static int is_fs_cmd(const char *c) {
    for (unsigned i = 0; i < sizeof(fs_cmds) / sizeof(fs_cmds[0]); i++)
        if (str_equal(c, fs_cmds[i])) return 1;
    return 0;
}
static int fs_command(const char *cmd) {
    char c0[16], a1[128], a2[128];
    const char *p = cmd;
    next_arg(&p, c0, sizeof c0);
    if (str_equal(c0, "format")) {
        if (!next_arg(&p, a1, sizeof a1) || !str_equal(a1, "yes"))
            con_puts("Ini menghapus SEMUA file. Ketik: format yes\n");
        else if (!ata_ok) con_puts("Disk tidak ditemukan.\n");
        else if (fs_format() < 0) con_puts("Format gagal.\n");
        else con_puts("Disk diformat.\n");
        return 1;
    }
    if (str_equal(c0, "info")) {
        con_puts("Disk: "); con_puts(ata_ok ? "OK" : "tidak ada");
        con_puts("  FS: "); con_puts(fs_ready ? "siap\n" : "belum diformat\n");
        return 1;
    }
    if (!is_fs_cmd(c0)) return 0;
    if (!fs_check()) return 1;
    if (str_equal(c0, "pwd")) {
        dir_path(cwd, a1, sizeof a1);
        con_puts(a1); con_puts("\n");
    } else if (str_equal(c0, "cd")) {
        int i;
        if (!next_arg(&p, a1, sizeof a1)) cwd = ROOT;
        else if (path_resolve(a1, &i) < 0 || (i != ROOT && !fs_dir[i].is_dir))
            con_puts("cd: direktori tidak ada.\n");
        else cwd = i;
    } else if (str_equal(c0, "ls")) {
        if (!next_arg(&p, a1, sizeof a1)) a1[0] = 0;
        cmd_ls(a1);
    } else if (str_equal(c0, "mkdir")) {
        int any = 0;
        while (next_arg(&p, a1, sizeof a1)) { cmd_mkdir(a1); any = 1; }
        if (!any) con_puts("Format: mkdir <path>\n");
    } else if (str_equal(c0, "rmdir")) {
        int any = 0;
        while (next_arg(&p, a1, sizeof a1)) { cmd_rmdir(a1); any = 1; }
        if (!any) con_puts("Format: rmdir <path>\n");
    } else if (str_equal(c0, "rm")) {
        int any = 0, rec = 0;
        while (next_arg(&p, a1, sizeof a1)) {
            if (str_equal(a1, "-r")) { rec = 1; continue; }
            cmd_rm(a1, rec);
            any = 1;
        }
        if (!any) con_puts("Format: rm [-r] <path>\n");
    } else if (str_equal(c0, "cp")) {
        if (!next_arg(&p, a1, sizeof a1) || !next_arg(&p, a2, sizeof a2))
            con_puts("Format: cp <sumber> <tujuan>\n");
        else cmd_cp(a1, a2);
    } else if (str_equal(c0, "mv")) {
        if (!next_arg(&p, a1, sizeof a1) || !next_arg(&p, a2, sizeof a2))
            con_puts("Format: mv <sumber> <tujuan>\n");
        else cmd_mv(a1, a2);
    } else if (str_equal(c0, "cat")) {
        if (!next_arg(&p, a1, sizeof a1)) con_puts("Format: cat <path>\n");
        else cmd_cat(a1);
    } else if (str_equal(c0, "write") || str_equal(c0, "append")) {
        int d; char leaf[32];
        if (!next_arg(&p, a1, sizeof a1)) { con_puts("Format: write <path> <teks>\n"); return 1; }
        while (*p == ' ') p++;
        if (path_split(a1, &d, leaf) < 0 || !leaf[0]) con_puts("Path tidak valid.\n");
        else fs_put(d, leaf, p, c0[0] == 'a');
    } else {
        if (!next_arg(&p, a1, sizeof a1)) con_puts("Format: nano <berkas>\n");
        else editor_open(a1);
    }
    return 1;
}
static void print_prompt(void) {
    char b[300];
    dir_path(cwd, b, sizeof b);
    con_color(BLUE, con_bg);
    con_puts(b);
    con_color(BLACK, con_bg);
    con_puts("> ");
}
static void btn_action(uint16_t id) {
    switch (id) {
    case 1: con_puts("Halo dari tombol 1!\n"); break;
    case 2: {
        rtc_time_t t; char s[20];
        rtc_read_local(&t); rtc_fmt(&t, s);
        con_puts("Waktu sekarang: "); con_puts(s); con_puts("\n");
        break;
    }
    case 3: speaker_beep(800, 150); con_puts("Beep!\n"); break;
    case 4: shutdown(); break;
    case 5: random_word(); break;
    case 10: con_puts("Tombol khusus nomor 10.\n"); break;
    default:
        con_puts("Tombol #"); con_dec(id); con_puts(" ditekan.\n");
        break;
    }
}
static int parse_uint(const char *s, uint32_t *v) {
    uint32_t r = 0;
    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        r = r * 10 + (*s - '0');
        s++;
    }
    *v = r;
    return 1;
}
static void cmd_button(const char *args) {
    char a[32], b[32];
    const char *p = args;
    uint32_t id;
    if (!next_arg(&p, a, sizeof a)) {
        btn_register(1, "Halo");
        btn_register(2, "Waktu");
        btn_register(3, "Bunyi");
        btn_register(4, "Shutdown");
        btn_register(5, "She Talk");
        return;
    }
    if (str_equal(a, "add")) {
        if (!next_arg(&p, a, sizeof a) || !parse_uint(a, &id) || id == 0 || id > 65535
            || !next_arg(&p, b, sizeof b)) {
            con_puts("Format: button add <id 1-65535> <label>\n");
            return;
        }
        int r = btn_register((uint16_t)id, b);
        if (r == -1)      con_puts("button: id sudah dipakai.\n");
        else if (r == -2) con_puts("button: register penuh (maks 8).\n");
        else              con_puts("Tombol didaftarkan.\n");
    } else if (str_equal(a, "del")) {
        if (!next_arg(&p, a, sizeof a) || !parse_uint(a, &id) || id > 65535) {
            con_puts("Format: button del <id>\n");
            return;
        }
        if (btn_unregister((uint16_t)id) < 0) con_puts("button: id tidak ada.\n");
        else con_puts("Tombol dihapus.\n");
    } else if (str_equal(a, "list")) {
        int n = 0;
        for (int i = 0; i < BTN_MAX; i++)
            if (btns[i].used) {
                con_puts("  #"); con_dec(btns[i].id);
                con_puts("  "); con_puts(btns[i].label); con_puts("\n");
                n++;
            }
        if (!n) con_puts("  (belum ada tombol)\n");
    } else {
        con_puts("Format: button | button add <id> <label> | button del <id> | button list\n");
    }
}

/* ============================================================
 * 9f. WIDHY COMP v2
 * ============================================================ */

static void widhy_print_int(long v) {
    if (v < 0) { con_putc('-'); v = -v; }
    char b[24]; int i = 22; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = '0' + v % 10; v /= 10; }
    con_puts(b + i);
}
static void widhy_print_char(long c) { con_putc((char)c); }
static char widhy_inbuf[256];
static char *widhy_input(void) {
    int n = 0;
    for (;;) {
        int k = wait_key();
        if (k == '\n' || k == '\r') break;
        if (k == '\b') { if (n) { n--; con_putc('\b'); } continue; }
        if (k >= 32 && k < 127 && n < 255) { widhy_inbuf[n++] = (char)k; con_putc((char)k); }
    }
    widhy_inbuf[n] = 0;
    con_putc('\n');
    return widhy_inbuf;
}
static long widhy_getkey(void) { return (long)wait_key(); }
static void widhy_beep(long f, long ms) { speaker_beep((uint32_t)f, (uint32_t)ms); }
static void widhy_exit(long c) { (void)c; }

static long widhy_poll(void) {
    if (key_tail != key_head) return (long)key_buf[key_tail++];
    return 0;
}
static void widhy_cls(void) {
    fill_rect(0, CH, width, height - CH, con_bg);
    btn_clear_all();
    con_x = 0;
    con_y = 1;
}
static void widhy_putat(long x, long y, const char *s, long color) {
    uint8_t fg = (uint8_t)(color & 0x0F);
    uint8_t bg = (uint8_t)((color >> 4) & 0x0F);
    int cx = (int)x, cy = (int)y;
    if (cy < 0 || cy >= height / CH) return;
    while (*s) {
        if (cx < 0 || cx >= width / CW) break;
        draw_glyph(cx * CW, cy * CH, (uint8_t)*s, fg, bg);
        cx++; s++;
    }
}
static void widhy_putintat(long x, long y, long v, long color) {
    char b[24]; int i = 22; b[i] = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (!v) b[--i] = '0';
    while (v) { b[--i] = '0' + (char)(v % 10); v /= 10; }
    if (neg) b[--i] = '-';
    widhy_putat(x, y, b + i, color);
}
static long widhy_cursor_state = 1;
static void widhy_cursor(long on) {
    widhy_cursor_state = on;
    text_cursor(on ? 1 : 0);
}
static void widhy_delay(long ms) {
    if (ms <= 0) return;
    uint64_t need = ((uint64_t)ms * TICK_HZ + 999) / 1000;
    if (need < 1) need = 1;
    uint64_t start = ticks;
    while ((ticks - start) < need) __asm__ volatile ("pause");
}
static long widhy_key_down(long sc) {
    int s = (int)(sc & 0x7F);
    if (s < 0 || s > 127) return 0;
    return (long)scancode_down[s];
}

static void widhy_gfx_clear(long color) {
    uint8_t c = (uint8_t)(color & 0xFF);
    for (int y = 16; y < height; y++) {
        volatile uint8_t *row = gd + y * pitch;
        for (int x = 0; x < width; x++) row[x] = c;
    }
}
static void widhy_gfx_rect(long x, long y, long w, long h, long color) {
    int px = (int)x, py = (int)y, pw = (int)w, ph = (int)h;
    uint8_t c = (uint8_t)(color & 0xFF);
    if (px < 0) { pw += px; px = 0; }
    if (py < 0) { ph += py; py = 0; }
    if (px + pw > width)  pw = width - px;
    if (py + ph > height) ph = height - py;
    if (pw <= 0 || ph <= 0) return;
    for (int j = 0; j < ph; j++) {
        volatile uint8_t *r = gd + (py + j) * pitch + px;
        for (int i = 0; i < pw; i++) r[i] = c;
    }
}
static void widhy_gfx_frame(long x, long y, long w, long h, long color) {
    widhy_gfx_rect(x, y, w, 1, color);
    widhy_gfx_rect(x, y+h-1, w, 1, color);
    widhy_gfx_rect(x, y, 1, h, color);
    widhy_gfx_rect(x+w-1, y, 1, h, color);
}
static void widhy_gfx_text(long x, long y, const char *s, long color) {
    uint8_t fg = (uint8_t)(color & 0x0F);
    uint8_t bg = (uint8_t)((color >> 4) & 0x0F);
    int px = (int)x, py = (int)y;
    if (py < 0 || py + CH > height) return;
    while (*s) {
        if (px >= 0 && px + CW <= width)
            draw_glyph(px, py, (uint8_t)*s, fg, bg);
        px += CW;
        s++;
    }
}
static void widhy_gfx_int(long x, long y, long v, long color) {
    char b[24]; int i = 22; b[i] = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (!v) b[--i] = '0';
    while (v) { b[--i] = '0' + (char)(v % 10); v /= 10; }
    if (neg) b[--i] = '-';
    widhy_gfx_text(x, y, b + i, color);
}
static long widhy_mouse_x(void)   { return (long)mouse_x; }
static long widhy_mouse_y(void)   { return (long)mouse_y; }
static long widhy_mouse_btn(void) { return (long)mouse_btn; }
static void widhy_mouse_hide_b(void){ mouse_hide(); }
static void widhy_mouse_show_b(void){ mouse_show(); }

#define PANEL_BUF 0x120000
static int panel_sx, panel_sy, panel_sw, panel_sh;
static int panel_saved = 0;
static void widhy_panel_save(long x, long y, long w, long h) {
    int px = (int)x, py = (int)y, pw = (int)w, ph = (int)h;
    if (pw <= 0 || ph <= 0) return;
    if (pw > 320 || ph > 240) return;
    if (px < 0 || py < 0 || px+pw > width || py+ph > height) return;
    uint8_t *buf = (uint8_t *)PANEL_BUF;
    for (int j = 0; j < ph; j++)
        for (int i = 0; i < pw; i++)
            buf[j*pw + i] = fb[(py+j)*pitch + px+i];
    panel_sx = px; panel_sy = py; panel_sw = pw; panel_sh = ph;
    panel_saved = 1;
}
static void widhy_panel_restore(void) {
    if (!panel_saved) return;
    uint8_t *buf = (uint8_t *)PANEL_BUF;
    for (int j = 0; j < panel_sh; j++)
        for (int i = 0; i < panel_sw; i++)
            fb[(panel_sy+j)*pitch + panel_sx+i] = buf[j*panel_sw + i];
    panel_saved = 0;
}
static void widhy_ui_button(long x, long y, long w, long h,
                            const char *label, long fg, long bg) {
    int px = (int)x, py = (int)y, pw = (int)w, ph = (int)h;
    widhy_gfx_rect(px, py, pw, ph, 0);
    widhy_gfx_rect(px+1, py+1, pw-2, ph-2, bg);
    int len = 0; while (label[len]) len++;
    int lw = len * CW;
    int lx = px + (pw - lw) / 2;
    int ly = py + (ph - CH) / 2;
    widhy_gfx_text(lx, ly, label, ((bg & 0x0F) << 4) | (fg & 0x0F));
}
static long widhy_ui_button_hit(long x, long y, long w, long h) {
    int mx = mouse_x, my = mouse_y;
    int px = (int)x, py = (int)y, pw = (int)w, ph = (int)h;
    if (mx >= px && mx < px+pw && my >= py && my < py+ph) return 1;
    return 0;
}

/* --- double buffering + palet --- */
static void widhy_gfx_buf(long on) { gd = on ? (volatile uint8_t *)BACKBUF : fb; }
static void widhy_gfx_flip(void) {
    if (gd == fb) return;
    for (int y = 16; y < height; y++) {
        volatile uint64_t *d = (volatile uint64_t *)(fb + y * pitch);
        volatile uint64_t *s = (volatile uint64_t *)(gd + y * pitch);
        for (int i = 0; i < width / 8; i++) d[i] = s[i];
    }
}
static void widhy_pal_set(long i, long r, long g, long b) {
    if (i < 0 || i > 15) return;
    outb(0x3C8, (uint8_t)i);
    outb(0x3C9, (uint8_t)(r & 63));
    outb(0x3C9, (uint8_t)(g & 63));
    outb(0x3C9, (uint8_t)(b & 63));
}
static void widhy_pal_reset(void) { set_palette(); }

struct wc_bi { const char *name; void *fn; int nargs; };
static void wc_bi_print(const char *s) { con_puts(s); }
/* URUTAN JANGAN DIUBAH: tambahkan builtin baru hanya di AKHIR tabel */
static const struct wc_bi wc_builtins[] = {
    { "print",      (void*)wc_bi_print,      1 },
    { "print_int",  (void*)widhy_print_int,  1 },
    { "print_char", (void*)widhy_print_char, 1 },
    { "beep",       (void*)widhy_beep,       2 },
    { "getkey",     (void*)widhy_getkey,     0 },
    { "input",      (void*)widhy_input,      0 },
    { "exit",       (void*)widhy_exit,       1 },
    { "poll",       (void*)widhy_poll,       0 },
    { "cls",        (void*)widhy_cls,        0 },
    { "putat",      (void*)widhy_putat,      4 },
    { "putintat",   (void*)widhy_putintat,   4 },
    { "cursor",     (void*)widhy_cursor,     1 },
    { "delay",      (void*)widhy_delay,      1 },
    { "key_down",   (void*)widhy_key_down,   1 },
    { "gfx_clear",  (void*)widhy_gfx_clear,  1 },
    { "gfx_rect",   (void*)widhy_gfx_rect,   5 },
    { "gfx_frame",  (void*)widhy_gfx_frame,  5 },
    { "gfx_text",   (void*)widhy_gfx_text,   4 },
    { "gfx_int",    (void*)widhy_gfx_int,    4 },
    { "mouse_x",    (void*)widhy_mouse_x,    0 },
    { "mouse_y",    (void*)widhy_mouse_y,    0 },
    { "mouse_btn",  (void*)widhy_mouse_btn,  0 },
    { "mouse_hide", (void*)widhy_mouse_hide_b, 0 },
    { "mouse_show", (void*)widhy_mouse_show_b, 0 },
    { "panel_save",    (void*)widhy_panel_save,    4 },
    { "panel_restore", (void*)widhy_panel_restore, 0 },
    { "ui_button",     (void*)widhy_ui_button,     7 },
    { "ui_button_hit", (void*)widhy_ui_button_hit, 4 },
    { "gfx_buf",   (void*)widhy_gfx_buf,   1 },
    { "gfx_flip",  (void*)widhy_gfx_flip,  0 },
    { "pal_set",   (void*)widhy_pal_set,   4 },
    { "pal_reset", (void*)widhy_pal_reset, 0 },
};
#define WC_NBUILTIN ((int)(sizeof(wc_builtins)/sizeof(wc_builtins[0])))
static int wc_builtin_idx(const char *n) {
    for (int i = 0; i < WC_NBUILTIN; i++)
        if (str_equal(n, wc_builtins[i].name)) return i;
    return -1;
}

#define WC_HDR       20
#define WC_MAXFN     32
#define WC_MAXREL    1024
#define WC_MAXVAR    64
#define WC_MAXLABEL  2048
#define WC_MAXJUMP   4096
#define WC_SRCBUF    16384

#define WCX_MAGIC_0 'W'
#define WCX_MAGIC_1 'C'
#define WCX_MAGIC_2 'X'
#define WCX_MAGIC_3 '2'

enum {
    T_EOF, T_ID, T_NUM, T_STR,
    T_LP, T_RP, T_LB, T_RB, T_LBRK, T_RBRK,
    T_SEMI, T_COMMA, T_COLON,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_ASSIGN, T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_AND, T_OR, T_NOT, T_ERR
};
static const char *wc_src;
static int   wc_tok, wc_line, wc_failed;
static long  wc_num;
static char  wc_text[512];
static char  wc_err[80];

static void wc_fail(const char *m) {
    if (wc_failed) return;
    wc_failed = 1;
    int n = sb_str(wc_err, 0, sizeof wc_err, "baris ");
    n = sb_dec(wc_err, n, sizeof wc_err, wc_line);
    n = sb_str(wc_err, n, sizeof wc_err, ": ");
    sb_str(wc_err, n, sizeof wc_err, m);
}
static void wc_next(void) {
    for (;;) {
        char c = *wc_src;
        if (c == '\n') { wc_line++; wc_src++; }
        else if (c==' '||c=='\t'||c=='\r') wc_src++;
        else if (c=='/' && wc_src[1]=='/') while (*wc_src && *wc_src!='\n') wc_src++;
        else if (c=='/' && wc_src[1]=='*') {
            wc_src += 2;
            while (*wc_src && !(*wc_src=='*'&&wc_src[1]=='/')) {
                if (*wc_src=='\n') wc_line++;
                wc_src++;
            }
            if (*wc_src) wc_src += 2;
        }
        else break;
    }
    char c = *wc_src;
    if (!c) { wc_tok = T_EOF; return; }
    if (c=='(') { wc_src++; wc_tok=T_LP; return; }
    if (c==')') { wc_src++; wc_tok=T_RP; return; }
    if (c=='{') { wc_src++; wc_tok=T_LB; return; }
    if (c=='}') { wc_src++; wc_tok=T_RB; return; }
    if (c=='[') { wc_src++; wc_tok=T_LBRK; return; }
    if (c==']') { wc_src++; wc_tok=T_RBRK; return; }
    if (c==';') { wc_src++; wc_tok=T_SEMI; return; }
    if (c==',') { wc_src++; wc_tok=T_COMMA; return; }
    if (c==':') { wc_src++; wc_tok=T_COLON; return; }
    if (c=='+') { wc_src++; wc_tok=T_PLUS; return; }
    if (c=='-') { wc_src++; wc_tok=T_MINUS; return; }
    if (c=='*') { wc_src++; wc_tok=T_STAR; return; }
    if (c=='/') { wc_src++; wc_tok=T_SLASH; return; }
    if (c=='%') { wc_src++; wc_tok=T_PERCENT; return; }
    if (c=='=') { wc_src++; if (*wc_src=='=') { wc_src++; wc_tok=T_EQ; } else wc_tok=T_ASSIGN; return; }
    if (c=='!') { wc_src++; if (*wc_src=='=') { wc_src++; wc_tok=T_NE; } else wc_tok=T_NOT; return; }
    if (c=='<') { wc_src++; if (*wc_src=='=') { wc_src++; wc_tok=T_LE; } else wc_tok=T_LT; return; }
    if (c=='>') { wc_src++; if (*wc_src=='=') { wc_src++; wc_tok=T_GE; } else wc_tok=T_GT; return; }
    if (c=='&') { wc_src++; if (*wc_src=='&') { wc_src++; wc_tok=T_AND; } else wc_tok=T_ERR; return; }
    if (c=='|') { wc_src++; if (*wc_src=='|') { wc_src++; wc_tok=T_OR; } else wc_tok=T_ERR; return; }
    if (c>='0' && c<='9') {
        long v = 0;
        while (*wc_src>='0' && *wc_src<='9') { v = v*10 + (*wc_src-'0'); wc_src++; }
        wc_num = v; wc_tok = T_NUM; return;
    }
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_') {
        int n = 0;
        while ((*wc_src>='a'&&*wc_src<='z')||(*wc_src>='A'&&*wc_src<='Z')||
               (*wc_src>='0'&&*wc_src<='9')||*wc_src=='_') {
            if (n < (int)sizeof wc_text - 1) wc_text[n++] = *wc_src;
            wc_src++;
        }
        wc_text[n] = 0; wc_tok = T_ID; return;
    }
    if (c=='\'') {
        wc_src++;
        char ch = *wc_src;
        if (!ch || ch=='\n') { wc_tok = T_ERR; return; }
        wc_src++;
        if (ch=='\\' && *wc_src) {
            ch = *wc_src++;
            if (ch=='n') ch='\n';
            else if (ch=='t') ch='\t';
            else if (ch=='r') ch='\r';
            else if (ch=='0') ch=0;
        }
        if (*wc_src != '\'') { wc_tok = T_ERR; return; }
        wc_src++;
        wc_num = (long)(unsigned char)ch;
        wc_tok = T_NUM;
        return;
    }
    if (c=='"') {
        wc_src++;
        int n = 0;
        while (*wc_src && *wc_src!='"') {
            char ch = *wc_src++;
            if (ch=='\\' && *wc_src) {
                ch = *wc_src++;
                if (ch=='n') ch='\n';
                else if (ch=='t') ch='\t';
                else if (ch=='r') ch='\r';
                else if (ch=='0') ch=0;
                else if (ch=='\\') ch='\\';
                else if (ch=='"') ch='"';
            }
            if (n < (int)sizeof wc_text - 1) wc_text[n++] = ch;
        }
        if (*wc_src!='"') { wc_tok = T_ERR; return; }
        wc_src++;
        wc_text[n] = 0; wc_tok = T_STR; return;
    }
    wc_src++; wc_tok = T_ERR;
}

static uint8_t *wc_code; static int wc_clen;
static char    *wc_data; static int wc_dlen;

struct wc_rel { uint32_t off; uint16_t kind; uint16_t idx; };
static struct wc_rel wc_rels[WC_MAXREL];
static int wc_nrel;

static void wc_b(uint8_t b) {
    if (wc_clen < WC_CODE_MAX) wc_code[wc_clen++] = b;
    else wc_fail("kode terlalu besar");
}
static void wc_d32(uint32_t v) { for (int i=0;i<4;i++) wc_b((uint8_t)(v>>(8*i))); }
static void wc_d64(uint64_t v) { for (int i=0;i<8;i++) wc_b((uint8_t)(v>>(8*i))); }

static void emit_call_reloc(uint16_t kind, uint16_t idx) {
    if (wc_nrel >= WC_MAXREL) { wc_fail("relokasi penuh"); return; }
    wc_b(0x48); wc_b(0xB8);
    wc_rels[wc_nrel].off  = (uint32_t)wc_clen;
    wc_rels[wc_nrel].kind = kind;
    wc_rels[wc_nrel].idx  = idx;
    wc_nrel++;
    wc_d64(0);
    wc_b(0xFF); wc_b(0xD0);
}
static void emit_prologue(void) {
    wc_b(0x55);
    wc_b(0x48); wc_b(0x89); wc_b(0xE5);
    wc_b(0x53);
}
static void emit_epilogue(void) {
    wc_b(0x48); wc_b(0x8D); wc_b(0x65); wc_b(0xF8);
    wc_b(0x5B);
    wc_b(0x5D);
    wc_b(0xC3);
}
static void emit_mov_rax_imm(uint64_t v) { wc_b(0x48); wc_b(0xB8); wc_d64(v); }
static void emit_load_local(int32_t off)  { wc_b(0x48); wc_b(0x8B); wc_b(0x85); wc_d32((uint32_t)off); }
static void emit_store_local(int32_t off) { wc_b(0x48); wc_b(0x89); wc_b(0x85); wc_d32((uint32_t)off); }
static void emit_lea_local(int32_t off)   { wc_b(0x48); wc_b(0x8D); wc_b(0x85); wc_d32((uint32_t)off); }
static void emit_push_rax(void) { wc_b(0x50); }

static void emit_add_rax_rcx(void) { wc_b(0x48);wc_b(0x01);wc_b(0xC8); }
static void emit_sub_rax_rcx(void) { wc_b(0x48);wc_b(0x29);wc_b(0xC8); }
static void emit_imul_rax_rcx(void){ wc_b(0x48);wc_b(0x0F);wc_b(0xAF);wc_b(0xC1); }
static void emit_idiv_rcx(void)    { wc_b(0x48);wc_b(0x99);wc_b(0x48);wc_b(0xF7);wc_b(0xF9); }
static void emit_cmp_rax_rcx(void) { wc_b(0x48);wc_b(0x39);wc_b(0xC8); }

static void emit_setcc(uint8_t cc) {
    wc_b(0x0F); wc_b(cc); wc_b(0xC0);
    wc_b(0x48); wc_b(0x0F); wc_b(0xB6); wc_b(0xC0);
}
#define SCC_E  0x94
#define SCC_NE 0x95
#define SCC_L  0x9C
#define SCC_LE 0x9E
#define SCC_G  0x9F
#define SCC_GE 0x9D
#define JCC_E  0x84
#define JCC_NE 0x85
#define JCC_L  0x8C
#define JCC_LE 0x8E
#define JCC_G  0x8F
#define JCC_GE 0x8D

static uint32_t wc_labels[WC_MAXLABEL];
static int wc_nlabel;
static struct { uint32_t off; int label; } wc_jumps[WC_MAXJUMP];
static int wc_njump;

static int lab_new(void) {
    if (wc_nlabel >= WC_MAXLABEL) { wc_fail("label penuh"); return 0; }
    wc_labels[wc_nlabel] = 0xFFFFFFFF;
    return wc_nlabel++;
}
static void lab_place(int id) { wc_labels[id] = (uint32_t)wc_clen; }
static void emit_jmp(int label) {
    if (wc_njump >= WC_MAXJUMP) { wc_fail("jump penuh"); return; }
    wc_b(0xE9); wc_d32(0);
    wc_jumps[wc_njump].off = (uint32_t)(wc_clen - 4);
    wc_jumps[wc_njump].label = label;
    wc_njump++;
}
static void emit_jcc(uint8_t cc, int label) {
    if (wc_njump >= WC_MAXJUMP) { wc_fail("jump penuh"); return; }
    wc_b(0x0F); wc_b(cc); wc_d32(0);
    wc_jumps[wc_njump].off = (uint32_t)(wc_clen - 4);
    wc_jumps[wc_njump].label = label;
    wc_njump++;
}
static void resolve_jumps(void) {
    for (int i = 0; i < wc_njump; i++) {
        uint32_t t = wc_labels[wc_jumps[i].label];
        if (t == 0xFFFFFFFF) { wc_fail("label tak terdefinisi"); return; }
        int32_t rel = (int32_t)t - (int32_t)(wc_jumps[i].off + 4);
        for (int k = 0; k < 4; k++)
            wc_code[wc_jumps[i].off + k] = (uint8_t)(rel >> (8*k));
    }
    wc_njump = 0;
    wc_nlabel = 0;
}

struct wc_var {
    char name[32];
    int  off;
    int  is_array;
    int  array_size;
    int  is_global;
};
static struct wc_var wc_vars[WC_MAXVAR];
static int wc_nvar;
static int wc_frame;

static struct wc_fn { char name[32]; int off; int nparams; } wc_fns[WC_MAXFN];
static int wc_nfn;

static int find_var(const char *n) {
    for (int i = wc_nvar - 1; i >= 0; i--)
        if (str_equal(wc_vars[i].name, n)) return i;
    return -1;
}
static int find_fn(const char *n) {
    for (int i = 0; i < wc_nfn; i++)
        if (str_equal(wc_fns[i].name, n)) return i;
    return -1;
}
static int alloc_var(const char *n, int is_array, int size) {
    if (wc_nvar >= WC_MAXVAR) { wc_fail("variabel penuh"); return -1; }
    int sz = is_array ? size * 8 : 8;
    wc_frame += sz;
    wc_frame = (wc_frame + 7) & ~7;
    struct wc_var *v = &wc_vars[wc_nvar];
    str_copy(v->name, n);
    v->off = -wc_frame;
    v->is_array = is_array;
    v->array_size = size;
    v->is_global = 0;
    return wc_nvar++;
}

static int wc_loop_depth;
static int wc_break_lab[16];
static int wc_cont_lab[16];

static void parse_expr(void);
static void parse_stmt(void);

static int is_type_kw(const char *s) {
    return str_equal(s, "int") || str_equal(s, "char") ||
           str_equal(s, "void") || str_equal(s, "long");
}
static int is_type_tok(void) { return wc_tok == T_ID && is_type_kw(wc_text); }

static void parse_primary(void) {
    if (wc_tok == T_NUM) {
        emit_mov_rax_imm((uint64_t)wc_num);
        wc_next();
        return;
    }
    if (wc_tok == T_STR) {
        char *addr = wc_data + wc_dlen;
        int n = 0; while (wc_text[n]) n++;
        if (wc_dlen + n + 1 > WC_DATA_MAX) { wc_fail("data penuh"); return; }
        for (int i = 0; i <= n; i++) addr[i] = wc_text[i];
        wc_dlen += n + 1;
        emit_mov_rax_imm((uint64_t)addr);
        wc_next();
        return;
    }
    if (wc_tok == T_LP) {
        wc_next(); parse_expr();
        if (wc_tok != T_RP) { wc_fail("')' diharapkan"); return; }
        wc_next();
        return;
    }
    if (wc_tok == T_ID) {
        char name[32]; str_copy(name, wc_text);
        wc_next();
        if (wc_tok == T_LP) {
            wc_next();
            int bi = wc_builtin_idx(name);
            int fi = find_fn(name);
            if (bi < 0 && fi < 0) { wc_fail("fungsi tak dikenal"); return; }
            int nargs = 0;
            if (wc_tok != T_RP) {
                for (;;) {
                    parse_expr();
                    emit_push_rax();
                    nargs++;
                    if (nargs > 6) { wc_fail("argumen maks 6"); return; }
                    if (wc_tok == T_COMMA) { wc_next(); continue; }
                    break;
                }
            }
            if (wc_tok != T_RP) { wc_fail("')' diharapkan"); return; }
            wc_next();
            for (int i = nargs - 1; i >= 0; i--) {
                switch (i) {
                    case 0: wc_b(0x5F); break;
                    case 1: wc_b(0x5E); break;
                    case 2: wc_b(0x5A); break;
                    case 3: wc_b(0x59); break;
                    case 4: wc_b(0x41); wc_b(0x58); break;
                    case 5: wc_b(0x41); wc_b(0x59); break;
                }
            }
            if (bi >= 0)      emit_call_reloc(0, (uint16_t)bi);
            else              emit_call_reloc(1, (uint16_t)wc_fns[fi].off);
            return;
        }
        int vi = find_var(name);
        if (vi >= 0 && wc_tok == T_LBRK) {
            wc_next();
            parse_expr();
            if (wc_tok != T_RBRK) { wc_fail("']' diharapkan"); return; }
            wc_next();
            wc_b(0x48); wc_b(0xC1); wc_b(0xE0); wc_b(0x03);
            wc_b(0x4C); wc_b(0x8D); wc_b(0x9D);
            wc_d32((uint32_t)wc_vars[vi].off);
            wc_b(0x4C); wc_b(0x01); wc_b(0xD8);
            wc_b(0x48); wc_b(0x8B); wc_b(0x00);
            return;
        }
        if (vi < 0) { wc_fail("variabel tak dikenal"); return; }
        if (wc_vars[vi].is_array) { emit_lea_local(wc_vars[vi].off); return; }
        emit_load_local(wc_vars[vi].off);
        return;
    }
    wc_fail("ekspresi tidak valid");
}
static void parse_unary(void) {
    if (wc_tok == T_MINUS) {
        wc_next(); parse_unary();
        wc_b(0x48); wc_b(0xF7); wc_b(0xD8);
        return;
    }
    if (wc_tok == T_NOT) {
        wc_next(); parse_unary();
        wc_b(0x48); wc_b(0x85); wc_b(0xC0);
        emit_setcc(SCC_E);
        return;
    }
    parse_primary();
}
static void finish_binop(int op) {
    wc_b(0x48); wc_b(0x89); wc_b(0xC1);
    wc_b(0x58);
    switch (op) {
        case T_PLUS:    emit_add_rax_rcx(); break;
        case T_MINUS:   emit_sub_rax_rcx(); break;
        case T_STAR:    emit_imul_rax_rcx(); break;
        case T_SLASH:   emit_idiv_rcx(); break;
        case T_PERCENT: emit_idiv_rcx(); wc_b(0x48); wc_b(0x89); wc_b(0xD0); break;
    }
}
static void finish_cmp(int op) {
    wc_b(0x48); wc_b(0x89); wc_b(0xC1);
    wc_b(0x58);
    emit_cmp_rax_rcx();
    uint8_t cc = SCC_E;
    if (op==T_EQ) cc=SCC_E; else if (op==T_NE) cc=SCC_NE;
    else if (op==T_LT) cc=SCC_L; else if (op==T_GT) cc=SCC_G;
    else if (op==T_LE) cc=SCC_LE; else cc=SCC_GE;
    emit_setcc(cc);
}
static void parse_mul(void) {
    parse_unary();
    while (wc_tok==T_STAR || wc_tok==T_SLASH || wc_tok==T_PERCENT) {
        int op = wc_tok; wc_next();
        emit_push_rax();
        parse_unary();
        finish_binop(op);
    }
}
static void parse_add(void) {
    parse_mul();
    while (wc_tok==T_PLUS || wc_tok==T_MINUS) {
        int op = wc_tok; wc_next();
        emit_push_rax();
        parse_mul();
        finish_binop(op);
    }
}
static void parse_cmp(void) {
    parse_add();
    while (wc_tok==T_EQ||wc_tok==T_NE||wc_tok==T_LT||wc_tok==T_GT||
           wc_tok==T_LE||wc_tok==T_GE) {
        int op = wc_tok; wc_next();
        emit_push_rax();
        parse_add();
        finish_cmp(op);
    }
}
static void parse_and(void) {
    parse_cmp();
    while (wc_tok == T_AND) {
        wc_next();
        int lbl = lab_new();
        wc_b(0x48); wc_b(0x85); wc_b(0xC0);
        emit_jcc(JCC_E, lbl);
        parse_cmp();
        wc_b(0x48); wc_b(0x85); wc_b(0xC0);
        emit_setcc(SCC_NE);
        lab_place(lbl);
    }
}
static void parse_or(void) {
    parse_and();
    while (wc_tok == T_OR) {
        wc_next();
        int lbl = lab_new();
        wc_b(0x48); wc_b(0x85); wc_b(0xC0);
        emit_jcc(JCC_NE, lbl);
        parse_and();
        wc_b(0x48); wc_b(0x85); wc_b(0xC0);
        emit_setcc(SCC_NE);
        lab_place(lbl);
    }
}
static void parse_expr(void) { parse_or(); }

static int parse_local_decl(void) {
    if (wc_tok != T_ID) { wc_fail("nama variabel diharapkan"); return 0; }
    char name[32]; str_copy(name, wc_text);
    wc_next();
    if (wc_tok == T_LBRK) {
        wc_next();
        if (wc_tok != T_NUM) { wc_fail("ukuran array harus angka"); return 0; }
        int sz = (int)wc_num;
        wc_next();
        if (wc_tok != T_RBRK) { wc_fail("']' diharapkan"); return 0; }
        wc_next();
        int vi = alloc_var(name, 1, sz);
        if (wc_tok == T_ASSIGN) {
            wc_next(); parse_expr();
            wc_b(0x49); wc_b(0x89); wc_b(0xC3);
            wc_b(0x48); wc_b(0x8D); wc_b(0x85);
            wc_d32((uint32_t)wc_vars[vi].off);
            wc_b(0x4C); wc_b(0x89); wc_b(0x18);
        }
    } else if (wc_tok == T_ASSIGN) {
        wc_next(); parse_expr();
        int vi = alloc_var(name, 0, 1);
        emit_store_local(wc_vars[vi].off);
    } else {
        alloc_var(name, 0, 1);
    }
    if (wc_tok != T_SEMI) { wc_fail("';' diharapkan"); return 0; }
    wc_next();
    return 1;
}
static void parse_assign_or_expr(void) {
    if (wc_tok == T_ID) {
        char save[512]; str_copy(save, wc_text);
        const char *save_src = wc_src; int save_line = wc_line;
        int save_tok = wc_tok;
        wc_next();
        int is_lval = 0, idx = -1;
        if (wc_tok == T_ASSIGN) {
            idx = find_var(save);
            if (idx >= 0 && !wc_vars[idx].is_array) is_lval = 1;
        } else if (wc_tok == T_LBRK) {
            int depth = 1; wc_next();
            while (wc_tok != T_EOF && depth > 0) {
                if (wc_tok == T_LBRK) depth++;
                else if (wc_tok == T_RBRK) depth--;
                if (depth > 0) wc_next();
            }
            if (depth == 0) {
                wc_next();
                if (wc_tok == T_ASSIGN) {
                    idx = find_var(save);
                    if (idx >= 0 && wc_vars[idx].is_array) is_lval = 2;
                }
            }
        }
        if (!is_lval) {
            str_copy(wc_text, save);
            wc_src = save_src; wc_line = save_line; wc_tok = save_tok;
            parse_expr();
            return;
        }
        if (is_lval == 1) {
            wc_next();
            parse_expr();
            emit_store_local(wc_vars[idx].off);
            return;
        }
        str_copy(wc_text, save);
        wc_src = save_src; wc_line = save_line; wc_tok = save_tok;
        wc_next();
        wc_next();
        parse_expr();
        wc_b(0x48); wc_b(0xC1); wc_b(0xE0); wc_b(0x03);
        wc_b(0x4C); wc_b(0x8D); wc_b(0x9D);
        wc_d32((uint32_t)wc_vars[idx].off);
        wc_b(0x4C); wc_b(0x01); wc_b(0xD8);
        wc_b(0x50);
        if (wc_tok != T_RBRK) { wc_fail("']' diharapkan"); return; }
        wc_next();
        if (wc_tok != T_ASSIGN) { wc_fail("'=' diharapkan"); return; }
        wc_next();
        parse_expr();
        wc_b(0x41); wc_b(0x5A);
        wc_b(0x49); wc_b(0x89); wc_b(0x02);
        return;
    }
    parse_expr();
}
static void parse_block(void) {
    if (wc_tok != T_LB) { wc_fail("'{' diharapkan"); return; }
    wc_next();
    while (wc_tok != T_RB && wc_tok != T_EOF) {
        parse_stmt();
        if (wc_failed) return;
    }
    if (wc_tok != T_RB) { wc_fail("'}' diharapkan"); return; }
    wc_next();
}
static void parse_if(void) {
    wc_next();
    if (wc_tok != T_LP) { wc_fail("'(' diharapkan"); return; }
    wc_next();
    parse_expr();
    if (wc_tok != T_RP) { wc_fail("')' diharapkan"); return; }
    wc_next();
    int lbl_else = lab_new(), lbl_end = lab_new();
    wc_b(0x48); wc_b(0x85); wc_b(0xC0);
    emit_jcc(JCC_E, lbl_else);
    parse_stmt();
    if (wc_tok == T_ID && str_equal(wc_text, "else")) {
        emit_jmp(lbl_end);
        lab_place(lbl_else);
        wc_next();
        parse_stmt();
    } else {
        lab_place(lbl_else);
    }
    lab_place(lbl_end);
}
static void parse_while(void) {
    wc_next();
    if (wc_tok != T_LP) { wc_fail("'(' diharapkan"); return; }
    wc_next();
    int lbl_top = lab_new(), lbl_end = lab_new();
    lab_place(lbl_top);
    parse_expr();
    if (wc_tok != T_RP) { wc_fail("')' diharapkan"); return; }
    wc_next();
    wc_b(0x48); wc_b(0x85); wc_b(0xC0);
    emit_jcc(JCC_E, lbl_end);
    if (wc_loop_depth < 16) {
        wc_break_lab[wc_loop_depth] = lbl_end;
        wc_cont_lab[wc_loop_depth]  = lbl_top;
        wc_loop_depth++;
    }
    parse_stmt();
    if (wc_loop_depth > 0) wc_loop_depth--;
    emit_jmp(lbl_top);
    lab_place(lbl_end);
}
static void parse_for(void) {
    wc_next();
    if (wc_tok != T_LP) { wc_fail("'(' diharapkan"); return; }
    wc_next();
    if (wc_tok != T_SEMI) {
        if (is_type_tok()) { wc_next(); parse_local_decl(); }
        else { parse_assign_or_expr(); if (wc_tok != T_SEMI) { wc_fail("';' diharapkan"); return; } wc_next(); }
    } else wc_next();
    int lbl_incr = lab_new(), lbl_cond = lab_new(), lbl_end = lab_new();
    emit_jmp(lbl_cond);
    lab_place(lbl_incr);
    if (wc_tok != T_SEMI) parse_expr();
    if (wc_tok != T_SEMI) { wc_fail("';' diharapkan"); return; }
    wc_next();
    lab_place(lbl_cond);
    if (wc_tok != T_RP) {
        parse_expr();
        wc_b(0x48); wc_b(0x85); wc_b(0xC0);
        emit_jcc(JCC_E, lbl_end);
    }
    if (wc_tok != T_RP) { wc_fail("')' diharapkan"); return; }
    wc_next();
    if (wc_loop_depth < 16) {
        wc_break_lab[wc_loop_depth] = lbl_end;
        wc_cont_lab[wc_loop_depth]  = lbl_incr;
        wc_loop_depth++;
    }
    parse_stmt();
    if (wc_loop_depth > 0) wc_loop_depth--;
    emit_jmp(lbl_incr);
    lab_place(lbl_end);
}
static void parse_switch(void) {
    wc_next();
    if (wc_tok != T_LP) { wc_fail("'(' diharapkan"); return; }
    wc_next();
    parse_expr();
    wc_b(0x48); wc_b(0x89); wc_b(0xC3);
    if (wc_tok != T_RP) { wc_fail("')' diharapkan"); return; }
    wc_next();
    if (wc_tok != T_LB) { wc_fail("'{' diharapkan"); return; }
    wc_next();
    int lbl_end = lab_new(), lbl_dispatch = lab_new();
    emit_jmp(lbl_dispatch);
    struct sw_case { long val; int lbl; };
    struct sw_case sw[32];
    int nsw = 0;
    int default_lbl = -1;
    if (wc_loop_depth < 16) {
        wc_break_lab[wc_loop_depth] = lbl_end;
        wc_cont_lab[wc_loop_depth]  = lbl_end;
        wc_loop_depth++;
    }
    while (wc_tok != T_RB && wc_tok != T_EOF) {
        if (wc_tok == T_ID && str_equal(wc_text, "case")) {
            wc_next();
            int neg = 0;
            if (wc_tok == T_MINUS) { neg = 1; wc_next(); }
            if (wc_tok != T_NUM) { wc_fail("angka case diharapkan"); return; }
            long v = wc_num; if (neg) v = -v;
            wc_next();
            if (wc_tok != T_COLON) { wc_fail("':' diharapkan"); return; }
            wc_next();
            int lbl = lab_new();
            lab_place(lbl);
            if (nsw < 32) { sw[nsw].val = v; sw[nsw].lbl = lbl; nsw++; }
        } else if (wc_tok == T_ID && str_equal(wc_text, "default")) {
            wc_next();
            if (wc_tok != T_COLON) { wc_fail("':' diharapkan"); return; }
            wc_next();
            default_lbl = lab_new();
            lab_place(default_lbl);
        } else {
            parse_stmt();
            if (wc_failed) return;
        }
    }
    if (wc_tok != T_RB) { wc_fail("'}' diharapkan"); return; }
    wc_next();
    emit_jmp(lbl_end);
    lab_place(lbl_dispatch);
    for (int i = 0; i < nsw; i++) {
        wc_b(0x48); wc_b(0x81); wc_b(0xFB); wc_d32((uint32_t)sw[i].val);
        emit_jcc(JCC_E, sw[i].lbl);
    }
    if (default_lbl >= 0) emit_jmp(default_lbl);
    else                  emit_jmp(lbl_end);
    lab_place(lbl_end);
    if (wc_loop_depth > 0) wc_loop_depth--;
}
static void parse_stmt(void) {
    if (wc_failed) return;
    if (wc_tok == T_LB) { parse_block(); return; }
    if (wc_tok == T_SEMI) { wc_next(); return; }
    if (is_type_tok()) { wc_next(); parse_local_decl(); return; }
    if (wc_tok == T_ID) {
        if (str_equal(wc_text, "if"))     { parse_if(); return; }
        if (str_equal(wc_text, "while"))  { parse_while(); return; }
        if (str_equal(wc_text, "for"))    { parse_for(); return; }
        if (str_equal(wc_text, "switch")) { parse_switch(); return; }
        if (str_equal(wc_text, "break")) {
            wc_next();
            if (wc_tok != T_SEMI) { wc_fail("';' diharapkan"); return; }
            wc_next();
            if (wc_loop_depth > 0) emit_jmp(wc_break_lab[wc_loop_depth-1]);
            return;
        }
        if (str_equal(wc_text, "continue")) {
            wc_next();
            if (wc_tok != T_SEMI) { wc_fail("';' diharapkan"); return; }
            wc_next();
            if (wc_loop_depth > 0) emit_jmp(wc_cont_lab[wc_loop_depth-1]);
            return;
        }
        if (str_equal(wc_text, "return")) {
            wc_next();
            if (wc_tok != T_SEMI) parse_expr();
            if (wc_tok != T_SEMI) { wc_fail("';' diharapkan"); return; }
            wc_next();
            emit_epilogue();
            return;
        }
    }
    parse_assign_or_expr();
    if (wc_tok != T_SEMI) { wc_fail("';' diharapkan"); return; }
    wc_next();
}
static void parse_function(void) {
    wc_next();
    if (wc_tok != T_ID) { wc_fail("nama fungsi diharapkan"); return; }
    if (wc_nfn >= WC_MAXFN) { wc_fail("fungsi penuh"); return; }
    str_copy(wc_fns[wc_nfn].name, wc_text);
    wc_next();
    if (wc_tok != T_LP) { wc_fail("'(' diharapkan"); return; }
    wc_next();
    wc_frame = 8;
    wc_nvar  = 0;
    wc_fns[wc_nfn].off = wc_clen;
    emit_prologue();
    wc_b(0x48); wc_b(0x81); wc_b(0xEC); wc_d32(0);
    int sub_patch = wc_clen - 4;
    int nparams = 0;
    if (wc_tok != T_RP) {
        for (;;) {
            if (!is_type_tok()) { wc_fail("tipe parameter diharapkan"); return; }
            wc_next();
            if (wc_tok != T_ID) { wc_fail("nama parameter diharapkan"); return; }
            int vi = alloc_var(wc_text, 0, 1);
            if      (nparams == 0) { wc_b(0x48); wc_b(0x89); wc_b(0xBD); wc_d32((uint32_t)wc_vars[vi].off); }
            else if (nparams == 1) { wc_b(0x48); wc_b(0x89); wc_b(0xB5); wc_d32((uint32_t)wc_vars[vi].off); }
            else if (nparams == 2) { wc_b(0x48); wc_b(0x89); wc_b(0x95); wc_d32((uint32_t)wc_vars[vi].off); }
            else if (nparams == 3) { wc_b(0x48); wc_b(0x89); wc_b(0x8D); wc_d32((uint32_t)wc_vars[vi].off); }
            else if (nparams == 4) { wc_b(0x4C); wc_b(0x89); wc_b(0x85); wc_d32((uint32_t)wc_vars[vi].off); }
            else if (nparams == 5) { wc_b(0x4C); wc_b(0x89); wc_b(0x8D); wc_d32((uint32_t)wc_vars[vi].off); }
            nparams++;
            wc_next();
            if (wc_tok == T_COMMA) { wc_next(); continue; }
            break;
        }
    }
    if (wc_tok != T_RP) { wc_fail("')' diharapkan"); return; }
    wc_next();
    wc_fns[wc_nfn].nparams = nparams;
    wc_nfn++;
    wc_loop_depth = 0;
    if (wc_tok != T_LB) { wc_fail("'{' diharapkan"); return; }
    wc_next();
    while (wc_tok != T_RB && wc_tok != T_EOF) {
        parse_stmt();
        if (wc_failed) return;
    }
    if (wc_tok != T_RB) { wc_fail("'}' diharapkan"); return; }
    wc_next();
    emit_epilogue();
    uint32_t frame = (uint32_t)(((wc_frame + 15) & ~15u) - 8);
    for (int i = 0; i < 4; i++) wc_code[sub_patch + i] = (uint8_t)(frame >> (8*i));
}
static int wc_compile_src(const char *src) {
    wc_src = src; wc_line = 1; wc_failed = 0; wc_err[0] = 0;
    wc_code = (uint8_t *)WC_CODE; wc_clen = 0;
    wc_data = (char *)WC_DATA;    wc_dlen = 0;
    wc_nfn = 0; wc_nrel = 0; wc_nvar = 0;
    wc_nlabel = 0; wc_njump = 0;
    wc_next();
    while (!wc_failed && wc_tok != T_EOF) {
        if (is_type_tok()) parse_function();
        else { wc_fail("deklarasi fungsi diharapkan"); break; }
    }
    if (!wc_failed) resolve_jumps();
    if (!wc_failed && find_fn("main") < 0) wc_fail("fungsi main tidak ada");
    if (wc_failed) return -1;
    return 0;
}

static char wc_srcbuf[WC_SRCBUF];
static char wc_mainsrc[FS_MAX_SIZE];

static int expand_into(const char *src, int srclen, char *out, int outmax, int depth) {
    if (depth > 3) return -1;
    int oi = 0, i = 0;
    while (i < srclen) {
        int j = i;
        while (j < srclen && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j + 8 <= srclen && src[j] == '#' && strncmp_(src + j, "#include", 8) == 0) {
            int k = j + 8;
            while (k < srclen && (src[k]==' '||src[k]=='\t')) k++;
            if (k < srclen && src[k] == '"') {
                k++;
                char fn[64]; int n = 0;
                while (k < srclen && src[k] != '"' && n < 63) fn[n++] = src[k++];
                fn[n] = 0;
                if (k < srclen && src[k] == '"') k++;
                int fi;
                if (path_resolve(fn, &fi) >= 0 && fi != ROOT && !fs_dir[fi].is_dir) {
                    if (fs_load(fi) < 0) return -1;
                    uint32_t fsz = fs_dir[fi].size;
                    if (fsz >= FS_MAX_SIZE) fsz = FS_MAX_SIZE - 1;
                    char tmp[FS_MAX_SIZE];
                    for (uint32_t z = 0; z < fsz; z++) tmp[z] = (char)filebuf[z];
                    tmp[fsz] = 0;
                    int got = expand_into(tmp, (int)fsz, out + oi, outmax - oi, depth + 1);
                    if (got < 0) return -1;
                    oi += got;
                }
                i = k;
                while (i < srclen && src[i] != '\n') i++;
                if (i < srclen) { if (oi < outmax - 1) out[oi++] = '\n'; i++; }
                continue;
            }
        }
        if (oi >= outmax - 1) return -1;
        out[oi++] = src[i++];
    }
    out[oi] = 0;
    return oi;
}

static void wr32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static int wc_pack(int main_off, int *out_size) {
    uint32_t total = WC_HDR + (uint32_t)wc_nrel * 8
                   + (uint32_t)wc_clen + (uint32_t)wc_dlen;
    if (total > FS_MAX_SIZE) return -1;
    filebuf[0]=WCX_MAGIC_0; filebuf[1]=WCX_MAGIC_1;
    filebuf[2]=WCX_MAGIC_2; filebuf[3]=WCX_MAGIC_3;
    wr32(filebuf + 4,  (uint32_t)wc_clen);
    wr32(filebuf + 8,  (uint32_t)wc_dlen);
    wr32(filebuf + 12, (uint32_t)main_off);
    wr32(filebuf + 16, (uint32_t)wc_nrel);
    uint8_t *p = filebuf + WC_HDR;
    for (int i = 0; i < wc_nrel; i++) {
        wr32(p, wc_rels[i].off);
        p[4] = (uint8_t)(wc_rels[i].kind & 0xFF);
        p[5] = (uint8_t)(wc_rels[i].kind >> 8);
        p[6] = (uint8_t)(wc_rels[i].idx & 0xFF);
        p[7] = (uint8_t)(wc_rels[i].idx >> 8);
        p += 8;
    }
    for (int i = 0; i < wc_clen; i++) *p++ = wc_code[i];
    for (int i = 0; i < wc_dlen; i++) *p++ = (uint8_t)wc_data[i];
    *out_size = (int)total;
    return 0;
}

static void cmd_wcc(const char *args) {
    char a[128], o[128];
    const char *p = args;
    int i, od; char leaf[32];
    if (!fs_check()) return;
    if (!next_arg(&p, a, sizeof a)) {
        con_puts("Format: wcc <berkas.wc> [keluaran]  (bawaan ./hasil)\n");
        return;
    }
    if (!next_arg(&p, o, sizeof o)) str_copy(o, "./hasil");
    if (path_split(o, &od, leaf) < 0 || !leaf[0] || !fs_name_ok(leaf)) {
        con_puts("wcc: alamat keluaran tidak valid.\n"); return;
    }
    int ex = fs_child(od, leaf);
    if (ex >= 0 && fs_dir[ex].is_dir) { con_puts("wcc: keluaran direktori.\n"); return; }
    if (path_resolve(a, &i) < 0 || i == ROOT || fs_dir[i].is_dir) {
        con_puts("wcc: berkas tidak ada.\n"); return;
    }
    if (i == ex) { con_puts("wcc: keluaran == sumber.\n"); return; }
    if (fs_load(i) < 0) { con_puts("Gagal membaca disk.\n"); return; }
    uint32_t n = fs_dir[i].size;
    if (n >= FS_MAX_SIZE) n = FS_MAX_SIZE - 1;
    filebuf[n] = 0;
    for (uint32_t k = 0; k <= n; k++) wc_mainsrc[k] = (char)filebuf[k];
    int got = expand_into(wc_mainsrc, (int)n, wc_srcbuf, WC_SRCBUF, 0);
    if (got < 0) {
        con_color(RED, con_bg);
        con_puts("wcc: gagal #include.");
        con_color(BLACK, con_bg); con_puts("\n");
        return;
    }
    if (wc_compile_src(wc_srcbuf) < 0) {
        con_color(RED, con_bg);
        con_puts("wcc: "); con_puts(wc_err);
        con_color(BLACK, con_bg); con_puts("\n");
        return;
    }
    int main_idx = find_fn("main");
    int total;
    if (wc_pack(wc_fns[main_idx].off, &total) < 0) {
        con_color(RED, con_bg);
        con_puts("wcc: hasil > FS_MAX_SIZE.");
        con_color(BLACK, con_bg); con_puts("\n");
        return;
    }
    int r = fs_write_file(od, leaf, (uint32_t)total);
    if (r == -3)      { con_puts("Tabel penuh.\n"); return; }
    else if (r == -2) { con_puts("wcc: keluaran direktori.\n"); return; }
    else if (r < 0)   { con_puts("Gagal menulis!\n"); return; }
    con_puts("Kompilasi OK: "); con_dec(wc_clen); con_puts(" byte kode -> ");
    con_puts(o); con_puts(" (");
    con_dec((uint32_t)total); con_puts(" byte)\n");
}
static void cmd_wrun(const char *args) {
    char a[128];
    const char *p = args;
    int i;
    if (!fs_check()) return;
    if (!next_arg(&p, a, sizeof a)) str_copy(a, "./hasil");
    if (path_resolve(a, &i) < 0 || i == ROOT || fs_dir[i].is_dir) {
        con_puts("wrun: berkas tidak ada.\n"); return;
    }
    if (fs_load(i) < 0) { con_puts("Gagal membaca disk.\n"); return; }
    uint32_t size = fs_dir[i].size;
    if (size < WC_HDR || filebuf[0]!=WCX_MAGIC_0 || filebuf[1]!=WCX_MAGIC_1 ||
        filebuf[2]!=WCX_MAGIC_2 || filebuf[3]!=WCX_MAGIC_3) {
        con_puts("wrun: bukan berkas WCX2.\n"); return;
    }
    uint32_t clen = rd32(filebuf + 4);
    uint32_t dlen = rd32(filebuf + 8);
    uint32_t moff = rd32(filebuf + 12);
    uint32_t nrel = rd32(filebuf + 16);
    if (clen > WC_CODE_MAX || dlen > WC_DATA_MAX || nrel > WC_MAXREL ||
        moff >= clen || WC_HDR + nrel * 8 + clen + dlen > size) {
        con_puts("wrun: berkas rusak.\n"); return;
    }
    const uint8_t *rel  = filebuf + WC_HDR;
    const uint8_t *code = rel + nrel * 8;
    const uint8_t *data = code + clen;
    uint8_t *dc = (uint8_t *)WC_CODE;
    uint8_t *dd = (uint8_t *)WC_DATA;
    for (uint32_t k = 0; k < clen; k++) dc[k] = code[k];
    for (uint32_t k = 0; k < dlen; k++) dd[k] = data[k];
    for (uint32_t k = 0; k < nrel; k++) {
        uint32_t off  = rd32(rel + k*8);
        uint16_t kind = (uint16_t)(rel[k*8+4] | (rel[k*8+5]<<8));
        uint16_t idx  = (uint16_t)(rel[k*8+6] | (rel[k*8+7]<<8));
        if (off + 8 > clen) { con_puts("wrun: relokasi rusak.\n"); return; }
        uint64_t val;
        if (kind == 0) {
            if (idx >= (uint32_t)WC_NBUILTIN) { con_puts("wrun: builtin rusak.\n"); return; }
            val = (uint64_t)wc_builtins[idx].fn;
        } else if (kind == 1) {
            if (idx >= clen) { con_puts("wrun: alamat fungsi rusak.\n"); return; }
            val = (uint64_t)(dc + idx);
        } else { con_puts("wrun: kind tak dikenal.\n"); return; }
        for (int b = 0; b < 8; b++) dc[off + b] = (uint8_t)(val >> (8*b));
    }
    void (*f)(void) = (void (*)(void))(dc + moff);
    f();
    gd = fb;   /* jaga-jaga jika program lupa gfx_buf(0) */
}

/* ============================================================
 * 9f-b. MARIO (embedded .wc, dari mario_src.h)
 * ============================================================ */
static const char mario_src[] =
"// MARIO WIDHY v3\n"
"int mc(int d) {\n"
" if (d==1) return 4;\n"
" if (d==2) return 8;\n"
" if (d==3) return 6;\n"
" if (d==4) return 1;\n"
" if (d==5) return 14;\n"
" if (d==6) return 15;\n"
" if (d==8) return 5;\n"
" if (d==9) return 12;\n"
" return 0;\n"
"}\n"
"void srow(int x,int y,int v,int s,int f) {\n"
" int i; int d;\n"
" i=11;\n"
" while (i>=0) {\n"
"  d=v%10; v=v/10;\n"
"  if (d!=0) {\n"
"   if (f==0) gfx_rect(x+i*s,y,s,s,mc(d)); else gfx_rect(x+(11-i)*s,y,s,s,mc(d));\n"
"  }\n"
"  i=i-1;\n"
" }\n"
"}\n"
"void mario(int x,int y,int f,int m) {\n"
" srow(x,y,000001111100,2,f);\n"
" srow(x,y+2,000011111111,2,f);\n"
" srow(x,y+4,000033322720,2,f);\n"
" srow(x,y+6,000323222722,2,f);\n"
" srow(x,y+8,000332233322,2,f);\n"
" srow(x,y+10,000002222220,2,f);\n"
" srow(x,y+12,001114444111,2,f);\n"
" srow(x,y+14,002114544112,2,f);\n"
" srow(x,y+16,002224444222,2,f);\n"
" srow(x,y+18,000044444400,2,f);\n"
" srow(x,y+20,000044444400,2,f);\n"
" if (m==0) {\n"
"  srow(x,y+22,000044004400,2,f);\n"
"  srow(x,y+24,000044004400,2,f);\n"
"  srow(x,y+26,000044004400,2,f);\n"
"  srow(x,y+28,000333003330,2,f);\n"
"  srow(x,y+30,003333003333,2,f);\n"
" } else if (m==1) {\n"
"  srow(x,y+22,000444004440,2,f);\n"
"  srow(x,y+24,004440000444,2,f);\n"
"  srow(x,y+26,003330000333,2,f);\n"
"  srow(x,y+28,033330000333,2,f);\n"
" } else {\n"
"  srow(x,y+22,004440004440,2,f);\n"
"  srow(x,y+24,033440004433,2,f);\n"
"  srow(x,y+26,033300000333,2,f);\n"
" }\n"
"}\n"
"void goomba(int x,int y,int m) {\n"
" srow(x,y,000003333000,2,0);\n"
" srow(x,y+2,000033333300,2,0);\n"
" srow(x,y+4,000333333330,2,0);\n"
" srow(x,y+6,003366336633,2,0);\n"
" srow(x,y+8,003367337633,2,0);\n"
" srow(x,y+10,033333333330,2,0);\n"
" srow(x,y+12,000099999900,2,0);\n"
" srow(x,y+14,000099999900,2,0);\n"
" srow(x,y+16,000099999900,2,0);\n"
" if (m==0) {\n"
"  srow(x,y+18,008880008880,2,0);\n"
"  srow(x,y+20,088888008888,2,0);\n"
" } else {\n"
"  srow(x,y+18,000888088800,2,0);\n"
"  srow(x,y+20,888880000888,2,0);\n"
" }\n"
"}\n"
"void block(int x,int y,int t) {\n"
" if (t==0) {\n"
"  gfx_rect(x,y,32,32,5);\n"
"  gfx_rect(x+1,y+1,14,6,6); gfx_rect(x+17,y+1,14,6,6);\n"
"  gfx_rect(x+1,y+9,6,6,6); gfx_rect(x+9,y+9,14,6,6); gfx_rect(x+25,y+9,6,6,6);\n"
"  gfx_rect(x+1,y+17,14,6,6); gfx_rect(x+17,y+17,14,6,6);\n"
"  gfx_rect(x+1,y+25,6,6,6); gfx_rect(x+9,y+25,14,6,6); gfx_rect(x+25,y+25,6,6,6);\n"
"  gfx_rect(x+1,y+1,30,1,12);\n"
" } else if (t==1) {\n"
"  gfx_rect(x,y,32,32,0); gfx_rect(x+2,y+2,28,28,14);\n"
"  gfx_rect(x+2,y+2,28,2,15); gfx_rect(x+2,y+28,28,2,6);\n"
"  gfx_rect(x+4,y+4,2,2,0); gfx_rect(x+26,y+4,2,2,0);\n"
"  gfx_rect(x+4,y+26,2,2,0); gfx_rect(x+26,y+26,2,2,0);\n"
"  gfx_text(x+12,y+8,\"?\",230);\n"
" } else {\n"
"  gfx_rect(x,y,32,32,0); gfx_rect(x+2,y+2,28,28,6);\n"
"  gfx_rect(x+2,y+2,28,2,12); gfx_rect(x+4,y+4,2,2,5); gfx_rect(x+26,y+4,2,2,5);\n"
"  gfx_rect(x+4,y+26,2,2,5); gfx_rect(x+26,y+26,2,2,5);\n"
" }\n"
"}\n"
"void coin(int x,int y,int f) {\n"
" int w;\n"
" w=12;\n"
" if (f==1 || f==3) w=8;\n"
" if (f==2) w=4;\n"
" gfx_rect(x+(12-w)/2,y,w,16,0);\n"
" gfx_rect(x+(12-w)/2+1,y+1,w-2,14,14);\n"
" if (f==0) gfx_rect(x+3,y+3,2,8,15);\n"
"}\n"
"void cloud(int x,int y) {\n"
" gfx_rect(x+14,y,36,6,15); gfx_rect(x+6,y+6,52,6,15); gfx_rect(x,y+12,64,6,15);\n"
" gfx_rect(x,y+18,64,4,3); gfx_rect(x+6,y+22,52,3,3);\n"
"}\n"
"void hill(int x,int y,int n) {\n"
" int j;\n"
" j=0;\n"
" while (j<n) {\n"
"  gfx_rect(x+j*16,y-(j+1)*12,(n-j)*32-16,12,2);\n"
"  gfx_rect(x+j*16,y-(j+1)*12,(n-j)*32-16,3,10);\n"
"  j=j+1;\n"
" }\n"
"}\n"
"void bush(int x,int y) {\n"
" gfx_rect(x+6,y-20,18,20,10); gfx_rect(x+20,y-26,24,26,10);\n"
" gfx_rect(x+40,y-18,18,18,10); gfx_rect(x+6,y-6,52,6,2);\n"
"}\n"
"void flagpole(int x) {\n"
" gfx_rect(x,240,6,180,15); gfx_rect(x+4,240,2,180,7); gfx_rect(x-2,232,10,8,14);\n"
" gfx_rect(x-30,246,30,6,4); gfx_rect(x-24,252,24,6,4); gfx_rect(x-18,258,18,6,4);\n"
" gfx_rect(x-12,264,12,6,4); gfx_rect(x-6,270,6,6,4);\n"
" gfx_rect(x-8,410,22,10,7);\n"
" gfx_rect(x+50,350,90,70,5); gfx_rect(x+52,352,86,68,6);\n"
" gfx_rect(x+50,336,20,14,5); gfx_rect(x+52,338,16,12,6);\n"
" gfx_rect(x+80,336,20,14,5); gfx_rect(x+82,338,16,12,6);\n"
" gfx_rect(x+120,336,20,14,5); gfx_rect(x+122,338,16,12,6);\n"
" gfx_rect(x+82,380,26,40,0); gfx_rect(x+86,374,18,8,0);\n"
"}\n"
"void sky() {\n"
" int i;\n"
" gfx_rect(0,16,640,130,9);\n"
" gfx_rect(0,146,640,110,11);\n"
" gfx_rect(0,256,640,224,13);\n"
" i=0;\n"
" while (i<4) {\n"
"  gfx_rect(0,130+i*4,640,i+1,11);\n"
"  gfx_rect(0,240+i*4,640,i+1,13);\n"
"  i=i+1;\n"
" }\n"
"}\n"
"int ispit(int c) {\n"
" if (c>=24 && c<=26) return 1;\n"
" if (c>=50 && c<=52) return 1;\n"
" if (c>=76 && c<=78) return 1;\n"
" return 0;\n"
"}\n"
"void ground(int cam) {\n"
" int i; int c; int g;\n"
" i=0;\n"
" while (i<21) {\n"
"  c=cam/32+i;\n"
"  g=c*32-cam;\n"
"  if (ispit(c)==0) {\n"
"   gfx_rect(g,420,32,5,10); gfx_rect(g,425,32,4,2); gfx_rect(g,429,32,51,6);\n"
"   gfx_rect(g,429,32,1,12);\n"
"   gfx_rect(g,445,32,2,5); gfx_rect(g,461,32,2,5);\n"
"   gfx_rect(g+30,431,2,14,5); gfx_rect(g+14,447,2,14,5); gfx_rect(g+30,463,2,17,5);\n"
"  }\n"
"  i=i+1;\n"
" }\n"
"}\n"
"void pbox(int c) {\n"
" gfx_rect(190,150,260,110,0);\n"
" gfx_rect(193,153,254,104,c);\n"
"}\n"
"void main() {\n"
" int px; int py; int vy; int oy; int ox; int og; int dir; int spd; int jk; int jp;\n"
" int cam; int fr; int quit; int st; int dead; int inv; int lives; int score; int nc;\n"
" int cp; int face; int pose; int i; int k; int sx; int dx; int dy; int cc; int t; int n;\n"
" int bx[12]; int bh[12]; int bt[12];\n"
" int ex[5]; int emin[5]; int emax[5]; int ed[5]; int ea[5]; int et[5];\n"
" int cxw[18]; int cyh[18]; int got[18];\n"
" bx[0]=416; bx[1]=448; bx[2]=480; bx[3]=1056; bx[4]=1088; bx[5]=1088;\n"
" bx[6]=1824; bx[7]=1856; bx[8]=1888; bx[9]=2400; bx[10]=2432; bx[11]=2464;\n"
" i=0;\n"
" while (i<12) { bh[i]=96; bt[i]=0; i=i+1; }\n"
" bh[5]=176; bt[1]=1; bt[4]=1; bt[5]=1; bt[7]=1; bt[10]=1;\n"
" ex[0]=560; emin[0]=520; emax[0]=740;\n"
" ex[1]=1000; emin[1]=940; emax[1]=1180;\n"
" ex[2]=1480; emin[2]=1400; emax[2]=1560;\n"
" ex[3]=2100; emin[3]=1950; emax[3]=2250;\n"
" ex[4]=2650; emin[4]=2560; emax[4]=2800;\n"
" i=0;\n"
" while (i<5) { ed[i]=2-(i%2)*4; ea[i]=1; et[i]=0; i=i+1; }\n"
" i=0;\n"
" while (i<18) { cxw[i]=250+i*160; cyh[i]=60+(i%4)*22; got[i]=0; i=i+1; }\n"
" px=40; py=0; vy=0; og=1; dir=0; face=1; jp=0; cam=0; fr=0; quit=0; st=0;\n"
" dead=0; inv=0; lives=3; score=0; nc=0; cp=40;\n"
" mouse_hide(); cursor(0);\n"
" while (poll()!=0) { }\n"
" pal_set(1,8,16,58); pal_set(2,3,30,4); pal_set(3,44,48,58); pal_set(4,60,6,6);\n"
" pal_set(5,22,10,2); pal_set(6,44,22,8); pal_set(8,63,46,32); pal_set(9,10,30,60);\n"
" pal_set(10,24,56,10); pal_set(11,24,44,63); pal_set(12,58,40,18);\n"
" pal_set(13,44,56,63); pal_set(14,63,58,10);\n"
" gfx_buf(1);\n"
" while (quit==0) {\n"
"  k=poll();\n"
"  while (k!=0) {\n"
"   if (k==27) quit=1;\n"
"   if (k==10) { if (st==0) st=1; else if (st>1) quit=1; }\n"
"   k=poll();\n"
"  }\n"
"  if (st==1) {\n"
"   if (dead==0) {\n"
"    dir=0;\n"
"    if (key_down(30)||key_down(75)) dir=dir-1;\n"
"    if (key_down(32)||key_down(77)) dir=dir+1;\n"
"    spd=4;\n"
"    if (key_down(42)||key_down(54)) spd=6;\n"
"    jk=0;\n"
"    if (key_down(57)||key_down(17)||key_down(72)) jk=1;\n"
"    if (dir!=0) face=dir;\n"
"    ox=px;\n"
"    px=px+dir*spd;\n"
"    if (px<0) px=0;\n"
"    if (px>3176) px=3176;\n"
"    i=0;\n"
"    while (i<12) {\n"
"     if (py<bh[i] && py+32>bh[i]-32 && px+21>bx[i] && px+3<bx[i]+32) px=ox;\n"
"     i=i+1;\n"
"    }\n"
"    if (jk==1 && jp==0 && og==1) vy=16;\n"
"    if (jk==0 && vy>6) vy=6;\n"
"    jp=jk;\n"
"    oy=py;\n"
"    vy=vy-1;\n"
"    if (vy<-14) vy=-14;\n"
"    py=py+vy;\n"
"    og=0;\n"
"    cc=(px+12)/32;\n"
"    if (vy<0 && oy>=0 && py<=0 && ispit(cc)==0) { py=0; vy=0; og=1; }\n"
"    i=0;\n"
"    while (i<12) {\n"
"     if (px+21>bx[i] && px+3<bx[i]+32) {\n"
"      if (vy<0 && oy>=bh[i] && py<=bh[i]) { py=bh[i]; vy=0; og=1; }\n"
"      else if (vy>0 && oy+32<=bh[i]-32 && py+32>bh[i]-32) {\n"
"       py=bh[i]-64; vy=0;\n"
"       if (bt[i]==1) { bt[i]=2; nc=nc+1; score=score+200; beep(1600,12); }\n"
"      }\n"
"     }\n"
"     i=i+1;\n"
"    }\n"
"    i=0;\n"
"    while (i<5) {\n"
"     if (ea[i]==1) {\n"
"      ex[i]=ex[i]+ed[i];\n"
"      if (ex[i]<emin[i]) { ex[i]=emin[i]; ed[i]=2; }\n"
"      if (ex[i]>emax[i]) { ex[i]=emax[i]; ed[i]=-2; }\n"
"      if (dead==0 && px+21>ex[i]+1 && px+3<ex[i]+23 && py<22) {\n"
"       if (vy<0 && oy>=14) { ea[i]=2; et[i]=25; vy=9; score=score+100; beep(500,20); }\n"
"       else if (inv==0) { dead=1; vy=12; beep(250,40); }\n"
"      }\n"
"     } else if (ea[i]==2) {\n"
"      et[i]=et[i]-1;\n"
"      if (et[i]<=0) ea[i]=0;\n"
"     }\n"
"     i=i+1;\n"
"    }\n"
"    i=0;\n"
"    while (i<18) {\n"
"     if (got[i]==0) {\n"
"      dx=px+12-cxw[i]-6;\n"
"      dy=py+16-cyh[i];\n"
"      if (dx>-20 && dx<20 && dy>-26 && dy<26) {\n"
"       got[i]=1; nc=nc+1; score=score+50; beep(1500,12);\n"
"      }\n"
"     }\n"
"     i=i+1;\n"
"    }\n"
"    if (px>1760 && cp<1760) cp=1760;\n"
"    if (py<-70) { dead=30; vy=0; beep(220,60); }\n"
"    if (px+12>=3040) { st=2; beep(1000,80); beep(1300,80); beep(1700,200); }\n"
"    if (inv>0) inv=inv-1;\n"
"   } else {\n"
"    dead=dead+1;\n"
"    py=py+vy;\n"
"    vy=vy-1;\n"
"    if (vy<-14) vy=-14;\n"
"    if (dead>=90) {\n"
"     lives=lives-1;\n"
"     if (lives<=0) st=3;\n"
"     else { px=cp; py=0; vy=0; og=1; dead=0; inv=100; }\n"
"    }\n"
"   }\n"
"  }\n"
"  cam=px-220;\n"
"  if (cam<0) cam=0;\n"
"  if (cam>2560) cam=2560;\n"
"  sky();\n"
"  t=cam/6; n=t/260;\n"
"  i=0;\n"
"  while (i<4) { sx=(n+i)*260+30-t; cloud(sx,36+((n+i)%3)*30); i=i+1; }\n"
"  t=cam/3; n=t/480;\n"
"  i=0;\n"
"  while (i<3) { sx=(n+i)*480+40-t; hill(sx,420,3+((n+i)%2)*2); i=i+1; }\n"
"  n=cam/400;\n"
"  i=0;\n"
"  while (i<3) { sx=(n+i)*400+250-cam; bush(sx,420); i=i+1; }\n"
"  ground(cam);\n"
"  i=0;\n"
"  while (i<12) {\n"
"   sx=bx[i]-cam;\n"
"   if (sx>-32 && sx<640) block(sx,420-bh[i],bt[i]);\n"
"   i=i+1;\n"
"  }\n"
"  i=0;\n"
"  while (i<18) {\n"
"   sx=cxw[i]-cam;\n"
"   if (got[i]==0 && sx>-16 && sx<640) coin(sx,412-cyh[i],(fr/6)%4);\n"
"   i=i+1;\n"
"  }\n"
"  flagpole(3040-cam);\n"
"  i=0;\n"
"  while (i<5) {\n"
"   sx=ex[i]-cam;\n"
"   if (sx>-24 && sx<640) {\n"
"    if (ea[i]==1) goomba(sx,398,(fr/8)%2);\n"
"    else if (ea[i]==2) {\n"
"     gfx_rect(sx,412,24,8,6); gfx_rect(sx+2,416,20,4,12);\n"
"     gfx_rect(sx+5,413,3,2,15); gfx_rect(sx+16,413,3,2,15);\n"
"    }\n"
"   }\n"
"   i=i+1;\n"
"  }\n"
"  pose=0;\n"
"  if (og==0 || dead>0) pose=2;\n"
"  else if (dir!=0 && (fr/4)%2==1) pose=1;\n"
"  t=0;\n"
"  if (face<0) t=1;\n"
"  if (inv==0 || (fr/3)%2==0) mario(px-cam,388-py,t,pose);\n"
"  gfx_rect(0,16,640,20,0);\n"
"  gfx_text(12,18,\"SKOR\",14); gfx_int(60,18,score,15);\n"
"  gfx_text(176,18,\"KOIN\",14); gfx_int(224,18,nc,15);\n"
"  gfx_text(292,18,\"NYAWA\",14); gfx_int(348,18,lives,15);\n"
"  gfx_text(436,18,\"SPASI lompat ESC keluar\",7);\n"
"  if (st==0) {\n"
"   pbox(1);\n"
"   gfx_text(236,164,\"S U P E R   M A R I O\",30);\n"
"   gfx_text(256,184,\"WIDHY OS EDITION\",31);\n"
"   gfx_text(232,206,\"A/D gerak SPASI lompat\",31);\n"
"   gfx_text(228,226,\"ENTER mulai  ESC keluar\",30);\n"
"  } else if (st==2) {\n"
"   pbox(2);\n"
"   gfx_text(272,166,\"KAMU MENANG!\",46);\n"
"   gfx_text(248,194,\"SKOR\",47); gfx_int(296,194,score,47);\n"
"   gfx_text(264,226,\"ENTER keluar\",47);\n"
"  } else if (st==3) {\n"
"   pbox(4);\n"
"   gfx_text(284,166,\"GAME OVER\",78);\n"
"   gfx_text(248,194,\"SKOR\",79); gfx_int(296,194,score,79);\n"
"   gfx_text(264,226,\"ENTER keluar\",79);\n"
"  }\n"
"  gfx_flip();\n"
"  fr=fr+1;\n"
"  delay(16);\n"
" }\n"
" gfx_buf(0);\n"
" pal_reset();\n"
" mouse_hide();\n"
" cls();\n"
" cursor(1);\n"
" print(\"Terima kasih sudah bermain!\\n\");\n"
"}\n"
;

static void run_wc_mem(const char *src) {
    int n = 0;
    while (src[n] && n < WC_SRCBUF - 1) { wc_srcbuf[n] = src[n]; n++; }
    wc_srcbuf[n] = 0;

    if (wc_compile_src(wc_srcbuf) < 0) {
        con_color(RED, con_bg);
        con_puts("compile: "); con_puts(wc_err);
        con_color(BLACK, con_bg); con_puts("\n");
        return;
    }
    uint8_t *dc = (uint8_t *)WC_CODE;
    for (int i = 0; i < wc_nrel; i++) {
        uint32_t off  = wc_rels[i].off;
        uint16_t kind = wc_rels[i].kind;
        uint16_t idx  = wc_rels[i].idx;
        uint64_t val;
        if (kind == 0)      val = (uint64_t)wc_builtins[idx].fn;
        else                val = (uint64_t)(dc + idx);
        for (int b = 0; b < 8; b++) dc[off + b] = (uint8_t)(val >> (8*b));
    }
    int mi = find_fn("main");
    void (*f)(void) = (void (*)(void))(dc + wc_fns[mi].off);
    f();
    gd = fb;   /* jaga-jaga jika program lupa gfx_buf(0) */
}

static void ensure_wc_file(const char *name, const char *src) {
    if (!fs_ready) return;
    uint32_t n = 0;
    while (src[n]) n++;
    if (n > FS_MAX_SIZE) return;
    int idx = fs_child(ROOT, name);
    if (idx >= 0 && fs_dir[idx].is_dir) return;
    if (idx >= 0 && fs_dir[idx].size == n) return;
    if (idx < 0) {
        idx = fs_alloc(ROOT, name, 0);
        if (idx < 0) return;
    }
    for (uint32_t i = 0; i < n; i++) filebuf[i] = (uint8_t)src[i];
    fs_dir[idx].size = n;
    fs_store(idx);
    fs_sync_dir();
}

static void cmd_mario(void) { run_wc_mem(mario_src); }

/* ============================================================
 * 9f-c. WIDHY PAINT - native app, simpan .wpg (RLE)
 * ============================================================ */
#define PAINT_X    4
#define PAINT_Y    60
#define PAINT_W    632
#define PAINT_H    400
#define PAINT_HDR  10

static int paint_rle_compress(uint8_t *out, int outmax) {
    int oi = 0;
    for (int y = 0; y < PAINT_H; y++) {
        volatile uint8_t *row = fb + (PAINT_Y + y) * pitch + PAINT_X;
        int x = 0;
        while (x < PAINT_W) {
            uint8_t color = row[x];
            int run = 1;
            while (x + run < PAINT_W && run < 255 && row[x + run] == color) run++;
            if (oi + 2 > outmax) return -1;
            out[oi++] = (uint8_t)run;
            out[oi++] = color;
            x += run;
        }
    }
    return oi;
}
static void paint_rle_decompress(const uint8_t *in, int inlen) {
    int pixel = 0;
    int total = PAINT_W * PAINT_H;
    int i = 0;
    while (i + 1 < inlen && pixel < total) {
        int run = in[i];
        uint8_t color = in[i + 1];
        i += 2;
        if (run == 0) break;
        for (int k = 0; k < run && pixel < total; k++, pixel++) {
            int y = pixel / PAINT_W;
            int x = pixel % PAINT_W;
            fb[(PAINT_Y + y) * pitch + PAINT_X + x] = color;
        }
    }
}

static void paint_draw_palette(int cur) {
    fill_rect(0, 16, width, 40, LGRAY);
    for (int i = 0; i < 16; i++) {
        fill_rect(4 + i * 38, 22, 32, 32, (uint8_t)i);
        widhy_gfx_frame(4 + i * 38, 22, 32, 32, BLACK);
    }
    widhy_gfx_frame(2 + cur * 38, 20, 36, 36, WHITE);
    widhy_gfx_frame(3 + cur * 38, 21, 34, 34, BLACK);
}
static void paint_draw_status(const char *msg) {
    fill_rect(0, 462, width, 18, LGRAY);
    if (msg) widhy_gfx_text(4, 463, msg, 0x70);
    else     widhy_gfx_text(4, 463,
        "Kiri=gambar  Kanan=hapus  Ctrl+S simpan  Ctrl+O buka  Ctrl+N baru  ESC keluar",
        0x70);
}

static void paint_ensure_wpg(const char *name, char *out, int outmax) {
    int len = 0;
    while (name[len] && len < outmax - 5) { out[len] = name[len]; len++; }
    if (len < 4 || out[len-4] != '.' || out[len-3] != 'w'
                || out[len-2] != 'p' || out[len-1] != 'g') {
        out[len++]='.'; out[len++]='w'; out[len++]='p'; out[len++]='g';
    }
    out[len] = 0;
}

static void paint_save_file(const char *name) {
    char full[40];
    paint_ensure_wpg(name, full, sizeof full);
    if (!fs_name_ok(full)) { paint_draw_status("Nama tidak valid"); return; }

    filebuf[0]='W'; filebuf[1]='P'; filebuf[2]='G'; filebuf[3]='1';
    filebuf[4] = PAINT_W & 0xFF;
    filebuf[5] = (PAINT_W >> 8) & 0xFF;
    filebuf[6] = PAINT_H & 0xFF;
    filebuf[7] = (PAINT_H >> 8) & 0xFF;

    int rle = paint_rle_compress(filebuf + PAINT_HDR, FS_MAX_SIZE - PAINT_HDR);
    if (rle < 0) { paint_draw_status("Gambar terlalu besar untuk FS"); return; }
    filebuf[8] = rle & 0xFF;
    filebuf[9] = (rle >> 8) & 0xFF;

    int total = PAINT_HDR + rle;
    int r = fs_write_file(ROOT, full, (uint32_t)total);
    if (r < 0) paint_draw_status("Gagal menyimpan");
    else       paint_draw_status("Tersimpan");
}
static void paint_open_file(const char *name) {
    char full[40];
    paint_ensure_wpg(name, full, sizeof full);

    int idx;
    if (path_resolve(full, &idx) < 0 || idx == ROOT || fs_dir[idx].is_dir) {
        paint_draw_status("File tidak ada");
        return;
    }
    if (fs_load(idx) < 0) { paint_draw_status("Gagal baca disk"); return; }
    if (fs_dir[idx].size < PAINT_HDR ||
        filebuf[0]!='W' || filebuf[1]!='P' || filebuf[2]!='G' || filebuf[3]!='1') {
        paint_draw_status("Bukan WPG");
        return;
    }
    int rle = filebuf[8] | (filebuf[9] << 8);
    if (PAINT_HDR + rle > (int)fs_dir[idx].size) {
        paint_draw_status("Berkas rusak");
        return;
    }
    paint_rle_decompress(filebuf + PAINT_HDR, rle);
    paint_draw_status("Terbuka");
}

static int paint_prompt(const char *label, char *buf, int maxn) {
    int bx = 170, by = 220, bw = 300, bh = 56;
    int n = 0;
    buf[0] = 0;

    widhy_panel_save(bx, by, bw, bh);

    fill_rect(bx, by, bw, bh, LGRAY);
    widhy_gfx_frame(bx, by, bw, bh, BLACK);
    widhy_gfx_text(bx + 8, by + 6, label, 0x70);

    int quit = 0, ok = 0;
    while (!quit) {
        char disp[40];
        int d = 0;
        for (int i = 0; i < n && d < 36; i++) disp[d++] = buf[i];
        disp[d++] = '_'; disp[d] = 0;
        fill_rect(bx + 1, by + 28, bw - 2, CH, LGRAY);
        widhy_gfx_text(bx + 8, by + 28, disp, 0x70);

        int k = widhy_poll();
        if (k == 0) { widhy_delay(30); continue; }
        if (k == 27) { quit = 1; break; }
        if (k == '\n') { ok = 1; quit = 1; break; }
        if (k == '\b') { if (n > 0) { n--; buf[n] = 0; } }
        else if (k >= 32 && k < 127 && n < maxn - 1) {
            buf[n++] = (char)k;
            buf[n] = 0;
        }
    }
    widhy_panel_restore();
    return ok;
}

static void paint_run(const char *initial_file) {
    int cur = 12;
    int quit = 0;
    int drawing = 0;
    int last_mx = -1, last_my = -1;

    mouse_hide();
    widhy_cursor(0);

    fill_rect(0, CH, width, height - CH, DGRAY);
    paint_draw_palette(cur);
    fill_rect(PAINT_X, PAINT_Y, PAINT_W, PAINT_H, WHITE);
    widhy_gfx_frame(PAINT_X, PAINT_Y, PAINT_W, PAINT_H, BLACK);
    paint_draw_status(0);

    if (initial_file && initial_file[0]) {
        paint_open_file(initial_file);
    }

    mouse_show();

    while (!quit) {
        mouse_hide();
        int mx = mouse_x, my = mouse_y, btn = mouse_btn;

        if ((btn & 1) && my >= 22 && my < 54 && mx >= 4 && mx < 4 + 16 * 38) {
            int i = (mx - 4) / 38;
            if (i >= 0 && i < 16) {
                cur = i;
                paint_draw_palette(cur);
            }
        }

        int in_canvas = (mx >= PAINT_X && mx < PAINT_X + PAINT_W &&
                         my >= PAINT_Y && my < PAINT_Y + PAINT_H);
        if (((btn & 1) || (btn & 2)) && in_canvas) {
            int color = cur;
            if (btn & 2) color = 15;
            if (drawing && last_mx >= 0) {
                int dx = mx - last_mx; if (dx < 0) dx = -dx;
                int dy = my - last_my; if (dy < 0) dy = -dy;
                int steps = dx > dy ? dx : dy;
                if (steps < 1) steps = 1;
                for (int s = 0; s <= steps; s++) {
                    int lx = last_mx + (mx - last_mx) * s / steps;
                    int ly = last_my + (my - last_my) * s / steps;
                    if (lx >= PAINT_X+2 && lx < PAINT_X+PAINT_W-2 &&
                        ly >= PAINT_Y+2 && ly < PAINT_Y+PAINT_H-2)
                        fill_rect(lx - 2, ly - 2, 5, 5, (uint8_t)color);
                }
            } else {
                if (mx >= PAINT_X+2 && mx < PAINT_X+PAINT_W-2 &&
                    my >= PAINT_Y+2 && my < PAINT_Y+PAINT_H-2)
                    fill_rect(mx - 2, my - 2, 5, 5, (uint8_t)color);
            }
            last_mx = mx; last_my = my;
            drawing = 1;
        } else {
            drawing = 0;
        }

        mouse_show();

        int k = widhy_poll();
        if (k == 27) {
            quit = 1;
        } else if (k == 19) {
            char name[32];
            mouse_hide();
            if (paint_prompt("Simpan sebagai (.wpg):", name, 30)) {
                paint_save_file(name);
            }
            mouse_hide();
            paint_draw_palette(cur);
            paint_draw_status(0);
            mouse_show();
        } else if (k == 15) {
            char name[32];
            mouse_hide();
            if (paint_prompt("Buka file (.wpg):", name, 30)) {
                paint_open_file(name);
            }
            mouse_hide();
            paint_draw_palette(cur);
            mouse_show();
        } else if (k == 14) {
            fill_rect(PAINT_X + 1, PAINT_Y + 1, PAINT_W - 2, PAINT_H - 2, WHITE);
            paint_draw_status("Canvas baru");
        }

        widhy_delay(10);
    }

    mouse_hide();
    fill_rect(0, CH, width, height - CH, con_bg);
    con_x = 0;
    con_y = 1;
    widhy_cursor(1);
    mouse_show();
    con_puts("Paint selesai.\n");
}

/* ============================================================
 * 9g. run_command
 * ============================================================ */
static void run_command(const char *cmd){
  if(cmd[0] == 0) return;
  if(fs_command(cmd)) return;
  if(str_equal(cmd, "shutdown")){
    shutdown();
  } else if(str_equal(cmd, "help")){
    con_puts("Umum     : help, clear, shutdown, info, date/jam, gmt, beep, lady\n");
    con_puts("Tombol   : button | button add <id> <label> | button del <id> | button list\n");
    con_puts("Disk     : format yes\n");
    con_puts("Folder   : pwd, cd, mkdir, rmdir, ls [path]\n");
    con_puts("Berkas   : cat, write, append, cp, mv, rm [-r], nano\n");
    con_puts("Widhy Comp: wcc <sumber> [keluaran] | wrun [berkas]\n");
    con_puts("            mario  (game grafis: a/d/spasi/Shift/ESC)\n");
    con_puts("Paint    : paint [nama.wpg]   (kiri=draw, kanan=erase)\n");
    con_puts("           Ctrl+S simpan, Ctrl+O buka, Ctrl+N baru, ESC keluar\n");
    con_puts("  Fitur: int/char/void, array, fungsi, if/else, while, for,\n");
    con_puts("         switch/case/default, break, continue, return, #include\n");
    con_puts("  Builtin: print print_int print_char beep getkey input exit\n");
    con_puts("           poll cls putat putintat cursor delay key_down\n");
    con_puts("           gfx_clear gfx_rect gfx_frame gfx_text gfx_int\n");
    con_puts("           mouse_x mouse_y mouse_btn mouse_hide mouse_show\n");
    con_puts("           panel_save panel_restore ui_button ui_button_hit\n");
    con_puts("           gfx_buf gfx_flip pal_set pal_reset\n");
  } else if(str_equal(cmd, "date") || str_equal(cmd, "jam")){
    rtc_time_t t;
    char s[20];
    rtc_read_local(&t);
    rtc_fmt(&t, s);
    con_puts(s);
    con_puts("\n");
  } else if(str_equal(cmd, "lady")){
     random_word();
  } else if(str_equal(cmd, "clear")){
     fill_rect(0, CH, width, height - CH, con_bg);
     btn_clear_all();
     con_x = 0; con_y = 1;
  } else if (strncmp_(cmd, "beep ", 5) == 0) {
    const char *arg = cmd + 5;
    int freq = 0, ms = 0, i = 0;
    while (arg[i] >= '0' && arg[i] <= '9') { freq = freq * 10 + (arg[i] - '0'); i++; }
    while (arg[i] == ' ') i++;
    while (arg[i] >= '0' && arg[i] <= '9') { ms = ms * 10 + (arg[i] - '0'); i++; }
    if (freq > 0 && ms > 0) speaker_beep((uint32_t)freq, (uint32_t)ms);
    else con_puts("beep: format -> beep <freq> <ms>\n");
    return;
  } else if (str_equal(cmd, "kapal")) {
    speaker_beep(200, 800);
    widhy_output("Beep");
    return;
  } else if (strncmp_(cmd, "gmt ", 4) == 0) {
    const char *arg = cmd + 4;
    int sign = 1, val = 0, i = 0;
    if (arg[i] == '+') { sign = 1; i++; }
    else if (arg[i] == '-') { sign = -1; i++; }
    while (arg[i] >= '0' && arg[i] <= '9') { val = val * 10 + (arg[i] - '0'); i++; }
    if (val >= 0 && val <= 14) {
        rtc_set_gmt(sign * val);
        con_puts("[gmt] offset changed\n");
    } else con_puts("gmt: format -> gmt +7  or  gmt -5\n");
    return;
  } else if (str_equal(cmd, "button") || str_equal(cmd, "tombol")
          || strncmp_(cmd, "button ", 7) == 0 || strncmp_(cmd, "tombol ", 7) == 0) {
    const char *args = cmd;
    while (*args && *args != ' ') args++;
    cmd_button(args);
  } else if (str_equal(cmd, "mario")) {
    cmd_mario();
  } else if (str_equal(cmd, "paint")) {
    paint_run(0);
  } else if (strncmp_(cmd, "paint ", 6) == 0) {
    const char *p = cmd + 6;
    char fn[40]; int n = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && n < 39) fn[n++] = *p++;
    fn[n] = 0;
    paint_run(fn);
  } else if (strncmp_(cmd, "wcc", 3) == 0 && (cmd[3] == 0 || cmd[3] == ' ')) {
    cmd_wcc(cmd + 3);
  } else if (strncmp_(cmd, "wrun", 4) == 0 && (cmd[4] == 0 || cmd[4] == ' ')) {
    cmd_wrun(cmd + 4);
  } else {
    con_color(RED, con_bg);
    con_puts("Fuck You! Invalid Command!");
    con_color(BLACK, con_bg);
    con_puts("\n");
  }
}

/* ============================================================
 * 10. KMAIN
 * ============================================================ */
void kmain(void) {
    gfx_init();
    fill_rect(0, 0, width, height, LGREEN);
    draw_title_bar();
    con_color(BLUE, LGREEN);
    con_puts("WIDHYOS VERSION 2.0!\n");
    con_color(BLACK, LGREEN);
    con_puts("Widhy OS v2 (x86_64, 640x480). My Holiest Fixation:\n\n");
    con_puts("Secretive OS For Server!\n\n");
    ata_init();
    fs_mount();
    con_puts(ata_ok ? (fs_ready ? "Disk: FS siap.\n"
                                : "Disk: ada, belum diformat (ketik: format yes)\n")
                    : "Disk: tidak ditemukan.\n");
    ensure_wc_file("mario.wc", mario_src);
    print_prompt();
    idt_init();
    pic_init();
    pit_init();
    mouse_init();
    __asm__ volatile ("sti");
    char line[128];
    int line_len = 0;
    int cursor_on = 1;
    uint64_t last_blink = 0;
    mouse_show();
    text_cursor(1);
    for (;;) {
        while (key_tail != key_head) {
            uint8_t c = key_buf[key_tail++];
            mouse_hide();
            text_cursor(0);
            if(c == '\n'){
              con_putc('\n');
              line[line_len] = 0;
              run_command(line);
              line_len = 0;
              print_prompt();
            } else if(c == '\b'){
               if(line_len > 0){ line_len--; con_putc('\b'); }
            } else if(c >= 32 && c < 127 && line_len < 127){
               line[line_len++] = (char)c;
               con_putc((char)c);
            }
            cursor_on = 1;
            text_cursor(1);
            last_blink = ticks;
            btn_draw_all();
            mouse_show();
        }
        if (mouse_dirty) {
            mouse_dirty = 0;
            mouse_hide();
            update_title_status();
            uint16_t clicked = btn_mouse_update();
            if (clicked) {
                text_cursor(0);
                con_puts("\n");
                btn_action(clicked);
                print_prompt();
                for (int i = 0; i < line_len; i++) con_putc(line[i]);
                btn_draw_all();
                cursor_on = 1;
                text_cursor(1);
                last_blink = ticks;
            }
            mouse_show();
        }
        clock_tick();
        if (widhy_cursor_state && ticks - last_blink >= TICK_HZ / 2) {
            last_blink = ticks;
            cursor_on ^= 1;
            mouse_hide();
            text_cursor(cursor_on);
            mouse_show();
        }
        __asm__ volatile ("cli");
        if (key_tail == key_head && !mouse_dirty)
            __asm__ volatile ("sti; hlt");
        else
            __asm__ volatile ("sti");
    }
}
