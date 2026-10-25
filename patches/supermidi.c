/* superMIDI controller 1.0.0, for BeatStep Pro firmware 2.0.1.6. MIDI controller mode only
 * (mode 3). See README.md and docs/.
 *
 * Profiles. Eight lanes: knob n and knob n+8 form a lane. The lane's two profile bits are
 * the 16 step buttons under the knobs, buttons 2n-1 (bit A) and 2n (bit B): buttons 1 and
 * 2 for knobs 1 and 9, 3 and 4 for knobs 2 and 10, and so on. Hold Shift and press a button
 * to toggle its bit; while Shift is held the buttons light for the bits that are set.
 * Neither bit is profile 1 (the stock setup, untouched), A is profile 2, B is profile 3,
 * both are profile 4. Profiles 2 to 4, and every profile on the Shift layer (Shift + knob),
 * use their own copies of the knob's stock setup entry, each with its own stored value.
 * Switching profile sends no MIDI.
 *
 * Settings. A profile's slot can override the CC (byte 4 of the entry), the channel
 * (bits 18..24 of its first word), the range (bytes 5 and 6), the knob mode (byte 1 bits
 * 0..2) and the acceleration (byte 1 bits 3..5); what it does not override follows the
 * stock setup. A slot left on "auto" sends the stock CC plus CC_STEP per profile (plus
 * SHIFT_CC on the Shift layer). The settings are edited over USB MIDI with plain Control
 * Change messages on channel 16 (docs/protocol.md). They live in RAM until the host sends
 * COMMIT 0x2A, which writes them to the blank flash page at 0x0803C000 (outside the update
 * image; nothing else in the firmware uses it); they are loaded back at start-up if the
 * page holds a complete, correct copy. COMMIT 0x2B erases the page. The messages arrive in
 * an interrupt, so replies are queued and sent from the main loop, which is also where
 * flash is written.
 *
 * Hooks (calls in the stock code, replaced the way the other patches do it):
 *   0x080225D6 control-event function builds a message from a setup entry
 *   0x0802FA28 encoder handler looks up the acceleration step (passed through unchanged)
 *   0x08019A0C polled button scanner: the press of a step button
 *   0x0801B296 MIDI input: a finished three-byte channel message goes to the stock code
 *   0x0800C196 main loop: the call to the encoder poll, where replies and saving happen
 *   and the stock light calls in the routine at 0x080087D8 (see tools/install_supermidi.py)
 */
#include "fw.h"

#ifndef SM_HOST
#define FLASH_WORD(i) (((volatile u32 *)FLASH_PAGE)[i])
#define SM_RAM     ((struct sm *)0x20006000)        /* patch RAM, not cleared at start-up */
#define SHIFT_HELD (*(volatile u8 *)0x20000199)     /* 1 while the Shift button is down */
#define MODE       (*(volatile u32 *)0x200001F0)    /* 3: MIDI controller mode */
#endif

#define CONTROL_MODE 3
#define KNOBS        16
#define PADS         16
#define MAGIC        0x534D4331u                    /* "SMC1" */
#define CC_STEP      16                             /* "auto" CC offset per profile */
#define SHIFT_CC     64                             /* "auto" extra offset on the Shift layer */

#define SETS         8                              /* layer * 4 + profile */
#define SLOT_BYTES   7                              /* channel, cc, min, max, mode, accel, flags */
#define SET_BYTES    (SLOT_BYTES * KNOBS)
#define TXQ          32                             /* status queue, a power of two */
#define F_RANGE      1                              /* flags: minimum and maximum override */
#define F_MODE       2                              /* flags: knob mode overrides (0..3) */
#define F_ACCEL      4                              /* flags: acceleration overrides (0..4, 4 = global) */

