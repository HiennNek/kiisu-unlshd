# Kiisu-UNLSHD
### Unleashed firmware fork for Kiisu v4b/v4br - with U2F and rolling code support

> **Note:** Kiisu-UNLSHD is a community fork of [Unleashed FW](https://github.com/DarkFlippers/unleashed-firmware) for Kiisu.

## Why choose this over other forks? (Actually, is there any other Unleashed fork for Kiisu other than this one?)

- **Up-to-date** - Actively maintained and always in sync with upstream Unleashed, plus the latest tweaks and apps from upstream Kiisu FW.

- **Rolling code & U2F support** - Includes rolling code and U2F support for Kiisu. The firmware covers most rolling code manufacturers from upstream Unleashed, but not all. If you find a missing manufacturer key, please [contribute here](https://github.com/HiennNek/non-flipper-rolling-code-support#-missing-keys).

- **Kiisu assets** - Replaces all Flipper assets with Kiisu branding. Also, the original Kiisu assets had minor visual issues (noise, low-res images); this firmware ships with fixed assets redrawn by hand. Found something missing? [Open an issue](https://github.com/HiennNek/kiisu-unlshd/issues).

## How to install

1. Download **`kiisu-unlshd_XXXXXXXX_dd-MM-yyyy.tgz`** from [Releases](https://github.com/HiennNek/kiisu-unlshd/releases)
2. Open **qFlipper** or visit [lab.flipper.net](https://lab.flipper.net/)
3. Select **Install from file** and choose the downloaded file

## How to build

Clone the repository:

```bash
git clone --recursive --jobs 8 https://github.com/HiennNek/kiisu-unlshd.git
cd kiisu-unlshd/
```

Flash directly to Kiisu (device must be connected via USB with qFlipper closed):

```bash
./fbt COMPACT=1 DEBUG=0 flash_usb_full
```

Compile a TGZ package:

```bash
./fbt COMPACT=1 DEBUG=0 updater_package
```

Build and launch a single app:

```bash
./fbt COMPACT=1 DEBUG=0 launch APPSRC=your_appid
```

## FAQ

> Don't see your question answered here? [Open an issue](https://github.com/HiennNek/kiisu-unlshd/issues).

### Common issues
<details>
<summary><b>Screen glitching after installation</b></summary>
Just restart your Kiisu.
</details>
<details>
<summary><b>Things broke after switching from other firmware (e.g. qFlipper/Flipper Lab shows it as incompatible, etc...)</b></summary>

Delete all files from your SD card (you can still keep captured files/downloaded DB files), then reinstall the firmware.

If you can't flash the firmware using the normal way, try flashing it in DFU mode. (Turn off Kiisu, then hold the `OK` button and plug your Kiisu into your PC. qFlipper and lab.flipper.net will recognize it as DFU mode.)

You can find the DFU file in the latest [releases](https://github.com/HiennNek/kiisu-unlshd/releases).
</details>
<details>
<summary><b>Why doesn't U2F work?</b></summary>
Try deleting all files in <code>SD Card/u2f/</code>. It will regenerate all cert files.
If it still doesn't work, then you're using an older version of Kiisu that doesn't ship with Kiisu's secret keys.
</details>
<details>
<summary><b>Why can't Sub-GHz copy my remote?</b></summary>
The upstream firmware might not support your remote.
If you believe your remote is supported by upstream, check <a href="https://github.com/HiennNek/non-flipper-rolling-code-support#-missing-keys">non-flipper-rolling-code-support</a>.
Or <a href="https://github.com/HiennNek/kiisu-unlshd/issues">open an issue</a>.
</details>
<details>
<summary><b>Windows Defender flags the firmware as a virus</b></summary>

~~Use Linux~~

Windows Defender uses AI to scan and detect malware, which might create a false positive.

Try using VirusTotal if you don't trust it.
</details>

### General

<details>
<summary><b>What is Kiisu-UNLSHD?</b></summary>

Kiisu-UNLSHD is a community-maintained fork of [Unleashed Firmware](https://github.com/DarkFlippers/unleashed-firmware) built specifically for the Kiisu device. It adds rolling code and U2F support that aren't available in official Unleashed firmware on Kiisu hardware, ships hand-redrawn Kiisu-branded assets, and stays continuously synced with upstream Unleashed plus the latest Kiisu-specific tweaks and apps.

</details>

<details>
<summary><b>Is this an official Unleashed or Kiisu firmware?</b></summary>

No. Kiisu-UNLSHD is **not** an official Unleashed branch, and it isn't the stock Kiisu firmware either - it's a separate, independently maintained community fork.

</details>

### Installation & Updates

<details>
<summary><b>How do I install Kiisu-UNLSHD?</b></summary>

See above

</details>

<details>
<summary><b>Can I switch back to stock firmware or another fork later?</b></summary>

Yes - switching is just a matter of flashing a different firmware file the same way, via qFlipper or lab.flipper.net.

</details>

<details>
<summary><b>How often is it updated?</b></summary>

Weekly. Check the [Releases](https://github.com/HiennNek/kiisu-unlshd/releases) page for the latest build.

</details>

### Features

<details>
<summary><b>What is rolling code support, and why does it matter for Kiisu?</b></summary>

Rolling code is the security scheme used by many garage door openers, gate remotes, and car key fobs, where each transmission uses a new code instead of a static one. Official Flipper firmware needs factory-provisioned keys to handle certain rolling-code protocols - and since Kiisu hardware isn't produced by Flipper Devices Inc., it doesn't ship with those keys. Kiisu-UNLSHD implements its own support so these protocols work on Kiisu anyway.

</details>

<details>
<summary><b>Which rolling code manufacturers are supported?</b></summary>

Most of the manufacturers covered by upstream Unleashed Firmware, though not all of them.

</details>

<details>
<summary><b>What if my remote/manufacturer isn't supported?</b></summary>

You can contribute the missing manufacturer key at the companion repo: [non-flipper-rolling-code-support](https://github.com/HiennNek/non-flipper-rolling-code-support#-missing-keys).

</details>

<details>
<summary><b>What is U2F, and why didn't it work before?</b></summary>

U2F (Universal 2nd Factor) is a hardware authentication standard used for two-factor login. Like rolling code, U2F functionality needs certification that other firmware doesn't ship with - Kiisu-UNLSHD adds its own support (cert generation), so U2F works on Kiisu.

</details>

<details>
<summary><b>What's different about the Kiisu-themed assets in this fork?</b></summary>

All visuals are Kiisu-branded rather than Flipper-branded, and the original Kiisu asset set - which had some noise and low-resolution artifacts - has been redrawn by hand to fix those issues.

</details>

### Kiisu-UNLSHD vs. Other Firmware

<details>
<summary><b>How is this different from stock Kiisu firmware?</b></summary>

Kiisu-UNLSHD brings Unleashed's broader feature set on top of Kiisu, stays actively synced with upstream, and includes the hand-corrected assets - none of which are part of stock Kiisu firmware.

</details>




