# Windows PPD schema extension

The export remains `schema: "makcu-input-extraction"`, `version: 4`. `schema_extension: "windows-ppd-v1"` adds preparsed-data evidence and report-level selection. Existing physical descriptor bytes, generic semantics and known controller protocols remain available. This document describes the extension; [README.md](README.md) describes the underlying semantic schema.

## Evidence and source identity

`interfaces[].windows_hid_collections[].preparsed_data` records:

| Property | Meaning |
| --- | --- |
| `preparsed_data_source` | `live_hid_api_collection_descriptor`, `raw_input_api`, or `saved_artifact`; null when unavailable |
| `preparsed_data_length` | Actual independently acquired byte count |
| `length_source` | Collection size/returned bytes, Raw Input size/returned bytes, or exact binary file length |
| `collection_match` | Opened collection handle, exact device-interface path, or matching collection name plus the same resolved Windows device instance |
| `sidecar_file` | Filename of the corresponding live `.ppd` capture; null for imports |
| `ppd_decoder_format` | `hidp-kdr-header44-cap104-link16-le-v1` on a supported decode; otherwise null |
| `reference_revision` | HIDAPI reference commit `852cc68b8e0e4c7eba0815ef992951cd8b75c55b` |
| `capture_os_version`, `hid_api_version` | Observed capture metadata; null when unknown |
| `supported` | Decoder recognized and safely decoded this representation, not universal Windows-version compatibility |
| `descriptor_exact`, `physical_capture_verified` | False: PPD recovery does not recover the exact original descriptor or capture physical traffic |
| `decoded_section_bytes`, `trailing_bytes` | Bounded decoded extent and remaining preserved bytes |
| `acquisition_diagnostics` | Errors retained from acquisition attempts, including unsuccessful primary acquisition before fallback |
| `diagnostics` | Severity, code, byte offset and explanation for malformed, unsupported or incomplete interpretation |
| `records` | Original record indices/offsets, type/ID, storage extents, flags, raw range words, signed/unsigned union views and units |
| `report_validation` | Independent Windows-oracle or offline-record evidence for each report type and ID |

`cached_collection_descriptor_hex` preserves all acquired PPD bytes, including trailing data. It is not a HID report descriptor. A direct PPD layout therefore has `raw_report_descriptor.available: false`, `raw_report_descriptor.exact: false` and `layout_source: "windows_preparsed_data"` unless an independent exact descriptor is present and preferred.

Original descriptors and PPD may coexist: the descriptor supplies the primary layout, while PPD and its Windows validation remain collection-level evidence. Successful decode alone does not authorize physical packet interpretation.

## Offset and length domains

All bit numbering is least-significant-bit first within a byte.

| Field | Convention |
| --- | --- |
| `windows_api_bit_offset` | Offset in a Windows HID API report buffer, which always reserves its first byte for the Report ID |
| `payload_bit_offset` | Offset after removing that Windows prefix byte |
| `wire_bit_offset` | Candidate HID wire offset: payload offset plus eight for a nonzero Report ID, or unchanged for ID zero |
| `bit_offset` | Compatibility alias of `wire_bit_offset` |

For every PPD field, `windows_api_bit_offset = payload_bit_offset + 8`. For Report ID zero, `wire_bit_offset = payload_bit_offset`; the zero prefix used by Windows is not transmitted as a HID Report ID byte. On an unbound or logical collection, wire offsets remain candidate reconstruction offsets rather than verified USB positions.

`reports[].length_evidence[]` separates Input, Output and Feature even when they share an ID:

| Property | Meaning |
| --- | --- |
| `windows_api_report_byte_length` | Collection-wide maximum Windows buffer length for that report type |
| `wire_bytes_lower_bound` | Byte extent established by the decoded fields and prefix convention |
| `exact_wire_bytes` | Candidate length only when the recovered extent reaches the Windows maximum after removing an artificial ID-zero prefix; otherwise null |
| `wire_sizes_exact` | Whether that exact candidate length was established; not a physical-traffic verification flag |
| `reconstruction_complete` | Supported interpretation of occupied records for that type/ID; does not imply knowledge of opaque storage |

