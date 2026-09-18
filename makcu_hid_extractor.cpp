#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cfgmgr32.h>
#include <usbioctl.h>
#include <hidclass.h>

#include <algorithm>
#include <array>
#include <type_traits>
#include <cctype>
#include <cwctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>
#include "third_party_notices.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "user32.lib")

#ifndef FILE_DEVICE_KEYBOARD
#define FILE_DEVICE_KEYBOARD 0x0000000B
#endif
#ifndef FILE_DEVICE_USB
#define FILE_DEVICE_USB 0x00000022
#endif
#ifndef USB_GET_HUB_INFORMATION_EX
#define USB_GET_HUB_INFORMATION_EX 277
#endif
#ifndef IOCTL_USB_GET_HUB_INFORMATION_EX
#define IOCTL_USB_GET_HUB_INFORMATION_EX CTL_CODE(FILE_DEVICE_USB, USB_GET_HUB_INFORMATION_EX, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_HID_GET_COLLECTION_INFORMATION
#define IOCTL_HID_GET_COLLECTION_INFORMATION CTL_CODE(FILE_DEVICE_KEYBOARD, 106, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_HID_GET_COLLECTION_DESCRIPTOR
#define IOCTL_HID_GET_COLLECTION_DESCRIPTOR CTL_CODE(FILE_DEVICE_KEYBOARD, 100, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif

static const GUID GUID_DEVINTERFACE_USB_HUB_CONST = {
    0xF18A0E88, 0xC30C, 0x11D0, {0x88, 0x15, 0x00, 0xA0, 0xC9, 0x06, 0xBE, 0xD8}
};
static const GUID GUID_DEVINTERFACE_USB_DEVICE_CONST = {
    0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}
};
static const GUID GUID_DEVINTERFACE_USB_HOST_CONTROLLER_CONST = {
    0x3ABF6F2D, 0x71C4, 0x462A, {0x8A, 0x92, 0x1E, 0x68, 0x61, 0xE6, 0xAF, 0x27}
};

static std::string to_hex(const uint8_t* data, size_t len) {
    static const char hex_digits[] = "0123456789ABCDEF";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(hex_digits[(data[i] >> 4) & 0xF]);
        s.push_back(hex_digits[data[i] & 0xF]);
    }
    return s;
}

static std::string hex_bytes_spaced(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        if (!out.empty()) out.push_back(' ');
        out.push_back(hex[i]);
        out.push_back(hex[i+1]);
    }
    return out;
}

static std::string escape_json(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (static_cast<unsigned char>(c) < 32) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
            out += buf;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string s(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &s[0], size, nullptr, nullptr);
    return s;
}

static std::string str_toupper(std::string s) {
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static std::string str_tolower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static HANDLE open_handle_w(const std::wstring& path, DWORD access = 0) {
    HANDLE h = CreateFileW(
        path.c_str(),
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    return h;
}

static std::string win32_error_text(DWORD code) {
    char buf[512] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, sizeof(buf), nullptr);
    std::string msg = buf;
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' '))
        msg.pop_back();
    char out[600];
    snprintf(out, sizeof(out), "Win32 error %lu: %s", code, msg.c_str());
    return out;
}

struct DeviceInterfaceEntry {
    std::wstring path;
    std::wstring instance_id;
    std::wstring parent_instance_id;
    std::vector<std::wstring> ancestor_instance_ids;
    std::string device_description;
    std::string manufacturer_property;
    std::string friendly_name;
    std::vector<std::string> hardware_ids;
    std::string location_information;
    std::string driver_key;
    uint32_t address_property = 0;
    bool has_address = false;
};

static std::vector<std::wstring> devinst_ancestors(DWORD devinst, int max_depth = 16) {
    std::vector<std::wstring> ancestors;
    DWORD current = devinst;
    std::set<DWORD> seen;
    for (int i = 0; i < max_depth; ++i) {
        DWORD parent = 0;
        if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS) break;
        if (seen.count(parent)) break;
        seen.insert(parent);
        wchar_t buf[4096] = {0};
        if (CM_Get_Device_IDW(parent, buf, 4096, 0) == CR_SUCCESS && buf[0]) {
            ancestors.push_back(buf);
        }
        current = parent;
    }
    return ancestors;
}

static void get_registry_property(HDEVINFO info_set, PSP_DEVINFO_DATA devinfo, DWORD prop, DeviceInterfaceEntry& entry) {
    BYTE buf[4096] = {0};
    DWORD reg_type = 0, needed = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(info_set, devinfo, prop, &reg_type, buf, sizeof(buf), &needed))
        return;
    DWORD size = (needed < sizeof(buf)) ? needed : sizeof(buf);
    if (reg_type == REG_SZ) {
        std::wstring ws(reinterpret_cast<wchar_t*>(buf), size / sizeof(wchar_t));
        while (!ws.empty() && ws.back() == L'\0') ws.pop_back();
        std::string s = wstring_to_utf8(ws);
        if (prop == SPDRP_DEVICEDESC) entry.device_description = s;
        else if (prop == SPDRP_MFG) entry.manufacturer_property = s;
        else if (prop == SPDRP_FRIENDLYNAME) entry.friendly_name = s;
        else if (prop == SPDRP_LOCATION_INFORMATION) entry.location_information = s;
        else if (prop == SPDRP_DRIVER) entry.driver_key = s;
    } else if (reg_type == REG_MULTI_SZ) {
        const wchar_t* p = reinterpret_cast<wchar_t*>(buf);
        const wchar_t* end = p + (size / sizeof(wchar_t));
        while (p < end && *p) {
            std::wstring item(p);
            entry.hardware_ids.push_back(wstring_to_utf8(item));
            p += item.size() + 1;
        }
    } else if (reg_type == REG_DWORD && size >= 4) {
        if (prop == SPDRP_ADDRESS) {
            entry.address_property = *reinterpret_cast<uint32_t*>(buf);
            entry.has_address = true;
        }
    }
}

static std::vector<DeviceInterfaceEntry> enumerate_device_interfaces(const GUID& interface_guid) {
    std::vector<DeviceInterfaceEntry> result;
    HDEVINFO info_set = SetupDiGetClassDevsW(&interface_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info_set == INVALID_HANDLE_VALUE || info_set == nullptr) return result;

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(info_set, nullptr, &interface_guid, index, &iface); ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(info_set, &iface, nullptr, 0, &required, nullptr);
        if (required < 4) continue;

        std::vector<BYTE> detail_buf(required);
        SP_DEVINFO_DATA devinfo;
        bool detail_ok = false;
        DWORD cb_sizes[] = { 8, 6, 5 };
        for (DWORD cb : cb_sizes) {
            *reinterpret_cast<DWORD*>(detail_buf.data()) = cb;
            devinfo.cbSize = sizeof(SP_DEVINFO_DATA);
            if (SetupDiGetDeviceInterfaceDetailW(info_set, &iface,
                    reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detail_buf.data()),
                    required, &required, &devinfo)) {
                detail_ok = true;
                break;
            }
            if (GetLastError() != 1784) break;
        }
        if (!detail_ok) continue;

        DeviceInterfaceEntry entry;
        wchar_t* pPath = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detail_buf.data())->DevicePath;
        entry.path = pPath;

        wchar_t inst_buf[4096] = {0};
        if (SetupDiGetDeviceInstanceIdW(info_set, &devinfo, inst_buf, 4096, nullptr)) {
            entry.instance_id = inst_buf;
        }
        entry.ancestor_instance_ids = devinst_ancestors(devinfo.DevInst);
        if (!entry.ancestor_instance_ids.empty())
            entry.parent_instance_id = entry.ancestor_instance_ids[0];

        get_registry_property(info_set, &devinfo, SPDRP_DEVICEDESC, entry);
        get_registry_property(info_set, &devinfo, SPDRP_MFG, entry);
        get_registry_property(info_set, &devinfo, SPDRP_FRIENDLYNAME, entry);
        get_registry_property(info_set, &devinfo, SPDRP_HARDWAREID, entry);
        get_registry_property(info_set, &devinfo, SPDRP_LOCATION_INFORMATION, entry);
        get_registry_property(info_set, &devinfo, SPDRP_ADDRESS, entry);
        get_registry_property(info_set, &devinfo, SPDRP_DRIVER, entry);

        result.push_back(entry);
    }
    SetupDiDestroyDeviceInfoList(info_set);
    return result;
}

static bool parse_vid_pid(const std::string& str, uint16_t& vid, uint16_t& pid) {
    std::string upper = str_toupper(str);
    size_t vpos = upper.find("VID_");
    if (vpos == std::string::npos) return false;
    size_t ppos = upper.find("PID_", vpos);
    if (ppos == std::string::npos) return false;
    if (vpos + 8 > upper.size() || ppos + 8 > upper.size()) return false;
    char vbuf[5] = {0}, pbuf[5] = {0};
    memcpy(vbuf, upper.c_str() + vpos + 4, 4);
    memcpy(pbuf, upper.c_str() + ppos + 4, 4);
    char* end = nullptr;
    vid = (uint16_t)strtoul(vbuf, &end, 16);
    if (end != vbuf + 4) return false;
    pid = (uint16_t)strtoul(pbuf, &end, 16);
    if (end != pbuf + 4) return false;
    return true;
}

static bool entry_vid_pid_check(const DeviceInterfaceEntry& entry, uint16_t& vid, uint16_t& pid) {
    if (parse_vid_pid(wstring_to_utf8(entry.path), vid, pid)) return true;
    if (parse_vid_pid(wstring_to_utf8(entry.instance_id), vid, pid)) return true;
    for (const auto& hid : entry.hardware_ids) {
        if (parse_vid_pid(hid, vid, pid)) return true;
    }
    return false;
}

static std::string hid_get_string(HANDLE h, BOOLEAN(__stdcall *func)(HANDLE, PVOID, ULONG)) {
    wchar_t buf[512] = {0};
    if (func(h, buf, sizeof(buf))) {
        while (wcslen(buf) > 0 && buf[wcslen(buf)-1] == L' ') buf[wcslen(buf)-1] = 0;
        return wstring_to_utf8(buf);
    }
    return "";
}

struct HidCollectionEntry {
    DeviceInterfaceEntry base;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint16_t bcd_device = 0;
    std::string manufacturer;
    std::string product;
    std::string serial_number;
    int interface_number = -1;
    int collection_hint = -1;
};

static std::vector<HidCollectionEntry> enumerate_hid_collections() {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    std::vector<DeviceInterfaceEntry> ifaces = enumerate_device_interfaces(hid_guid);
    std::vector<HidCollectionEntry> result;

    for (const auto& item : ifaces) {
        HidCollectionEntry entry;
        entry.base = item;
        uint16_t vid = 0, pid = 0;
        bool has_vid_pid = entry_vid_pid_check(item, vid, pid);

        HANDLE h = open_handle_w(item.path, 0);
        if (!h) {
            if (!has_vid_pid) continue;
            entry.vid = vid;
            entry.pid = pid;
            entry.manufacturer = item.manufacturer_property;
            entry.product = !item.friendly_name.empty() ? item.friendly_name : item.device_description;
        } else {
            HIDD_ATTRIBUTES attrs{};
            attrs.Size = sizeof(attrs);
            if (HidD_GetAttributes(h, &attrs)) {
                entry.vid = attrs.VendorID;
                entry.pid = attrs.ProductID;
                entry.bcd_device = attrs.VersionNumber;
                entry.manufacturer = hid_get_string(h, HidD_GetManufacturerString);
                entry.product = hid_get_string(h, HidD_GetProductString);
                entry.serial_number = hid_get_string(h, HidD_GetSerialNumberString);
            } else if (has_vid_pid) {
                entry.vid = vid;
                entry.pid = pid;
            } else {
                CloseHandle(h);
                continue;
            }
            if (entry.manufacturer.empty()) entry.manufacturer = item.manufacturer_property;
            if (entry.product.empty()) entry.product = !item.friendly_name.empty() ? item.friendly_name : item.device_description;
            CloseHandle(h);
        }

        std::string path_upper = str_toupper(wstring_to_utf8(item.path));
        size_t mi_pos = path_upper.find("MI_");
        if (mi_pos != std::string::npos && mi_pos + 5 <= path_upper.size()) {
            char buf[3] = { path_upper[mi_pos+3], path_upper[mi_pos+4], 0 };
            entry.interface_number = (int)strtoul(buf, nullptr, 16);
        } else {
            std::string inst_upper = str_toupper(wstring_to_utf8(item.instance_id));
            mi_pos = inst_upper.find("MI_");
            if (mi_pos != std::string::npos && mi_pos + 5 <= inst_upper.size()) {
                char buf[3] = { inst_upper[mi_pos+3], inst_upper[mi_pos+4], 0 };
                entry.interface_number = (int)strtoul(buf, nullptr, 16);
            }
        }
        size_t col_pos = path_upper.find("&COL");
        if (col_pos != std::string::npos && col_pos + 6 <= path_upper.size()) {
            char buf[3] = { path_upper[col_pos+4], path_upper[col_pos+5], 0 };
            entry.collection_hint = (int)strtoul(buf, nullptr, 16);
        }

        result.push_back(entry);
    }
    return result;
}

struct UsbDeviceEntry {
    DeviceInterfaceEntry base;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string manufacturer;
    std::string product;
};

static std::vector<UsbDeviceEntry> enumerate_usb_devices() {
    std::vector<DeviceInterfaceEntry> ifaces = enumerate_device_interfaces(GUID_DEVINTERFACE_USB_DEVICE_CONST);
    std::vector<UsbDeviceEntry> result;
    std::set<std::string> seen;

    for (const auto& item : ifaces) {
        uint16_t vid = 0, pid = 0;
        if (!entry_vid_pid_check(item, vid, pid)) continue;
        std::string key = str_tolower(wstring_to_utf8(!item.instance_id.empty() ? item.instance_id : item.path));
        if (seen.count(key)) continue;
        seen.insert(key);

        UsbDeviceEntry u;
        u.base = item;
        u.vid = vid;
        u.pid = pid;
        u.manufacturer = item.manufacturer_property;
        u.product = !item.friendly_name.empty() ? item.friendly_name : item.device_description;
        result.push_back(u);
    }
    return result;
}

int hub_port_count(HANDLE handle) {
    BYTE buf_ex[512] = {0};
    DWORD ret_ex = 0;
    if (DeviceIoControl(handle, IOCTL_USB_GET_HUB_INFORMATION_EX, buf_ex, sizeof(buf_ex), buf_ex, sizeof(buf_ex), &ret_ex, nullptr) && ret_ex >= 6) {
        uint16_t highest = *reinterpret_cast<uint16_t*>(buf_ex + 4);
        if (highest > 0) return highest;
    }
    BYTE buf[512] = {0};
    DWORD ret = 0;
    if (DeviceIoControl(handle, IOCTL_USB_GET_NODE_INFORMATION, buf, sizeof(buf), buf, sizeof(buf), &ret, nullptr) && ret >= 7) {
        uint8_t ports = buf[6];
        if (ports > 0) return ports;
    }
    return 32;
}

static bool hub_get_descriptor(HANDLE hHub, ULONG port, UCHAR bmRequest, UCHAR bRequest, USHORT wValue, USHORT wIndex, USHORT length, std::vector<uint8_t>& out_data, DWORD& out_err) {
    out_data.clear();
    out_err = 0;
    if (length == 0 || length > 65535) return false;
    size_t header_size = sizeof(USB_DESCRIPTOR_REQUEST);
    std::vector<BYTE> buffer(header_size + length);
    auto* req = reinterpret_cast<PUSB_DESCRIPTOR_REQUEST>(buffer.data());
    req->ConnectionIndex = port;
    req->SetupPacket.bmRequest = bmRequest;
    req->SetupPacket.bRequest = bRequest;
    req->SetupPacket.wValue = wValue;
    req->SetupPacket.wIndex = wIndex;
    req->SetupPacket.wLength = length;

    DWORD returned = 0;
    SetLastError(0);
    if (!DeviceIoControl(hHub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buffer.data(), (DWORD)buffer.size(), buffer.data(), (DWORD)buffer.size(), &returned, nullptr) || returned <= header_size) {
        out_err = GetLastError();
        return false;
    }
    size_t actual = (std::min)(static_cast<size_t>(length), static_cast<size_t>(returned - header_size));
    out_data.assign(buffer.data() + header_size, buffer.data() + header_size + actual);
    return true;
}

static void pack_int_to_buf(std::vector<uint8_t>& out, int32_t val, bool signed_val) {
    if (signed_val) {
        if (val >= -128 && val <= 127) {
            out.push_back(static_cast<uint8_t>(val & 0xFF));
        } else if (val >= -32768 && val <= 32767) {
            out.push_back(static_cast<uint8_t>(val & 0xFF));
            out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        } else {
            out.push_back(static_cast<uint8_t>(val & 0xFF));
            out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        }
    } else {
        uint32_t u = static_cast<uint32_t>(val);
        if (u <= 0xFF) {
            out.push_back(static_cast<uint8_t>(u));
        } else if (u <= 0xFFFF) {
            out.push_back(static_cast<uint8_t>(u & 0xFF));
            out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
        } else {
            out.push_back(static_cast<uint8_t>(u & 0xFF));
            out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
        }
    }
}

static void add_hid_item(std::vector<uint8_t>& out, uint8_t prefix, int32_t val, bool signed_val = false) {
    std::vector<uint8_t> data;
    pack_int_to_buf(data, val, signed_val);
    uint8_t size_code = (uint8_t)data.size();
    if (size_code == 4) size_code = 3;
    out.push_back((prefix & 0xFC) | size_code);
    out.insert(out.end(), data.begin(), data.end());
}

struct UsageRange {
    int usage_page = 0;
    int minimum = 0;
    int maximum = 0;
    bool explicit_selector = false;
    int64_t selector_min = 0;
};

struct ParsedCollection {
    int ordinal = 0;
    int parent_ordinal = 0;
    int top_level_ordinal = 0;
    uint32_t collection_type = 0;
    int usage_page = 0;
    int usage = 0;
};

struct ParsedField {
    std::string report_type;
    uint8_t report_id = 0;
    int usage_page = 0;
    int usage = 0;
    uint32_t payload_bit_offset = 0;
    uint32_t wire_bit_offset = 0;
    uint32_t bit_size = 0;
    uint32_t report_count = 0;
    int64_t logical_min = 0;
    int64_t logical_max = 0;
    int64_t physical_min = 0;
    int64_t physical_max = 0;
    uint32_t unit = 0;
    int32_t unit_exponent = 0;
    bool is_signed = false;
    bool is_constant = false;
    bool is_variable = false;
    bool is_relative = false;
    bool has_null_state = false;
    uint32_t main_item_flags = 0;
    uint32_t main_item_index = 0;
    uint32_t element_index = 0;
    int top_level_collection_ordinal = 0;
    int collection_ordinal = 0;
    std::vector<UsageRange> usage_ranges;
    std::vector<UsageRange> usage_aliases;
    int64_t windows_api_bit_offset = -1;
    std::vector<uint32_t> ppd_source_records;
    uint32_t ppd_link_collection = 0;
    bool uninterpreted_storage = false;
    bool interpretation_complete = true;
    bool ppd_button_cap = false;
    bool selector_mapping_resolved = true;
    bool ppd_oracle_validated = false;
    bool ppd_physical_eligible = false;
};

struct ParsedReport {
    uint8_t report_id = 0;
    uint32_t input_payload_bits = 0;
    uint32_t input_wire_bytes = 0;
    uint32_t output_payload_bits = 0;
    uint32_t output_wire_bytes = 0;
    uint32_t feature_payload_bits = 0;
    uint32_t feature_wire_bytes = 0;
    std::vector<ParsedField> fields;
    bool ppd_reconstructed = false;
    std::array<uint32_t,3> windows_report_byte_lengths{};
    std::array<bool,3> wire_length_exact{};
    std::array<bool,3> reconstruction_complete{};
};

