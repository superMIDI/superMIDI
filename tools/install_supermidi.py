#!/usr/bin/env python3
"""Installs superMIDI into a checkout of the bstp_firmware_explore project.

    python tools/install_supermidi.py PATH_TO_THAT_CHECKOUT

It copies patches/supermidi.c into the checkout's patches/ folder and adds to the
checkout's own files: the list of stock calls to redirect and the "supermidi" feature in
tools/build_patch.py, the stock functions it uses in patches/fw.ld, and its entry in
patches/mods.json. Nothing else is touched. Safe to run again: it replaces its own
lines and adds nothing twice. Then build as the README says.
"""
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "patches", "supermidi.c")
BASE = "0x08038000"          # where the patch is placed: blank flash in the update image, ~12 KB

SITES = r'''    (0x080225D6, 0x0802F4F0, "sm_ctl_build"),  # the control-event function builds a message from a setup entry
    (0x0802FA28, 0x08029F24, "sm_accel"),  # the encoder handler looks up the acceleration step (passed through)
    (0x08019A0C, 0x08016E80, "sm_button_press"),  # the polled button scanner: the press of a step button
    (0x080088A0, 0x0801AF30, "sm_led"),  # the light routine 0x080087D8 sets each step button's colour (41 calls)
    (0x080088EA, 0x0801AF30, "sm_led"),
    (0x08008958, 0x0801AF30, "sm_led"),
    (0x080089B6, 0x0801AF30, "sm_led"),
    (0x08008A48, 0x0801AF30, "sm_led"),
    (0x08008B14, 0x0801AF30, "sm_led"),
    (0x08008B3C, 0x0801AF30, "sm_led"),
    (0x08008B8C, 0x0801AF30, "sm_led"),
    (0x08008CB2, 0x0801AF30, "sm_led"),
    (0x08008CDA, 0x0801AF30, "sm_led"),
    (0x08008D2C, 0x0801AF30, "sm_led"),
    (0x08008D66, 0x0801AF30, "sm_led"),
    (0x08008D88, 0x0801AF30, "sm_led"),
    (0x08008D9C, 0x0801AF30, "sm_led"),
    (0x08008DB2, 0x0801AF30, "sm_led"),
    (0x08008DD8, 0x0801AF30, "sm_led"),
    (0x08008DF8, 0x0801AF30, "sm_led"),
    (0x08008E1C, 0x0801AF30, "sm_led"),
    (0x08008E42, 0x0801AF30, "sm_led"),
    (0x08008E56, 0x0801AF30, "sm_led"),
    (0x08008F38, 0x0801AF30, "sm_led"),
    (0x08008F4A, 0x0801AF30, "sm_led"),
    (0x08008F7C, 0x0801AF30, "sm_led"),
    (0x08008F9A, 0x0801AF30, "sm_led"),
    (0x08008FC6, 0x0801AF30, "sm_led"),
    (0x080090BC, 0x0801AF30, "sm_led"),
    (0x0800912A, 0x0801AF30, "sm_led"),
    (0x0800916C, 0x0801AF30, "sm_led"),
    (0x080091CA, 0x0801AF30, "sm_led"),
    (0x0800920A, 0x0801AF30, "sm_led"),
    (0x08009264, 0x0801AF30, "sm_led"),
    (0x080092A4, 0x0801AF30, "sm_led"),
    (0x080092C2, 0x0801AF30, "sm_led"),
    (0x080092EC, 0x0801AF30, "sm_led"),
    (0x08009318, 0x0801AF30, "sm_led"),
    (0x08009342, 0x0801AF30, "sm_led"),
    (0x0800936E, 0x0801AF30, "sm_led"),
    (0x08009398, 0x0801AF30, "sm_led"),
    (0x0800945C, 0x0801AF30, "sm_led"),
    (0x080094BC, 0x0801AF30, "sm_led"),
    (0x080094CA, 0x0801AF30, "sm_led"),
    (0x0801B296, 0x0802F44E, "sm_midi_in"),  # MIDI input: a finished three-byte channel message
    (0x0800C196, 0x0802BEE4, "sm_poll"),  # main loop: replies and saving
'''

SYMBOLS = [
    ("fw_ctrl_build", "0x0802f4f1", "(obj, event, entry, a, b): builds the outgoing message from a control setup entry"),
    ("fw_accel", "0x08029f25", "(mode, clicks) -> step: the knob acceleration table"),
    ("fw_button_press", "0x08016e81", "(button, event): a button was pressed; button[4] is its function code"),
    ("fw_led", "0x0801af31", "(leds, button 0..15, colour)"),
    ("fw_midi_in", "0x0802f44f", "(obj, message): a finished channel message from MIDI input: status, data 1, data 2"),
    ("fw_encoders_poll", "0x0802bee5", "the encoder poll, called from the main loop"),
    ("fw_flash_clear", "0x08004ba1", "(flags): clear the flash status flags; the stock settings save passes 0x35"),
    ("fw_flash_erase", "0x08004bad", "(address) -> 4 when done: erase the flash page that holds the address"),
    ("fw_flash_word", "0x08004c65", "(address, word) -> 4 when done: program one 32-bit word"),
    ("fw_flash_lock", "0x08004c35", "lock the flash controller again"),
    ("fw_flash_unlock", "0x08004ce9", "unlock the flash controller"),
    ("fw_irq_pause", "0x08007939", "(resume): 0 stops the peripheral interrupts for a flash write, 1 restores them"),
    ("fw_tick_pause", "0x08005feb", "(resume): 0 stops the system tick interrupt, 1 restores it"),
]

