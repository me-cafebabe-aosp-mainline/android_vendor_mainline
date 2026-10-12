# Bootsplash Agent Notes

Follow `hardware/mainline/common/AGENTS.md` and its relevant `docs/` guidance,
except this is a standalone system_ext utility and does not live in an APEX.
Builds, tests and deploying follow `hardware/mainline/common/docs/WORKFLOW.md`: ask the user
first.

- Do not write after the graphics HAL owns the display or change a HAL to make
  the splash work. Init stops this service at early-boot.
- BGRT, product PNG, product BMP, and property text are fallbacks in that
  order. Bound compressed and decoded images, and composite PNG alpha.
- Default to a pure black canvas for every source, but honor an explicit
  canvas color.
- `Image::pixels` holds 0xAARRGGBB. Every backend must map its actual output
  format, pitch, and visible area; never assume `fb0` is XRGB8888.
- Auto fbdev must avoid a bound framebuffer console. Do not alter console
  bindings, palette, framebuffer mode, or display blanking.
- Request that init stop fbkeyboard before starting the splash when both are
  installed; do not assume fbdev nodes provide exclusive-open semantics.
- Keep product selection in `device/mainline/common/optional/bootsplash/` and
  the executable, property, and sysfs policy in
  `device/mainline/common/sepolicy/private/`.
- Keep documentation in `README.md` aligned with property names and hardware
  support. The service must fail without delaying graphics startup.
