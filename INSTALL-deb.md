# Installing swayfs on Debian 13 (Trixie/Sid)

> **Automated installer:** the script below performs every step in this guide
> on Debian/Ubuntu (and Arch), including dependency installation:
>
> ```sh
> curl -fsSL https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/main/packaging/aur/install.sh | sh
> ```
>
> The manual steps remain the reference implementation. They install to
> `/usr/local` and require `sudo` (or a root shell).

> **Note:** this guide targets **swayfs 0.6** (based on **SwayFX 0.6**, itself
> based on **Sway 1.12**). Debian does not package the required
> **wlroots 0.20.2** and **scenefx 0.5**, so they are built as Meson
> subprojects.

## 1. Install dependencies

Debian Trixie needs development headers for the wlroots 0.20 backend,
including `liblcms2`, `libliftoff` and `libdisplay-info`:

```bash
sudo apt update && sudo apt install -y \
  meson pkg-config cmake git scdoc bison python3-jinja2 \
  wayland-protocols libwayland-dev libpcre2-dev libjson-c-dev \
  libfcft-dev libfontconfig-dev libfreetype-dev libcairo2-dev \
  libdrm-dev libgbm-dev libinput-dev libevdev-dev libseat-dev libxkbcommon-dev \
  libxcb-dri3-dev libxcb-present-dev libxcb-res0-dev \
  libxcb-render-util0-dev libxcb-ewmh-dev libxcb-icccm4-dev \
  libxcb1-dev libxcb-composite0-dev libxcb-render0-dev \
  libliftoff-dev libdisplay-info-dev liblcms2-dev libpixman-1-dev \
  libegl-dev libgles-dev libudev-dev \
  libffi-dev libexpat1-dev libxml2-dev hwdata \
  libxcb-errors-dev libxcb-xinput-dev libvulkan-dev glslang-tools xwayland
```

## 2. Fetch the sources and subprojects

```bash
mkdir -p ~/build
cd ~/build

git clone https://github.com/noobmaisfier/swayfs.git
cd swayfs
git checkout v0.6.swayfs.4   # or: git checkout main
```

`ci/fetch-subprojects.sh` clones the pinned dependencies into
`subprojects/` (wayland 1.24.0, wayland-protocols 1.47, libdrm 2.4.134,
libxkbcommon 1.9.2, wlroots 0.20.2, scenefx 0.5):

```bash
WLR_TAG=0.20.2 bash ci/fetch-subprojects.sh
```

If your system already provides a recent enough library, skip the
corresponding clone with `SKIP_WAYLAND=1`, `SKIP_WAYLAND_PROTOCOLS=1`,
`SKIP_LIBDRM=1` or `SKIP_LIBXKBCOMMON=1`.

The resulting tree looks like:

```text
~/build/swayfs
└── subprojects
    ├── wayland (v1.24.0)
    ├── wayland-protocols (v1.47)
    ├── libxkbcommon (v1.9.2)
    ├── libdrm (v2.4.134)
    ├── scenefx (v0.5)
    └── wlroots (v0.20.2)
```

## 3. Install pixman 0.46 to /usr/local

wlroots 0.20 unconditionally builds its pixman renderer, which requires
pixman >= 0.46.0 and has no Meson fallback (Debian Trixie ships 0.44):

```bash
bash ci/install-pixman-prefix.sh
```

## 4. Compile and install

Meson detects the sources in `subprojects/` and builds them automatically.
Per-subproject `werror=false` is needed because wlroots/scenefx force
`werror=true` and GCC 14 trips over some of their warnings. `--libdir=lib`
ensures the subproject libraries land where `ldconfig` and `pkg-config` look
by default.

```bash
meson setup build/ --prefix=/usr/local --libdir=lib \
  -Dwerror=false -Dman-pages=enabled \
  -Dwlroots:werror=false -Dscenefx:werror=false \
  -Dwayland:werror=false -Dwayland-protocols:werror=false \
  -Dlibdrm:werror=false -Dlibxkbcommon:werror=false

meson compile -C build/
sudo meson install -C build/
```

## 5. Update the linker cache

Debian does not index `/usr/local/lib` subdirectories immediately. Register
the multiarch directory if Meson created it, then refresh the cache:

```bash
if [ -d /usr/local/lib/x86_64-linux-gnu ]; then
  echo "/usr/local/lib/x86_64-linux-gnu" | sudo tee /etc/ld.so.conf.d/swayfs-local.conf
fi
sudo ldconfig
```

## 6. Install runtime dependencies

```bash
sudo apt install -y --no-install-recommends \
  libgl1 libegl1 libgles2 libgbm1 mesa-vulkan-drivers \
  libgl1-mesa-dri fonts-dejavu-core
```

## 7. Verify and start

```bash
sway --version
# swayfs version 0.6 (based on sway 1.12.0)
# git checkouts may append the commit, e.g. swayfs version 0.6-d72c0fe7 ...
```

If `/usr/local/bin` is not in your `PATH`, use the absolute path or add it to
your shell profile.

Log out and pick **Sway** at your display manager (the session file is
`sway.desktop`), or run `sway` from a TTY.
