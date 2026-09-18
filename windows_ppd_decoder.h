#pragma once

static constexpr const char* ppd_format_id = "hidp-kdr-header44-cap104-link16-le-v1";
static constexpr const char* ppd_reference_revision = "852cc68b8e0e4c7eba0815ef992951cd8b75c55b";
static constexpr size_t ppd_max_bytes = 1024 * 1024;
static constexpr size_t ppd_max_fields = 65536;
static constexpr size_t ppd_max_expanded_metadata = 262144;

struct PpdDiagnostic {
    std::string severity;
    std::string code;
    size_t byte_offset = 0;
    std::string detail;
};

struct PpdOracle {
    uint32_t passed = 0;
    uint32_t failed = 0;
    uint32_t skipped = 0;
    uint32_t fields_checked = 0;
    uint32_t fields_expected = 0;
    bool metadata_matches = false;
    bool executed = false;
    bool report_buffer_ready = false;
    bool native_initializer_called = false;
    bool initialization_setter_called = false;
    int32_t initialization_setter_status = 0;
    std::vector<std::string> outcomes;
    int32_t initializer_status = 0;
    uint32_t initializer_checks_passed = 0;
    uint32_t initializer_checks_failed = 0;
    std::string initialization_method;
    std::vector<std::string> initializer_diagnostics;
};

struct PpdInitializerAuditFailure {
    std::string test;
    int report_id = 0;
    uint32_t buffer_length = 0;
    bool declared_for_type = false;
    int32_t expected_status = 0;
    int32_t actual_status = 0;
    uint8_t returned_id = 0;
    bool writes_within_buffer = true;
};

struct PpdInitializerAudit {
    bool executed = false;
    uint32_t buffer_length = 0;
    uint32_t checks_passed = 0;
    uint32_t checks_failed = 0;
    std::vector<PpdInitializerAuditFailure> failures;
};

struct PpdReportState {
    int type = 0;
    int report_id = -1;
    uint32_t windows_bytes = 0;
    uint32_t known_wire_bytes = 0;
    bool length_exact = false;
    bool complete = true;
    bool requires_base_report = true;
    PpdOracle oracle;
    bool shared_layout_conflict = false;
};

struct PpdRecord {
    uint32_t index = 0;
    uint32_t offset = 0;
    int type = 0;
    uint16_t page = 0;
    uint8_t id = 0;
    uint8_t bit = 0;
    uint16_t size = 0;
    uint16_t count = 0;
    uint16_t byte = 0;
    uint16_t bit_count = 0;
    uint32_t main_flags = 0;
    uint16_t next_byte = 0;
    uint16_t link = 0;
    uint16_t link_page = 0;
    uint16_t link_usage = 0;
    uint8_t flags = 0;
    std::array<uint16_t,8> range{};
    std::array<uint32_t,5> raw_union{};
    uint32_t units = 0;
    uint32_t units_exp = 0;
    bool unknown_tokens = false;
};

struct PpdResult {
    bool supported = false;
    size_t length = 0;
    size_t section_end = 0;
    uint16_t usage = 0;
    uint16_t usage_page = 0;
    std::array<uint16_t,3> windows_bytes{};
    std::array<uint16_t,3> occupied_caps{};
    std::array<uint16_t,3> reserved_caps{};
    std::vector<PpdRecord> records;
    std::vector<PpdReportState> report_states;
    std::vector<PpdDiagnostic> diagnostics;
    ParsedDescriptorResult parsed;
};

struct PpdDecodeFailure : std::runtime_error {
    std::string code;
    size_t offset;
    PpdDecodeFailure(const char* c, size_t o, const char* message) : std::runtime_error(message), code(c), offset(o) {}
};

