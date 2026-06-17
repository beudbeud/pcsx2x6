# make-acgame.sh

Generates a PCSX2x6 arcade manifest (`.acgame`) and the file layout needed to
boot a Namco System 246/256 arcade game from a disc image (ISO/CHD) with the
PCSX2x6 libretro core or standalone.

## What it does

PCSX2x6 boots arcade games through a small INI manifest with the `.acgame`
extension. The manifest points at a game directory containing the media image,
the boot ELF and (optionally) the security dongle and a save card. This script
assembles that structure for you:

```
<out>/<Name>.acgame                manifest (INI)
<out>/roms/<slug>/<image>          media image (mediasrc)
<out>/roms/<slug>/<elf>            boot ELF
<out>/roms/<slug>/<dongle>         dongle memcard  -> slot 1 (mc0:), optional
<out>/roms/<slug>/<card>           save card       -> slot 2 (mc1:), optional
```

## Quick start

```bash
./make-acgame.sh -i "Soul Calibur 3.iso" -e boot.elf -d NM00031.bin \
    -n "Soul Calibur 3" -g SC3 -p 246 -j fighting -o ~/roms/namco2x6
```

Then launch it:

```bash
retroarch -L pcsx2x6_libretro.so "~/roms/namco2x6/Soul Calibur 3.acgame"
```

## Options

| Option | Argument | Description |
|---|---|---|
| `-i, --image` | FILE | Media image (`.iso`, `.chd`, `.img`, `.bin`). **Required.** |
| `-e, --elf` | FILE | Boot ELF as a local file (e.g. `boot.elf`). **Required unless `-x` is used.** |
| `-x, --extract-elf` | PATH | Extract the boot ELF directly from the image (path inside the ISO, e.g. `SLPM_123.45` or `MODULES/BOOT.ELF`). Mutually exclusive with `-e`. |
| `-L, --list` | | List the image contents (to find the ELF path for `-x`) and exit. |
| `-n, --name` | NAME | Game title; also names the `.acgame` file. **Required.** |
| `-g, --gameid` | ID | Game ID / serial. Drives JVS input adaptation in the core. Defaults to the name, uppercased and truncated to 8 chars. |
| `-p, --platform` | `246` \| `256` \| `super256` | Target hardware. Default: `246`. |
| `-m, --media` | `CD` \| `DVD` \| `HDD` | Media type. Guessed from the image when omitted (see below). |
| `-d, --dongle` | FILE | Security dongle memcard image, installed to slot 1 (`mc0:`). |
| `-c, --card` | FILE | Save card image, installed to slot 2 (`mc1:`) — e.g. the Soul Calibur II conquest card. |
| `-j, --jvsmode` | `fighting` \| `lightgun` | JVS input mode. `lightgun` configures GunCon2 on USB1+USB2. |
| `-a, --args` | ARGS | Arguments passed to the game ELF (`args=` key). |
| `-o, --out` | DIR | Output directory. Default: current directory. |
| `-S, --symlink` | | Symlink the image into the game directory instead of copying it. |
| `-h, --help` | | Show usage. |

### Extracting the ELF from the image

If the boot ELF lives on the disc itself, you can pull it straight out of the
image instead of providing a local file:

```bash
# 1. Find the ELF path inside the image
./make-acgame.sh -i game.chd -L

# 2. Generate, extracting it directly
./make-acgame.sh -i game.chd -x MODULES/BOOT.ELF -n "My Game" -o ~/roms/namco2x6
```

Extraction reads the ISO9660 filesystem with `bsdtar`, `7z` or `isoinfo`
(whichever is installed). `.chd` images are unpacked first with `chdman`
(mame-tools), trying `extractdvd` then `extractcd`.

The script checks the extracted file for the `\x7fELF` magic and warns when it
doesn't match: on most System 246/256 discs the on-disc ELF is **encrypted**
(e.g. Soul Calibur 3's `SC3AC10.BIN`) and the real boot ELF comes with the
dongle dump — `-x` is mainly useful for the titles that ship a plain ELF.

### Media type guessing

When `-m` is omitted:

| Image | Guessed type |
|---|---|
| `.chd` | `DVD` (the CHD's own sector size takes precedence in the core anyway) |
| `.iso` > 750 MB | `DVD` |
| `.iso` ≤ 750 MB | `CD` |
| `.img`, `.bin`, `.raw` | `HDD` (flat hard-disk image) |

## Generated manifest

```ini
[game]
name=Soul Calibur 3
gameid=SC3
platform=246

[data]
subdir=roms/soul_calibur_3
elf=boot.elf
media=DVD
mediasrc=Soul Calibur 3.iso
dongle=NM00031.bin
jvsmode=fighting
```

### Key reference

| Section | Key | Meaning |
|---|---|---|
| `[game]` | `name` | Display title. |
| | `gameid` | Serial; the core adapts JVS input to it. |
| | `platform` | `246`, `256` or `super256`. 246/256 enable extended IOP RAM; 256/super256 raise the EE clock. |
| `[data]` | `subdir` | Game directory, relative to the `.acgame` location. |
| | `elf` | Boot ELF, relative to `subdir`. |
| | `media` | `CD`, `DVD` or `HDD` (ATA/ATAPI device presented to the game). |
| | `mediasrc` | Image file, relative to `subdir` (absolute paths also accepted). |
| | `dongle` | Memcard file copied to slot 1 at boot. **Always overwritten** by the core (DONGLEMAN corrupts it at runtime), so keep your pristine copy in the game directory. |
| | `card` | Memcard file for slot 2. Never overwrites an existing save. |
| | `sram` | SRAM file name. Default: `sram.bin` in `subdir`. |
| | `jvsmode` | `fighting` or `lightgun`. |
| | `args` | Arguments passed to the game ELF. |

## Notes

- **HDD media is writable**: the core opens flat images read-write and the game
  writes back to them. Don't use `-S/--symlink` for HDD games unless you intend
  the original image to be modified — the script warns about this. CHD images
  are read-only (writes are skipped).
- The script refuses to overwrite an existing `.acgame` or files already
  present in the target game directory.
- The boot ELF is not on the disc image for most System 246/256 games — it
  ships with the dongle dump. You need all three pieces: image, ELF, dongle.

## In-game controls (libretro core)

When an `.acgame` is loaded, the core feeds the RetroPad into the JVS I/O
board emulation (the per-game button layout — Tekken / standard / 6-button —
is selected automatically from the `gameid`):

| RetroPad | JVS function |
|---|---|
| D-Pad | Joystick |
| Y / X / B / A (+ L / R on 6-button games) | Game buttons per layout |
| Start | Start |
| **Select** | **Insert coin** |
| **L3** | **Test menu** (toggles the TESTMODE DIP switch) |
| **R3** | **Service button** |

## Examples

Fighting game on System 246 (Soul Calibur 3):

```bash
./make-acgame.sh -i game.chd -e boot.elf -d NM00031.bin \
    -n "Soul Calibur 3" -g SC3 -p 246 -j fighting -o ~/roms/namco2x6
```

Lightgun game on System 256, keeping the big image as a symlink:

```bash
./make-acgame.sh -i tc4.iso -e boot.elf -d dongle.bin \
    -n "Time Crisis 4" -g TC4 -p 256 -j lightgun -S -o ~/roms/namco2x6
```