/* Messages are Control Change on channel 16 (status 0xBF). */
#define PROTO_STATUS 0xBF
#define CC_DATA      0x68                           /* one 7-bit byte of a set */
#define CC_BEGIN     0x69                           /* value = set id; starts a set */
#define CC_END       0x6A                           /* value = (set id + sum of bytes) & 0x7F */
#define CC_STATUS    0x6B                           /* device to host: result of a set */
#define CC_GET       0x6C                           /* value = set id; the device sends it back */
#define CC_COMMIT    0x6D                           /* value 0x2A saves, 0x2B erases the saved copy */

#define ST_OK        0
#define ST_CHECKSUM  1
#define ST_BAD_SET   2
#define ST_READ_ONLY 3
#define ST_BAD_DATA  4
#define ST_BAD_LEN   5
#define ST_BUSY      7
#define ST_FLASH     8                              /* erase, program or verify failed */

#define COMMIT_SAVE  0x2A                           /* CC_COMMIT values */
#define COMMIT_ERASE 0x2B

#define FLASH_PAGE   0x0803C000u                    /* the one page we ever touch (blank in the image) */
#define FLASH_MAGIC  0x534D4333u                    /* "SMC3" */
#define FLASH_FORMAT 1u
#define SAVED_BYTES  ((SETS - 1) * SET_BYTES)       /* sets 1 to 7 */
#define FLASH_WORDS  (3 + SAVED_BYTES / 4)          /* magic, format and length, checksum, data */

#define SLOT_AUTO    0x7F                           /* first byte: "auto", offsets from the stock CC */
#define SLOT_KEEP_CH 0x7E                           /* first byte: own CC, channel stays the stock one */

#define KIND_PAD_DOWN 4
#define KIND_PAD_UP   6
#define KIND_TURN_R   7
#define KIND_TURN_L   8

/* The input hook runs in an interrupt, the queue is drained in the main loop. */
#ifndef SM_HOST
static inline u32 irq_save(void)
{
    u32 primask;

    __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) : : "memory");
    return primask;
}
static inline void irq_restore(u32 primask)
{
    __asm__ volatile("msr primask, %0" : : "r"(primask) : "memory");
}
#else
#define irq_save()        0u
#define irq_restore(p)    ((void)(p))
#endif

/* Stock functions (addresses in fw.ld). */
u32 fw_ctrl_build(u8 *obj, u8 *event, u8 *entry, u32 *a, u16 *b);
void fw_button_press(u32 *button, u32 event);
u32 fw_accel(u32 mode, u32 clicks);
void *fw_out(void);
u32 fw_midi_usb(void *out, u32 message, u32 x);
void fw_led(void *leds, u32 pad, u32 colour);
void fw_midi_in(void *obj, u8 *message);
/* The stock flash routines (the ones 0x08004498 uses to save its own settings). */
void fw_flash_clear(u32 flags);
u32 fw_flash_erase(u32 address);                    /* 4 = done */
u32 fw_flash_word(u32 address, u32 word);           /* 4 = done */
void fw_flash_lock(void);
void fw_flash_unlock(void);
void fw_irq_pause(u32 resume);                      /* 0: interrupts off, 1: back on */
void fw_tick_pause(u32 resume);

struct sm {
    u32 magic;
    u16 bits;                    /* bit p set: profile bit p is on */
    u8 valid[2][4][KNOBS];       /* layer/profile/knob entry has been started */
    u16 align_alt;              /* keep stock 32-bit entry reads aligned */
    u8 alt[2][4][KNOBS][8];      /* independent setup entries; byte 7 is the value */
    u8 cfg[SETS][KNOBS][SLOT_BYTES];   /* the editable profiles */
    u8 rx[SET_BYTES];            /* the set being received */
    u8 rx_n;                     /* bytes received so far */
    u8 rx_set;                   /* its set id */
    u8 rx_state;                 /* 0 idle, 1 receiving, 2 too many bytes */
    u8 get_set;                  /* set being sent back, 0xFF when none */
    u8 get_pos;                  /* next message of that reply: 0 begin, 1..112 data, 113 end */
    u8 commit;                   /* flash job for the main loop: 0 none, 1 save, 2 erase */
    u8 tx_head;                  /* status queue: written by the input hook, */
    u8 tx_tail;                  /* read by the main loop */
    u16 tx[TXQ];                 /* (controller << 8) | value */
};

