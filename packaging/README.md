# Debian packaging

`build-deb.sh` stages the tree and calls `dpkg-deb --build`; there is no
debhelper, no `debian/` directory and no build-time network access, so the same
script runs on a Debian host, an Ubuntu runner or a minimal container.

```sh
./packaging/build-deb.sh                            # native arch, helpers prebuilt
./packaging/build-deb.sh --arch all --no-binaries   # source-only, installable anywhere
./packaging/build-deb.sh --version 0.3.1 --out dist/
make deb
```

## Layout

| path | contents |
|---|---|
| `/usr/bin/qandroid` | symlink to `../lib/qandroid/qandroid` |
| `/usr/lib/qandroid/` | CLI, `tools/`, `scripts/`, `patches/`, `frontend/`, `shim/`, `hwc/`, docs |
| `/usr/share/man/man1/qandroid.1.gz` | man page |
| `/usr/share/doc/qandroid/` | README, `docs/*.md`, changelog, copyright |

The CLI resolves its own location with `realpath`, so running it through the
symlink still finds `tools/`, `scripts/` and the HWC headers.

## What is prebuilt and what is not

* `binder-mk`, `binder-ver` — built for the host if a compiler is present.
* `frontend/hwc_client` — built when `libsdl2-dev` is available; otherwise the
  CLI compiles it on first `qandroid display` if SDL2 headers exist, and says
  exactly what to install if not.
* `hwc/hwcomposer.qandroid.so` is **never** shipped: it is aarch64 guest code
  built inside an AOSP tree (see `docs/HWC.md`), which is not a packaging-time
  artifact.

## Dependencies

`Depends` keeps to what a first run needs: `python3`, `qemu-user-static`,
`e2fsprogs` (debugfs does the unprivileged ext4 extraction). `Recommends` adds
what specific paths want: SDL2 for the display client, `curl` for resumable
image downloads, `android-sdk-libsparse-utils` for sparse images.

`binfmt-support` is only *recommended*, not required: on Ubuntu the binfmt
registration comes with `qemu-user-static`, and on a host where you build a
patched qemu you will register your own entry anyway (see `docs/BRINGUP.md`).

## postinst

Intentionally inert. It prints the two commands worth running and exits. Editing
`/proc/sys/fs/binfmt_misc` from a maintainer script is how you break somebody's
machine, and this project does not need to.