The older `input_wire_bytes`, `output_wire_bytes` and `feature_wire_bytes` contain recovered extents on PPD layouts. Consult `length_evidence` before treating them as complete lengths. A nonzero Windows Feature maximum with no occupied Feature records yields an unknown-ID validation/readiness record, not an invented report or fields. Endpoint maximum packet size is never substituted for a report's length.

## Fields, arrays and unknown storage

PPD fields use the existing generic semantic classifiers and usage dictionaries. Generic HID evidence does not assign PlayStation/Xbox button aliases, trigger roles or left/right-stick identities. Known controller profiles remain a separate layer.

Array `report_count` counts stored selector slots, not available usages. `usage_ranges` retain page, usage endpoints and explicit selector endpoints. Multiple usage groups may share one storage array. `usage_aliases` describes alternative names for that same storage; it does not allocate duplicate fields. Signed selectors require sign extension before lookup. Repeated scalar values remain multi-element value fields and must be read or written slot by slot.

`ppd_source_records` and `ppd_link_collection` connect normalized fields to raw records and collection nodes. `interpretation_complete` and `selector_mapping_resolved` remain false when an alias or selector domain is unresolved. Reserved flags and unknown global tokens are preserved without authorizing a guessed interpretation.

Unassigned variable elements, gaps and rounded storage bits use `semantic_name: "uninterpreted_storage"`, `preserve: true`, `padding_proven: false` and an unknown storage interpretation. They are not relabeled as Constant fields. Actual Constant fields and unknown storage have range validation marked not applicable.

Original range values and raw union words remain available, including impossible maxima. Bitmap semantics can independently establish a Boolean range. Arrays with verified selectors can be usable even when the raw union's declared logical maximum is inconsistent; use their explicit selector mapping, not that inconsistent maximum, to encode usage values. Scalar scaling must not use a range whose `range_authoritative_for_physical_wire` is false.

`windows_logical_oracle_validated` describes validation of the field's report against trusted Windows operations. `physical_report_eligible` additionally requires physical identity, framing and conflict checks. `range_authoritative_for_physical_wire` also requires a meaningful, representable field range. A passing report cannot confer these physical flags on another report.

## Windows in-memory validation

`report_validation[]` is keyed by `report_type` and `report_id`; ID is null when no occupied record establishes it. It exports pass/fail/skip counts, fields checked/expected, metadata agreement, diagnostic outcomes and `windows_logical_validation_passed`.

`verification_source: "windows_in_memory_oracle"` means checks were attempted using original API-owned PPD from the live collection. It does not mean they passed. `report_buffer_ready` records successful preparation of a type/ID-scoped in-memory test buffer and is required for a Windows layout-validation pass. It does not establish field coverage, physical framing or whole-packet generation. Invalid metadata, report identity, field bounds, initialization statuses or initial data leave the buffer unready and discarded. Field getters must still reject incompatible IDs and short buffers.

Newly generated `initialization_method` values are `typed_zero_payload_report_id_and_HidP_SetUsageValue`, `typed_zero_payload_report_id_and_HidP_SetUsageValueArray`, `HidP_InitializeReportForID_checked_zero_null`, or null when no accepted preparation exists. The typed setter path is the primary path for complete, reconciled, non-null scalar/value-array storage. It validates report membership and field extents, seeds the declared ID, and requires a successful setter that preserves the ID, zero payload and buffer boundary. It does not call the native initializer. For button/selector or null-state storage, the native initializer must succeed and its result must contain inactive button usages and zero/nonactive-null value data for the requested report. Neither path can manufacture field coverage.