struct PpdBytes {
    const uint8_t* data;
    size_t length;
    void need(size_t offset, size_t count) const {
        if (offset > length || count > length - offset)
            throw PpdDecodeFailure("truncated_section",offset,"Section exceeds independently acquired PPD length");
    }
    uint8_t u8(size_t o) const { need(o,1); return data[o]; }
    uint16_t u16(size_t o) const { need(o,2); return static_cast<uint16_t>(data[o] | (uint16_t(data[o+1]) << 8)); }
    uint32_t u32(size_t o) const {
        need(o,4);
        return uint32_t(data[o]) | (uint32_t(data[o+1]) << 8) | (uint32_t(data[o+2]) << 16) | (uint32_t(data[o+3]) << 24);
    }
};

static int64_t ppd_signed32(uint32_t value) {
    return value & UINT32_C(0x80000000) ? static_cast<int64_t>(value) - INT64_C(4294967296) : value;
}

static const char* ppd_type_name(int type) {
    static const char* names[] = {"Input","Output","Feature"};
    return type >= 0 && type < 3 ? names[type] : "Unknown";
}

static int ppd_type_index(const std::string& type) {
    return type == "Input" ? 0 : type == "Output" ? 1 : type == "Feature" ? 2 : -1;
}

static bool ppd_read_bits(const uint8_t* bytes, size_t length, uint64_t offset, uint32_t width, uint64_t& value) {
    value = 0;
    if (!bytes || !width || width > 64 || length > ppd_max_bytes || offset > uint64_t(length)*8 || width > uint64_t(length)*8-offset) return false;
    for (uint32_t bit = 0; bit < width; ++bit)
        value |= uint64_t((bytes[(offset+bit)/8] >> ((offset+bit)%8)) & 1u) << bit;
    return true;
}

static bool ppd_write_bits(uint8_t* bytes, size_t length, uint64_t offset, uint32_t width, uint64_t value) {
    if (!bytes || !width || width > 64 || length > ppd_max_bytes || offset > uint64_t(length)*8 || width > uint64_t(length)*8-offset ||
        (width < 64 && (value >> width))) return false;
    for (uint32_t bit = 0; bit < width; ++bit) {
        const uint8_t mask = static_cast<uint8_t>(1u << ((offset+bit)%8));
        auto& b = bytes[(offset+bit)/8];
        b = static_cast<uint8_t>((b & ~mask) | (((value >> bit) & 1u) ? mask : 0));
    }
    return true;
}

static bool ppd_selector_usage(const ParsedField& field, int64_t selector, int& page, int& usage) {
    int64_t start = field.logical_min;
    for (const auto& r : field.usage_ranges) {
        if (r.explicit_selector) start = r.selector_min;
        const int64_t end = start + r.maximum - r.minimum;
        if (selector >= start && selector <= end) {
            page = r.usage_page;
            usage = static_cast<int>(r.minimum + selector - start);
            return true;
        }
        start = end + 1;
    }
    return false;
}

static bool ppd_usage_selector(const ParsedField& field, int page, int usage, int64_t& selector) {
    int64_t start = field.logical_min;
    for (const auto& r : field.usage_ranges) {
        if (r.explicit_selector) start = r.selector_min;
        if (r.usage_page == page && usage >= r.minimum && usage <= r.maximum) {
            selector = start + usage - r.minimum;
            return true;
        }
        start += r.maximum - r.minimum + 1;
    }
    for(const auto& r:field.usage_aliases) {
        if(r.usage_page==page&&usage>=r.minimum&&usage<=r.maximum) {
            selector=r.selector_min+usage-r.minimum;
            return true;
        }
    }
    return false;
}

