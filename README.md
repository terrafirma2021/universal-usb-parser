# Universal USB Parser

Extract a connected USB device's descriptors and input layouts into **machine-readable JSON and a readable text report**. The tool describes keyboard, mouse, and controller fields so another tool or firmware agent can build the appropriate input parser.

This is a portable native executable for **Windows 10/11 x64**. Running it needs no Python, package manager, additional runtime, GameInput installation, or replacement USB driver. Descriptor availability still depends on the device and Windows access; unavailable evidence is reported explicitly.

The tool reads USB descriptors and Windows HID information. It does **not** record live input packets, inject input, flash firmware, or implement controller authentication and output effects. `firmware_layout_ready` describes the exported layout's suitability for input-parser generation, not an already generated or hardware-tested firmware implementation.

## Supported layouts

| Device or transport | What the export provides | Boundary |
| --- | --- | --- |
| HID mouse | Buttons, axes, wheel/pan, bit packing, signedness, and relative/absolute state from the descriptor | Layouts follow the actual descriptor, not a fixed mouse report |
| HID keyboard | Modifiers, key arrays, NKRO keys, media/system controls, LED outputs, and declared usage dictionaries | Array slots remain selectors; usage names are not locale-specific text |
| Generic HID gamepad, joystick, or composite device | Declared buttons, axes, hats, collections, and separate Input/Output/Feature reports | Generic usages alone do not identify left/right sticks, triggers, or platform button names |
| Windows raw HID preparsed data (PPD) | Recovered collection hierarchy, packed fields, selector arrays, aliases and repeated values through the same semantic model | Supported PPD representation only; physical eligibility requires separate live identity, framing and per-report validation |
| Xbox GIP, XUSB, and Xbox 360 wireless receiver | Known input selectors, normalized buttons, triggers, and sticks | GIP framing/reassembly is a consumer responsibility; variant extensions remain preserved |
| DualShock 3-compatible and DualShock 4-compatible USB controllers | Known basic controller input fields when the parsed Input layout matches | Includes third-party VID/PID identities; additional unmodeled data stays in the raw layout |
| DualSense-compatible USB controllers, including matching Edge and third-party layouts | Basic controls plus sequence, native PlayStation names, raw motion sensors, timestamp, both touch contacts, and known battery/audio status fields | Selected by parsed Input layout, without a VID/PID allowlist. The common layout does not identify the manufacturer or Edge variant; variant-specific controls, Output/Feature semantics and unknown extensions remain separate |

Support here means the listed interpretation is implemented. It does not guarantee that Windows will expose an exact descriptor for every device or that every device-specific extension is understood. All related interface layouts are exported with their own evidence and readiness flags.

VID/PID identifies the device to extract; it does not choose its controller protocol. Sony-compatible families are recognized from the parsed HID Input report, and Xbox families from their USB interface signatures. A third-party controller can use a Sony-compatible layout under its own VID/PID. When no unique compatible family is established, the export retains the generic descriptor-derived fields instead of assigning a protocol from the device name or IDs.

## Quick start

