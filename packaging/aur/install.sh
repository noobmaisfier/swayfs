#!/bin/sh
# swayfs one-shot installer for Arch Linux and Debian/Ubuntu.
#
# Detects the distribution and installs the current release:
#   - Arch:   fetches the PKGBUILD (release tag archive + checksum) and runs
#             `makepkg -si` (needs base-devel + an AUR-style toolchain).
#   - Debian: installs build dependencies with apt, fetches the release
#             branch tarball, builds pinned wlroots/scenefx as meson
#             subprojects, then builds and installs swayfs to /usr/local.
#
# The repository is public, so no account is needed. Nothing is executed
# from an untrusted source beyond this script itself: the PKGBUILD and its
# checksum live in this repository, the Debian path builds a signed GitHub
# tag/branch tarball.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/noobmaisfier/swayfs/refs/heads/main/packaging/aur/install.sh | sh
#
# Environment:
#   SWAYFS_BRANCH   branch to install from (default: main)
#   MAKEPKG         makepkg command to run on Arch (default: makepkg)
set -eu

BRANCH="${SWAYFS_BRANCH:-main}"
REPO="noobmaisfier/swayfs"
RAW="https://raw.githubusercontent.com/$REPO/refs/heads/$BRANCH"
MAKEPKG="${MAKEPKG:-makepkg}"

if [ "$(id -u)" -eq 0 ]; then
	SUDO=""
else
	SUDO="sudo"
fi

detect_distro() {
	if [ -f /etc/arch-release ]; then
		echo arch
	elif [ -f /etc/debian_version ]; then
		echo debian
	else
		# shellcheck disable=SC1091
		. /etc/os-release 2>/dev/null || true
		case "${ID:-}${ID_LIKE:-}" in
			*arch*) echo arch ;;
			*debian*|*ubuntu*) echo debian ;;
			*) echo unknown ;;
		esac
	fi
}

require_cmd() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "swayfs: required command not found: $1" >&2
		exit 1
	fi
}

install_arch() {
	# Bootstrap curl if missing (downloading the PKGBUILD needs it).
	if ! command -v curl >/dev/null 2>&1; then
		if ! command -v pacman >/dev/null 2>&1; then
			echo "swayfs: required command not found: curl" >&2
			exit 1
		fi
		echo "swayfs: installing curl..."
		$SUDO pacman -Sy --noconfirm --needed curl
	fi
	# base-devel provides makepkg (auto-install for a true one-shot setup).
	if ! command -v "$MAKEPKG" >/dev/null 2>&1; then
		echo "swayfs: installing base-devel (provides $MAKEPKG)..."
		$SUDO pacman -Sy --noconfirm --needed base-devel
	fi
	for c in curl "$MAKEPKG"; do require_cmd "$c"; done

	missing=""
	for c in gcc make fakeroot; do
		command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
	done
	if [ -n "$missing" ]; then
		echo "swayfs: installing base-devel (missing:$missing)..."
		$SUDO pacman -Sy --noconfirm --needed base-devel
		for c in gcc make fakeroot; do require_cmd "$c"; done
	fi

	tmp="$(mktemp -d)"
	trap 'rm -rf "$tmp"' EXIT
	cd "$tmp"

	echo "swayfs: fetching PKGBUILD from branch '$BRANCH'..."
	curl -fsSLO "$RAW/packaging/aur/swayfs/PKGBUILD"
	curl -fsSLO "$RAW/packaging/aur/swayfs/.SRCINFO"

	echo "swayfs: building and installing (makepkg will ask for your sudo password)..."
	"$MAKEPKG" -si --noconfirm
}