struct ParsedDescriptorResult {
    bool available = false;
    std::vector<ParsedReport> reports;
    std::vector<ParsedField> fields;
    std::vector<ParsedCollection> collections;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

static ParsedDescriptorResult parse_report_descriptor_bytes(const std::vector<uint8_t>& raw) {
    ParsedDescriptorResult res;
    res.available = !raw.empty();
    size_t cursor = 0;

    int cur_usage_page = 0;
    int64_t cur_log_min = 0, cur_log_max = 0;
    int64_t cur_phys_min = 0, cur_phys_max = 0;
    uint32_t cur_unit = 0;
    int32_t cur_unit_exp = 0;
    uint32_t cur_report_size = 0;
    uint8_t cur_report_id = 0;
    uint32_t cur_report_count = 0;

    struct GlobalStackState {
        int usage_page;
        int64_t log_min, log_max, phys_min, phys_max;
        uint32_t unit;
        int32_t unit_exp;
        uint32_t report_size;
        uint8_t report_id;
        uint32_t report_count;
    };
    std::vector<GlobalStackState> global_stack;

    std::vector<UsageRange> local_usages;
    int pending_range = -1;

    std::map<std::pair<std::string, uint8_t>, uint32_t> offsets;
    std::vector<int> collection_stack;
    uint32_t main_item_index = 0;
    constexpr size_t field_limit = 262144;
    auto usage_at = [&](uint32_t index) -> std::pair<int, int> {
        uint64_t remaining = index;
        for (const auto& u : local_usages) {
            uint64_t count = static_cast<uint64_t>(u.maximum - u.minimum) + 1;
            if (remaining < count) return {u.usage_page, u.minimum + static_cast<int>(remaining)};
            remaining -= count;
        }
        if (!local_usages.empty()) return {local_usages.back().usage_page, local_usages.back().maximum};
        return {cur_usage_page, 0};
    };

    while (cursor < raw.size()) {
        uint8_t prefix = raw[cursor++];
        if (prefix == 0xFE) { // Long item
            if (cursor + 2 > raw.size()) { res.errors.push_back("Truncated long item"); break; }
            uint8_t size = raw[cursor++];
            cursor++;
            if (size > raw.size() - cursor) { res.errors.push_back("Truncated long item payload"); break; }
            res.warnings.push_back("Uninterpreted long item at byte " + std::to_string(cursor - 3));
            cursor += size;
            continue;
        }

        uint8_t size_code = prefix & 0x03;
        uint8_t size = (size_code == 3) ? 4 : size_code;
        uint8_t item_type = (prefix >> 2) & 0x03;
        uint8_t tag = (prefix >> 4) & 0x0F;

        if (cursor + size > raw.size()) { res.errors.push_back("Truncated short item"); break; }
        int32_t sval = 0;
        uint32_t uval = 0;
        for (uint8_t b = 0; b < size; ++b) {
            uval |= (static_cast<uint32_t>(raw[cursor + b]) << (b * 8));
        }
        if (size == 1) sval = static_cast<int8_t>(uval);
        else if (size == 2) sval = static_cast<int16_t>(uval);
        else if (size == 4) sval = static_cast<int32_t>(uval);
        cursor += size;

        if (item_type == 0) { // Main
            if (tag == 8 || tag == 9 || tag == 11) { // Input (8), Output (9), Feature (11)
                if (collection_stack.empty()) res.errors.push_back("Report field outside a Collection");
                std::string rtype = (tag == 8) ? "Input" : (tag == 9 ? "Output" : "Feature");
                auto key = std::make_pair(rtype, cur_report_id);
                uint32_t start = offsets[key];
                bool is_constant = (uval & 0x01) != 0;
                bool is_variable = (uval & 0x02) != 0;
                bool is_relative = (uval & 0x04) != 0;

                uint64_t end = static_cast<uint64_t>(start) + static_cast<uint64_t>(cur_report_count) * cur_report_size;
                uint32_t emitted = is_variable && !is_constant ? cur_report_count : 1;
                if (end > UINT32_MAX - 15 || emitted > field_limit - res.fields.size()) {
                    res.errors.push_back("Report exceeds bounded field or bit-offset limits");
                    break;
                }
                if (!cur_report_count || !cur_report_size) {
                    res.errors.push_back("Main item has zero Report Size or Report Count");
                } else {
                    for (uint32_t i = 0; i < emitted; ++i) {
                        ParsedField f;
                        f.report_type = rtype;
                        f.report_id = cur_report_id;
                        auto usage = usage_at(i);
                        f.usage_page = usage.first;
                        f.usage = usage.second;
                        f.payload_bit_offset = start + (is_variable && !is_constant ? i * cur_report_size : 0);
                        f.wire_bit_offset = f.payload_bit_offset + (cur_report_id ? 8 : 0);
                        f.bit_size = cur_report_size;
                        f.report_count = is_variable && !is_constant ? 1 : cur_report_count;
                        f.logical_min = cur_log_min;
                        f.logical_max = cur_log_max;
                        f.physical_min = cur_phys_min;
                        f.physical_max = cur_phys_max;
                        f.unit = cur_unit;
                        f.unit_exponent = cur_unit_exp;
                        f.is_signed = cur_log_min < 0;
                        f.is_constant = is_constant;
                        f.is_variable = is_variable;
                        f.is_relative = is_relative;
                        f.has_null_state = (uval & 0x40) != 0;
                        f.main_item_flags = uval;
                        f.main_item_index = main_item_index;
                        f.element_index = i;
                        if (!collection_stack.empty()) {
                            f.collection_ordinal = collection_stack.back();
                            f.top_level_collection_ordinal = res.collections[f.collection_ordinal - 1].top_level_ordinal;
                        }
                        if (!is_variable || is_constant) f.usage_ranges = local_usages;
                        res.fields.push_back(std::move(f));
                    }
                }
                ++main_item_index;
                offsets[key] = static_cast<uint32_t>(end);
            } else if (tag == 10) { // Collection
                ParsedCollection c;
                c.ordinal = static_cast<int>(res.collections.size()) + 1;
                c.parent_ordinal = collection_stack.empty() ? 0 : collection_stack.back();
                c.top_level_ordinal = collection_stack.empty() ? c.ordinal : collection_stack.front();
                c.collection_type = uval;
                auto usage = usage_at(0);
                c.usage_page = usage.first;
                c.usage = usage.second;
                res.collections.push_back(c);
                collection_stack.push_back(c.ordinal);
            } else if (tag == 12) { // End Collection
                if (collection_stack.empty()) res.errors.push_back("End Collection without Collection");
                else collection_stack.pop_back();
            } else {
                res.warnings.push_back("Unknown main item tag " + std::to_string(tag));
            }
            if (pending_range >= 0) res.errors.push_back("Usage Minimum without Usage Maximum");
            local_usages.clear();
            pending_range = -1;
        } else if (item_type == 1) { // Global
            switch (tag) {
                case 0: cur_usage_page = static_cast<int>(uval); break;
                case 1: cur_log_min = sval; break;
                case 2: cur_log_max = (cur_log_min < 0) ? static_cast<int64_t>(sval) : static_cast<int64_t>(uval); break;
                case 3: cur_phys_min = sval; break;
                case 4: cur_phys_max = (cur_phys_min < 0) ? static_cast<int64_t>(sval) : static_cast<int64_t>(uval); break;
                case 5: cur_unit_exp = (size == 1 && (uval & 0x08)) ? static_cast<int32_t>(uval & 0x0F) - 16 : sval; break;
                case 6: cur_unit = uval; break;
                case 7: cur_report_size = uval; break;
                case 8:
                    if (uval == 0 || uval > 255) res.errors.push_back("Invalid Report ID");
                    else cur_report_id = static_cast<uint8_t>(uval);
                    break;
                case 9: cur_report_count = uval; break;
                case 10: { // Push
                    GlobalStackState st{cur_usage_page, cur_log_min, cur_log_max, cur_phys_min, cur_phys_max,
                                        cur_unit, cur_unit_exp, cur_report_size, cur_report_id, cur_report_count};
                    global_stack.push_back(st);
                    break;
                }
                case 11: { // Pop
                    if (!global_stack.empty()) {
                        auto st = global_stack.back();
                        global_stack.pop_back();
                        cur_usage_page = st.usage_page; cur_log_min = st.log_min; cur_log_max = st.log_max;
                        cur_phys_min = st.phys_min; cur_phys_max = st.phys_max; cur_unit = st.unit;
                        cur_unit_exp = st.unit_exp; cur_report_size = st.report_size;
                        cur_report_id = st.report_id; cur_report_count = st.report_count;
                    } else res.errors.push_back("Global Pop without Push");
                    break;
                }
                default: res.warnings.push_back("Unknown global item tag " + std::to_string(tag)); break;
            }
        } else if (item_type == 2) { // Local
            auto make_usage = [&](uint32_t v) -> std::pair<int, int> {
                if (size == 4) return { static_cast<int>((v >> 16) & 0xFFFF), static_cast<int>(v & 0xFFFF) };
                return { cur_usage_page, static_cast<int>(v & 0xFFFF) };
            };
            auto usage = make_usage(uval);
            if (tag == 0) local_usages.push_back({usage.first, usage.second, usage.second});
            else if (tag == 1) {
                if (pending_range >= 0) res.errors.push_back("Nested Usage Minimum");
                pending_range = static_cast<int>(local_usages.size());
                local_usages.push_back({usage.first, usage.second, usage.second});
            } else if (tag == 2) {
                if (pending_range < 0 || usage.first != local_usages[pending_range].usage_page ||
                    usage.second < local_usages[pending_range].minimum) {
                    res.errors.push_back("Invalid Usage Minimum/Maximum range");
                } else local_usages[pending_range].maximum = usage.second;
                pending_range = -1;
            } else if (tag == 10) {
                res.errors.push_back("Usage delimiter alternatives require explicit interpretation; raw bytes retained");
            }
        }
    }

    if (!collection_stack.empty()) res.errors.push_back("Unclosed Collection");
    if (!global_stack.empty()) res.warnings.push_back("Global Push stack not empty at descriptor end");
    std::set<uint8_t> all_rids;
    for (const auto& entry : offsets) all_rids.insert(entry.first.second);
    if (all_rids.size() > 1 && all_rids.count(0)) res.errors.push_back("Mixed unnumbered and numbered reports");
    for (uint8_t rid : all_rids) {
        ParsedReport pr;
        pr.report_id = rid;
        auto set_size = [&](const char* type, uint32_t& bits, uint32_t& bytes) {
            auto it = offsets.find({type, rid});
            if (it != offsets.end()) {
                bits = it->second;
                bytes = (bits + 7) / 8 + (rid ? 1 : 0);
            }
        };
        set_size("Input", pr.input_payload_bits, pr.input_wire_bytes);
        set_size("Output", pr.output_payload_bits, pr.output_wire_bytes);
        set_size("Feature", pr.feature_payload_bits, pr.feature_wire_bytes);
        for (const auto& f : res.fields) {
            if (f.report_id == rid) pr.fields.push_back(f);
        }
        res.reports.push_back(pr);
    }
    return res;
}

enum class SemanticFieldType {
    Unknown, Padding, Button, ButtonArray, Key, KeyArray, Modifier, Axis,
    Wheel, Pan, Hat, Trigger, ConsumerControl, ConsumerArray, SystemControl,
    SystemArray, VendorDefined, Led, Array, Undefined, SequenceNumber,
    Gyroscope, Accelerometer, Timestamp, TouchPoint, ContactId, Boolean,
    Coordinate, Battery, Status
};

struct SemanticField {
    SemanticFieldType type = SemanticFieldType::Unknown;
    std::string name;
    std::string normalized_role;
    std::string role_confidence = "unknown";
    std::string native_name;
};

static const char* semantic_type_name(SemanticFieldType type) {
    static const char* names[] = {
        "unknown", "padding", "button", "button_array", "key", "key_array", "modifier", "axis",
        "wheel", "pan", "hat", "trigger", "consumer_control", "consumer_control_array", "system_control",
        "system_control_array", "vendor_defined", "led", "array", "undefined", "sequence_number",
        "gyroscope", "accelerometer", "timestamp", "touch_point", "contact_id", "boolean",
        "coordinate", "battery", "status"
    };
    return names[static_cast<size_t>(type)];
}

static std::string hex_number(uint32_t value, int width = 4) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

static std::string usage_page_name(int page) {
    switch (page) {
        case 1: return "Generic Desktop";
        case 2: return "Simulation Controls";
        case 7: return "Keyboard / Keypad";
        case 8: return "LED";
        case 9: return "Button";
        case 12: return "Consumer";
        default: return "Usage Page " + hex_number(page);
    }
}

static std::string usage_name(int page, int usage) {
    static const std::map<int, std::string> desktop = {
        {1,"pointer"},{2,"mouse"},{4,"joystick"},{5,"gamepad"},{6,"keyboard"},{7,"keypad"},
        {8,"multi_axis_controller"},{0x30,"x"},{0x31,"y"},{0x32,"z"},{0x33,"rx"},{0x34,"ry"},
        {0x35,"rz"},{0x36,"slider"},{0x37,"dial"},{0x38,"wheel"},{0x39,"hat_switch"},
        {0x40,"vx"},{0x41,"vy"},{0x42,"vz"},{0x43,"vbrx"},{0x44,"vbry"},{0x45,"vbrz"},
        {0x46,"vno"},{0x80,"system_control"},{0x81,"system_power_down"},{0x82,"system_sleep"},
        {0x83,"system_wake_up"},{0x84,"system_context_menu"},{0x85,"system_main_menu"},
        {0x86,"system_app_menu"},{0x87,"system_menu_help"},{0x88,"system_menu_exit"},
        {0x89,"system_menu_select"},{0x8a,"system_menu_right"},{0x8b,"system_menu_left"},
        {0x8c,"system_menu_up"},{0x8d,"system_menu_down"},{0x90,"dpad_up"},
        {0x91,"dpad_down"},{0x92,"dpad_right"},{0x93,"dpad_left"}
    };
    static const std::map<int, std::string> simulation = {
        {1,"flight_simulation"},{2,"automobile_simulation"},{0xB0,"aileron"},{0xB1,"aileron_trim"},
        {0xB2,"anti_torque_control"},{0xB3,"autopilot_enable"},{0xB4,"chaff_release"},
        {0xB5,"collective_control"},{0xB6,"dive_brake"},{0xB7,"electronic_countermeasures"},
        {0xB8,"elevator"},{0xB9,"elevator_trim"},{0xBA,"rudder"},{0xBB,"throttle"},
        {0xBC,"flight_communications"},{0xBD,"flare_release"},{0xBE,"landing_gear"},
        {0xBF,"toe_brake"},{0xC0,"trigger"},{0xC1,"weapons_arm"},{0xC2,"weapons_select"},
        {0xC3,"wing_flaps"},{0xC4,"accelerator"},{0xC5,"brake"},{0xC6,"clutch"},
        {0xC7,"shifter"},{0xC8,"steering"},{0xC9,"turret_direction"},{0xCA,"barrel_elevation"}
    };
    static const std::map<int, std::string> consumer = {
        {0,"consumer_undefined"},{1,"consumer_control"},{0x30,"power"},{0x32,"sleep"},{0xB0,"play"},{0xB1,"pause"},
        {0xB2,"record"},{0xB3,"fast_forward"},{0xB4,"rewind"},{0xB5,"next_track"},
        {0xB6,"previous_track"},{0xB7,"stop"},{0xB8,"eject"},{0xCD,"play_pause"},
        {0xE2,"mute"},{0xE9,"volume_up"},{0xEA,"volume_down"},{0x183,"media_configuration"},
        {0x18A,"email"},{0x192,"calculator"},{0x194,"local_browser"},{0x221,"ac_search"},
        {0x223,"ac_home"},{0x224,"ac_back"},{0x225,"ac_forward"},{0x226,"ac_stop"},
        {0x227,"ac_refresh"},{0x22A,"ac_bookmarks"},{0x238,"ac_pan"}
    };
    static const std::map<int, std::string> keyboard = {
        {0,"none"},{1,"error_rollover"},{2,"post_fail"},{3,"error_undefined"},
        {0x28,"enter"},{0x29,"escape"},{0x2A,"backspace"},{0x2B,"tab"},{0x2C,"space"},
        {0x2D,"minus"},{0x2E,"equal"},{0x2F,"left_bracket"},{0x30,"right_bracket"},
        {0x31,"backslash"},{0x32,"non_us_hash"},{0x33,"semicolon"},{0x34,"apostrophe"},
        {0x35,"grave"},{0x36,"comma"},{0x37,"period"},{0x38,"slash"},{0x39,"caps_lock"},
        {0x46,"print_screen"},{0x47,"scroll_lock"},{0x48,"pause"},{0x49,"insert"},
        {0x4A,"home"},{0x4B,"page_up"},{0x4C,"delete"},{0x4D,"end"},{0x4E,"page_down"},
        {0x4F,"right"},{0x50,"left"},{0x51,"down"},{0x52,"up"},{0x53,"num_lock"},
        {0x54,"keypad_divide"},{0x55,"keypad_multiply"},{0x56,"keypad_subtract"},
        {0x57,"keypad_add"},{0x58,"keypad_enter"},{0x62,"keypad_0"},{0x63,"keypad_period"},
        {0x64,"non_us_backslash"},{0x65,"application"},{0x66,"power"},{0x67,"keypad_equal"},
        {0x74,"execute"},{0x75,"help"},{0x76,"menu"},{0x77,"select"},{0x78,"stop"},
        {0x79,"again"},{0x7A,"undo"},{0x7B,"cut"},{0x7C,"copy"},{0x7D,"paste"},{0x7E,"find"},
        {0x7F,"mute"},{0x80,"volume_up"},{0x81,"volume_down"},
        {0x82,"locking_caps_lock"},{0x83,"locking_num_lock"},{0x84,"locking_scroll_lock"},
        {0x85,"keypad_comma"},{0x86,"keypad_equal_sign"},{0x99,"alternate_erase"},
        {0x9A,"sysreq_attention"},{0x9B,"cancel"},{0x9C,"clear"},{0x9D,"prior"},
        {0x9E,"return"},{0x9F,"separator"},{0xA0,"out"},{0xA1,"oper"},{0xA2,"clear_again"},
        {0xA3,"crsel_props"},{0xA4,"exsel"},{0xB0,"keypad_00"},{0xB1,"keypad_000"},
        {0xB2,"thousands_separator"},{0xB3,"decimal_separator"},{0xB4,"currency_unit"},
        {0xB5,"currency_subunit"},{0xB6,"keypad_left_parenthesis"},{0xB7,"keypad_right_parenthesis"},
        {0xB8,"keypad_left_brace"},{0xB9,"keypad_right_brace"},{0xBA,"keypad_tab"},
        {0xBB,"keypad_backspace"},{0xC2,"keypad_xor"},{0xC3,"keypad_caret"},
        {0xC4,"keypad_percent"},{0xC5,"keypad_less_than"},{0xC6,"keypad_greater_than"},
        {0xC7,"keypad_ampersand"},{0xC8,"keypad_double_ampersand"},{0xC9,"keypad_vertical_bar"},
        {0xCA,"keypad_double_vertical_bar"},{0xCB,"keypad_colon"},{0xCC,"keypad_hash"},
        {0xCD,"keypad_space"},{0xCE,"keypad_at"},{0xCF,"keypad_exclamation"},
        {0xD0,"keypad_memory_store"},{0xD1,"keypad_memory_recall"},{0xD2,"keypad_memory_clear"},
        {0xD3,"keypad_memory_add"},{0xD4,"keypad_memory_subtract"},{0xD5,"keypad_memory_multiply"},
        {0xD6,"keypad_memory_divide"},{0xD7,"keypad_plus_minus"},{0xD8,"keypad_clear"},
        {0xD9,"keypad_clear_entry"},{0xDA,"keypad_binary"},{0xDB,"keypad_octal"},
        {0xDC,"keypad_decimal"},{0xDD,"keypad_hexadecimal"},
        {0xE0,"left_control"},{0xE1,"left_shift"},{0xE2,"left_alt"},{0xE3,"left_gui"},
        {0xE4,"right_control"},{0xE5,"right_shift"},{0xE6,"right_alt"},{0xE7,"right_gui"}
    };
    const std::map<int, std::string>* table = nullptr;
    if (page == 1) table = &desktop;
    else if (page == 2) table = &simulation;
    else if (page == 12) table = &consumer;
    else if (page == 7) {
        if (usage >= 4 && usage <= 29) return "keyboard_" + std::string(1, static_cast<char>('a' + usage - 4));
        if (usage >= 0x1E && usage <= 0x27) return "keyboard_" + std::to_string((usage - 0x1D) % 10);
        if (usage >= 0x3A && usage <= 0x45) return "keyboard_f" + std::to_string(usage - 0x39);
        if (usage >= 0x68 && usage <= 0x73) return "keyboard_f" + std::to_string(usage - 0x68 + 13);
        if (usage >= 0x59 && usage <= 0x61) return "keyboard_keypad_" + std::to_string(usage - 0x58);
        if (usage >= 0x87 && usage <= 0x8F) return "keyboard_international_" + std::to_string(usage - 0x86);
        if (usage >= 0x90 && usage <= 0x98) return "keyboard_lang_" + std::to_string(usage - 0x8F);
        if (usage >= 0xBC && usage <= 0xC1) return "keyboard_keypad_" + std::string(1, static_cast<char>('a' + usage - 0xBC));
        auto it = keyboard.find(usage);
        return "keyboard_" + (it == keyboard.end() ? "usage_" + hex_number(usage) : it->second);
    } else if (page == 9) return "button_" + std::to_string(usage);
    else if (page == 8 && usage >= 1 && usage <= 5) {
        static const char* leds[] = {"num_lock", "caps_lock", "scroll_lock", "compose", "kana"};
        return leds[usage - 1];
    }
    if (table) {
        auto it = table->find(usage);
        if (it != table->end()) return it->second;
    }
    return "usage_" + hex_number(page) + "_" + hex_number(usage);
}

static std::string collection_classification(int page, int usage) {
    if (page == 1) {
        switch (usage) {
            case 1: case 2: return "mouse";
            case 4: return "joystick";
            case 5: return "gamepad";
            case 6: case 7: return "keyboard";
            case 8: return "multi_axis_controller";
            case 0x80: return "system_control";
        }
    }
    if (page == 7) return "keyboard";
    if (page == 12) return "consumer_control";
    if (page >= 0xFF00) return "vendor_defined";
    return "unknown";
}

static const char* collection_type_name(uint32_t type) {
    static const char* names[] = {"physical", "application", "logical", "report", "named_array", "usage_switch", "usage_modifier"};
    if (type < 7) return names[type];
    return type >= 128 ? "vendor_defined" : "reserved";
}

static SemanticField classify_field(const ParsedField& f) {
    SemanticField s;
    if (f.uninterpreted_storage) { s.name = "uninterpreted_storage"; return s; }
    s.name = usage_name(f.usage_page, f.usage);
    if (f.is_constant) { s.type = SemanticFieldType::Padding; s.name = "padding"; }
    else if (f.usage_page >= 0xFF00) s.type = SemanticFieldType::VendorDefined;
    else if (f.usage_page == 7) {
        if (!f.is_variable) { s.type = SemanticFieldType::KeyArray; s.name = "keyboard_keys"; }
        else s.type = f.usage >= 0xE0 && f.usage <= 0xE7 ? SemanticFieldType::Modifier : SemanticFieldType::Key;
    } else if (f.usage_page == 9) s.type = f.is_variable ? SemanticFieldType::Button : SemanticFieldType::ButtonArray;
    else if (f.usage_page == 12) {
        if (!f.is_variable) { s.type = SemanticFieldType::ConsumerArray; s.name = "consumer_controls"; }
        else if (f.usage == 0) s.type = SemanticFieldType::Undefined;
        else if (f.usage == 0x238) { s.type = SemanticFieldType::Pan; s.name = "pan"; }
        else s.type = SemanticFieldType::ConsumerControl;
    } else if (f.usage_page == 1 && f.usage >= 0x80 && f.usage <= 0x8F) {
        s.type = f.is_variable ? SemanticFieldType::SystemControl : SemanticFieldType::SystemArray;
    } else if (!f.is_variable) s.type = SemanticFieldType::Array;
    else if (f.usage_page == 1) {
        if (f.usage == 0x38) s.type = SemanticFieldType::Wheel;
        else if (f.usage == 0x39) s.type = SemanticFieldType::Hat;
        else if ((f.usage >= 0x30 && f.usage <= 0x37) || (f.usage >= 0x40 && f.usage <= 0x46)) s.type = SemanticFieldType::Axis;
        else if (f.usage >= 0x90 && f.usage <= 0x93) s.type = SemanticFieldType::Button;
    } else if (f.usage_page == 2) {
        if (f.usage == 0xC0) s.type = SemanticFieldType::Trigger;
        else if (f.usage == 0xB0 || f.usage == 0xB1 || f.usage == 0xB2 || f.usage == 0xB5 ||
                 f.usage == 0xB6 || f.usage == 0xB8 || f.usage == 0xB9 || f.usage == 0xBA ||
                 f.usage == 0xBB || f.usage == 0xBF || f.usage == 0xC3 || f.usage == 0xC4 ||
                 f.usage == 0xC5 || f.usage == 0xC6 || f.usage == 0xC8 || f.usage == 0xC9 ||
                 f.usage == 0xCA) s.type = SemanticFieldType::Axis;
    } else if (f.usage_page == 8) s.type = SemanticFieldType::Led;
    return s;
}

static std::string json_string(const std::string& value) {
    return "\"" + escape_json(value) + "\"";
}

static void write_strings_json(std::ostream& out, const std::vector<std::string>& values) {
    out << '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << json_string(values[i]);
    }
    out << ']';
}

struct LayoutEvidence {
    std::string source = "hid_report_descriptor";
    std::string authority = "descriptor_semantics";
    std::string range_source = "hid_report_descriptor";
    bool physical_wire = false;
    bool authoritative_ranges = false;
    int selection_priority = 0;
};

#include "windows_ppd_decoder.h"

static LayoutEvidence windows_logical_evidence() {
    return {"windows_hid_stack", "logical_os_view", "windows_hid_caps", false, false, 4};
}

static LayoutEvidence protocol_definition_evidence() {
    return {"known_protocol_definition", "protocol_semantics", "known_protocol", false, false, 0};
}

static bool logical_range_fits_field(const ParsedField& f) {
    if (!f.bit_size || f.logical_min > f.logical_max) return false;
    if (f.is_signed) {
        if (f.bit_size >= 64) return true;
        const int64_t magnitude = static_cast<int64_t>(UINT64_C(1) << (f.bit_size - 1));
        return f.logical_min >= -magnitude && f.logical_max <= magnitude - 1;
    }
    if (f.logical_min < 0) return false;
    if (f.bit_size >= 63) return true;
    return static_cast<uint64_t>(f.logical_max) <= (UINT64_C(1) << f.bit_size) - 1;
}

static std::pair<std::string, std::string> storage_range_numbers(uint32_t width, bool is_signed) {
    if (!width || width > 64) return {"null", "null"};
    if (is_signed) {
        if (width == 64) return {std::to_string(INT64_MIN), std::to_string(INT64_MAX)};
        const int64_t magnitude = static_cast<int64_t>(UINT64_C(1) << (width - 1));
        return {std::to_string(-magnitude), std::to_string(magnitude - 1)};
    }
    return {"0", std::to_string(width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1)};
}

