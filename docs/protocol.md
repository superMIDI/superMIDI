# superMIDI live-editing protocol (version 1.0.0)

Plain Control Change messages on **MIDI channel 16, over USB**. No SysEx, so a browser
needs no SysEx permission. The stock firmware never sees these controller numbers: it
hands them to the patch first, and everything else goes on as before.

## Sets

A *set* is one layer of one profile: **set id = layer * 4 + profile**, layer 0 the plain
knob, layer 1 Shift + knob, profile 0 to 3 (shown as profiles 1 to 4). So ids 0 to 7:

| id | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| layer, profile | plain, 1 | plain, 2 | plain, 3 | plain, 4 | Shift, 1 | Shift, 2 | Shift, 3 | Shift, 4 |

Set 0 is the stock setup, edited in MIDI Control Center: it is read-only here and reads back
as all auto. A set holds 16 slots, one per knob (knob 1 first), **seven bytes each**, every byte 0 to 127:

| Byte | Meaning |
|---|---|
| 0 | channel: 0 to 15 = MIDI channel 1 to 16 with the CC below; 0x7E = the stock channel with the CC below; 0x7F = auto (stock CC + 16 per profile, + 64 on the Shift layer; the CC byte is ignored) |
| 1 | CC number 0 to 127 |
| 2, 3 | minimum and maximum (0 to 127, minimum not above maximum), used only if flag 1 is set |
| 4 | knob mode 0 to 3, used only if flag 2 is set: 0 absolute, 1 relative (64 + step), 2 relative (step as a 7-bit two's complement), 3 relative (16 + step) |
| 5 | acceleration 0 to 4, used only if flag 4 is set: 0 off, 1 low, 2 medium, 3 high, 4 the unit's global setting |
| 6 | flags: 1 range, 2 mode, 4 acceleration; any other bit is refused |

The stock setup supplies everything that is not overridden (message type, and any field whose
flag is clear), so changing those in MIDI Control Center carries over to every profile. Range
means nothing in a relative mode. A set is therefore 16 x 7 = **112** data bytes.

## Messages (all `BF cc value`, so Control Change on channel 16)

| cc | Name | Direction | Value |
|---|---|---|---|
| 0x69 | BEGIN | host to device, and in a GET reply | set id |
| 0x68 | DATA | both | one byte of the set; send 112 |
| 0x6A | END | both | (set id + sum of the 112 bytes) & 0x7F |
| 0x6B | STATUS | device to host | result, below |
| 0x6C | GET | host to device | set id: the device sends that set back |
| 0x6D | COMMIT | host to device | 0x2A: save all profiles to flash; 0x2B: erase the saved copy |

To write: BEGIN, 112 DATA, END. The device checks the length, the id, the checksum and
every first byte before it changes anything, then answers with one STATUS. A new BEGIN
starts over. To read: GET; the reply is BEGIN, 112 DATA, END (or one STATUS if it cannot).

| Status | Meaning |
|---|---|
| 0 | OK, the set is live |
| 1 | wrong checksum |
| 2 | no such set |
| 3 | read-only (set 0) |
| 4 | bad data (a channel byte that is not 0 to 15, 0x7E or 0x7F; a flag that is not 1, 2 or 4; minimum above maximum; mode above 3; acceleration above 4) |
| 5 | wrong length (too few or too many bytes, or END without BEGIN) |
| 6 | (not used) |
| 7 | busy: a set is still being sent back, or a save is waiting; ask again |
| 8 | flash error: the erase, a write or the read-back check failed |

Statuses are queued in the interrupt that receives the message and sent from the main loop; a
set being read back is produced there on demand. At most 16 messages go out per pass and a refused
one is retried, so nothing is lost. A reply being sent finishes before the next status.

## Saving

COMMIT 0x2A writes sets 1 to 7 (set 0 is the stock setup and is never stored) to the flash page at
**0x0803C000**, a page the update image leaves blank and nothing else in the firmware uses. The
interrupt that receives the message only notes the job; the main loop does it, using the stock
erase and program routines with the interrupts and the tick off, the way the stock settings save
does. It checks every word by reading it back and answers STATUS 0, or 8 if anything failed. The
unit stops answering MIDI for a few tens of milliseconds while it writes.

Stored: a magic word, a format and length word, a checksum word, then 784 bytes (sets 1 to 7). At
start-up the profiles are loaded only if all of it is there and correct (including that every slot
passes the same checks as a write); otherwise the profiles start as auto, as on a unit that has
never saved. COMMIT 0x2B erases the page so the next start-up has the defaults. A firmware update
leaves the page alone (the update image does not cover it; checked on a unit by installing Arturia's own update).

Edits are live at once but are saved only by COMMIT 0x2A.
