# Arch Linux packaging

`swayfs/PKGBUILD` builds this fork from a release tag and relies on the
official `[extra]` packages `scenefx` and `wlroots0.20` — no bundled or AUR
dependency is required.

The package is called `swayfs`, provides `sway`, and conflicts with `sway`,
`swayfx`, `swayfx-git` and `swayfs-git`.

The package is **not published on the AUR yet**. These files are the source of
truth until it is.

## Quick start (no AUR account needed)

The source repository is public. The fastest way is the convenience installer,
which auto-detects Arch vs Debian/Ubuntu:

```sh
curl -fsSL https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/main/packaging/aur/install.sh | sh
```

Or fetch the packaging files yourself and build:

```sh
mkdir -p ~/build/swayfs && cd ~/build/swayfs
curl -fsSLO https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/main/packaging/aur/swayfs/PKGBUILD
curl -fsSLO https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/main/packaging/aur/swayfs/.SRCINFO
makepkg -si
```

A prebuilt package may also be attached to a GitHub release:
<https://github.com/noobmaisfier/swayfs/releases>.

## Build locally

The release tag is `v<pkgver>`, for example `v0.6.swayfs.4`. `makepkg` downloads
the GitHub tag archive and verifies its checksum:

```sh
cd packaging/aur/swayfs
makepkg -si
```

To build from a local working tree instead (for example before re-tagging),
create a source tarball with the exact expected name and prefix so makepkg
skips the download:

```sh
# From the repository root. The GitHub archive prefix is swayfs-<pkgver>, with
# no "v".
git archive --format=tar.gz --prefix=swayfs-0.6.swayfs.4/ \
    -o swayfs-0.6.swayfs.4.tar.gz HEAD

cd packaging/aur/swayfs
makepkg -od                  # source verification/extraction only
sudo pacman -S --needed --asdeps base-devel scenefx wlroots0.20
makepkg -si                  # full build + install
```

## Release checklist

1. Bump `pkgver` in `PKGBUILD` to the new fork revision (for example
   `0.6.swayfs.5`), keep `pkgrel=1` unless repackaging the same version.
2. Tag the validated `main` commit with `v<pkgver>` and push the tag:
   ```sh
   git tag -a v0.6.swayfs.5 -m "swayfs 0.6.swayfs.5"
   git push origin v0.6.swayfs.5
   ```
3. Refresh the checksum (replace the `sha256sums=('…')` line):
   ```sh
   cd packaging/aur/swayfs
   makepkg -g
   ```
4. Regenerate the AUR metadata:
   ```sh
   makepkg --printsrcinfo > .SRCINFO
   ```
5. Build the release package, test it, and attach it to the GitHub release.
6. Validate on a clean system (or in a chroot, e.g. `extra-x86_64-build`) and
   lint with `namcap PKGBUILD swayfs-*.pkg.tar.zst`.

The PKGBUILD removes any previous `build/` directory before configuring. Do
not "fix" that: reusing a build directory configured against an older release
archive has already shipped a package containing the wrong binary.
