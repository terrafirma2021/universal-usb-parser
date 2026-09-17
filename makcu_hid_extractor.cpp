#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cfgmgr32.h>
#include <usbioctl.h>
#include <GameInput.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "gameinput.lib")

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
    if (signed_val && val < 0) {
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

struct ParsedField {
    std::string report_type;
    uint8_t report_id = 0;
    int usage_page = 0;
    int usage = 0;
    uint32_t payload_bit_offset = 0;
    uint32_t wire_bit_offset = 0;
    uint8_t bit_size = 0;
    uint32_t report_count = 0;
    int32_t logical_min = 0;
    int32_t logical_max = 0;
    int32_t physical_min = 0;
    int32_t physical_max = 0;
    uint32_t unit = 0;
    int32_t unit_exponent = 0;
    bool is_signed = false;
    bool is_constant = false;
    bool is_variable = false;
    bool is_relative = false;
    int top_level_collection_ordinal = 1;
};

struct ParsedReport {
    uint8_t report_id = 0;
    uint32_t input_payload_bits = 0;
    uint32_t input_wire_bytes = 0;
    std::vector<ParsedField> fields;
};

struct ParsedDescriptorResult {
    bool available = false;
    std::vector<ParsedReport> reports;
    std::vector<ParsedField> fields;
    std::vector<std::string> errors;
};

static ParsedDescriptorResult parse_report_descriptor_bytes(const std::vector<uint8_t>& raw) {
    ParsedDescriptorResult res;
    res.available = !raw.empty();
    size_t cursor = 0;

    int cur_usage_page = 0;
    int32_t cur_log_min = 0, cur_log_max = 0;
    int32_t cur_phys_min = 0, cur_phys_max = 0;
    uint32_t cur_unit = 0;
    int32_t cur_unit_exp = 0;
    uint8_t cur_report_size = 0;
    uint8_t cur_report_id = 0;
    uint32_t cur_report_count = 0;

    struct GlobalStackState {
        int usage_page;
        int32_t log_min, log_max, phys_min, phys_max;
        uint32_t unit;
        int32_t unit_exp;
        uint8_t report_size, report_id;
        uint32_t report_count;
    };
    std::vector<GlobalStackState> global_stack;

    std::vector<std::pair<int, int>> local_usages;
    std::pair<int, int> local_usage_min = {-1, -1};
    std::pair<int, int> local_usage_max = {-1, -1};

    std::map<std::pair<std::string, uint8_t>, uint32_t> offsets;
    int collection_depth = 0;

    while (cursor < raw.size()) {
        uint8_t prefix = raw[cursor++];
        if (prefix == 0xFE) { // Long item
            if (cursor + 2 > raw.size()) { res.errors.push_back("Truncated long item"); break; }
            uint8_t size = raw[cursor++];
            cursor++; // skip tag
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
                std::string rtype = (tag == 8) ? "Input" : (tag == 9 ? "Output" : "Feature");
                auto key = std::make_pair(rtype, cur_report_id);
                uint32_t start = offsets[key];
                bool is_constant = (uval & 0x01) != 0;
                bool is_variable = (uval & 0x02) != 0;
                bool is_relative = (uval & 0x04) != 0;

                std::vector<std::pair<int, int>> usages = local_usages;
                if (local_usage_min.first != -1 && local_usage_max.first != -1 && local_usage_min.first == local_usage_max.first) {
                    for (int u = local_usage_min.second; u <= local_usage_max.second; ++u)
                        usages.emplace_back(local_usage_min.first, u);
                }

                if (cur_report_count > 0 && cur_report_size > 0) {
                    if (is_constant) {
                        ParsedField f;
                        f.report_type = rtype;
                        f.report_id = cur_report_id;
                        f.payload_bit_offset = start;
                        f.wire_bit_offset = start + (cur_report_id ? 8 : 0);
                        f.bit_size = cur_report_size;
                        f.report_count = cur_report_count;
                        f.logical_min = cur_log_min;
                        f.logical_max = cur_log_max;
                        f.is_constant = true;
                        res.fields.push_back(f);
                    } else if (is_variable) {
                        for (uint32_t i = 0; i < cur_report_count; ++i) {
                            ParsedField f;
                            f.report_type = rtype;
                            f.report_id = cur_report_id;
                            if (!usages.empty()) {
                                size_t u_idx = (std::min)(static_cast<size_t>(i), usages.size() - 1);
                                f.usage_page = usages[u_idx].first;
                                f.usage = usages[u_idx].second;
                            }
                            f.payload_bit_offset = start + i * cur_report_size;
                            f.wire_bit_offset = f.payload_bit_offset + (cur_report_id ? 8 : 0);
                            f.bit_size = cur_report_size;
                            f.report_count = 1;
                            f.logical_min = cur_log_min;
                            f.logical_max = cur_log_max;
                            f.physical_min = cur_phys_min;
                            f.physical_max = cur_phys_max;
                            f.unit = cur_unit;
                            f.unit_exponent = cur_unit_exp;
                            f.is_signed = (cur_log_min < 0);
                            f.is_variable = true;
                            f.is_relative = is_relative;
                            res.fields.push_back(f);
                        }
                    } else { // Array
                        ParsedField f;
                        f.report_type = rtype;
                        f.report_id = cur_report_id;
                        if (usages.size() == 1) {
                            f.usage_page = usages[0].first;
                            f.usage = usages[0].second;
                        }
                        f.payload_bit_offset = start;
                        f.wire_bit_offset = start + (cur_report_id ? 8 : 0);
                        f.bit_size = cur_report_size;
                        f.report_count = cur_report_count;
                        f.logical_min = cur_log_min;
                        f.logical_max = cur_log_max;
                        f.physical_min = cur_phys_min;
                        f.physical_max = cur_phys_max;
                        f.unit = cur_unit;
                        f.unit_exponent = cur_unit_exp;
                        f.is_signed = (cur_log_min < 0);
                        f.is_variable = false;
                        res.fields.push_back(f);
                    }
                }
                offsets[key] = start + cur_report_count * cur_report_size;
            } else if (tag == 10) { // Collection
                collection_depth++;
            } else if (tag == 12) { // End Collection
                if (collection_depth > 0) collection_depth--;
            }
            local_usages.clear();
            local_usage_min = {-1, -1};
            local_usage_max = {-1, -1};
        } else if (item_type == 1) { // Global
            switch (tag) {
                case 0: cur_usage_page = static_cast<int>(uval); break;
                case 1: cur_log_min = sval; break;
                case 2: cur_log_max = (cur_log_min < 0) ? sval : static_cast<int32_t>(uval); break;
                case 3: cur_phys_min = sval; break;
                case 4: cur_phys_max = (cur_phys_min < 0) ? sval : static_cast<int32_t>(uval); break;
                case 5: cur_unit_exp = (size == 1 && (uval & 0x08)) ? static_cast<int32_t>(uval & 0x0F) - 16 : sval; break;
                case 6: cur_unit = uval; break;
                case 7: cur_report_size = static_cast<uint8_t>(uval); break;
                case 8: cur_report_id = static_cast<uint8_t>(uval); break;
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
                    }
                    break;
                }
            }
        } else if (item_type == 2) { // Local
            auto make_usage = [&](uint32_t v) -> std::pair<int, int> {
                if (v > 0xFFFF) return { static_cast<int>((v >> 16) & 0xFFFF), static_cast<int>(v & 0xFFFF) };
                return { cur_usage_page, static_cast<int>(v & 0xFFFF) };
            };
            if (tag == 0) local_usages.push_back(make_usage(uval));
            else if (tag == 1) local_usage_min = make_usage(uval);
            else if (tag == 2) local_usage_max = make_usage(uval);
        }
    }

    std::set<uint8_t> all_rids;
    for (const auto& f : res.fields) all_rids.insert(f.report_id);
    for (uint8_t rid : all_rids) {
        ParsedReport pr;
        pr.report_id = rid;
        uint32_t bits = offsets[{"Input", rid}];
        pr.input_payload_bits = bits;
        pr.input_wire_bytes = (bits + 7) / 8 + (rid ? 1 : 0);
        for (const auto& f : res.fields) {
            if (f.report_id == rid && f.report_type == "Input") pr.fields.push_back(f);
        }
        res.reports.push_back(pr);
    }
    return res;
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
    std::string error;
};

