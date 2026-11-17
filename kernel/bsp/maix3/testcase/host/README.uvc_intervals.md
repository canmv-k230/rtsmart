# UVC frame interval regression

Run from the RT-Smart repository root:

```sh
python3 -B kernel/bsp/maix3/testcase/host/test_uvc_intervals.py
python3 -B kernel/bsp/maix3/testcase/host/test_uvc_host.py
python3 -B kernel/bsp/maix3/testcase/host/test_dwc2_isoc_ddma.py
```

The interval test compiles the production descriptor-connect function and
frame/interval/format ioctl cases unchanged, using the production frame structs
and USB video descriptors, with ASan/UBSan. USB transfers and kernel user copies
are stubbed. It checks continuous min/max/step ranges, default-only enumeration
through the existing scalar ioctl, termination at EINVAL, discrete intervals,
unaligned descriptors, counts larger than the interval/frame arrays, truncated
payloads, missing frame entries, fallback from unsupported intervals, a camera
default kept when it lies past the stored discrete entries, and several
descriptors of one size (seen on a real HDMI capture camera). Both
kernel arguments and user-copy paths run.
It also checks rejection of complete continuous ranges with reversed bounds or
a zero minimum, safe default selection for empty discrete interval lists, and
warnings for malformed descriptor lengths (including a trailing single byte).

Optional `--video-source` and `--uvc-source` paths accept older sources;
`--case continuous_enum` and `--case missing_frame` reproduce the original
regressions. Neither test requires a board. Continuous ranges are covered on the
host; a physical camera is needed to verify its descriptors and streaming.

`bFrameIntervalType == 0` means a continuous min/max/step payload. The existing
scalar interval ioctl lists its default once, then returns EINVAL; format
selection accepts supported intervals throughout the range. Missing or unusable
frame slots are omitted from frame enumeration without renumbering the stored
USB descriptor indexes.

Callers see sizes and intervals; descriptors are a driver detail. A camera may
describe one size with several descriptors, for example one per default rate.
Frame enumeration lists each usable size once. Interval enumeration lists the
intervals of every descriptor of that size, each once. S_FMT selects the first
descriptor of the size that supports the requested interval and records its
USB format and frame index; opening the stream probes exactly that frame. Callers must never receive a zero interval.

Two cameras can remain attached with one opened at a time. The
SDK `CONFIG_USBHOST_MAX_VIDEO_CLASS` option controls the device limit and
defaults to two. RTOS builds without that SDK option use the same default
through CherryUSB's fallback configuration.
`test_uvc_host.py` also checks opening each camera in turn, EBUSY when the other
is already open, and unplugging an unused camera without stopping the active
camera. Use `UVC.select_device(0)` or `UVC.select_device(1)` before probe, mode
listing and streaming; selecting a new camera stops the previous one. Device
indexes follow USB enumeration order and can change after reconnecting.