static void write_layout_evidence_json(std::ostream& out, const LayoutEvidence& evidence) {
    out << "\"layout_source\":" << json_string(evidence.source)
        << ",\"authority\":" << json_string(evidence.authority)
        << ",\"physical_wire_layout\":" << (evidence.physical_wire ? "true" : "false")
        << ",\"layout_selection_priority\":"
        << (evidence.selection_priority ? std::to_string(evidence.selection_priority) : "null");
}

static void write_range_json(std::ostream& out, const ParsedField& f,
                             const LayoutEvidence& evidence, bool known_protocol) {
    const bool applicable = !f.is_constant && !f.uninterpreted_storage;
    const bool consistent = applicable && logical_range_fits_field(f);
    const std::string source = known_protocol ? "known_protocol" : evidence.range_source;
    out << ",\"logical_range_source\":" << json_string(source)
        << ",\"range_validation_applicable\":" << (applicable ? "true" : "false")
        << ",\"range_consistent_with_field_width\":" << (!applicable ? "null" : consistent ? "true" : "false")
        << ",\"range_authoritative_for_physical_wire\":"
        << (evidence.authoritative_ranges && consistent && (f.ppd_source_records.empty() || f.ppd_physical_eligible) ? "true" : "false");
    if (known_protocol || !f.ppd_source_records.empty()) {
        const auto storage = storage_range_numbers(f.bit_size, f.is_signed);
        out << ",\"wire_bit_size\":" << f.bit_size
            << ",\"raw_storage_min\":" << storage.first << ",\"raw_storage_max\":" << storage.second
            << ",\"semantic_min\":" << f.logical_min << ",\"semantic_max\":" << f.logical_max
            << ",\"range_source\":" << json_string(known_protocol ? "known_protocol" : "windows_preparsed_data");
    }
}

static void write_range_txt(std::ostream& out, const ParsedField& f, const LayoutEvidence& evidence) {
    const bool applicable = !f.is_constant && !f.uninterpreted_storage;
    const bool consistent = applicable && logical_range_fits_field(f);
    out << "    Logical range source: " << evidence.range_source
        << "; range validation applicable: " << (applicable ? "yes" : "no")
        << "; consistent with field width: " << (!applicable ? (f.uninterpreted_storage ? "not applicable (uninterpreted storage)" : "not applicable (constant padding)") : consistent ? "yes" : "no")
        << "; authoritative for physical wire: "
        << (evidence.authoritative_ranges && consistent && (f.ppd_source_records.empty() || f.ppd_physical_eligible) ? "yes" : "no") << '\n';
    if (applicable && !consistent)
        out << "    Range diagnostic: declared logical range is not representable by this field; values retained without normalization.\n";
}

static std::vector<int> array_usages(const ParsedField& f) {
    std::vector<int> usages;
    if (f.is_variable || f.is_constant || (f.usage_page != 7 && f.usage_page != 12)) return usages;
    std::vector<std::pair<int, int>> ranges;
    for (const auto& range : f.usage_ranges) {
        if (range.usage_page == f.usage_page && range.minimum >= 0 && range.maximum <= 0xFFFF && range.minimum <= range.maximum)
            ranges.emplace_back(range.minimum, range.maximum);
    }
    std::sort(ranges.begin(), ranges.end());
    int next_usage = 0;
    for (const auto& range : ranges) {
        for (int usage = (std::max)(range.first, next_usage); usage <= range.second; ++usage)
            usages.push_back(usage);
        next_usage = (std::max)(next_usage, range.second + 1);
    }
    return usages;
}

static void write_array_usage_json(std::ostream& out, const ParsedField& f) {
    const auto usages = array_usages(f);
    const int hex_width = f.usage_page == 12 ? 4 : 2;
    out << ",\"usage_min\":" << (usages.empty() ? "null" : std::to_string(usages.front()))
        << ",\"usage_max\":" << (usages.empty() ? "null" : std::to_string(usages.back()))
        << ",\"usage_values\":{";
    bool first = true;
    for (int usage : usages) {
        if (!first) out << ',';
        first = false;
        out << json_string(hex_number(static_cast<uint32_t>(usage), hex_width)) << ':' << json_string(usage_name(f.usage_page, usage));
    }
    out << '}';
}

static void write_field_json(std::ostream& out, const ParsedField& f,
                             const SemanticField* known = nullptr, bool hid = true,
                             const LayoutEvidence& evidence = {}, bool close_object = true) {
    SemanticField s = known ? *known : classify_field(f);
    out << std::dec << std::boolalpha;
    out << "{\"report_type\":" << json_string(f.report_type)
        << ",\"report_id\":" << (hid || f.report_id ? std::to_string(f.report_id) : "null")
        << ",\"usage_page\":" << (hid ? std::to_string(f.usage_page) : "null")
        << ",\"usage\":" << (hid ? std::to_string(f.usage) : "null")
        << ",\"usage_name\":" << (hid ? json_string(usage_name(f.usage_page, f.usage)) : "null")
        << ",\"usage_page_name\":" << (hid ? json_string(usage_page_name(f.usage_page)) : "null")
        << ",\"payload_bit_offset\":" << f.payload_bit_offset
        << ",\"wire_bit_offset\":" << f.wire_bit_offset
        << ",\"bit_offset\":" << f.wire_bit_offset
        << ",\"bit_size\":" << f.bit_size
        << ",\"report_count\":" << f.report_count
        << ",\"element_size\":" << f.bit_size
        << ",\"element_count\":" << f.report_count
        << ",\"total_bits\":" << static_cast<uint64_t>(f.bit_size) * f.report_count
        << ",\"logical_min\":" << f.logical_min << ",\"logical_max\":" << f.logical_max
        << ",\"physical_min\":" << f.physical_min << ",\"physical_max\":" << f.physical_max
        << ",\"unit\":" << f.unit << ",\"unit_exponent\":" << f.unit_exponent
        << ",\"signed\":" << f.is_signed << ",\"constant\":" << f.is_constant
        << ",\"variable\":" << f.is_variable << ",\"array\":" << (!f.is_variable && !f.is_constant && !f.uninterpreted_storage)
        << ",\"relative\":" << f.is_relative << ",\"absolute\":" << !f.is_relative
        << ",\"null_state\":" << f.has_null_state << ",\"main_item_flags\":" << f.main_item_flags
        << ",\"wrap\":" << ((f.main_item_flags & 8) != 0)
        << ",\"nonlinear\":" << ((f.main_item_flags & 16) != 0)
        << ",\"no_preferred_state\":" << ((f.main_item_flags & 32) != 0)
        << ",\"volatile\":" << ((f.main_item_flags & 128) != 0)
        << ",\"buffered_bytes\":" << ((f.main_item_flags & 256) != 0)
        << ",\"main_item_index\":" << f.main_item_index << ",\"element_index\":" << f.element_index
        << ",\"collection_ordinal\":" << f.collection_ordinal
        << ",\"top_level_collection_ordinal\":" << f.top_level_collection_ordinal
        << ",\"semantic_type\":" << json_string(semantic_type_name(s.type))
        << ",\"semantic_name\":" << json_string(s.name)
        << ",\"normalized_role\":" << (s.normalized_role.empty() ? "null" : json_string(s.normalized_role))
        << ",\"role_confidence\":" << json_string(s.role_confidence);
    if (!s.native_name.empty()) out << ",\"native_name\":" << json_string(s.native_name);
    write_range_json(out, f, evidence, !hid && s.role_confidence == "known_protocol");
    if (hid) out << ",\"windows_api_bit_offset\":" << (f.windows_api_bit_offset >= 0 ? f.windows_api_bit_offset : static_cast<int64_t>(f.payload_bit_offset) + 8);
    if (f.uninterpreted_storage) out << ",\"preserve\":true,\"padding_proven\":false,\"storage_interpretation\":\"unknown\"";
    if (!f.ppd_source_records.empty()) {
        out << ",\"interpretation_complete\":" << f.interpretation_complete
            << ",\"windows_logical_oracle_validated\":" << f.ppd_oracle_validated
            << ",\"physical_report_eligible\":" << f.ppd_physical_eligible
            << ",\"selector_mapping_resolved\":" << f.selector_mapping_resolved
            << ",\"ppd_link_collection\":" << f.ppd_link_collection << ",\"ppd_source_records\":[";
        for (size_t i=0;i<f.ppd_source_records.size();++i) { if(i)out<<',';out<<f.ppd_source_records[i]; }
        out << "]";
    }
    if (hid && f.usage_page == 9 && f.is_variable && !f.is_constant) out << ",\"button_index\":" << f.usage;
    out << ",\"usage_ranges\":[";
    int64_t selector = f.logical_min;
    for (size_t i = 0; i < f.usage_ranges.size(); ++i) {
        if (i) out << ',';
        const auto& u = f.usage_ranges[i];
        if (u.explicit_selector) selector = u.selector_min;
        out << "{\"usage_page\":" << u.usage_page << ",\"usage_min\":" << u.minimum
            << ",\"usage_max\":" << u.maximum;
        if (!f.is_variable && !f.is_constant) {
            out << ",\"selector_min\":" << selector << ",\"selector_max\":" << selector + u.maximum - u.minimum;
        }
        out << '}';
        selector += static_cast<int64_t>(u.maximum) - u.minimum + 1;
    }
    out << ']';
    if (!f.usage_aliases.empty()) {
        out << ",\"usage_aliases\":[";
        for(size_t i=0;i<f.usage_aliases.size();++i) {
            if(i)out<<',';
            const auto& u=f.usage_aliases[i];
            out<<"{\"usage_page\":"<<u.usage_page<<",\"usage_min\":"<<u.minimum<<",\"usage_max\":"<<u.maximum
                <<",\"selector_min\":"<<u.selector_min<<",\"shared_storage\":true}";
        }
        out<<']';
    }
    if (hid && (s.type == SemanticFieldType::KeyArray || s.type == SemanticFieldType::ConsumerArray))
        write_array_usage_json(out, f);
    if (close_object) out << '}';
}

static bool report_has_collection(const ParsedReport& r, int ordinal) {
    return std::any_of(r.fields.begin(), r.fields.end(), [&](const ParsedField& f) {
        return ordinal < 0 || f.top_level_collection_ordinal == ordinal;
    });
}

static void write_reports_json(std::ostream& out, const ParsedDescriptorResult& parsed, int ordinal = -1,
                               const LayoutEvidence& evidence = {}) {
    out << std::dec << '[';
    bool first_report = true;
    for (const auto& r : parsed.reports) {
        if (!report_has_collection(r, ordinal)) continue;
        if (!first_report) out << ',';
        first_report = false;
        out << "\n        {";
        write_layout_evidence_json(out, evidence);
        out << ",\"report_id\":" << static_cast<unsigned>(r.report_id)
            << ",\"has_report_id_byte\":" << (r.report_id != 0 ? "true" : "false")
            << ",\"input_payload_bits\":" << r.input_payload_bits << ",\"input_wire_bytes\":" << r.input_wire_bytes
            << ",\"output_payload_bits\":" << r.output_payload_bits << ",\"output_wire_bytes\":" << r.output_wire_bytes
            << ",\"feature_payload_bits\":" << r.feature_payload_bits << ",\"feature_wire_bytes\":" << r.feature_wire_bytes;
        if(r.ppd_reconstructed) {
            out << ",\"length_evidence\":[";
            for(int t=0;t<3;++t) {
                if(t)out<<',';
                const uint32_t lower=t==0?r.input_wire_bytes:t==1?r.output_wire_bytes:r.feature_wire_bytes;
                out << "{\"report_type\":"<<json_string(ppd_type_name(t))<<",\"windows_api_report_byte_length\":"<<r.windows_report_byte_lengths[t]
                    <<",\"wire_bytes_lower_bound\":"<<lower<<",\"exact_wire_bytes\":"<<(r.wire_length_exact[t]?std::to_string(lower):"null")
                    <<",\"wire_sizes_exact\":"<<(r.wire_length_exact[t]?"true":"false")<<",\"reconstruction_complete\":"<<(r.reconstruction_complete[t]?"true":"false")<<'}';
            }
            out<<']';
        }
        out << ",\"fields\":[";
        bool first_field = true;
        for (const auto& f : r.fields) {
            if (ordinal >= 0 && f.top_level_collection_ordinal != ordinal) continue;
            if (!first_field) out << ',';
            first_field = false;
            out << "\n          ";
            write_field_json(out, f, nullptr, true, evidence);
        }
        out << "]}";
    }
    out << ']';
}

static std::vector<std::string> descriptor_classifications(const ParsedDescriptorResult& parsed) {
    std::set<std::string> unique;
    for (const auto& c : parsed.collections) {
        if (c.parent_ordinal == 0) unique.insert(collection_classification(c.usage_page, c.usage));
    }
    if (unique.empty()) unique.insert("unknown");
    return {unique.begin(), unique.end()};
}

static void write_parsed_json(std::ostream& out, const ParsedDescriptorResult& parsed, const std::string& protocol_class = "",
                              const LayoutEvidence& evidence = {}) {
    auto classes = descriptor_classifications(parsed);
    if (!parsed.available && !protocol_class.empty()) classes = {protocol_class};
    out << "\"classification\":" << json_string(classes.size() == 1 ? classes.front() : "composite")
        << ",\"classifications\":";
    write_strings_json(out, classes);
    out << ",\"hid_layout_evidence\":{";
    write_layout_evidence_json(out, evidence);
    out << '}';
    out << ",\"parse_errors\":";
    write_strings_json(out, parsed.errors);
    out << ",\"parse_warnings\":";
    write_strings_json(out, parsed.warnings);
    out << ",\"reports\":";
    write_reports_json(out, parsed, -1, evidence);
    out << ",\"collections\":[";
    bool first = true;
    for (const auto& c : parsed.collections) {
        if (c.parent_ordinal != 0) continue;
        if (!first) out << ',';
        first = false;
        out << '{';
        write_layout_evidence_json(out, evidence);
        out << ",\"ordinal\":" << c.ordinal << ",\"collection_type\":" << json_string(collection_type_name(c.collection_type))
            << ",\"collection_type_value\":" << c.collection_type << ",\"usage_page\":" << c.usage_page << ",\"usage\":" << c.usage
            << ",\"classification\":" << json_string(collection_classification(c.usage_page, c.usage))
            << ",\"semantic_name\":" << json_string(usage_name(c.usage_page, c.usage)) << ",\"reports\":";
        write_reports_json(out, parsed, c.ordinal, evidence);
        out << '}';
    }
    out << "],\"collection_nodes\":[";
    for (size_t i = 0; i < parsed.collections.size(); ++i) {
        const auto& c = parsed.collections[i];
        if (i) out << ',';
        out << "{\"ordinal\":" << c.ordinal << ",\"parent_ordinal\":" << c.parent_ordinal
            << ",\"top_level_ordinal\":" << c.top_level_ordinal
            << ",\"collection_type\":" << json_string(collection_type_name(c.collection_type))
            << ",\"collection_type_value\":" << c.collection_type
            << ",\"usage_page\":" << c.usage_page << ",\"usage\":" << c.usage
            << ",\"semantic_name\":" << json_string(usage_name(c.usage_page, c.usage)) << '}';
    }
    out << ']';
}

static void write_array_txt(std::ostream& out, const ParsedField& f) {
    const auto usages = array_usages(f);
    const int hex_width = f.usage_page == 12 ? 4 : 2;
    out << std::dec << (f.usage_page == 12 ? "    CONSUMER CONTROL ARRAY\n" : "    KEY ARRAY\n")
        << "    Wire bit: " << f.wire_bit_offset << "; payload bit: " << f.payload_bit_offset << '\n'
        << "    Element size: " << f.bit_size << " bits; slots: " << f.report_count << '\n'
        << "    Usage Page: " << usage_page_name(f.usage_page) << " (" << hex_number(f.usage_page) << ")\n"
        << "    Type: " << (f.is_signed ? "signed " : "unsigned ") << (f.is_relative ? "relative " : "absolute ")
        << "array slots; logical: " << f.logical_min << ".." << f.logical_max << "; null state: " << f.has_null_state << '\n';
    if (usages.empty()) {
        out << "    Usage range: unavailable; no local " << usage_page_name(f.usage_page) << " usages declared\n"
            << "    Values: unavailable\n";
        return;
    }
    const bool contiguous = usages.size() == static_cast<size_t>(usages.back() - usages.front() + 1);
    out << (contiguous ? "    Usage range: " : "    Usage bounds: ")
        << hex_number(static_cast<uint32_t>(usages.front()), hex_width) << ".." << hex_number(static_cast<uint32_t>(usages.back()), hex_width)
        << (contiguous ? "\n" : " (sparse; see Values)\n")
        << "    Selector ranges:\n";
    int64_t selector = f.logical_min;
    for (const auto& range : f.usage_ranges) {
        if (range.explicit_selector) selector = range.selector_min;
        const int64_t last = selector + range.maximum - range.minimum;
        out << "      " << selector << ".." << last << " -> " << usage_page_name(range.usage_page)
            << " usages " << hex_number(static_cast<uint32_t>(range.minimum), range.usage_page == 12 ? 4 : 2)
            << ".." << hex_number(static_cast<uint32_t>(range.maximum), range.usage_page == 12 ? 4 : 2) << '\n';
        selector = last + 1;
    }
    out << "    Values (" << usage_page_name(f.usage_page) << " usage IDs):\n";
    for (int usage : usages)
        out << "      " << hex_number(static_cast<uint32_t>(usage), hex_width) << " = " << usage_name(f.usage_page, usage) << '\n';
}

static void write_semantic_txt(std::ostream& out, const ParsedDescriptorResult& parsed,
                               const LayoutEvidence& evidence = {}) {
    out << "SEMANTIC REPORT LAYOUTS\n";
    for (const auto& c : parsed.collections) {
        if (c.parent_ordinal) continue;
        out << "\n" << str_toupper(collection_classification(c.usage_page, c.usage)) << " collection " << c.ordinal
            << " (" << usage_page_name(c.usage_page) << " / " << usage_name(c.usage_page, c.usage) << ")\n";
        for (const auto& r : parsed.reports) {
            if (!report_has_collection(r, c.ordinal)) continue;
            out << "Report ID: " << static_cast<unsigned>(r.report_id) << " | Wire bytes: Input " << r.input_wire_bytes
                << ", Output " << r.output_wire_bytes << ", Feature " << r.feature_wire_bytes << '\n';
            if(r.ppd_reconstructed)for(int type=0;type<3;++type) {
                if(!r.windows_report_byte_lengths[type])continue;
                const uint32_t bytes=type==0?r.input_wire_bytes:type==1?r.output_wire_bytes:r.feature_wire_bytes;
                out<<"  "<<ppd_type_name(type)<<" length evidence: Windows buffer "<<r.windows_report_byte_lengths[type]
                    <<"; candidate wire lower bound "<<bytes<<"; exact candidate length "<<(r.wire_length_exact[type]?std::to_string(bytes):"unknown")<<'\n';
            }
            for (const auto& f : r.fields) {
                if (f.top_level_collection_ordinal != c.ordinal) continue;
                auto s = classify_field(f);
                out << "  " << f.report_type << " " << s.name << " [" << semantic_type_name(s.type) << "]\n";
                if(r.ppd_reconstructed)out<<"    Offsets: Windows API bit "<<f.windows_api_bit_offset<<"; payload bit "<<f.payload_bit_offset
                    <<"; candidate wire bit "<<f.wire_bit_offset<<"; preserve storage: "<<(f.uninterpreted_storage?"yes (unknown)":"retain unrelated bits")<<'\n';
                if (s.type == SemanticFieldType::KeyArray || s.type == SemanticFieldType::ConsumerArray) {
                    write_array_txt(out, f);
                    write_range_txt(out, f, evidence);
                    continue;
                }
                out << "    Usage: " << usage_page_name(f.usage_page) << " / " << usage_name(f.usage_page, f.usage)
                    << " (" << hex_number(f.usage_page) << ':' << hex_number(f.usage) << ")\n"
                    << "    Wire bit: " << f.wire_bit_offset << "; payload bit: " << f.payload_bit_offset
                    << "; size: " << f.bit_size << "; count: " << f.report_count << '\n'
                    << "    Type: " << (f.is_signed ? "signed " : "unsigned ") << (f.is_relative ? "relative " : "absolute ")
                    << (f.is_constant ? "constant" : f.is_variable ? "variable" : "array slots")
                    << "; logical: " << f.logical_min << ".." << f.logical_max << "; null state: " << f.has_null_state << '\n';
                write_range_txt(out, f, evidence);
            }
        }
    }
    for (const auto& e : parsed.errors) out << "PARSE ERROR: " << e << '\n';
    for (const auto& w : parsed.warnings) out << "PARSE NOTE: " << w << '\n';
    out << '\n';
}

struct WindowsHidInfo {
    bool available = false;
    HIDD_ATTRIBUTES attributes{};
    std::string manufacturer;
    std::string product;
    std::string serial_number;
    HIDP_CAPS caps{};
    std::vector<HIDP_BUTTON_CAPS> input_button_caps;
    std::vector<HIDP_VALUE_CAPS> input_value_caps;
    std::vector<HIDP_BUTTON_CAPS> output_button_caps;
    std::vector<HIDP_VALUE_CAPS> output_value_caps;
    std::vector<HIDP_BUTTON_CAPS> feature_button_caps;
    std::vector<HIDP_VALUE_CAPS> feature_value_caps;
    std::vector<HIDP_LINK_COLLECTION_NODE> link_collections;
    std::vector<uint8_t> cached_collection_descriptor_bytes;
    std::vector<ParsedField> observed_fields;
    std::vector<std::string> layout_errors;
    std::string error;
    PpdResult ppd;
    std::string ppd_source;
    std::string ppd_length_source;
    std::string capture_os_version;
    ULONG hid_api_version = 0;
    bool public_caps_complete = false;
    std::array<bool,3> public_type_caps_complete{};
    bool public_links_complete = false;
    std::string ppd_collection_match;
    std::string ppd_sidecar_file;
    std::vector<std::string> ppd_acquisition_diagnostics;
    std::array<PpdInitializerAudit,3> native_initializer_audit;
};

#include "windows_ppd_live.h"

static bool changed_bit_span(const std::vector<char>& a, const std::vector<char>& b, uint32_t& start, uint32_t& width) {
    start = UINT32_MAX;
    uint32_t end = 0, count = 0;
    for (size_t i = 1; i < a.size(); ++i) {
        uint8_t changed = static_cast<uint8_t>(a[i] ^ b[i]);
        for (uint32_t bit = 0; bit < 8; ++bit) {
            if (!(changed & (1u << bit))) continue;
            uint32_t pos = static_cast<uint32_t>(i * 8) + bit;
            start = (std::min)(start, pos);
            end = pos;
            ++count;
        }
    }
    if (!count) return false;
    width = end - start + 1;
    return width == count;
}