/* ---- saving to flash ------------------------------------------------------ */

static u32 slot_ok(const u8 *b);

static u32 checksum_of(const u8 *data, u32 n)       /* two running sums, so order matters */
{
    u32 a = 0, c = 0, i;

    for (i = 0; i < n; i++) {
        a = (a + data[i]) & 0xFFFF;
        c = (c + a) & 0xFFFF;
    }
    return c << 16 | a;
}

static u32 saved_byte(u32 n)                        /* byte n of the saved data in flash */
{
    return (FLASH_WORD(3 + n / 4) >> (8 * (n % 4))) & 0xFF;
}

/* Copy the saved profiles into the table if the page holds a complete, correct copy. */
static void flash_load(struct sm *s)
{
    u8 slot[SLOT_BYTES];
    u32 a = 0, c = 0, n, i;

    if (FLASH_WORD(0) != FLASH_MAGIC || FLASH_WORD(1) != (FLASH_FORMAT << 16 | SAVED_BYTES))
        return;
    for (n = 0; n < SAVED_BYTES; n++) {
        a = (a + saved_byte(n)) & 0xFFFF;
        c = (c + a) & 0xFFFF;
    }
    if (FLASH_WORD(2) != (c << 16 | a))
        return;
    for (n = 0; n < SAVED_BYTES; n += SLOT_BYTES) {
        for (i = 0; i < SLOT_BYTES; i++)
            slot[i] = (u8)saved_byte(n + i);
        if (!slot_ok(slot))
            return;
    }
    for (n = 0; n < SAVED_BYTES; n++)
        s->cfg[1 + n / SET_BYTES][(n % SET_BYTES) / SLOT_BYTES][n % SLOT_BYTES] = (u8)saved_byte(n);
}

/* Erase our page, and unless erase_only write the profiles into it and check them.
 * Only addresses inside the page are ever passed to the stock routines. Same steps as
 * the stock settings save: interrupts and the tick off, unlock, clear flags, erase,
 * program word by word, lock, back on. 1 = done. */
static u32 flash_write(struct sm *s, u32 erase_only)
{
    const u8 *data = (const u8 *)s->cfg + SET_BYTES;       /* sets 1 to 7 are contiguous */
    u32 ok = 1, i;

    fw_irq_pause(0);
    fw_tick_pause(0);
    fw_flash_unlock();
    fw_flash_clear(0x35);
    if (fw_flash_erase(FLASH_PAGE) != 4)
        ok = 0;
    for (i = 0; ok && !erase_only && i < FLASH_WORDS; i++) {
        u32 w;

        if (i == 0)
            w = FLASH_MAGIC;
        else if (i == 1)
            w = FLASH_FORMAT << 16 | SAVED_BYTES;
        else if (i == 2)
            w = checksum_of(data, SAVED_BYTES);
        else
            w = (u32)data[(i - 3) * 4] | (u32)data[(i - 3) * 4 + 1] << 8 |
                (u32)data[(i - 3) * 4 + 2] << 16 | (u32)data[(i - 3) * 4 + 3] << 24;
        if (fw_flash_word(FLASH_PAGE + 4 * i, w) != 4 || FLASH_WORD(i) != w)
            ok = 0;
    }
    fw_flash_lock();
    fw_irq_pause(1);
    fw_tick_pause(1);
    return ok;
}

static struct sm *ready(void)
{
    struct sm *s = SM_RAM;
    u8 *p = (u8 *)s;
    u32 i;

    if (s->magic != MAGIC) {
        for (i = 0; i < sizeof(*s); i++)
            p[i] = 0;
        for (i = 0; i < SETS * KNOBS; i++)
            s->cfg[i / KNOBS][i % KNOBS][0] = SLOT_AUTO;
        s->get_set = 0xFF;
        flash_load(s);
        s->magic = MAGIC;
    }
    return s;
}

