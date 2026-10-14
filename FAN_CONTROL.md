# CC30 fan-control findings

The fan controller in `07_CC30` is Clevo's Windows fan application, packaged inside ControlCenter 3.0 Package v6.025. The recovered FanSpeedSetting package is v6.18.0.0. On this laptop the relevant ACPI device is `CLV0001:00`, path `\_SB_.DCHU`. A focused Linux driver and CLI are in [linux-fan](../linux-fan/README.md).

## Direct binary observations

The InstallShield package was decoded offline with `extract_cc30.py`. It produced 211 valid files, each checked against its recorded expanded size and MD5. `inventory.json` retains cabinet offsets and SHA-256 identities. No Windows code was executed.

`FanSpeedSetting.exe` is managed .NET code. `monodis` recovered CIL into `FanSpeedSetting.il`. Missing Windows WPF dependencies cause incomplete UI method signatures in that listing, but the relevant CIL bodies and operand values are present.

| Operation | Exact evidence | Message |
|---|---|---|
| Set mode | `FAN.SetFanMode`, RVA `0x4674`, IL `0006..000a` | `SetWMI(0x79, 1, mode)` |
| Pack message | `InsydeDriver.SetWMI(int,int,uint)`, RVA `0x4b68` | Convert value to 4 LE bytes, overwrite byte 3 with subcommand, call `SetDCHU_Data(command, bytes, 4)` |
| Auto selection | UI body RVA `0x76bc`, IL `0075..007b` | mode `0` |
| Max selection | UI body RVA `0x7778`, IL `0025..002c` | mode `1` |
| Silent selection | UI body and `oem.ini` mode comment | mode `3`; recovered but not implemented |
| MaxQ selection | UI maxq selection body; `oem.ini` | mode `5`; recovered but not implemented |
| Custom selection | UI body RVA `0x7860`, IL `0025..002b` | mode `6`; requires fan table data, not implemented |
| Fan offset | `FAN.SetFanOffset`, RVA `0x473c` | `SetWMI(0x79, 0x0e, round(255 * offset / 100))`; not implemented |
| Custom fan table | CIL ending IL `02d0..02d8` in table writer | 256-byte message via command `0x0e`; not implemented |

The app's own `InsydeDCHU.dll` exports `SetDCHU_Data` at RVA `0x3d765e`; its jump resolves to VA `0x18041a550`. That function initializes a 256-byte zero buffer, copies the supplied four bytes, writes `_DSM` into its driver request, sets revision zero and function from the command parameter, and issues `DeviceIoControl` with IOCTL `0x322400`. The GUID literal at VA `0x180bb0dc0`, file offset `0x7e3dc0`, is `93f224e4-fbdc-4bbf-add6-db71bdc0afad`.

`SetDCHU_Data.disassembly.txt` preserves the actual app DLL function. The older service DLL in extracted item 173 independently uses the same GUID and construction at VA `0x180002920`. The recovered service CIL maps command decimal 121 (`0x79`) to `SystemControlFunction`.

`AcpiBridge.sys` routes IOCTL `0x322400` at VA `0x1400078f1` to `0x140006560`, then calls `0x140006000` to construct the ACPI evaluation. `AcpiBridge-DSM.disassembly.txt` shows four ACPI arguments, GUID buffer 16, integer revision, integer function, and a package (type 4) containing the buffer argument. This corroborates Linux's package-of-buffer representation.

The exact Linux equivalents implemented are:

```text
Handle:   \_SB_.DCHU
Method:   _DSM
Arg0:     UUID buffer for 93f224e4-fbdc-4bbf-add6-db71bdc0afad
Arg1:     Integer 0
Arg2:     Integer 0x79
Arg3:     Package { Buffer[256] }
Auto:     buffer starts 00 00 00 01; remaining 252 bytes zero
Maximum:  buffer starts 01 00 00 01; remaining 252 bytes zero
```

This is an ACPI firmware command interface. The Windows path is fan UI → Insyde DLL → AcpiBridge kernel driver → firmware `_DSM` → platform fan management. Clevo also provides a WMI route with corresponding commands. The Linux implementation follows the observed app's `_DSM` route directly. How this BIOS dispatches those messages into EC operations is unknown; no raw EC writes are needed by the supplied driver.

## Linux observations and limits

DMI reads: COLORFUL / P15 TA 24 / BIOS1.07.05COLO1. The built-in `acpi-fan` driver exposes CPU/GPU RPM plus a zero RPM GPU2 entry and a possible GPU alias. See `linux-fan-interfaces.json` for the full snapshot. Initially CPU/GPU fans read roughly 5450/5320 RPM. Those samples do not prove whether the laptop was in automatic or maximum mode.

Linux cooling devices TFN1–TFN4 support fine-grained `_FSL` levels. `cur_state=0` coexists with running fans, so its meaning cannot establish automatic restoration. The agent did not write those controls. The actual AML tables are root-only and could not be inspected in this session; sudo is blocked by the session's no-new-privileges restriction. No hardware control request or module load was performed.

Installed Tuxedo driver source corroborates the same `_DSM` UUID and package arguments. Its broad driver initialization covers unrelated laptop controls and has a compatibility gate that may reject this Colorful model. The new module only exposes the recovered auto/max operations, checks this exact DMI/BIOS/path, and has no automatic loading alias.

Primary source references for the corroborating driver/API layouts:

- [Tuxedo Clevo ACPI driver](https://github.com/tuxedocomputers/tuxedo-drivers/blob/main/clevo_acpi.c)
- [Tuxedo Clevo command definitions](https://github.com/tuxedocomputers/tuxedo-drivers/blob/main/clevo_interfaces.h)
- [Linux ACPI fan driver](https://github.com/torvalds/linux/blob/master/drivers/acpi/fan_core.c)
- [Unshield InstallShield format definitions](https://github.com/twogood/unshield/blob/main/lib/cabfile.h)

## Identity and evidence ledger

REA is not connected as MCP in this session. An existing cached REA 6.5.0 CLI supplied managed identity/member Evidence without registration changes. Native instructions were inspected using ordinary `objdump`; they are not presented as REA-native decompilation Evidence. `doctor` reported absent registration and unavailable native provider prerequisites, so no setup changes were made.

| File | SHA-256 / Evidence |
|---|---|
| App FanSpeedSetting.exe | `d3652b1a32e050cb02a2ae66eb9de6d7b06d4663178b08de935539ad100b0338` |
| App InsydeDCHU.dll | `cb3b03f38d4f3b98709a2e395acec0e958841e67627855874268e418b990f106` |
| Managed app identity | `rea-fan-app.json`: `ev_64ee8076b0ba7988f7f90a1851ce427b223d4dcf1df35f0f7d7ef5d4094eda6e` |
| Fan/driver CIL member anchors | `rea-fan-members.json`: `ev_468286b904a0e8f9d1f83292b650ffe512b1ee80e8e1f283fdeab7ffc111f1da` |
| Service identity | `rea-service.json`: `ev_9b54be3e76fae246cb5f6fce4697742b796bade263de7d7658cc5d78ad8f3bc8` |

The fan-command construction is answered by static evidence; Linux compatibility and live telemetry are observed read-only; the supplied utility is built and tested offline. Physical fan response, firmware command return semantics, actual EC implementation, and behavior of arbitrary percentages/custom curves remain unverified. A returned ACPI object establishes evaluation completion only.
