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

Part 1: in Recovery

Save your work and shut down the Mac (Apple menu → Shut Down).
Press and hold the power button (Touch ID button) until you see “Loading startup options”, then let go.
Click Options, then Continue. Sign in with your Mac password if asked.
In the menu bar at the top, click Utilities → Startup Security Utility.
Select your disk (usually “Macintosh HD”) and click Security Policy…
Choose Reduced Security and tick “Allow user management of kernel extensions from identified developers”. Click OK and enter your password.
Apple menu → Restart.

Part 2: back in macOS

Plug in your NTFS drive.
Open Terminal and run ntfs mount.
A “System Extension Blocked” message will pop up. Open System Settings → Privacy & Security, scroll down, and click Allow next to the macFUSE message (“Benjamin Fleischer”).
Restart once more.

Part 3: use it

Plug in the drive and run ntfs mount.
Run ntfs to check that the drive shows read-write. You can now copy to it in Finder.
Before unplugging, eject it in Finder or run ntfs unmount <name>.

## Uninstall

```sh
sudo rm /usr/local/bin/ntfs
```

## How it works

macOS mounts NTFS read-only. `ntfs` unmounts it and remounts it read/write with
[ntfs-3g](https://github.com/tuxera/ntfs-3g) over [macFUSE](https://macfuse.github.io).
Build from source with `make && sudo make install`.