static void observe_windows_layout(WindowsHidInfo& info, PHIDP_PREPARSED_DATA pp) {
    struct Type {
        HIDP_REPORT_TYPE value;
        const char* name;
        USHORT length;
        const std::vector<HIDP_BUTTON_CAPS>& buttons;
        const std::vector<HIDP_VALUE_CAPS>& values;
    };
    Type types[] = {
        {HidP_Input,"Input",info.caps.InputReportByteLength,info.input_button_caps,info.input_value_caps},
        {HidP_Output,"Output",info.caps.OutputReportByteLength,info.output_button_caps,info.output_value_caps},
        {HidP_Feature,"Feature",info.caps.FeatureReportByteLength,info.feature_button_caps,info.feature_value_caps}
    };
    for (const auto& t : types) {
        if (!t.length) continue;
        auto initial = [&](UCHAR rid) {
            std::vector<char> report(t.length, 0);
            report[0] = static_cast<char>(rid);
            return report;
        };
        auto save = [&](ParsedField f, const std::vector<char>& zero, const std::vector<char>& ones, uint32_t expected) {
            uint32_t offset = 0, width = 0;
            if (!changed_bit_span(zero, ones, offset, width) || width != expected) return false;
            f.payload_bit_offset = offset - 8;
            f.wire_bit_offset = f.payload_bit_offset + (f.report_id ? 8 : 0);
            f.report_type = t.name;
            f.top_level_collection_ordinal = 1;
            f.collection_ordinal = 1;
            f.main_item_index = static_cast<uint32_t>(info.observed_fields.size());
            info.observed_fields.push_back(std::move(f));
            return true;
        };
        for (const auto& b : t.buttons) {
            if (b.IsAlias) {
                info.layout_errors.push_back(std::string(t.name) + " aliased button usages require the physical descriptor");
                continue;
            }
            if (!(b.BitField & 2)) {
                info.layout_errors.push_back(std::string(t.name) + " array slot width/count is unavailable in legacy Windows button caps; physical descriptor required");
                continue;
            }
            uint32_t first = b.IsRange ? b.Range.UsageMin : b.NotRange.Usage;
            uint32_t last = b.IsRange ? b.Range.UsageMax : b.NotRange.Usage;
            for (uint32_t usage = first; usage <= last; ++usage) {
                auto zero = initial(b.ReportID), ones = zero;
                USAGE u = static_cast<USAGE>(usage);
                ULONG n = 1;
                NTSTATUS clear_status = HidP_UnsetUsages(t.value, b.UsagePage, b.LinkCollection, &u, &n, pp, zero.data(), t.length);
                ones = zero;
                n = 1;
                NTSTATUS set_status = HidP_SetUsages(t.value, b.UsagePage, b.LinkCollection, &u, &n, pp, ones.data(), t.length);
                ParsedField f;
                f.report_id = b.ReportID;
                f.usage_page = b.UsagePage;
                f.usage = static_cast<int>(usage);
                f.bit_size = 1;
                f.report_count = 1;
                f.logical_max = 1;
                f.is_variable = true;
                f.is_constant = (b.BitField & 1) != 0;
                f.is_relative = !b.IsAbsolute;
                f.main_item_flags = b.BitField;
                f.has_null_state = (b.BitField & 0x40) != 0;
                if ((clear_status != HIDP_STATUS_SUCCESS && clear_status != HIDP_STATUS_BUTTON_NOT_PRESSED) ||
                    set_status != HIDP_STATUS_SUCCESS || !save(f, zero, ones, 1)) {
                    info.layout_errors.push_back(std::string(t.name) + " button offset could not be verified: " + usage_name(f.usage_page, f.usage));
                }
            }
        }
        for (const auto& v : t.values) {
            if (v.IsAlias || !v.BitSize) {
                info.layout_errors.push_back(std::string(t.name) + " aliased or zero-width value requires the physical descriptor");
                continue;
            }
            uint32_t first = v.IsRange ? v.Range.UsageMin : v.NotRange.Usage;
            uint32_t last = v.IsRange ? v.Range.UsageMax : v.NotRange.Usage;
            for (uint32_t usage = first; usage <= last; ++usage) {
                ParsedField f;
                f.report_id = v.ReportID;
                f.usage_page = v.UsagePage;
                f.usage = static_cast<int>(usage);
                f.bit_size = v.BitSize;
                f.report_count = !v.IsRange && v.ReportCount > 1 ? v.ReportCount : 1;
                f.logical_min = v.LogicalMin;
                f.logical_max = v.LogicalMin < 0 ? static_cast<int64_t>(v.LogicalMax) : static_cast<uint32_t>(v.LogicalMax);
                f.physical_min = v.PhysicalMin;
                f.physical_max = v.PhysicalMin < 0 ? static_cast<int64_t>(v.PhysicalMax) : static_cast<uint32_t>(v.PhysicalMax);
                f.unit = v.Units;
                f.unit_exponent = static_cast<int32_t>(v.UnitsExp);
                f.is_signed = v.LogicalMin < 0;
                f.is_variable = (v.BitField & 2) != 0;
                f.is_constant = (v.BitField & 1) != 0;
                f.is_relative = !v.IsAbsolute;
                f.has_null_state = v.HasNull != FALSE;
                f.main_item_flags = v.BitField;
                auto zero = initial(v.ReportID), ones = zero;
                NTSTATUS a = HIDP_STATUS_NOT_IMPLEMENTED, b = HIDP_STATUS_NOT_IMPLEMENTED;
                uint64_t total = static_cast<uint64_t>(f.bit_size) * f.report_count;
                if (f.report_count == 1 && f.bit_size <= 32) {
                    a = HidP_SetUsageValue(t.value,v.UsagePage,v.LinkCollection,static_cast<USAGE>(usage),0,pp,zero.data(),t.length);
                    ULONG mask = f.bit_size == 32 ? UINT32_MAX : (1u << f.bit_size) - 1;
                    b = HidP_SetUsageValue(t.value,v.UsagePage,v.LinkCollection,static_cast<USAGE>(usage),mask,pp,ones.data(),t.length);
                } else if ((total + 7) / 8 <= USHRT_MAX) {
                    USHORT bytes = static_cast<USHORT>((total + 7) / 8);
                    std::vector<char> values(bytes, 0);
                    a = HidP_SetUsageValueArray(t.value,v.UsagePage,v.LinkCollection,static_cast<USAGE>(usage),values.data(),bytes,pp,zero.data(),t.length);
                    std::fill(values.begin(), values.end(), static_cast<char>(0xFF));
                    if (total % 8) values.back() = static_cast<char>((1u << (total % 8)) - 1);
                    b = HidP_SetUsageValueArray(t.value,v.UsagePage,v.LinkCollection,static_cast<USAGE>(usage),values.data(),bytes,pp,ones.data(),t.length);
                }
                if (a != HIDP_STATUS_SUCCESS || b != HIDP_STATUS_SUCCESS || !save(f, zero, ones, static_cast<uint32_t>(total))) {
                    info.layout_errors.push_back(std::string(t.name) + " value offset/width could not be verified: " + usage_name(f.usage_page, f.usage));
                }
            }
        }
    }
}

static WindowsHidInfo query_windows_hid(const std::wstring& path) {
    WindowsHidInfo info;
    info.capture_os_version = ppd_current_os_version();
    static const ULONG api_version = [] { ULONG v=0; return HidP_GetVersion(&v)==HIDP_STATUS_SUCCESS?v:ULONG(0); }();
    info.hid_api_version = api_version;
    PpdHandleOwner handle(open_handle_w(path,0));
    HANDLE h=handle.value;
    if (!h || h==INVALID_HANDLE_VALUE) {
        info.error = win32_error_text(GetLastError());
        if(ppd_acquire_raw_input(path,info))info.ppd=decode_windows_ppd(info.cached_collection_descriptor_bytes);
        return info;
    }

    info.attributes.Size = sizeof(HIDD_ATTRIBUTES);
    HidD_GetAttributes(h, &info.attributes);
    info.manufacturer = hid_get_string(h, HidD_GetManufacturerString);
    info.product = hid_get_string(h, HidD_GetProductString);
    info.serial_number = hid_get_string(h, HidD_GetSerialNumberString);

    if(!ppd_acquire_ioctl(h,info))ppd_acquire_raw_input(path,info);
    if(!info.cached_collection_descriptor_bytes.empty())info.ppd=decode_windows_ppd(info.cached_collection_descriptor_bytes);
    PpdApiOwner owned_pp;
    if (HidD_GetPreparsedData(h, &owned_pp.value) && owned_pp.value) {
        const PHIDP_PREPARSED_DATA pp_data=owned_pp.value;
        if (HidP_GetCaps(pp_data, &info.caps) == HIDP_STATUS_SUCCESS) {
            info.available = true;
            info.public_caps_complete=true;
            info.public_type_caps_complete.fill(true);
            info.public_links_complete=true;
            auto get_buttons=[&](HIDP_REPORT_TYPE type,USHORT count,std::vector<HIDP_BUTTON_CAPS>& caps) {
                if(!count)return;caps.resize(count);USHORT actual=count;
                const NTSTATUS status=HidP_GetButtonCaps(type,caps.data(),&actual,pp_data);
                if(status!=HIDP_STATUS_SUCCESS||actual!=count) {caps.clear();info.public_caps_complete=false;info.public_type_caps_complete[type]=false;info.layout_errors.push_back("Button capability retrieval failed for "+std::string(ppd_type_name(type)));}
                else caps.resize(actual);
            };
            auto get_values=[&](HIDP_REPORT_TYPE type,USHORT count,std::vector<HIDP_VALUE_CAPS>& caps) {
                if(!count)return;caps.resize(count);USHORT actual=count;
                const NTSTATUS status=HidP_GetValueCaps(type,caps.data(),&actual,pp_data);
                if(status!=HIDP_STATUS_SUCCESS||actual!=count) {caps.clear();info.public_caps_complete=false;info.public_type_caps_complete[type]=false;info.layout_errors.push_back("Value capability retrieval failed for "+std::string(ppd_type_name(type)));}
                else caps.resize(actual);
            };
            get_buttons(HidP_Input,info.caps.NumberInputButtonCaps,info.input_button_caps);
            get_buttons(HidP_Output,info.caps.NumberOutputButtonCaps,info.output_button_caps);
            get_buttons(HidP_Feature,info.caps.NumberFeatureButtonCaps,info.feature_button_caps);
            get_values(HidP_Input,info.caps.NumberInputValueCaps,info.input_value_caps);
            get_values(HidP_Output,info.caps.NumberOutputValueCaps,info.output_value_caps);
            get_values(HidP_Feature,info.caps.NumberFeatureValueCaps,info.feature_value_caps);
            if (info.caps.NumberLinkCollectionNodes > 0) {
                info.link_collections.resize(info.caps.NumberLinkCollectionNodes);
                ULONG num = info.caps.NumberLinkCollectionNodes;
                const NTSTATUS status=HidP_GetLinkCollectionNodes(info.link_collections.data(), &num, pp_data);
                if(status!=HIDP_STATUS_SUCCESS||num!=info.link_collections.size()) {
                    info.link_collections.clear();info.public_caps_complete=false;info.public_links_complete=false;info.layout_errors.push_back("Link collection retrieval failed");
                } else info.link_collections.resize(num);
            }
        } else info.error="HidP_GetCaps failed for the API-owned preparsed data";
        if (info.available) { observe_windows_layout(info, pp_data); ppd_validate_windows(info,pp_data); }
    } else {
        info.error = "HidD_GetPreparsedData failed: " + win32_error_text(GetLastError());
    }
    return info;
}

static std::vector<uint8_t> reconstruct_report_descriptor_from_hid(const WindowsHidInfo& wh) {
    if (!wh.available || !wh.layout_errors.empty() || wh.observed_fields.empty()) return {};
    std::vector<uint8_t> out;
    add_hid_item(out, 0x04, wh.caps.UsagePage);
    add_hid_item(out, 0x08, wh.caps.Usage);
    out.insert(out.end(), {0xA1, 0x01});
    std::map<std::pair<std::string, uint8_t>, std::vector<ParsedField>> groups;
    for (const auto& f : wh.observed_fields) groups[{f.report_type, f.report_id}].push_back(f);
    for (auto& group : groups) {
        const std::string& type = group.first.first;
        uint8_t rid = group.first.second;
        uint8_t tag = type == "Input" ? 0x80 : type == "Output" ? 0x90 : 0xB0;
        USHORT length = type == "Input" ? wh.caps.InputReportByteLength : type == "Output" ? wh.caps.OutputReportByteLength : wh.caps.FeatureReportByteLength;
        auto fields = group.second;
        std::sort(fields.begin(), fields.end(), [](const ParsedField& a, const ParsedField& b) {
            return a.payload_bit_offset < b.payload_bit_offset;
        });
        out.push_back(0xA4);
        if (rid) add_hid_item(out, 0x84, rid);
        uint32_t offset = 0;
        auto padding = [&](uint32_t count) {
            if (!count) return;
            add_hid_item(out, 0x74, 1);
            add_hid_item(out, 0x94, static_cast<int32_t>(count));
            add_hid_item(out, tag, 1);
        };
        for (const auto& f : fields) {
            if (f.payload_bit_offset < offset) return {};
            padding(f.payload_bit_offset - offset);
            add_hid_item(out, 0x04, f.usage_page);
            add_hid_item(out, 0x08, f.usage);
            add_hid_item(out, 0x14, static_cast<int32_t>(f.logical_min), true);
            add_hid_item(out, 0x24, static_cast<int32_t>(f.logical_max), f.logical_min < 0);
            add_hid_item(out, 0x34, static_cast<int32_t>(f.physical_min), true);
            add_hid_item(out, 0x44, static_cast<int32_t>(f.physical_max), f.physical_min < 0);
            add_hid_item(out, 0x54, f.unit_exponent, true);
            add_hid_item(out, 0x64, static_cast<int32_t>(f.unit));
            add_hid_item(out, 0x74, static_cast<int32_t>(f.bit_size));
            add_hid_item(out, 0x94, static_cast<int32_t>(f.report_count));
            add_hid_item(out, tag, static_cast<int32_t>(f.main_item_flags));
            offset = f.payload_bit_offset + f.bit_size * f.report_count;
        }
        uint32_t maximum_payload_bits = length ? static_cast<uint32_t>(length - 1) * 8 : 0;
        if (offset > maximum_payload_bits) return {};
        padding(maximum_payload_bits - offset);
        out.push_back(0xB4);
    }
    out.push_back(0xC0);
    return out;
}
struct EndpointDesc {
    uint8_t address = 0;
    std::string direction;
    std::string transfer_type;
    uint16_t max_packet_size = 0;
    uint8_t interval = 0;
};

struct InterfaceDesc {
    int interface_number = 0;
    int alternate_setting = 0;
    int interface_class = 0;
    int interface_subclass = 0;
    int interface_protocol = 0;
    std::string raw_descriptor;
    std::vector<EndpointDesc> endpoints;
    std::vector<std::pair<uint8_t, uint16_t>> hid_descriptors; // type, length
};

struct ConfigurationDesc {
    uint8_t config_value = 0;
    std::string raw_descriptor;
    std::vector<InterfaceDesc> interfaces;
};

struct PhysicalUsbMatch {
    std::wstring hub_path;
    std::wstring hub_instance_id;
    std::wstring instance_id;
    std::string driver_key;
    std::string device_descriptor_hex;
    ULONG port = 0;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint8_t current_configuration_value = 0;
    std::vector<ConfigurationDesc> configurations;
    std::map<int, std::vector<uint8_t>> raw_report_descriptors;
    std::map<int, std::pair<DWORD, std::string>> report_descriptor_errors;
};

static void parse_configuration_interfaces(const std::vector<uint8_t>& raw, ConfigurationDesc& config) {
    if (raw.size() < 9 || raw[1] != 0x02) return;
    config.config_value = raw[5];
    config.raw_descriptor = to_hex(raw.data(), raw.size());

    size_t cursor = 0;
    InterfaceDesc* cur_iface = nullptr;
    while (cursor + 2 <= raw.size()) {
        uint8_t len = raw[cursor];
        uint8_t dtype = raw[cursor + 1];
        if (len < 2 || cursor + len > raw.size()) break;

        if (dtype == 0x04 && len >= 9) { // Interface
            InterfaceDesc iface;
            iface.interface_number = raw[cursor + 2];
            iface.alternate_setting = raw[cursor + 3];
            iface.interface_class = raw[cursor + 5];
            iface.interface_subclass = raw[cursor + 6];
            iface.interface_protocol = raw[cursor + 7];
            iface.raw_descriptor = to_hex(raw.data() + cursor, len);
            config.interfaces.push_back(iface);
            cur_iface = &config.interfaces.back();
        } else if (dtype == 0x05 && len >= 7 && cur_iface) { // Endpoint
            EndpointDesc ep;
            ep.address = raw[cursor + 2];
            ep.direction = (ep.address & 0x80) ? "IN" : "OUT";
            uint8_t xfer = raw[cursor + 3] & 0x03;
            ep.transfer_type = (xfer == 0) ? "Control" : (xfer == 1 ? "Isochronous" : (xfer == 2 ? "Bulk" : "Interrupt"));
            ep.max_packet_size = *reinterpret_cast<const uint16_t*>(raw.data() + cursor + 4);
            ep.interval = raw[cursor + 6];
            cur_iface->endpoints.push_back(ep);
        } else if (dtype == 0x21 && len >= 6 && cur_iface) { // HID
            uint8_t count = raw[cursor + 5];
            size_t c = cursor + 6;
            for (uint8_t i = 0; i < count && c + 3 <= cursor + len; ++i) {
                uint8_t desc_type = raw[c];
                uint16_t desc_len = *reinterpret_cast<const uint16_t*>(raw.data() + c + 1);
                cur_iface->hid_descriptors.emplace_back(desc_type, desc_len);
                c += 3;
            }
        }
        cursor += len;
    }
}

static std::vector<PhysicalUsbMatch> scan_physical_usb(uint16_t vid, uint16_t pid) {
    std::vector<PhysicalUsbMatch> matches;
    auto hubs = enumerate_device_interfaces(GUID_DEVINTERFACE_USB_HUB_CONST);
    auto hcs = enumerate_device_interfaces(GUID_DEVINTERFACE_USB_HOST_CONTROLLER_CONST);
    std::set<std::wstring> seen_hubs;
    for (const auto& h : hubs) seen_hubs.insert(str_tolower(wstring_to_utf8(h.path)) == "" ? L"" : h.path);
    for (const auto& hc : hcs) {
        if (!seen_hubs.count(hc.path)) {
            hubs.push_back(hc);
            seen_hubs.insert(hc.path);
        }
    }

    for (const auto& hub_entry : hubs) {
        HANDLE hHub = nullptr;
        DWORD access_modes[] = { 0, GENERIC_READ, GENERIC_READ | GENERIC_WRITE };
        for (DWORD acc : access_modes) {
            hHub = open_handle_w(hub_entry.path, acc);
            if (hHub) break;
        }
        if (!hHub) continue;

        int ports = hub_port_count(hHub);
        for (int port = 1; port <= ports; ++port) {
            std::vector<BYTE> conn_buf(4096, 0);
            *reinterpret_cast<ULONG*>(conn_buf.data()) = port;
            DWORD returned = 0;
            if (!DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, conn_buf.data(), (DWORD)conn_buf.size(), conn_buf.data(), (DWORD)conn_buf.size(), &returned, nullptr) || returned < 36) {
                continue;
            }
            const uint8_t* desc_raw = reinterpret_cast<const uint8_t*>(conn_buf.data() + 4);
            if (desc_raw[0] < 18 || desc_raw[1] != 0x01) continue;
            uint16_t dev_vid = *reinterpret_cast<const uint16_t*>(desc_raw + 8);
            uint16_t dev_pid = *reinterpret_cast<const uint16_t*>(desc_raw + 10);
            if (dev_vid != vid || dev_pid != pid) continue;

            PhysicalUsbMatch match;
            match.hub_path = hub_entry.path;
            match.hub_instance_id = hub_entry.instance_id;
            match.port = port;
            match.vid = dev_vid;
            match.pid = dev_pid;
            match.current_configuration_value = conn_buf[22];
            match.device_descriptor_hex = to_hex(desc_raw, 18);
            std::vector<BYTE> driver_buffer(8192, 0);
            auto* driver = reinterpret_cast<PUSB_NODE_CONNECTION_DRIVERKEY_NAME>(driver_buffer.data());
            driver->ConnectionIndex = port;
            DWORD driver_returned = 0;
            if (DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME, driver_buffer.data(),
                    static_cast<DWORD>(driver_buffer.size()), driver_buffer.data(), static_cast<DWORD>(driver_buffer.size()),
                    &driver_returned, nullptr) && driver_returned >= offsetof(USB_NODE_CONNECTION_DRIVERKEY_NAME, DriverKeyName) + sizeof(wchar_t)) {
                size_t capacity = (driver_returned - offsetof(USB_NODE_CONNECTION_DRIVERKEY_NAME, DriverKeyName)) / sizeof(wchar_t);
                capacity = (std::min)(capacity, (driver_buffer.size() - offsetof(USB_NODE_CONNECTION_DRIVERKEY_NAME, DriverKeyName)) / sizeof(wchar_t));
                size_t length = 0;
                while (length < capacity && driver->DriverKeyName[length]) ++length;
                match.driver_key = wstring_to_utf8(std::wstring(driver->DriverKeyName, length));
            }

            uint8_t num_configs = desc_raw[17];
            for (uint8_t ci = 0; ci < num_configs; ++ci) {
                std::vector<uint8_t> header;
                DWORD err = 0;
                if (!hub_get_descriptor(hHub, port, 0x80, 0x06, (0x02 << 8) | ci, 0, 9, header, err) || header.size() < 9)
                    continue;
                uint16_t total_len = *reinterpret_cast<const uint16_t*>(header.data() + 2);
                std::vector<uint8_t> full_blob;
                if (!hub_get_descriptor(hHub, port, 0x80, 0x06, (0x02 << 8) | ci, 0, total_len, full_blob, err))
                    continue;

                ConfigurationDesc conf;
                parse_configuration_interfaces(full_blob, conf);
                match.configurations.push_back(conf);

                if (conf.config_value == match.current_configuration_value) {
                    for (const auto& iface : conf.interfaces) {
                        for (const auto& hd : iface.hid_descriptors) {
                            if (hd.first == 0x22 && hd.second > 0) {
                                std::vector<uint8_t> rpt_data;
                                DWORD rpt_err = 0;
                                if (hub_get_descriptor(hHub, port, 0x81, 0x06, (0x22 << 8), iface.interface_number, hd.second, rpt_data, rpt_err)) {
                                    match.raw_report_descriptors[iface.interface_number] = rpt_data;
                                } else {
                                    match.report_descriptor_errors[iface.interface_number] = { rpt_err, win32_error_text(rpt_err) };
                                }
                            }
                        }
                    }
                }
            }
            matches.push_back(match);
        }
        CloseHandle(hHub);
    }
    return matches;
}

static std::string instance_key(const std::wstring& value) {
    return str_tolower(wstring_to_utf8(value));
}

struct HidTransportIdentity {
    int interface_number=-1;
    bool logical=false;
    bool conflicting=false;
};

static HidTransportIdentity hid_transport_identity(const HidCollectionEntry& collection) {
    HidTransportIdentity result;
    auto chain=collection.base.ancestor_instance_ids;
    chain.insert(chain.begin(),collection.base.parent_instance_id);
    chain.insert(chain.begin(),collection.base.instance_id);
    for(const auto& id:chain) {
        const std::string key=instance_key(id);
        if(key.find("&ig_")!=std::string::npos||key.find("bth")==0)result.logical=true;
        const size_t mi=key.find("&mi_");
        if(mi!=std::string::npos&&mi+6<=key.size()&&std::isxdigit(static_cast<unsigned char>(key[mi+4]))&&std::isxdigit(static_cast<unsigned char>(key[mi+5]))) {
            const int value=static_cast<int>(strtoul(key.substr(mi+4,2).c_str(),nullptr,16));
            if(result.interface_number>=0&&result.interface_number!=value)result.conflicting=true;
            else result.interface_number=value;
        }
    }
    return result;
}

static const UsbDeviceEntry* physical_parent(const HidCollectionEntry& h, const std::vector<UsbDeviceEntry>& devices) {
    std::vector<std::wstring> chain = {h.base.instance_id};
    chain.insert(chain.end(), h.base.ancestor_instance_ids.begin(), h.base.ancestor_instance_ids.end());
    for (const auto& id : chain) {
        std::string key = instance_key(id);
        if (key.find("&mi_") != std::string::npos || key.find("&ig_") != std::string::npos) continue;
        for (const auto& u : devices) {
            if (!key.empty() && key == instance_key(u.base.instance_id)) return &u;
        }
    }
    return nullptr;
}