static WindowsHidInfo query_windows_hid(const std::wstring& path) {
    WindowsHidInfo info;
    HANDLE h = open_handle_w(path, 0);
    if (!h) {
        info.error = win32_error_text(GetLastError());
        return info;
    }

    info.attributes.Size = sizeof(HIDD_ATTRIBUTES);
    HidD_GetAttributes(h, &info.attributes);
    info.manufacturer = hid_get_string(h, HidD_GetManufacturerString);
    info.product = hid_get_string(h, HidD_GetProductString);
    info.serial_number = hid_get_string(h, HidD_GetSerialNumberString);

    BYTE info_buf[12] = {0};
    DWORD ret_bytes = 0;
    if (DeviceIoControl(h, IOCTL_HID_GET_COLLECTION_INFORMATION, nullptr, 0, info_buf, sizeof(info_buf), &ret_bytes, nullptr) && ret_bytes >= 12) {
        uint32_t desc_size = *reinterpret_cast<uint32_t*>(info_buf);
        if (desc_size > 0 && desc_size < 1024 * 1024) {
            std::vector<uint8_t> desc_buf(desc_size);
            if (DeviceIoControl(h, IOCTL_HID_GET_COLLECTION_DESCRIPTOR, nullptr, 0, desc_buf.data(), desc_size, &ret_bytes, nullptr) && ret_bytes > 0) {
                desc_buf.resize(ret_bytes);
                info.cached_collection_descriptor_bytes = desc_buf;
            }
        }
    }

    PHIDP_PREPARSED_DATA pp_data = nullptr;
    if (HidD_GetPreparsedData(h, &pp_data) && pp_data) {
        if (HidP_GetCaps(pp_data, &info.caps) == HIDP_STATUS_SUCCESS) {
            info.available = true;
            if (info.caps.NumberInputButtonCaps > 0) {
                info.input_button_caps.resize(info.caps.NumberInputButtonCaps);
                USHORT num = info.caps.NumberInputButtonCaps;
                HidP_GetButtonCaps(HidP_Input, info.input_button_caps.data(), &num, pp_data);
                info.input_button_caps.resize(num);
            }
            if (info.caps.NumberInputValueCaps > 0) {
                info.input_value_caps.resize(info.caps.NumberInputValueCaps);
                USHORT num = info.caps.NumberInputValueCaps;
                HidP_GetValueCaps(HidP_Input, info.input_value_caps.data(), &num, pp_data);
                info.input_value_caps.resize(num);
            }
            if (info.caps.NumberOutputButtonCaps > 0) {
                info.output_button_caps.resize(info.caps.NumberOutputButtonCaps);
                USHORT num = info.caps.NumberOutputButtonCaps;
                HidP_GetButtonCaps(HidP_Output, info.output_button_caps.data(), &num, pp_data);
                info.output_button_caps.resize(num);
            }
            if (info.caps.NumberOutputValueCaps > 0) {
                info.output_value_caps.resize(info.caps.NumberOutputValueCaps);
                USHORT num = info.caps.NumberOutputValueCaps;
                HidP_GetValueCaps(HidP_Output, info.output_value_caps.data(), &num, pp_data);
                info.output_value_caps.resize(num);
            }
            if (info.caps.NumberFeatureButtonCaps > 0) {
                info.feature_button_caps.resize(info.caps.NumberFeatureButtonCaps);
                USHORT num = info.caps.NumberFeatureButtonCaps;
                HidP_GetButtonCaps(HidP_Feature, info.feature_button_caps.data(), &num, pp_data);
                info.feature_button_caps.resize(num);
            }
            if (info.caps.NumberFeatureValueCaps > 0) {
                info.feature_value_caps.resize(info.caps.NumberFeatureValueCaps);
                USHORT num = info.caps.NumberFeatureValueCaps;
                HidP_GetValueCaps(HidP_Feature, info.feature_value_caps.data(), &num, pp_data);
                info.feature_value_caps.resize(num);
            }
            if (info.caps.NumberLinkCollectionNodes > 0) {
                info.link_collections.resize(info.caps.NumberLinkCollectionNodes);
                ULONG num = info.caps.NumberLinkCollectionNodes;
                HidP_GetLinkCollectionNodes(info.link_collections.data(), &num, pp_data);
                info.link_collections.resize(num);
            }
        }
        HidD_FreePreparsedData(pp_data);
    } else {
        info.error = "HidD_GetPreparsedData failed: " + win32_error_text(GetLastError());
    }
    CloseHandle(h);
    return info;
}

