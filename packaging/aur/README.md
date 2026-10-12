# Arch Linux packaging (AUR)

`swayfs/PKGBUILD` builds this fork from a release tag and relies on the
official `[extra]` packages `scenefx` and `wlroots0.20` — no bundled or AUR
dependency is required.

## Quick start (no AUR account needed)

The source repository is public. The fastest way is the convenience installer:

```sh
curl -fsSL https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/stable/packaging/aur/install.sh | sh
```

Or fetch the packaging files yourself and build:

```sh
mkdir -p ~/build/swayfs && cd ~/build/swayfs
curl -fsSLO https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/stable/packaging/aur/swayfs/PKGBUILD
curl -fsSLO https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/stable/packaging/aur/swayfs/.SRCINFO
makepkg -si
```

## Build locally

The source repository is **public**:
<https://github.com/noobmaisfier/swayfs>. Once the release tag
`v0.6.swayfs.2` is pushed, makepkg downloads the archive itself:

```sh
cd packaging/aur/swayfs
makepkg -si
```

To build from a local working tree instead (for example before re-tagging),
create a source tarball with the exact expected name/prefix so makepkg skips
the download:

```sh
git -C ../../.. archive --format=tar.gz --prefix=swayfs-v0.6.swayfs.2/ \
    -o swayfs-v0.6.swayfs.2.tar.gz HEAD

makepkg -od          # source download/verification/extraction only
sudo pacman -S --needed --asdeps base-devel scenefx
makepkg -si          # full build + install
```

## Release checklist

1. Tag the validated `stable` commit, e.g. `v0.6.swayfs.2`, and push the tag.
2. Refresh the checksum (replace the `sha256sums=('SKIP')` line):
   ```sh
   cd packaging/aur/swayfs
   makepkg -g
   ```
3. Regenerate the AUR metadata:
   ```sh
   makepkg --printsrcinfo > .SRCINFO
   ```
4. Validate on a clean system (or in a chroot, e.g. `extra-x86_64-build`) and
   lint with `namcap PKGBUILD swayfs-*.pkg.tar.zst`.

## Publishing to the AUR

The AUR is a **source** repository (users compile locally) and only serves
publicly fetchable sources over **SSH**:

1. Create an account on <https://aur.archlinux.org> and register an SSH public
   key (Account → SSH Public Key). Generate one if needed:
   ```sh
   ssh-keygen -t ed25519 -C aur
   cat ~/.ssh/id_ed25519.pub
   ```
2. Clone the (empty) package repo, copy the packaging files and push:
   ```sh
   git clone ssh://aur@aur.archlinux.org/swayfs.git
   cp packaging/aur/swayfs/{PKGBUILD,.SRCINFO} swayfs/
   cd swayfs
   git add PKGBUILD .SRCINFO
   git commit -m "Initial import: swayfs 0.6.swayfs.2-1"
   git push
   ```
3. Optional VCS package `swayfs-git` follows the same model with
   `source=("$pkgname::git+https://github.com/noobmaisfier/swayfs.git#branch=dev")`
   and a `pkgver()` function.

> The source repository is public, so makepkg can fetch the release archive on
> any machine.