static void associate_physical_instances(std::vector<PhysicalUsbMatch>& matches, const std::vector<UsbDeviceEntry>& devices) {
    for (auto& p : matches) {
        std::vector<const UsbDeviceEntry*> candidates;
        for (const auto& u : devices) {
            if (u.vid != p.vid || u.pid != p.pid) continue;
            bool driver_match = !p.driver_key.empty() && str_tolower(p.driver_key) == str_tolower(u.base.driver_key);
            bool port_match = u.base.has_address && u.base.address_property == p.port &&
                !p.hub_instance_id.empty() && instance_key(u.base.parent_instance_id) == instance_key(p.hub_instance_id);
            if (driver_match || port_match) candidates.push_back(&u);
        }
        if (candidates.size() == 1) p.instance_id = candidates.front()->base.instance_id;
    }
}

static int physical_index_for(const HidCollectionEntry& h, const std::vector<UsbDeviceEntry>& devices,
                              const std::vector<PhysicalUsbMatch>& matches) {
    const auto* parent = physical_parent(h, devices);
    if (!parent) return -1;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (!matches[i].instance_id.empty() && instance_key(parent->base.instance_id) == instance_key(matches[i].instance_id)) return static_cast<int>(i);
    }
    return -1;
}

struct ProtocolField {
    ParsedField field;
    SemanticField semantic;
    bool little_endian = false;
    bool active_low = false;
    std::vector<ProtocolField> fields;
};

struct PacketCondition {
    uint32_t byte_offset = 0;
    uint32_t mask = 255;
    uint32_t value = 0;
};

struct ProtocolReport {
    std::string selector = "input_state";
    int report_id = -1;
    uint32_t minimum_wire_bytes = 0;
    uint32_t exact_wire_bytes = 0;
    uint32_t header_bytes = 0;
    bool gip_length = false;
    bool basic_input_semantics_complete = false;
    bool extended_input_semantics_complete = false;
    std::vector<PacketCondition> conditions;
    std::vector<ProtocolField> fields;
};

struct ProtocolOutputReport {
    uint8_t report_id = 0;
    uint32_t known_protocol_wire_bytes = 0;
};

struct ProtocolProfile {
    std::string name;
    uint16_t source_vid = 0;
    uint16_t source_pid = 0;
    std::vector<std::string> sources;
    std::vector<std::string> notes;
    std::vector<ProtocolReport> reports;
    std::vector<ProtocolOutputReport> output_reports;
};

static ProtocolField& add_protocol_field(ProtocolReport& report, SemanticFieldType type, const char* role,
                               uint32_t wire_bit, uint32_t width, int64_t minimum, int64_t maximum) {
    ProtocolField p;
    p.field.report_type = "Input";
    p.field.report_id = report.report_id > 0 ? static_cast<uint8_t>(report.report_id) : 0;
    p.field.wire_bit_offset = wire_bit;
    p.field.payload_bit_offset = wire_bit - report.header_bytes * 8;
    p.field.bit_size = width;
    p.field.report_count = 1;
    p.field.logical_min = minimum;
    p.field.logical_max = maximum;
    p.field.is_signed = minimum < 0;
    p.field.is_variable = true;
    p.field.main_item_flags = 2;
    p.semantic.type = type;
    p.semantic.name = role;
    p.semantic.normalized_role = role;
    p.semantic.role_confidence = "known_protocol";
    report.fields.push_back(std::move(p));
    return report.fields.back();
}

static void add_dualsense_touch_point(ProtocolReport& report, const char* name, uint32_t wire_byte) {
    ProtocolReport contact;
    contact.report_id = report.report_id;
    contact.header_bytes = report.header_bytes;
    add_protocol_field(contact,SemanticFieldType::ContactId,"contact_id",wire_byte * 8,7,0,127);
    add_protocol_field(contact,SemanticFieldType::Boolean,"active",wire_byte * 8 + 7,1,0,1).active_low = true;
    add_protocol_field(contact,SemanticFieldType::Coordinate,"x",wire_byte * 8 + 8,12,0,1919).little_endian = true;
    add_protocol_field(contact,SemanticFieldType::Coordinate,"y",wire_byte * 8 + 20,12,0,1079).little_endian = true;
    auto& point = add_protocol_field(report,SemanticFieldType::TouchPoint,name,wire_byte * 8,32,0,INT64_C(4294967295));
    point.fields = std::move(contact.fields);
}

static void write_protocol_field_json(std::ostream& out, const ProtocolField& field,
                                      const LayoutEvidence& evidence, int64_t parent_bit_offset = -1) {
    write_field_json(out,field.field,&field.semantic,false,evidence,false);
    if (parent_bit_offset >= 0)
        out << ",\"relative_bit_offset\":" << static_cast<int64_t>(field.field.wire_bit_offset) - parent_bit_offset;
    if (field.little_endian) out << ",\"byte_order\":\"little_endian\",\"bit_order\":\"lsb_first\"";
    if (field.active_low)
        out << ",\"value_encoding\":\"active_low\",\"true_raw_value\":0,\"false_raw_value\":1";
    if (field.semantic.type == SemanticFieldType::Gyroscope || field.semantic.type == SemanticFieldType::Accelerometer)
        out << ",\"value_encoding\":\"twos_complement\",\"units\":\"raw_counts\",\"calibration_applied\":false";
    if (field.semantic.type == SemanticFieldType::Timestamp) out << ",\"units\":\"raw_ticks\"";
    if (field.semantic.type == SemanticFieldType::Battery || field.semantic.type == SemanticFieldType::Status)
        out << ",\"units\":\"raw_protocol_code\"";
    if (!field.fields.empty()) {
        out << ",\"value_encoding\":\"packed_structure\",\"preserve\":true,\"raw_bytes\":{\"wire_byte_offset\":"
            << field.field.wire_bit_offset / 8 << ",\"byte_count\":" << field.field.bit_size / 8
            << ",\"preserve\":true},\"data_valid_when\":{\"field\":\"active\",\"equals\":true},\"fields\":{";
        for (size_t i = 0; i < field.fields.size(); ++i) {
            if (i) out << ',';
            out << json_string(field.fields[i].semantic.name) << ':';
            write_protocol_field_json(out,field.fields[i],evidence,field.field.wire_bit_offset);
        }
        out << '}';
    }
    out << '}';
}

static void write_protocol_field_txt(std::ostream& out, const ProtocolField& field,
                                     const LayoutEvidence& evidence, const std::string& indent = "    ") {
    out << indent << field.semantic.normalized_role;
    if (!field.semantic.native_name.empty()) out << " (" << field.semantic.native_name << ')';
    out << " wire bit " << field.field.wire_bit_offset << " size " << field.field.bit_size
        << " " << (field.field.is_signed ? "signed" : "unsigned")
        << " " << field.field.logical_min << ".." << field.field.logical_max;
    if (field.little_endian) out << " little-endian";
    if (field.active_low) out << "; active-low: raw 0=true, raw 1=false";
    out << '\n';
    const auto storage = storage_range_numbers(field.field.bit_size,field.field.is_signed);
    out << indent << "  Storage range: " << storage.first << ".." << storage.second
        << "; semantic range: " << field.field.logical_min << ".." << field.field.logical_max << "; source: known_protocol\n";
    write_range_txt(out,field.field,evidence);
    if (field.semantic.type == SemanticFieldType::Gyroscope || field.semantic.type == SemanticFieldType::Accelerometer)
        out << indent << "  Raw sensor counts; calibration not applied.\n";
    if (field.semantic.type == SemanticFieldType::Timestamp) out << indent << "  Raw timestamp ticks; no time conversion applied.\n";
    if (field.semantic.type == SemanticFieldType::Battery || field.semantic.type == SemanticFieldType::Status)
        out << indent << "  Raw protocol code; no percentage or state conversion applied.\n";
    if (!field.fields.empty()) {
        out << indent << "  Packed touch contact: preserve all four bytes; associated data is valid only while active.\n";
        for (const auto& child : field.fields) write_protocol_field_txt(out,child,evidence,indent + "  ");
    }
}

static const ParsedReport* descriptor_report_by_id(const ParsedDescriptorResult* parsed, uint8_t report_id) {
    if (parsed && parsed->available)
        for (const auto& report : parsed->reports) if (report.report_id == report_id) return &report;
    return nullptr;
}

static bool sony_profile_structure(const ParsedDescriptorResult& parsed, const std::string& family,
                                   std::vector<std::string>& reasons);

static ProtocolProfile protocol_for_interface(uint16_t vid, uint16_t pid, const InterfaceDesc& iface,
                                               const ParsedDescriptorResult* parsed = nullptr) {
    ProtocolProfile profile;
    profile.source_vid = vid;
    profile.source_pid = pid;
    bool gip = iface.interface_class == 0xFF && iface.interface_subclass == 0x47 && iface.interface_protocol == 0xD0;
    bool xusb = iface.interface_class == 0xFF && iface.interface_subclass == 0x5D && iface.interface_protocol == 1;
    bool wireless = iface.interface_class == 0xFF && iface.interface_subclass == 0x5D && iface.interface_protocol == 0x81;
    if (gip || xusb || wireless) {
        profile.name = gip ? "xbox_gip" : wireless ? "xbox_360_wireless" : "xbox_xusb";
        profile.sources = {"https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c"};
        ProtocolReport report;
        report.header_bytes = gip ? 4 : wireless ? 6 : 2;
        report.minimum_wire_bytes = gip ? 18 : wireless ? 24 : 20;
        report.exact_wire_bytes = xusb ? 20 : 0;
        uint32_t start = report.header_bytes * 8;
        if (gip) {
            report.gip_length = true;
            report.conditions = {{0,255,0x20},{1,0xC0,0},{3,0x80,0}};
            profile.sources.push_back("https://github.com/medusalix/xone/blob/master/bus/protocol.c");
            profile.notes.push_back("Offsets apply to unchunked packets with a one-byte payload length. Reassemble chunked or extended headers before using payload offsets.");
            profile.notes.push_back("Share and Elite paddle extensions vary by device and firmware; unparsed packet tails must be preserved.");
        } else if (wireless) report.conditions = {{1,255,1},{4,255,0},{5,255,20}};
        else report.conditions = {{0,255,0},{1,255,20}};
        const char* gip_buttons[] = {nullptr,nullptr,"menu","view","button_south","button_east","button_west","button_north",
            "dpad_up","dpad_down","dpad_left","dpad_right","left_shoulder","right_shoulder","left_stick_click","right_stick_click"};
        const char* xusb_buttons[] = {"dpad_up","dpad_down","dpad_left","dpad_right","menu","view","left_stick_click","right_stick_click",
            "left_shoulder","right_shoulder","guide",nullptr,"button_south","button_east","button_west","button_north"};
        for (uint32_t bit = 0; bit < 16; ++bit) {
            const char* role = gip ? gip_buttons[bit] : xusb_buttons[bit];
            if (role) add_protocol_field(report,SemanticFieldType::Button,role,start + bit,1,0,1);
        }
        uint32_t trigger_bits = gip ? 16 : 8;
        add_protocol_field(report,SemanticFieldType::Trigger,"left_trigger",start + 16,trigger_bits,0,gip ? 1023 : 255);
        add_protocol_field(report,SemanticFieldType::Trigger,"right_trigger",start + 16 + trigger_bits,trigger_bits,0,gip ? 1023 : 255);
        const char* axes[] = {"left_stick_x","left_stick_y","right_stick_x","right_stick_y"};
        for (uint32_t i = 0; i < 4; ++i) add_protocol_field(report,SemanticFieldType::Axis,axes[i],start + 16 + trigger_bits * 2 + i * 16,16,-32768,32767);
        profile.reports.push_back(std::move(report));
        if (gip) {
            ProtocolReport guide;
            guide.selector = "virtual_key";
            guide.header_bytes = 4;
            guide.minimum_wire_bytes = 6;
            guide.gip_length = true;
            guide.conditions = {{0,255,7},{1,0xC0,0},{3,0x80,0}};
            add_protocol_field(guide,SemanticFieldType::Button,"guide",32,2,0,3);
            profile.reports.push_back(std::move(guide));
        }
        return profile;
    }
    if (iface.interface_class != 3 || iface.interface_subclass != 0 || iface.interface_protocol != 0 || !parsed)
        return profile;
    std::string family;
    for (const char* candidate : {"dualshock3_usb", "dualshock4_usb", "dualsense_usb"}) {
        std::vector<std::string> reasons;
        if (!sony_profile_structure(*parsed, candidate, reasons)) continue;
        if (!family.empty()) {
            profile.notes.push_back("The parsed HID Input layout matches multiple protocol families; retain the generic HID layout.");
            return profile;
        }
        family = candidate;
    }
    if (family.empty()) return profile;
    const bool ds3 = family == "dualshock3_usb";
    const bool ds4 = family == "dualshock4_usb";
    const bool ds5 = family == "dualsense_usb";
    profile.name = family;
    profile.notes.push_back("Protocol family selected from the parsed HID Input layout. VID/PID record the device identity and do not select or restrict the family.");
    profile.sources = {ds3 ? "https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps3.c" :
        "https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c"};
    if (ds5) {
        profile.sources = {"https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c"};
        profile.notes.push_back("Known DualSense USB report 1 semantics take precedence for mapped fields; exact HID descriptor bytes and generic fields remain supporting physical evidence. Bluetooth envelopes are not covered.");
        profile.notes.push_back("Extended completeness covers the common sequence, raw motion sensors, timestamp, two packed touch contacts, battery/charging codes and headphone/microphone detection and mute status. Touch X/Y semantic ranges are 0..1919 and 0..1079; both retain 12-bit storage 0..4095 without clipping. It does not assign meanings to unknown bits or apply calibration, coordinate scaling, or battery percentage conversion.");
        profile.notes.push_back("Report 2 descriptor Output length and the known 63-byte USB transport form are separate. Output effects and Feature semantics remain undecoded and must be preserved.");
        profile.output_reports.push_back({2,63});
    } else profile.notes.push_back("USB report 1 mapping; Bluetooth packets use different envelopes. Raw HID layouts remain authoritative for additional fields.");
    if (ds5) profile.notes.push_back("This common DualSense-compatible layout does not identify a manufacturer or distinguish an Edge variant. Variant-specific controls remain raw until their layout is proven.");
    ProtocolReport report;
    report.report_id = 1;
    report.header_bytes = 1;
    report.minimum_wire_bytes = report.exact_wire_bytes = ds3 ? 49 : 64;
    report.conditions = {{0,255,1}};
    const char* axes[] = {"left_stick_x","left_stick_y","right_stick_x","right_stick_y"};
    for (uint32_t i = 0; i < 4; ++i) add_protocol_field(report,SemanticFieldType::Axis,axes[i],((ds3 ? 6 : 1) + i) * 8,8,0,255);
    uint32_t trigger_start = ds3 ? 18 : ds4 ? 8 : 5;
    add_protocol_field(report,SemanticFieldType::Trigger,"left_trigger",trigger_start * 8,8,0,255);
    add_protocol_field(report,SemanticFieldType::Trigger,"right_trigger",(trigger_start + 1) * 8,8,0,255);
    if (ds5) add_protocol_field(report,SemanticFieldType::SequenceNumber,"sequence_number",7 * 8,8,0,255);
    if (ds3) {
        const char* buttons[] = {"view","left_stick_click","right_stick_click","menu","dpad_up","dpad_right","dpad_down","dpad_left",
            "left_trigger_button","right_trigger_button","left_shoulder","right_shoulder","button_north","button_east","button_south","button_west","guide"};
        for (uint32_t i = 0; i < 17; ++i) add_protocol_field(report,SemanticFieldType::Button,buttons[i],16 + i,1,0,1);
    } else {
        uint32_t buttons_start = (ds4 ? 5 : 8) * 8;
        add_protocol_field(report,SemanticFieldType::Hat,"dpad",buttons_start,4,0,7);
        report.fields.back().field.has_null_state = true;
        const char* buttons[] = {"button_west","button_south","button_east","button_north","left_shoulder","right_shoulder",
            "left_trigger_button","right_trigger_button","view","menu","left_stick_click","right_stick_click","guide","touchpad_click","microphone_mute"};
        const char* native_buttons[] = {"square","cross","circle","triangle","l1","r1","l2_button","r2_button",
            "create","options","l3","r3","ps_home","touchpad_click","microphone_mute"};
        for (uint32_t i = 0; i < (ds4 ? 14u : 15u); ++i) {
            auto& button = add_protocol_field(report,SemanticFieldType::Button,buttons[i],buttons_start + 4 + i,1,0,1);
            if (ds5) button.semantic.native_name = native_buttons[i];
        }
    }
    if (ds5) {
        report.basic_input_semantics_complete = true;
        const char* gyro[] = {"gyro_x","gyro_y","gyro_z"};
        const char* accel[] = {"accel_x","accel_y","accel_z"};
        for (uint32_t i = 0; i < 3; ++i)
            add_protocol_field(report,SemanticFieldType::Gyroscope,gyro[i],(16 + i * 2) * 8,16,-32768,32767).little_endian = true;
        for (uint32_t i = 0; i < 3; ++i)
            add_protocol_field(report,SemanticFieldType::Accelerometer,accel[i],(22 + i * 2) * 8,16,-32768,32767).little_endian = true;
        add_protocol_field(report,SemanticFieldType::Timestamp,"sensor_timestamp",28 * 8,32,0,INT64_C(4294967295)).little_endian = true;
        add_dualsense_touch_point(report,"touch_point_1",33);
        add_dualsense_touch_point(report,"touch_point_2",37);
        add_protocol_field(report,SemanticFieldType::Battery,"battery_level",53 * 8,4,0,15);
        add_protocol_field(report,SemanticFieldType::Status,"charging_status",53 * 8 + 4,4,0,15);
        add_protocol_field(report,SemanticFieldType::Status,"headphone_detect",54 * 8,1,0,1);
        add_protocol_field(report,SemanticFieldType::Status,"microphone_detect",54 * 8 + 1,1,0,1);
        add_protocol_field(report,SemanticFieldType::Status,"microphone_mute_status",54 * 8 + 2,1,0,1);
        report.extended_input_semantics_complete = true;
    }
    profile.reports.push_back(std::move(report));
    return profile;
}

struct DescriptorVerification {
    std::string status = "unavailable";
    uint32_t observed_fields_checked = 0;
    bool all_offsets_verified = false;
    std::vector<std::string> differences;
    uint32_t constant_elements_checked = 0;
};

static bool field_contains_usage(const ParsedField& f, int page, int usage) {
    if (f.is_constant) return false;
    if (f.is_variable) return f.usage_page == page && f.usage == usage;
    for (const auto& r : f.usage_ranges) {
        if (r.usage_page == page && usage >= r.minimum && usage <= r.maximum) return true;
    }
    return false;
}

static DescriptorVerification verify_descriptor(const ParsedDescriptorResult& parsed, const WindowsHidInfo& wh,
                                                int report_type = -1, int report_id = -1) {
    DescriptorVerification result;
    if (!parsed.available || !wh.available) return result;
    std::set<int> roots;
    for (const auto& c : parsed.collections) {
        if (c.parent_ordinal == 0 && c.usage_page == wh.caps.UsagePage && c.usage == wh.caps.Usage) roots.insert(c.ordinal);
    }
    if (roots.empty()) result.differences.push_back("Windows top-level collection usage is missing from descriptor");
    auto relevant = [&](const ParsedField& f) { return roots.count(f.top_level_collection_ordinal) != 0; };
    auto selected = [&](const std::string& type, int id) {
        return (report_type < 0 || type == ppd_type_name(report_type)) && (report_id < 0 || id == report_id);
    };
    for (const auto& observed : wh.observed_fields) {
        if (!selected(observed.report_type, observed.report_id)) continue;
        const bool constant = observed.is_constant || (observed.main_item_flags & 1) != 0;
        for (uint32_t element = 0; element < observed.report_count; ++element) {
            bool matched = false;
            const uint64_t offset = uint64_t(observed.wire_bit_offset) + uint64_t(element) * observed.bit_size;
            for (const auto& f : parsed.fields) {
                if (!relevant(f) || f.report_type != observed.report_type || f.report_id != observed.report_id ||
                    f.uninterpreted_storage || f.is_constant != constant || !f.bit_size) continue;
                const uint64_t end = uint64_t(f.wire_bit_offset) + uint64_t(f.bit_size) * f.report_count;
                if (offset < f.wire_bit_offset || offset + observed.bit_size > end ||
                    (offset - f.wire_bit_offset) % f.bit_size != 0 || f.bit_size != observed.bit_size) continue;
                if (constant) {
                    matched = true;
                    ++result.constant_elements_checked;
                    break;
                }
                if (field_contains_usage(f,observed.usage_page,observed.usage) &&
                    f.bit_size == observed.bit_size && f.logical_min == observed.logical_min && f.logical_max == observed.logical_max &&
                    f.is_relative == observed.is_relative && f.is_variable == observed.is_variable &&
                    f.is_signed == observed.is_signed) {
                    matched = true;
                    ++result.observed_fields_checked;
                    break;
                }
            }
            if (!matched) result.differences.push_back(observed.report_type + " ID " + std::to_string(observed.report_id) +
                (constant ? " Constant storage mismatch: " : " usage/offset/width/range mismatch: ") + usage_name(observed.usage_page,observed.usage));
        }
    }
    auto check_buttons = [&](const std::vector<HIDP_BUTTON_CAPS>& caps, const char* type) {
        for (const auto& cap : caps) {
            if (!selected(type, cap.ReportID) || (cap.BitField & 1)) continue;
            uint32_t first = cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage;
            uint32_t last = cap.IsRange ? cap.Range.UsageMax : cap.NotRange.Usage;
            for (uint32_t usage = first; usage <= last; ++usage) {
                bool found = std::any_of(parsed.fields.begin(),parsed.fields.end(),[&](const ParsedField& f) {
                    return relevant(f) && f.report_type == type && f.report_id == cap.ReportID &&
                        f.is_variable == ((cap.BitField & 2) != 0) && field_contains_usage(f,cap.UsagePage,static_cast<int>(usage));
                });
                if (!found) {
                    result.differences.push_back(std::string(type) + " button/array capability usage missing: " + usage_name(cap.UsagePage,static_cast<int>(usage)));
                    break;
                }
            }
        }
    };
    check_buttons(wh.input_button_caps,"Input");
    check_buttons(wh.output_button_caps,"Output");
    check_buttons(wh.feature_button_caps,"Feature");
    uint32_t input = 0, output = 0, feature = 0;
    for (const auto& r : parsed.reports) {
        bool has_input = false, has_output = false, has_feature = false;
        for (const auto& f : r.fields) {
            if (!relevant(f)) continue;
            if (f.report_type == "Input") has_input = true;
            if (f.report_type == "Output") has_output = true;
            if (f.report_type == "Feature") has_feature = true;
        }
        uint32_t reserved_id = r.report_id ? 0 : 1;
        if (has_input) input = (std::max)(input,r.input_wire_bytes + reserved_id);
        if (has_output) output = (std::max)(output,r.output_wire_bytes + reserved_id);
        if (has_feature) feature = (std::max)(feature,r.feature_wire_bytes + reserved_id);
    }
    const std::array<uint32_t,3> maxima = {input,output,feature};
    const std::array<uint32_t,3> windows_maxima = {wh.caps.InputReportByteLength,wh.caps.OutputReportByteLength,wh.caps.FeatureReportByteLength};
    for (int type = 0; type < 3; ++type) {
        if (report_type >= 0 && report_type != type) continue;
        if (report_id < 0) {
            if (maxima[type] != windows_maxima[type]) result.differences.push_back(std::string(ppd_type_name(type)) + " maximum Windows report length differs");
        } else {
            for (const auto& r : parsed.reports) {
                if (r.report_id != report_id) continue;
                const bool belongs = std::any_of(r.fields.begin(),r.fields.end(),[&](const ParsedField& f) {
                    return relevant(f) && f.report_type == ppd_type_name(type);
                });
                const uint32_t length = type == 0 ? r.input_wire_bytes : type == 1 ? r.output_wire_bytes : r.feature_wire_bytes;
                if (belongs && length && length + (r.report_id ? 0 : 1) > windows_maxima[type])
                    result.differences.push_back(std::string(ppd_type_name(type)) + " report exceeds the Windows buffer maximum");
            }
        }
    }
    result.status = !parsed.errors.empty() ? "parse_error" : result.differences.empty() ? "match" : "mismatch";
    result.all_offsets_verified = result.status == "match" && wh.layout_errors.empty() && result.observed_fields_checked != 0;
    for (const auto& f : parsed.fields) {
        if (!relevant(f) || f.is_constant || f.uninterpreted_storage || !selected(f.report_type,f.report_id)) continue;
        bool covered = std::any_of(wh.observed_fields.begin(),wh.observed_fields.end(),[&](const ParsedField& o) {
            uint64_t end = static_cast<uint64_t>(o.wire_bit_offset) + static_cast<uint64_t>(o.bit_size) * o.report_count;
            return !o.is_constant && !(o.main_item_flags & 1) && o.report_type == f.report_type && o.report_id == f.report_id && o.usage_page == f.usage_page && o.usage == f.usage &&
                o.bit_size == f.bit_size && o.is_variable == f.is_variable && o.wire_bit_offset <= f.wire_bit_offset &&
                static_cast<uint64_t>(f.wire_bit_offset) + static_cast<uint64_t>(f.bit_size) * f.report_count <= end;
        });
        if (!covered) result.all_offsets_verified = false;
    }
    return result;
}

