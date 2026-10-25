/* Host test for patches/supermidi.c: the logic runs on this machine with the stock
 * functions and RAM replaced by stand-ins. It proves the patch's logic, not that the
 * hooks are right (that needs the device). Build and run by test_supermidi_c.py.
 *
 * The button tests drive the hook the way the stock polled state machine does: a
 * press sets byte +0x0B, and every scan calls the press handler while +0x0B is set,
 * +0x0C is clear and +0x08 is not 2. The stock handler's first three statements are
 * reproduced in the stand-in, so a hook that skips them is called over and over. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define SM_HOST
static unsigned int fake_flash[512];                 /* our 2 KB page */
#define FLASH_WORD(i) (fake_flash[i])
static unsigned int host_ram[1024];
static volatile unsigned char host_shift;
static volatile unsigned int host_mode;
#define SM_RAM     ((struct sm *)host_ram)
#define SHIFT_HELD host_shift
#define MODE       host_mode

#include "supermidi.c"

/* Stand-ins for the stock code. */
static int button_calls, led_calls, led_last_pad, led_last_colour;
static u8 *build_entry;
static u8 build_seen[8];

u32 fw_ctrl_build(u8 *obj, u8 *event, u8 *entry, u32 *a, u16 *b)
{
    int i;
    (void)obj; (void)event; (void)a; (void)b;
    build_entry = entry;
    for (i = 0; i < 8; i++)
        build_seen[i] = entry[i];
    entry[7]++;                       /* the stock code moves the value inside the entry */
    return 0;
}
void fw_led(void *leds, u32 pad, u32 colour)
{ (void)leds; led_calls++; led_last_pad = (int)pad; led_last_colour = (int)colour; }
void fw_ctrl_pad(void *mgr, u32 pad, u32 kind, u32 velocity)
{ (void)mgr; (void)pad; (void)kind; (void)velocity; }
void fw_button_press(u32 *button, u32 event)
{
    u8 *b = (u8 *)button;
    (void)event;
    button_calls++;
    b[0x0B] = 0;                      /* the stock handler's first three statements */
    b[0x0C] = 1;
    if (b[0x0A])
        b[0x08] = 1;
}
u32 fw_accel(u32 mode, u32 clicks) { (void)mode; return clicks * 3; }
void *fw_out(void) { return 0; }
/* A fake flash page: erase sets it to 1s, a word can only be written into an erased
 * word, and any address outside our page counts as a violation. */
static int fl_unlock, fl_lock, irq_off, irq_on, tick_off, tick_on, fl_bad, fl_erases, fl_words;
static int fl_fail_erase, fl_fail_at, fl_drop_at;
#define PAGE_LO 0x0803C000u
#define PAGE_HI 0x0803C800u
void fw_flash_clear(u32 f) { (void)f; }
u32 fw_flash_erase(u32 a)
{
    int i;
    fl_erases++;
    if (a != PAGE_LO) fl_bad++;
    if (fl_fail_erase) return 3;
    for (i = 0; i < 512; i++) fake_flash[i] = 0xFFFFFFFFu;
    return 4;
}
u32 fw_flash_word(u32 a, u32 w)
{
    u32 i;
    fl_words++;
    if (a < PAGE_LO || a >= PAGE_HI || (a & 3)) { fl_bad++; return 2; }
    i = (a - PAGE_LO) / 4;
    if ((int)i == fl_fail_at) return 2;
    if (fake_flash[i] != 0xFFFFFFFFu) return 2;
    if ((int)i != fl_drop_at) fake_flash[i] = w;      /* a "dropped" word reads back wrong */
    return 4;
}
void fw_flash_lock(void) { fl_lock++; }
void fw_flash_unlock(void) { fl_unlock++; }
void fw_irq_pause(u32 resume) { if (resume) irq_on++; else irq_off++; }
void fw_tick_pause(u32 resume) { if (resume) tick_on++; else tick_off++; }