Download `universal_usb_parser.exe` or the Windows ZIP from [GitHub Releases](https://github.com/terrafirma2021/universal-usb-parser/releases/latest). Extract the ZIP before running the executable.

Connect the device, launch `universal_usb_parser.exe`, and choose its number. The tool writes `makcu_hid_<VID>_<PID>.json` and `makcu_hid_<VID>_<PID>.txt` in the current directory.

For command-line use:

```cmd
universal_usb_parser.exe --list
universal_usb_parser.exe --vid 054C --pid 0CE6 --output-dir captures
universal_usb_parser.exe --vid 045E --pid 0B12 --output-dir captures
universal_usb_parser.exe --help
```

`--vid` and `--pid` accept hexadecimal IDs, including an optional `0x` prefix; both must be supplied together. Extraction errors return a nonzero exit status. A successful extraction can still contain unavailable or unverified layouts: inspect readiness before generating firmware.

## Windows raw preparsed-data recovery

When Windows exposes HID preparsed data but the original USB HID report descriptor is unavailable, the extractor now decodes that PPD directly into the existing keyboard, mouse, controller and vendor-field model. It does not need to invent a replacement descriptor or a fixed keyboard/mouse template. Exact physical descriptors and applicable known controller protocols retain their higher priority.

Live recovery first requests the collection information and descriptor with an independently measured byte length. It can fall back to Raw Input for the same collection. A matching VID/PID alone is insufficient: different interface-class paths must resolve to the same collection name and Windows device instance. Acquisition errors remain visible beside a successful fallback. Some Raw Input mouse/keyboard entries expose no PPD even when the HID collection IOCTL works.

Each acquired blob is saved as `makcu_hid_<VID>_<PID>_collection_<index>.ppd` and remains available as `cached_collection_descriptor_hex` in the JSON. These are Windows PPD bytes, **not original HID report-descriptor bytes**. The export identifies the source, actual length, collection match, decoder format, reference revision, Windows version and HID API version when known.

For a saved blob, use the standalone offline mode:

```cmd
universal_usb_parser.exe --ppd-file captures\keyboard.ppd --output-dir captures\offline
```

Offline mode writes `makcu_ppd_keyboard.json` and `.txt`. Supply a locally saved PPD file; captures and fixtures are not distributed with this repository. It never passes imported bytes to Windows HID parsing functions. Physical identity, transport and live Windows validation are not inferred from a filename or a successful decode. Every offline layout remains excluded from physical firmware generation.

The supported representation is `hidp-kdr-header44-cap104-link16-le-v1`, pinned to the HIDAPI reference revision recorded in [third-party notices](THIRD_PARTY_NOTICES.md). Windows reserves the internal PPD format; a recognized signature alone does not establish compatibility. Bounds, occupied record ranges, links, field extents and supported semantics are checked before publishing normalized fields. Unsupported data retains diagnostics and raw evidence.

Live Windows validation uses only the original API-owned `HidD_GetPreparsedData` allocation. It compares capabilities and performs in-memory field read/write checks, including arrays, signed storage, aliases, short buffers, incompatible IDs and preservation of unrelated bits. These checks send no reports to the device. A Windows validation pass is distinct from physical packet capture or generated-firmware testing.

Report-buffer preparation validates the requested report type, ID, metadata and field bounds first. Complete non-null scalar/value-array reports use a zero-payload buffer with the declared ID and a successful type-scoped Windows setter directly. The setter must retain that ID, the zero payload and the buffer boundary. This avoids relying on the native initializer's report-ID lookup and disambiguates usages shared by several reports. Null-state and button/selector reports use native initialization only with additional zero/null and inactive-usage checks. A failed preparation discards its buffer. `report_buffer_ready` does not grant field validation: every control still needs the independent Windows read/write comparisons, and physical generation still requires a matching live base report.

Native initializer conformance is audited separately, once per collection and report type, against public capability IDs. The audit checks all 256 IDs and one short buffer; it retains actual statuses for every discrepancy in `native_initializer_audit` without repeating the same absent-ID failure for every Feature report. Preparation failures and API discrepancies remain visible in the export. No Windows DLL, VID/PID exception or imported PPD is substituted into Windows APIs.

Descriptor comparisons distinguish real Constant storage from data and validate every repeated slot. A PPD-only recovered extent remains a lower bound unless separate evidence establishes the complete report length. Validation and conflicts are scoped to each report type and ID, so an unrelated Output/Feature failure cannot invalidate a correct Input report.

## Using the export for firmware generation

Start with **`firmware_layout_selection.selected_reports`**. Each entry contains a `layout_path` JSON pointer to the selected report, its report type/ID or protocol selector, collection identity, physical device, configuration, interface and alternate setting. Apply the entry's `report_type` and `top_level_collection_ordinal` filters when a generic HID report object contains several types or collections. The legacy `selected_layouts` list remains an interface-level Input summary for compatibility; it is not the complete per-report generation contract.

For a known controller protocol, follow the selected protocol report and its framing/selector conditions. For generic HID, follow the selected report and fields. Require the selection's `firmware_layout_ready: true` and `eligible_for_firmware_parser_generation: true`, together with the containing HID/protocol layer's physical evidence. Input, Output and Feature reports are assessed separately; one failed Feature or unknown report cannot suppress a valid reconstructed Input report. Independent collections and partially covered reports remain available.

For PPD recovery, readiness means **modifying defined fields in a matching existing live base report**. `requires_live_base_report` is true and `whole_packet_generation_ready` is false. Preserve unknown storage, padding, other collections and unrelated controls. Do not generate a complete packet by zero-filling unmodeled bytes. `windows_api_bit_offset`, `payload_bit_offset` and candidate `wire_bit_offset` are separate: the artificial Windows prefix for Report ID zero is not a byte on the USB wire.

Windows buffer lengths are maxima for a collection and report type. A shorter report's recovered extent is a lower bound, not proof of its full physical length. Read `report_readiness` and `length_evidence` rather than treating an interface-wide flag or a Windows maximum as approval for every ID. Conflicting shared layouts and known-protocol Output length conflicts are excluded from selection. [SCHEMA.md](SCHEMA.md) defines these additive fields and the consumption rules.

The evidence order is **known protocol bound to a physical interface → exact physical HID descriptor → physically correlated reconstruction**. Windows logical HID views remain diagnostic and cannot override the physical layout. For example, Xbox `045E:02FF` can be a Windows child of physical `045E:0B12`; use the physical GIP layout for USB packet parsing.

For a matching DualSense-compatible USB layout, the resolved physical interface selects `dualsense_usb` at priority **1**, regardless of VID/PID. Basic and common extended input semantics are complete within the documented scope. The selected Input report is **64 bytes including Report ID `0x01`**. Exact descriptor fields, all other Report IDs, and reserved bits remain available alongside that semantic layer.

Readiness does not require every sensor or output effect to be modeled. `basic_input_semantics_complete`, `extended_input_semantics_complete`, and `output_semantics_complete` describe different scopes. Downstream firmware must preserve uninterpreted data, implement any required transport handling, and validate its generated parser and injection behavior on hardware.

## Build from source

Clone or download the repository source, install Visual Studio C++ Build Tools and a Windows SDK, then run:

```cmd
build.cmd
```

`build.cmd` locates the installed x64 MSVC toolchain and builds C++17 with `/W3 /WX /MT`. The C/C++ runtime is linked statically. It produces `universal_usb_parser.exe` and the identical compatibility copy `makcu_hid_extractor.exe`. GitHub Actions builds from source, checks executable startup and embedded licenses, and verifies that both executable copies are identical before publishing the executable and ZIP. Local compiled files are ignored and never committed.

The public build compiles the parser only. Tests, fixtures, captures and machine-specific validation records remain local, are excluded by `.gitignore`, and are rejected by the publication check if tracked.

## Output files

Each selection writes `makcu_hid_<VID>_<PID>.json` and `makcu_hid_<VID>_<PID>.txt`. The filename and top-level device VID/PID describe the requested identity. `physical_devices` records the correlated physical USB identities, which can differ from Windows HID child identities.

The JSON schema is **`makcu-input-extraction`, version `4`**. Consumers of the older `makcu-hid-extraction` version 3 schema must explicitly adopt version 4. Existing raw descriptor evidence remains available.

The additive extension is `schema_extension: "windows-ppd-v1"`. Live captures include `.ppd` sidecars; saved imports have `offline: true`, `device: null`, an empty `physical_devices` array and no firmware selections.

| JSON path | Contents |
| --- | --- |
| `device` | Requested VID/PID and associated Windows HID collection count |
| `physical_devices[]` | Physical instance, parent hub, port, driver key, device/configuration descriptor hex, interfaces, alternate settings, endpoints, and descriptor retrieval errors |
| `interfaces[]` | A physical interface layout or an explicitly scoped Windows collection layout |
| `interfaces[].raw_report_descriptor` | Availability, original-byte `exact` flag, source, length, hex, and parent-hub error |
| `interfaces[].reports[]` | One layout per Report ID, with independent Input, Output, and Feature lengths and fields |
| `interfaces[].collections[]` | Top-level collection classification, usage, ordinal, and associated reports |
| `interfaces[].collection_nodes[]` | Nested collection hierarchy and raw collection types/usages |
| `interfaces[].windows_hid_collections[]` | Windows identities, ancestors, strings, all three classes of button/value caps, collection links, observed fields, and verification results |
| `interfaces[].windows_hid_collections[].preparsed_data` | PPD source/length/sidecar, raw record evidence, diagnostics, OS/API metadata and per-type/ID Windows-oracle results |
| `interfaces[].report_readiness[]` | Per-type/ID PPD reconstruction, length evidence, physical eligibility and shared-layout conflicts |
| `interfaces[].protocol_layout` | Known protocol Input selectors, normalized/native fields, structured touch data, completeness flags, and separate Output transport metadata, or `null` |
| `interfaces[].hid_layout_evidence` | Provenance of the generic HID reports, separate from a preferred proprietary protocol |
| `firmware_layout_selection` | Selection policy and JSON pointers to eligible physical firmware layouts, scoped by physical unit and interface |
| `firmware_layout_selection.selected_reports[]` | Report-level selections scoped by physical unit, configuration, interface, alternate, collection, report type/ID and selector |

A physical HID descriptor is exported once per interface, even when Windows exposes several top-level collection handles for it. When only per-collection Windows information is available, separate entries retain `layout_scope: "windows_top_level_collection"`. Always use `physical_device_index`, `interface_number`, `alternate_setting`, and collection ordinals together; VID/PID alone does not identify an individual physical unit or logical function.

Classification values include `mouse`, `keyboard`, `gamepad`, `joystick`, `multi_axis_controller`, `consumer_control`, `system_control`, `vendor_defined`, and `unknown`. An interface containing multiple classifications uses `composite`; each collection retains its own classification.

## Consuming a generic HID layout

Read `interfaces[].reports[].fields[]` and select `report_type: "Input"` for input parsing. The same array also preserves Output and Feature fields. Reports are never merged across IDs; their Input, Output, and Feature offsets are independent.

Each field includes:

- `report_type`, `report_id`, `usage_page`, `usage`, readable usage/page names, and collection ordinals.
- `payload_bit_offset`, `wire_bit_offset`, `bit_offset`, `bit_size`, `report_count`, `element_size`, `element_count`, and `total_bits`.
- Logical/physical ranges, unit/exponent, signedness, constant/variable/array flags, relative/absolute state, null-state behavior, and original main-item flags.
- `semantic_type`, `semantic_name`, `normalized_role`, and `role_confidence`.

`wire_bit_offset` starts at the first bit of the USB report, including an actual Report ID byte. `payload_bit_offset` excludes that byte. `bit_offset` is an explicit alias of `wire_bit_offset`. An unnumbered report has ID zero and no on-wire ID byte. Windows HID buffers reserve a byte even for unnumbered reports; that Windows-only byte is excluded from USB wire offsets and sizes.

Fields use least-significant-bit-first HID packing. Extract the indicated bits, sign-extend only when `signed` is true, and retain the logical range and relative/absolute interpretation. Non-byte-aligned 12-bit fields, unsigned 32-bit maxima, and fields wider than 255 bits are retained without narrowing.

For variable fields, each element has its own field entry. `main_item_index` and `element_index` retain grouping. For arrays, a single field retains the slot width and count: an 8-bit, six-element `key_array` is six key selectors, never 48 Boolean keys. Ordered `usage_ranges` map array `selector_min..selector_max` to `usage_min..usage_max` on the specified page, including sparse and nonzero-starting usage lists.

Every `key_array` and `consumer_control_array` also exports `usage_min`, `usage_max`, and `usage_values`. These dictionaries extend version 4 without changing report layouts. `usage_values` maps each declared usage ID on the field's usage page to its semantic name. Keyboard examples include `"0x04": "keyboard_a"`, `"0x28": "keyboard_enter"`, and `"0x2C": "keyboard_space"`. Keyboard dictionary keys use uppercase hexadecimal digits with at least two digits and retain the full 16-bit usage ID. A declared `0x00..0xFF` keyboard array exports all 256 entries, including no-key/error statuses, named keyboard/keypad usages, and numeric fallback names for reserved or unknown usages.

Consumer dictionary keys use four hexadecimal digits, for example `"0x00B0": "play"`, `"0x00B1": "pause"`, `"0x00B5": "next_track"`, `"0x00B6": "previous_track"`, `"0x00B7": "stop"`, `"0x00CD": "play_pause"`, `"0x00E2": "mute"`, `"0x00E9": "volume_up"`, and `"0x00EA": "volume_down"`. A Consumer array declaring usages `0x0000..0x023C` exports all 573 entries. Usages without an internal name remain explicit numeric fallbacks such as `usage_0x000C_0x0008`; their presence is not a claim that they are known controls.

The dictionary is built from the descriptor's ordered Usage items and Usage Minimum/Maximum ranges. It includes only declared usages, deduplicates overlapping/repeated usages, and preserves sparse domains. `usage_min`/`usage_max` are bounds; they do not fill gaps. Missing local usages produce an empty dictionary and null bounds, without inferring a domain from the logical range. Mixed-page arrays keep all original page-qualified selector ranges; `usage_values` describes only the field's `usage_page` (`0x07` for keyboard arrays or `0x0C` for Consumer arrays). Never apply that dictionary to a selector mapped to another page.

To decode an array, extract each slot at `wire_bit_offset + slot_index * element_size`, apply signedness and logical bounds, map the selector through `usage_ranges`, then look up the resulting usage on the matching page in `usage_values`. Dictionary keys are usage IDs, which need not equal report selector values. For the eight-byte keyboard report with six slots at wire bit 16 and matching `0..255` logical/usage ranges, bytes 2 through 7 map directly to these usage IDs. A sparse array can instead map selector 1 to usage `0x28` (Enter) and selector 2 to usage `0x04` (A). The three-byte Consumer report with Report ID 2 and a 16-bit slot at wire bit 8 similarly uses bytes 1 and 2 as a little-endian selector; the Report ID is not part of that selector.

Consumer usage `0x0000` is undefined. Nonconstant variable Input, Output, and Feature fields with this usage export `semantic_type: "undefined"` and `semantic_name: "consumer_undefined"`, retaining every raw bit offset, width, count, range, flag, and collection reference. A Consumer array whose first usage is zero remains `consumer_control_array`, and its dictionary names zero `consumer_undefined`. Constant fields remain padding. No normalized control role is invented for undefined fields.

The TXT export prints `KEY ARRAY` and `CONSUMER CONTROL ARRAY` sections with wire/payload offsets, element width, slot count, declared usage ranges, selector mappings, and every dictionary entry. NKRO keys and variable Consumer fields retain individual field positions and do not acquire array dictionaries. These names identify HID usages rather than locale-dependent characters; a broad declared usage domain does not prove that the device physically has every corresponding key or control. Keyboard numeric fallback names remain unknown/reserved usages rather than named keys.

Mouse semantics include arbitrary button usages, X/Y/Z/Rx/Ry/Rz, Slider, Dial, Wheel, and Consumer AC Pan. Keyboard semantics distinguish modifiers, selector arrays, NKRO keys, consumer/media controls, system controls, and LED outputs. Hat switches retain logical/physical ranges and the null-state flag. Unknown and vendor-defined fields remain in the report layouts.

Generic controller axes keep names such as `x`, `rx`, or `z`, with `normalized_role: null` and `role_confidence: "unknown"`. Their usages do not establish left/right sticks or triggers. No generic HID layout is selected by a device-specific VID/PID exception.

The TXT file contains semantic collection/report summaries, field names, bit offsets, widths, ranges, signed/relative state, protocol fields, raw descriptor hex, and actual comparison results.

## Physical and reconstructed evidence

The primary path retrieves descriptors through the physical parent USB hub. Original descriptors use `source: "usb_hub_ioctl_physical_device"` and `exact: true` when the returned length agrees with the declaration.

When that path is unavailable, the Windows fallback uses `HidD_GetPreparsedData`, capability queries, and `HidP_SetUsages` / `HidP_SetUsageValue` / `HidP_SetUsageValueArray` on **local memory buffers** to measure field locations. These operations do not send input, Output reports, or Feature reports to the device. Reconstruction uses those observed offsets and preserves gaps instead of sorting fields by usage or assuming byte alignment.

A reconstructed descriptor uses `source: "windows_hid_stack"` and `exact: false`. It is a canonical layout, not recovery of the original descriptor byte sequence or original nested collection topology. The original Windows capability and collection-link evidence remains alongside it. Cached Windows collection-descriptor bytes are exported separately as opaque evidence and are not misrepresented as a USB HID report descriptor.

Some information cannot be recovered through the available Windows capabilities. Legacy button-array caps do not provide enough information here to establish original array slot widths/counts safely. Aliases or failed field probes also leave an unresolved layout. The tool retains capabilities and diagnostics and withholds a reconstructed descriptor instead of inventing a keyboard bitmap. An exact physical descriptor can still describe and export these arrays normally.

Windows exposes maximum report-buffer sizes per report type. When several Report IDs share one Windows report type, fallback lengths are upper bounds rather than proven per-ID wire sizes. `wire_sizes_exact: false` and accompanying notes make that limitation explicit.

Use `firmware_layout_ready` as a gate, then inspect the requested Input report or protocol selector. Generic HID readiness requires a physical HID association, a usable descriptor without parsing errors or unhandled-item warnings, established wire lengths, and no known comparison mismatch. `parse_errors`, `parse_warnings`, descriptor provenance, and Windows `layout_errors` remain inspectable. A Windows-only logical layout is never advertised as a physical firmware transport layout.

`descriptor_verification.status` can be `match`, `mismatch`, `parse_error`, or `unavailable`. Verification compares collection usages, capability membership, maximum report lengths, and available measured offsets/widths/ranges. `observed_fields_checked` and `all_offsets_verified` describe its coverage; `independent_descriptor` distinguishes comparison against physical bytes from checking a reconstruction derived from the same Windows data. A `match` is not a blanket claim that every array bit was independently measured.

## Layout authority and range diagnostics

This additive schema-v4 metadata distinguishes physical USB layouts, operating-system views, and protocol definitions. Each interface, HID collection/report, Windows capability view, and protocol profile/report carries `layout_source`, `authority`, and `physical_wire_layout`. Interface entries also expose `protocol`, `transport`, `layout_selection_priority`, and `preferred_firmware_layout`.

| Evidence | `layout_source` | `authority` | Selection priority |
| --- | --- | --- | --- |
| Physical proprietary input transport | `physical_usb_protocol` | `physical_wire` | 1 |
| Known protocol definition selected by parsed Input layout or USB transport signature and bound to a matching physical interface | `known_protocol_definition` | `protocol_semantics` | 1 |
| Physical HID report descriptor | `physical_hid_report_descriptor` | `physical_wire` | 2 |
| Reconstructed HID layout tied to a physical HID interface | `windows_hid_stack` | `physical_wire_reconstruction` | 3 |
| Validated raw PPD tied to a physical HID interface | `windows_preparsed_data` | `physical_wire_reconstruction` | 3 |
| Windows logical collection | `windows_hid_stack` | `logical_os_view` | 4, diagnostic only |

Lower numbers take precedence among candidates for the same physical device, configuration, interface, alternate setting and covered report/collection. Read `firmware_layout_selection.selected_reports[].layout_path` for the selected report and apply its type, collection and selector filters. Every selected entry must pass its own readiness checks. Multiple physical devices, independent report types/IDs and incompletely covered collections retain separate selections. A partial descriptor, auxiliary interface, unverified alternate setting or unbound definition does not become eligible merely by having a source label or priority. Windows logical views are excluded even when no usable physical layout exists.

Every semantic field exports `logical_range_source`, `range_validation_applicable`, `range_consistent_with_field_width`, and `range_authoritative_for_physical_wire`. For nonconstant fields, `range_validation_applicable` is `true`, and validation checks the declared minimum and maximum against the signed or unsigned storage width, including zero widths, reversed ranges, and 32/64-bit boundaries. It does not change `logical_min`, `logical_max`, signedness, flags, or offsets. Inconsistent nonconstant Input ranges prevent generic HID firmware readiness; Output/Feature inconsistencies remain diagnostics on their respective fields. Constant fields retain their inherited descriptor ranges but export `range_validation_applicable: false`, `range_consistent_with_field_width: null`, and `range_authoritative_for_physical_wire: false`. TXT marks their range validation as not applicable and emits no inconsistency diagnostic for padding.

For example, a Windows logical 16-bit axis with `logical_min: 0` and `logical_max: 4294967295` retains those values while exporting `logical_range_source: "windows_hid_caps"`, `range_consistent_with_field_width: false`, and `range_authoritative_for_physical_wire: false`. A 32-bit unsigned field with the same declared maximum is width-consistent. Source authority is independent of representability: even consistent Windows logical values remain non-authoritative for physical USB parsing.

`windows_hid_collections[].value_caps` retains the original signed Windows `LONG` values. The existing observed/reconstructed range interpretation also remains intact: an unsigned maximum with bit pattern `0xFFFFFFFF` can appear as `-1` in the raw capability record and as `4294967295` in the reconstructed field. This update does not clamp it to the measured field width or use it to scale a physical controller. Read the selected physical protocol or physical HID layout instead. The TXT output includes the same source, authority, and range diagnostics.

## Known controller protocols

`protocol_layout` is separate from generic HID fields. Physical vendor-specific transports are not forced through the Windows logical HID layout.

| Protocol | Evidence and exported core controls |
| --- | --- |
| Xbox GIP | `FF/47/D0` interface signature with an interrupt IN endpoint; input-state and virtual-key selectors, buttons, triggers, and four signed stick axes |
| Xbox XUSB | `FF/5D/01`; 20-byte state packet selector and normalized buttons, triggers, and sticks |
| Xbox 360 wireless receiver | `FF/5D/81`; receiver envelope and nested state packet selectors |
| DualShock 3-compatible USB | Parsed report 1 with matching 49-byte framing, controller collection, axes and buttons; core controls |
| DualShock 4-compatible USB | Parsed report 1 with matching 64-byte framing, controller collection, axes, triggers, button positions and D-pad hat |
| DualSense-compatible USB | Parsed report 1 with matching 64-byte framing and DualSense control structure; common controls, native button names, sequence, signed motion sensors, timestamp, packed touch contacts and battery/status codes |

Each protocol report exports selector byte masks, minimum/exact lengths, header length, wire/payload offsets, logical ranges, normalized roles with `known_protocol` confidence, and uninterpreted regions/tails that must be preserved. HID usages are `null` for protocol-specific fields rather than fabricated. Sony HID descriptor layouts remain available separately; incompatible Input framing or control structure withholds the normalized profile.

`controller_profile_match` explains the candidate's match or rejection in JSON; TXT includes the same decision and reasons. Its `selection_basis` is `parsed_hid_input_layout` for Sony-compatible families or `usb_interface_signature` for Xbox families; `vid_pid_used_for_family_selection` is always false. Sony-compatible candidates are selected after parsing the HID Input layout, with exactly one matching family required. Physical binding requires a non-boot HID interface, one interrupt IN/OUT pair, compatible endpoint capacities, and one controller collection with the expected report ID, length, axes, triggers, hat, button positions and ranges. This prevents equal-length DS4 and DualSense reports from being confused. PPD-backed matching additionally requires complete, exact-length, report-specific Windows Input validation. Unrelated Feature layout changes do not prevent common Input matching.

GIP and XUSB require their corresponding vendor-interface class/subclass/protocol and interrupt endpoint topology. GIP controller data is restricted to interface 0; auxiliary audio/bulk interfaces and unsupported alternate settings are rejected. Third-party identities are eligible for all supported families when their actual layout and transport satisfy the corresponding checks. VID/PID values remain attached to the actual device for identity correlation, without a manufacturer or product allowlist. A common DualSense-compatible Input layout exports `dualsense_usb`, including matching Edge layouts; no Edge-specific identity or controls are inferred from PID. Logical Windows children and offline imports cannot obtain physical binding. These checks establish compatibility with a known profile, not device authentication; `device_authentication_verified` remains false.

GIP fixed wire offsets apply only to the exported unchunked, single-length-byte selector. Its payload-length condition must also pass. Extended headers and chunked packets require protocol decoding/reassembly before applying payload offsets. Audio/bulk auxiliary interfaces do not receive the interrupt gamepad state layout. Variant-specific Share, Elite paddle, and Edge extensions are not guessed; unmapped bytes remain opaque. These profiles describe known input fields, not a complete implementation of controller authentication, initialization, output effects, calibrated sensor processing, or every firmware-specific extension.

Windows identities are correlated using instance/ancestor chains and physical driver-key/port evidence. Selecting the Xbox `045E:02FF` child can therefore include its actual `045E:0B12` GIP parent. The child remains a separately labeled `windows_logical_collection`, and its offsets are not presented as the physical GIP packet layout. Multiple units with identical VID/PID remain separated by physical instance.

The physical GIP interrupt input interface exports `classification: "gamepad"`, `protocol: "xbox_gip"`, `physical_wire_layout: true`, and `firmware_layout_ready: true`. An unavailable HID report descriptor is expected for this vendor-specific transport. The Windows `02FF` child retains `physical_wire_layout: false`, `firmware_layout_ready: false`, and `authority: "logical_os_view"`. Its reconstructed fields and raw capability evidence remain available for diagnostics.

GIP protocol metadata includes `transport: "usb"`, `header_bytes: 4`, and `requires_reassembly_before_parse: true`. Profile-level `header_bytes` and `minimum_wire_bytes` describe the named `default_selector`, normally `input_state`; each report has its own lengths. Existing `selector` and `payload_start_byte` properties remain, with explicit aliases `selector_name` and `payload_offset_bytes`. The normal-state selector has `command: 32`, an 18-byte minimum, and payload offset 4. Guide remains on the separate `virtual_key` selector with `command: 7`, a 6-byte minimum, and payload offset 4.

The reassembly flag requires a framing stage; it does not mean an already complete unchunked packet needs to be reassembled again. `framing.wire_offsets_apply_to` requires an unchunked packet with a single length byte that passes every existing selector and payload-length condition. Chunked traffic must be reassembled and extended headers decoded before applying payload-relative offsets to the resulting logical payload. Reassembly does not make the raw USB packet's original header four bytes long. This extractor publishes the framing contract; it does not implement a new reassembler or alter the existing GIP field offsets.

Protocol fields separate storage from interpretation using `wire_bit_size`, `raw_storage_min`, `raw_storage_max`, `semantic_min`, `semantic_max`, and `range_source: "known_protocol"`. GIP triggers retain 16-bit unsigned storage (`0..65535`) and semantic range `0..1023`; stick axes retain signed 16-bit ranges. Original `bit_size` and `logical_min`/`logical_max` properties remain compatible. A protocol definition also identifies `definition_source: "known_protocol_definition"` and `definition_authority: "protocol_semantics"`, independently of its association with a physical interface.

Uninterpreted regions and `unknown_extensions` carry `semantic_type: "unknown_protocol_extension"` and `preserve: true`. Existing `preserve_uninterpreted_bytes` and `uninterpreted_tail_start_byte` properties remain. These are preservation rules for downstream packet processing; the extractor does not fabricate Share, paddle, or firmware-specific tail meanings.

## DualSense USB semantic fields

For a matching parsed DualSense-compatible Input layout, the resolved physical interface is selected at priority 1. Neither VID/PID nor a fixed interface number selects this family. The Input selector remains Report ID `0x01`, exactly 64 wire bytes, with a one-byte Report ID header. `protocol_layout.reports[].fields` contains the known semantics; interface `reports`, `collections`, and `raw_report_descriptor` retain the original descriptor interpretation and exact physical bytes. The semantic overlay does not rewrite vendor-defined descriptor fields, combine Report IDs, or replace Windows diagnostic evidence.

Normalized button roles retain their existing names and add `native_name`: `button_west/south/east/north` map to `square/cross/circle/triangle`; shoulders and trigger buttons map to `l1/r1/l2_button/r2_button`; `view/menu` map to `create/options`; stick clicks map to `l3/r3`; and `guide` maps to `ps_home`. Touchpad click and microphone mute preserve both names.

All offsets below count from wire byte zero, including the Report ID. The sequence byte is at 7. Gyroscope X/Y/Z occupy signed little-endian 16-bit fields at bytes 16/18/20; accelerometer X/Y/Z use the same representation at 22/24/26. The unsigned little-endian 32-bit sensor timestamp starts at 28. Sensor values are raw counts without calibration; the timestamp remains raw ticks without conversion to a time unit.

`touch_point_1` and `touch_point_2` are four-byte packed structures at bytes 33 and 37. Each contains a `fields` object with complete `contact_id`, `active`, `x`, and `y` layouts. Their contact-relative bit offsets are 0, 7, 8, and 20, with widths 7, 1, 12, and 12. Absolute wire and report-payload offsets are also exported. The active flag has `value_encoding: "active_low"`, `true_raw_value: 0`, and `false_raw_value: 1`; interpret it before checking the parent's `data_valid_when`. Preserve the four original bytes even when a contact is inactive. Both coordinates retain unsigned 12-bit storage `0..4095`; the known 1920-by-1080 touchpad gives X semantic range `0..1919` and Y semantic range `0..1079`. Raw values outside those semantic ranges remain available without clipping or coordinate scaling.

| Touch coordinate | Wire storage | Semantic range |
| --- | --- | --- |
| X, either contact | Unsigned 12-bit, `0..4095` | `0..1919` |
| Y, either contact | Unsigned 12-bit, `0..4095` | `0..1079` |

Byte 53 contains the low-nibble `battery_level` and high-nibble `charging_status`. Both retain raw four-bit protocol codes; no percentage, charging-state label, or reserved-value meaning is invented. Byte 54 (`status[1]`) exports `headphone_detect`, `microphone_detect`, and `microphone_mute_status` at wire bits 432, 433, and 434, each an unsigned one-bit status with range `0..1`. The mute status is separate from the `microphone_mute` button. Bits 3..7 of byte 54, byte 55, and the remaining unmapped/reserved tail stay in `uninterpreted_regions` with `preserve: true`. Those regions and the known top-level fields cover every payload bit exactly once.

`basic_input_semantics_complete` and `extended_input_semantics_complete` are separate at profile and Input-report level. Extended completeness covers the common fields described above, including both structured touch contacts, battery/charging codes, headphone/microphone detection, and microphone mute status; it does not claim complete knowledge of every firmware-specific or Edge-only field. Basic firmware readiness does not depend on the extended completeness flag. `eligible_for_firmware_parser_generation` follows physical Input readiness and remains false for unbound definitions and Windows logical views.

`protocol_layout.output_reports` describes the known USB Output transport separately. Its Report ID `0x02` entry exports `descriptor_output_wire_bytes` from the retained descriptor and `known_protocol_output_wire_bytes: 63`. The captured descriptor still reports 48 Output wire bytes in the interface's raw `reports` entry. When no descriptor is available, `descriptor_output_wire_bytes` is `null` rather than an invented length. Both lengths include the Report ID. `output_semantics_complete` remains false, the Output semantic field list is empty, and Output parser-generation eligibility is false. Rumble, adaptive-trigger, LED, speaker, mute, and Feature semantics are not guessed.

| DualSense report representation | Wire bytes, including Report ID | Export meaning |
| --- | --- | --- |
| USB Input `0x01` | 64 | Known basic and common extended input fields |
| Captured HID descriptor Output `0x02` | 48 | Original descriptor-defined length, retained unchanged |
| Known USB protocol Output `0x02` | 63 | Separate transport-length metadata; internal output semantics are incomplete |

## Validation scope

Layout exports describe the evidence available for each interface and report. A live Windows API validation result does not establish live sensor accuracy, generated-firmware execution, on-device injection, Edge-specific behavior, Bluetooth compatibility, or implementation of controller Output/Feature effects. Inspect the resulting JSON/TXT evidence for the connected device before consuming its layout. This utility does not record live button presses or controller input packets.

## Implementation references

The executable embeds the applicable third-party notice and complete BSD license, available with `--licenses`. The release ZIP includes `THIRD_PARTY_NOTICES.md` and `LICENSES/HIDAPI-BSD.txt`. No external HIDAPI library is loaded.

- [Pinned HIDAPI representation and license notices](THIRD_PARTY_NOTICES.md)
- [Windows PPD schema extension](SCHEMA.md)
- [HidD_GetPreparsedData](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidsdi/nf-hidsdi-hidd_getpreparseddata)
- [GetRawInputDeviceInfoW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getrawinputdeviceinfow)
- [HidP_InitializeReportForID](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidpi/nf-hidpi-hidp_initializereportforid)

- [USB-IF HID Usage Tables 1.7, Consumer Page, section 15](https://usb.org/sites/default/files/hut1_7.pdf)
- [Windows HID button capabilities](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidpi/ns-hidpi-_hidp_button_caps)
- [Windows HID value capabilities](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidpi/ns-hidpi-_hidp_value_caps)
- [HidP_SetUsages](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidpi/nf-hidpi-hidp_setusages)
- [HidP_SetUsageValueArray](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/hidpi/nf-hidpi-hidp_setusagevaluearray)
- [Linux Xbox packet handling](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c)
- [xone GIP envelope handling](https://github.com/medusalix/xone/blob/master/bus/protocol.c)
- [Linux PlayStation USB report structures](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c)
- [Linux DualSense common USB sensor, touch and status layouts](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c)
- [SDL DualShock 3 packet handling](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps3.c)