`native_initializer_called` identifies whether report preparation actually invoked `HidP_InitializeReportForID`. `initializer_status` is null when it did not; a null status is not a native success. `initialization_setter_status` likewise remains null unless a preparation setter ran. `initializer_checks_passed`, `initializer_checks_failed` and `initializer_diagnostics` retain actual native preparation checks and other preparation diagnostics separately from layout `checks_passed`, `checks_failed` and `checks_skipped`. Older version-4 exports can carry the previous native/fallback method names; those historical results must not be reinterpreted as execution of the corrected preparation path.

Each collection's `preparsed_data.native_initializer_audit` has one entry per report type. `executed`, `windows_api_report_byte_length`, `checks_passed`, `checks_failed` and `failures` describe a separate native API conformance audit. When live public capabilities are complete, each audit tests all 256 IDs and one short buffer once, rather than duplicating an absent-ID test across all reports. A failure records its test, report ID, declared membership in that type, buffer length, expected and actual statuses, returned ID and boundary preservation. Unexpected acceptance of another type's ID and rejection of a declared ID both remain failures. `scope` is `once_per_collection_report_type`; `affects_layout_readiness` is false because actual preparation and all field comparisons are independently required. An unavailable/offline audit has `executed: false`, zero counts and an empty failure list.

Successful preparation never counts as field coverage. All independent getter/setter, slot, ID, length and unrelated-bit checks remain mandatory. When no preparation path succeeds, the layout retains a failed check, a dependent-comparison skip and its nonzero expected-field count. Imported or mutated PPD bytes are never passed to Windows.

Oracle buffers stay in process memory. Comparisons cover capability/link agreement, initialization, scalar and repeated-value setters/getters, usage sets, aliases, individual array slots, simultaneous selectors, varying values across repeated slots, incompatible IDs, short lengths and preservation of unrelated bits. API width/work limits generate skips. No `WriteFile`, Output report or Feature report is sent to the controller by this validation path.

`verification_source: "offline_record_checks"` means only the bounded decoder processed saved data. Oracle counts remain zero and Windows logical validation stays false. Synthetic counters used in policy unit tests are explicitly labeled in their test-only root object and never describe real Windows validation.

## Physical selection and readiness

Consume `firmware_layout_selection.selected_reports`, not an interface-wide ready flag. Each selected entry carries physical device index, configuration, interface, alternate setting, collection identity/ordinal, type, ID, optional protocol selector, priority, a report-level JSON pointer and generation eligibility.

For a generic report, apply `report_type` and `top_level_collection_ordinal` when traversing the target's fields. A single report object can include several types or collections. For a protocol report, enforce the selected protocol's packet conditions and any framing/reassembly requirements.

Selection prefers an applicable known physical protocol, then an exact physical descriptor, then a validated physically correlated reconstruction. Lower-priority reports are removed only when the higher-priority selection covers the same type/ID and relevant collection fields. Independent or partially covered collections remain selected. Conflicting physical reconstructions of the same storage block the affected report type/ID in both collections. The same ID on another report type or physical unit is independent.

The legacy `selected_layouts` array preserves earlier broad Input/interface selection for compatibility. `selected_layouts_scope` labels it as a legacy summary. It can omit independent reports retained by `selected_reports`; new consumers must use the latter.

PPD interfaces add `report_readiness[]`, `readiness_scope: "per_report_modify_existing_base"` and `whole_packet_generation_ready: false`. Interface-level `firmware_layout_ready` remains a summary of usable Input support. A valid Output can still be individually selected when that summary is false. Each PPD report requires supported interpretation, exact candidate framing, complete trusted Windows field coverage without failures, a physical HID association and no relevant shared-layout or known Output-length conflict.

Every selected PPD report requires a matching live base report. Modify only defined, eligible fields and preserve all other bytes. Do not synthesize a packet by zero-filling opaque regions or replacing a shorter per-ID length with a type-wide maximum. Windows logical validation can pass while physical eligibility remains false.

