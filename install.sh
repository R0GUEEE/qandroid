#!/bin/sh
# qandroid installer - installs the CLI and everything it needs to run.
#
#   curl -fsSL https://raw.githubusercontent.com/R0GUEEE/qandroid/main/install.sh | sh
#   sh install.sh --version 0.3.0
#   sh install.sh --file ./qandroid_0.3.0_amd64.deb
#   sh install.sh --source --prefix /opt/qandroid     # any distro
#   sh install.sh --uninstall
#
# Debian/Ubuntu get the release .deb; every other
# distribution gets the source install, which lays the tree down under
# --prefix, installs that distro's equivalent packages, and symlinks the CLI.
#
# Nothing here touches binfmt_misc, the guest image, or your home directory.
set -eu

REPO=R0GUEEE/qandroid
BASE=https://github.com/$REPO
VERSION=latest
PREFIX=/opt/qandroid
DEB_FILE=
MODE=install
DO_UPDATE=1
WITH_IMAGE=0
PURGE_DATA=0
DRY_RUN=0
ASSUME_YES=0
FORCE=0
SKIP_DEPS=0

# $0 is "sh" (or /dev/fd/N) when this arrives through a pipe, and then the
# script cannot re-exec itself under sudo - the user has to do that.
SELF=$0
[ -f "$SELF" ] || SELF=

TMP=
cleanup() { [ -n "$TMP" ] && rm -rf "$TMP" || true; }
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------- output ---
if [ -t 1 ] && [ "${NO_COLOR:-}" = "" ]; then
    B=$(printf '\033[1m'); R=$(printf '\033[31m'); G=$(printf '\033[32m'); N=$(printf '\033[0m')
else
    B=; R=; G=; N=
fi
say()  { printf '%s==>%s %s\n' "$B" "$N" "$*"; }
ok()   { printf '%s  ok%s %s\n' "$G" "$N" "$*"; }
warn() { printf '  !  %s\n' "$*" >&2; }
die()  { printf '%serror:%s %s\n' "$R" "$N" "$*" >&2; exit 1; }

usage() {
    cat <<'USAGE'
qandroid installer - installs the CLI and everything it needs to run.

  curl -fsSL https://raw.githubusercontent.com/R0GUEEE/qandroid/main/install.sh | sudo sh
  sudo sh install.sh --version 0.3.0
  sudo sh install.sh --file ./qandroid_0.3.0_amd64.deb
  sudo sh install.sh --source --prefix /opt/qandroid     # any distro
  sudo sh install.sh --uninstall [--purge-data]

Options:
  --version V     release to install (default: latest)
  --file PATH     install a local .deb instead of downloading
  --source        build from the source tarball (non-Debian hosts)
  --prefix DIR    source install location (default /opt/qandroid)
  --with-image    also run 'qandroid setup' (downloads ~1 GiB)
  --no-update     skip 'apt-get update'
  --no-deps       do not install distro packages
  --dry-run       print what would happen
  -y, --yes       do not prompt
  --force         use the source install on non-apt hosts

Debian/Ubuntu get the release .deb; every other distribution gets the source
install, which lays the tree down under --prefix, installs that distro's
equivalent packages, and symlinks the CLI.

Nothing here touches binfmt_misc, the guest image, or your home directory.
USAGE
    exit 0
}

elevate() {
    [ -n "$SELF" ] || die "running from a pipe, so this script cannot re-run itself.
  Either:  curl -fsSL .../install.sh | sudo sh
  or:      curl -fsSLO .../install.sh && sudo sh install.sh"
    say "elevating with sudo"
    exec sudo -- sh "$SELF" "$@"
}

# ----------------------------------------------------------------- args ----
while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION=$2; shift 2 ;;
        --file) DEB_FILE=$2; shift 2 ;;
        --prefix) PREFIX=$2; shift 2 ;;
        --source) MODE=source; shift ;;
        --uninstall) MODE=uninstall; shift ;;
        --purge-data) PURGE_DATA=1; shift ;;
        --with-image) WITH_IMAGE=1; shift ;;
        --no-update) DO_UPDATE=0; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -y|--yes) ASSUME_YES=1; shift ;;
        --force) FORCE=1; shift ;;
        --no-deps) SKIP_DEPS=1; shift ;;
        -h|--help) usage ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