static void write_verification_json(std::ostream& out, const DescriptorVerification& v, bool independent) {
    out << "{\"status\":" << json_string(v.status) << ",\"independent_descriptor\":" << (independent ? "true" : "false")
        << ",\"observed_fields_checked\":" << v.observed_fields_checked
        << ",\"constant_elements_checked\":" << v.constant_elements_checked
        << ",\"all_offsets_verified\":" << (v.all_offsets_verified ? "true" : "false") << ",\"differences\":";
    write_strings_json(out,v.differences);
    out << '}';
}

static void write_windows_hid_json(std::ostream& out, const HidCollectionEntry& col, const WindowsHidInfo& wh,
                                    const ParsedDescriptorResult& parsed, bool exact) {
    out << std::dec << '{';
    write_layout_evidence_json(out, windows_logical_evidence());
    out << ",\"layout_scope\":\"windows_logical_collection\",\"firmware_layout_ready\":false"
        << ",\"available\":" << (wh.available ? "true" : "false")
        << ",\"path\":" << json_string(wstring_to_utf8(col.base.path))
        << ",\"instance_id\":" << json_string(wstring_to_utf8(col.base.instance_id))
        << ",\"parent_instance_id\":" << json_string(wstring_to_utf8(col.base.parent_instance_id))
        << ",\"ancestor_instance_ids\":[";
    for (size_t i = 0; i < col.base.ancestor_instance_ids.size(); ++i) {
        if (i) out << ',';
        out << json_string(wstring_to_utf8(col.base.ancestor_instance_ids[i]));
    }
    out << "],\"vid\":" << json_string(hex_number(col.vid)) << ",\"pid\":" << json_string(hex_number(col.pid))
        << ",\"product\":" << json_string(col.product) << ",\"manufacturer\":" << json_string(col.manufacturer)
        << ",\"serial_number\":" << json_string(col.serial_number) << ",\"usage_page\":" << wh.caps.UsagePage
        << ",\"usage\":" << wh.caps.Usage << ",\"input_report_byte_length\":" << wh.caps.InputReportByteLength
        << ",\"output_report_byte_length\":" << wh.caps.OutputReportByteLength
        << ",\"feature_report_byte_length\":" << wh.caps.FeatureReportByteLength
        << ",\"error\":" << json_string(wh.error) << ",\"layout_errors\":";
    write_strings_json(out,wh.layout_errors);
    out << ",\"cached_collection_descriptor_hex\":" << json_string(to_hex(wh.cached_collection_descriptor_bytes.data(),wh.cached_collection_descriptor_bytes.size()))
        << ",\"cached_collection_descriptor_kind\":\"windows_preparsed_data\",\"preparsed_data\":";
    write_ppd_metadata_json(out,wh);
    out << ",\"button_caps\":[";
    bool first = true;
    auto buttons = [&](const std::vector<HIDP_BUTTON_CAPS>& caps, const char* type) {
        for (const auto& b : caps) {
            if (!first) out << ',';
            first = false;
            out << "{\"report_type\":" << json_string(type) << ",\"report_id\":" << static_cast<unsigned>(b.ReportID)
                << ",\"usage_page\":" << b.UsagePage << ",\"usage_min\":" << (b.IsRange ? b.Range.UsageMin : b.NotRange.Usage)
                << ",\"usage_max\":" << (b.IsRange ? b.Range.UsageMax : b.NotRange.Usage)
                << ",\"is_range\":" << (b.IsRange ? "true" : "false") << ",\"is_alias\":" << (b.IsAlias ? "true" : "false")
                << ",\"main_item_flags\":" << b.BitField << ",\"link_collection\":" << b.LinkCollection
                << ",\"link_usage_page\":" << b.LinkUsagePage << ",\"link_usage\":" << b.LinkUsage
                << ",\"absolute\":" << (b.IsAbsolute ? "true" : "false")
                << ",\"report_count_api_v2\":" << (wh.hid_api_version>=2?std::to_string(b.ReportCount):"null")
                << ",\"data_index_min\":" << (b.IsRange ? b.Range.DataIndexMin : b.NotRange.DataIndex)
                << ",\"data_index_max\":" << (b.IsRange ? b.Range.DataIndexMax : b.NotRange.DataIndex) << '}';
        }
    };
    buttons(wh.input_button_caps,"Input"); buttons(wh.output_button_caps,"Output"); buttons(wh.feature_button_caps,"Feature");
    out << "],\"value_caps\":[";
    first = true;
    auto values = [&](const std::vector<HIDP_VALUE_CAPS>& caps, const char* type) {
        for (const auto& v : caps) {
            if (!first) out << ',';
            first = false;
            out << "{\"report_type\":" << json_string(type) << ",\"report_id\":" << static_cast<unsigned>(v.ReportID)
                << ",\"usage_page\":" << v.UsagePage << ",\"usage_min\":" << (v.IsRange ? v.Range.UsageMin : v.NotRange.Usage)
                << ",\"usage_max\":" << (v.IsRange ? v.Range.UsageMax : v.NotRange.Usage)
                << ",\"is_range\":" << (v.IsRange ? "true" : "false") << ",\"is_alias\":" << (v.IsAlias ? "true" : "false")
                << ",\"main_item_flags\":" << v.BitField << ",\"bit_size\":" << v.BitSize << ",\"report_count\":" << v.ReportCount
                << ",\"logical_min\":" << v.LogicalMin << ",\"logical_max\":" << v.LogicalMax
                << ",\"physical_min\":" << v.PhysicalMin << ",\"physical_max\":" << v.PhysicalMax
                << ",\"unit\":" << v.Units << ",\"unit_exponent\":" << static_cast<int32_t>(v.UnitsExp)
                << ",\"null_state\":" << (v.HasNull ? "true" : "false") << ",\"link_collection\":" << v.LinkCollection
                << ",\"link_usage_page\":" << v.LinkUsagePage << ",\"link_usage\":" << v.LinkUsage
                << ",\"absolute\":" << (v.IsAbsolute ? "true" : "false") << '}';
        }
    };
    values(wh.input_value_caps,"Input"); values(wh.output_value_caps,"Output"); values(wh.feature_value_caps,"Feature");
    out << "],\"link_collections\":[";
    for (size_t i = 0; i < wh.link_collections.size(); ++i) {
        const auto& c = wh.link_collections[i];
        if (i) out << ',';
        out << "{\"index\":" << i << ",\"parent\":" << c.Parent << ",\"usage_page\":" << c.LinkUsagePage
            << ",\"usage\":" << c.LinkUsage << ",\"collection_type_value\":" << c.CollectionType << '}';
    }
    out << "],\"observed_fields\":[";
    for (size_t i = 0; i < wh.observed_fields.size(); ++i) {
        if (i) out << ',';
        write_field_json(out,wh.observed_fields[i],nullptr,true,windows_logical_evidence());
    }
    out << "],\"descriptor_verification\":";
    write_verification_json(out,verify_descriptor(parsed,wh),exact);
    out << '}';
}

static void write_protocol_json(std::ostream& out, const ProtocolProfile& profile,
                                const LayoutEvidence& evidence = protocol_definition_evidence(), bool ready = false,
                                const ParsedDescriptorResult* descriptor = nullptr) {
    if (profile.name.empty()) { out << "null"; return; }
    const bool gip = profile.name == "xbox_gip";
    const bool dualsense = profile.name == "dualsense_usb";
    out << '{';
    write_layout_evidence_json(out, evidence);
    out << ",\"definition_source\":\"known_protocol_definition\",\"definition_authority\":\"protocol_semantics\""
        << ",\"firmware_layout_ready\":" << (ready && evidence.physical_wire && !profile.reports.empty() ? "true" : "false")
        << ",\"eligible_for_firmware_parser_generation\":" << (ready && evidence.physical_wire && !profile.reports.empty() ? "true" : "false")
        << ",\"transport\":\"usb\",\"requires_reassembly_before_parse\":" << (gip ? "true" : "false")
        << ",\"default_selector\":" << (profile.reports.empty() ? "null" : json_string(profile.reports.front().selector))
        << ",\"header_bytes\":" << (profile.reports.empty() ? "null" : std::to_string(profile.reports.front().header_bytes))
        << ",\"minimum_wire_bytes\":" << (profile.reports.empty() ? "null" : std::to_string(profile.reports.front().minimum_wire_bytes))
        << ",\"protocol\":" << json_string(profile.name)
        << ",\"classification\":" << json_string(profile.reports.empty() ? "unknown" : "gamepad")
        << ",\"coverage\":" << json_string(dualsense ? "known_basic_and_extended_input_fields" : "known_input_controls")
        << ",\"sources\":";
    write_strings_json(out,profile.sources);
    out << ",\"notes\":";
    write_strings_json(out,profile.notes);
    if (dualsense) {
        out << ",\"basic_input_semantics_complete\":"
            << (!profile.reports.empty() && profile.reports.front().basic_input_semantics_complete ? "true" : "false")
            << ",\"extended_input_semantics_complete\":"
            << (!profile.reports.empty() && profile.reports.front().extended_input_semantics_complete ? "true" : "false")
            << ",\"extended_input_semantics_scope\":\"common_dualsense_usb_fields\"";
    }
    if (gip) {
        out << ",\"framing\":{\"wire_offsets_apply_to\":\"unchunked_single_length_byte_packet_matching_all_selector_conditions\","
            << "\"payload_offsets_apply_to\":\"decoded_reassembled_payload\","
            << "\"chunked_packets\":\"reassemble_before_parse\",\"extended_headers\":\"decode_before_parse\","
            << "\"raw_usb_offsets_unconditional\":false}";
    }
    out << ",\"reports\":[";
    for (size_t i = 0; i < profile.reports.size(); ++i) {
        if (i) out << ',';
        const auto& report = profile.reports[i];
        int command = -1;
        if (gip) {
            for (const auto& condition : report.conditions)
                if (condition.byte_offset == 0 && condition.mask == 255) command = static_cast<int>(condition.value);
        }
        out << '{';
        write_layout_evidence_json(out, evidence);
        out << ",\"selector\":" << json_string(report.selector)
            << ",\"selector_name\":" << json_string(report.selector)
            << ",\"command\":" << (command < 0 ? "null" : std::to_string(command))
            << ",\"header_bytes\":" << report.header_bytes
            << ",\"payload_offset_bytes\":" << report.header_bytes
            << ",\"requires_reassembly_before_parse\":" << (report.gip_length ? "true" : "false")
            << ",\"report_id\":" << (report.report_id < 0 ? "null" : std::to_string(report.report_id))
            << ",\"report_type\":\"Input\",\"bit_order\":\"lsb_first\",\"byte_order\":\"little_endian\""
            << ",\"minimum_wire_bytes\":" << report.minimum_wire_bytes
            << ",\"exact_wire_bytes\":" << (report.exact_wire_bytes ? std::to_string(report.exact_wire_bytes) : "null")
            << ",\"payload_start_byte\":" << report.header_bytes << ",\"selector_conditions\":[";
        for (size_t j = 0; j < report.conditions.size(); ++j) {
            if (j) out << ',';
            const auto& condition = report.conditions[j];
            out << "{\"byte_offset\":" << condition.byte_offset << ",\"mask\":" << condition.mask
                << ",\"equals\":" << condition.value << '}';
        }
        out << "]";
        if (dualsense)
            out << ",\"basic_input_semantics_complete\":" << (report.basic_input_semantics_complete ? "true" : "false")
                << ",\"extended_input_semantics_complete\":" << (report.extended_input_semantics_complete ? "true" : "false");
        if (report.gip_length) {
            out << ",\"payload_length\":{\"byte_offset\":3,\"bit_size\":7,\"minimum\":"
                << report.minimum_wire_bytes - report.header_bytes
                << ",\"require_complete_packet\":true,\"packet_size_addend\":" << report.header_bytes << '}';
        }
        out << ",\"fields\":[";
        std::vector<bool> mapped(report.minimum_wire_bytes * 8, false);
        for (size_t j = 0; j < report.fields.size(); ++j) {
            if (j) out << ',';
            const auto& f = report.fields[j];
            write_protocol_field_json(out,f,evidence);
            for (uint32_t bit = f.field.wire_bit_offset; bit < f.field.wire_bit_offset + f.field.bit_size && bit < mapped.size(); ++bit) mapped[bit] = true;
        }
        out << "],\"uninterpreted_regions\":[";
        bool first = true;
        for (uint32_t bit = report.header_bytes * 8; bit < mapped.size();) {
            if (mapped[bit]) { ++bit; continue; }
            uint32_t start = bit;
            while (bit < mapped.size() && !mapped[bit]) ++bit;
            if (!first) out << ',';
            first = false;
            out << "{\"wire_bit_offset\":" << start << ",\"bit_size\":" << bit - start
                << ",\"semantic_type\":\"unknown_protocol_extension\",\"preserve\":true}";
        }
        out << "],\"preserve_uninterpreted_bytes\":true,\"uninterpreted_tail_start_byte\":"
            << (report.exact_wire_bytes ? "null" : std::to_string(report.minimum_wire_bytes))
            << ",\"unknown_extensions\":{\"semantic_type\":\"unknown_protocol_extension\",\"preserve\":true,\"tail_start_byte\":"
            << (report.exact_wire_bytes ? "null" : std::to_string(report.minimum_wire_bytes)) << "}}";
    }
    out << ']';
    if (!profile.output_reports.empty()) {
        out << ",\"output_semantics_complete\":false,\"output_reports\":[";
        for (size_t i = 0; i < profile.output_reports.size(); ++i) {
            if (i) out << ',';
            const auto& report = profile.output_reports[i];
            const auto* raw = descriptor_report_by_id(descriptor,report.report_id);
            out << '{';
            write_layout_evidence_json(out,evidence);
            out << ",\"report_id\":" << static_cast<unsigned>(report.report_id)
                << ",\"report_type\":\"Output\",\"descriptor_output_wire_bytes\":"
                << (raw && raw->output_wire_bytes ? std::to_string(raw->output_wire_bytes) : "null")
                << ",\"known_protocol_output_wire_bytes\":" << report.known_protocol_wire_bytes
                << ",\"output_semantics_complete\":false,\"eligible_for_firmware_parser_generation\":false"
                << ",\"preserve_uninterpreted_bytes\":true,\"fields\":[]}";
        }
        out << ']';
    }
    out << '}';
}

static void write_endpoints_json(std::ostream& out, const InterfaceDesc& iface) {
    out << '[';
    for (size_t i = 0; i < iface.endpoints.size(); ++i) {
        if (i) out << ',';
        const auto& ep = iface.endpoints[i];
        out << "{\"address\":" << static_cast<unsigned>(ep.address) << ",\"direction\":" << json_string(ep.direction)
            << ",\"transfer_type\":" << json_string(ep.transfer_type) << ",\"max_packet_size\":" << ep.max_packet_size
            << ",\"interval\":" << static_cast<unsigned>(ep.interval) << '}';
    }
    out << ']';
}

static void write_physical_json(std::ostream& out, const std::vector<PhysicalUsbMatch>& matches) {
    out << '[';
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i) out << ',';
        const auto& p = matches[i];
        out << "{\"index\":" << i << ",\"vid\":" << json_string(hex_number(p.vid)) << ",\"pid\":" << json_string(hex_number(p.pid))
            << ",\"instance_id\":" << json_string(wstring_to_utf8(p.instance_id))
            << ",\"parent_hub_path\":" << json_string(wstring_to_utf8(p.hub_path))
            << ",\"parent_hub_instance_id\":" << json_string(wstring_to_utf8(p.hub_instance_id))
            << ",\"port\":" << p.port << ",\"driver_key\":" << json_string(p.driver_key)
            << ",\"device_descriptor_hex\":" << json_string(p.device_descriptor_hex)
            << ",\"current_configuration_value\":" << static_cast<unsigned>(p.current_configuration_value)
            << ",\"configurations\":[";
        for (size_t j = 0; j < p.configurations.size(); ++j) {
            if (j) out << ',';
            const auto& c = p.configurations[j];
            out << "{\"configuration_value\":" << static_cast<unsigned>(c.config_value)
                << ",\"raw_descriptor_hex\":" << json_string(c.raw_descriptor) << ",\"interfaces\":[";
            for (size_t k = 0; k < c.interfaces.size(); ++k) {
                if (k) out << ',';
                const auto& f = c.interfaces[k];
                out << "{\"interface_number\":" << f.interface_number << ",\"alternate_setting\":" << f.alternate_setting
                    << ",\"interface_class\":" << f.interface_class << ",\"interface_subclass\":" << f.interface_subclass
                    << ",\"interface_protocol\":" << f.interface_protocol << ",\"raw_descriptor_hex\":" << json_string(f.raw_descriptor)
                    << ",\"endpoints\":";
                write_endpoints_json(out,f);
                out << ",\"hid_descriptors\":[";
                for (size_t d = 0; d < f.hid_descriptors.size(); ++d) {
                    if (d) out << ',';
                    out << "{\"descriptor_type\":" << static_cast<unsigned>(f.hid_descriptors[d].first)
                        << ",\"length\":" << f.hid_descriptors[d].second << '}';
                }
                out << "]}";
            }
            out << "]}";
        }
        out << "],\"report_descriptor_errors\":[";
        bool first = true;
        for (const auto& e : p.report_descriptor_errors) {
            if (!first) out << ',';
            first = false;
            out << "{\"interface_number\":" << e.first << ",\"code\":" << e.second.first << ",\"message\":" << json_string(e.second.second) << '}';
        }
        out << "]}";
    }
    out << ']';
}

struct InterfaceExtraction {
    int physical_device_index = -1;
    uint16_t physical_vid = 0;
    uint16_t physical_pid = 0;
    int configuration_value = 0;
    InterfaceDesc descriptor;
    std::string scope;
    std::vector<size_t> windows_collections;
    std::vector<uint8_t> report_bytes;
    bool exact = false;
    bool wire_sizes_exact = false;
    bool physical_hid = false;
    std::string source;
    std::string parent_error;
    ParsedDescriptorResult parsed;
    ProtocolProfile protocol;
    std::vector<std::string> notes;
    bool using_ppd=false;
    PpdResult ppd;
    std::string collection_identity;
    std::set<std::pair<int,int>> conflicting_reports;
};

#include "controller_profile_match.h"

static bool fallback_sizes_exact(const WindowsHidInfo& wh) {
    std::map<std::string,std::set<uint8_t>> ids;
    for (const auto& f : wh.observed_fields) ids[f.report_type].insert(f.report_id);
    for (const auto& group : ids) if (group.second.size() > 1) return false;
    return !ids.empty();
}

static void populate_hid_layout(InterfaceExtraction& row, const std::vector<WindowsHidInfo>& windows) {
    bool decoded_ppd=false;
    if (row.report_bytes.empty() && row.windows_collections.size() == 1) {
        const auto& wh = windows[row.windows_collections.front()];
        if(wh.ppd.supported&&wh.ppd.parsed.available) {
            row.using_ppd=true;row.ppd=wh.ppd;row.parsed=wh.ppd.parsed;row.source="windows_preparsed_data";
            row.exact=false;row.wire_sizes_exact=true;decoded_ppd=true;
            for(const auto& report:row.ppd.report_states)if(!report.length_exact)row.wire_sizes_exact=false;
            row.notes.push_back("Bounded Windows PPD reconstruction; original HID descriptor bytes were not recovered. Per-report validation and framing govern eligibility; preserve the live base report.");
        } else row.report_bytes = reconstruct_report_descriptor_from_hid(wh);
        if (!row.report_bytes.empty()) {
            row.source = "windows_hid_stack";
            row.wire_sizes_exact = fallback_sizes_exact(wh);
            row.notes.push_back("Canonical descriptor reconstructed from Windows in-memory HID operations; original descriptor bytes and nested collection topology are not recovered.");
            if (!row.wire_sizes_exact) row.notes.push_back("Windows supplies maximum report buffer lengths; per-ID wire lengths are upper bounds and must be verified before firmware generation.");
        } else if (wh.available && !decoded_ppd) row.notes.push_back("Windows capabilities retained; a complete byte layout could not be established without guessing.");
    }
    if(!decoded_ppd)row.parsed = parse_report_descriptor_bytes(row.report_bytes);
    if (!row.parsed.available && row.windows_collections.size() == 1) {
        const auto& wh = windows[row.windows_collections.front()];
        if (wh.available) {
            ParsedCollection c;
            c.ordinal = c.top_level_ordinal = 1;
            c.collection_type = 1;
            c.usage_page = wh.caps.UsagePage;
            c.usage = wh.caps.Usage;
            row.parsed.collections.push_back(c);
        }
    }
    if (row.physical_hid && row.descriptor.alternate_setting == 0)
        row.protocol = protocol_for_interface(row.physical_vid, row.physical_pid, row.descriptor, &row.parsed);
}

static bool physical_layout_candidate(const InterfaceExtraction& row) {
    return row.physical_device_index >= 0 && row.descriptor.alternate_setting == 0 &&
        (row.scope == "physical_usb_interface" || row.scope == "windows_top_level_collection");
}

static bool physical_protocol_layout(const InterfaceExtraction& row) {
    return physical_layout_candidate(row) && row.configuration_value > 0 && controller_profile_match(row).matched;
}

static bool ppd_physical_report_ready(const InterfaceExtraction& row,const PpdReportState& state) {
    if(!row.using_ppd||!row.physical_hid||!physical_layout_candidate(row)||
        state.shared_layout_conflict||row.conflicting_reports.count({state.type,state.report_id})||!ppd_report_operation_ready(state))return false;
    if(state.type==1)for(const auto& output:row.protocol.output_reports)
        if(output.report_id==state.report_id&&output.known_protocol_wire_bytes!=state.known_wire_bytes)return false;
    bool data=false;
    for(const auto& f:row.parsed.fields) {
        if(f.report_type!=ppd_type_name(state.type)||f.report_id!=state.report_id||f.is_constant||f.uninterpreted_storage)continue;
        data=true;
        if(!f.interpretation_complete)return false;
        if(!f.is_variable&&f.ppd_button_cap) {
            if(!f.selector_mapping_resolved||f.usage_ranges.empty())return false;
            for(const auto& range:f.usage_ranges) {
                ParsedField domain;
                domain.bit_size=f.bit_size;domain.is_signed=f.is_signed;
                domain.logical_min=range.selector_min;
                domain.logical_max=range.selector_min+range.maximum-range.minimum;
                if(!logical_range_fits_field(domain))return false;
            }
        } else if(!logical_range_fits_field(f))return false;
    }
    return data;
}

