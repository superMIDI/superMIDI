# How superMIDI works

All of this is specific to Arturia BeatStep Pro firmware **2.0.1.6**, read from the update file
(with a disassembler and a decompiler) and then tried on a unit. Names are descriptions, not
Arturia's. Addresses are in the unit's address space.

## The setup entries

In controller mode every control has an 8-byte setup entry (the thing MIDI Control Center
edits). Fields as far as they are used here:

| Byte | Meaning |
|---|---|
| 0 | low nibble: control kind (0 = off); high nibble: message type |
| 1 | bits 0-2 knob mode (0 absolute; 1, 2, 3 relative: 64 + step, two's complement, 16 + step); bits 3-5 acceleration (0 off to 3 high, 4 = global) |
| 2, 3 | bits 18-24 of the first 32-bit word: the MIDI channel (0x41 = global channel) |
| 4 | CC number |
| 5, 6 | minimum, maximum (absolute mode) |
| 7 | the knob's current value |

A knob turn arrives as an event (kind 7 or 8, id 0x20 + knob, clicks). The function at
0x0802258C picks the entry and 0x0802F4F0 builds the message from it; the acceleration table is
looked up at 0x08029F24.

## Profiles

superMIDI replaces the call to 0x0802F4F0 so that, for a knob in profile 2 to 4 or on the Shift
layer, it passes a **copy** of the stock entry instead. The copy has the CC, channel, range, mode and
acceleration overrides applied and keeps its own value in byte 7, so the stock code does the
rest (acceleration, relative modes, limits) and the stock entry never changes. The copies live in
patch RAM at 0x20006000 (about 2.2 KB; the region is free in this firmware and not cleared at
start-up, so a magic word marks it as initialised).

The profile bits are 16 bits of state toggled from the press of a step button: the polled button
scanner (call at 0x08019A0C) calls the stock press handler with the button and event 1. The hook swallows the
press when Shift (flag at 0x20000199) is held in controller mode (value 3 at 0x200001F0) and the
button's function code is 0x20-0x2F. It must also do the stock handler's first three statements
(clear byte +0x0B, set byte +0x0C, copy flag +0x0A to +0x08) or the scanner calls the hook again on
every scan while the button is down. The lights are the 41 calls to the stock light function
(0x0801AF30) inside the routine at 0x080087D8, redirected to show the bits while Shift is held.

## Editing and saving

The editor speaks plain Control Change on channel 16 (docs/protocol.md). The call at 0x0801B296
is where the MIDI input handler hands a finished three-byte message to the stock code; the hook
takes controller numbers 104-109 on channel 16 and passes everything else on. It runs in an
interrupt, so it only stores and queues; replies and flash writes happen in the main loop, from a
hook on the call to the encoder poll at 0x0800C196.

Saving writes the 784 bytes of sets 1-7, behind a magic word, a format and length word and a
checksum, to the flash page at **0x0803C000**, using the firmware's own erase, program and lock
routines in the same order its settings save uses (interrupts and the tick off during the write),
and reads every word back. The page is outside the update image (0x08003000-0x0803B000) and
nothing else in the firmware references it (the only flash the stock code writes is the 8-byte
marker at 0x0803F800), so firmware updates leave it alone. At start-up the profiles are loaded
only if the whole copy is present and every slot passes the same checks as a write.

## Limits and untested corners

- Other firmware versions would need every address found again.
- The unit stops responding for a few tens of milliseconds while it saves.
- A write-protected page would show up as a "flash error" status; nothing else is written.
- Message type (CC, NRPN, RPN, 14-bit and so on) is not overridable; the stock entry's type is kept.
- The Shift flag and the button latch bytes were read from the code and confirmed by watching a
  unit; the rest of the tracking in RAM is from reading only.

## Testing

`tests/supermidi_host.c` includes the patch source with the stock functions, RAM and flash
replaced by stand-ins and checks, among other things: all 65,536 profile-bit states, the button
scanner model, every refusal of a bad set, the exact bytes written to a fake flash page and what is
loaded back from damaged copies, and that nothing outside the page is ever touched.
