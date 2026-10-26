
# superMIDI controller

An unofficial add-on for the **Arturia BeatStep Pro**, firmware **2.0.1.6**. It extends the
MIDI controller mode: four switchable profiles for every pair of knobs, a second set of
knob messages on Shift, and a small editor page to set them up. Not made or supported by
Arturia. Arturia and BeatStep Pro are trademarks of their owners. No Arturia material is
in this repository: you supply your own copy of Arturia's update file.

<img width="1536" height="1024" alt="file_000000007c1c81f58903699ad91fcc67" src="https://github.com/user-attachments/assets/3576dc37-abd9-4bbd-a82c-de940b8d6d3f" />

## What it adds

- **Four profiles per knob pair.** Knob *n* and knob *n+8* form a lane. The two step buttons
  under the knobs for that lane (buttons 2n-1 and 2n, counting 1 to 16) are its two profile
  bits. Neither lit is profile 1, the first alone is 2, the second alone is 3, both is 4.
- **Profile 1 is your normal setup**, as edited in MIDI Control Center, untouched.
- **A Shift layer.** Shift + knob sends its own message, set per profile, so every knob can
  reach up to eight messages (four profiles, plain and Shift).
- **Per knob and profile you can set** the CC number, the MIDI channel, the range, the knob
  mode (absolute or relative) and the acceleration. Anything you leave on "stock" follows the
  normal setup, so changing it in MIDI Control Center carries over.
- **Each profile keeps its own value**, so switching back to a profile continues where you
  left it. Switching sends no MIDI. Profiles 2 to 4 start at 0 after a power-on.
- **Settings are saved in the unit** (one blank flash page, outside the update image) and
  survive power-off and firmware updates.

## Using it

1. In MIDI controller mode, hold **Shift** and press a step button to switch its profile bit.
   While Shift is held, the step buttons light for the bits that are set.
2. Turn knobs as usual. Hold Shift while turning for the Shift layer.
3. Until you edit anything, a profile's knob uses the "auto" message: the stock CC plus 16
   per profile (plus 64 on the Shift layer), so you can hear the switch work straight away.
4. To set real values, open `editor/supermidi_editor.html` in **Chrome or Edge**, click
   *Connect* (allow MIDI access, no SysEx needed), pick a layer and profile, edit the table,
   *Send this set* (or *Send all*), and *Save to unit* to keep it. *Read* shows what the unit
   holds. *Forget saved* erases the saved copy. *Export* and *Import* keep a backup file.

## Installing

**Back up all your projects with MIDI Control Center first**, and install Arturia's own
2.0.1.6 file once before anything modified, so you know the way back works on your unit.
Modified firmware is installed at your own risk.

You need Python 3 and the tooling of the
[bstp_firmware_explore](https://angellinares.github.io/bstp_firmware_explore/) project, which
this builds on (it unpacks and repacks Arturia's update file and compiles patches into it;
see its README for the compiler it needs). Then:

```
python tools/install_supermidi.py PATH_TO_THAT_CHECKOUT      # adds superMIDI to it
cd PATH_TO_THAT_CHECKOUT
python tools/led.py unpack BeatStepPro_Firmware_Update_2_0_1_6.led out/bsp_2.0.1.6.bin
python tools/build_patch.py out/bsp_2.0.1.6.bin BeatStepPro_Firmware_Update_2_0_1_6.led supermidi --dev
```

That writes `out/bsp_2.0.1.6-supermidi.led`; install it with MIDI Control Center like any
update. **Going back:** install Arturia's original file the same way. If the unit does not
start, hold PLAY + STOP + RECORD while switching it on to reach the updater.

## Status

Written and checked against firmware 2.0.1.6 only; the addresses are specific to it.
Verified on one unit: profile switching, per-profile CC, the Shift layer, the editor
(read and write), saving, and reloading after power-off and after reinstalling Arturia's
firmware. The channel, range, mode and acceleration overrides are covered by the unit tests
but have had less hands-on testing. Please report what you find.

`tests/` runs the patch's logic on a computer (`python -m unittest discover -s tests`, needs a
C compiler). It cannot check the hooks into Arturia's code; only a unit can.

## More

- `docs/protocol.md`: the messages the editor uses; any MIDI tool can use them.
- `docs/internals.md`: how it works, the hooks, the flash page and the limits.
- Released under the MIT license (`LICENSE`).
