# BlackHD-PC

**BlackHD-PC** by **Spring — Spring Code**

A native Windows version of **BLACK** (Criterion Games, 2006), built from the original Xbox release (NTSC-U), tuned for
modern PCs: high frame rates, HD output, a PC-style control scheme, keyboard and mouse prompts everywhere, and an
HD crosshair.

The original game code runs directly on the PC; the parts of the Xbox it talks to (kernel, Direct3D 8, DirectSound,
controller library) are replaced by PC code. **No game code or data is included: you need your own copy of the
game.**

## What BlackHD-PC adds

| Area | Improvement |
|---|---|
| Frame rate | 30, 60, **120**, 144, 165 or 240 fps (default **120**). Frame pacing rebuilt so high rates no longer run in slow motion; short waits stay accurate even on PCs with coarse system timers. |
| Crosshair | **HD crosshair**: the HUD texture rebuilt at 4x from your own game files with a sharp dot (white disc, black outline), mipmapped so the outline never flickers away. Colours set in `black.ini`: **green** normally, **blood red** over an enemy. |
| Aiming | **Hold to aim** (aim while the button is held) or the original toggle, in PC SETTINGS > CONTROLS > AIM. |
| Aim assist | AUTO-AIM: OFF, ON or **CONTROLLER ONLY** (default): the pull towards enemies works with a controller and switches off as soon as the keyboard or mouse is used. |
| Crouch | **Hold to crouch** (crouch while Ctrl is held), whatever the game profile says, in PC SETTINGS > CONTROLS > CROUCH. |
| Weapons | Keys **1** and **2** select the first or second weapon you carry; **Q** swaps between them. |
| Walk | **Shift** to walk slowly (the movement keys push only part of the way; `walk_speed` in `black.ini`). |
| Prompts | Every Xbox button icon replaced by the key you bound: hints ("[X] to use the health packs"), pick-up and swap prompts ("Press [F] to pick up"), "PRESS ENTER" on the title screen and the menus' legend ([ARROWS] select, [ENTER] confirm, [BACKSPACE] back). Prompts follow your own bindings. |
| Menus | Controller options readable in the Portuguese translation (YES / NO instead of overlapping ON / OFF words). |
| Textures | Texture replacement: a PNG in `port\textures\` named after a game texture replaces it (any size, mipmapped). `--dump-textures` saves the game's textures to `port\textures\dump\` to find their names. |
| Performance | Accurate frame pacing above 60 fps without a busy core: the game's vertical blank runs at the chosen rate on a separate CPU. |

Plus everything of the base port: any render resolution, anti-aliasing, widescreen, adjustable field of view, mouse
look, bindable keys, XInput controllers, borderless fullscreen, settings in the game's own menus.

## Requirements

- Windows 10 or 11, a graphics card with Direct3D 9 (Shader Model 3).
- Nothing else has to be installed.
- The **NTSC-U** (North America) Xbox disc of BLACK. Other regions and builds are not supported.

## Installing

1. Dump your disc and extract it with [extract-xiso](https://github.com/XboxDev/extract-xiso), then rename the extracted
   folder to `game`. Only the NTSC-U build works: `game\default.xbe` must have SHA-256
   `DF2739C372D254A90AEECCF5971D12097F22D89FBB90DB021FDA96DD4DEB68F6`
   (PowerShell: `Get-FileHash game\default.xbe -Algorithm SHA256`).
2. Extract `BlackHD-PCv1.0.zip` next to `game`, so the `port` folder sits beside it:

   ```
   BLACK\
     game\
       default.xbe
       ...
     port\
       black.exe
       blackrt.dll
       black.ini
   ```

3. Start `port\black.exe`.

The port keeps saved games (`savedata\`), settings (`black.ini`), the log (`black.log`) and its menu files (`cache\`)
in `port`. Nothing is written to `game`. A translated copy of the game (for example the Portuguese translation) works
the same way, as long as `default.xbe` is the NTSC-U one.

## Controls (keyboard and mouse)

| Action | Key |
|---|---|
| Move | W A S D |
| Walk slowly (hold) | Shift |
| Look | mouse |
| Fire / Aim (hold) | left / right mouse button |
| Reload | R |
| Use, pick up | F |
| Crouch (hold) | Ctrl |
| Grenade | G |
| Melee | V |
| Weapon 1 / Weapon 2 | 1 / 2 |
| Swap weapon | Q or mouse wheel |
| Change firing mode | Z |
| Toggle suppressor | T |
| Health pack | X |
| Show objectives | Space |
| Pause | Esc |

Menus: arrow keys, Enter to confirm, Backspace to go back. Every key can be changed in
PC SETTINGS > CONTROLS > KEY BINDINGS (two keys per action). Controllers work as well.

## Settings

In the game: Options > PC SETTINGS (main menu) and PC SETTINGS in the pause menu.

| Page | Settings |
|---|---|
| VIDEO | render resolution, anti-aliasing, field of view, brightness, gamma, fullscreen |
| DISPLAY | aspect ratio, frame rate (applies from the next start), geometry seam fill, effects at Xbox size |
| AUDIO AND GAME | room reverb, auto-aim (off / on / controller only), skip any movie |
| CONTROLS | mouse sensitivity, mouse inversion, aim (hold / toggle), crouch (hold / profile option), key bindings |

Also in `port\black.ini`:

| Key | Meaning |
|---|---|
| `hd_reticle = 1` | the HD crosshair (0 = the game's) |
| `reticle_colour = 00FF00` | crosshair colour (RRGGBB, or `game`) |
| `reticle_enemy_colour = 8C0000` | crosshair colour over an enemy |
| `walk_speed = 0.45` | share of full speed while walking (0.1 to 1.0) |
| `crouch_hold = 1` | crouch while the key is held |
| `aim_hold = 1` | aim while the key is held |
| `autoaim = 2` | 0 off, 1 on, 2 controller only |

`black.exe --help` lists the command-line options.

## Building from source

Visual Studio 2022 or 2026 (Build Tools are enough) with the C++ x86 tools, and CMake 3.24 or newer (it ships with
Visual Studio).

```
cd loader
cmake --preset win32-vs2022     # or win32 for Visual Studio 2026
cmake --build build/win32-vs2022 --config RelWithDebInfo
```

This builds `black.exe` and `blackrt.dll`.

## Credits

**BlackHD-PC** — Spring, **Spring Code**: all the improvements listed above.

Based on [black-pc](https://github.com/Spark198rus/black-pc) by Spark198rus.

## License

BlackHD-PC is free software under the GNU General Public License, version 2 or (at your option) any later version,
like the project it is based on. See `LICENSE` and `NOTICE`.

## Legal

This project is not affiliated with or endorsed by Criterion Games or Electronic Arts. BLACK is a trademark of
Electronic Arts Inc.; Xbox is a trademark of Microsoft Corporation. No code or data from the game and no code from the
Microsoft Xbox Development Kit is included. You need your own legally obtained copy of the game.