static int midi_in_calls;
static u8 midi_in_last[3];
static u32 usb_log[4096];
static int usb_n, usb_refuse;
void fw_midi_in(void *obj, u8 *message)
{ (void)obj; midi_in_calls++; midi_in_last[0] = message[0]; midi_in_last[1] = message[1]; midi_in_last[2] = message[2]; }
u32 fw_midi_usb(void *out, u32 message, u32 x)
{
    (void)out; (void)x;
    if (usb_refuse)
        return 0;
    usb_log[usb_n++ & 4095] = message;
    return 1;
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static u32 button_obj[8];
static int hook_calls;

static void reset(void)
{
    int i;
    for (i = 0; i < 1024; i++) host_ram[i] = 0;
    host_shift = 0; host_mode = CONTROL_MODE;
    button_calls = 0; build_entry = 0; led_calls = 0; led_last_pad = -1; led_last_colour = -1;
    midi_in_calls = 0; usb_n = 0; usb_refuse = 0;
    for (i = 0; i < 512; i++) fake_flash[i] = 0xFFFFFFFFu;
    fl_unlock = fl_lock = irq_off = irq_on = tick_off = tick_on = fl_bad = fl_erases = fl_words = 0;
    fl_fail_erase = 0; fl_fail_at = -1; fl_drop_at = -1;
}

/* One physical press of button 0..15 (function code 0x20 + n), then the scans that
 * follow while it is held: the state machine calls the hook while a press is pending. */
static void press_button(u32 n, int scans)
{
    u8 *b = (u8 *)button_obj;
    int i;

    for (i = 0; i < 8; i++) button_obj[i] = 0;
    button_obj[4] = 0x20 + n;
    b[0x0B] = 1;                      /* press detected */
    hook_calls = 0;
    for (i = 0; i < scans; i++)
        if (b[0x0B] && b[0x0C] == 0 && b[0x08] != 2) {
            hook_calls++;
            sm_button_press(button_obj, 1);
        }
}
static void shift_button(u32 n)
{
    host_shift = 1;
    press_button(n, 6);
    host_shift = 0;
}
static void turn(u32 knob, u8 *entry)
{
    u8 event[4]; event[0] = 7; event[1] = (u8)(0x20 + knob); event[2] = 1; event[3] = 0;
    sm_ctl_build(0, event, entry, 0, 0);
}


/* ---- the live-editing protocol ---- */
static void cc(u32 number, u32 value)
{
    u8 m[3]; m[0] = 0xBF; m[1] = (u8)number; m[2] = (u8)value;
    sm_midi_in(0, m);
}
static void poll_all(void) { int i; for (i = 0; i < 100; i++) sm_poll_step(); }
/* Send a whole set; chk_add spoils the checksum, n sends more or fewer bytes. */
static void send_set(u32 id, const u8 *data, u32 n, u32 chk_add)
{
    u32 i, sum = id;
    cc(0x69, id);
    for (i = 0; i < n; i++) { cc(0x68, data[i]); sum += data[i]; }
    cc(0x6A, (sum + chk_add) & 0x7F);
}
static u32 last_status(void)       /* the last message sent must be a status: its value */
{
    u32 m = usb_log[(usb_n - 1) & 4095];
    CHECK((m & 0xFFFF) == (0xBFu | 0x6Bu << 8));
    return (m >> 16) & 0x7F;
}
/* A set of 16 slots, seven bytes each: channel code, CC, min, max, mode, accel, flags. */
static void clear_set(u8 *b) { int k, j; for (k = 0; k < 16; k++) { for (j = 0; j < 7; j++) b[7 * k + j] = 0; b[7 * k] = 0x7F; } }
static void slot(u8 *b, int k, u8 code, u8 cc_) { b[7 * k] = code; b[7 * k + 1] = cc_; }
static void extra(u8 *b, int k, u8 flags, u8 mn, u8 mx, u8 mode, u8 acc)
{ b[7 * k + 2] = mn; b[7 * k + 3] = mx; b[7 * k + 4] = mode; b[7 * k + 5] = acc; b[7 * k + 6] = flags; }
static u8 set_a[112], set_b[112];

static void power_cycle(void)        /* RAM is lost; the first pass of the main loop starts the patch again */
{ int i; for (i = 0; i < 1024; i++) host_ram[i] = 0; sm_poll_step(); }

int main(void)
{
    /* first word: channel 5 in bits 18..24 plus a message type; byte 4 is the CC */
    u8 stock[8] = {0x11, 0x00, 0x14, 0x00, 10, 0, 127, 64};
    u8 chan[8];
    u32 prof, lane, i;

    CHECK(offsetof(struct sm, alt) % 4 == 0);          /* the stock code reads entries as words */

    /* the map: buttons 1,2 -> knobs 1,9 (A,B); 3,4 -> knobs 2,10; ...; 15,16 -> knobs 8,16 */
    for (lane = 0; lane < 8; lane++) {
        reset();
        shift_button(2 * lane);      /* A */
        CHECK(profile_of(SM_RAM->bits, lane) == 1 && profile_of(SM_RAM->bits, lane + 8) == 1);
        shift_button(2 * lane + 1);  /* A and B */
        CHECK(profile_of(SM_RAM->bits, lane) == 3);
        shift_button(2 * lane);      /* B only */
        CHECK(profile_of(SM_RAM->bits, lane) == 2 && profile_of(SM_RAM->bits, lane + 8) == 2);
        shift_button(2 * lane + 1);  /* neither */
        CHECK(profile_of(SM_RAM->bits, lane) == 0 && SM_RAM->bits == 0);
    }
    reset();
    shift_button(0);
    for (i = 0; i < 16; i++)
        CHECK(profile_of(SM_RAM->bits, i) == ((i & 7) == 0 ? 1u : 0u));   /* only lane 0 */
    reset();
    shift_button(1);  CHECK(profile_of(SM_RAM->bits, 0) == 2 && profile_of(SM_RAM->bits, 1) == 0);
    shift_button(2);  CHECK(profile_of(SM_RAM->bits, 1) == 1 && profile_of(SM_RAM->bits, 0) == 2);

    /* every state of the button bits */
    for (prof = 0; prof < 65536; prof++) {
        u32 k;
        for (k = 0; k < 16; k++)
            CHECK(profile_of(prof, k) == (((prof >> (2 * (k & 7))) & 1) | (((prof >> (2 * (k & 7) + 1)) & 1) << 1)));
    }

    /* one press toggles once, however long it is held (the scans before the fix toggled on each) */
    reset(); host_shift = 1;
    press_button(4, 50);
    CHECK(hook_calls == 1 && SM_RAM->bits == (1u << 4));
    CHECK(button_calls == 0);                            /* the stock handler never ran */
    CHECK(((u8 *)button_obj)[0x0B] == 0 && ((u8 *)button_obj)[0x0C] == 1);
    press_button(4, 3);  CHECK(hook_calls == 1 && SM_RAM->bits == 0);   /* tap again: off */
    press_button(4, 3);  CHECK(SM_RAM->bits == (1u << 4));              /* and on */
    ((u8 *)button_obj)[0x0A] = 1; ((u8 *)button_obj)[0x0B] = 1; ((u8 *)button_obj)[0x0C] = 0;
    sm_button_press(button_obj, 1);
    CHECK(((u8 *)button_obj)[0x08] == 1);                /* the state the stock handler would set */

    /* without Shift, in other modes, or for other buttons the stock handler runs */
    reset();
    press_button(4, 5);  CHECK(button_calls == 1 && SM_RAM->bits == 0);
    host_shift = 1; host_mode = 1;
    press_button(4, 5);  CHECK(button_calls == 2 && SM_RAM->bits == 0);
    host_mode = CONTROL_MODE;
    for (i = 0; i < 8; i++) button_obj[i] = 0;
    button_obj[4] = 0x0C;                                 /* Shift itself */
    sm_button_press(button_obj, 1);  CHECK(button_calls == 3);
    button_obj[4] = 0x30;
    sm_button_press(button_obj, 1);  CHECK(button_calls == 4 && SM_RAM->bits == 0);
    button_obj[4] = 0x25;
    sm_button_press(button_obj, 0);  CHECK(button_calls == 5 && SM_RAM->bits == 0);   /* event 0 */

    /* lights: while Shift is held in controller mode each button shows its bit */
    reset();
    shift_button(3);
    sm_led(0, 3, 6);  CHECK(led_calls == 1 && led_last_colour == 6);      /* no Shift: stock colour */
    host_shift = 1;
    sm_led(0, 3, 6);  CHECK(led_last_pad == 3 && led_last_colour == 1);
    sm_led(0, 3, 8);  CHECK(led_last_colour == 1);
    sm_led(0, 4, 2);  CHECK(led_last_pad == 4 && led_last_colour == 8);   /* the stock purple is hidden */
    sm_led(0, 16, 2); CHECK(led_last_colour == 2);                        /* not a button */
    host_mode = 1;
    sm_led(0, 3, 6);  CHECK(led_last_colour == 6);
    host_mode = CONTROL_MODE; host_shift = 0;
    sm_led(0, 3, 4);  CHECK(led_last_colour == 4);

    /* knobs: profile 1 is the stock entry itself */
    reset(); stock[7] = 64;
    turn(0, stock);
    CHECK(build_entry == stock && stock[7] == 65);

    /* profile 2: a copy whose CC (byte 4) is +16 and whose channel word is untouched */
    reset(); stock[7] = 64; stock[4] = 10;
    for (i = 0; i < 8; i++) chan[i] = stock[i];
    shift_button(0);
    turn(0, stock);
    CHECK(build_entry != stock && build_seen[4] == 26);
    CHECK(build_seen[0] == chan[0] && build_seen[1] == chan[1] && build_seen[2] == chan[2] && build_seen[3] == chan[3]);
    CHECK(build_seen[5] == 0 && build_seen[6] == 127 && build_seen[7] == 0);   /* starts at 0 */
    CHECK(stock[7] == 64 && stock[4] == 10);
    turn(0, stock);  CHECK(build_seen[7] == 1);                   /* the copy's value moves on by itself */

    /* profiles 3 and 4, each with its own CC and value */
    shift_button(0); shift_button(1);                             /* only B: profile 3 */
    turn(0, stock);  CHECK(build_seen[4] == 42 && build_seen[7] == 0);
    shift_button(0);                                              /* A and B: profile 4 */
    turn(0, stock);  CHECK(build_seen[4] == 58 && build_seen[7] == 0);
    shift_button(1);                                              /* only A: profile 2 again */
    turn(0, stock);  CHECK(build_seen[4] == 26 && build_seen[7] == 2);
    shift_button(0);                                              /* neither: stock */
    turn(0, stock);  CHECK(build_entry == stock && stock[7] == 65);

    /* a lane's two knobs share the profile; other lanes keep theirs */
    reset(); stock[7] = 0; shift_button(0);
    turn(8, stock);   CHECK(build_seen[4] == 26);
    turn(1, stock);   CHECK(build_entry == stock);
    turn(9, stock);   CHECK(build_entry == stock);
    shift_button(2);                                              /* lane 1, bit A */
    turn(1, stock);   CHECK(build_seen[4] == 26);
    turn(9, stock);   CHECK(build_seen[4] == 26);

    /* the copy follows the stock entry, but keeps its own value */
    reset(); stock[4] = 10; stock[5] = 0; shift_button(0);
    turn(0, stock);   stock[4] = 20; stock[5] = 5;
    turn(0, stock);   CHECK(build_seen[4] == 36 && build_seen[5] == 5 && build_seen[7] == 1);

    /* Shift + knob: its own copy with CC + 64 (+ the profile offset) and its own value */
    reset(); stock[4] = 10; stock[7] = 92; stock[5] = 0; host_shift = 1;
    turn(0, stock);   CHECK(build_entry != stock && build_seen[4] == 74 && build_seen[7] == 0 && stock[7] == 92);
    turn(0, stock);   CHECK(build_seen[4] == 74 && build_seen[7] == 1);
    host_shift = 0;
    turn(0, stock);   CHECK(build_entry == stock && stock[7] == 93);   /* plain turn: stock, unmoved by the shift copy */
    shift_button(0);  shift_button(1);                                 /* profile 4 */
    host_shift = 1;
    turn(0, stock);   CHECK(build_seen[4] == ((10 + 48 + 64) & 0x7F) && build_seen[7] == 0);
    host_shift = 0;

    /* turns in either direction; other events and other modes are left alone */
    reset(); stock[4] = 10; shift_button(0);
    { u8 ev[4] = {8, 0x20, 1, 0}; sm_ctl_build(0, ev, stock, 0, 0); CHECK(build_seen[4] == 26); }
    { u8 ev[4] = {7, 0x30, 1, 0}; sm_ctl_build(0, ev, stock, 0, 0); CHECK(build_entry == stock); }
    { u8 ev[4] = {4, 0x20, 1, 0}; sm_ctl_build(0, ev, stock, 0, 0); CHECK(build_entry == stock); }
    host_mode = 1;
    { u8 ev[4] = {7, 0x20, 1, 0}; sm_ctl_build(0, ev, stock, 0, 0); CHECK(build_entry == stock); }
    host_mode = CONTROL_MODE;

    /* the stock acceleration is kept */
    CHECK(sm_accel(2, 2) == 6);
    host_shift = 1;  CHECK(sm_accel(2, 2) == 6);
    host_shift = 0;

    /* start-up: random RAM is cleared once, then kept */
    for (i = 0; i < 1024; i++) host_ram[i] = 0xA5A5A5A5u;
    host_mode = CONTROL_MODE; host_shift = 1; press_button(2, 3);
    CHECK(SM_RAM->bits == 4 && SM_RAM->magic == MAGIC);


    /* ======== live editing over USB MIDI ======== */
    CHECK(sizeof(struct sm) <= 3072);                    /* patch RAM budget */
    clear_set(set_a); clear_set(set_b);
    slot(set_a, 0, 5, 99);                               /* knob 1: channel 6, CC 99 */
    slot(set_a, 1, 0x7E, 23);                            /* knob 2: stock channel, CC 23 */
    extra(set_a, 0, 7, 10, 100, 2, 3);                   /* range 10..100, mode 2, acceleration 3 */

    /* messages that are not ours go to the stock code untouched */
    reset();
    { u8 m[3] = {0xBE, 0x69, 1}; sm_midi_in(0, m); }
    { u8 m[3] = {0xBF, 0x67, 1}; sm_midi_in(0, m); }
    { u8 m[3] = {0xBF, 0x6E, 1}; sm_midi_in(0, m); }
    { u8 m[3] = {0x9F, 0x69, 1}; sm_midi_in(0, m); }
    CHECK(midi_in_calls == 4 && midi_in_last[0] == 0x9F && midi_in_last[1] == 0x69 && midi_in_last[2] == 1);
    cc(0x69, 1); CHECK(midi_in_calls == 4 && SM_RAM->rx_state == 1);

    /* a good set is applied and answered with OK after the main loop runs */
    reset();
    send_set(1, set_a, 112, 0);
    CHECK(usb_n == 0);                                                    /* nothing sent from the input hook */
    poll_all();
    CHECK(usb_n == 1 && last_status() == 0);
    CHECK(SM_RAM->cfg[1][0][0] == 5 && SM_RAM->cfg[1][0][1] == 99 && SM_RAM->cfg[1][0][6] == 7 && SM_RAM->cfg[1][1][1] == 23);
    CHECK(SM_RAM->cfg[2][0][0] == 0x7F && SM_RAM->get_set == 0xFF);

    /* refusals change nothing */
    reset(); send_set(1, set_a, 112, 0); poll_all();
    send_set(1, set_b, 112, 1);  poll_all(); CHECK(last_status() == 1 && SM_RAM->cfg[1][0][1] == 99);   /* checksum */
    send_set(8, set_b, 112, 0);  poll_all(); CHECK(last_status() == 2);                                  /* no such set */
    send_set(0, set_b, 112, 0);  poll_all(); CHECK(last_status() == 3);                                  /* stock profile */
    send_set(1, set_b, 111, 0);  poll_all(); CHECK(last_status() == 5 && SM_RAM->cfg[1][0][1] == 99);   /* too short */
    { u8 big[113]; for (i = 0; i < 113; i++) big[i] = (i % 7 == 0) ? 0x7F : 0;
      send_set(1, big, 113, 0); poll_all(); CHECK(last_status() == 5 && SM_RAM->cfg[1][0][1] == 99); }  /* too long */
    { u8 bad[112]; int k; struct { int k; u8 code, flags, mn, mx, mode, acc; } cases[6] = {
          {3, 0x20, 0, 0, 0, 0, 0},      /* channel code out of range */
          {4, 0x7F, 8, 0, 0, 0, 0},      /* unknown flag */
          {5, 0x7F, 1, 90, 10, 0, 0},    /* minimum above maximum */
          {6, 0x7F, 2, 0, 0, 4, 0},      /* mode 4 */
          {7, 0x7F, 4, 0, 0, 0, 5},      /* acceleration 5 */
          {8, 0x7F, 0, 0, 0, 0, 0} };    /* a good one, to be sure the harness accepts */
      for (k = 0; k < 6; k++) {
          clear_set(bad); bad[7 * cases[k].k] = cases[k].code;
          extra(bad, cases[k].k, cases[k].flags, cases[k].mn, cases[k].mx, cases[k].mode, cases[k].acc);
          send_set(2, bad, 112, 0); poll_all();
          CHECK(last_status() == (k == 5 ? 0u : 4u));
          CHECK(k == 5 || SM_RAM->cfg[2][cases[k].k][0] == 0x7F);
      } }
    cc(0x6A, 0);                 poll_all(); CHECK(last_status() == 5);                                  /* END with no BEGIN */
    cc(0x6D, 5);                 poll_all(); CHECK(last_status() == 4);                                  /* not a save or erase code */
    CHECK(fl_erases == 0 && fl_words == 0);
    CHECK(SM_RAM->cfg[1][0][0] == 5);

    /* a new BEGIN starts over */
    reset();
    cc(0x69, 2); cc(0x68, 1); cc(0x68, 2);
    send_set(3, set_a, 112, 0); poll_all();
    CHECK(last_status() == 0 && SM_RAM->cfg[3][0][1] == 99 && SM_RAM->cfg[2][0][0] == 0x7F);

    /* GET sends the set back: BEGIN, 112 bytes, END with the checksum, produced on demand */
    reset(); send_set(5, set_a, 112, 0); poll_all(); usb_n = 0;
    cc(0x6C, 5);
    CHECK(usb_n == 0);                                                    /* nothing from the input hook */
    poll_all();
    CHECK(usb_n == 114 && usb_log[0] == (0xBFu | 0x69u << 8 | 5u << 16));
    { u32 sum = 5, ok = 1;
      for (i = 0; i < 112; i++) {
          u32 m = usb_log[1 + i], v = (m >> 16) & 0x7F;
          if ((m & 0xFFFF) != (0xBFu | 0x68u << 8) || v != set_a[i]) ok = 0;
          sum += v;
      }
      CHECK(ok);
      CHECK(usb_log[113] == (0xBFu | 0x6Au << 8 | (sum & 0x7Fu) << 16)); }
    usb_n = 0; cc(0x6C, 0); poll_all();                                   /* the stock profile reads as all auto */
    CHECK(usb_n == 114 && ((usb_log[1] >> 16) & 0x7F) == 0x7F && ((usb_log[2] >> 16) & 0x7F) == 0 && ((usb_log[8] >> 16) & 0x7F) == 0x7F);
    CHECK(((usb_log[113] >> 16) & 0x7F) == ((0 + 16 * 0x7F) & 0x7F));
    usb_n = 0; cc(0x6C, 9); poll_all(); CHECK(usb_n == 1 && last_status() == 2);

    /* a second GET while one is being sent answers busy, after the reply; nothing garbled */
    reset(); send_set(1, set_a, 112, 0); poll_all(); usb_n = 0;
    cc(0x6C, 1); cc(0x6C, 2);
    poll_all();
    CHECK(usb_n == 115 && last_status() == 7);
    CHECK(usb_log[0] == (0xBFu | 0x69u << 8 | 1u << 16) && (usb_log[113] & 0xFFFF) == (0xBFu | 0x6Au << 8));

    /* the main loop sends at most 16 a pass and retries what the output refuses */
    reset(); send_set(1, set_a, 112, 0); cc(0x6C, 1);                     /* a status, then a reply */
    usb_n = 0; sm_poll_step();  CHECK(usb_n == 16);
    usb_refuse = 1; sm_poll_step(); sm_poll_step();  CHECK(usb_n == 16);  /* refused: kept */
    usb_refuse = 0; poll_all();  CHECK(usb_n == 115);
    CHECK(usb_log[0] == (0xBFu | 0x69u << 8 | 1u << 16) && usb_log[114] == (0xBFu | 0x6Bu << 8));   /* reply, then the status */
    CHECK(SM_RAM->tx_head == SM_RAM->tx_tail && SM_RAM->get_set == 0xFF);

    /* the status queue wraps without losing order */
    reset();
    for (i = 0; i < 150; i++) { send_set(1 + (i % 7), set_b, 112, 0); poll_all(); CHECK(last_status() == 0); }
    CHECK(usb_n == 150);

    /* the editor's settings drive the knobs: set id = layer * 4 + profile */
    reset(); stock[4] = 10; stock[2] = 0x14; stock[3] = 0; stock[1] = 0x09; stock[5] = 0; stock[6] = 127;
    shift_button(0);                                                      /* lane 0, profile 2 (set 1) */
    send_set(1, set_a, 112, 0); poll_all();
    turn(0, stock);
    CHECK(build_seen[4] == 99 && (build_seen[2] >> 2) == 5 && (build_seen[3] & 1) == 0);   /* CC 99, channel 6 */
    CHECK(build_seen[5] == 10 && build_seen[6] == 100);                                    /* range */
    CHECK((build_seen[1] & 7) == 2 && ((build_seen[1] >> 3) & 7) == 3);                    /* mode 2, acceleration 3 */
    CHECK((build_seen[1] & 0xC0) == (stock[1] & 0xC0) && build_seen[0] == stock[0]);       /* other bits kept */
    turn(1, stock);                                                      /* knob 1 is another lane: stock */
    CHECK(build_entry == stock);
    turn(8, stock);                                                      /* knob 8 shares lane 0: auto, no flags */
    CHECK(build_seen[4] == 26 && build_seen[1] == stock[1] && build_seen[5] == 0 && build_seen[6] == 127);
    clear_set(set_b); slot(set_b, 1, 0x7E, 44); stock[2] = 0x28;
    shift_button(2);                                                     /* lane 1, profile 2 */
    send_set(1, set_b, 112, 0); poll_all();
    turn(1, stock);
    CHECK(build_seen[4] == 44 && build_seen[2] == 0x28);                 /* channel word untouched */
    turn(0, stock);                                                      /* knob 0 is auto again: stock CC + 16 */
    CHECK(build_seen[4] == 26 && build_seen[1] == stock[1]);

    /* an auto slot can still override range, and only the flagged parts change */
    reset(); stock[4] = 10; stock[1] = 0x1B; stock[5] = 3; stock[6] = 120;   /* mode 3, acceleration 3 */
    clear_set(set_b); extra(set_b, 0, 4, 0, 0, 0, 4);                    /* acceleration only: global */
    shift_button(0); send_set(1, set_b, 112, 0); poll_all();
    turn(0, stock);
    CHECK(build_seen[4] == 26 && (build_seen[1] & 7) == 3 && ((build_seen[1] >> 3) & 7) == 4 && build_seen[5] == 3 && build_seen[6] == 120);

    /* the Shift layer has its own sets (4 to 7) */
    reset(); stock[4] = 10; stock[2] = 0x14; stock[1] = 0;
    send_set(4, set_a, 112, 0); poll_all();                              /* layer 1, profile 1 */
    host_shift = 1; turn(0, stock);
    CHECK(build_seen[4] == 99 && (build_seen[2] >> 2) == 5 && build_seen[5] == 10);
    host_shift = 0; turn(0, stock);  CHECK(build_entry == stock);        /* plain turn: stock */
    shift_button(0); shift_button(1);                                    /* profile 4: shift layer set 7 */
    host_shift = 1; turn(0, stock);
    CHECK(build_seen[4] == ((10 + 48 + 64) & 0x7F));                     /* set 7 is still auto */
    host_shift = 0;

    /* a wrong channel byte can never reach the entry */
    reset(); stock[2] = 0x14;
    send_set(1, set_a, 112, 0); poll_all();
    shift_button(0); turn(0, stock);
    CHECK((build_seen[2] >> 2) <= 15);


    /* ======== saving to flash ======== */
    {
        const u8 *cfg = (const u8 *)SM_RAM->cfg;
        u32 sum, k;

        /* nothing touches flash until COMMIT, and the hook itself never does */
        reset(); clear_set(set_a); slot(set_a, 0, 5, 99); extra(set_a, 0, 7, 10, 100, 2, 3);
        slot(set_a, 9, 0x7E, 7);
        send_set(1, set_a, 112, 0); poll_all();
        CHECK(fl_erases == 0 && fl_words == 0 && fl_unlock == 0);
        cc(0x6D, 0x2A);
        CHECK(fl_erases == 0 && fl_words == 0);                          /* the interrupt side only notes the job */
        sm_poll_step();
        CHECK(fl_erases == 1 && fl_words == 199 && fl_bad == 0);
        CHECK(fl_unlock == 1 && fl_lock == 1 && irq_off == 1 && irq_on == 1 && tick_off == 1 && tick_on == 1);
        CHECK(last_status() == 0);
        CHECK(fake_flash[0] == 0x534D4333u && fake_flash[1] == (1u << 16 | 784u));
        sum = checksum_of(cfg + 112, 784);
        CHECK(fake_flash[2] == sum);
        for (k = 0; k < 784; k++) if (((fake_flash[3 + k / 4] >> (8 * (k % 4))) & 0xFF) != cfg[112 + k]) break;
        CHECK(k == 784);

        /* power cycle: the saved profiles come back, the rest stays auto */
        for (k = 0; k < 1024; k++) host_ram[k] = 0xA5A5A5A5u;
        sm_poll_step();
        CHECK(SM_RAM->cfg[1][0][0] == 5 && SM_RAM->cfg[1][0][1] == 99 && SM_RAM->cfg[1][0][6] == 7 && SM_RAM->cfg[1][0][3] == 100);
        CHECK(SM_RAM->cfg[1][9][0] == 0x7E && SM_RAM->cfg[1][9][1] == 7);
        CHECK(SM_RAM->cfg[2][0][0] == 0x7F && SM_RAM->cfg[0][0][0] == 0x7F && SM_RAM->get_set == 0xFF && SM_RAM->commit == 0);
        CHECK(fl_erases == 1);                                           /* loading only reads */

        /* the saved profiles drive the knobs after a restart, and can still be edited */
        stock[4] = 10; stock[2] = 0x14; stock[1] = 0; stock[5] = 0; stock[6] = 127; host_mode = CONTROL_MODE;
        shift_button(0); turn(0, stock);
        CHECK(build_seen[4] == 99 && build_seen[5] == 10 && build_seen[6] == 100);
        clear_set(set_b); slot(set_b, 0, 0x7E, 12);
        send_set(1, set_b, 112, 0); poll_all();
        CHECK(SM_RAM->cfg[1][0][1] == 12 && fake_flash[2] == sum);       /* RAM changes, flash waits for a COMMIT */

        /* a damaged copy is ignored whole: flipped bit, wrong magic, wrong format, a bad slot */
        reset(); send_set(1, set_a, 112, 0); cc(0x6D, 0x2A); poll_all();
        { u32 good[512]; int i;
          for (i = 0; i < 512; i++) good[i] = fake_flash[i];
          fake_flash[10] ^= 0x100;
          power_cycle();
          CHECK(SM_RAM->cfg[1][0][0] == 0x7F);                           /* checksum catches it */
          for (i = 0; i < 512; i++) fake_flash[i] = good[i];
          fake_flash[0] ^= 1;
          power_cycle();
          CHECK(SM_RAM->cfg[1][0][0] == 0x7F);
          fake_flash[0] = good[0]; fake_flash[1] = 2u << 16 | 784u;
          power_cycle();
          CHECK(SM_RAM->cfg[1][0][0] == 0x7F);
          for (i = 0; i < 512; i++) fake_flash[i] = good[i];             /* a copy whose checksum is right but a slot is not */
          fake_flash[3] = (fake_flash[3] & 0xFFFFFF00u) | 0x20;         /* channel byte 0x20 */
          { u8 bytes[784]; int j;
            for (j = 0; j < 784; j++) bytes[j] = (u8)((fake_flash[3 + j / 4] >> (8 * (j % 4))) & 0xFF);
            fake_flash[2] = checksum_of(bytes, 784); }
          power_cycle();
          CHECK(SM_RAM->cfg[1][0][0] == 0x7F && SM_RAM->cfg[1][1][0] == 0x7F);
          for (i = 0; i < 512; i++) fake_flash[i] = good[i];             /* and the good copy loads again */
          power_cycle();
          CHECK(SM_RAM->cfg[1][0][0] == 5); }

        /* the checksum notices a swap of two bytes */
        { u8 x[4] = {1, 2, 3, 4}, y[4] = {2, 1, 3, 4};
          CHECK(checksum_of(x, 4) != checksum_of(y, 4)); }

        /* erase: the page goes blank and the next start-up has the defaults */
        reset(); send_set(1, set_a, 112, 0); cc(0x6D, 0x2A); poll_all();
        fl_erases = fl_words = 0;
        cc(0x6D, 0x2B); poll_all();
        CHECK(fl_erases == 1 && fl_words == 0 && last_status() == 0 && fl_bad == 0);
        { int blank = 1, i; for (i = 0; i < 512; i++) if (fake_flash[i] != 0xFFFFFFFFu) blank = 0; CHECK(blank); }
        power_cycle();
        CHECK(SM_RAM->cfg[1][0][0] == 0x7F);

        /* a second commit while one is waiting answers busy, the first still runs */
        reset(); send_set(1, set_a, 112, 0); poll_all(); usb_n = 0;
        cc(0x6D, 0x2A); cc(0x6D, 0x2A); poll_all();
        CHECK(usb_n == 2 && (usb_log[0] >> 16 & 0x7F) == 7 && (usb_log[1] >> 16 & 0x7F) == 0 && fl_erases == 1);

        /* failures: the table in RAM is untouched, the interrupts and tick come back, the status says so */
        reset(); send_set(1, set_a, 112, 0); poll_all(); fl_fail_erase = 1; usb_n = 0;
        cc(0x6D, 0x2A); poll_all();
        CHECK(last_status() == 8 && fl_words == 0 && SM_RAM->cfg[1][0][1] == 99);
        CHECK(fl_unlock == 1 && fl_lock == 1 && irq_off == 1 && irq_on == 1 && tick_off == 1 && tick_on == 1);
        reset(); send_set(1, set_a, 112, 0); poll_all(); fl_fail_at = 50;
        cc(0x6D, 0x2A); poll_all();
        CHECK(last_status() == 8 && fl_words == 51 && fl_lock == 1 && irq_on == 1 && tick_on == 1);
        power_cycle();
        CHECK(SM_RAM->cfg[1][0][0] == 0x7F);                             /* a half-written page is not loaded */
        reset(); send_set(1, set_a, 112, 0); poll_all(); fl_drop_at = 10;
        cc(0x6D, 0x2A); poll_all();
        CHECK(last_status() == 8 && fl_lock == 1 && irq_on == 1);        /* read-back caught it */

        /* ordinary editing never touches flash, and nothing ever leaves our page */
        reset();
        for (k = 0; k < 20; k++) { send_set(1 + k % 7, set_a, 112, 0); cc(0x6C, 1 + k % 7); poll_all(); }
        CHECK(fl_erases == 0 && fl_words == 0 && fl_unlock == 0 && fl_bad == 0);
    }

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
