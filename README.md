# obscure-coop

Online co-op for **Obscure** (Hydravision, 2004) over Steam: two players on two PCs play the story
together. The game's own two-player local co-op provides player 2; this mod lets that player sit at
another PC. It is a proxy `xinput1_3.dll` placed next to `Obscure.exe`. It changes no game files and
ships none.

Works only with the **Steam version, build 215286** (`Obscure.exe` SHA-256
`7107439eb6ee1b20cad32e87f7a10a3a95b71c0b5ee26b1ec9139b263577a0f2`). With any other exe the game
closes at start and the log says why.

How it was built, every address and the evidence: the
[Obscure write-up in coop-anything](https://github.com/cranebeaker/coop-anything/tree/main/games/obscure)
(`README.md`, and `NOTES.md` for the full research log).

## What is shared

The host's game is the real one. The friend's game mirrors it:

- Both characters: position, facing, animation state, attacks, health, deaths, character switches.
  Movement is smoothed over the internet (shown about one packet behind, blended between updates).
- Enemies (smoothed the same way), their health and deaths.
- Doors (with their animation), items picked up, story flags, level changes.
- Weapons per character, the team bag, the Give action, each player's own inventory menu.
- Pickup pictures and messages.
- **Loading a save:** when the host loads, the friend's game loads the same save by itself. At join
  the friend also gets and loads the host's save automatically.
- When a player dies, the pause menu opens on the host's screen after 3 s (LOAD or RESUME).
  F1 opens it at any time.

Optional graphics, per PC (do not need to match):

- **Supersampling** (`SSAA`): the game renders at 2x (or 4x) width and height and is averaged down.
  Menu text keeps its normal size (`TEXT_SCALE`). Fullscreen becomes a borderless window.
- **16x anisotropic filtering** (`ANISO`).
- ReShade presets (see Graphics below).

## Play

Both players need Obscure on Steam, the same build, and Steam running.

**Friend (player 2):**

1. Steam > Obscure > Manage > Browse local files. Copy `xinput1_3.dll` and `config/obscoop-guest.ini`
   (renamed to `obscoop.ini`) next to `Obscure.exe`.
2. Start Obscure from Steam. The window title shows the status ("co-op guest: connected, 40 ms").
3. The host's save arrives and loads by itself. Press START on your controller; the host confirms.

**Host (player 1):** use a copy of the game folder for hosting, put `xinput1_3.dll` and
`config/start_host.bat` in it and start the bat. The title shows "co-op host: waiting for a guest",
then "connected". Load your save; confirm "Player 2 joins" when your friend presses START. In the
game's controller settings use Player 1 = Game controller 1, Player 2 = Game controller 2.

Both players must run the same mod version (the protocol version is checked; different versions do
not connect).

### Settings

`obscoop.ini` section `[obscoop]`, or the same name with `OBSCOOP_` in front as an environment
variable (the host's bat uses those).

| Setting | Values |
|---|---|
| `ROLE` | `host` or `guest` |
| `NET` | `steam` for play (set in both config files); missing = `udp`, used by the one-PC tests |
| `HOST_STEAMID` | guest: the host's 17-digit Steam ID; empty = the friend who is playing Obscure |
| `LOCAL_PAD`, `REMOTE_PAD`, `VPADS`, `HIDE_REAL_PADS` | controller mapping, see the two config files |
| `SSAA` | 1 or missing = off, 2, 4 |
| `TEXT_SCALE` | text size with SSAA; empty = same as `SSAA` |
| `ANISO` | 0 or missing = off, 2..16 |
| `SMOOTH_MS` | movement smoothing delay; empty = adaptive, `0` = off, a number = fixed ms |

Log: `obscoop.log` next to `Obscure.exe` (`%LOCALAPPDATA%\obscoop\` when the game is under Program
Files). Co-op saves go to `coop_profile\` in the same place; your own saves are not touched.

## Graphics (optional ReShade)

`graphics/` holds a ReShade configuration that was tuned for this game. It is not ReShade itself:

1. Get ReShade 6.8.0 from [reshade.me](https://reshade.me) (tested: `ReShade_Setup_6.8.0.exe`,
   signer thumbprint `589690208A5E52FB96980C4A6698F50ACD47C49F`). Install it for `Obscure.exe`,
   Direct3D 9, or put its `ReShade32.dll` next to `Obscure.exe` as `d3d9.dll`.
2. Shaders, into `reshade-shaders/Shaders` and `Textures` next to the exe:
   [crosire/reshade-shaders](https://github.com/crosire/reshade-shaders) (`slim`, tested at `fd00221`),
   [CeeJayDK/SweetFX](https://github.com/CeeJayDK/SweetFX) (tested at `93ddf39`),
   [martymcmodding/qUINT](https://github.com/martymcmodding/qUINT) (tested at `98fed77`).
3. Copy everything in `graphics/` next to `Obscure.exe`.

Presets: `ObscureCoop.ini` (MXAO ambient occlusion, SMAA, Deband, CAS sharpening, light vibrance)
and `ObscureCoop-Horror.ini` (adds cooler colours, contrast, film grain, vignette). Home = ReShade
menu, Scroll Lock = effects on/off.

Two settings are needed for this game:

- `ReShade2.ini` must exist. With `SSAA` the mod presents through an extra swap chain, and ReShade
  configures that one from `ReShade2.ini`; the file here is a copy of `ReShade.ini`.
- `[DEPTH] DepthCopyBeforeClears=1`: without it ReShade finds no depth, so MXAO does nothing.

## Build

```sh
sh build.sh    # needs an i686 MinGW gcc/clang on PATH (e.g. llvm-mingw); writes build/xinput1_3.dll
```

`src/obscoop.c` includes the other files: `net.c` (packets, Steam and UDP transport), `steam.c`
(Steam interfaces by vtable), `sync.c` (everything that is mirrored), `render.c` (supersampling,
text size, filtering). Addresses are for the build above only and are checked at start.

## Tests

Bot tests drive two real copies of the game on one PC (host and guest over UDP) through a named pipe
in the DLL (virtual pads, keys, test commands) and read both processes' memory. They assert on game
state, not on screenshots.

1. Make test copies of your game folder inside this checkout: `obscure-test2/` (guest) and
   `obscure-test3/` (host). `.gitignore` keeps them out of git. In each, set the in-game controller
   settings as above, and put a save in `coop_profile/remote/game0.sav` of the host copy.
2. `export STEAM_USERDATA="/c/Program Files (x86)/Steam/userdata/<your account id>"` (the tests
   fingerprint your real install and saves before and after, and report any change).
3. `sh tests/suite.sh` runs all tests, or `sh tests/suite.sh load death` some of them.
   `tests/chaos_run.sh` runs the lag and loss test through the coop-anything chaos relay
   (`COOP_ANYTHING` = path to a coop-anything checkout).

Results so far are in the write-up. The suite refuses to start while any Obscure is running.

## Known issues

- On the friend's screen, ladders, racks and locks do not animate (the result is still mirrored).
- Two players only (the game has two player slots).
- The lag and packet-loss test has not been run.

## License

MIT, see `LICENSE`. Obscure is © its owners; this repository contains no game files or assets.
