# USB gadget

For tethered boot: the system runs from storage on the USB host (the PC
the device is plugged into), served over USB by a program there. First
stage makes the USB gadget with that function, and the rest of the boot
waits for the disk to appear like for any other.

Files: `usb_gadget.cpp`, and its callers in `devices.cpp`,
`dynamic_mount_handler.cpp` and `init.cpp`.

**TL;DR:** `androidboot.usb_gadget=blk` makes first stage create a
gadget with the `blk` function and bind it to the first USB device
controller (UDC) that appears. The disk the host attaches then shows up as
`/dev/usbblkN`, and is probed as any block device.

## Parameter

| Parameter | Default | Values |
|-----------|---------|--------|
| `androidboot.usb_gadget` | none | `blk` |

Unset: nothing is done. An unknown function is logged and ignored. The
kernel needs the function (`CONFIG_USB_CONFIGFS_F_BLK` for `blk`, see
`Documentation/usb/gadget-blk.rst` of the kernel) and USB gadget support
with configfs, and the UDC driver must be built in or in the ramdisk.

## What happens

1. `UsbGadget::Init()` reads the parameter, from `OnPreBlockDevices()`.
   Only first stage does: the ueventd that init starts in second stage
   never does anything here.
2. A `udc` uevent (add) reaches `UsbGadget::OnUdcAdd()`. It comes before
   the device node test in `DeviceHandler::HandleUevent()`, as a UDC has
   none.
3. The first time, configfs is mounted on `/config` and the gadget is
   made in `/config/usb_gadget/generic_init`: IDs of the Linux Foundation
   multifunction gadget, strings (a random serial number), the
   configuration `c.1` and the function `<function>.0` linked in it. For
   `blk`, Microsoft OS descriptors are set too, so that Windows hosts bind
   WinUSB without a driver package. The name is not `g1`, which Android's
   USB HAL uses.
4. The name of the UDC is written to `UDC`, unless the gadget is bound.
   Another `udc` uevent, for example when a role switch brings the
   controller back, does not write again: the kernel binds the gadget to
   a controller of that name by itself.
5. The host attaches a disk, `usbblkN` appears as a block device, and
   `dynamic_mount_handler` goes on as with any other disk. Nothing in
   `CanQuitUeventd()` is specific to it: the boot waits for what the
   `androidboot.mount_*` parameters need.

## Second stage

Nothing else touches the gadget configuration. Second stage init sees
`ro.boot.usb_gadget` and leaves `SetUsbController()` without setting
`sys.usb.controller`, so:

- `hardware_detect` selects the `none` USB gadget APEX, and no USB HAL runs.
- `sys.usb.configfs` stays `0` (it was forced to `1` in recovery and
  charger modes only through that function), so the actions of init USB
  scripts are not triggered.

The disk of the gadget survives the cable being unplugged and the host
program restarted, as long as the host attaches the same disk again within
the timeout of the function (30 seconds by default). Requests are held
back meanwhile, and probing a disk in first stage waits as well.

## Limitations

- One function, `blk`. Others need a row in `kFunctions` of
  `usb_gadget.cpp`.
- No ADB over that port while tethered: the gadget has just the one
  function.
- The first UDC wins. A second one is left alone.