static bool hid_report_ready(const InterfaceExtraction& row, const std::vector<WindowsHidInfo>& windows, int type, int id) {
    if (type < 0 || type > 2 || !physical_layout_candidate(row) || !row.physical_hid || row.descriptor.interface_class != 3) return false;
    if (row.using_ppd) {
        for (const auto& state : row.ppd.report_states)
            if (state.type == type && state.report_id == id) return ppd_physical_report_ready(row,state);
        return false;
    }
    if (!row.parsed.available || !row.parsed.errors.empty() || !row.parsed.warnings.empty() || !row.wire_sizes_exact) return false;
    const auto* report = descriptor_report_by_id(&row.parsed,id);
    if (!report) return false;
    const uint32_t length = type == 0 ? report->input_wire_bytes : type == 1 ? report->output_wire_bytes : report->feature_wire_bytes;
    if (!length || row.conflicting_reports.count({type,id})) return false;
    if (type == 1) for (const auto& output : row.protocol.output_reports)
        if (output.report_id == id && output.known_protocol_wire_bytes != length) return false;
    bool data = false;
    for (const auto& field : report->fields) {
        if (field.report_type != ppd_type_name(type) || field.is_constant || field.uninterpreted_storage) continue;
        data = true;
        if (!field.interpretation_complete || !logical_range_fits_field(field) ||
            uint64_t(field.wire_bit_offset) + uint64_t(field.bit_size) * field.report_count > uint64_t(length) * 8) return false;
    }
    if (!data) return false;
    for (size_t index : row.windows_collections) {
        if (index >= windows.size()) return false;
        const auto verification = verify_descriptor(row.parsed,windows[index],type,id);
        if (verification.status == "mismatch" || verification.status == "parse_error") return false;
    }
    return true;
}

static bool hid_firmware_layout_ready(const InterfaceExtraction& row, const std::vector<WindowsHidInfo>& windows) {
    if (!physical_layout_candidate(row)) return false;
    for (const auto& report : row.parsed.reports)
        if (hid_report_ready(row,windows,0,report.report_id)) return true;
    return false;
}

static bool firmware_layout_ready(const InterfaceExtraction& row, const std::vector<WindowsHidInfo>& windows) {
    if (physical_protocol_layout(row)) {
        for (const auto& report : row.protocol.reports)
            for (const auto& f : report.fields)
                if (!logical_range_fits_field(f.field)) return false;
        return true;
    }
    return hid_firmware_layout_ready(row, windows);
}

static LayoutEvidence hid_layout_evidence(const InterfaceExtraction& row, const std::vector<WindowsHidInfo>& windows) {
    if(row.using_ppd) {
        const bool physical=row.physical_hid&&physical_layout_candidate(row);
        return {"windows_preparsed_data",physical?"physical_wire_reconstruction":"logical_os_view","windows_preparsed_data",
            physical,physical,physical?3:4};
    }
    if (row.scope == "windows_logical_collection") return windows_logical_evidence();
    if (row.physical_hid && physical_layout_candidate(row)) {
        if (row.source == "usb_hub_ioctl_physical_device")
            return {"physical_hid_report_descriptor", "physical_wire", "physical_hid_report_descriptor",
                row.parsed.available, hid_firmware_layout_ready(row, windows), 2};
        return {"windows_hid_stack", "physical_wire_reconstruction", "windows_hid_caps",
            row.parsed.available, hid_firmware_layout_ready(row, windows), 3};
    }
    return {"unavailable", "unavailable", "unknown", false, false, 0};
}

static LayoutEvidence protocol_layout_evidence(const InterfaceExtraction& row, const std::vector<WindowsHidInfo>& windows) {
    auto evidence = protocol_definition_evidence();
    if (physical_protocol_layout(row)) {
        if (!row.physical_hid) {
            evidence.source = "physical_usb_protocol";
            evidence.authority = "physical_wire";
        }
        evidence.physical_wire = true;
        evidence.authoritative_ranges = firmware_layout_ready(row, windows);
        evidence.selection_priority = 1;
    }
    return evidence;
}

static LayoutEvidence interface_layout_evidence(const InterfaceExtraction& row, const std::vector<WindowsHidInfo>& windows) {
    if(row.using_ppd&&!physical_protocol_layout(row))return hid_layout_evidence(row,windows);
    if (row.scope == "windows_logical_collection") return windows_logical_evidence();
    if (physical_protocol_layout(row)) return protocol_layout_evidence(row, windows);
    if (row.physical_hid) return hid_layout_evidence(row, windows);
    return {"physical_usb_descriptors", "physical_metadata", "unknown", false, false, 0};
}

static std::vector<size_t> selected_firmware_layouts(const std::vector<InterfaceExtraction>& rows,
                                                   const std::vector<WindowsHidInfo>& windows);

struct FirmwareReportChoice {
    size_t row=0;
    size_t report=0;
    int type=0;
    int id=-1;
    int collection=0;
    bool protocol=false;
    int priority=0;
    std::string identity;
    std::string selector;
};

static std::vector<FirmwareReportChoice> selected_firmware_reports(const std::vector<InterfaceExtraction>& rows,
                                                                  const std::vector<WindowsHidInfo>& windows) {
    std::vector<FirmwareReportChoice> choices;
    for(size_t i=0;i<rows.size();++i) {
        const auto& row=rows[i];
        if(!physical_layout_candidate(row))continue;
        if(physical_protocol_layout(row)&&firmware_layout_ready(row,windows)) {
            for(size_t j=0;j<row.protocol.reports.size();++j) {
                const auto& report=row.protocol.reports[j];
                choices.push_back({i,j,0,report.report_id,0,true,1,row.collection_identity,report.selector});
            }
        }
        if(!row.physical_hid||!row.parsed.available)continue;
        for(size_t j=0;j<row.parsed.reports.size();++j) {
            const auto& report=row.parsed.reports[j];
            for(int type=0;type<3;++type) {
                bool ready=hid_report_ready(row,windows,type,report.report_id);
                if(type==1)for(const auto& output:row.protocol.output_reports)
                    if(output.report_id==report.report_id&&output.known_protocol_wire_bytes!=report.output_wire_bytes)ready=false;
                if(!ready)continue;
                std::set<int> collections;
                for(const auto& f:report.fields)
                    if(f.report_type==ppd_type_name(type)&&!f.is_constant&&!f.uninterpreted_storage)collections.insert(f.top_level_collection_ordinal);
                for(int collection:collections) {
                    std::string identity=row.collection_identity;
                    if(identity.empty())identity="descriptor_collection_"+std::to_string(collection);
                    choices.push_back({i,j,type,report.report_id,collection,false,hid_layout_evidence(row,windows).selection_priority,identity,{}});
                }
            }
        }
    }
    auto covers=[&](const FirmwareReportChoice& high,const FirmwareReportChoice& low) {
        if(high.type!=low.type||high.id!=low.id)return false;
        const auto& a=rows[high.row];const auto& b=rows[low.row];
        if(a.physical_device_index!=b.physical_device_index||a.configuration_value!=b.configuration_value||
            a.descriptor.interface_number!=b.descriptor.interface_number||a.descriptor.alternate_setting!=b.descriptor.alternate_setting)return false;
        if(high.protocol)return high.type==0;
        if(low.protocol)return false;
        for(const auto& field:b.parsed.reports[low.report].fields) {
            if(field.report_type!=ppd_type_name(low.type)||field.top_level_collection_ordinal!=low.collection||field.is_constant||field.uninterpreted_storage)continue;
            bool found=false;
            for(const auto& other:a.parsed.reports[high.report].fields) {
                if(other.report_type==field.report_type&&other.top_level_collection_ordinal==high.collection&&other.wire_bit_offset==field.wire_bit_offset&&
                    other.bit_size==field.bit_size&&other.report_count==field.report_count&&other.usage_page==field.usage_page&&other.usage==field.usage)found=true;
            }
            if(!found)return false;
        }
        return true;
    };
    std::vector<FirmwareReportChoice> result;
    for(size_t i=0;i<choices.size();++i) {
        bool superseded=false;
        for(size_t j=0;j<choices.size();++j)
            if(i!=j&&choices[j].priority<choices[i].priority&&covers(choices[j],choices[i])){superseded=true;break;}
        if(!superseded)result.push_back(choices[i]);
    }
    return result;
}

static std::vector<size_t> selected_firmware_layouts(const std::vector<InterfaceExtraction>& rows,
                                                    const std::vector<WindowsHidInfo>& windows) {
    std::vector<size_t> selected;
    for(size_t i=0;i<rows.size();++i) {
        if(!firmware_layout_ready(rows[i],windows))continue;
        bool superseded=false;
        const auto& row=rows[i];
        const int priority=interface_layout_evidence(row,windows).selection_priority;
        for(size_t j=0;j<rows.size();++j) {
            const auto& other=rows[j];
            if(i==j||row.physical_device_index!=other.physical_device_index||row.configuration_value!=other.configuration_value||
                row.descriptor.interface_number!=other.descriptor.interface_number||row.descriptor.alternate_setting!=other.descriptor.alternate_setting)continue;
            if(!firmware_layout_ready(other,windows)||interface_layout_evidence(other,windows).selection_priority>=priority)continue;
            if(!physical_protocol_layout(other)) {
                bool covered=true;
                for(const auto& report:row.parsed.reports) {
                    if(!report.input_wire_bytes)continue;
                    bool found=false;
                    for(const auto& candidate:other.parsed.reports)if(candidate.report_id==report.report_id&&candidate.input_wire_bytes)found=true;
                    if(!found)covered=false;
                }
                if(!covered)continue;
            }
            superseded=true;break;
        }
        if(!superseded)selected.push_back(i);
    }
    return selected;
}

static void write_selected_firmware_reports_json(std::ostream& out,const std::vector<InterfaceExtraction>& rows,
                                                 const std::vector<WindowsHidInfo>& windows) {
    const auto selected=selected_firmware_reports(rows,windows);
    out<<'[';
    for(size_t i=0;i<selected.size();++i) {
        if(i)out<<',';const auto& c=selected[i];const auto& row=rows[c.row];
        out<<"{\"interface_index\":"<<c.row<<",\"physical_device_index\":"<<row.physical_device_index
            <<",\"configuration_value\":"<<row.configuration_value<<",\"interface_number\":"<<row.descriptor.interface_number
            <<",\"alternate_setting\":"<<row.descriptor.alternate_setting<<",\"collection_identity\":"<<json_string(c.identity)
            <<",\"top_level_collection_ordinal\":"<<(c.collection?std::to_string(c.collection):"null")
            <<",\"report_type\":"<<json_string(ppd_type_name(c.type))<<",\"report_id\":"<<(c.id<0?"null":std::to_string(c.id))
            <<",\"selector\":"<<(c.selector.empty()?"null":json_string(c.selector))
            <<",\"layout_path\":"<<json_string("/interfaces/"+std::to_string(c.row)+(c.protocol?"/protocol_layout/reports/":"/reports/")+std::to_string(c.report))
            <<",\"layout_selection_priority\":"<<c.priority<<",\"firmware_layout_ready\":true,\"eligible_for_firmware_parser_generation\":true"
            <<",\"requires_live_base_report\":"<<(!c.protocol&&row.using_ppd?"true":"false")
            <<",\"selection_scope\":\"physical_device_configuration_interface_alternate_collection_type_id_selector\"}";
    }
    out<<']';
}

static void write_firmware_selection_json(std::ostream& out, const std::vector<InterfaceExtraction>& rows,
                                          const std::vector<WindowsHidInfo>& windows) {
    out << "{\"priority_order\":[\"known_physical_protocol\",\"exact_physical_hid_descriptor\","
        << "\"reconstructed_physical_hid_descriptor\",\"windows_logical_collection\"],"
        << "\"requires_firmware_layout_ready\":true,\"windows_logical_views_eligible\":false,"
        << "\"selection_scope\":\"physical_device_configuration_interface_alternate_setting\",\"selected_layouts\":[";
    const auto selected = selected_firmware_layouts(rows, windows);
    for (size_t i = 0; i < selected.size(); ++i) {
        if (i) out << ',';
        const size_t index = selected[i];
        const auto& row = rows[index];
        const bool protocol = physical_protocol_layout(row);
        out << '{';
        write_layout_evidence_json(out, interface_layout_evidence(row, windows));
        out << ",\"interface_index\":" << index << ",\"physical_device_index\":" << row.physical_device_index
            << ",\"configuration_value\":" << row.configuration_value
            << ",\"interface_number\":" << row.descriptor.interface_number
            << ",\"alternate_setting\":" << row.descriptor.alternate_setting
            << ",\"firmware_layout_ready\":true,\"protocol\":" << (protocol ? json_string(row.protocol.name) : "null")
            << ",\"layout_path\":" << json_string("/interfaces/" + std::to_string(index) + (protocol ? "/protocol_layout" : "/reports")) << '}';
    }
    out << "],\"selected_layouts_scope\":\"legacy_input_interface_summary_use_selected_reports_for_generation\",\"selected_reports\":";
    write_selected_firmware_reports_json(out,rows,windows);
    out << '}';
}

static void write_interface_json(std::ostream& out, const InterfaceExtraction& row,
                                const std::vector<HidCollectionEntry>& collections, const std::vector<WindowsHidInfo>& windows) {
    const bool ready = firmware_layout_ready(row, windows);
    const auto profile_match = controller_profile_match(row);
    out << "    {";
    write_layout_evidence_json(out, interface_layout_evidence(row, windows));
    out << ",\"controller_profile_match\":{\"policy\":\"usb-controller-match-v2\",\"candidate\":"
        << (row.protocol.name.empty() ? "null" : json_string(row.protocol.name))
        << ",\"selection_basis\":" << (profile_match.selection_basis.empty() ? "null" : json_string(profile_match.selection_basis))
        << ",\"vid_pid_used_for_family_selection\":false"
        << ",\"status\":" << json_string(profile_match.status) << ",\"definition_matches\":" << (profile_match.matched ? "true" : "false")
        << ",\"physical_binding_verified\":" << (physical_protocol_layout(row) ? "true" : "false")
        << ",\"device_authentication_verified\":false,\"reasons\":";
    write_strings_json(out,profile_match.reasons);
    out << '}';
    out << ",\"transport\":" << (row.scope=="offline_ppd_import"?"null":json_string(row.scope == "windows_logical_collection" ? "windows_hid" : "usb"))
        << ",\"protocol\":" << (row.protocol.name.empty() ? "null" : json_string(row.protocol.name))
        << ",\"preferred_firmware_layout\":" << (ready ? json_string(physical_protocol_layout(row) ? "protocol_layout" : "reports") : "null")
        << ",\"interface_number\":" << row.descriptor.interface_number
        << ",\"alternate_setting\":" << row.descriptor.alternate_setting
        << ",\"configuration_value\":" << row.configuration_value
        << ",\"physical_device_index\":" << (row.physical_device_index < 0 ? "null" : std::to_string(row.physical_device_index))
        << ",\"layout_scope\":" << json_string(row.scope)
        << ",\"interface_class\":" << row.descriptor.interface_class
        << ",\"interface_subclass\":" << row.descriptor.interface_subclass
        << ",\"interface_protocol\":" << row.descriptor.interface_protocol
        << ",\"physical_hid_transport\":" << (row.physical_hid ? "true" : "false")
        << ",\"firmware_layout_ready\":" << (ready ? "true" : "false")
        << ",\"eligible_for_firmware_parser_generation\":" << (ready ? "true" : "false")
        << ",\"wire_sizes_exact\":" << (row.wire_sizes_exact ? "true" : "false")
        << ",\"descriptor_exact\":" << (row.exact ? "true" : "false")
        << ",\"physical_capture_verified\":false,\"collection_identity\":" << (row.collection_identity.empty()?"null":json_string(row.collection_identity));
    if(row.using_ppd) {
        out<<",\"readiness_scope\":\"per_report_modify_existing_base\",\"whole_packet_generation_ready\":false,\"report_readiness\":[";
        for(size_t i=0;i<row.ppd.report_states.size();++i) {
            if(i)out<<',';const auto& r=row.ppd.report_states[i];const bool eligible=ppd_physical_report_ready(row,r);
            out<<"{\"report_type\":"<<json_string(ppd_type_name(r.type))<<",\"report_id\":"<<(r.report_id<0?"null":std::to_string(r.report_id))
                <<",\"reconstruction_complete\":"<<(r.complete?"true":"false")<<",\"wire_sizes_exact\":"<<(r.length_exact?"true":"false")
                <<",\"windows_api_report_byte_length\":"<<r.windows_bytes<<",\"wire_bytes_lower_bound\":"<<r.known_wire_bytes
                <<",\"exact_wire_bytes\":"<<(r.length_exact?std::to_string(r.known_wire_bytes):"null")
                <<",\"firmware_layout_ready\":"<<(eligible?"true":"false")<<",\"eligible_for_firmware_parser_generation\":"<<(eligible?"true":"false")
                <<",\"shared_layout_conflict\":"<<(row.conflicting_reports.count({r.type,r.report_id})?"true":"false")
                <<",\"requires_live_base_report\":true,\"verification_source\":"<<json_string(r.oracle.executed?"windows_in_memory_oracle":"offline_record_checks")<<'}';
        }
        out<<']';
    }
    out << ",\"endpoints\":";
    write_endpoints_json(out,row.descriptor);
    out << ",\"raw_report_descriptor\":{\"available\":" << (!row.report_bytes.empty() ? "true" : "false")
        << ",\"exact\":" << (row.exact ? "true" : "false") << ",\"source\":" << json_string(row.source)
        << ",\"length\":" << row.report_bytes.size() << ",\"hex\":" << json_string(to_hex(row.report_bytes.data(),row.report_bytes.size()))
        << ",\"parent_hub_error\":" << json_string(row.parent_error) << "},\n      ";
    write_parsed_json(out,row.parsed,row.protocol.reports.empty() ? "" : "gamepad",hid_layout_evidence(row,windows));
    out << ",\"protocol_layout\":";
    write_protocol_json(out,row.protocol,protocol_layout_evidence(row,windows),ready && physical_protocol_layout(row),&row.parsed);
    out << ",\"notes\":";
    write_strings_json(out,row.notes);
    out << ",\"windows_hid_collections\":[";
    for (size_t i = 0; i < row.windows_collections.size(); ++i) {
        if (i) out << ',';
        size_t index = row.windows_collections[i];
        write_windows_hid_json(out,collections[index],windows[index],row.parsed,row.exact);
    }
    out << "]}";
}

static bool ppd_equivalent_usage_ranges(const std::vector<UsageRange>& a,const std::vector<UsageRange>& b) {
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i)if(a[i].usage_page!=b[i].usage_page||a[i].minimum!=b[i].minimum||a[i].maximum!=b[i].maximum||
        a[i].explicit_selector!=b[i].explicit_selector||a[i].selector_min!=b[i].selector_min)return false;
    return true;
}

static bool ppd_equivalent_fields(const ParsedField& a,const ParsedField& b) {
    return a.wire_bit_offset==b.wire_bit_offset&&a.bit_size==b.bit_size&&a.report_count==b.report_count&&
        a.usage_page==b.usage_page&&a.usage==b.usage&&a.main_item_flags==b.main_item_flags&&a.logical_min==b.logical_min&&
        a.logical_max==b.logical_max&&a.is_signed==b.is_signed&&a.has_null_state==b.has_null_state&&
        a.physical_min==b.physical_min&&a.physical_max==b.physical_max&&a.unit==b.unit&&a.unit_exponent==b.unit_exponent&&
        a.interpretation_complete==b.interpretation_complete&&a.selector_mapping_resolved==b.selector_mapping_resolved&&
        ppd_equivalent_usage_ranges(a.usage_ranges,b.usage_ranges)&&ppd_equivalent_usage_ranges(a.usage_aliases,b.usage_aliases);
}

static void resolve_ppd_shared_layouts(std::vector<InterfaceExtraction>& rows) {
    using ReportKey=std::pair<int,int>;
    std::vector<std::map<ReportKey,std::vector<const ParsedField*>>> reports(rows.size());
    for(size_t i=0;i<rows.size();++i) {
        if(!rows[i].using_ppd||!physical_layout_candidate(rows[i]))continue;
        for(const auto& field:rows[i].parsed.fields)if(!field.is_constant&&!field.uninterpreted_storage)
            reports[i][{ppd_type_index(field.report_type),field.report_id}].push_back(&field);
        for(auto& report:reports[i])std::sort(report.second.begin(),report.second.end(),[](const ParsedField* a,const ParsedField* b){return a->wire_bit_offset<b->wire_bit_offset;});
    }
    for(size_t i=0;i<rows.size();++i)for(size_t j=i+1;j<rows.size();++j) {
        auto& a=rows[i];auto& b=rows[j];
        if(!a.using_ppd||!b.using_ppd||!physical_layout_candidate(a)||!physical_layout_candidate(b)||
            a.physical_device_index!=b.physical_device_index||a.configuration_value!=b.configuration_value||
            a.descriptor.interface_number!=b.descriptor.interface_number||a.descriptor.alternate_setting!=b.descriptor.alternate_setting)continue;
        for(const auto& x:a.ppd.report_states)for(const auto& y:b.ppd.report_states) {
            if(x.report_id<0||x.type!=y.type||x.report_id!=y.report_id)continue;
            if((x.length_exact&&y.known_wire_bytes>x.known_wire_bytes)||(y.length_exact&&x.known_wire_bytes>y.known_wire_bytes)) {
                a.conflicting_reports.insert({x.type,x.report_id});b.conflicting_reports.insert({x.type,x.report_id});
            }
        }
        for(const auto& group:reports[i]) {
            const auto found=reports[j].find(group.first);
            if(found==reports[j].end())continue;
            size_t ai=0,bi=0;
            while(ai<group.second.size()&&bi<found->second.size()) {
                const auto& x=*group.second[ai];const auto& y=*found->second[bi];
                const uint64_t xe=uint64_t(x.wire_bit_offset)+uint64_t(x.bit_size)*x.report_count;
                const uint64_t ye=uint64_t(y.wire_bit_offset)+uint64_t(y.bit_size)*y.report_count;
                if(x.wire_bit_offset<ye&&y.wire_bit_offset<xe&&!ppd_equivalent_fields(x,y)) {
                    a.conflicting_reports.insert(group.first);b.conflicting_reports.insert(group.first);
                }
                if(xe<=ye)++ai;
                if(ye<=xe)++bi;
            }
        }
    }
    for(auto& row:rows) {
        if(!row.using_ppd)continue;
        std::map<ReportKey,bool> readiness;
        for(auto& state:row.ppd.report_states)state.shared_layout_conflict=row.conflicting_reports.count({state.type,state.report_id})!=0;
        for(const auto& state:row.ppd.report_states)readiness[{state.type,state.report_id}]=ppd_physical_report_ready(row,state);
        auto update=[&](ParsedField& field) {
            const auto found=readiness.find({ppd_type_index(field.report_type),field.report_id});
            field.ppd_physical_eligible=found!=readiness.end()&&found->second;
        };
        for(auto& field:row.parsed.fields)update(field);
        for(auto& report:row.parsed.reports)for(auto& field:report.fields)update(field);
        for(const auto& key:row.conflicting_reports)row.notes.push_back(std::string("Conflicting shared ")+ppd_type_name(key.first)+" report "+std::to_string(key.second)+" is excluded from physical generation.");
    }
}