# --------------------------------------------------------------- helpers ---
fetch() {   # fetch URL OUT
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 --connect-timeout 20 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -T 30 -O "$2" "$1"
    else
        die "need curl or wget to download; install one and retry"
    fi
}

latest_tag() {
    if command -v curl >/dev/null 2>&1; then
        # follow the /releases/latest redirect; avoids the API rate limit
        url=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$BASE/releases/latest" 2>/dev/null || true)
        case "$url" in
            */tag/*) printf '%s' "${url##*/}" ;;
        esac
    fi
}

detect_pm() {
    for pm in apt-get dnf yum pacman zypper apk; do
        if command -v "$pm" >/dev/null 2>&1; then echo "$pm"; return; fi
    done
    echo unknown
}

detect_arch() {
    case "$(uname -m)" in
        x86_64|amd64)  echo amd64 ;;
        aarch64|arm64) echo arm64 ;;
        armv7l|armv7)  echo armhf ;;
        i?86)          echo i386 ;;
        *)             uname -m ;;
    esac
}

need_root() {
    [ "$(id -u)" -eq 0 ] && return 0
    if command -v sudo >/dev/null 2>&1; then
        say "re-running with sudo"
        exec sudo -- sh "$0" "$@"
    fi
    die "root is required; re-run with sudo"
}

require() { command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"; }

# ------------------------------------------------------------ prereqs ------
install_pm_deps() {
    if [ "$SKIP_DEPS" = 1 ]; then
        say "skipping package installation (--no-deps)"
        return 0
    fi
    pm=$(detect_pm)
    case "$pm" in
        apt-get)
            [ "$DO_UPDATE" = 1 ] && { say "apt-get update"; apt-get update -qq; }
            deps="ca-certificates curl python3 qemu-user-static e2fsprogs"
            [ "$MODE" = source ] && deps="$deps build-essential"
            say "installing: $deps"
            # shellcheck disable=SC2086
            DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $deps
            ;;
        dnf|yum)
            say "installing: python3 qemu-user-static e2fsprogs curl"
            "$pm" install -y python3 qemu-user-static e2fsprogs curl
            ;;
        pacman)
            say "installing: python qemu-user-static e2fsprogs curl"
            pacman -Sy --needed --noconfirm python qemu-user-static e2fsprogs curl
            ;;
        zypper)
            say "installing: python3 qemu-linux-user e2fsprogs curl"
            zypper --non-interactive install python3 qemu-linux-user e2fsprogs curl
            ;;
        apk)
            say "installing: python3 qemu-aarch64 e2fsprogs curl"
            apk add --no-cache python3 e2fsprogs curl || true
            apk add --no-cache qemu-aarch64 || apk add --no-cache qemu-user || true
            ;;
        *)
            warn "unknown package manager; install these yourself:"
            warn "  python3, qemu-aarch64 user emulator, e2fsprogs, curl"
            ;;
    esac
}