Windows IG/logical children retain their actual ancestor interface association. A matching VID/PID, one available HID sibling or a known controller map is insufficient to promote a proprietary logical child into physical HID. In particular, an IG child below an XUSB interface must not inherit a neighboring HID interface's endpoints or parser.

The DualSense Input layer remains independent of generic Output/Feature evidence. Its 48-byte descriptor Output and 63-byte known USB Output form stay separate; the shorter generic Output is not selected as the known physical Output packet. Undecoded output effects remain unsupported.

## Controller profile matching

Every interface exports `controller_profile_match` with policy `usb-controller-match-v2`, `candidate`, `selection_basis`, `vid_pid_used_for_family_selection`, `status` (`no_profile`, `matched` or `rejected`), `definition_matches`, `physical_binding_verified`, `device_authentication_verified` and `reasons`. `selection_basis` is `parsed_hid_input_layout` for Sony-compatible candidates, `usb_interface_signature` for Xbox candidates, or null without a recognized candidate. `vid_pid_used_for_family_selection` is always false. A structural definition match alone cannot grant physical authority to an offline or logical view. `physical_binding_verified` additionally requires a resolved physical device, supported scope, configuration and alternate setting. `device_authentication_verified` is always false.

Sony-compatible family selection runs after HID parsing and requires exactly one matching Input layout; VID/PID is retained only as device identity, never as a family allowlist. The Input layout must contain a single controller collection with the expected report ID, length and control anchors, including offsets, widths, usage identities, ranges and relevant flags. Physical binding additionally checks the non-boot HID interface, interrupt IN/OUT pair and endpoint capacities. DS4 and DualSense cannot cross-match merely because both use 64-byte Input reports. A PPD reconstruction must also have exact Input framing, complete interpretation and passing report-specific Windows validation. GIP/XUSB matching checks the physical vendor-interface signature and interrupt pair; GIP data is limited to interface 0. Third-party identities remain eligible for every family when their layout and transport match. Missing or incompatible Input evidence retains generic HID fields without assigning a family from identity.

Matching standard DualSense, Edge and third-party common Input layouts all export `dualsense_usb`. The earlier PID-selected `dualsense_edge_usb` label is no longer emitted: the common layout cannot establish the physical variant, and variant-specific controls remain uninterpreted. The original VID/PID, report descriptors and other report IDs remain available in their existing identity/raw-evidence fields.

Descriptor/Windows comparisons export `constant_elements_checked` separately from `observed_fields_checked`. Constant storage is compared for type, ID, position and extent without applying inherited data ranges or usages. Repeated value observations validate every slot. A real control mismatch still rejects its affected report, while another report type/ID remains independent. TXT carries the same Constant count and profile-match reasons.

## Offline import

`--ppd-file` is exclusive with live VID/PID selection and `--list`. It uses the exact binary file length and only the bounded decoder. Exports contain `offline: true`, `device: null`, no physical devices, `layout_scope: "offline_ppd_import"`, `transport: null`, unknown capture OS/API metadata and empty firmware selections. `import_metadata` identifies the file while keeping identity-origin and physical-capture verification false.

Supported import returns exit code zero even though physical readiness is false. Unsupported format returns one after emitting diagnostics. Argument or I/O errors return two. A successful live extraction likewise does not imply that every recovered layout is ready.

## Representation and resource limits

The decoder requires the supported 44-byte header, 104-byte occupied capability records and 16-byte link-node representation, with the link section relative to the capability base. It validates section capacities independently of occupied records, all bounded field spans, parent/child/sibling topology and storage overlaps. Unused capability capacity never becomes a fabricated report.

Limits are 1 MiB per raw blob, 65,536 normalized fields, 4,096 collection nodes and 262,144 expanded usage/alias/source-record metadata entries. Explicit arithmetic and bounds checks precede reads, writes and expansions. Unsupported representations fail closed for direct recovery; diagnostic raw data and any separately available existing descriptor/capability evidence remain distinguishable.

Detailed validation records and captures remain local. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the pinned representation reference and retained license.