install_debian() {
	# Bootstrap: apt + (root or sudo) + network tools come first, since a
	# minimal container/netinstall may lack even curl and git.
	if [ "$(id -u)" -ne 0 ] && ! command -v sudo >/dev/null 2>&1; then
		echo "swayfs: run as root or install sudo first." >&2
		exit 1
	fi
	if ! command -v curl >/dev/null 2>&1 || ! command -v git >/dev/null 2>&1; then
		echo "swayfs: installing bootstrap tools (curl, git)..."
		$SUDO apt-get update
		$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
			ca-certificates curl git
	fi
	for c in curl git; do require_cmd "$c"; done
	echo "swayfs: installing build dependencies with apt..."
	# apt dependency list mirrors INSTALL-deb.md and .github/workflows/build.yml.
	# Debian ships neither wlroots-0.20 nor scenefx, so both are built as
	# pinned meson subprojects (ci/fetch-subprojects.sh).
	$SUDO apt-get update
	# shellcheck disable=SC2086
	$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		ca-certificates git meson ninja-build gcc pkg-config scdoc bison python3-jinja2 \
		wayland-protocols libwayland-dev libpcre2-dev libjson-c-dev \
		libfcft-dev \
		libfontconfig-dev libfreetype-dev libcairo2-dev \
		libdrm-dev libgbm-dev libinput-dev libevdev-dev libseat-dev libxkbcommon-dev \
		libxcb-dri3-dev libxcb-present-dev libxcb-res0-dev \
		libxcb-render-util0-dev libxcb-ewmh-dev libxcb-icccm4-dev \
		libxcb1-dev libxcb-composite0-dev libxcb-render0-dev \
		libliftoff-dev libdisplay-info-dev liblcms2-dev libpixman-1-dev \
		libegl-dev libgles-dev hwdata libudev-dev libffi-dev libexpat1-dev libxml2-dev \
		libxcb-errors-dev libxcb-xinput-dev libvulkan-dev glslang-tools xwayland

	tmp="$(mktemp -d)"
	trap 'rm -rf "$tmp"' EXIT
	cd "$tmp"

	echo "swayfs: fetching branch '$BRANCH' tarball..."
	if [ -n "${SWAYFS_SRC_DIR:-}" ]; then
		# Dev/test hook: build a local tree instead of downloading the
		# branch tarball (tracked files only, subprojects excluded).
		echo "swayfs: using local source $SWAYFS_SRC_DIR (SWAYFS_SRC_DIR set)..."
		mkdir -p "swayfs-$BRANCH"
		git -C "$SWAYFS_SRC_DIR" -c safe.directory='*' archive HEAD | tar -x -C "swayfs-$BRANCH"
	else
		curl -fsSL -o swayfs.tar.gz "https://github.com/$REPO/archive/refs/heads/$BRANCH.tar.gz"
		tar -xzf swayfs.tar.gz
	fi
	cd "swayfs-$BRANCH"

	echo "swayfs: fetching pinned wlroots/scenefx subprojects..."
	bash ci/fetch-subprojects.sh

	echo "swayfs: installing pixman 0.46 to /usr/local (wlroots needs >= 0.46)..."
	$SUDO bash ci/install-pixman-prefix.sh

	echo "swayfs: building..."
	# Per-subproject werror=false, see .github/workflows/build.yml.
	# --libdir=lib: meson's Debian default (lib64 triplet path) is in
	# neither ldconfig's nor pkg-config's default search path; /usr/local/lib
	# is in both, so the subproject libraries are found at runtime.
	meson setup build --prefix=/usr/local --libdir=lib -Dwerror=false -Dman-pages=enabled \
		-Dwlroots:werror=false -Dscenefx:werror=false -Dwayland:werror=false \
		-Dwayland-protocols:werror=false -Dlibdrm:werror=false \
		-Dlibxkbcommon:werror=false
	meson compile -C build

	echo "swayfs: installing to /usr/local ($SUDO meson install)..."
	$SUDO meson install -C build
	# Register the multiarch lib dir: meson installs subproject libraries to
	# /usr/local/lib/<triplet>, which ldconfig does not index by default.
	if [ -d /usr/local/lib/x86_64-linux-gnu ]; then
		echo "/usr/local/lib/x86_64-linux-gnu" | $SUDO tee /etc/ld.so.conf.d/swayfs-local.conf >/dev/null
	fi
	$SUDO ldconfig || true

	echo "swayfs: installing runtime dependencies..."
	$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		libgl1 libegl1 libgles2 libgbm1 mesa-vulkan-drivers \
		libgl1-mesa-dri fonts-dejavu-core

	if ! command -v sway >/dev/null 2>&1 && [ -x /usr/local/bin/sway ]; then
		echo "swayfs: note: /usr/local/bin is not on your PATH in this shell." >&2
	fi
}

main() {
	distro="$(detect_distro)"
	echo "swayfs: detected distribution: $distro"
	case "$distro" in
		arch) install_arch ;;
		debian) install_debian ;;
		*)
			echo "swayfs: unsupported distribution (need Arch or Debian/Ubuntu)." >&2
			exit 1
			;;
	esac
	echo "swayfs: done. Log out and pick 'sway' at your display manager, or run 'sway' from a TTY."
}

main
