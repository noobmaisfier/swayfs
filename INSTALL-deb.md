# A guide to installing SwayFX on Debian 13 (Trixie/Sid)

> **swayfs note:** the automated installer does all of this for you on
> Debian/Ubuntu (and Arch):
> `curl -fsSL https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/stable/packaging/aur/install.sh | sh`
> The manual steps below remain the reference. They apply to swayfs as well —
> just clone `https://github.com/noobmaisfier/swayfs` instead of upstream
> SwayFX.

> **Note:** This guide targets **SwayFX 0.6** (based on Sway 1.12). It manually builds the required dependencies (**wlroots 0.20.2** and **scenefx 0.5**) using Meson subprojects to ensure compatibility and stability.

## 1\. Install Dependencies

Debian Trixie requires a specific set of development headers, particularly for the newer `wlroots` 0.19 backend (which needs `liblcms2`, `libliftoff`, and `libdisplay-info`).

```bash
sudo apt update && sudo apt install -y \
  meson pkg-config cmake git scdoc bison python3-jinja2 \
  wayland-protocols libwayland-dev libpcre2-dev libjson-c-dev \
  libfcft-dev libfontconfig-dev libfreetype-dev libcairo2-dev \
  libdrm-dev libgbm-dev libinput-dev libseat-dev libxkbcommon-dev \
  libxcb-dri3-dev libxcb-present-dev libxcb-res0-dev \
  libxcb-render-util0-dev libxcb-ewmh-dev libxcb-icccm4-dev \
  libxcb1-dev libxcb-composite0-dev libxcb-render0-dev \
  libliftoff-dev libdisplay-info-dev liblcms2-dev libpixman-1-dev \
  libgles2-mesa-dev hwdata libudev-dev \
  libffi-dev libexpat1-dev libxml2-dev \
  libxcb-errors-dev libxcb-xinput-dev libvulkan-dev glslang-tools xwayland
```

## 2\. Setup Build Environment

We will use the **Meson Subprojects** method. This places the specific versions of `wlroots` and `scenefx` directly inside the build folder, preventing conflicts with system libraries.

```bash
# Create a clean build directory
mkdir -p ~/build
cd ~/build

# 1. Clone SwayFX 0.6 (The Window Manager)
git clone https://github.com/WillPower3309/swayfx.git
cd swayfx
git checkout 0.6

# 2. Setup Subprojects Directory
mkdir subprojects
cd subprojects

# 3. Clone Wayland 1.24 (Debian Trixie ships 1.23, wlroots 0.20 needs >= 1.24)
git clone https://gitlab.freedesktop.org/wayland/wayland.git
cd wayland
git checkout 1.24.0
cd ..

# 4. Clone wayland-protocols 1.47 (Debian Trixie ships 1.44, wlroots needs >= 1.47)
git clone https://gitlab.freedesktop.org/wayland/wayland-protocols.git wayland-protocols
cd wayland-protocols
git checkout 1.47
cd ..

# 5. Clone libxkbcommon 1.9.2 (Debian Trixie ships 1.7, wlroots needs >= 1.8)
git clone https://github.com/xkbcommon/libxkbcommon.git libxkbcommon
cd libxkbcommon
git checkout xkbcommon-1.9.2
cd ..

# 6. Clone libdrm 2.4.134 (Debian Trixie ships 2.4.124, wlroots needs >= 2.4.129)
git clone https://gitlab.freedesktop.org/mesa/drm.git libdrm
cd libdrm
git checkout libdrm-2.4.134
cd ..

# 7. Clone SceneFX 0.5 (The Rendering FX Library)
git clone https://github.com/wlrfx/scenefx.git
cd scenefx
git checkout 0.5
cd ..

# 8. Clone Wlroots 0.20.2 (The Wayland Compositor Backend)
git clone https://gitlab.freedesktop.org/wlroots/wlroots.git
cd wlroots
git checkout 0.20.2

# Return to source root
cd ../..
```

Your directory structure should now look like this:

```text
~/build/swayfx
└── subprojects
    ├── wayland (v1.24.0)
    ├── wayland-protocols (v1.47)
    ├── libxkbcommon (v1.9.2)
    ├── libdrm (v2.4.134)
    ├── scenefx (v0.5)
    └── wlroots (v0.20.2)
```

## 3\. Install pixman 0.46 to /usr/local

wlroots 0.20 unconditionally builds its pixman renderer, which requires
pixman >= 0.46.0 with no meson fallback (Debian Trixie ships 0.44):

```bash
bash ci/install-pixman-prefix.sh
```

## 4\. Compile and Install

Now we build everything in one go. Meson will detect the code in `subprojects/` and build it automatically.

```bash
# Configure the build (per-subproject werror=false: wlroots/scenefx force
# werror=true, which GCC 14 trips over)
meson setup build/ -Dwerror=false -Dwlroots:werror=false -Dscenefx:werror=false \
  -Dwayland:werror=false -Dwayland-protocols:werror=false -Dlibdrm:werror=false \
  -Dlibxkbcommon:werror=false

# Compile
ninja -C build/

# Install
sudo ninja -C build/ install
```

## 4\. Finalize

Debian does not automatically link new libraries in `/usr/local/lib` immediately. You must update the linker cache to avoid "shared object not found" errors.

```bash
sudo ldconfig
```

## 5\. Verification

Check that the installation was successful and is using the correct version base.

```bash
sway --version
# Expected Output: 
# swayfx version 0.6... (based on sway 1.12.0)

```

  + Guide updated for Debian 13 / SwayFX 0.6 with ♥️