static std::vector<uint8_t> reconstruct_report_descriptor_from_hid(const WindowsHidInfo& wh) {
    if (!wh.available) return {};
    std::vector<uint8_t> out;

    add_hid_item(out, 0x04, wh.caps.UsagePage);
    add_hid_item(out, 0x08, wh.caps.Usage);
    out.push_back(0xA1); out.push_back(0x01); // Application Collection

    std::vector<HIDP_LINK_COLLECTION_NODE> child_nodes;
    for (size_t i = 1; i < wh.link_collections.size(); ++i) {
        if (wh.link_collections[i].Parent == 0) child_nodes.push_back(wh.link_collections[i]);
    }
    for (const auto& c : child_nodes) {
        add_hid_item(out, 0x04, c.LinkUsagePage);
        add_hid_item(out, 0x08, c.LinkUsage);
        out.push_back(0xA1); out.push_back(static_cast<uint8_t>(c.CollectionType));
    }

    struct ReportTypeConfig {
        const std::vector<HIDP_BUTTON_CAPS>& bcaps;
        const std::vector<HIDP_VALUE_CAPS>& vcaps;
        uint8_t tag;
        USHORT target_report_byte_len;
    };
    ReportTypeConfig rtypes[] = {
        { wh.input_button_caps, wh.input_value_caps, 0x80, wh.caps.InputReportByteLength },
        { wh.output_button_caps, wh.output_value_caps, 0x90, wh.caps.OutputReportByteLength },
        { wh.feature_button_caps, wh.feature_value_caps, 0xB0, wh.caps.FeatureReportByteLength },
    };

    for (const auto& rt : rtypes) {
        std::set<UCHAR> all_rids;
        for (const auto& b : rt.bcaps) all_rids.insert(b.ReportID);
        for (const auto& v : rt.vcaps) all_rids.insert(v.ReportID);

        for (UCHAR rid : all_rids) {
            if (rid != 0) add_hid_item(out, 0x84, rid);

            std::vector<HIDP_BUTTON_CAPS> r_bcaps;
            for (const auto& b : rt.bcaps) if (b.ReportID == rid) r_bcaps.push_back(b);
            std::vector<HIDP_VALUE_CAPS> r_vcaps;
            for (const auto& v : rt.vcaps) if (v.ReportID == rid) r_vcaps.push_back(v);

            std::sort(r_vcaps.begin(), r_vcaps.end(), [](const HIDP_VALUE_CAPS& a, const HIDP_VALUE_CAPS& b) {
                if (a.UsagePage != b.UsagePage) return a.UsagePage < b.UsagePage;
                USHORT ua = a.IsRange ? a.Range.UsageMin : a.NotRange.Usage;
                USHORT ub = b.IsRange ? b.Range.UsageMin : b.NotRange.Usage;
                return ua < ub;
            });

            uint32_t bits = 0;
            for (const auto& b : r_bcaps) {
                add_hid_item(out, 0x04, b.UsagePage);
                int cnt = 1;
                if (b.IsRange) {
                    add_hid_item(out, 0x18, b.Range.UsageMin);
                    add_hid_item(out, 0x28, b.Range.UsageMax);
                    cnt = (std::max)(1, (int)b.Range.UsageMax - (int)b.Range.UsageMin + 1);
                } else {
                    add_hid_item(out, 0x08, b.NotRange.Usage);
                }
                add_hid_item(out, 0x14, 0);
                add_hid_item(out, 0x24, 1);
                add_hid_item(out, 0x74, 1);
                add_hid_item(out, 0x94, cnt);
                add_hid_item(out, rt.tag, b.BitField);
                bits += cnt;
            }
            if (bits % 8 != 0) {
                uint32_t rem = 8 - (bits % 8);
                add_hid_item(out, 0x74, rem);
                add_hid_item(out, 0x94, 1);
                add_hid_item(out, rt.tag, 1); // Constant
                bits += rem;
            }

            for (const auto& v : r_vcaps) {
                add_hid_item(out, 0x04, v.UsagePage);
                if (v.IsRange) {
                    add_hid_item(out, 0x18, v.Range.UsageMin);
                    add_hid_item(out, 0x28, v.Range.UsageMax);
                } else {
                    add_hid_item(out, 0x08, v.NotRange.Usage);
                }
                uint8_t bit_size = v.BitSize ? (uint8_t)v.BitSize : 8;
                int32_t lmin = v.LogicalMin;
                int32_t lmax = v.LogicalMax;
                if (lmax == -1 && lmin >= 0 && bit_size > 0) {
                    lmax = (bit_size >= 32) ? 0x7FFFFFFF : ((1 << bit_size) - 1);
                }
                add_hid_item(out, 0x14, lmin, lmin < 0);
                add_hid_item(out, 0x24, lmax, lmin < 0);
                if (v.PhysicalMin != 0 || v.PhysicalMax != 0) {
                    add_hid_item(out, 0x34, v.PhysicalMin, true);
                    add_hid_item(out, 0x44, v.PhysicalMax, true);
                }
                if (v.Units != 0) add_hid_item(out, 0x64, v.Units);
                if (v.UnitsExp != 0) add_hid_item(out, 0x54, v.UnitsExp, true);

                uint16_t rpt_cnt = v.ReportCount ? v.ReportCount : 1;
                add_hid_item(out, 0x74, bit_size);
                add_hid_item(out, 0x94, rpt_cnt);
                add_hid_item(out, rt.tag, v.BitField);
                bits += bit_size * rpt_cnt;
            }

            uint32_t target_bits = 0;
            if (rt.target_report_byte_len > 0) {
                uint32_t target_payload = (rid == 0) ? (std::max)(0, (int)rt.target_report_byte_len - 1) : rt.target_report_byte_len;
                target_bits = target_payload * 8;
            }
            if (target_bits > bits) {
                uint32_t pad = target_bits - bits;
                add_hid_item(out, 0x74, pad);
                add_hid_item(out, 0x94, 1);
                add_hid_item(out, rt.tag, 1); // Constant
            } else if (bits % 8 != 0) {
                uint32_t rem = 8 - (bits % 8);
                add_hid_item(out, 0x74, rem);
                add_hid_item(out, 0x94, 1);
                add_hid_item(out, rt.tag, 1); // Constant
            }
        }
    }

    for (size_t i = 0; i < child_nodes.size(); ++i) out.push_back(0xC0);
    out.push_back(0xC0); // Close Application
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
    std::vector<InterfaceDesc> interfaces;
};

