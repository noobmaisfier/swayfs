<p align="center">
    <img src="assets/swayfx_logo.svg" width="256" alt="swayfs logo">
</p>

# swayfs

**swayfs** is a personal fork of [SwayFX](https://github.com/wlrfx/swayfx),
itself a fork of [Sway](https://github.com/swaywm/sway). It keeps the SwayFX
visual effects while targeting lower RAM and CPU usage: the bundled bar and
wallpaper daemon are removed, Pango is replaced by fcft, and several hot paths
are optimised.

[![build](https://github.com/noobmaisfier/swayfs/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/noobmaisfier/swayfs/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/noobmaisfier/swayfs)](https://github.com/noobmaisfier/swayfs/releases/latest)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

- Source: <https://github.com/noobmaisfier/swayfs>
- Releases: <https://github.com/noobmaisfier/swayfs/releases>
- Issues: <https://github.com/noobmaisfier/swayfs/issues>

> **Status:** personal fork, provided as-is. The Arch package is not published
> on the AUR yet; use the installer, a release package, or build from source.
> Only `x86_64` is packaged for Arch.

## Contents

- [Why this fork](#why-this-fork)
- [Installation](#installation)
  - [Arch Linux](#arch-linux)
  - [Debian / Ubuntu](#debian--ubuntu)
  - [Nix](#nix)
  - [First start](#first-start)
- [Differences vs SwayFX](#differences-vs-swayfx)
- [Migrating from Sway or SwayFX](#migrating-from-sway-or-swayfx)
- [Configuration](#configuration)
- [Building from source](#building-from-source)
- [Keeping in sync with upstream](#keeping-in-sync-with-upstream)
- [Uninstall](#uninstall)
- [Contributing](#contributing)
- [Acknowledgements](#acknowledgements)
- [License](#license)

## Why this fork

- No bundled `swaybar` and no built-in `swaybg` integration; use an external
  bar (waybar, ironbar, …) and a standalone wallpaper daemon (swaybg, swww,
  hyprpaper, mpvpaper, …).
- Pango replaced by [fcft](https://codeberg.org/dnkl/fcft) for title text.
- Hot paths optimised: transactions, IPC, text rendering, seat operations.

Visual features inherited from SwayFX:

- blur, including xray and per-layer effects;
- anti-aliased rounded corners, borders and titlebars;
- shadows;
- dimming of unfocused windows;
- scratchpad treated as minimised (opt-in, see
  `scratchpad_minimize`);
- Nix flake for development.

## Installation

swayfs installs the `sway` binary and therefore **replaces** the `sway`
package. The Arch package declares:

```text
provides  = sway, wayland-compositor
conflicts = sway, swayfx, swayfx-git, swayfs-git
```

Your configuration files are not modified: `~/.config/sway/config` is left
alone, and `/etc/sway/config` is registered as a pacman backup file.

### Arch Linux

The official packages `scenefx` and `wlroots0.20` (both in `[extra]`) are used;
no AUR dependency is required.

#### A. Installer

The installer fetches the `PKGBUILD` for the current release, then `makepkg`
downloads the tagged source archive, verifies its checksum, builds and installs
the package. `base-devel` is installed automatically if missing.

```sh
curl -fsSL https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/main/packaging/aur/install.sh | sh
```

Inspect it first if you prefer:

```sh
curl -fsSL https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/main/packaging/aur/install.sh | less
```

#### B. Prebuilt package from a release

Release pages may include a prebuilt package
(`swayfs-<version>-<pkgrel>-x86_64.pkg.tar.zst`). Download it from
<https://github.com/noobmaisfier/swayfs/releases/latest>, then:

```sh
sudo pacman -U swayfs-<version>-<pkgrel>-x86_64.pkg.tar.zst
```

The package is built by the maintainer for the current Arch `x86_64` repos; if
your system is older than the build environment, prefer option A.

#### C. Manual build with makepkg

```sh
mkdir -p ~/build/swayfs && cd ~/build/swayfs
curl -fsSLO https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/main/packaging/aur/swayfs/PKGBUILD
curl -fsSLO https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/main/packaging/aur/swayfs/.SRCINFO
makepkg -si
```

See [`packaging/aur/README.md`](packaging/aur/README.md) for the release
checklist and local packaging instructions.

### Debian / Ubuntu

The same installer detects Debian/Ubuntu, installs the build dependencies with
`apt`, builds pinned `wlroots` and `scenefx` as Meson subprojects (Debian does
not ship them), and installs swayfs under `/usr/local`:

```sh
curl -fsSL https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/main/packaging/aur/install.sh | sh
```

`sudo` (or running as root) is required. See
[`INSTALL-deb.md`](INSTALL-deb.md) for the manual steps and an explanation of
the pinned dependencies.

### Nix

If you have Nix with flakes enabled:

```sh
nix build
./result/bin/sway
```

Or enter a development shell with all dependencies:

```sh
nix develop
```

> The flake is inherited from SwayFX and may lag behind swayfs-specific
> changes. It is convenient for development, but the Arch and Debian paths
> above are the supported installation methods.

### First start

Log out and pick **Sway** at your display manager (the session file is still
`sway.desktop`), or run `sway` from a TTY.

Verify the installation:

```sh
sway --version
# swayfs version 0.6 (based on sway 1.12.0)

pacman -Q swayfs      # Arch: check the package version
```

The package version is `0.6.swayfs.N`, while `sway --version` reports the base
fork version (`0.6`). Builds made from a git checkout may append the commit,
for example `0.6-d72c0fe7`. Note that `/usr/local/bin` must be in `PATH` on
Debian if you installed there manually.

## Differences vs SwayFX

| | SwayFX | swayfs |
|---|---|---|
| Bar | bundled `swaybar` | removed; use an external bar |
| Wallpaper | built-in `swaybg` integration | removed; use a standalone daemon |
| Text rendering | Pango | fcft |
| Renderer | scenefx `fx_renderer` | unchanged |
| Pango markup | styled | stripped, styling not applied |
| Branches | `dev` and `stable` | `main` only |
| Arch package | `swayfx` | `swayfs`, provides/conflicts `sway` |

The fork does **not** remove the SwayFX effects: blur, shadows, rounded
corners, dim-inactive and layer effects are still available.

## Migrating from Sway or SwayFX

### Packages

On Arch, installing `swayfs` removes `sway`, `swayfx`, `swayfx-git` or
`swayfs-git` because of the declared conflicts. Keep a copy of your config
before switching, and reinstall your previous compositor if you want to roll
back.

### Bar

The `swaybar` binary is no longer built. Remove `bar { … }` blocks from your
config and start an external bar instead:

```text
exec waybar
```

`bar { swaybar_command … }` only makes sense for clients that implement the
swaybar protocol; layer-shell bars such as waybar should be started with
`exec`.

### Wallpaper

The built-in `swaybg` integration was removed. Lines such as:

```text
output * bg ~/wallpaper.png fill
```

are no longer valid and will be reported as unknown commands. Remove them and
start a wallpaper daemon instead, for example:

```text
exec swaybg -i ~/wallpaper.png -m fill
# or: exec swww-daemon, exec hyprpaper, exec mpvpaper, ...
```

### Fonts and Pango markup

`font` still uses a family name followed by a point size:

```text
font monospace 10
font Noto Sans 11
```

Bold and italic are detected from the font string (`font Noto Sans Bold 11`).

The `pango:` prefix is accepted for compatibility and still enables markup
handling, but **markup styling is ignored**: tags are stripped before
rendering, so `<b>`, `<i>` and `<span>` do not produce bold, italic or colours.
Pick the style through the font name instead. Markup in `title_format` is
subject to the same limitation.

### Scratchpad

`scratchpad_minimize` still defaults to off. The upstream warning remains:
keep it off unless you accept the known rough edges.

## Configuration

The configuration file is unchanged: `~/.config/sway/config`, or
`/etc/sway/config` by default. The options below are inherited from SwayFX and
keep working, except where noted in
[Migrating from Sway or SwayFX](#migrating-from-sway-or-swayfx).

### SwayFX effects

- Animations: `animation_duration_ms <integer value 0-5000>`

- Blur:
  - `blur enable|disable`
  - `blur_xray enable|disable`: blur based on the background rather than the
    windows below; usually you want this `disable`.
  - `blur_passes <integer value 0-10>`
  - `blur_radius <integer value 0-10>`
  - `blur_noise <float value 0-1>`
  - `blur_brightness <float value 0-2>`
  - `blur_contrast <float value 0-2>`
  - `blur_saturation <float value 0-2>`

- Corner radius: `corner_radius <val>`

- Shadows:
  - `shadows enable|disable`
  - `shadows_on_csd enable|disable` (**note:** the shadow may not fit every
    window)
  - `shadow_blur_radius <integer value 0-99>`
  - `shadow_color <hex color with alpha>`, e.g. `#0000007F`
  - `shadow_offset <x offset> <y offset>`
  - `shadow_inactive_color <hex color with alpha>`, e.g. `#0000007F`

- Layer-shell effects (panels, notifications, …):
  - `layer_effects <layer namespace> <effects>`
  - List namespaces with:
    `swaymsg -r -t get_outputs | jq '.[0].layer_shell_surfaces | .[] | .namespace'`
  - Example:
    `layer_effects "waybar" blur enable; shadows enable; corner_radius 6`
    (GTK applications usually use the `gtk-layer-shell` namespace.)
  - Via Sway IPC, one effect at a time:
    `swaymsg layer_effects "waybar" "blur enable"`
  - Config example:

    ```text
    layer_effects "waybar" {
        blur enable;
        blur_xray enable;
        blur_ignore_transparent enable;
        shadows enable;
        corner_radius 20;
    }
    ```

  - Available effects:
    - `blur <enable|disable>`
    - `blur_xray <enable|disable>`
    - `blur_ignore_transparent <enable|disable>`
    - `shadows <enable|disable>`
    - `corner_radius <int>`
    - `reset`: reset all effects previously applied to that layer namespace

- Dim unfocused windows:
  - `default_dim_inactive <float value 0.0-1.0>`
  - `for_window [CRITERIA] dim_inactive <float value 0.0-1.0>`
  - `dim_inactive_colors.unfocused <hex color>`, e.g. `#000000FF`
  - `dim_inactive_colors.urgent <hex color>`, e.g. `#900000FF`

- Titlebar separator: `titlebar_separator enable|disable`

- Scratchpad as minimised: `scratchpad_minimize enable|disable` (**keep it
  off unless you accept the upstream rough edges**)

### Text rendering

- `font [pango:]<family> [style] <size>` — see
  [Fonts and Pango markup](#fonts-and-pango-markup).
- `pango:` only enables tag stripping; it does not restore Pango styling.

## Building from source

### Dependencies

Compile-time requirements:

- meson >= 1.3, ninja, pkg-config, a C compiler
- wayland-server >= 1.21, wayland-protocols >= 1.41
- wlroots 0.20.x and scenefx 0.5
- pixman, libdrm, libinput >= 1.26, libevdev, libxkbcommon >= 1.5
- json-c >= 0.13, pcre2, cairo, fcft, fontconfig, freetype
- libxcb and xcb-util-wm for XWayland
- optional: scdoc >= 1.9.2 (man pages), git (version info),
  gdk-pixbuf2 (extra tray image formats)

On Arch, the dependency set used by CI is:

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf git \
  wayland wayland-protocols libxkbcommon libinput libdrm libxcb \
  xcb-util-wm json-c pcre2 cairo fcft fontconfig libevdev pixman \
  scenefx wlroots0.20 scdoc
```

If `scenefx` or `wlroots 0.20` is not available on your distribution, place
them in `subprojects/`; Meson builds them automatically. On Debian/Ubuntu,
follow [INSTALL-deb.md](INSTALL-deb.md), which pins the required versions.

### Manual steps

```sh
git clone https://github.com/noobmaisfier/swayfs.git
cd swayfs

meson setup build --prefix=/usr/local -Dwerror=false -Dman-pages=enabled
meson compile -C build
sudo meson install -C build
```

On Debian, add `--libdir=lib` so the subproject libraries land where
`ldconfig` and `pkg-config` look by default, and pass
`-Dwlroots:werror=false -Dscenefx:werror=false` (see `INSTALL-deb.md`).

On systems without logind or seatd, the compositor needs the setuid bit:

```sh
sudo chmod u+s /usr/local/bin/sway
```

Prefer logind or seatd; setuid is a fallback. swayfs drops root privileges
shortly after startup.

### Container (Docker)

The container is a development aid, not an installation method. From the
repository root:

```sh
# build the container and run it (which builds swayfs)
docker compose up --build

# become root to install the previously built files
sudo bash

# copy all files into the host file system at /usr/local/
while read -r FILE; do
  NEW_FILE=/usr/local${FILE#opt}
  NEW_DIR=$(dirname "${NEW_FILE}")
  [[ -d ${NEW_DIR} ]] || mkdir -p "${NEW_DIR}"

  echo "Creating '${NEW_FILE}'"
  cp "${FILE}" "${NEW_FILE}"
done < <(command find opt/ -type f)
```

## Keeping in sync with upstream

`main` is the only branch. Releases are tagged `v0.6.swayfs.N`: `0.6` follows
the SwayFX release line and `N` is the fork revision (the Arch `pkgrel` is
independent).

To merge upstream changes, work on a dedicated branch rather than merging
directly into `main`:

```sh
git remote add upstream https://github.com/wlrfx/swayfx.git
git fetch upstream

git switch -c sync/upstream-$(date +%Y%m%d) main
git merge upstream/master
```

The merge is expected to conflict with the fork's removals and rewrites,
especially in `swaybar/`, `swaybg/`, `common/font.c`, `common/pango.c` and
`meson.build`. Do **not** resolve conflicts by taking upstream blindly: that
reintroduces `swaybar`, `swaybg` and Pango. Build, run the sanitizer and render
smoke tests, and only then fast-forward `main`.

## Uninstall

### Arch Linux

```sh
sudo pacman -Rns swayfs
```

This leaves your configuration in place. Reinstall `sway` or `swayfx`
afterwards if you still need a compositor.

### Debian / Ubuntu

The automated installer builds in a temporary directory and does not keep an
uninstall manifest, so it cannot uninstall for you. If you still have the
build directory, run:

```sh
sudo ninja -C build uninstall
```

Otherwise remove the files installed under `/usr/local` (`bin/sway`,
`bin/swaymsg`, `bin/swaynag`, man pages, `lib/sway`, subproject libraries) and
`/etc/ld.so.conf.d/swayfs-local.conf`, then run `sudo ldconfig`.

## Contributing

swayfs is a personal fork with a deliberately narrow scope: low memory/CPU
usage, bug fixes, and keeping up with upstream. New eye-candy features belong
upstream in [SwayFX](https://github.com/wlrfx/swayfx). Issues and pull
requests are welcome; see [`CONTRIBUTING.md`](CONTRIBUTING.md) for code style
and commit conventions.

GitHub Actions (`.github/workflows/build.yml`) builds on pushes to `main` and
on pull requests targeting `main`, in Arch and Debian containers. The
repository also ships a GitLab CI pipeline (`.gitlab-ci.yml`) that runs the
ASan/UBSan headless smoke test (`ci/asan-smoke.sh`) and the framebuffer
rendering regression test (`ci/render-smoke.sh`). `ci/measure-ram.sh` is a
developer tool for comparing memory use between builds.

## Acknowledgements

swayfs would not exist without the Sway maintainers and contributors, and
without the SwayFX team, who built the effects and the renderer this fork is
based on.

The visual assets come from the SwayFX community:

- [@pkdesuwu](https://twitter.com/pkdesuwu) and honchokomodo for the mascot;
- spooky_skeleton for the logo, with adjustments by
  [Basil](https://basil.cafe).

The upstream SwayFX Discord is a good place for general SwayFX questions:
<https://discord.gg/qsSx397rkh>. Please report swayfs-specific bugs on this
repository instead.

## License

MIT, inherited from Sway and SwayFX. See [`LICENSE`](LICENSE).