/* Lane n has button 2n (bit A) and button 2n + 1 (bit B), counting from 0. */
static u32 profile_of(u32 bits, u32 knob)
{
    u32 lane = knob & 7;

    return ((bits >> (2 * lane)) & 1) | (((bits >> (2 * lane + 1)) & 1) << 1);
}

/* Replaces the call that builds the message from a control's setup entry.
 * Main layer/profile 1 stays stock. All other (layer, profile, knob) combinations
 * get their own copied entry, including an independent byte-7 value. */
u32 sm_ctl_build(u8 *obj, u8 *event, u8 *entry, u32 *a, u16 *b)
{
    if (MODE == CONTROL_MODE && (event[0] == KIND_TURN_R || event[0] == KIND_TURN_L) &&
        event[1] >= 0x20 && event[1] < 0x20 + KNOBS) {
        struct sm *s = ready();
        u32 knob = event[1] - 0x20;
        u32 prof = profile_of(s->bits, knob);
        u32 layer = SHIFT_HELD ? 1u : 0u;

        if (layer || prof) {
            u8 *alt = s->alt[layer][prof][knob];
            u32 i;

            for (i = 0; i < 7; i++)
                alt[i] = entry[i];
            if (!s->valid[layer][prof][knob]) {
                /* Each alternate slot starts at zero, independent of stock/current value. */
                alt[7] = 0;
                s->valid[layer][prof][knob] = 1;
            }
            /* Byte 4 is the CC number; bits 18..24 of the first word are the channel
             * (byte 2 bits 2..7 and byte 3 bit 0). An assigned slot overrides them. */
            const u8 *slot = s->cfg[layer * 4 + prof][knob];

            if (slot[0] == SLOT_AUTO) {
                alt[4] = (u8)((entry[4] + CC_STEP * prof + (layer ? SHIFT_CC : 0)) & 0x7F);
            } else {
                alt[4] = slot[1] & 0x7F;
                if (slot[0] <= 15) {
                    alt[2] = (u8)((alt[2] & 0x03) | (slot[0] << 2));
                    alt[3] = (u8)(alt[3] & 0xFE);
                }
            }
            /* Byte 1 bits 0..2 are the knob mode and bits 3..5 the acceleration;
             * bytes 5 and 6 are the range. Only what the flags name is overridden. */
            if (slot[6] & F_RANGE) {
                alt[5] = slot[2];
                alt[6] = slot[3];
            }
            if (slot[6] & F_MODE)
                alt[1] = (u8)((alt[1] & ~0x07) | (slot[4] & 7));
            if (slot[6] & F_ACCEL)
                alt[1] = (u8)((alt[1] & ~0x38) | ((slot[5] & 7) << 3));
            entry = alt;
        }
    }
    return fw_ctrl_build(obj, event, entry, a, b);
}

/* Keep stock encoder acceleration. Shift-layer independence is implemented by
 * selecting a separate control entry in sm_ctl_build, not by multiplying speed. */
u32 sm_accel(u32 mode, u32 clicks)
{
    return fw_accel(mode, clicks);
}


/* Replaces LED redraws inside the stock display refresh routine. While Shift is
 * held in Controller mode, show the profile bits directly on all 16 pads. Colour
 * 1 is a single LED channel; colour 8 is off. The normal renderer is used for
 * every other mode/state, so stock Controller-mode lights resume on Shift release. */
void sm_led(void *leds, u32 pad, u32 colour)
{
    struct sm *s = ready();

    if (MODE == CONTROL_MODE && SHIFT_HELD && pad < PADS)
        colour = (s->bits & (u16)(1u << pad)) ? 1u : 8u;
    fw_led(leds, pad, colour);
}

