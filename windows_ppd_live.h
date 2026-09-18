#pragma once

struct PpdHandleOwner {
    HANDLE value=nullptr;
    ~PpdHandleOwner(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    PpdHandleOwner(const PpdHandleOwner&)=delete;
    PpdHandleOwner& operator=(const PpdHandleOwner&)=delete;
    explicit PpdHandleOwner(HANDLE h):value(h){}
};

struct PpdApiOwner {
    PHIDP_PREPARSED_DATA value=nullptr;
    ~PpdApiOwner(){if(value)HidD_FreePreparsedData(value);}
    PpdApiOwner()=default;
    PpdApiOwner(const PpdApiOwner&)=delete;
    PpdApiOwner& operator=(const PpdApiOwner&)=delete;
};

static std::wstring ppd_device_path_key(std::wstring path) {
    for(auto& c:path) c=static_cast<wchar_t>(std::towupper(c));
    if(path.size()>=4 && path.substr(0,4)==L"\\??\\")path.replace(0,4,L"\\\\?\\");
    return path;
}

static std::wstring ppd_collection_path_key(const std::wstring& path) {
    auto key=ppd_device_path_key(path);
    const size_t suffix=key.rfind(L"#{");
    if(suffix!=std::wstring::npos&&key.size()-suffix>=39&&key[suffix+38]==L'}'&&
        (key.size()-suffix==39||key[suffix+39]==L'\\'))key.resize(suffix);
    return key;
}

static bool ppd_interface_devinst(const std::wstring& path,DEVINST& devinst) {
    const HDEVINFO handle=SetupDiCreateDeviceInfoList(nullptr,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    std::unique_ptr<void,decltype(&SetupDiDestroyDeviceInfoList)> owner(handle,&SetupDiDestroyDeviceInfoList);
    SP_DEVICE_INTERFACE_DATA iface{};iface.cbSize=sizeof(iface);
    if(!SetupDiOpenDeviceInterfaceW(handle,path.c_str(),0,&iface))return false;
    DWORD needed=0;
    SetupDiGetDeviceInterfaceDetailW(handle,&iface,nullptr,0,&needed,nullptr);
    if(needed<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)||needed>65536)return false;
    std::vector<uint32_t> storage((needed+3)/4);
    auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
    detail->cbSize=sizeof(*detail);
    SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
    if(!SetupDiGetDeviceInterfaceDetailW(handle,&iface,detail,needed,nullptr,&device))return false;
    devinst=device.DevInst;
    return true;
}

static std::string ppd_current_os_version() {
    using VersionFn=LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
    if(!ntdll)return {};
    const FARPROC address=GetProcAddress(ntdll,"RtlGetVersion");
    VersionFn fn=nullptr;
    static_assert(sizeof(fn)==sizeof(address),"Function pointer width");
    std::memcpy(&fn,&address,sizeof(fn));
    if(!fn)return {};
    OSVERSIONINFOW version{};version.dwOSVersionInfoSize=sizeof(version);
    if(fn(&version)!=0)return {};
    return std::to_string(version.dwMajorVersion)+"."+std::to_string(version.dwMinorVersion)+"."+std::to_string(version.dwBuildNumber);
}

static bool ppd_acquire_ioctl(HANDLE h,WindowsHidInfo& info) {
    for(int attempt=0;attempt<3;++attempt) {
        HID_COLLECTION_INFORMATION metadata{};
        DWORD returned=0;
        if(!DeviceIoControl(h,IOCTL_HID_GET_COLLECTION_INFORMATION,nullptr,0,&metadata,sizeof(metadata),&returned,nullptr)) {
            info.ppd_acquisition_diagnostics.push_back("collection_information: "+win32_error_text(GetLastError()));
            return false;
        }
        if(returned!=sizeof(metadata)||metadata.DescriptorSize<44||metadata.DescriptorSize>ppd_max_bytes) {
            info.ppd_acquisition_diagnostics.push_back("collection_information: invalid returned structure or DescriptorSize");
            return false;
        }
        std::vector<uint32_t> aligned((metadata.DescriptorSize+3)/4);
        returned=0;
        if(DeviceIoControl(h,IOCTL_HID_GET_COLLECTION_DESCRIPTOR,nullptr,0,aligned.data(),metadata.DescriptorSize,&returned,nullptr)) {
            if(!returned||returned>metadata.DescriptorSize) {
                info.ppd_acquisition_diagnostics.push_back("collection_descriptor: invalid returned byte count");
                return false;
            }
            const auto* p=reinterpret_cast<const uint8_t*>(aligned.data());
            info.cached_collection_descriptor_bytes.assign(p,p+returned);
            info.ppd_source="live_hid_api_collection_descriptor";
            info.ppd_length_source="HID_COLLECTION_INFORMATION.DescriptorSize_and_DeviceIoControl_returned_bytes";
            info.ppd_collection_match="opened_collection_handle";
            return true;
        }
        const DWORD error=GetLastError();
        info.ppd_acquisition_diagnostics.push_back("collection_descriptor: "+win32_error_text(error));
        if(error!=ERROR_INSUFFICIENT_BUFFER&&error!=ERROR_MORE_DATA)break;
    }
    return false;
}

static bool ppd_acquire_raw_input(const std::wstring& path,WindowsHidInfo& info) {
    const auto key=ppd_device_path_key(path);
    const auto collection_key=ppd_collection_path_key(path);
    DEVINST expected_devinst=0;
    const bool resolved=ppd_interface_devinst(path,expected_devinst);
    for(int retry=0;retry<3;++retry) {
        UINT count=0;
        if(GetRawInputDeviceList(nullptr,&count,sizeof(RAWINPUTDEVICELIST))==UINT(-1)||count>4096) {
            info.ppd_acquisition_diagnostics.push_back("raw_input_list: "+win32_error_text(GetLastError()));return false;
        }
        if(!count)break;
        std::vector<RAWINPUTDEVICELIST> list(count);
        const UINT received=GetRawInputDeviceList(list.data(),&count,sizeof(RAWINPUTDEVICELIST));
        if(received==UINT(-1))continue;
        if(received>list.size())return false;
        list.resize(received);
        for(const auto& device:list) {
            UINT chars=0;
            if(GetRawInputDeviceInfoW(device.hDevice,RIDI_DEVICENAME,nullptr,&chars)!=0||!chars||chars>65536)continue;
            std::vector<wchar_t> name(chars+1,0);
            UINT capacity=chars;
            const UINT got=GetRawInputDeviceInfoW(device.hDevice,RIDI_DEVICENAME,name.data(),&capacity);
            if(got==UINT(-1)||got>chars||capacity>chars)continue;
            const std::wstring actual_name(name.data());
            const bool exact_name=ppd_device_path_key(actual_name)==key;
            if(!exact_name) {
                DEVINST actual_devinst=0;
                if(!resolved||ppd_collection_path_key(actual_name)!=collection_key||
                    !ppd_interface_devinst(actual_name,actual_devinst)||actual_devinst!=expected_devinst)continue;
            }
            for(int attempt=0;attempt<3;++attempt) {
                UINT size=0;
                const UINT query=GetRawInputDeviceInfoW(device.hDevice,RIDI_PREPARSEDDATA,nullptr,&size);
                if(query!=0||size<44||size>ppd_max_bytes) {
                    info.ppd_acquisition_diagnostics.push_back("raw_input_ppd_size: returned="+std::to_string(query)+" bytes="+std::to_string(size)+
                        (query==UINT(-1)?" "+win32_error_text(GetLastError()):"; the matched Raw Input entry did not expose a supported PPD length"));
                    break;
                }
                std::vector<uint32_t> aligned((size+3)/4);
                UINT actual=size;
                const UINT bytes=GetRawInputDeviceInfoW(device.hDevice,RIDI_PREPARSEDDATA,aligned.data(),&actual);
                if(bytes==UINT(-1)) {
                    const DWORD error=GetLastError();
                    if(error==ERROR_INSUFFICIENT_BUFFER)continue;
                    info.ppd_acquisition_diagnostics.push_back("raw_input_ppd: "+win32_error_text(error));break;
                }
                if(!bytes||bytes>size||actual>size) {
                    info.ppd_acquisition_diagnostics.push_back("raw_input_ppd: invalid returned byte count");break;
                }
                const auto* p=reinterpret_cast<const uint8_t*>(aligned.data());
                info.cached_collection_descriptor_bytes.assign(p,p+bytes);
                info.ppd_source="raw_input_api";
                info.ppd_length_source="RIDI_PREPARSEDDATA_size_query_and_returned_bytes";
                info.ppd_collection_match=exact_name?"exact_device_interface_path":"matching_collection_name_and_same_devinst";
                return true;
            }
        }
        break;
    }
    info.ppd_acquisition_diagnostics.push_back("raw_input_ppd: no recoverable PPD for the exact collection device name");
    return false;
}

static void ppd_record_check(PpdOracle& oracle,bool passed,const std::string& detail) {
    if(passed)++oracle.passed;
    else {
        ++oracle.failed;
        if(oracle.outcomes.size()<256)oracle.outcomes.push_back("FAIL "+detail);
    }
}

static void ppd_record_skip(PpdOracle& oracle,const std::string& detail) {
    ++oracle.skipped;
    if(oracle.outcomes.size()<256)oracle.outcomes.push_back("SKIP "+detail);
}

static bool ppd_outside_unchanged(const std::vector<uint8_t>& before,const std::vector<uint8_t>& after,uint32_t start,uint32_t width) {
    if(before.size()!=after.size())return false;
    for(size_t i=0;i<before.size();++i) {
        const uint8_t changed=before[i]^after[i];
        for(uint32_t bit=0;bit<8;++bit)
            if((changed&(1u<<bit))&&(i*8+bit<start||i*8+bit>=uint64_t(start)+width))return false;
    }
    return true;
}

static bool ppd_windows_report_validated(const PpdReportState& state) {
    const auto& v=state.oracle;
    return state.report_id>=0&&state.complete&&v.executed&&v.metadata_matches&&v.report_buffer_ready&&v.failed==0&&
        v.fields_expected>0&&v.fields_checked==v.fields_expected;
}

static bool ppd_report_operation_ready(const PpdReportState& state) {
    return state.length_exact&&ppd_windows_report_validated(state);
}

static std::pair<int,int> ppd_canonical_usage(const std::vector<ParsedField>& fields,std::pair<int,int> usage) {
    for(const auto& f:fields) {
        if(f.usage_aliases.empty()||f.usage_ranges.empty()||!f.selector_mapping_resolved)continue;
        for(const auto& alias:f.usage_aliases) {
            if(alias.usage_page!=usage.first||usage.second<alias.minimum||usage.second>alias.maximum)continue;
            if(f.is_variable) return {f.usage_page,f.usage};
            const int64_t selector=alias.selector_min+usage.second-alias.minimum;
            int page=0,value=0;
            if(ppd_selector_usage(f,selector,page,value))return {page,value};
        }
    }
    return usage;
}

static std::set<std::pair<int,int>> ppd_canonical_usages(const std::vector<ParsedField>& fields,const std::set<std::pair<int,int>>& usages) {
    std::set<std::pair<int,int>> result;
    for(const auto& usage:usages)result.insert(ppd_canonical_usage(fields,usage));
    return result;
}

static std::set<std::pair<int,int>> ppd_decoded_usages(const std::vector<ParsedField>& fields,const std::vector<uint8_t>& wire) {
    std::set<std::pair<int,int>> usages;
    for(const auto& f:fields) {
        if(f.is_constant||f.uninterpreted_storage||!f.ppd_button_cap||!f.interpretation_complete)continue;
        if(f.bit_size>32)continue;
        for(uint32_t slot=0;slot<f.report_count;++slot) {
            uint64_t value=0;
            if(!ppd_read_bits(wire.data(),wire.size(),uint64_t(f.windows_api_bit_offset)+uint64_t(slot)*f.bit_size,f.bit_size,value))continue;
            int page=f.usage_page,usage=f.usage;
            if(f.is_variable) {if(!value)continue;}
            else {
                int64_t selector=static_cast<int64_t>(value);
                if(f.is_signed&&f.bit_size<64&&(value&(UINT64_C(1)<<(f.bit_size-1))))selector-=INT64_C(1)<<f.bit_size;
                if(!ppd_selector_usage(f,selector,page,usage))continue;
            }
            if(usage)usages.insert({page,usage});
        }
    }
    return usages;
}

static bool ppd_windows_usage_set(HIDP_REPORT_TYPE type,PHIDP_PREPARSED_DATA pp,const std::vector<uint8_t>& report,
                                  std::set<std::pair<int,int>>& result,NTSTATUS& status) {
    const ULONG count=HidP_MaxUsageListLength(type,0,pp);
    if(count>65536){status=HIDP_STATUS_BUFFER_TOO_SMALL;return false;}
    std::vector<USAGE_AND_PAGE> list((std::max)(ULONG(1),count));
    ULONG length=static_cast<ULONG>(list.size());
    status=HidP_GetUsagesEx(type,0,list.data(),&length,pp,reinterpret_cast<PCHAR>(const_cast<uint8_t*>(report.data())),static_cast<ULONG>(report.size()));
    if(status!=HIDP_STATUS_SUCCESS||length>list.size())return false;
    for(ULONG i=0;i<length;++i)if(list[i].Usage)result.insert({list[i].UsagePage,list[i].Usage});
    return true;
}

static NTSTATUS ppd_oracle_getter_status(const ParsedField& field,PHIDP_PREPARSED_DATA pp,std::vector<uint8_t>& report,ULONG length) {
    const auto type=static_cast<HIDP_REPORT_TYPE>(ppd_type_index(field.report_type));
    auto* buffer=reinterpret_cast<PCHAR>(report.data());
    if(field.ppd_button_cap) {
        ULONG count=HidP_MaxUsageListLength(type,static_cast<USAGE>(field.usage_page),pp);
        if(count>65536)return HIDP_STATUS_BUFFER_TOO_SMALL;
        std::vector<USAGE> usages((std::max)(ULONG(1),count));
        count=static_cast<ULONG>(usages.size());
        return HidP_GetUsages(type,static_cast<USAGE>(field.usage_page),static_cast<USHORT>(field.ppd_link_collection),usages.data(),&count,pp,buffer,length);
    }
    if(field.report_count>1) {
        const size_t bytes=(uint64_t(field.bit_size)*field.report_count+7)/8;
        if(bytes>USHRT_MAX)return HIDP_STATUS_BUFFER_TOO_SMALL;
        std::vector<char> values(bytes);
        return HidP_GetUsageValueArray(type,static_cast<USAGE>(field.usage_page),static_cast<USHORT>(field.ppd_link_collection),
            static_cast<USAGE>(field.usage),values.data(),static_cast<USHORT>(values.size()),pp,buffer,length);
    }
    ULONG value=0;
    return HidP_GetUsageValue(type,static_cast<USAGE>(field.usage_page),static_cast<USHORT>(field.ppd_link_collection),
        static_cast<USAGE>(field.usage),&value,pp,buffer,length);
}

struct PpdInitializationApi {
    decltype(&HidP_InitializeReportForID) initialize = HidP_InitializeReportForID;
    decltype(&HidP_SetUsageValue) set_value = HidP_SetUsageValue;
    decltype(&HidP_SetUsageValueArray) set_array = HidP_SetUsageValueArray;
};

static void ppd_audit_native_initializers(WindowsHidInfo& info, PHIDP_PREPARSED_DATA trusted_pp,
        const PpdInitializationApi& api = {}) {
    if (!trusted_pp || !info.available || !api.initialize) return;
    const std::array<const std::vector<HIDP_BUTTON_CAPS>*,3> buttons = {&info.input_button_caps,&info.output_button_caps,&info.feature_button_caps};
    const std::array<const std::vector<HIDP_VALUE_CAPS>*,3> values = {&info.input_value_caps,&info.output_value_caps,&info.feature_value_caps};
    const std::array<USHORT,3> lengths = {info.caps.InputReportByteLength,info.caps.OutputReportByteLength,info.caps.FeatureReportByteLength};
    const size_t capacity = *std::max_element(lengths.begin(),lengths.end());
    for (int type = 0; type < 3; ++type) {
        auto& audit = info.native_initializer_audit[type];
        audit = {};
        if (!lengths[type] || !info.public_type_caps_complete[type]) continue;
        std::set<int> declared;
        for (const auto& cap : *buttons[type]) declared.insert(cap.ReportID);
        for (const auto& cap : *values[type]) declared.insert(cap.ReportID);
        if (declared.empty()) continue;
        audit.executed = true;
        audit.buffer_length = lengths[type];
        std::vector<uint8_t> buffer(capacity + 32,0);
        auto probe = [&](int id, ULONG length, NTSTATUS expected, const char* test) {
            std::fill(buffer.begin(),buffer.end(),0xa5);
            std::fill(buffer.begin(),buffer.begin() + length,0);
            const auto status = api.initialize(static_cast<HIDP_REPORT_TYPE>(type),static_cast<UCHAR>(id),trusted_pp,
                reinterpret_cast<PCHAR>(buffer.data()),length);
            const bool bounded = std::all_of(buffer.begin() + length,buffer.end(),[](uint8_t byte) { return byte == 0xa5; });
            if (status == expected && (expected != HIDP_STATUS_SUCCESS || buffer[0] == id) && bounded) ++audit.checks_passed;
            else {
                ++audit.checks_failed;
                audit.failures.push_back({test,id,length,declared.count(id) != 0,expected,status,buffer[0],bounded});
            }
            return bounded;
        };
        bool bounded = true;
        for (int id = 0; id < 256 && bounded; ++id)
            bounded = probe(id,lengths[type],declared.count(id) ? HIDP_STATUS_SUCCESS : HIDP_STATUS_REPORT_DOES_NOT_EXIST,"report_type_id_membership");
        if (bounded) probe(*declared.begin(),lengths[type] - 1,HIDP_STATUS_INVALID_REPORT_LENGTH,"short_buffer");
    }
}

static void ppd_initializer_check(PpdOracle& oracle, bool passed, const std::string& detail) {
    if (passed) ++oracle.initializer_checks_passed;
    else {
        ++oracle.initializer_checks_failed;
        if (oracle.initializer_diagnostics.size() < 32) oracle.initializer_diagnostics.push_back(detail);
    }
}

static bool ppd_initialize_oracle_report(const PpdReportState& state, const std::vector<ParsedField>& fields,
        PHIDP_PREPARSED_DATA trusted_pp, std::vector<uint8_t>& report, PpdOracle& oracle,
        const PpdInitializationApi& api = {}) {
    report.clear();
    oracle.report_buffer_ready = false;
    oracle.native_initializer_called = false;
    oracle.initialization_setter_called = false;
    oracle.initialization_method.clear();
    if (!trusted_pp || state.report_id < 0 || state.report_id > 255 || state.type < 0 || state.type > 2 ||
        !state.windows_bytes || state.windows_bytes > USHRT_MAX || !state.complete || !oracle.metadata_matches) return false;
    const auto type = static_cast<HIDP_REPORT_TYPE>(state.type);
    const ParsedField* seed = nullptr;
    bool scalar_zero = true;
    for (const auto& field : fields) {
        if (field.is_constant || field.uninterpreted_storage) continue;
        if (field.report_type != ppd_type_name(state.type) || field.report_id != state.report_id ||
            !field.interpretation_complete || !field.selector_mapping_resolved || !field.bit_size || field.bit_size > 32 ||
            !field.report_count || field.report_count > 4096 || field.windows_api_bit_offset < 8 ||
            uint64_t(field.windows_api_bit_offset) + uint64_t(field.bit_size) * field.report_count > uint64_t(state.windows_bytes) * 8 ||
            field.usage_page < 0 || field.usage_page > USHRT_MAX || field.usage < 0 || field.usage > USHRT_MAX ||
            field.ppd_link_collection < 0 || field.ppd_link_collection > USHRT_MAX) return false;
        if (field.ppd_button_cap || !field.is_variable || field.has_null_state) scalar_zero = false;
        if (!seed) seed = &field;
    }
    if (!seed) return false;
    std::vector<uint8_t> prepared(size_t(state.windows_bytes) + 32,0xa5);
    std::fill(prepared.begin(),prepared.begin() + state.windows_bytes,0);
    prepared[0] = static_cast<uint8_t>(state.report_id);
    auto bounded = [&] {
        return std::all_of(prepared.begin() + state.windows_bytes,prepared.end(),[](uint8_t byte) { return byte == 0xa5; });
    };
    if (scalar_zero) {
        const std::string setter = seed->report_count == 1 ? "HidP_SetUsageValue" : "HidP_SetUsageValueArray";
        NTSTATUS set = HIDP_STATUS_NOT_IMPLEMENTED;
        if (seed->report_count == 1 && api.set_value) {
            oracle.initialization_setter_called = true;
            set = api.set_value(type,static_cast<USAGE>(seed->usage_page),static_cast<USHORT>(seed->ppd_link_collection),
                static_cast<USAGE>(seed->usage),0,trusted_pp,reinterpret_cast<PCHAR>(prepared.data()),state.windows_bytes);
        } else if (seed->report_count > 1 && api.set_array) {
            const size_t bytes = (uint64_t(seed->bit_size) * seed->report_count + 7) / 8;
            if (bytes > USHRT_MAX) return false;
            std::vector<char> values(bytes,0);
            oracle.initialization_setter_called = true;
            set = api.set_array(type,static_cast<USAGE>(seed->usage_page),static_cast<USHORT>(seed->ppd_link_collection),
                static_cast<USAGE>(seed->usage),values.data(),static_cast<USHORT>(bytes),trusted_pp,
                reinterpret_cast<PCHAR>(prepared.data()),state.windows_bytes);
        }
        oracle.initialization_setter_status = set;
        if (set != HIDP_STATUS_SUCCESS || prepared[0] != state.report_id || !bounded() ||
            !std::all_of(prepared.begin() + 1,prepared.begin() + state.windows_bytes,[](uint8_t byte) { return byte == 0; })) {
            oracle.initializer_diagnostics.push_back("Typed zero-payload " + setter + " failed: status=" + std::to_string(set) +
                " returned_id=" + std::to_string(prepared[0]) + "; report discarded");
            return false;
        }
        oracle.initialization_method = "typed_zero_payload_report_id_and_" + setter;
    } else {
        if (!api.initialize) return false;
        oracle.native_initializer_called = true;
        const auto status = api.initialize(type,static_cast<UCHAR>(state.report_id),trusted_pp,
            reinterpret_cast<PCHAR>(prepared.data()),state.windows_bytes);
        oracle.initializer_status = status;
        const bool accepted = status == HIDP_STATUS_SUCCESS && prepared[0] == state.report_id && bounded();
        ppd_initializer_check(oracle,accepted,"InitializeReportForID status=" + std::to_string(status) +
            " returned_id=" + std::to_string(prepared[0]));
        if (!accepted) return false;
        prepared.resize(state.windows_bytes);
        if (!ppd_decoded_usages(fields,prepared).empty()) {
            oracle.initializer_diagnostics.push_back("Native initializer left active button/selector usages; report discarded");
            return false;
        }
        for (const auto& field : fields) {
            if (field.is_constant || field.uninterpreted_storage || field.ppd_button_cap) continue;
            for (uint32_t slot = 0; slot < field.report_count; ++slot) {
                uint64_t raw = 0;
                if (!ppd_read_bits(prepared.data(),prepared.size(),uint64_t(field.windows_api_bit_offset) + uint64_t(slot) * field.bit_size,field.bit_size,raw)) return false;
                int64_t value = static_cast<int64_t>(raw);
                if (field.is_signed && (raw & (UINT64_C(1) << (field.bit_size - 1)))) value -= INT64_C(1) << field.bit_size;
                if (field.has_null_state ? value >= field.logical_min && value <= field.logical_max : raw != 0) {
                    oracle.initializer_diagnostics.push_back("Native initializer did not establish zero/null data for the requested report; report discarded");
                    return false;
                }
            }
        }
        oracle.initialization_method = "HidP_InitializeReportForID_checked_zero_null";
    }
    prepared.resize(state.windows_bytes);
    report.swap(prepared);
    oracle.report_buffer_ready = true;
    return true;
}

static void ppd_validate_windows(WindowsHidInfo& info,PHIDP_PREPARSED_DATA trusted_pp) {
    if(!trusted_pp||!info.available||!info.ppd.supported)return;
    ppd_audit_native_initializers(info,trusted_pp);
    const std::array<const std::vector<HIDP_BUTTON_CAPS>*,3> buttons={&info.input_button_caps,&info.output_button_caps,&info.feature_button_caps};
    const std::array<const std::vector<HIDP_VALUE_CAPS>*,3> values={&info.input_value_caps,&info.output_value_caps,&info.feature_value_caps};
    const std::array<USHORT,3> lengths={info.caps.InputReportByteLength,info.caps.OutputReportByteLength,info.caps.FeatureReportByteLength};
    for(auto& state:info.ppd.report_states) {
        auto& check=state.oracle;
        check={};
        check.executed=true;
        if(state.report_id<0) {ppd_record_skip(check,"No occupied records establish an ID for this report type");continue;}
        const auto type=static_cast<HIDP_REPORT_TYPE>(state.type);
        check.metadata_matches=info.public_type_caps_complete[state.type]&&info.public_links_complete&&info.caps.Usage==info.ppd.usage&&info.caps.UsagePage==info.ppd.usage_page&&
            lengths[state.type]==state.windows_bytes&&info.caps.NumberLinkCollectionNodes==info.ppd.parsed.collections.size();
        for(size_t i=0;i<info.link_collections.size()&&i<info.ppd.parsed.collections.size();++i) {
            const auto& a=info.link_collections[i];const auto& c=info.ppd.parsed.collections[i];
            check.metadata_matches &= a.LinkUsage==c.usage&&a.LinkUsagePage==c.usage_page&&a.CollectionType==c.collection_type&&
                (i?int(a.Parent)+1:0)==c.parent_ordinal;
        }
        size_t button_records=0,value_records=0;
        for(const auto& r:info.ppd.records) {
            if(r.type!=state.type||r.id!=state.report_id||r.main_flags&1)continue;
            bool found=false;
            const bool range=(r.flags&16)!=0;
            if(r.flags&4) {
                ++button_records;
                for(const auto& cap:*buttons[state.type]) {
                    if(cap.ReportID!=r.id||cap.UsagePage!=r.page||cap.LinkCollection!=r.link||cap.BitField!=r.main_flags||
                       !!cap.IsRange!=range||!!cap.IsAlias!=!!(r.flags&32))continue;
                    if((range&&(cap.Range.UsageMin!=r.range[0]||cap.Range.UsageMax!=r.range[1]))||(!range&&cap.NotRange.Usage!=r.range[0]))continue;
                    if(info.hid_api_version>=2 && cap.ReportCount && cap.ReportCount!=r.count)continue;
                    found=true;break;
                }
            } else {
                ++value_records;
                for(const auto& cap:*values[state.type]) {
                    if(cap.ReportID!=r.id||cap.UsagePage!=r.page||cap.LinkCollection!=r.link||cap.BitField!=r.main_flags||
                        cap.BitSize!=r.size||!!cap.IsRange!=range||!!cap.IsAlias!=!!(r.flags&32))continue;
                    if((range&&(cap.Range.UsageMin!=r.range[0]||cap.Range.UsageMax!=r.range[1]))||(!range&&cap.NotRange.Usage!=r.range[0]))continue;
                    if(uint32_t(cap.LogicalMin)!=r.raw_union[1]||uint32_t(cap.LogicalMax)!=r.raw_union[2]||
                        uint32_t(cap.PhysicalMin)!=r.raw_union[3]||uint32_t(cap.PhysicalMax)!=r.raw_union[4]||
                        cap.Units!=r.units||cap.UnitsExp!=r.units_exp||(!range&&cap.ReportCount!=r.count))continue;
                    found=true;break;
                }
            }
            if(!found)check.metadata_matches=false;
        }
        size_t public_buttons=0,public_values=0;
        for(const auto& c:*buttons[state.type])if(c.ReportID==state.report_id && !(c.BitField&1))++public_buttons;
        for(const auto& c:*values[state.type])if(c.ReportID==state.report_id && !(c.BitField&1))++public_values;
        if(button_records!=public_buttons||value_records!=public_values)check.metadata_matches=false;
        ppd_record_check(check,check.metadata_matches,"Decoded header, capability records and links versus public HID APIs");
        std::vector<ParsedField> fields;
        for(const auto& f:info.ppd.parsed.fields)if(f.report_type==ppd_type_name(state.type)&&f.report_id==state.report_id)fields.push_back(f);
        for(const auto& f:fields)if(!f.is_constant&&!f.uninterpreted_storage)++check.fields_expected;
        std::vector<uint8_t> neutral;
        const bool initialized=ppd_initialize_oracle_report(state,fields,trusted_pp,neutral,check);
        ppd_record_check(check,initialized,"Windows report-buffer initialization");
        if(!initialized) {
            ppd_record_skip(check,"Control comparisons not run: no validated report-buffer initialization path");
            continue;
        }
        ppd_record_check(check,neutral[0]==state.report_id,"Initialized Windows buffer contains the requested report ID");
        int absent=-1;
        for(int id=1;id<256;++id) {
            bool exists=false;
            for(const auto& other:info.ppd.report_states)if(other.type==state.type&&other.report_id==id)exists=true;
            if(!exists){absent=id;break;}
        }
        auto compare_usages=[&](const std::vector<uint8_t>& wire,const std::string& reason) {
            std::set<std::pair<int,int>> actual;NTSTATUS status=0;
            const bool acquired=ppd_windows_usage_set(type,trusted_pp,wire,actual,status);
            ppd_record_check(check,acquired&&ppd_canonical_usages(fields,actual)==ppd_canonical_usages(fields,ppd_decoded_usages(fields,wire)),reason+" status="+std::to_string(status));
        };
        for(const auto& field:fields) {
            if(field.is_constant||field.uninterpreted_storage)continue;
            const auto failed_before=check.failed,skipped_before=check.skipped,passed_before=check.passed;
            const std::string label=field.report_type+" ID "+std::to_string(field.report_id)+" "+usage_name(field.usage_page,field.usage);
            if(!check.metadata_matches||!field.interpretation_complete||!field.selector_mapping_resolved) {
                ppd_record_skip(check,label+": unresolved metadata or interpretation");continue;
            }
            const uint64_t estimated_work=uint64_t(field.report_count)*(field.ppd_button_cap?uint64_t(fields.size())*12:12);
            if(check.passed+check.failed+check.skipped>65536||field.bit_size>32||field.report_count>4096||estimated_work>1000000) {
                ppd_record_skip(check,label+": oracle work or API width limit");continue;
            }
            const uint32_t start=static_cast<uint32_t>(field.windows_api_bit_offset);
            const uint32_t span=field.bit_size*field.report_count;
            auto invalid=neutral;
            const auto short_status=ppd_oracle_getter_status(field,trusted_pp,invalid,static_cast<ULONG>(invalid.size()-1));
            ppd_record_check(check,short_status==HIDP_STATUS_INVALID_REPORT_LENGTH,label+" getter rejects a short buffer "+std::to_string(short_status));
            if(absent>=0) {
                invalid=neutral;invalid[0]=static_cast<uint8_t>(absent);
                const auto wrong_id=ppd_oracle_getter_status(field,trusted_pp,invalid,static_cast<ULONG>(invalid.size()));
                ppd_record_check(check,wrong_id==HIDP_STATUS_INCOMPATIBLE_REPORT_ID,label+" getter rejects an incompatible report ID "+std::to_string(wrong_id));
            } else ppd_record_skip(check,label+": no absent report ID is available for the negative getter test");
            if(field.ppd_button_cap) {
                compare_usages(neutral,label+" initialized/null usage set");
                std::vector<std::pair<int,int>> samples;
                if(field.is_variable) samples.push_back({field.usage_page,field.usage});
                else for(const auto& range:field.usage_ranges) {
                    const int first=(std::max)(1,range.minimum);
                    if(first<=range.maximum) {
                        samples.push_back({range.usage_page,first});
                        if(range.maximum!=first)samples.push_back({range.usage_page,range.maximum});
                        if(first+1<range.maximum)samples.push_back({range.usage_page,first+1});
                    }
                }
                for(const auto& alias:field.usage_aliases) {
                    if(alias.minimum>0)samples.push_back({alias.usage_page,alias.minimum});
                    if(alias.maximum>0&&alias.maximum!=alias.minimum)samples.push_back({alias.usage_page,alias.maximum});
                }
                if(samples.empty()){ppd_record_skip(check,label+": no nonzero selector usage to compare");continue;}
                if(samples.size()>128){samples.resize(128);ppd_record_skip(check,label+": usage-group test bound");}
                for(const auto& sample:samples) {
                    auto win_report=neutral;
                    USAGE u=static_cast<USAGE>(sample.second);ULONG n=1;
                    const auto status=HidP_SetUsages(type,static_cast<USAGE>(sample.first),static_cast<USHORT>(field.ppd_link_collection),&u,&n,
                        trusted_pp,reinterpret_cast<PCHAR>(win_report.data()),static_cast<ULONG>(win_report.size()));
                    ppd_record_check(check,status==HIDP_STATUS_SUCCESS,label+" Windows SetUsages "+std::to_string(status));
                    if(status==HIDP_STATUS_SUCCESS) {
                        ppd_record_check(check,ppd_outside_unchanged(neutral,win_report,start,span),label+" Windows setter preserves unrelated fields");
                        compare_usages(win_report,label+" Windows write / decoded usage set");
                        ppd_record_check(check,ppd_canonical_usages(fields,ppd_decoded_usages({field},win_report)).count(ppd_canonical_usage(fields,sample))!=0,label+" requested usage recovered from the addressed field");
                        n=1;
                        const auto cleared=HidP_UnsetUsages(type,static_cast<USAGE>(sample.first),static_cast<USHORT>(field.ppd_link_collection),&u,&n,
                            trusted_pp,reinterpret_cast<PCHAR>(win_report.data()),static_cast<ULONG>(win_report.size()));
                        ppd_record_check(check,cleared==HIDP_STATUS_SUCCESS,label+" Windows UnsetUsages");
                        if(cleared==HIDP_STATUS_SUCCESS)compare_usages(win_report,label+" cleared usage set");
                    }
                    int64_t selector=1;
                    if(!field.is_variable&&!ppd_usage_selector(field,sample.first,sample.second,selector)) {
                        ppd_record_skip(check,label+": selector mapping unavailable");continue;
                    }
                    const uint64_t mask=(UINT64_C(1)<<field.bit_size)-1;
                    const uint64_t encoded=uint64_t(selector)&mask;
                    for(uint32_t slot=0;slot<field.report_count;++slot) {
                        auto ours=neutral;
                        const bool written=ppd_write_bits(ours.data(),ours.size(),uint64_t(start)+uint64_t(slot)*field.bit_size,field.bit_size,encoded);
                        ppd_record_check(check,written&&ppd_outside_unchanged(neutral,ours,start,span),label+" bounded slot write and unrelated bits");
                        if(written) {
                            compare_usages(ours,label+" our slot write / Windows usage set");
                            std::set<std::pair<int,int>> got;NTSTATUS status2=0;
                            const bool read=ppd_windows_usage_set(type,trusted_pp,ours,got,status2);
                            ppd_record_check(check,read&&ppd_canonical_usages(fields,got).count(ppd_canonical_usage(fields,sample))!=0,label+" Windows decoded requested slot usage");
                        }
                    }
                }
                if(!field.is_variable&&field.report_count>1) {
                    auto ours=neutral,win=neutral;
                    std::map<int,std::vector<USAGE>> by_page;
                    uint32_t slot=0;
                    std::set<std::pair<int,int>> inserted;
                    for(const auto& sample:samples) {
                        if(slot>=field.report_count)break;
                        if(!inserted.insert(ppd_canonical_usage(fields,sample)).second)continue;
                        int64_t selector=0;
                        if(!ppd_usage_selector(field,sample.first,sample.second,selector))continue;
                        ppd_write_bits(ours.data(),ours.size(),uint64_t(start)+uint64_t(slot)*field.bit_size,field.bit_size,uint64_t(selector)&((UINT64_C(1)<<field.bit_size)-1));
                        by_page[sample.first].push_back(static_cast<USAGE>(sample.second));++slot;
                    }
                    compare_usages(ours,label+" simultaneous selectors from bounded writer");
                    for(auto& entry:by_page) {
                        ULONG count=static_cast<ULONG>(entry.second.size());
                        const auto status=HidP_SetUsages(type,static_cast<USAGE>(entry.first),static_cast<USHORT>(field.ppd_link_collection),entry.second.data(),&count,
                            trusted_pp,reinterpret_cast<PCHAR>(win.data()),static_cast<ULONG>(win.size()));
                        ppd_record_check(check,status==HIDP_STATUS_SUCCESS,label+" Windows simultaneous selectors");
                    }
                    compare_usages(win,label+" simultaneous selectors from Windows");
                }
            } else {
                const uint64_t mask=(UINT64_C(1)<<field.bit_size)-1;
                const std::set<uint64_t> distinct_patterns={0,mask,UINT64_C(1)<<(field.bit_size-1),(UINT64_C(1)<<(field.bit_size-1))-1,
                    uint64_t(field.logical_min)&mask,uint64_t(field.logical_max)&mask};
                const std::vector<uint64_t> patterns(distinct_patterns.begin(),distinct_patterns.end());
                const bool repeated=field.report_count>1;
                for(size_t sample=0;sample<patterns.size()+(repeated?2:0);++sample) {
                    auto win=neutral,ours=neutral;
                    const size_t packed_bytes=(span+7)/8;
                    std::vector<uint8_t> packed(packed_bytes,0),readback(packed_bytes,0);
                    std::vector<uint64_t> expected(field.report_count);
                    if(packed_bytes>USHRT_MAX){ppd_record_skip(check,label+": value-array API length bound");break;}
                    for(uint32_t slot=0;slot<field.report_count;++slot) {
                        const uint64_t pattern=sample<patterns.size()?patterns[sample]:
                            (patterns[slot%patterns.size()]^((uint64_t(slot)+1)*UINT64_C(0x9e3779b9))^(sample==patterns.size()?0:mask))&mask;
                        expected[slot]=pattern;
                        ppd_write_bits(packed.data(),packed.size(),uint64_t(slot)*field.bit_size,field.bit_size,pattern);
                        ppd_write_bits(ours.data(),ours.size(),uint64_t(start)+uint64_t(slot)*field.bit_size,field.bit_size,pattern);
                    }
                    NTSTATUS set=0,get=0;
                    if(repeated) {
                        set=HidP_SetUsageValueArray(type,static_cast<USAGE>(field.usage_page),static_cast<USHORT>(field.ppd_link_collection),static_cast<USAGE>(field.usage),
                            reinterpret_cast<PCHAR>(packed.data()),static_cast<USHORT>(packed_bytes),trusted_pp,reinterpret_cast<PCHAR>(win.data()),static_cast<ULONG>(win.size()));
                        get=HidP_GetUsageValueArray(type,static_cast<USAGE>(field.usage_page),static_cast<USHORT>(field.ppd_link_collection),static_cast<USAGE>(field.usage),
                            reinterpret_cast<PCHAR>(readback.data()),static_cast<USHORT>(packed_bytes),trusted_pp,reinterpret_cast<PCHAR>(ours.data()),static_cast<ULONG>(ours.size()));
                    } else {
                        set=HidP_SetUsageValue(type,static_cast<USAGE>(field.usage_page),static_cast<USHORT>(field.ppd_link_collection),static_cast<USAGE>(field.usage),
                            static_cast<ULONG>(expected.front()),trusted_pp,reinterpret_cast<PCHAR>(win.data()),static_cast<ULONG>(win.size()));
                        ULONG raw=0;
                        get=HidP_GetUsageValue(type,static_cast<USAGE>(field.usage_page),static_cast<USHORT>(field.ppd_link_collection),static_cast<USAGE>(field.usage),
                            &raw,trusted_pp,reinterpret_cast<PCHAR>(ours.data()),static_cast<ULONG>(ours.size()));
                        ppd_write_bits(readback.data(),readback.size(),0,field.bit_size,raw&mask);
                    }
                    ppd_record_check(check,set==HIDP_STATUS_SUCCESS,label+" Windows scalar/value-array setter "+std::to_string(set));
                    ppd_record_check(check,get==HIDP_STATUS_SUCCESS,label+" Windows scalar/value-array getter "+std::to_string(get));
                    if(set==HIDP_STATUS_SUCCESS)for(uint32_t slot=0;slot<field.report_count;++slot) {
                        uint64_t raw=0;
                        const bool read=ppd_read_bits(win.data(),win.size(),uint64_t(start)+uint64_t(slot)*field.bit_size,field.bit_size,raw);
                        ppd_record_check(check,read&&raw==expected[slot],label+" every Windows value-array slot decoded");
                    }
                    if(get==HIDP_STATUS_SUCCESS)for(uint32_t slot=0;slot<field.report_count;++slot) {
                        uint64_t raw=0;
                        const bool read=ppd_read_bits(readback.data(),readback.size(),uint64_t(slot)*field.bit_size,field.bit_size,raw);
                        ppd_record_check(check,read&&raw==expected[slot],label+" every bounded-writer slot decoded by Windows");
                    }
                    if(set==HIDP_STATUS_SUCCESS)ppd_record_check(check,ppd_outside_unchanged(neutral,win,start,span),label+" Windows value setter preserves unrelated fields");
                    ppd_record_check(check,ppd_outside_unchanged(neutral,ours,start,span),label+" bounded value writer preserves unrelated bits");
                }
            }
            if(check.failed==failed_before&&check.skipped==skipped_before&&check.passed>passed_before)++check.fields_checked;
        }
    }
    for(auto& field:info.ppd.parsed.fields) {
        for(const auto& state:info.ppd.report_states)
            if(field.report_id==state.report_id&&field.report_type==ppd_type_name(state.type))field.ppd_oracle_validated=ppd_windows_report_validated(state);
    }
    for(auto& report:info.ppd.parsed.reports)for(auto& field:report.fields) {
        for(const auto& state:info.ppd.report_states)
            if(field.report_id==state.report_id&&field.report_type==ppd_type_name(state.type))field.ppd_oracle_validated=ppd_windows_report_validated(state);
    }
}

static void write_ppd_metadata_json(std::ostream& out,const WindowsHidInfo& wh) {
    const auto& p=wh.ppd;
    out << "{\"preparsed_data_source\":" << (wh.ppd_source.empty()?"null":json_string(wh.ppd_source))
        <<",\"preparsed_data_length\":"<<wh.cached_collection_descriptor_bytes.size()<<",\"length_source\":"<<json_string(wh.ppd_length_source)
        <<",\"collection_match\":"<<(wh.ppd_collection_match.empty()?"null":json_string(wh.ppd_collection_match))
        <<",\"sidecar_file\":"<<(wh.ppd_sidecar_file.empty()?"null":json_string(wh.ppd_sidecar_file))
        <<",\"ppd_decoder_format\":"<<(p.supported?json_string(ppd_format_id):"null")<<",\"reference_revision\":"<<json_string(ppd_reference_revision)
        <<",\"capture_os_version\":"<<(wh.capture_os_version.empty()?"null":json_string(wh.capture_os_version))
        <<",\"hid_api_version\":"<<(wh.hid_api_version?std::to_string(wh.hid_api_version):"null")
        <<",\"descriptor_exact\":false,\"physical_capture_verified\":false,\"supported\":"<<(p.supported?"true":"false")
        <<",\"decoded_section_bytes\":"<<p.section_end<<",\"trailing_bytes\":"<<(p.section_end<=p.length?p.length-p.section_end:0)
        <<",\"acquisition_diagnostics\":";
    write_strings_json(out,wh.ppd_acquisition_diagnostics);
    out<<",\"diagnostics\":[";
    for(size_t i=0;i<p.diagnostics.size();++i) {
        if(i)out<<',';const auto& d=p.diagnostics[i];
        out<<"{\"severity\":"<<json_string(d.severity)<<",\"code\":"<<json_string(d.code)<<",\"byte_offset\":"<<d.byte_offset<<",\"detail\":"<<json_string(d.detail)<<'}';
    }
    out<<"],\"records\":[";
    for(size_t i=0;i<p.records.size();++i) {
        if(i)out<<',';const auto& r=p.records[i];
        out<<"{\"source_record\":"<<r.index<<",\"byte_offset\":"<<r.offset<<",\"report_type\":"<<json_string(ppd_type_name(r.type))
            <<",\"report_id\":"<<unsigned(r.id)<<",\"windows_api_bit_offset\":"<<uint32_t(r.byte)*8+r.bit
            <<",\"report_size\":"<<r.size<<",\"report_count\":"<<r.count<<",\"bit_count\":"<<r.bit_count<<",\"next_byte_position\":"<<r.next_byte
            <<",\"main_item_flags\":"<<r.main_flags<<",\"internal_flags\":"<<unsigned(r.flags)<<",\"usage_page\":"<<r.page
            <<",\"link_collection\":"<<r.link<<",\"raw_range_words\":[";
        for(size_t j=0;j<r.range.size();++j){if(j)out<<',';out<<r.range[j];}
        out<<"],\"raw_union_u32\":[";
        for(size_t j=0;j<r.raw_union.size();++j){if(j)out<<',';out<<r.raw_union[j];}
        out<<"],\"raw_union_i32\":[";
        for(size_t j=0;j<r.raw_union.size();++j){if(j)out<<',';out<<ppd_signed32(r.raw_union[j]);}
        out<<"],\"units\":"<<r.units<<",\"units_exponent_raw\":"<<r.units_exp<<'}';
    }
    out<<"],\"report_validation\":[";
    for(size_t i=0;i<p.report_states.size();++i) {
        if(i)out<<',';const auto& r=p.report_states[i];const auto& v=r.oracle;
        out<<"{\"report_type\":"<<json_string(ppd_type_name(r.type))<<",\"report_id\":"<<(r.report_id<0?"null":std::to_string(r.report_id))
            <<",\"windows_api_report_byte_length\":"<<r.windows_bytes<<",\"wire_bytes_lower_bound\":"<<r.known_wire_bytes
            <<",\"exact_wire_bytes\":"<<(r.length_exact?std::to_string(r.known_wire_bytes):"null")<<",\"wire_sizes_exact\":"<<(r.length_exact?"true":"false")
            <<",\"reconstruction_complete\":"<<(r.complete?"true":"false")<<",\"verification_source\":"<<json_string(v.executed?"windows_in_memory_oracle":"offline_record_checks")
            <<",\"independent_descriptor\":false,\"physical_capture_verified\":false,\"metadata_matches\":"<<(v.metadata_matches?"true":"false")
            <<",\"checks_passed\":"<<v.passed<<",\"checks_failed\":"<<v.failed<<",\"checks_skipped\":"<<v.skipped
            <<",\"fields_checked\":"<<v.fields_checked<<",\"fields_expected\":"<<v.fields_expected
            <<",\"initialization_method\":"<<(v.initialization_method.empty()?"null":json_string(v.initialization_method))
            <<",\"report_buffer_ready\":"<<(v.report_buffer_ready?"true":"false")
            <<",\"native_initializer_called\":"<<(v.native_initializer_called?"true":"false")
            <<",\"initialization_setter_status\":"<<(v.initialization_setter_called?std::to_string(v.initialization_setter_status):"null")
            <<",\"initializer_status\":"<<(v.native_initializer_called?std::to_string(v.initializer_status):"null")
            <<",\"initializer_checks_passed\":"<<v.initializer_checks_passed<<",\"initializer_checks_failed\":"<<v.initializer_checks_failed
            <<",\"initializer_diagnostics\":";
        write_strings_json(out,v.initializer_diagnostics);
        out
            <<",\"windows_logical_validation_passed\":"<<(ppd_windows_report_validated(r)?"true":"false")<<",\"requires_live_base_report\":true,\"outcomes\":";
        write_strings_json(out,v.outcomes);out<<'}';
    }
    out<<"],\"native_initializer_audit\":[";
    for (int type=0;type<3;++type) {
        if(type)out<<',';
        const auto& audit=wh.native_initializer_audit[type];
        out<<"{\"report_type\":"<<json_string(ppd_type_name(type))
            <<",\"executed\":"<<(audit.executed?"true":"false")
            <<",\"scope\":\"once_per_collection_report_type\",\"affects_layout_readiness\":false"
            <<",\"windows_api_report_byte_length\":"<<audit.buffer_length
            <<",\"checks_passed\":"<<audit.checks_passed<<",\"checks_failed\":"<<audit.checks_failed<<",\"failures\":[";
        for(size_t i=0;i<audit.failures.size();++i) {
            if(i)out<<',';
            const auto& failure=audit.failures[i];
            out<<"{\"test\":"<<json_string(failure.test)<<",\"report_id\":"<<failure.report_id
                <<",\"buffer_length\":"<<failure.buffer_length<<",\"declared_for_type\":"<<(failure.declared_for_type?"true":"false")
                <<",\"expected_status\":"<<failure.expected_status<<",\"actual_status\":"<<failure.actual_status
                <<",\"returned_id\":"<<unsigned(failure.returned_id)<<",\"writes_within_buffer\":"<<(failure.writes_within_buffer?"true":"false")<<'}';
        }
        out<<"]}";
    }
    out<<"]}";
}

static void write_ppd_metadata_txt(std::ostream& out,const WindowsHidInfo& wh) {
    out<<"WINDOWS RAW PREPARSED DATA\nSource: "<<wh.ppd_source<<"\nActual byte length: "<<wh.cached_collection_descriptor_bytes.size()
        <<"\nLength evidence: "<<wh.ppd_length_source<<"\nDecoder format: "<<(wh.ppd.supported?ppd_format_id:"unsupported")
        <<"\nReference revision: "<<ppd_reference_revision<<"\nOriginal descriptor: not recovered\nPhysical traffic verified: no\n";
    for(const auto& d:wh.ppd_acquisition_diagnostics)out<<"Acquisition: "<<d<<'\n';
    for(const auto& d:wh.ppd.diagnostics)out<<d.severity<<" "<<d.code<<" at byte "<<d.byte_offset<<": "<<d.detail<<'\n';
    for(int type=0;type<3;++type) {
        const auto& audit=wh.native_initializer_audit[type];
        if(!audit.executed)continue;
        out<<"Native initializer audit "<<ppd_type_name(type)<<": passed "<<audit.checks_passed<<"; failed "<<audit.checks_failed
            <<"; once per collection/type; diagnostic API conformance, not layout readiness\n";
        for(const auto& failure:audit.failures)
            out<<"  "<<failure.test<<" ID "<<failure.report_id<<"; declared "<<(failure.declared_for_type?"yes":"no")
                <<"; expected "<<failure.expected_status<<"; actual "<<failure.actual_status<<"; returned ID "<<unsigned(failure.returned_id)
                <<"; bounded "<<(failure.writes_within_buffer?"yes":"no")<<'\n';
    }
    for(const auto& r:wh.ppd.report_states) {
        const auto& v=r.oracle;
        out<<ppd_type_name(r.type)<<" ID "<<(r.report_id<0?"unknown":std::to_string(r.report_id))<<": Windows buffer "<<r.windows_bytes
            <<" bytes; wire lower bound "<<r.known_wire_bytes<<"; exact length "<<(r.length_exact?std::to_string(r.known_wire_bytes):"unknown")
            <<"; reconstruction "<<(r.complete?"complete":"incomplete")<<"\nVerification: "<<(v.executed?"Windows in-memory oracle":"offline record checks")
            <<"; passed "<<v.passed<<"; failed "<<v.failed<<"; skipped "<<v.skipped<<"; fields "<<v.fields_checked<<'/'<<v.fields_expected<<'\n';
        for(const auto& detail:v.outcomes)out<<"  "<<detail<<'\n';
        out<<"Initialization method: "<<(v.initialization_method.empty()?"not run":v.initialization_method)
            <<"; report buffer ready "<<(v.report_buffer_ready?"yes":"no")
            <<"; native initializer called "<<(v.native_initializer_called?"yes":"no")
            <<"; initializer API checks passed "<<v.initializer_checks_passed<<"; failed "<<v.initializer_checks_failed<<'\n';
        for(const auto& detail:v.initializer_diagnostics)out<<"  Initializer: "<<detail<<'\n';
    }
}
