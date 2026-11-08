# KeeperFX on macOS

KeeperFX builds natively for macOS on Apple Silicon (M1-M4, arm64). Intel Macs
should work the same way but are untested. It uses the same POSIX platform layer
as the Linux build, SDL3 for the window and input, OpenGL 4.1 (core) for the
renderer and OpenAL Soft for sound. The game data is the same as on Windows;
only the executable differs.

This port is new and not yet built in CI. Expect rough edges, and report what
breaks along with `keeperfx.log`.

## 1. Install the tools and libraries

Install the Xcode command line tools (C/C++ compiler, linker, SDK):

```bash
xcode-select --install
```

Install [Homebrew](https://brew.sh) if you do not have it yet, then the build
tools and libraries:

```bash
brew install cmake ninja pkgconf git sdl3 sdl3_image sdl3_mixer ffmpeg openal-soft luajit libspng minizip zlib miniupnpc libnatpmp curl
```

Astronomy, centijson and ENet6 have no Homebrew packages; the CMake configure
step downloads their source and builds them itself.

## 2. Build

From the KeeperFX source folder:

```bash
cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

```bash
cmake --build out --target keeperfx
```

The result is `out/keeperfx`. `--target keeperfx_hvlog` also builds the
heavy-log version for bug reports.

Compiler warnings do not stop the macOS build (Apple clang warns about things
GCC does not). To make them errors, as on Windows and Linux, configure with
`-DKFX_MACOS_WERROR=ON`.

## 3. Set up a game folder

1. **Get a full KeeperFX install.** Download the latest complete release from
   [keeperfx.net](https://keeperfx.net) or the
   [GitHub releases](https://github.com/dkfans/keeperfx/releases) and unpack it
   into a folder of your choice, e.g. `~/Games/keeperfx`. The Windows `.exe`
   and `.dll` files in it are not used on the Mac.
2. **Update the game data to match your build.** A build from current source
   needs the data from the newest alpha patch: unpack the latest alpha patch
   from [keeperfx.net](https://keeperfx.net) over the install.
3. **Add the original Dungeon Keeper files.** Copy the files listed in
   [files_required_from_original_dk.txt](files_required_from_original_dk.txt)
   (`data/*.dat|pal|col` and `sound/*.sbk`) from your Dungeon Keeper CD, or
   GOG/Steam/EA install, into the same folders of the KeeperFX install. File
   name case does not matter.
4. **Copy in the Mac executable:**

   ```bash
   cp out/keeperfx ~/Games/keeperfx/
   ```

## 4. Run

Run it from the game folder; KeeperFX looks for its data in the current folder:

```bash
cd ~/Games/keeperfx && ./keeperfx
```

Command line options are the same as on Windows, see
[Command Line Options](https://github.com/dkfans/keeperfx/wiki/Command-Line-Options).
The game writes `keeperfx.log` in the game folder. Settings saved per user go
to `~/Library/Application Support/keeperfx/keeperfx/`.

The executable links to the Homebrew libraries where they are installed, so it
runs on the Mac that built it (or one with the same Homebrew packages). There is
no self-contained `.app` bundle yet.

## Known limitations

- **Multiplayer with other platforms is not guaranteed.** Multiplayer needs every
  machine to compute the same game state. The build disables fused
  multiply-add (`-ffp-contract=off`) for that, but ARM maths libraries may
  still round some results differently from Windows. Single player,
  Mac-to-Mac games and replays recorded on the Mac are not affected.
- **No `.app` bundle or code signing for distribution yet.** A binary you build
  yourself runs without Gatekeeper prompts; one downloaded from elsewhere needs
  `xattr -d com.apple.quarantine keeperfx` first.
- **Memory-change tracking** (Linux soft-dirty pages / Windows write watch) has
  no macOS version; the game logs "Kernel memory tracking is not available"
  and uses its fallback.
- **OpenGL is deprecated by Apple** but still supported on Apple Silicon (4.1
  core profile, which the renderer needs).

## Troubleshooting

- **`Homebrew not found`**: install Homebrew, or make sure `brew` is on your
  `PATH` (`eval "$(/opt/homebrew/bin/brew shellenv)"`).
- **`Package 'xyz' not found`** during configure: install that package with
  `brew install`, then delete `out/` and configure again.
- **`dyld: Library not loaded`** when starting: a Homebrew package was removed
  or upgraded to a new major version; reinstall it, or rebuild.
- **The game starts but cannot find its files**: run it from the game folder
  (`cd` into it first), not from `out/`.
