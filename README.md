# Linux fan control for Colorful P15 TA24

This utility requests **automatic cooling** or **maximum cooling** using the same firmware messages as the Windows CC30 fan app. It targets COLORFUL P15 TA 24 with BIOS `1.07.05COLO1`. The driver refuses other laptop/BIOS identities.

Hardware operation has been confirmed by the laptop owner on kernel `7.2.9-zen1-1-zen`. The module builds cleanly, and the CLI lifecycle tests pass.

## Quick start

```sh
./cc30-fan status
make
sudo insmod ./cc30_fan.ko
./cc30-fan status
sudo ./cc30-fan max --seconds 30
./cc30-fan status
sudo ./cc30-fan auto
sudo rmmod cc30_fan
```

Loading the module only checks laptop identity, ACPI device/path, and existence of `_DSM`, and exposes attributes. It sends no fan command until a write. The timed `max` command requests automatic cooling on completion, Ctrl-C, SIGTERM, or SIGHUP. You can observe RPM in another terminal with `watch -n 1 ./cc30-fan status`. A fan already running at maximum may show little RPM difference.

`--seconds` defaults to 30 and accepts 1–3600. While running, the CLI renews the kernel's 60-second lease every 20 seconds. If the process crashes or is killed, the kernel attempts automatic mode when the last lease expires. Stop a running CLI with Ctrl-C before issuing a separate `auto` command; the CLI uses an advisory lock to prevent conflicting sessions.

```sh
./cc30-fan --dry-run max --seconds 30
```

This describes the message and performs no hardware writes. `status` reads existing sensors and driver attributes without firmware control calls.

## Recovery and boundaries

The module also requests auto on suspend, shutdown, removal, and lease expiry if it has previously issued a command. Failed automatic evaluations retry every five seconds while the module is running; a failed restore blocks suspend. Restoration errors during removal or shutdown are logged. Driver `mode` is the **last requested mode**, initially `unknown`; `last_error` reports the result of the ACPI evaluation. Use `status` to view fan RPM.

Only `auto` and `max` are exposed. This version does not provide arbitrary fixed RPM/percentages, silent mode, firmware curve changes, EC register access, or persistent installation. Existing firmware thermal management supplies automatic mode.

The module binds the existing `CLV0001:00` platform device at `\_SB_.DCHU`. Its attributes are:

- `/sys/bus/platform/devices/CLV0001:00/mode`: readable by everyone; writable by root; accepts exactly `auto` or `max`.
- `lease_seconds_remaining`: time before automatic restoration is due.
- `last_error`: zero after a returned ACPI object, or negative errno after evaluation failure.

If the attributes do not appear, the driver did not bind. Check `dmesg`, laptop/BIOS identity, and `readlink /sys/bus/platform/devices/CLV0001:00/driver`. Do not unbind another driver to force this one. Rebuild after a kernel update using the instructions below. A host policy that rejects unsigned modules must be handled through the host's normal module signing process.

## Build

```sh
make clean
make W=1
modinfo -F vermagic ./cc30_fan.ko
sudo insmod ./cc30_fan.ko
```

By default, the Makefile uses `/lib/modules/$(uname -r)/build`. To build for a different installed kernel, set `KDIR` explicitly. Replace the example release with the exact target release listed in `/lib/modules`:

```sh
CC30_KERNEL_RELEASE=7.2.9-zen1-1-zen
CC30_KERNEL_BUILD=/lib/modules/$CC30_KERNEL_RELEASE/build
make KDIR="$CC30_KERNEL_BUILD" clean
make KDIR="$CC30_KERNEL_BUILD" W=1
modinfo -F vermagic ./cc30_fan.ko
```

You can also point `KDIR` at a configured, prepared kernel build directory containing the target kernel's generated headers and `Module.symvers`. A plain source tree alone is insufficient.

The resulting `cc30_fan.ko` is specific to the target kernel. Boot that kernel before loading it, and check that `uname -r` matches the release in `modinfo -F vermagic`. If an older build is already loaded, stop any running `cc30-fan` command and unload it with `sudo rmmod cc30_fan` before loading the rebuilt module.

Selecting another kernel does not change laptop compatibility: the same model, BIOS, and ACPI checks still apply. Other kernel releases may require source adjustments if their kernel APIs differ.

## Validation and evidence

```sh
make W=1
python3 -m unittest discover -s . -p 'test_*.py'
```

The tests cover timeout/renewal, interruption, ambiguous write failure, and restoration failure without touching hardware. Hardware operation was subsequently confirmed by the laptop owner.

See [the reverse engineering report](./FAN_CONTROL.md) for byte-level messages, source addresses, hashes, and limitations. The driver source is GPL-2.0-only. Neither the Windows executable nor its drivers are executed by this Linux utility.

## Credits

Created by [rea](https://github.com/morluto/rea) on Codex (GPT-6.1-Sol)

Based on https://www.colorful.cn/home/DownLoadInfo?mid=158&id=33
