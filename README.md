# goginput_remapping_tool

An interactive Windows console tool that builds controller presets for the **GOG DirectInput wrapper** (`dinput.dll` + `goginput.ini`).

You press the buttons on your own pad, and the tool writes a matching `[section]` into `goginput.ini`. Optionally, it uses [SDL2](https://libsdl.org/)'s controller database to suggest a layout automatically.

> Not affiliated with or endorsed by GOG. This is a community tool for the `goginput.ini` format used by GOG's wrapper.

## Why

GOG's wrapper remaps controllers using per-device sections in `goginput.ini`, keyed by a DirectInput product GUID. Writing those by hand means looking up your VID/PID, working out which DirectInput axis and button numbers your pad reports, and getting the inversions and trigger quirks right. This tool does that for you.

## How the ini format works

Taken from `goginput.ini`:

```ini
[sony_dualshock4_a]
guid              = 09CC054C-0000-0000-0000-504944564944
pad_zaxisplus     = pad_rxaxisplus     ; physical input  =  what the game sees
pad_zaxisminus    = pad_rxaxisminus
pad_rxaxisplus    = dev_null           ; dev_null disables an input
pad_rzaxisplus    = pad_ryaxisplus
pad_button0       = pad_button2
pad_button1       = pad_button0
pad_button2       = pad_button1
```

- The GUID is `PPPPVVVV-0000-0000-0000-504944564944` (product ID, then vendor ID).
- Each line is `<physical input> = <what the game sees>`.
- The "game sees" side is the Xbox-style layout the shipped presets target:

| Logical control | ini name |
|---|---|
| A, B, X, Y | `pad_button0` – `pad_button3` |
| LB, RB | `pad_button4`, `pad_button5` |
| LT, RT | `pad_button6`, `pad_button7` |
| Back, Start | `pad_button8`, `pad_button9` |
| LS click, RS click | `pad_button10`, `pad_button11` |
| Left stick | `pad_xaxis…`, `pad_yaxis…` |
| Right stick | `pad_rxaxis…`, `pad_ryaxis…` |
| D-pad | hat (POV), or `pad_dpadup/down/left/right` |

Other valid names include `kb_*` (keyboard keys) and `ms_*` (mouse), which this tool does not generate.

## Features

- Reads the pad through DirectInput, so button and axis numbers are exactly what the wrapper sees.
- Builds the GUID from the pad itself. No VID/PID lookup is needed.
- A 20-step wizard covers both sticks, face buttons, bumpers, triggers, Back/Start, stick clicks and the D-pad. Enter or `S` skips a step.
- Handles **inverted axes** and **triggers on axes or buttons**. If a pad reports both a button and an axis for a trigger, the digital button is preferred.
- Adds `dev_null` lines for leftover inputs that would collide with a remapped target, as the shipped DualShock/DualSense sections do.
- **Optional SDL assist:** it shows the pad's SDL name and GUID, suggests a layout from SDL's controller database, and shows SDL's guess as a hint during the wizard. You can accept the suggestion without pressing anything.
- Merges into your existing `goginput.ini`:
  - replaces a section with the same name or GUID (it asks first),
  - otherwise inserts the new section before `[keyboardandmouse]`,
  - keeps CRLF line endings,
  - saves a one-time `goginput.ini.bak` backup.
- If no ini is found, it writes the section alone to `<name>_preset.ini`.
- `--monitor` mode prints the ini key for every input as you press it.

## Requirements

- Windows with DirectX/DirectInput 8
- A C++14 compiler: MSVC or MinGW-w64
- Optional: `SDL2.dll` for the SDL features

## Build

MSVC (Developer Command Prompt):

```bat
cl /EHsc /O2 goginput_remappping_tool_sdl.cpp dinput8.lib dxguid.lib user32.lib
```

MinGW-w64:

```sh
g++ -O2 -static goginput_remappping_tool_sdl.cpp -o goginput_remappping_tool_sdl.exe -ldinput8 -ldxguid -luser32
```

**Build as x86 (32-bit)** if you want SDL support alongside GOG's wrapper. The wrapper's `dinput.dll` is 32-bit, so the `SDL2.dll` next to the game is most likely 32-bit too, and a 64-bit exe cannot load it. The tool reports "bitness differs" when that happens. The DirectInput part works in either bitness.

## Usage

```text
goginput_remappping_tool_sdl.exe [path\to\goginput.ini] [options]
```

| Option | Meaning |
|---|---|
| *(none)* | Use `goginput.ini` in the current folder |
| `--monitor` | Show what the pad reports, then exit with Ctrl+C |
| `--sdl <path>` | Use this `SDL2.dll` |
| `--no-sdl` | DirectInput only |
| `--sdl-native` | Don't force SDL onto its DirectInput backend (see below) |

Typical session:

1. Connect the pad and run the tool, pointing it at the game's `goginput.ini`.
2. Leave the pad untouched for about a second while it calibrates.
3. If SDL knows your pad, it shows a suggested layout. Answer **Y** to accept it as-is, or **N** to press each control. In the wizard, SDL's guess is shown as a hint.
4. Confirm or edit the section name, and review the generated section.
5. The tool writes the section into `goginput.ini`.

Use `--monitor` first if you want to see how your pad is numbered. Button numbers are 0-based, as in the ini; `joy.cpl` counts from 1.

## SDL support

The shipped `dinput.dll` wraps the real DirectInput and loads `sdl2.dll` at run time. From its strings, it appears to use SDL only for GUID, name and LED functions. The ini keys are therefore plain DirectInput indices, and this tool keeps DirectInput as the source of truth for presses. SDL only adds identification and a suggested layout.

- `SDL2.dll` is loaded at run time, so nothing links against SDL. The search order is: `--sdl <path>`, the folder of the ini you pass, then standard Windows lookup (tool folder, system folders, PATH).
- By default the tool sets SDL hints (`SDL_JOYSTICK_HIDAPI=0`, `SDL_JOYSTICK_RAWINPUT=0`, `SDL_XINPUT_ENABLED=0`) so SDL uses its DirectInput backend and its button and axis numbering lines up with the wrapper's. With `--sdl-native`, PlayStation and Switch pads get SDL-synthesised layouts that may not match.
- A `gamecontrollerdb.txt` next to the ini or the tool is loaded as extra mappings.
- SDL's suggestion treats triggers as axes if the database says so, which in auto mode produces analog-to-button mappings. The wizard prefers the pad's digital trigger buttons when they exist, matching the shipped DualShock presets.

## How it works

1. Enumerates DirectInput game controllers and reads the product GUID.
2. Calibrates resting axis values, then waits for each control. An axis counts as pressed after roughly 25% of its travel, and the direction of travel decides the sign.
3. Records `physical → logical` pairs and drops identity pairs.
4. Adds `dev_null` for unmapped physical inputs whose names collide with a target in use.
5. Merges the section into the ini.

## Limitations

- Hats (POV D-pads) cannot be mapped onto buttons. A pad with a hat D-pad needs no D-pad lines.
- Slider axes are not handled.
- Keyboard and mouse targets (`kb_*`, `ms_*`) and the `[general]` options are not generated. Edit them by hand.
- Only axes and buttons are remapped. Rumble and LED behaviour are untouched.

## Status

The ini parsing, merging and SDL-mapping translation logic was compiled and tested on Linux against a real `goginput.ini`. The Windows and DirectInput code was only syntax-checked against stub headers, and the SDL calls have not been run against a real `SDL2.dll`. Bug reports with the console output and your pad's name and VID/PID are welcome.