static PpdResult decode_windows_ppd(const uint8_t* data, size_t length) {
    PpdResult result;
    result.length = length;
    try {
        if (!data || length < 44 || length > ppd_max_bytes)
            throw PpdDecodeFailure("invalid_length",0,"PPD length must be between 44 bytes and 1 MiB");
        PpdBytes b{data,length};
        if (std::memcmp(data,"HidP KDR",8))
            throw PpdDecodeFailure("unsupported_signature",0,"Unsupported Windows PPD signature");
        result.usage = b.u16(8);
        result.usage_page = b.u16(10);
        const size_t link_relative = b.u16(40);
        const size_t links = b.u16(42);
        if (link_relative % 104 || !links || links > 4096)
            throw PpdDecodeFailure("unsupported_section_layout",40,"Expected bounded 104-byte capabilities and 16-byte collection nodes");
        const size_t slots = link_relative / 104;
        const size_t link_base = 44 + link_relative;
        b.need(44,link_relative);
        b.need(link_base,links*16);
        result.section_end = link_base + links*16;
        std::vector<uint8_t> ownership(slots,0);
        std::vector<uint16_t> parents(links),children(links),siblings(links),first_child(links);
        for (size_t i = 0; i < links; ++i) {
            const size_t o = link_base+i*16;
            parents[i] = b.u16(o+4); children[i] = b.u16(o+6); siblings[i] = b.u16(o+8); first_child[i] = b.u16(o+10);
            if (parents[i] >= links || children[i] >= links || siblings[i] >= links || first_child[i] >= links ||
                (!i && (parents[i] || siblings[i])) || (i && parents[i] == i))
                throw PpdDecodeFailure("invalid_collection_link",o,"Collection index is outside the bounded tree");
            ParsedCollection c;
            c.ordinal = static_cast<int>(i+1);
            c.parent_ordinal = i ? parents[i]+1 : 0;
            c.top_level_ordinal = 1;
            c.usage = b.u16(o); c.usage_page = b.u16(o+2); c.collection_type = b.u32(o+12)&255;
            result.parsed.collections.push_back(c);
            if (b.u32(o+12)&~UINT32_C(0x1ff))
                result.diagnostics.push_back({"warning","unknown_collection_flags",o+12,"Reserved link-node flags retained without interpretation"});
        }
        if (result.parsed.collections[0].usage != result.usage || result.parsed.collections[0].usage_page != result.usage_page)
            throw PpdDecodeFailure("collection_header_mismatch",link_base,"Header usage does not match the root collection");
        std::vector<uint8_t> reached(links,0);
        reached[0] = 1;
        for (size_t i = 0; i < links; ++i) {
            uint16_t child = first_child[i];
            for (size_t j = 0; j < children[i]; ++j) {
                if (!child || reached[child] || parents[child] != i)
                    throw PpdDecodeFailure("collection_cycle_or_duplicate",link_base+i*16,"Collection child chain cycles, repeats or disagrees with parent");
                reached[child] = 1;
                child = siblings[child];
            }
            if (child) throw PpdDecodeFailure("collection_child_count",link_base+i*16,"Child count does not match sibling chain");
            size_t steps = 0, current = i;
            while (current) {
                if (++steps > links) throw PpdDecodeFailure("collection_parent_cycle",link_base+i*16,"Collection parent chain is cyclic");
                current = parents[current];
            }
        }
        if (std::find(reached.begin(),reached.end(),0) != reached.end())
            throw PpdDecodeFailure("unreachable_collection",link_base,"Collection is not reachable from the root");
        std::map<std::pair<int,int>,std::vector<size_t>> groups;
        bool numbered = false, unnumbered = false;
        for (int type = 0; type < 3; ++type) {
            const size_t o = 16+size_t(type)*8;
            const size_t first = b.u16(o), capacity = b.u16(o+2), last = b.u16(o+4);
            result.windows_bytes[type] = b.u16(o+6);
            if (first > last || first > slots || capacity > slots-first || last-first > capacity)
                throw PpdDecodeFailure("invalid_capability_range",o,"FirstCap/LastCap occupied range exceeds capability capacity");
            result.occupied_caps[type] = static_cast<uint16_t>(last-first);
            result.reserved_caps[type] = static_cast<uint16_t>(capacity-(last-first));
            for (size_t i = first; i < first+capacity; ++i) {
                if (ownership[i]) throw PpdDecodeFailure("overlapping_capability_sections",o,"Input/Output/Feature capability capacities overlap");
                ownership[i] = static_cast<uint8_t>(type+1);
            }
            if (first == last && result.windows_bytes[type]) {
                PpdReportState empty;
                empty.type = type; empty.windows_bytes = result.windows_bytes[type]; empty.complete = false;
                result.report_states.push_back(empty);
                result.diagnostics.push_back({"warning","report_type_without_records",o,"Windows buffer length exists without occupied capability records; report ID and layout remain unknown"});
            }
            for (size_t i = first; i < last; ++i) {
                const size_t c = 44+i*104;
                PpdRecord r;
                r.index = static_cast<uint32_t>(i); r.offset = static_cast<uint32_t>(c); r.type = type;
                r.page=b.u16(c); r.id=b.u8(c+2); r.bit=b.u8(c+3); r.size=b.u16(c+4); r.count=b.u16(c+6);
                r.byte=b.u16(c+8); r.bit_count=b.u16(c+10); r.main_flags=b.u32(c+12);
                r.next_byte=b.u16(c+16); r.link=b.u16(c+18); r.link_page=b.u16(c+20); r.link_usage=b.u16(c+22); r.flags=b.u8(c+24);
                for (size_t j=0;j<8;++j) r.range[j]=b.u16(c+60+j*2);
                for (size_t j=0;j<5;++j) r.raw_union[j]=b.u32(c+76+j*4);
                r.units=b.u32(c+96); r.units_exp=b.u32(c+100);
                for (size_t j=0;j<4;++j) if (b.u8(c+28+j*8)) r.unknown_tokens=true;
                const uint64_t width=uint64_t(r.size)*r.count;
                const uint64_t start=uint64_t(r.byte)*8+r.bit;
                if (!r.size || !r.count || !r.byte || r.bit>7 || width>65535 || width!=r.bit_count ||
                    start+width>uint64_t(result.windows_bytes[type])*8 || r.next_byte<(start+width+7)/8 || r.next_byte>result.windows_bytes[type])
                    throw PpdDecodeFailure("invalid_field_extent",c+3,"Capability position, size, count, BitCount or NextBytePosition contradicts bounded Windows report length");
                if (r.link>=links || (r.link_page != result.parsed.collections[r.link].usage_page) ||
                    (r.link_usage != result.parsed.collections[r.link].usage))
                    throw PpdDecodeFailure("invalid_capability_link",c+18,"Capability link index or usage disagrees with the collection tree");
                if ((r.flags&16) && r.range[0]>r.range[1])
                    throw PpdDecodeFailure("reversed_usage_range",c+60,"Usage minimum exceeds maximum");
                if (r.main_flags&~UINT32_C(0x1ff))
                    throw PpdDecodeFailure("unsupported_main_flags",c+12,"Unsupported HID main-item flags");
                if (((r.flags&8)!=0) != ((r.main_flags&4)==0))
                    throw PpdDecodeFailure("inconsistent_absolute_flag",c+24,"Internal absolute flag disagrees with HID main-item flags");
                numbered |= r.id!=0; unnumbered |= r.id==0;
                groups[{type,r.id}].push_back(result.records.size());
                result.records.push_back(r);
            }
        }
        if (numbered && unnumbered)
            throw PpdDecodeFailure("mixed_report_id_convention",16,"Mixed numbered and unnumbered records cannot establish a common HID prefix convention");
        size_t expanded_metadata=0;
        for (const auto& entry : groups) {
            PpdReportState state;
            state.type=entry.first.first; state.report_id=entry.first.second; state.windows_bytes=result.windows_bytes[state.type];
            std::vector<ParsedField> fields;
            std::map<std::pair<uint32_t,uint32_t>,size_t> spans;
            for (size_t ri : entry.second) {
                const auto& r=result.records[ri];
                ParsedField f;
                f.report_type=ppd_type_name(r.type); f.report_id=r.id; f.usage_page=r.page; f.usage=r.range[0];
                f.windows_api_bit_offset=uint32_t(r.byte)*8+r.bit;
                f.payload_bit_offset=static_cast<uint32_t>(f.windows_api_bit_offset)-8;
                f.wire_bit_offset=f.payload_bit_offset+(r.id?8:0);
                f.bit_size=r.size; f.report_count=r.count;
                f.main_item_flags=r.main_flags; f.main_item_index=r.index;
                f.top_level_collection_ordinal=1; f.collection_ordinal=r.link+1; f.ppd_link_collection=r.link;
                f.ppd_source_records={r.index}; f.ppd_button_cap=(r.flags&4)!=0;
                f.is_constant=(r.main_flags&1)!=0; f.is_variable=(r.main_flags&2)!=0;
                f.is_relative=(r.main_flags&4)!=0; f.has_null_state=(r.main_flags&64)!=0;
                f.unit=r.units; f.unit_exponent=static_cast<int32_t>(ppd_signed32(r.units_exp));
                const size_t li=f.ppd_button_cap?0:1;
                f.logical_min=ppd_signed32(r.raw_union[li]);
                f.logical_max=f.logical_min<0?ppd_signed32(r.raw_union[li+1]):r.raw_union[li+1];
                f.is_signed=f.logical_min<0;
                if (f.ppd_button_cap && f.is_variable && f.bit_size==1) {
                    f.logical_min=0;f.logical_max=1;f.is_signed=false;
                } else if (!f.ppd_button_cap) {
                    f.physical_min=ppd_signed32(r.raw_union[3]);
                    f.physical_max=f.physical_min<0?ppd_signed32(r.raw_union[4]):r.raw_union[4];
                    f.has_null_state |= (r.raw_union[0]&255)!=0;
                }
                if ((r.flags&2) && !f.is_constant) {
                    f.uninterpreted_storage=true;
                    f.interpretation_complete=false;
                    result.diagnostics.push_back({"warning","padding_flag_not_constant",r.offset+24,"Internal padding indication without Constant main flag is preserved as uninterpreted storage"});
                }
                if (r.unknown_tokens) {
                    f.interpretation_complete=false;
                    result.diagnostics.push_back({"warning","unsupported_global_tokens",r.offset+28,"Unknown global tokens retained; affected report cannot become generation-ready"});
                }
                if(r.flags&192) {
                    f.interpretation_complete=false;
                    result.diagnostics.push_back({"warning","unsupported_capability_flags",r.offset+24,"Reserved capability flags are retained without authorizing their interpretation"});
                }
                if (b.u32(link_base+r.link*16+12)&~UINT32_C(255)) {
                    f.interpretation_complete=false;
                    result.diagnostics.push_back({"warning","collection_alias_unresolved",link_base+r.link*16,"Aliased collection semantics require independent validation"});
                }
                UsageRange u;
                u.usage_page=r.page; u.minimum=r.range[0]; u.maximum=(r.flags&16)?r.range[1]:r.range[0];
                if (!f.is_variable && !f.is_constant) {u.explicit_selector=true;u.selector_min=f.logical_min;}
                f.usage_ranges.push_back(u);
                const auto key=std::make_pair(f.wire_bit_offset,uint32_t(r.size)*r.count);
                auto overlap=spans.find(key);
                if (overlap!=spans.end()) {
                    auto& original=fields[overlap->second];
                    bool marked_alias=(r.flags&32)!=0;
                    for (uint32_t index : original.ppd_source_records)
                        for (const auto& previous : result.records) if(previous.index==index && (previous.flags&32)) marked_alias=true;
                    const bool shared_array=!f.is_variable&&!f.is_constant&&(r.flags&1);
                    if (original.bit_size!=f.bit_size||original.report_count!=f.report_count||original.main_item_flags!=f.main_item_flags||
                        original.ppd_link_collection!=f.ppd_link_collection||(!marked_alias&&!shared_array))
                        throw PpdDecodeFailure("conflicting_overlap",r.offset,"Overlapping records are not a supported alias or shared selector-array group");
                    original.ppd_source_records.push_back(r.index);
                    original.interpretation_complete &= f.interpretation_complete;
                    if (marked_alias) {
                        const auto& domain=original.usage_ranges.front();
                        const bool compatible=domain.maximum-domain.minimum==u.maximum-u.minimum &&
                            (original.is_variable || domain.selector_min==u.selector_min);
                        original.usage_aliases.push_back(u);
                        if(!compatible) {
                            original.selector_mapping_resolved=false;original.interpretation_complete=false;
                            result.diagnostics.push_back({"warning","incompatible_alias_domain",r.offset,"Alias usage domains do not provide a supported one-to-one storage equivalence"});
                        }
                    }
                    else {
                        bool conflicting=false;
                        for(const auto& v:original.usage_ranges) {
                            const int64_t a=v.selector_min, z=a+v.maximum-v.minimum;
                            const int64_t c=u.selector_min, d=c+u.maximum-u.minimum;
                            if(a<=d && c<=z) conflicting=true;
                        }
                        original.usage_ranges.push_back(u);
                        if(conflicting) {
                            original.selector_mapping_resolved=false;
                            original.interpretation_complete=false;
                            result.diagnostics.push_back({"warning","ambiguous_shared_array",r.offset,"Shared storage is retained once; overlapping selector domains are not guessed"});
                        }
                    }
                    continue;
                }
                spans[key]=fields.size();
                fields.push_back(f);
            }
            for(auto& f:fields) {
                bool open_alias=false;
                for(uint32_t index:f.ppd_source_records) {
                    const auto r=std::find_if(result.records.begin(),result.records.end(),[&](const PpdRecord& item){return item.index==index;});
                    if(r!=result.records.end())open_alias=(r->flags&32)!=0;
                }
                if(open_alias) {
                    f.interpretation_complete=false;
                    result.diagnostics.push_back({"warning","unterminated_alias_group",0,"An alias record has no terminating non-alias record for its exact storage span"});
                }
            }
            std::sort(fields.begin(),fields.end(),[](const ParsedField& a,const ParsedField& b){return a.wire_bit_offset<b.wire_bit_offset;});
            uint32_t end=state.report_id?8u:0u;
            std::vector<ParsedField> normalized;
            auto unknown=[&](uint32_t start,uint32_t width) {
                if(!width) return;
                ParsedField f;
                f.report_type=ppd_type_name(state.type); f.report_id=static_cast<uint8_t>(state.report_id);
                f.wire_bit_offset=start; f.payload_bit_offset=start-(state.report_id?8:0); f.windows_api_bit_offset=f.payload_bit_offset+8;
                f.bit_size=width;f.report_count=1;f.uninterpreted_storage=true;
                f.collection_ordinal=f.top_level_collection_ordinal=1;
                normalized.push_back(f);
            };
            for(auto& f:fields) {
                if(f.wire_bit_offset<end) throw PpdDecodeFailure("conflicting_partial_overlap",0,"Partially overlapping fields do not describe independent storage");
                unknown(end,f.wire_bit_offset-end);
                const uint64_t field_end=uint64_t(f.wire_bit_offset)+uint64_t(f.bit_size)*f.report_count;
                end=static_cast<uint32_t>(field_end);
                state.complete &= f.interpretation_complete;
                const auto& u=f.usage_ranges.front();
                const uint64_t copies=f.is_variable&&u.maximum>u.minimum?
                    (std::min)(uint64_t(f.report_count),uint64_t(u.maximum-u.minimum)+1):1;
                const uint64_t metadata=copies*(f.usage_ranges.size()+f.usage_aliases.size()+f.ppd_source_records.size());
                if(metadata>ppd_max_expanded_metadata-expanded_metadata)
                    throw PpdDecodeFailure("semantic_expansion_limit",0,"Expanded usage, alias and source-record metadata exceed the bounded decoder budget");
                expanded_metadata+=static_cast<size_t>(metadata);
                if(f.is_variable && u.maximum>u.minimum) {
                    const uint32_t usages=uint32_t(u.maximum-u.minimum)+1;
                    if(usages>f.report_count) {
                        f.interpretation_complete=false;state.complete=false;
                        result.diagnostics.push_back({"warning","variable_usage_count",0,"Variable usage range exceeds stored element count"});
                    }
                    if(result.parsed.fields.size()+normalized.size()+f.report_count>ppd_max_fields)
                        throw PpdDecodeFailure("field_expansion_limit",0,"Expanded fields exceed bounded decoder limit");
                    const uint32_t used=(std::min)(f.report_count,usages);
                    for(uint32_t i=0;i<used;++i) {
                        auto element=f; element.report_count=1;element.element_index=i;
                        element.usage=u.minimum+static_cast<int>((std::min)(i,usages-1));
                        for(auto& alias:element.usage_aliases) {
                            alias.minimum+=static_cast<int>(i);alias.maximum=alias.minimum;
                        }
                        element.wire_bit_offset+=i*f.bit_size;element.payload_bit_offset+=i*f.bit_size;element.windows_api_bit_offset+=i*f.bit_size;
                        normalized.push_back(std::move(element));
                    }
                    if(used<f.report_count)unknown(f.wire_bit_offset+used*f.bit_size,(f.report_count-used)*f.bit_size);
                } else normalized.push_back(f);
            }
            const uint32_t max_wire=state.windows_bytes-(state.report_id?0:1);
            state.known_wire_bytes=(end+7)/8;
            state.length_exact=state.known_wire_bytes==max_wire;
            unknown(end,state.known_wire_bytes*8-end);
            if(result.parsed.fields.size()+normalized.size()>ppd_max_fields)
                throw PpdDecodeFailure("field_expansion_limit",0,"Normalized field expansion is too large");
            auto pr=std::find_if(result.parsed.reports.begin(),result.parsed.reports.end(),[&](const ParsedReport& r){return r.report_id==state.report_id;});
            if(pr==result.parsed.reports.end()) {
                ParsedReport r;r.report_id=static_cast<uint8_t>(state.report_id);r.ppd_reconstructed=true;
                result.parsed.reports.push_back(r);pr=result.parsed.reports.end()-1;
            }
            pr->windows_report_byte_lengths[state.type]=state.windows_bytes;
            pr->wire_length_exact[state.type]=state.length_exact;
            pr->reconstruction_complete[state.type]=state.complete;
            if(state.type==0) {pr->input_wire_bytes=state.known_wire_bytes;pr->input_payload_bits=end-(state.report_id?8:0);}
            if(state.type==1) {pr->output_wire_bytes=state.known_wire_bytes;pr->output_payload_bits=end-(state.report_id?8:0);}
            if(state.type==2) {pr->feature_wire_bytes=state.known_wire_bytes;pr->feature_payload_bits=end-(state.report_id?8:0);}
            pr->fields.insert(pr->fields.end(),normalized.begin(),normalized.end());
            result.parsed.fields.insert(result.parsed.fields.end(),normalized.begin(),normalized.end());
            result.report_states.push_back(state);
        }
        std::sort(result.parsed.reports.begin(),result.parsed.reports.end(),[](const ParsedReport& a,const ParsedReport& b){return a.report_id<b.report_id;});
        result.supported=true;
        result.parsed.available=!result.parsed.reports.empty();
        for(const auto& d:result.diagnostics) if(d.severity=="warning") result.parsed.warnings.push_back(d.code+": "+d.detail);
    } catch(const PpdDecodeFailure& e) {
        result.supported=false;
        result.parsed={};
        result.report_states.clear();
        result.diagnostics.push_back({"error",e.code,e.offset,e.what()});
        result.parsed.errors.push_back(e.code+": "+e.what());
    }
    return result;
}

static PpdResult decode_windows_ppd(const std::vector<uint8_t>& bytes) {
    return decode_windows_ppd(bytes.data(),bytes.size());
}