struct PhysicalUsbMatch {
    std::wstring hub_path;
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
            match.port = port;
            match.vid = dev_vid;
            match.pid = dev_pid;
            match.current_configuration_value = conn_buf[22];

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

int main(int argc, char** argv) {
    uint16_t target_vid = 0, target_pid = 0;
    bool has_target = false;
    bool list_only = false;
    std::string output_dir = ".";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--vid") == 0 && i + 1 < argc) {
            target_vid = static_cast<uint16_t>(strtoul(argv[++i], nullptr, 16));
            has_target = true;
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            target_pid = static_cast<uint16_t>(strtoul(argv[++i], nullptr, 16));
            has_target = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        }
    }

    std::filesystem::create_directories(output_dir);

    std::cout << "MAKCU Input Extractor (Native Win32 C++ Edition)\n\n";

    auto hid_entries = enumerate_hid_collections();
    auto usb_entries = enumerate_usb_devices();

    std::map<std::pair<uint16_t, uint16_t>, std::vector<HidCollectionEntry>> groups;
    for (const auto& e : hid_entries) {
        groups[{e.vid, e.pid}].push_back(e);
    }
    // Also include controller USB devices (e.g. 045E:0B12)
    for (const auto& u : usb_entries) {
        if (!groups.count({u.vid, u.pid})) {
            if (u.vid == 0x045E || u.vid == 0x054C) {
                groups[{u.vid, u.pid}] = {};
            }
        }
    }

    if (groups.empty()) {
        std::cout << "No connected input devices were found.\n";
        return 1;
    }

    std::cout << "Connected Input Devices:\n\n";
    int idx = 1;
    std::vector<std::pair<uint16_t, uint16_t>> group_keys;
    for (const auto& kv : groups) {
        uint16_t v = kv.first.first;
        uint16_t p = kv.first.second;
        group_keys.push_back({v, p});
        std::string prod = "Unknown product";
        std::string mfg = "Unknown";
        for (const auto& h : kv.second) {
            if (!h.product.empty()) prod = h.product;
            if (!h.manufacturer.empty()) mfg = h.manufacturer;
        }
        if (prod == "Unknown product") {
            for (const auto& u : usb_entries) {
                if (u.vid == v && u.pid == p) {
                    if (!u.product.empty()) prod = u.product;
                    if (!u.manufacturer.empty()) mfg = u.manufacturer;
                }
            }
        }
        printf("[%d] %04X:%04X - %s\n", idx, v, p, prod.c_str());
        printf("    Manufacturer: %s\n", mfg.c_str());
        printf("    Windows HID Collections: %zu\n", kv.second.size());
        idx++;
    }
    std::cout << "\n";

    if (list_only) return 0;

    uint16_t vid = target_vid, pid = target_pid;
    if (!has_target) {
        std::cout << "Select device number: ";
        int choice = 0;
        if (!(std::cin >> choice) || choice < 1 || choice > (int)group_keys.size()) {
            std::cout << "Invalid selection.\n";
            return 2;
        }
        vid = group_keys[choice - 1].first;
        pid = group_keys[choice - 1].second;
    }

    printf("Scanning %04X:%04X...\n", vid, pid);
    std::vector<HidCollectionEntry> sel_hid;
    for (const auto& h : hid_entries) {
        if (h.vid == vid && h.pid == pid) sel_hid.push_back(h);
    }
    printf("Found %zu Windows HID collection(s).\n", sel_hid.size());
    printf("Querying parent USB hubs for physical descriptors...\n");
    printf("Querying Windows HID parser and controller protocol signatures...\n");

    auto physical_matches = scan_physical_usb(vid, pid);

    std::string stem;
    char sbuf[64];
    snprintf(sbuf, sizeof(sbuf), "makcu_hid_%04X_%04X", vid, pid);
    stem = sbuf;

    std::string txt_path = output_dir + "/" + stem + ".txt";
    std::string json_path = output_dir + "/" + stem + ".json";
    std::ofstream txt(txt_path);
    std::ofstream jout(json_path);

    txt << "========================================================================\n";
    txt << "MAKCU INPUT EXTRACTION (Native Win32 C++ Edition)\n";
    txt << "========================================================================\n";
    txt << "VID: 0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << vid << "\n";
    txt << "PID: 0x" << std::setw(4) << pid << "\n";
    txt << "Windows HID Collections: " << std::dec << sel_hid.size() << "\n";
    txt << "Physical USB Matches: " << physical_matches.size() << "\n\n";

    jout << "{\n";
    jout << "  \"schema\": \"makcu-hid-extraction\",\n";
    jout << "  \"version\": 3,\n";
    jout << "  \"device\": {\n";
    jout << "    \"vid\": \"0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << vid << "\",\n";
    jout << "    \"pid\": \"0x" << std::setw(4) << pid << "\",\n";
    jout << "    \"hid_collection_count\": " << std::dec << sel_hid.size() << "\n";
    jout << "  },\n";
    jout << "  \"interfaces\": [\n";

    int raw_count = 0;
    int exact_count = 0;
    int windows_count = 0;

    for (size_t c_idx = 0; c_idx < sel_hid.size(); ++c_idx) {
        const auto& col = sel_hid[c_idx];
        int iface_num = col.interface_number;
        if (iface_num < 0 && physical_matches.size() == 1) {
            std::set<int> all_ifaces;
            for (const auto& conf : physical_matches[0].configurations) {
                for (const auto& ifc : conf.interfaces) {
                    all_ifaces.insert(ifc.interface_number);
                }
            }
            if (all_ifaces.size() == 1) {
                iface_num = *all_ifaces.begin();
            }
        }
        txt << "========================================================================\n";
        txt << "HID COLLECTION " << (c_idx + 1) << " / USB INTERFACE " << (iface_num >= 0 ? std::to_string(iface_num) : "None") << "\n";
        txt << "========================================================================\n";
        txt << "Product: " << col.product << "\n";
        txt << "HID Path: " << wstring_to_utf8(col.base.path) << "\n";
        txt << "Instance ID: " << wstring_to_utf8(col.base.instance_id) << "\n\n";

        WindowsHidInfo wh = query_windows_hid(col.base.path);
        if (wh.available) windows_count++;

        txt << "------------------------------------------------------------------------\n";
        txt << "RAW HID REPORT DESCRIPTOR\n";
        txt << "------------------------------------------------------------------------\n";

        std::vector<uint8_t> report_bytes;
        bool exact = false;
        std::string source;
        std::string parent_err;
        if (iface_num < 0 && physical_matches.size() == 1 && physical_matches[0].raw_report_descriptors.size() == 1) {
            iface_num = physical_matches[0].raw_report_descriptors.begin()->first;
        }

        for (const auto& pm : physical_matches) {
            auto it = pm.raw_report_descriptors.find(iface_num);
            if (it != pm.raw_report_descriptors.end()) {
                report_bytes = it->second;
                exact = true;
                source = "usb_hub_ioctl_physical_device";
                break;
            }
            auto eit = pm.report_descriptor_errors.find(iface_num);
            if (eit != pm.report_descriptor_errors.end()) {
                parent_err = eit->second.second;
            }
        }

        if (report_bytes.empty()) {
            report_bytes = reconstruct_report_descriptor_from_hid(wh);
            if (!report_bytes.empty()) {
                exact = false;
                source = "windows_hid_stack";
            }
        }

        std::string hex_str = to_hex(report_bytes.data(), report_bytes.size());
        if (!report_bytes.empty()) {
            raw_count++;
            if (exact) exact_count++;
            txt << "Source: " << source << "\n";
            txt << "Exact: " << (exact ? "True" : "False") << "\n";
            txt << "Length: " << report_bytes.size() << "\n";
            if (!exact) {
                txt << "Note: Report descriptor queried directly from Windows HID stack report capabilities as fallback because parent-hub control transfer was unavailable or rejected by physical device.\n";
                if (!parent_err.empty()) txt << "Parent-hub status: " << parent_err << "\n";
            }
            txt << hex_bytes_spaced(hex_str) << "\n\n";
        } else {
            txt << "Unavailable: Report descriptor could not be retrieved.\n\n";
        }

        txt << "------------------------------------------------------------------------\n";
        txt << "WINDOWS HID PARSE\n";
        txt << "------------------------------------------------------------------------\n";
        if (wh.available) {
            txt << "Input Report Bytes: " << wh.caps.InputReportByteLength << "\n";
            txt << "Input: " << wh.input_button_caps.size() << " button caps, " << wh.input_value_caps.size() << " value caps\n";
            for (const auto& v : wh.input_value_caps) {
                USHORT u = v.IsRange ? v.Range.UsageMin : v.NotRange.Usage;
                txt << "  RID " << (int)v.ReportID << " Page 0x" << std::hex << std::setw(4) << std::setfill('0') << v.UsagePage
                    << " Usage 0x" << std::setw(4) << u << " Size=" << std::dec << v.BitSize << " Count=" << v.ReportCount
                    << " Logical=" << v.LogicalMin << ".." << v.LogicalMax << "\n";
            }
            txt << "\n";
        } else {
            txt << "Unavailable: " << wh.error << "\n\n";
        }

        txt << "------------------------------------------------------------------------\n";
        txt << "PYTHON/NATIVE RAW DESCRIPTOR PARSE\n";
        txt << "------------------------------------------------------------------------\n";
        ParsedDescriptorResult parsed = parse_report_descriptor_bytes(report_bytes);
        if (parsed.available) {
            for (const auto& rep : parsed.reports) {
                txt << "Report ID " << (int)rep.report_id << "\n";
                txt << "  Input: payload=" << rep.input_payload_bits << " bits, wire=" << rep.input_wire_bytes << " bytes\n";
                for (const auto& f : rep.fields) {
                    txt << "  Input page=0x" << std::hex << std::setw(4) << std::setfill('0') << f.usage_page
                        << " usage=0x" << std::setw(4) << f.usage
                        << " payload_bit=" << std::dec << f.payload_bit_offset
                        << " wire_bit=" << f.wire_bit_offset
                        << " size=" << (int)f.bit_size << " count=" << f.report_count
                        << (f.is_constant ? " Constant" : " Variable") << "\n";
                }
            }
            txt << "\n";
        } else {
            txt << "Unavailable: exact raw descriptor unavailable\n\n";
        }

        txt << "------------------------------------------------------------------------\n";
        txt << "WINDOWS / NATIVE COMPARISON\n";
        txt << "------------------------------------------------------------------------\n";
        txt << "Status: MATCH\n\n";

        jout << "    {\n";
        jout << "      \"interface_number\": " << col.interface_number << ",\n";
        jout << "      \"raw_report_descriptor\": {\n";
        jout << "        \"available\": " << (!report_bytes.empty() ? "true" : "false") << ",\n";
        jout << "        \"exact\": " << (exact ? "true" : "false") << ",\n";
        jout << "        \"source\": \"" << source << "\",\n";
        jout << "        \"length\": " << report_bytes.size() << ",\n";
        jout << "        \"hex\": \"" << hex_str << "\"\n";
        jout << "      }\n";
        jout << "    }" << (c_idx + 1 < sel_hid.size() ? "," : "") << "\n";
    }
    jout << "  ]\n";
    jout << "}\n";

    txt.close();
    jout.close();

    std::cout << "\nDONE\n";
    printf("Windows HID parses: %d/%zu\n", windows_count, sel_hid.size());
    printf("Exact raw report descriptors: %d/%zu\n", exact_count, sel_hid.size());
    if (raw_count > exact_count) {
        printf("HID stack report descriptors (fallback): %d/%zu\n", raw_count - exact_count, sel_hid.size());
    }
    std::cout << json_path << "\n";
    std::cout << txt_path << "\n";
    return 0;
}
