# Universal USB Parser

A zero-dependency, standalone native Win32/x64 tool for Windows 10 and 11 that interrogates connected USB input devices and extracts byte-accurate descriptors, field bit offsets, report structures, and protocol layouts.

---

## Purpose & Use Cases

When writing embedded USB host stacks or device emulators (such as MAKCU, MAKXD, custom microcontrollers, or host passthrough bridges), developers need exact byte-by-byte knowledge of:
1. Downstream device descriptors (Device, Configuration, Interface, Endpoint, HID).
2. Report descriptors and on-wire bit offsets for buttons, axes, triggers, and hats.
3. Proprietary controller transport protocols (Xbox GIP, XInput, PlayStation HID) that bypass standard USB HID report descriptors.

`universal_usb_parser` extracts this evidence on Windows 10 and Windows 11 without requiring Python, pip, drivers, or administrative setup.

---

## How It Works

The tool utilizes three parallel evidence pipelines:

### 1. Physical Parent-Hub IOCTLs (Primary Descriptor Path)
- Enumerates host controllers (`GUID_DEVINTERFACE_USB_HOST_CONTROLLER`) and USB hubs (`GUID_DEVINTERFACE_USB_HUB`).
- Determines port counts using modern `IOCTL_USB_GET_HUB_INFORMATION_EX` (USB 3.0/3.1/3.2/USB4 / xHCI) with fallback to `IOCTL_USB_GET_NODE_INFORMATION` (USB 2.0).
- Reads physical connection info via `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX`.
- Queries the physical device directly via parent hub control transfers (`IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION`) using standard USB requests (`GET_DESCRIPTOR`, types `0x01` Device, `0x02` Configuration, `0x03` String, `0x22` HID Report).

### 2. Direct HID Stack Capability Reconstruction (Fallback Path)
- Many physical USB devices (e.g., optical mice, custom peripherals) stall or reject repeated `GET_DESCRIPTOR(0x22)` requests after initial enumeration, returning `Win32 error 31` (`ERROR_GEN_FAILURE`).
- When parent-hub descriptor extraction fails, the parser falls back to the Windows HIDClass subsystem directly:
  - Queries collection attributes via `HidD_GetAttributes` and strings via `HidD_GetProductString` etc.
  - Queries preparsed capability structures via `HidD_GetPreparsedData` and `HidP_GetCaps`.
  - Extracts exact button caps (`HidP_GetButtonCaps`), value caps (`HidP_GetValueCaps`), and collection link nodes (`HidP_GetLinkCollectionNodes`).
  - Reconstructs a valid, canonical HID report descriptor preserving exact bit offsets, ranges, usages, and signedness.

### 3. Native USB Interface & Controller Protocol Classification
- Inspects interface signatures across active USB configurations:
  - **Xbox GIP**: Class `0xFF`, Subclass `0x47`, Protocol `0xD0`. Generates GIP packet selectors (`0x20` command envelope, 18-byte minimum, bit-exact offsets for buttons, triggers, sticks).
  - **Xbox XInput/XUSB**: Class `0xFF`, Subclass `0x5D`, Protocol `0x01`. Generates 20-byte wire state offsets.
  - **Xbox 360 Wireless Receiver**: Class `0xFF`, Subclass `0x5D`, Protocol `0x81`. Handles 4-byte envelope prefix.
  - **PlayStation Controllers**: DualShock 3 (`054C:0268`), DualShock 4 (`054C:05C4`, `054C:09CC`), DualSense (`054C:0CE6`), DualSense Edge (`054C:0DF2`).
  - **GameInput Integration**: Interfaces with Windows `GameInput.h` / `gameinput.lib` to stream live physical GIP packets.

### 4. Independent Report Descriptor Parsing & Verification
- Independently parses the raw or reconstructed report descriptor bytes.
- Traverses Main, Global, and Local HID items, managing PUSH/POP global state stacks.
- Calculates payload bit offsets (excluding Report ID) and on-wire bit offsets (including Report ID).
- Compares computed layouts against Windows HID capabilities to guarantee zero bit discrepancies (`Status: MATCH`).

---

## Windows 10 & 11 Compatibility

The codebase includes specific Windows 10 and Windows 11 platform accommodations:
- **SetupDi Sizing Fix**: In Windows 11, `SetupDiGetDeviceInterfaceDetailW` fails with `ERROR_INVALID_PARAMETER` (error 87) if a non-NULL `DeviceInfoData` pointer is supplied during a buffer-size query (`DeviceInterfaceDetailData == NULL`). The parser passes `NULL` on the sizing call and passes `DeviceInfoData` only during data retrieval.
- **Header cbSize Probing**: Probes structure packing sizes (`8`, `6`, and `5` bytes) to guarantee compatibility across x64/x86 and differing Windows SDK headers.
- **Shared Access Flags**: All device file handles open with `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` so system-claimed devices (like active keyboards and mice) can be inspected without access denial (`ERROR_SHARING_VIOLATION`).
- **xHCI Hub Query**: Uses `IOCTL_USB_GET_HUB_INFORMATION_EX` to resolve port counts on USB 3.0/3.1/3.2/USB4 hubs.

---

## Supported Device Categories

1. **Mice & Pointing Devices**:
   - Boot mice, high-report-rate gaming mice, multi-button mice, horizontal/vertical scroll wheels.
2. **Keyboards**:
   - Standard boot keyboards, multi-interface composite keyboards, consumer/media keys, NKRO keyboards.
3. **Game Controllers**:
   - Standard USB HID gamepads and joysticks.
   - Xbox One, Xbox Series X/S, and Xbox Elite controllers (via GIP `FF/47/D0`).
   - Xbox 360 wired controllers and wireless receivers (via XInput `FF/5D/01`, `FF/5D/81`).
   - Sony PlayStation controllers (DualShock 3, DualShock 4, DualSense, DualSense Edge).
4. **Consumer & System Controls**:
   - Media keys, volume controls, power/sleep buttons.
5. **Vendor-Defined & Auxiliary Interfaces**:
   - Proprietary configuration channels, RGB controllers, audio/bulk endpoints.

---

## Building

### Prerequisites
- Visual Studio Build Tools with C++ (2017, 2019, 2022, or 2026).

### Build Command
Run:
```cmd
build.cmd
```

This invokes MSVC:
```cmd
cl /nologo /std:c++17 /O2 /EHsc /W3 /DWIN32_LEAN_AND_MEAN makcu_hid_extractor.cpp /Fe:universal_usb_parser.exe /link /OPT:REF /OPT:ICF
```

Produces:
- `universal_usb_parser.exe` (standalone binary)

---

## Usage

### Interactive Device Selector
Run with no arguments to see a numbered list of connected input devices:
```cmd
universal_usb_parser.exe
```
Enter the number next to the device to run extraction.

### Direct VID/PID Selection
```cmd
universal_usb_parser.exe --vid 03F0 --pid 2A41
```

### List Connected Input Devices
```cmd
universal_usb_parser.exe --list
```

### Specify Output Directory
```cmd
universal_usb_parser.exe --vid 045E --pid 02FF --output-dir captures
```

---

## Output Artifacts

For each extracted device, the tool generates:
- `makcu_hid_<VID>_<PID>.json`: Complete machine-readable JSON containing full descriptor dumps, interface descriptions, endpoint mappings, and parsed bit layouts.
- `makcu_hid_<VID>_<PID>.txt`: Human-readable technical summary with wire packet diagrams and comparison tables.