/* Hook only the polled per-button state machine. It calls the press handler with
 * event 1 on every scan while a press is pending (byte +0x0B set) and the button
 * is not yet marked pressed (+0x0C clear). The stock handler clears +0x0B and sets
 * +0x0C as its first act, which is what makes it one call per press. A swallowed
 * press must do the same, or the bit would toggle on every scan until release. */
void sm_button_press(u32 *button, u32 event)
{
    u32 code = button[4];

    if (MODE == CONTROL_MODE && SHIFT_HELD && event != 0 &&
        code >= 0x20 && code < 0x20 + PADS) {
        struct sm *s = ready();
        u32 id = code - 0x20;
        u8 *state = (u8 *)button;

        s->bits ^= (u16)(1u << id);
        /* The state machine keeps calling this hook on every scan while the button
         * is down until the stock handler has run its first three statements, so do
         * those ourselves: clear the press-pending flag, mark the button pressed,
         * and move state 0 to 1 if flag +0x0A is set. */
        state[0x0B] = 0;
        state[0x0C] = 1;
        if (state[0x0A])
            state[0x08] = 1;
        return;                         /* profile selection sends no MIDI */
    }
    fw_button_press(button, event);
}

/* ---- live editing over USB MIDI ------------------------------------------ */

/* Queue one status message, or drop it if the queue is full. Runs in interrupts. */
static void tx_put(struct sm *s, u32 cc, u32 value)
{
    u32 primask = irq_save();
    u32 next = (u32)(s->tx_head + 1) & (TXQ - 1);

    if (next != s->tx_tail) {
        s->tx[s->tx_head] = (u16)(cc << 8 | (value & 0x7F));
        s->tx_head = (u8)next;
    }
    irq_restore(primask);
}

static void set_done(struct sm *s, u32 status)
{
    s->rx_state = 0;
    tx_put(s, CC_STATUS, status);
}

/* Is the received slot acceptable? */
static u32 slot_ok(const u8 *b)
{
    if (b[0] > 15 && b[0] != SLOT_AUTO && b[0] != SLOT_KEEP_CH)
        return 0;
    if (b[6] > (F_RANGE | F_MODE | F_ACCEL))
        return 0;
    if ((b[6] & F_RANGE) && b[2] > b[3])
        return 0;
    if ((b[6] & F_MODE) && b[4] > 3)
        return 0;
    if ((b[6] & F_ACCEL) && b[5] > 4)
        return 0;
    return 1;
}

/* CC_END: the set is complete. Check everything before changing anything. */
static void set_end(struct sm *s, u32 checksum)
{
    u32 sum = s->rx_set;
    u32 i;

    if (s->rx_state != 1 || s->rx_n != SET_BYTES) {
        set_done(s, ST_BAD_LEN);
        return;
    }
    if (s->rx_set >= SETS) {
        set_done(s, ST_BAD_SET);
        return;
    }
    if (s->rx_set == 0) {                       /* main layer, profile 1: the stock setup */
        set_done(s, ST_READ_ONLY);
        return;
    }
    for (i = 0; i < SET_BYTES; i++)
        sum += s->rx[i];
    if (checksum != (sum & 0x7F)) {
        set_done(s, ST_CHECKSUM);
        return;
    }
    for (i = 0; i < KNOBS; i++) {
        if (!slot_ok(&s->rx[i * SLOT_BYTES])) {
            set_done(s, ST_BAD_DATA);
            return;
        }
    }
    for (i = 0; i < SET_BYTES; i++)
        s->cfg[s->rx_set][i / SLOT_BYTES][i % SLOT_BYTES] = s->rx[i];
    set_done(s, ST_OK);
}

/* CC_GET: start sending a set back; the main loop produces the messages. */
static void set_get(struct sm *s, u32 id)
{
    if (id >= SETS) {
        tx_put(s, CC_STATUS, ST_BAD_SET);
    } else if (s->get_set != 0xFF) {
        tx_put(s, CC_STATUS, ST_BUSY);
    } else {
        s->get_pos = 0;
        s->get_set = (u8)id;
    }
}

