#pragma once

struct ControllerProfileMatch {
    bool matched = false;
    std::string status = "no_profile";
    std::vector<std::string> reasons;
};

static bool sony_profile_structure(const InterfaceExtraction& row, std::vector<std::string>& reasons) {
    if (row.protocol.name.find("dual") != 0) return true;
    if (!row.parsed.available || !row.parsed.errors.empty()) {
        reasons.push_back("A parsed HID Input layout is required to bind Sony semantic offsets");
        return false;
    }
    const bool ds3 = row.protocol.name == "dualshock3_usb";
    const bool ds4 = row.protocol.name == "dualshock4_usb";
    const auto* report = descriptor_report_by_id(&row.parsed, 1);
    if (!report || report->input_wire_bytes != (ds3 ? 49u : 64u)) {
        reasons.push_back("Sony Input report 1 has an absent or incompatible wire length");
        return false;
    }
    if (report->ppd_reconstructed && (!report->wire_length_exact[0] || !report->reconstruction_complete[0])) {
        reasons.push_back("Sony Input PPD framing or interpretation is incomplete");
        return false;
    }
    std::set<int> roots;
    for (const auto& field : report->fields)
        if (field.report_type == "Input" && !field.is_constant && !field.uninterpreted_storage)
            roots.insert(field.top_level_collection_ordinal);
    if (roots.size() != 1) {
        reasons.push_back("Sony Input must resolve to one controller collection");
        return false;
    }
    const int root = *roots.begin();
    const bool controller = std::any_of(row.parsed.collections.begin(), row.parsed.collections.end(), [&](const ParsedCollection& c) {
        return c.ordinal == root && !c.parent_ordinal && c.usage_page == 1 && (c.usage == 4 || c.usage == 5);
    });
    if (!controller) {
        reasons.push_back("The matched Sony Input collection is not a gamepad or joystick");
        return false;
    }
    auto anchor = [&](int page, int usage, uint32_t offset, uint32_t width, int64_t maximum, bool null_state = false) {
        size_t matches = 0;
        for (const auto& field : report->fields) {
            if (field.report_type != "Input" || field.top_level_collection_ordinal != root || field.is_constant ||
                field.uninterpreted_storage || !field.is_variable || field.is_relative || field.is_signed ||
                !field.interpretation_complete || field.usage_page != page || field.usage != usage ||
                field.wire_bit_offset != offset || field.bit_size != width || field.report_count != 1 ||
                field.logical_min != 0 || field.logical_max != maximum || field.has_null_state != null_state) continue;
            ++matches;
        }
        if (matches != 1) reasons.push_back("Sony Input anchor mismatch: " + usage_name(page, usage) + " at wire bit " + std::to_string(offset));
    };
    const int axes[] = {0x30, 0x31, 0x32, 0x35};
    for (uint32_t i = 0; i < 4; ++i) anchor(1, axes[i], ((ds3 ? 6u : 1u) + i) * 8, 8, 255);
    if (ds3) {
        for (uint32_t i = 0; i < 17; ++i) anchor(9, static_cast<int>(i + 1), 16 + i, 1, 1);
    } else {
        anchor(1, 0x33, (ds4 ? 8u : 5u) * 8, 8, 255);
        anchor(1, 0x34, (ds4 ? 9u : 6u) * 8, 8, 255);
        const uint32_t buttons = (ds4 ? 5u : 8u) * 8;
        anchor(1, 0x39, buttons, 4, 7, true);
        for (uint32_t i = 0; i < (ds4 ? 14u : 15u); ++i) anchor(9, static_cast<int>(i + 1), buttons + 4 + i, 1, 1);
        if (!ds4) anchor(0xff00, 0x20, 56, 8, 255);
    }
    for (const auto& field : report->fields) {
        if (field.report_type != "Input") continue;
        if (uint64_t(field.wire_bit_offset) + uint64_t(field.bit_size) * field.report_count > uint64_t(report->input_wire_bytes) * 8)
            reasons.push_back("A Sony Input field exceeds its declared report boundary");
    }
    return reasons.empty();
}