# ------------------------------------------------------------ from .deb ----
install_deb() {
    arch=$(detect_arch)
    tag=$VERSION
    if [ "$VERSION" = latest ]; then
        tag=$(latest_tag)
        [ -n "$tag" ] || die "could not resolve the latest release tag; use --version"
    fi
    case "$tag" in v*) ver=${tag#v} ;; *) ver=$tag; tag=v$tag ;; esac

    TMP=$(mktemp -d)
    if [ -n "$DEB_FILE" ]; then
        [ -f "$DEB_FILE" ] || die "no such file: $DEB_FILE"
        deb=$DEB_FILE
        say "using local package: $deb"
    else
        name="qandroid_${ver}_${arch}.deb"
        url="$BASE/releases/download/$tag/$name"
        say "downloading $name ($tag)"
        [ "$DRY_RUN" = 1 ] && { echo "  would fetch $url"; return 0; }
        fetch "$url" "$TMP/$name" || die "download failed: $url
  (this release may not have an $arch build; try --source)"
        deb="$TMP/$name"
        if fetch "$BASE/releases/download/$tag/SHA256SUMS.txt" "$TMP/SHA256SUMS.txt" 2>/dev/null; then
            ( cd "$TMP" && grep " $name\$" SHA256SUMS.txt > sums \
              && sha256sum -c sums >/dev/null 2>&1 ) \
                && ok "checksum verified" || warn "checksum not verified (no matching line)"
        fi
    fi

    [ "$DRY_RUN" = 1 ] && { echo "  would apt-get install $deb"; return 0; }
    require apt-get
    install_pm_deps
    say "installing the package"
    # apt, not dpkg: it resolves Depends (qemu-user-static, e2fsprogs, libsdl2)
    DEBIAN_FRONTEND=noninteractive apt-get install -y "$deb" \
        || die "install failed; the package is at $deb"
    ok "installed $(dpkg-query -W -f='${Version}' qandroid 2>/dev/null || echo '')"
}

# --------------------------------------------------------- from source -----
install_source() {
    tag=$VERSION
    if [ "$VERSION" = latest ]; then
        tag=$(latest_tag)
        [ -n "$tag" ] || die "could not resolve the latest release tag; use --version"
    fi
    case "$tag" in v*) ;; *) tag=v$tag ;; esac
    ver=${tag#v}

    TMP=$(mktemp -d)
    url="$BASE/archive/refs/tags/$tag.tar.gz"
    say "downloading source $tag"
    if [ "$DRY_RUN" = 1 ]; then
        echo "  would fetch $url"
        echo "  would extract to $PREFIX and symlink /usr/local/bin/qandroid"
        return 0
    fi
    fetch "$url" "$TMP/src.tar.gz" || die "download failed: $url"

    install_pm_deps

    say "extracting to $PREFIX"
    rm -rf "$PREFIX.tmp" && mkdir -p "$PREFIX.tmp"
    tar -xzf "$TMP/src.tar.gz" -C "$PREFIX.tmp" --strip-components=1
    rm -rf "$PREFIX"
    mv "$PREFIX.tmp" "$PREFIX"
    chmod 0755 "$PREFIX/qandroid"
    for f in tools/apk.py tools/lpunpack.py patches/apply.sh frontend/viewer.py; do
        [ -e "$PREFIX/$f" ] && chmod 0755 "$PREFIX/$f"
    done
    for f in "$PREFIX"/scripts/*.sh; do [ -e "$f" ] && chmod 0755 "$f"; done

    if command -v cc >/dev/null 2>&1; then
        say "building host helpers"
        cc -O2 -w -o "$PREFIX/tools/binder-mk" "$PREFIX/tools/binder-mk.c" 2>/dev/null || true
        cc -O2 -w -o "$PREFIX/tools/binder-ver" "$PREFIX/tools/binder-ver.c" 2>/dev/null || true
    else
        warn "no C compiler; the CLI will build the helpers on first use"
    fi

    mkdir -p /usr/local/bin
    ln -sf "$PREFIX/qandroid" /usr/local/bin/qandroid
    ok "installed to $PREFIX (qandroid on PATH)"
}

# ------------------------------------------------------------ uninstall ----
do_uninstall() {
    removed=0
    # dpkg -s is the reliable test; parsing dpkg-query's Status field is brittle
    if command -v dpkg >/dev/null 2>&1 && dpkg -s qandroid >/dev/null 2>&1; then
        say "removing the Debian package"
        apt-get remove -y qandroid || dpkg -r qandroid || true
        removed=1
    fi
    if [ -d "$PREFIX" ]; then
        say "removing $PREFIX"
        rm -rf "$PREFIX"
        removed=1
    fi
    # leftovers from either install path, and from interrupted runs
    rm -f /usr/bin/qandroid /usr/local/bin/qandroid
    [ -d /usr/lib/qandroid ] && rm -rf /usr/lib/qandroid
    [ -d /usr/share/doc/qandroid ] && rm -rf /usr/share/doc/qandroid
    rm -f /usr/share/man/man1/qandroid.1.gz
    [ "$removed" = 1 ] || warn "qandroid did not look installed; cleaned up anyway"

    if [ "$PURGE_DATA" = 1 ]; then
        say "removing per-user images and rootfs data"
        for d in /root/.local/share/qandroid /home/*/.local/share/qandroid; do
            [ -d "$d" ] && rm -rf "$d"
        done
    else
        warn "kept image/rootfs data; remove ~/.local/share/qandroid or pass --purge-data"
    fi
    if command -v qandroid >/dev/null 2>&1; then
        warn "qandroid still resolves at $(command -v qandroid)"
        return 1
    fi
    ok "uninstalled"
}