ENTRY = {
    "name": "superMIDI controller",
    "page": False,
    "hardware": "1.0.0 tested on one device, firmware 2.0.1.6",
    "summary": "MIDI controller mode: four profiles per knob pair chosen with the step buttons, a Shift layer, "
               "per-knob CC, channel, range, mode and acceleration edited over USB MIDI, saved in flash.",
    "changes": [["1.0.0", "2026-10-25", "First release."]],
}


def read(path):
    with open(path, newline="") as f:
        text = f.read()
    nl = "\r\n" if "\r\n" in text else "\n"
    return text.replace("\r\n", "\n"), nl


def write(path, text, nl):
    with open(path, "w", newline="") as f:
        f.write(text.replace("\n", nl))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    paths = {k: os.path.join(root, v) for k, v in (
        ("bp", "tools/build_patch.py"), ("ld", "patches/fw.ld"),
        ("mods", "patches/mods.json"), ("fwh", "patches/fw.h"))}
    for p in paths.values():
        if not os.path.exists(p):
            sys.exit("%s not found: is that a bstp_firmware_explore checkout?" % p)

    shutil.copyfile(SOURCE, os.path.join(root, "patches", "supermidi.c"))
    print("copied patches/supermidi.c")

    text, nl = read(paths["bp"])
    block = "# supermidi: the stock calls to redirect (see patches/supermidi.c)\nSUPERMIDI = [\n" + SITES + "]\n\n"
    old = re.search(r"(# supermidi:[^\n]*\n)?^SUPERMIDI = \[\n.*?^\]\n\n?", text, re.S | re.M)
    if old:
        text = text[:old.start()] + block + text[old.end():]
    else:
        i = text.find("\nFEATURES = {")
        if i < 0:
            sys.exit("tools/build_patch.py has no FEATURES table; this is not a version the installer knows")
        text = text[:i + 1] + block + text[i + 1:]
    line = '    "supermidi": (["supermidi.c"], [], SUPERMIDI, %s),\n' % BASE
    if re.search(r'^    "supermidi":.*\n', text, re.M):
        text = re.sub(r'^    "supermidi":.*\n', line.replace("\\", "\\\\"), text, flags=re.M)
    else:
        m = re.search(r"^FEATURES = \{\n(.*?)^\}", text, re.S | re.M)
        if not m:
            sys.exit("could not read the FEATURES table in tools/build_patch.py")
        text = text[:m.end(1)] + line + text[m.end(1):]
    write(paths["bp"], text, nl)
    print("updated tools/build_patch.py (%d redirects)" % SITES.count('"sm_'))

    ld, nl = read(paths["ld"])
    added = []
    for name, addr, note in SYMBOLS:
        if not re.search(r"^%s\b" % name, ld, re.M):
            row = "%-19s = %s;  /* %s */\n" % (name, addr, note)
            i = ld.find("SECTIONS")
            ld = ld[:i] + row + ld[i:] if i >= 0 else ld.rstrip("\n") + "\n" + row
            added.append(name)
    write(paths["ld"], ld, nl)
    print("patches/fw.ld:", ("added " + ", ".join(added)) if added else "symbols already there")

    raw, nl = read(paths["mods"])
    mods = json.loads(raw)
    if "supermidi" in mods:
        if mods["supermidi"]["changes"][0][0] == ENTRY["changes"][0][0]:
            print("patches/mods.json: entry already there")
        else:                      # an older test entry: replace it (the file is rewritten, same content otherwise)
            mods["supermidi"] = ENTRY
            write(paths["mods"], json.dumps(mods, indent=1) + "\n", nl)
            print("patches/mods.json: replaced an older supermidi entry")
    else:
        end = "\n }\n}"
        text = raw.rstrip()
        if not text.endswith(end):
            sys.exit("patches/mods.json is not in the shape this installer expects")
        body = json.dumps(ENTRY, indent=1).replace("\n", "\n ")
        new = text[:-len(end)] + "\n },\n \"supermidi\": " + body + "\n}\n"
        json.loads(new)
        write(paths["mods"], new, nl)
        print("updated patches/mods.json")
    print("done; build with: python tools/build_patch.py out/bsp_2.0.1.6.bin YOUR_UPDATE_FILE.led supermidi")


main()