/* The byte number n of a set (the stock profile reads as all auto). */
static u32 set_byte(const struct sm *s, u32 id, u32 n)
{
    if (id == 0)
        return n % SLOT_BYTES == 0 ? SLOT_AUTO : 0;
    return s->cfg[id][n / SLOT_BYTES][n % SLOT_BYTES];
}

/* The message at the current position of the reply being sent. */
static u32 get_message(const struct sm *s)
{
    u32 id = s->get_set;
    u32 sum = id;
    u32 i;

    if (s->get_pos == 0)
        return CC_BEGIN << 8 | id;
    if (s->get_pos <= SET_BYTES)
        return CC_DATA << 8 | set_byte(s, id, s->get_pos - 1u);
    for (i = 0; i < SET_BYTES; i++)
        sum += set_byte(s, id, i);
    return CC_END << 8 | (sum & 0x7F);
}

/* Replaces the call that hands a finished three-byte channel message to the stock
 * code (0x0801B296). Our controller numbers on channel 16 stop here; everything else
 * goes on to the stock code unchanged. Runs in an interrupt. */
void sm_midi_in(void *obj, u8 *message)
{
    if (message[0] == PROTO_STATUS && message[1] >= CC_DATA && message[1] <= CC_COMMIT) {
        struct sm *s = ready();
        u32 value = message[2] & 0x7F;

        switch (message[1]) {
        case CC_BEGIN:
            s->rx_set = (u8)value;
            s->rx_n = 0;
            s->rx_state = 1;
            break;
        case CC_DATA:
            if (s->rx_state == 1 && s->rx_n < SET_BYTES)
                s->rx[s->rx_n++] = (u8)value;
            else
                s->rx_state = 2;
            break;
        case CC_END:
            set_end(s, value);
            break;
        case CC_GET:
            set_get(s, value);
            break;
        case CC_COMMIT:
            if (value != COMMIT_SAVE && value != COMMIT_ERASE)
                tx_put(s, CC_STATUS, ST_BAD_DATA);
            else if (s->commit)
                tx_put(s, CC_STATUS, ST_BUSY);
            else
                s->commit = value == COMMIT_SAVE ? 1 : 2;       /* the main loop does it */
            break;
        default:                                /* CC_STATUS is ours to send, not to receive */
            break;
        }
        return;
    }
    fw_midi_in(obj, message);
}

/* Sends a set being read back, then queued statuses, a few a pass; a refused
 * message is tried again next pass. */
void sm_poll_step(void)
{
    struct sm *s = ready();
    u32 sent = 0;
    u32 job = s->commit;

    if (job) {                                  /* flash is written here, never in the interrupt */
        s->commit = 0;
        tx_put(s, CC_STATUS, flash_write(s, job == 2) ? ST_OK : ST_FLASH);
    }
    while (sent < 16) {
        u32 m;
        u32 reply = s->get_set != 0xFF;

        if (reply)
            m = get_message(s);
        else if (s->tx_tail != s->tx_head)
            m = s->tx[s->tx_tail];
        else
            break;
        if (!fw_midi_usb(fw_out(), PROTO_STATUS | (m & 0xFF00) | (m & 0x7F) << 16, 0))
            break;
        if (!reply) {
            s->tx_tail = (u8)((s->tx_tail + 1) & (TXQ - 1));
        } else if (++s->get_pos > SET_BYTES + 1) {
            s->get_set = 0xFF;
        }
        sent++;
    }
}

#ifndef SM_HOST
/* Replaces the call to encoders_poll in the main loop (0x0800C196). */
__attribute__((naked)) void sm_poll(void)
{
    __asm__("push {r0-r3, r4, lr}\n"
            "bl sm_poll_step\n"
            "pop {r0-r3, r4, lr}\n"
            "b.w fw_encoders_poll\n");
}
#endif