static std::vector<InterfaceExtraction> collect_interface_layouts(const std::vector<PhysicalUsbMatch>& physical,
        const std::vector<UsbDeviceEntry>& usb, const std::vector<HidCollectionEntry>& hid,
        const std::vector<WindowsHidInfo>& windows) {
    std::vector<InterfaceExtraction> rows;
    std::vector<bool> assigned(hid.size(),false);
    std::vector<int> physical_indices(hid.size(),-1);
    for (size_t i = 0; i < hid.size(); ++i) physical_indices[i] = physical_index_for(hid[i],usb,physical);
    for (size_t pi = 0; pi < physical.size(); ++pi) {
        const auto& p = physical[pi];
        for (const auto& config : p.configurations) {
            if (config.config_value != p.current_configuration_value) continue;
            for (const auto& iface : config.interfaces) {
                InterfaceExtraction row;
                row.physical_device_index = static_cast<int>(pi);
                row.physical_vid = p.vid;
                row.physical_pid = p.pid;
                row.configuration_value = config.config_value;
                row.descriptor = iface;
                row.scope = "physical_usb_interface";
                row.physical_hid = iface.interface_class == 3;
                row.protocol = protocol_for_interface(p.vid,p.pid,iface);
                bool interrupt_input = std::any_of(iface.endpoints.begin(),iface.endpoints.end(),[](const EndpointDesc& ep) {
                    return ep.direction == "IN" && ep.transfer_type == "Interrupt";
                });
                if (!row.protocol.name.empty() && !interrupt_input) {
                    row.protocol.reports.clear();
                    row.protocol.notes.push_back("This interface has no interrupt input endpoint; the input-state layout is not assigned to auxiliary audio/bulk interfaces.");
                }
                if (iface.alternate_setting == 0 && row.physical_hid) {
                    for (size_t hi = 0; hi < hid.size(); ++hi) {
                        if (physical_indices[hi] != static_cast<int>(pi) || assigned[hi]) continue;
                        if (hid[hi].vid != p.vid || hid[hi].pid != p.pid) continue;
                        const auto identity=hid_transport_identity(hid[hi]);
                        if(identity.logical||identity.conflicting)continue;
                        int number=identity.interface_number;
                        if(number<0&&config.interfaces.size()==1&&iface.interface_class==3)number=iface.interface_number;
                        if (number == iface.interface_number) row.windows_collections.push_back(hi);
                    }
                    auto raw = p.raw_report_descriptors.find(iface.interface_number);
                    if (raw != p.raw_report_descriptors.end()) {
                        row.report_bytes = raw->second;
                        row.exact = true;
                        row.wire_sizes_exact = true;
                        row.source = "usb_hub_ioctl_physical_device";
                        for (const auto& declared : iface.hid_descriptors) {
                            if (declared.first == 0x22 && declared.second != row.report_bytes.size()) {
                                row.exact = false;
                                row.wire_sizes_exact = false;
                                row.notes.push_back("Physical report descriptor length differs from the declared length; layout is incomplete.");
                            }
                        }
                    }
                    auto error = p.report_descriptor_errors.find(iface.interface_number);
                    if (error != p.report_descriptor_errors.end()) row.parent_error = error->second.second;
                } else if (iface.alternate_setting != 0) {
                    row.scope = "alternate_interface_metadata";
                    row.physical_hid = false;
                    row.protocol.reports.clear();
                    row.notes.push_back("Alternate setting metadata is retained; this tool does not change or query the active alternate setting.");
                }
                if (row.report_bytes.empty() && row.windows_collections.size() > 1) {
                    for (size_t hi : row.windows_collections) {
                        InterfaceExtraction collection = row;
                        collection.windows_collections = {hi};
                        collection.collection_identity=instance_key(hid[hi].base.instance_id);
                        collection.scope = "windows_top_level_collection";
                        populate_hid_layout(collection,windows);
                        rows.push_back(std::move(collection));
                        assigned[hi] = true;
                    }
                } else {
                    for (size_t hi : row.windows_collections) assigned[hi] = true;
                    if(row.windows_collections.size()==1)row.collection_identity=instance_key(hid[row.windows_collections.front()].base.instance_id);
                    if (row.report_bytes.empty() && !row.windows_collections.empty()) row.scope = "windows_top_level_collection";
                    populate_hid_layout(row,windows);
                    rows.push_back(std::move(row));
                }
            }
        }
    }
    for (size_t hi = 0; hi < hid.size(); ++hi) {
        if (assigned[hi]) continue;
        InterfaceExtraction row;
        row.physical_device_index = physical_indices[hi];
        const auto identity=hid_transport_identity(hid[hi]);
        row.descriptor.interface_number = identity.interface_number;
        row.descriptor.interface_class = 0;
        row.collection_identity=instance_key(hid[hi].base.instance_id);
        if(row.physical_device_index>=0) {
            const auto& parent=physical[row.physical_device_index];
            for(const auto& config:parent.configurations)if(config.config_value==parent.current_configuration_value)
                for(const auto& iface:config.interfaces)if(iface.interface_number==identity.interface_number&&iface.alternate_setting==0) {
                    row.configuration_value=config.config_value;row.descriptor=iface;
                }
        }
        row.windows_collections = {hi};
        row.scope = "windows_logical_collection";
        row.notes.push_back("Windows collection layout is not proven to be the physical USB wire transport; physical descriptor/protocol layouts must be used for firmware.");
        populate_hid_layout(row,windows);
        rows.push_back(std::move(row));
    }
    resolve_ppd_shared_layouts(rows);
    return rows;
}

static void write_interface_txt(std::ostream& out, const InterfaceExtraction& row,
        const std::vector<HidCollectionEntry>& hid, const std::vector<WindowsHidInfo>& windows) {
    const auto evidence = interface_layout_evidence(row, windows);
    out << "========================================================================\n"
        << "USB INTERFACE " << row.descriptor.interface_number << " / ALTERNATE " << row.descriptor.alternate_setting
        << " / PHYSICAL DEVICE " << row.physical_device_index << "\n"
        << "Scope: " << row.scope << "\n"
        << "Layout source: " << evidence.source << "; authority: " << evidence.authority << '\n'
        << "Physical wire layout: " << (evidence.physical_wire ? "yes" : "no")
        << "; selection priority: " << (evidence.selection_priority ? std::to_string(evidence.selection_priority) : "none") << '\n'
        << "Firmware layout ready: " << (firmware_layout_ready(row,windows) ? "yes" : "no") << "\n"
        << "========================================================================\n";
    const auto profile_match = controller_profile_match(row);
    if (!row.protocol.name.empty()) {
        out << "Controller profile candidate: " << row.protocol.name << "; match: " << profile_match.status
            << "; selection basis: " << profile_match.selection_basis << "; VID/PID family restriction: none"
            << "; physical binding verified: " << (physical_protocol_layout(row) ? "yes" : "no")
            << "; device authentication verified: no\n";
        for (const auto& reason : profile_match.reasons) out << "  Profile match: " << reason << '\n';
    }
    if(row.using_ppd) {
        out<<"Readiness scope: per-report modification of a matching live base report. Whole-packet generation: no.\n";
        for(const auto& state:row.ppd.report_states)out<<ppd_type_name(state.type)<<" ID "<<(state.report_id<0?"unknown":std::to_string(state.report_id))
            <<": firmware ready "<<(ppd_physical_report_ready(row,state)?"yes":"no")<<"; shared-layout conflict "<<(state.shared_layout_conflict?"yes":"no")<<'\n';
    }
    write_semantic_txt(out,row.parsed,hid_layout_evidence(row,windows));
    if (!row.protocol.name.empty()) {
        out << "KNOWN CONTROLLER PROTOCOL: " << row.protocol.name << "\n";
        for (const auto& report : row.protocol.reports) {
            out << "  Selector: " << report.selector << "; minimum wire bytes: " << report.minimum_wire_bytes
                << "; payload starts at byte: " << report.header_bytes << '\n';
            if (row.protocol.name == "dualsense_usb")
                out << "    Basic input semantics complete: " << (report.basic_input_semantics_complete ? "yes" : "no")
                    << "; extended input semantics complete: " << (report.extended_input_semantics_complete ? "yes" : "no") << '\n';
            if (report.gip_length)
                out << "    Requires framing/reassembly before parse: yes. Fixed wire offsets require an unchunked, single-length-byte packet passing all selectors; payload offsets apply to the decoded/reassembled payload.\n";
            for (const auto& c : report.conditions) out << "    Match byte " << c.byte_offset << " & " << hex_number(c.mask,2) << " == " << hex_number(c.value,2) << '\n';
            for (const auto& f : report.fields) write_protocol_field_txt(out,f,protocol_layout_evidence(row,windows));
        }
        for (const auto& report : row.protocol.output_reports) {
            const auto* raw = descriptor_report_by_id(&row.parsed,report.report_id);
            out << "  Output Report ID " << hex_number(report.report_id,2)
                << "; descriptor output wire bytes: " << (raw && raw->output_wire_bytes ? std::to_string(raw->output_wire_bytes) : "unavailable")
                << "; known protocol USB output wire bytes: " << report.known_protocol_wire_bytes
                << "; output semantics complete: no; preserve raw Output layout.\n";
        }
        for (const auto& note : row.protocol.notes) out << "  Note: " << note << '\n';
    }
    out << "\nRAW HID REPORT DESCRIPTOR\n"
        << "Source: " << row.source << "\nExact: " << (row.exact ? "True" : "False")
        << "\nLength: " << row.report_bytes.size() << "\n";
    if (!row.parent_error.empty()) out << "Parent-hub status: " << row.parent_error << '\n';
    if (row.report_bytes.empty()) out << "Unavailable\n";
    else out << hex_bytes_spaced(to_hex(row.report_bytes.data(),row.report_bytes.size())) << '\n';
    for (size_t index : row.windows_collections) {
        const auto& col = hid[index];
        const auto& wh = windows[index];
        if(!wh.cached_collection_descriptor_bytes.empty())write_ppd_metadata_txt(out,wh);
        out << "\nWINDOWS HID COLLECTION\nProduct: " << col.product << "\nHID Path: " << wstring_to_utf8(col.base.path)
            << "\nInstance ID: " << wstring_to_utf8(col.base.instance_id)
            << "\nParent instance ID: " << wstring_to_utf8(col.base.parent_instance_id)
            << "\nInput Report Bytes: " << wh.caps.InputReportByteLength
            << "\nOutput Report Bytes: " << wh.caps.OutputReportByteLength
            << "\nFeature Report Bytes: " << wh.caps.FeatureReportByteLength << '\n';
        auto verification = verify_descriptor(row.parsed,wh);
        out << "WINDOWS / NATIVE COMPARISON\nStatus: " << str_toupper(verification.status)
            << "\nObserved fields checked: " << verification.observed_fields_checked
            << "\nConstant storage elements checked: " << verification.constant_elements_checked
            << "\nAll offsets independently checked: " << (verification.all_offsets_verified && row.exact ? "yes" : "no") << '\n';
        for (const auto& difference : verification.differences) out << "  Difference: " << difference << '\n';
        for (const auto& error : wh.layout_errors) out << "  Windows layout note: " << error << '\n';
    }
    for (const auto& note : row.notes) out << "Note: " << note << '\n';
    out << '\n';
}

static bool parse_hex_id(const char* text, uint16_t& value) {
    if (!text || !*text || *text == '-' || *text == '+') return false;
    const char* start = text;
    if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) start += 2;
    size_t length = strlen(start);
    if (!length || length > 4) return false;
    for (size_t i = 0; i < length; ++i) if (!std::isxdigit(static_cast<unsigned char>(start[i]))) return false;
    value = static_cast<uint16_t>(strtoul(start,nullptr,16));
    return true;
}

static int extract_ppd_file(const std::filesystem::path& filename,const std::filesystem::path& output_dir) {
    std::ifstream input(filename,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Unable to open PPD file");
    const auto end=input.tellg();
    if(end<0||end>static_cast<std::streamoff>(ppd_max_bytes))throw std::runtime_error("PPD file exceeds 1 MiB bound");
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    input.seekg(0);
    if(!bytes.empty()&&!input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("PPD file changed or could not be read completely");
    if(input.peek()!=std::char_traits<char>::eof()||input.bad())throw std::runtime_error("PPD file length changed during acquisition");
    WindowsHidInfo wh;
    wh.cached_collection_descriptor_bytes=bytes;wh.ppd_source="saved_artifact";wh.ppd_length_source="exact_binary_file_length";
    wh.ppd=decode_windows_ppd(bytes);
    InterfaceExtraction row;
    row.scope="offline_ppd_import";row.source="windows_preparsed_data";row.descriptor.interface_number=-1;
    row.windows_collections={0};row.using_ppd=true;row.ppd=wh.ppd;row.parsed=wh.ppd.parsed;
    row.notes.push_back("Imported PPD has no verified physical identity. No Windows oracle or physical-traffic validation was run. Original descriptor bytes are unavailable.");
    std::filesystem::create_directories(output_dir);
    const auto stem="makcu_ppd_"+filename.stem().string();
    const auto json_path=output_dir/(stem+".json"),txt_path=output_dir/(stem+".txt");
    std::ofstream json(json_path),txt(txt_path);
    if(!json||!txt)throw std::runtime_error("Unable to create offline export");
    json<<"{\"schema\":\"makcu-input-extraction\",\"version\":4,\"schema_extension\":\"windows-ppd-v1\",\"offline\":true,\"device\":null,"
        <<"\"import_metadata\":{\"file\":"<<json_string(filename.string())<<",\"identity_origin_verified\":false,\"physical_capture_verified\":false},"
        <<"\"physical_devices\":[],\"interfaces\":[";
    write_interface_json(json,row,{HidCollectionEntry{}},{wh});
    json<<"],\"firmware_layout_selection\":";
    write_firmware_selection_json(json,{row},{wh});json<<"}\n";
    write_interface_txt(txt,row,{HidCollectionEntry{}},{wh});
    json.flush();txt.flush();
    if(!json||!txt)throw std::runtime_error("Failed to write complete offline export");
    json.close();txt.close();
    if(json.fail()||txt.fail())throw std::runtime_error("Failed to close offline export");
    std::cout<<"Offline PPD: "<<bytes.size()<<" bytes; format "<<(wh.ppd.supported?ppd_format_id:"unsupported")
        <<"; normalized fields "<<wh.ppd.parsed.fields.size()<<"; physical firmware eligibility: false\n"
        <<json_path.string()<<'\n'<<txt_path.string()<<'\n';
    return wh.ppd.supported?0:1;
}

#ifndef UNIVERSAL_USB_PARSER_TEST
int main(int argc, char** argv) {
    try {
        uint16_t vid = 0, pid = 0;
        bool has_vid = false, has_pid = false, list_only = false;
        std::string output_dir = ".";
        std::string ppd_filename;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--vid" && i + 1 < argc) {
                if (!parse_hex_id(argv[++i],vid)) throw std::invalid_argument("Invalid hexadecimal VID");
                has_vid = true;
            } else if (arg == "--pid" && i + 1 < argc) {
                if (!parse_hex_id(argv[++i],pid)) throw std::invalid_argument("Invalid hexadecimal PID");
                has_pid = true;
            } else if (arg == "--list") list_only = true;
            else if (arg == "--ppd-file" && i+1 < argc) ppd_filename=argv[++i];
            else if (arg == "--output-dir" && i + 1 < argc) output_dir = argv[++i];
            else if(arg=="--licenses") {std::cout<<third_party_notices;return 0;}
            else if (arg == "--help" || arg == "-h") {
                std::cout << "universal_usb_parser.exe [--list] [--vid HEX --pid HEX] [--output-dir PATH]\n"
                    << "universal_usb_parser.exe --ppd-file FILE.ppd [--output-dir PATH]\n"
                    << "universal_usb_parser.exe --licenses\n";
                return 0;
            } else throw std::invalid_argument("Unknown or incomplete argument: " + arg);
        }
        if (has_vid != has_pid) throw std::invalid_argument("Specify both --vid and --pid");
        if(!ppd_filename.empty()) {
            if(has_vid||has_pid||list_only)throw std::invalid_argument("--ppd-file cannot be combined with live selection");
            return extract_ppd_file(ppd_filename,output_dir);
        }
        auto hid_entries = enumerate_hid_collections();
        auto usb_entries = enumerate_usb_devices();
        std::map<std::pair<uint16_t,uint16_t>,std::string> groups;
        for (const auto& h : hid_entries) groups[{h.vid,h.pid}] = h.product;
        for (const auto& u : usb_entries) if (!groups.count({u.vid,u.pid})) groups[{u.vid,u.pid}] = u.product;
        std::cout << "Universal USB Parser - semantic schema v4\n\n";
        if (groups.empty()) { std::cerr << "No connected USB or HID devices were found.\n"; return 1; }
        if (list_only || !has_vid) {
            std::vector<std::pair<uint16_t,uint16_t>> choices;
            for (const auto& group : groups) {
                choices.push_back(group.first);
                printf("[%zu] %04X:%04X - %s\n",choices.size(),group.first.first,group.first.second,group.second.c_str());
            }
            if (list_only) return 0;
            size_t choice = 0;
            std::cout << "\nSelect device number: ";
            if (!(std::cin >> choice) || !choice || choice > choices.size()) throw std::invalid_argument("Invalid selection");
            vid = choices[choice - 1].first;
            pid = choices[choice - 1].second;
        }
        if (!groups.count({vid,pid})) { std::cerr << "Selected VID/PID is not present.\n"; return 1; }
        std::set<std::string> selected_instances;
        std::set<std::pair<uint16_t,uint16_t>> physical_ids;
        for (const auto& u : usb_entries) {
            if (u.vid == vid && u.pid == pid && instance_key(u.base.instance_id).find("&mi_") == std::string::npos) {
                selected_instances.insert(instance_key(u.base.instance_id));
                physical_ids.insert({u.vid,u.pid});
            }
        }
        for (const auto& h : hid_entries) {
            if (h.vid != vid || h.pid != pid) continue;
            const auto* parent = physical_parent(h,usb_entries);
            if (parent) {
                selected_instances.insert(instance_key(parent->base.instance_id));
                physical_ids.insert({parent->vid,parent->pid});
            }
        }
        if (physical_ids.empty()) physical_ids.insert({vid,pid});
        std::vector<HidCollectionEntry> selected_hid;
        for (const auto& h : hid_entries) {
            const auto* parent = physical_parent(h,usb_entries);
            if ((h.vid == vid && h.pid == pid) || (parent && selected_instances.count(instance_key(parent->base.instance_id)))) selected_hid.push_back(h);
        }
        printf("Scanning %04X:%04X; %zu related Windows HID collection(s).\n",vid,pid,selected_hid.size());
        std::vector<PhysicalUsbMatch> physical;
        for (const auto& pair : physical_ids) {
            auto found = scan_physical_usb(pair.first,pair.second);
            physical.insert(physical.end(),found.begin(),found.end());
        }
        associate_physical_instances(physical,usb_entries);
        physical.erase(std::remove_if(physical.begin(),physical.end(),[&](const PhysicalUsbMatch& p) {
            return !selected_instances.empty() && !p.instance_id.empty() && !selected_instances.count(instance_key(p.instance_id));
        }),physical.end());
        std::vector<WindowsHidInfo> windows;
        for (const auto& h : selected_hid) windows.push_back(query_windows_hid(h.base.path));
        auto rows = collect_interface_layouts(physical,usb_entries,selected_hid,windows);
        std::filesystem::create_directories(output_dir);
        char stem[64];
        snprintf(stem,sizeof(stem),"makcu_hid_%04X_%04X",vid,pid);
        for(size_t i=0;i<windows.size();++i) {
            const auto& blob=windows[i].cached_collection_descriptor_bytes;
            if(blob.empty())continue;
            const auto path=std::filesystem::path(output_dir)/(std::string(stem)+"_collection_"+std::to_string(i)+".ppd");
            std::ofstream binary(path,std::ios::binary);
            if(!binary||!binary.write(reinterpret_cast<const char*>(blob.data()),static_cast<std::streamsize>(blob.size())))
                throw std::runtime_error("Unable to write acquired PPD sidecar");
            binary.close();
            if(binary.fail())throw std::runtime_error("Unable to close acquired PPD sidecar");
            windows[i].ppd_sidecar_file=path.filename().string();
        }
        std::filesystem::path json_path = std::filesystem::path(output_dir) / (std::string(stem) + ".json");
        std::filesystem::path txt_path = std::filesystem::path(output_dir) / (std::string(stem) + ".txt");
        std::ofstream json(json_path), txt(txt_path);
        if (!json || !txt) throw std::runtime_error("Unable to open extraction output files");
        json << "{\n  \"schema\":\"makcu-input-extraction\",\"version\":4,\"schema_extension\":\"windows-ppd-v1\",\n"
             << "  \"device\":{\"vid\":" << json_string(hex_number(vid)) << ",\"pid\":" << json_string(hex_number(pid))
             << ",\"hid_collection_count\":" << selected_hid.size() << "},\n"
             << "  \"offset_convention\":{\"bit_order\":\"lsb_first\",\"payload\":\"excludes HID report ID or protocol header\","
             << "\"wire\":\"includes HID report ID or protocol header\",\"bit_offset\":\"alias of wire_bit_offset\"},\n"
             << "  \"physical_devices\":";
        write_physical_json(json,physical);
        json << ",\n  \"interfaces\":[\n";
        txt << "UNIVERSAL USB INPUT EXTRACTION - SEMANTIC SCHEMA V4\nVID: " << hex_number(vid) << "\nPID: " << hex_number(pid)
            << "\nWindows HID Collections: " << selected_hid.size() << "\nPhysical USB Matches: " << physical.size() << "\n\n";
        for (size_t i = 0; i < physical.size(); ++i) {
            const auto& p = physical[i];
            txt << "Physical device " << i << ": " << hex_number(p.vid) << ':' << hex_number(p.pid)
                << "\nInstance: " << wstring_to_utf8(p.instance_id) << "\nDevice descriptor: " << hex_bytes_spaced(p.device_descriptor_hex) << '\n';
            for (const auto& c : p.configurations) txt << "Configuration " << static_cast<unsigned>(c.config_value) << ": " << hex_bytes_spaced(c.raw_descriptor) << '\n';
            txt << '\n';
        }
        size_t exact_count = 0, fallback_count = 0, ppd_count = 0, ready_count = 0, field_count = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) json << ",\n";
            write_interface_json(json,rows[i],selected_hid,windows);
            write_interface_txt(txt,rows[i],selected_hid,windows);
            if (rows[i].exact) ++exact_count;
            else if(rows[i].using_ppd&&rows[i].parsed.available)++ppd_count;
            else if (!rows[i].report_bytes.empty()) ++fallback_count;
            if (firmware_layout_ready(rows[i],windows)) ++ready_count;
            field_count += rows[i].parsed.fields.size();
        }
        json << "\n  ],\n  \"firmware_layout_selection\":";
        write_firmware_selection_json(json,rows,windows);
        json << "\n}\n";
        json.flush(); txt.flush();
        if (!json || !txt) throw std::runtime_error("Failed to write complete extraction output");
        json.close(); txt.close();
        if (json.fail() || txt.fail()) throw std::runtime_error("Failed to close extraction output");
        std::cout << "Physical devices: " << physical.size() << "; interface/collection layouts: " << rows.size()
            << "\nExact descriptors: " << exact_count << "; reconstructed descriptors: " << fallback_count
            << "; directly recovered PPD layouts: " << ppd_count
            << "\nSemantic HID fields: " << field_count << "; firmware-ready layouts: " << ready_count << '\n'
            << json_path.string() << '\n' << txt_path.string() << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 2;
    }
}
#endif
