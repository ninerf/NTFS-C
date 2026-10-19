# NTFS-C

Read **and write** Windows (NTFS) drives on any Apple Silicon Mac.
a very lightweight C program.

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/ninerf/NTFS-C/main/install.sh | bash
```

Needs [Homebrew](https://brew.sh). The installer adds macFUSE and ntfs-3g, then tells
you how to allow macFUSE once (a Mac security setting).

## Use

```sh
ntfs                  # list NTFS drives
ntfs mount            # make them writable
ntfs unmount disk4s1  # before unplugging
ntfs watch            # make drives writable as soon as they're plugged in
ntfs doctor           # check setup
```

Drive won't mount? Windows probably used Fast Startup. Shut Windows down fully
(hold Shift while clicking Shut Down), or run `ntfs mount -f`.

## Uninstall

```sh
sudo rm /usr/local/bin/ntfs
```

## How it works

macOS mounts NTFS read-only. `ntfs` unmounts it and remounts it read/write with
[ntfs-3g](https://github.com/tuxera/ntfs-3g) over [macFUSE](https://macfuse.github.io).
Build from source with `make && sudo make install`.