# ----------------------------------------------------------------- main ----
say "qandroid installer"
printf '  platform: %s %s (%s)\n' "$(uname -s)" "$(uname -r)" "$(uname -m)"
printf '  release:  %s\n' "$VERSION"

if [ "$MODE" = uninstall ]; then
    if [ "$(id -u)" -ne 0 ]; then
        command -v sudo >/dev/null 2>&1 || die "uninstall needs root"
        set -- --uninstall
        [ "$PURGE_DATA" = 1 ] && set -- "$@" --purge-data
        elevate "$@"
    fi
    do_uninstall
    exit 0
fi

if [ "$DRY_RUN" = 0 ] && [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        # re-run the whole script under sudo, arguments intact
        set -- --version "$VERSION" --prefix "$PREFIX"
        [ -n "$DEB_FILE" ] && set -- "$@" --file "$DEB_FILE"
        [ "$MODE" = source ] && set -- "$@" --source
        [ "$DO_UPDATE" = 0 ] && set -- "$@" --no-update
        [ "$WITH_IMAGE" = 1 ] && set -- "$@" --with-image
        [ "$DRY_RUN" = 1 ] && set -- "$@" --dry-run
        [ "$FORCE" = 1 ] && set -- "$@" --force
        [ "$SKIP_DEPS" = 1 ] && set -- "$@" --no-deps
        elevate "$@"
    fi
    die "root is required; re-run with sudo"
fi

pm=$(detect_pm)
arch=$(detect_arch)
printf '  package manager: %s, arch: %s\n' "$pm" "$arch"

if [ "$MODE" = source ]; then
    install_source
elif [ "$pm" = apt-get ] || [ -n "$DEB_FILE" ]; then
    install_deb
else
    if [ "$FORCE" = 1 ]; then
        install_source
    else
        warn "$pm is not apt: this release publishes a Debian package only."
        printf '\n  Install from source instead (same CLI, distro packages):\n\n'
        printf '      sh install.sh --source --version %s\n\n' "$VERSION"
        printf '  Or force it now by adding --force.\n'
        [ "$DRY_RUN" = 1 ] && exit 0
        exit 1
    fi
fi

if [ "$DRY_RUN" = 1 ]; then
    say "dry run: nothing was changed"
    exit 0
fi

say "checking the install"
if command -v qandroid >/dev/null 2>&1; then
    qandroid --version
    qandroid doctor || true
else
    warn "qandroid is not on PATH yet; open a new shell and run: qandroid doctor"
fi

cat <<EOF

${B}Next steps${N}
  1. qandroid doctor        # confirm the host is ready (binderfs, qemu, binfmt)
  2. qandroid setup         # download an arm64 image and build a rootfs (~2 GiB)
  3. sudo qandroid boot     # start the guest
  4. qandroid display       # HWC2 window, if the composer module is installed

Full docs: /usr/share/doc/qandroid/ (deb) or $PREFIX/docs/ (source)
EOF

if [ "$WITH_IMAGE" = 1 ]; then
    if [ "$ASSUME_YES" = 1 ] || [ -t 0 ]; then
        say "fetching an image and building a rootfs (about 2 GiB)"
        qandroid setup || warn "setup did not finish; run 'qandroid setup' to retry"
    else
        warn "skipping --with-image (no terminal to confirm)"
    fi
fi
