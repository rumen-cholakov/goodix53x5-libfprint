# Arch packaging runbook

How the Goodix 53x5 driver is installed on this machine, and what to do when
something needs rebuilding. Written for whoever (or whatever) does the next
rebuild — follow it top to bottom rather than improvising.

## What is installed

`packaging/arch/PKGBUILD` builds `libfprint-goodix53x5`: the stock Arch
libfprint plus this repo's driver, as a real pacman package. It declares
`conflicts=(libfprint)` and `provides=(libfprint libfprint-2 libfprint-2.so)`,
so it *replaces* the distro package and still satisfies fprintd's
`libfprint-2.so=2-64` dependency (makepkg fills in the soname version).

Do **not** install by running `sudo ninja install` into `/usr` as the README's
generic instructions suggest. That overwrites pacman-owned files, and the next
`pacman -Syu` silently reverts the driver.

Because the package conflicts with `libfprint`, `pacman -Syu` will *not* pull
in new upstream libfprint releases. Rebuilding is a deliberate act — that is
the point of this document.

First installed 2026-08-23: libfprint 1.94.100 + driver at commit `309d4c6`.

## Case 1: the driver changed, libfprint did not

```bash
cd packaging/arch
makepkg -f
sudo pacman -U libfprint-goodix53x5-*.pkg.tar.zst
systemctl restart fprintd     # it is dbus-activated; a stop is enough
fprintd-verify
```

The PKGBUILD copies the driver straight out of the working tree, so
uncommitted changes are built. Bump `pkgrel` if you want pacman to see it as a
distinct version.

If the change touched image preprocessing or the SIGFM feature format, existing
enrolled prints may stop matching. Re-enroll:

```bash
fprintd-delete "$USER" && fprintd-enroll
```

## Case 2: Arch ships a new libfprint

Check what the repos have (our package hides it from `-Syu`):

```bash
pacman -Si libfprint | grep -E '^(Version|Depends)'
```

### 1. Test the integration patch against the new tag

The patch is version-specific and named after the libfprint release:
`meson-integration-<version>.patch` at the repo root. (`meson-integration.patch`
without a version is upstream's, targeting v1.94.10.)

```bash
NEW=1.94.101   # the version you are moving to
git clone https://gitlab.freedesktop.org/libfprint/libfprint.git /tmp/lf
git -C /tmp/lf checkout "v$NEW"
git -C /tmp/lf apply --check /path/to/repo/meson-integration-1.94.100.patch
```

If it applies, skip to step 3 and just rename the file.

### 2. Rebase the patch

Apply what applies, fix the rest by hand, then regenerate. The parts that have
drifted before, and are most likely to drift again:

- **Driver registration.** Up to v1.94.10 the root `meson.build` had a flat
  `default_drivers` list plus a `driver_helper_mapping` dict. 1.94.100 replaced
  both with a single `drivers_info` dict — our entry is now
  `'goodix53x5': { 'helper': ['openssl'] },`.
- **Driver sources.** `libfprint/meson.build`'s `driver_sources` switched from
  bare string lists to `files(...)` in 1.94.100.
- **The SIGFM block** (OpenCV detection + `libsigfm` static library) is
  anchored after `libnbis`, and is then wired into `libfprint_drivers` and the
  main `libfprint` shared library via `link_with`.
- **hwdb/allowlist.** `data/autosuspend.hwdb` and
  `libfprint/fprint-list-udev-hwdb.c` list `5335/5385/5395` as *unsupported*
  upstream; the patch moves them into the supported/autosuspend section.

Working method, which is how the 1.94.100 patch was produced:

```bash
cd /tmp/lf
cp -r /path/to/repo/drivers/goodix53x5 libfprint/drivers/
cp -r /path/to/repo/sigfm libfprint/
patch -p1 --forward < /path/to/repo/meson-integration-1.94.100.patch || true
find . -name '*.rej' -delete
$EDITOR meson.build libfprint/meson.build      # hand-apply the failed hunks
# build until clean, then:
git diff > /path/to/repo/meson-integration-$NEW.patch
```

The driver's C/C++ sources have so far needed **no** changes across releases —
if they suddenly do, that is an upstream API break worth reading about before
patching around it.

### 3. Update the PKGBUILD

- Set `pkgver` to the new version, reset `pkgrel=1`.
- Diff our build options against the official Arch package and re-sync:
  `https://gitlab.archlinux.org/archlinux/packaging/packages/libfprint`
  (currently: `-D drivers=all -D installed-tests=false`, via `arch-meson`).
- `_integration_patch` resolves from `pkgver` automatically; no edit needed if
  the patch file is named correctly.

### 4. Build and verify before installing

```bash
cd packaging/arch && makepkg -f
```

Then check the package rather than trusting the exit code:

```bash
PKG=libfprint-goodix53x5-*.pkg.tar.zst

# soname provide must match fprintd's dependency exactly
bsdtar -xOf $PKG .PKGINFO | grep -E 'provides|conflict'

# the driver actually made it into the library
bsdtar -xOf $PKG usr/lib/libfprint-2.so.2.0.0 | strings | grep FpiDeviceGoodix53x5

# our three PIDs are in the autosuspend section (expect 3)
bsdtar -xOf $PKG usr/lib/udev/hwdb.d/60-autosuspend-libfprint-2.hwdb |
  grep -cE '27C6p(5335|5385|5395)'

# file set should match the distro package, so nothing else on the system
# breaks. Needs the files database: sudo pacman -Fy (once).
# Do NOT use `pacman -Ql libfprint` here: once our package is installed that
# name resolves through `provides` to our own package, and the diff is empty
# no matter what is in it.
diff <(bsdtar -tf $PKG | grep -v '^\.' | sed 's|/$||' | sort) \
     <(pacman -Fl libfprint | awk '{print $2}' | sed 's|/$||' | sort)
```

Then install and smoke-test:

```bash
sudo pacman -U $PKG
fprintd-list "$USER"     # must show "Goodix HTK32 Fingerprint Sensor"
fprintd-verify
```

## Known landmines

- **`glib-mkenums` not found.** Arch split it out of `glib2`; install
  `glib2-devel`. Meson reports it as a "distributor issue", which it is not.
- **g-ir-scanner drags in all of OpenCV.** If the shared library's
  `dependencies:` includes anything derived from OpenCV's pkg-config file, the
  introspection scanner links `pkg-config --libs opencv5` — all 58 modules,
  including `opencv_viz` and `opencv_hdf`, which need VTK and HDF5 and fail
  with hundreds of `undefined reference to vtk*` errors. Both integration
  patches avoid this by giving the shared library a link-only
  `opencv_link_dep` (just core/features/flann/imgproc) and keeping the
  pkg-config include dependency on `libsigfm`, which is the only thing that
  compiles OpenCV headers. Do not "simplify" that split; the alternative is
  disabling introspection and dropping the `FPrint-2.0` typelib, which is what
  the AUR package does.
- **`Package contains reference to $srcdir`** on `libfprint-2.so.2.0.0` is
  expected. It is an installed-tests path baked in upstream; the official Arch
  package has the same string with their build directory. There is no RPATH.
- **The AUR package is not this package.** `libfprint-goodix53x5` in the AUR
  builds libfprint v1.94.10 and pins the driver to an older commit. Installing
  it downgrades libfprint and loses whatever this checkout has that the pinned
  commit does not.
