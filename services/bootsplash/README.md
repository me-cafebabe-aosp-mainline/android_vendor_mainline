# Bootsplash

`bootsplash` is an optional, one-shot `/system_ext/bin/bootsplash` service for
the interval before the graphics HAL takes over. It does not implement a HAL
and does not restart after SurfaceFlinger starts. Enable it with
`TARGET_ENABLE_BOOTSPLASH := true` in the product configuration. It can be
installed alongside `fbkeyboard`; init requests that the keyboard stop before
starting the splash. This stop is asynchronous and does not lock the fbdev node;
the two can briefly overlap if the keyboard is still exiting or restarts.
Set this flag before including `device/mainline/common/mainline_common.mk`;
the optional product fragments are evaluated as that file is included.

The init RC starts it on `late-init` and stops it on `early-boot`, before the
platform's `boot` action starts the HAL class. It waits for the service to
reach `stopped` before continuing init actions so DRM master is released.
An uninterruptible kernel call can stall this wait; a display driver with this
problem needs a different product-specific startup strategy. There is no
seamless transition: the image may disappear when the composer takes over.
No graphics HAL or system init files are changed here.
The keyboard is not restarted when the splash stops; its existing console and
SurfaceFlinger-state triggers still govern later starts.

## Configuration

All properties are optional and may be set by the device configuration:

| Property | Meaning |
| --- | --- |
| `sys.bootsplash.device` | Explicit `/dev/dri/cardN`, `/dev/graphics/fbN`, or `/dev/fbN`; when empty, try available DRM cards then fbdev nodes. |
| `sys.bootsplash.percent` | Integer from 0 to 100 (invalid values display 0). |
| `sys.bootsplash.color` | Progress foreground color, decimal or `0xRRGGBB` (default `0x53b8df`). |
| `sys.bootsplash.background_color` | Progress background color, decimal or `0xRRGGBB` (default `0x30343b`). |
| `sys.bootsplash.canvas_color` | Full-screen background color, decimal or `0xRRGGBB` (default `0x080b12`). |
| `sys.bootsplash.text` | Label above the progress bar. |
| `sys.bootsplash.logo_text` | Final logo fallback, when neither image is usable. |
| `ro.sf.lcd_density` | DPI used to size the progress bar and labels; read once when the display opens (default 160). |

The logo source priority is the validated UEFI BGRT BMP at
`/sys/firmware/acpi/bgrt/image`, an optional uncompressed 24/32-bit BMP at
`/product/etc/bootsplash.bmp`, then `sys.bootsplash.logo_text`. A late-mounted
product image is retried while the service runs. BGRT x/y offsets
are used if the logo fits at that location; otherwise it is centered. Images
larger than the output are scaled to fit. The product supplies the optional BMP
and its product package entry. A readable `/system/fonts/Roboto-Regular.ttf`
provides Unicode glyphs; if absent, image and progress bar still appear but
the text cannot be rendered. Properties are sampled every 100 ms while running.

## Display Ownership

DRM requires an uncontended primary-node master, connected connector, dumb
buffer, and a working atomic or legacy modeset. It tries XRGB/XBGR/ARGB/ABGR
8888, RGB565, and 2101010 layouts, checking the selected primary plane's formats
for atomic devices. Automatic discovery sees only the cards still present after
device detection; use `sys.bootsplash.device` when a particular card is needed.
No existing compositor is stopped, and acquisition failure is nonfatal. If
another process already holds DRM master, automatic selection does not try
fbdev on the same display.

Fbdev automatically declines to draw when a bound framebuffer console is
found. An explicit fbdev path opts into sharing it with the console; console
logs can overwrite the splash. Fbdev supports validated packed TRUECOLOR
16-, 24-, and 32-bit channels, including RGB555/RGB565, RGB/BGR888, XRGB/XBGR,
and 10-bit RGB layouts, as well as matching packed RGB565, RGB24, XRGB32,
ARGB32, XBGR32, ABGR32, and ARGB2101010 FOURCC layouts. It uses the reported
bitfields or FOURCC layout and stride rather than assuming 32-bit RGB.
Indexed, DIRECTCOLOR, monochrome, and unknown layouts are deliberately rejected.
The visible page is updated and flushed through fbdev writes for drivers with
shadow/damage mappings; no mode, palette, pan, blank, or console state is changed.

System-ext SELinux policy lives in `device/mainline/common/sepolicy/private/`;
the `/dev/fbN` label is in its `sepolicy/vendor/file_contexts`. It labels the
BGRT and vtconsole sysfs paths needed by the service, the executable, and its
properties. Failures to read an image or open a display are logged without
holding up boot; an uninterruptible display-driver call is the exception to
the init stop barrier described above.