static ControllerProfileMatch controller_profile_match(const InterfaceExtraction& row) {
    ControllerProfileMatch result;
    const auto& profile = row.protocol;
    if (profile.name.empty()) return result;
    result.status = "rejected";
    auto reject = [&](const std::string& reason) { result.reasons.push_back(reason); };
    if (profile.reports.empty()) reject("No applicable Input selector remains in this profile");
    if (row.descriptor.alternate_setting != 0 || row.descriptor.interface_number < 0) reject("The interface/alternate setting is not supported by this profile");
    if ((row.physical_vid && profile.source_vid != row.physical_vid) || (row.physical_pid && profile.source_pid != row.physical_pid))
        reject("The profile candidate identity differs from the physical device");
    const bool gip = profile.name == "xbox_gip";
    const bool xusb = profile.name == "xbox_xusb";
    const bool wireless = profile.name == "xbox_360_wireless";
    const bool sony = profile.name.find("dual") == 0;
    if (gip || xusb || wireless) {
        if (row.descriptor.interface_class != 0xff || row.descriptor.interface_subclass != (gip ? 0x47 : 0x5d) ||
            row.descriptor.interface_protocol != (gip ? 0xd0 : wireless ? 0x81 : 1)) reject("The physical interface signature contradicts the Xbox transport");
        if (gip && row.descriptor.interface_number != 0) reject("GIP data profiles require interface 0");
        if (row.physical_hid) reject("A proprietary Xbox data interface cannot be treated as HID");
    } else if (sony) {
        if (profile.source_vid != 0x054c || !row.physical_hid || row.descriptor.interface_class != 3 ||
            row.descriptor.interface_subclass != 0 || row.descriptor.interface_protocol != 0)
            reject("Sony USB semantics require a matching non-boot physical HID interface");
        const bool identity = (profile.name == "dualshock3_usb" && profile.source_pid == 0x0268) ||
            (profile.name == "dualshock4_usb" && (profile.source_pid == 0x05c4 || profile.source_pid == 0x09cc)) ||
            (profile.name == "dualsense_usb" && profile.source_pid == 0x0ce6) ||
            (profile.name == "dualsense_edge_usb" && profile.source_pid == 0x0df2);
        if (!identity) reject("The Sony family/variant does not match its candidate identity");
        sony_profile_structure(row, result.reasons);
        if (row.using_ppd) {
            bool validated = false;
            for (const auto& report : row.ppd.report_states)
                if (report.type == 0 && report.report_id == 1 && ppd_report_operation_ready(report) &&
                    !report.shared_layout_conflict && !row.conflicting_reports.count({0,1})) validated = true;
            if (!validated) reject("The Sony PPD Input reconstruction has no passing report-specific Windows validation");
        } else if (!row.exact || !row.wire_sizes_exact) reject("Sony physical binding requires an exact descriptor or validated PPD Input layout");
    } else reject("Unknown controller profile family");
    std::set<uint8_t> addresses;
    unsigned inputs = 0, outputs = 0;
    for (const auto& endpoint : row.descriptor.endpoints) {
        const bool input = (endpoint.address & 0x80) != 0;
        if (!(endpoint.address & 15) || (endpoint.address & 0x70) || !addresses.insert(endpoint.address).second ||
            endpoint.direction != (input ? "IN" : "OUT") || endpoint.transfer_type != "Interrupt") {
            reject("The controller endpoint topology is invalid or is not an interrupt pair");
            continue;
        }
        const uint32_t packet = endpoint.max_packet_size;
        if ((gip || sony) ? packet != 64 : packet < (wireless ? 24u : 20u) || packet > 64)
            reject("The interrupt endpoint capacity is incompatible with the controller transport");
        if (input) ++inputs; else ++outputs;
    }
    if (inputs != 1 || outputs != 1 || row.descriptor.endpoints.size() != 2) reject("A single interrupt IN/OUT pair is required");
    result.matched = result.reasons.empty();
    if (result.matched) result.status = "matched";
    return result;
}
