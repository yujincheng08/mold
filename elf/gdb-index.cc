// This file contains code to read DWARF debug info to create .gdb_index.
//
// .gdb_index is an optional section to speed up GNU debugger. It contains
// two maps: 1) a map from function/variable/type names to compunits, and
// 2) a map from function address ranges to compunits. gdb uses these
// maps to quickly find a compunit given a name or an instruction pointer.
//
// (Terminology: a compilation unit, often abbreviated as compunit or
// CU, is a unit of debug info. An input .debug_info section usually
// contains one compunit, and thus an output .debug_info contains as
// many compunits as the number of input files.)
//
// .gdb_index is not mandatory. All the information in .gdb_index is
// also in other debug info sections. You can actually create an
// executable without .gdb_index and later add it using the
// `gdb-add-index` post-processing tool that comes with gdb.
//
// Post-relocated debug section contents are needed to create a
// .gdb_index. Therefore, we create it after relocating all the other
// sections. The size of the section is also hard to estimate before
// applying relocations to debug info sections, so a .gdb_index is
// placed at the very end of the output file, even after the section
// header.
//
// The mapping from names to compunits is 1:n while the mapping from
// address ranges to compunits is 1:1. That is, two object files may
// define the same type name (with the same definition), while there
// should be no two functions that overlap with each other in memory.
//
// .gdb_index contains an on-disk hash table for names, so gdb can
// lookup names without loading all strings into memory and construct an
// in-memory hash table.
//
// Names are in .debug_gnu_pubnames and .debug_gnu_pubtypes input
// sections. These sections are created if `-ggnu-pubnames` is given.
// Besides names, these sections contain attributes for each name so
// that gdb can distinguish type names from function names, for example.
//
// A compunit contains one or more function address ranges. If an
// object file is compiled without -ffunction-sections, it contains
// only one .text section and therefore contains a single address range.
// Such range is typically stored directly to the compunit.
//
// If an object file is compiled with -ffunction-sections, it contains
// more than one .text section, and it has as many address ranges as
// the number of .text sections. Such discontiguous address ranges are
// stored to .debug_ranges in DWARF 2/3/4/5 and
// .debug_rnglists/.debug_addr in DWARF 5.
//
// .debug_info section contains DWARF debug info. Although we don't need
// to parse the whole .debug_info section to read address ranges, we
// have to do a little bit. DWARF is complicated and often handled using
// a library such as libdwarf. But we don't use any library because we
// don't want to add an extra run-time dependency just for --gdb-index.
//
// This page explains the format of .gdb_index:
// https://sourceware.org/gdb/onlinedocs/gdb/Index-Section-Format.html

#include "mold.h"
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_sort.h>

namespace mold::elf {

struct SectionHeader {
  ul32 version = 7;
  ul32 cu_list_offset = 0;
  ul32 cu_types_offset = 0;
  ul32 ranges_offset = 0;
  ul32 symtab_offset = 0;
  ul32 const_pool_offset = 0;
};

struct NameType {
  bool operator==(const NameType &) const = default;

  bool operator<(const NameType &other) const {
    return std::tuple(hash, type, name) <
           std::tuple(other.hash, other.type, other.name);
  }

  std::string_view name;
  u32 hash;
  u8 type;
};

struct MapValue {
  std::string_view name;
  u32 hash = 0;
  Atomic<u32> count = 0;
  u32 name_offset = 0;
  u32 type_offset = 0;
};

struct Compunit {
  i64 offset;
  i64 size;
  std::vector<std::pair<u64, u64>> ranges;
  std::vector<NameType> nametypes;
  std::vector<MapValue *> entries;
};

// The hash function for .gdb_index.
static u32 gdb_hash(std::string_view name) {
  u32 h = 0;
  for (u8 c : name) {
    if ('A' <= c && c <= 'Z')
      c = 'a' + c - 'A';
    h = h * 67 + c - 113;
  }
  return h;
}

template <typename E>
u8 * find_cu_abbrev(Context<E> &ctx, u8 **p, i64 dwarf_version) {
  i64 abbrev_offset;

  switch (dwarf_version) {
  case 2:
  case 3:
  case 4:
    abbrev_offset = *(U32<E> *)*p;
    if (i64 address_size = (*p)[4]; address_size != sizeof(Word<E>))
      Fatal(ctx) << "--gdb-index: unsupported address size " << address_size;
    *p += 5;
    break;
  case 5: {
    abbrev_offset = *(U32<E> *)(*p + 2);
    if (i64 address_size = (*p)[1]; address_size != sizeof(Word<E>))
      Fatal(ctx) << "--gdb-index: unsupported address size " << address_size;

    switch (i64 unit_type = (*p)[0]; unit_type) {
    case DW_UT_compile:
    case DW_UT_partial:
      *p += 6;
      break;
    case DW_UT_skeleton:
    case DW_UT_split_compile:
      *p += 14;
      break;
    default:
      Fatal(ctx) << "--gdb-index: unknown DW_UT_* value: 0x"
                 << std::hex << unit_type;
    }
    break;
  }
  default:
    Fatal(ctx) << "--gdb-index: unknown DWARF version: " << dwarf_version;
  }

  i64 abbrev_code = read_uleb(p);

  // Find a .debug_abbrev record corresponding to the .debug_info record.
  // We assume the .debug_info record at a given offset is of
  // DW_TAG_compile_unit which describes a compunit.
  u8 *abbrev = &ctx.debug_abbrev[0] + abbrev_offset;

  for (;;) {
    u32 code = read_uleb(&abbrev);
    if (code == 0)
      Fatal(ctx) << "--gdb-index: .debug_abbrev does not contain"
                 << " a record for the first .debug_info record";

    if (code == abbrev_code) {
      // Found a record
      u64 abbrev_tag = read_uleb(&abbrev);
      if (abbrev_tag != DW_TAG_compile_unit && abbrev_tag != DW_TAG_skeleton_unit)
        Fatal(ctx) << "--gdb-index: the first entry's tag is not"
                   << " DW_TAG_compile_unit/DW_TAG_skeleton_unit but 0x"
                   << std::hex << abbrev_tag;
      break;
    }

    // Skip an uninteresting record
    read_uleb(&abbrev); // tag
    abbrev++; // has_children byte
    for (;;) {
      u64 name = read_uleb(&abbrev);
      u64 form = read_uleb(&abbrev);
      if (name == 0 && form == 0)
        break;
      if (form == DW_FORM_implicit_const)
        read_uleb(&abbrev);
    }
  }

  abbrev++; // skip has_children byte
  return abbrev;
}

// .debug_info contains variable-length fields.
// This function reads one scalar value from a given location.
template <typename E>
u64 read_scalar(Context<E> &ctx, u8 **p, u64 form) {
  switch (form) {
  case DW_FORM_flag_present:
    return 0;
  case DW_FORM_data1:
  case DW_FORM_flag:
  case DW_FORM_strx1:
  case DW_FORM_addrx1:
  case DW_FORM_ref1:
    return *(*p)++;
  case DW_FORM_data2:
  case DW_FORM_strx2:
  case DW_FORM_addrx2:
  case DW_FORM_ref2: {
    u64 val = *(U16<E> *)(*p);
    *p += 2;
    return val;
  }
  case DW_FORM_strx3:
  case DW_FORM_addrx3: {
    u64 val = *(U24<E> *)(*p);
    *p += 3;
    return val;
  }
  case DW_FORM_data4:
  case DW_FORM_strp:
  case DW_FORM_sec_offset:
  case DW_FORM_line_strp:
  case DW_FORM_strx4:
  case DW_FORM_addrx4:
  case DW_FORM_ref4: {
    u64 val = *(U32<E> *)(*p);
    *p += 4;
    return val;
  }
  case DW_FORM_data8:
  case DW_FORM_ref8: {
    u64 val = *(U64<E> *)(*p);
    *p += 8;
    return val;
  }
  case DW_FORM_addr:
  case DW_FORM_ref_addr: {
    u64 val = *(Word<E> *)(*p);
    *p += sizeof(Word<E>);
    return val;
  }
  case DW_FORM_strx:
  case DW_FORM_addrx:
  case DW_FORM_udata:
  case DW_FORM_ref_udata:
  case DW_FORM_loclistx:
  case DW_FORM_rnglistx:
    return read_uleb(p);
  case DW_FORM_string:
    *p += strlen((char *)(*p)) + 1;
    return 0;
  default:
    Fatal(ctx) << "--gdb-index: unhandled debug info form: 0x"
               << std::hex << form;
  }
}

// Read a range list from .debug_ranges starting at the given offset.
template <typename E>
static std::vector<std::pair<u64, u64>>
read_debug_range(Word<E> *range, u64 base) {
  std::vector<std::pair<u64, u64>> vec;

  for (i64 i = 0; range[i] || range[i + 1]; i += 2) {
    if (range[i] + 1 == 0)
      base = range[i + 1];
    else
      vec.emplace_back(range[i] + base, range[i + 1] + base);
  }
  return vec;
}

// Read a range list from .debug_rnglists starting at the given offset.
template <typename E>
static void
read_rnglist_range(std::vector<std::pair<u64, u64>> &vec, u8 *rnglist,
                   Word<E> *addrx, u64 base) {
  for (;;) {
    switch (*rnglist++) {
    case DW_RLE_end_of_list:
      return;
    case DW_RLE_base_addressx:
      base = addrx[read_uleb(&rnglist)];
      break;
    case DW_RLE_startx_endx: {
      u64 val1 = read_uleb(&rnglist);
      u64 val2 = read_uleb(&rnglist);
      vec.emplace_back(addrx[val1], addrx[val2]);
      break;
    }
    case DW_RLE_startx_length: {
      u64 val1 = read_uleb(&rnglist);
      u64 val2 = read_uleb(&rnglist);
      vec.emplace_back(addrx[val1], addrx[val1] + val2);
      break;
    }
    case DW_RLE_offset_pair: {
      u64 val1 = read_uleb(&rnglist);
      u64 val2 = read_uleb(&rnglist);
      vec.emplace_back(base + val1, base + val2);
      break;
    }
    case DW_RLE_base_address:
      base = *(Word<E> *)rnglist;
      rnglist += sizeof(Word<E>);
      break;
    case DW_RLE_start_end: {
      u64 val1 = ((Word<E> *)rnglist)[0];
      u64 val2 = ((Word<E> *)rnglist)[1];
      rnglist += sizeof(Word<E>) * 2;
      vec.emplace_back(val1, val2);
      break;
    }
    case DW_RLE_start_length: {
      u64 val1 = *(Word<E> *)rnglist;
      rnglist += sizeof(Word<E>);
      u64 val2 = read_uleb(&rnglist);
      vec.emplace_back(val1, val1 + val2);
      break;
    }
    }
  }
}

// Returns a list of address ranges explained by a compunit at the
// `offset` in an output .debug_info section.
//
// .debug_info contains DWARF debug info records, so this function
// parses DWARF. If a designated compunit contains multiple ranges, the
// ranges are read from .debug_ranges (or .debug_rnglists for DWARF5).
// Otherwise, a range is read directly from .debug_info (or possibly
// from .debug_addr for DWARF5).
template <typename E>
static std::vector<std::pair<u64, u64>>
read_address_ranges(Context<E> &ctx, i64 offset) {
  // Read .debug_info to find the record at a given offset.
  u8 *p = &ctx.debug_info[0] + offset;

  i64 dwarf_version = *(U16<E> *)(p + 4);
  p += 6;

  u8 *abbrev = find_cu_abbrev(ctx, &p, dwarf_version);

  // Now, read debug info records.
  struct Record {
    u64 form = 0;
    u64 value = 0;
  };

  Record low_pc;
  Record high_pc;
  Record ranges;
  u64 rnglists_base = -1;
  Word<E> *addrx = nullptr;

  // Read all interesting debug records.
  for (;;) {
    u64 name = read_uleb(&abbrev);
    u64 form = read_uleb(&abbrev);
    if (name == 0 && form == 0)
      break;

    u64 val = read_scalar(ctx, &p, form);

    switch (name) {
    case DW_AT_low_pc:
      low_pc = {form, val};
      break;
    case DW_AT_high_pc:
      high_pc = {form, val};
      break;
    case DW_AT_rnglists_base:
      rnglists_base = val;
      break;
    case DW_AT_addr_base:
      addrx = (Word<E> *)(&ctx.debug_addr[0] + val);
      break;
    case DW_AT_ranges:
      ranges = {form, val};
      break;
    }
  }

  // Handle non-contiguous address ranges.
  if (ranges.form) {
    if (dwarf_version <= 4) {
      Word<E> *range_begin = (Word<E> *)(&ctx.debug_ranges[0] + ranges.value);
      return read_debug_range<E>(range_begin, low_pc.value);
    }

    assert(dwarf_version == 5);

    std::vector<std::pair<u64, u64>> vec;

    u8 *buf = &ctx.debug_rnglists[0];
    if (ranges.form == DW_FORM_sec_offset) {
      read_rnglist_range<E>(vec, buf + ranges.value, addrx, low_pc.value);
    } else {
      if (rnglists_base == -1)
        Fatal(ctx) << "--gdb-index: missing DW_AT_rnglists_base";

      u8 *base = buf + rnglists_base;
      i64 num_offsets = *(U32<E> *)(base - 4);
      U32<E> *offsets = (U32<E> *)base;

      for (i64 i = 0; i < num_offsets; i++)
        read_rnglist_range<E>(vec, base + offsets[i], addrx, low_pc.value);
    }
    return vec;
  }

  // Handle a contiguous address range.
  if (low_pc.form && high_pc.form) {
    u64 lo;

    switch (low_pc.form) {
    case DW_FORM_addr:
      lo = low_pc.value;
      break;
    case DW_FORM_addrx:
    case DW_FORM_addrx1:
    case DW_FORM_addrx2:
    case DW_FORM_addrx4:
      lo = addrx[low_pc.value];
      break;
    default:
      Fatal(ctx) << "--gdb-index: unhandled form for DW_AT_low_pc: 0x"
                 << std::hex << high_pc.form;
    }

    switch (high_pc.form) {
    case DW_FORM_addr:
      return {{lo, high_pc.value}};
    case DW_FORM_addrx:
    case DW_FORM_addrx1:
    case DW_FORM_addrx2:
    case DW_FORM_addrx4:
      return {{lo, addrx[high_pc.value]}};
    case DW_FORM_udata:
    case DW_FORM_data1:
    case DW_FORM_data2:
    case DW_FORM_data4:
    case DW_FORM_data8:
      return {{lo, lo + high_pc.value}};
    default:
      Fatal(ctx) << "--gdb-index: unhandled form for DW_AT_high_pc: 0x"
                 << std::hex << high_pc.form;
    }
  }

  return {};
}

// Parses .debug_gnu_pubnames and .debug_gnu_pubtypes. These sections
// start with a 14 bytes header followed by (4-byte offset, 1-byte type,
// null-terminated string) tuples.
//
// The 4-byte offset is an offset into .debug_info that contains details
// about the name. The 1-byte type is a type of the corresponding name
// (e.g. function, variable or datatype). The string is a name of a
// function, a variable or a type.
template <typename E>
static void read_pubnames(Context<E> &ctx, std::vector<Compunit> &cus,
                          ObjectFile<E> &file) {
  auto get_cu = [&](i64 offset) {
    for (i64 i = 0; i < cus.size(); i++)
      if (cus[i].offset == offset)
        return &cus[i];
    Fatal(ctx) << file << ": corrupted debug_info_offset";
  };

  auto read = [&](InputSection<E> &isec) {
    isec.uncompress(ctx);
    std::string_view contents = isec.contents;

    while (!contents.empty()) {
      if (contents.size() < 14)
        Fatal(ctx) << isec << ": corrupted header";

      u32 len = *(U32<E> *)contents.data() + 4;
      u32 debug_info_offset = *(U32<E> *)(contents.data() + 6);
      Compunit *cu = get_cu(file.debug_info->offset + debug_info_offset);

      std::string_view data = contents.substr(14, len - 14);
      contents = contents.substr(len);

      while (!data.empty()) {
        u32 offset = *(U32<E> *)data.data();
        data = data.substr(4);
        if (offset == 0)
          break;

        u8 type = data[0];
        data = data.substr(1);

        std::string_view name = data.data();
        data = data.substr(name.size() + 1);

        cu->nametypes.push_back({name, gdb_hash(name), type});
      }
    }
  };

  if (file.debug_pubnames)
    read(*file.debug_pubnames);
  if (file.debug_pubtypes)
    read(*file.debug_pubtypes);
}

template <typename E>
static std::vector<Compunit> read_compunits(Context<E> &ctx) {
  std::vector<Compunit> cus;

  // Read compunits from the output .debug_info section.
  u8 *begin = &ctx.debug_info[0];
  u8 *end = begin + ctx.debug_info.size();

  for (u8 *p = begin; p < end;) {
    if (*(U32<E> *)p == 0xffff'ffff)
      Fatal(ctx) << "--gdb-index: DWARF64 is not not supported";
    i64 len = *(U32<E> *)p + 4;
    cus.push_back(Compunit{p - begin, len});
    p += len;
  }

  // Read address ranges for each compunit.
  tbb::parallel_for((i64)0, (i64)cus.size(), [&](i64 i) {
    cus[i].ranges = read_address_ranges(ctx, cus[i].offset);

    // Remove empty ranges
    std::erase_if(cus[i].ranges, [](std::pair<u64, u64> p) {
      return p.first == 0 || p.first == p.second;
    });
  });

  // Read symbols from .debug_gnu_pubnames and .debug_gnu_pubtypes.
  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    read_pubnames(ctx, cus, *file);
  });

  // Uniquify elements because GCC 11 seems to emit one record for each
  // comdat group which results in having a lot of duplicate records.
  tbb::parallel_for_each(cus, [&](Compunit &cu) {
    sort(cu.nametypes);
    remove_duplicates(cu.nametypes);
  });

  return cus;
}

template <typename E>
std::span<u8> get_buffer(Context<E> &ctx, Chunk<E> *chunk) {
  if (chunk->is_compressed)
    return chunk->uncompressed_data;
  return {ctx.buf + chunk->shdr.sh_offset, chunk->shdr.sh_size};
}

template <typename E>
void write_gdb_index(Context<E> &ctx) {
  Timer t(ctx, "write_gdb_index");

  // Find debug info sections
  for (Chunk<E> *chunk : ctx.chunks) {
    std::string_view name = chunk->name;
    if (name == ".debug_info")
      ctx.debug_info = get_buffer(ctx, chunk);
    if (name == ".debug_abbrev")
      ctx.debug_abbrev = get_buffer(ctx, chunk);
    if (name == ".debug_ranges")
      ctx.debug_ranges = get_buffer(ctx, chunk);
    if (name == ".debug_addr")
      ctx.debug_addr = get_buffer(ctx, chunk);
    if (name == ".debug_rnglists")
      ctx.debug_rnglists = get_buffer(ctx, chunk);
  }

  if (ctx.debug_info.empty())
    return;

  // Read debug info
  std::vector<Compunit> cus = read_compunits(ctx);

  // Uniquify symbols
  HyperLogLog estimator;

  tbb::parallel_for_each(cus, [&](Compunit &cu) {
    HyperLogLog e;
    for (NameType &nt : cu.nametypes)
      e.insert(nt.hash);
    estimator.merge(e);
  });

  ConcurrentMap<MapValue> map(estimator.get_cardinality() * 3 / 2);

  tbb::parallel_for_each(cus, [&](Compunit &cu) {
    cu.entries.reserve(cu.nametypes.size());

    for (NameType &nt : cu.nametypes) {
      MapValue *ent;
      bool inserted;
      MapValue value = {nt.name, nt.hash};
      std::tie(ent, inserted) = map.insert(nt.name, nt.hash, value);
      ent->count++;
      cu.entries.push_back(ent);
    }
  });

  // Sort symbols for build reproducibility
  std::vector<MapValue *> entries;
  entries.reserve(estimator.get_cardinality());

  for (i64 i = 0; i < map.nbuckets; i++)
    if (map.entries[i].key)
      entries.push_back(&map.entries[i].value);

  tbb::parallel_sort(entries, [](MapValue *a, MapValue *b) {
    return std::tuple(a->hash, a->name) < std::tuple(b->hash, b->name);
  });

  // Compute sizes of each components
  SectionHeader hdr;
  hdr.cu_list_offset = sizeof(hdr);
  hdr.cu_types_offset = hdr.cu_list_offset + cus.size() * 16;
  hdr.ranges_offset = hdr.cu_types_offset;

  hdr.symtab_offset = hdr.ranges_offset;
  for (Compunit &cu : cus)
    hdr.symtab_offset += cu.ranges.size() * 20;

  i64 ht_size = bit_ceil(estimator.get_cardinality() * 5 / 4);
  hdr.const_pool_offset = hdr.symtab_offset + ht_size * 8;

  i64 offset = 0;
  for (MapValue *ent : entries) {
    ent->type_offset = offset;
    offset += ent->count * 4 + 4;
  }

  for (MapValue *ent : entries) {
    ent->name_offset = offset;
    offset += ent->name.size() + 1;
  }

  i64 bufsize = hdr.const_pool_offset + offset;

  // Allocate an output buffer
  ctx.output_file->buf2.resize(bufsize);
  u8 *buf = ctx.output_file->buf2.data();

  // Write a section header
  memcpy(buf, &hdr, sizeof(hdr));

  // Write a CU list
  u8 *p = buf + sizeof(hdr);

  for (Compunit &cu : cus) {
    *(ul64 *)p = cu.offset;
    *(ul64 *)(p + 8) = cu.size;
    p += 16;
  }

  // Write address areas
  for (i64 i = 0; i < cus.size(); i++) {
    for (std::pair<u64, u64> range : cus[i].ranges) {
      *(ul64 *)p = range.first;
      *(ul64 *)(p + 8) = range.second;
      *(ul32 *)(p + 16) = i;
      p += 20;
    }
  }

  // Write a symbol table
  u32 mask = ht_size - 1;
  ul32 *ht = (ul32 *)(buf + hdr.symtab_offset);

  for (MapValue *ent : entries) {
    u32 hash = ent->hash;
    u32 step = (hash & mask) | 1;
    u32 j = hash & mask;

    while (ht[j * 2] || ht[j * 2 + 1])
      j = (j + step) & mask;

    ht[j * 2] = ent->name_offset;
    ht[j * 2 + 1] = ent->type_offset;
  }

  // Write types
  for (i64 i = 0; i < cus.size(); i++) {
    Compunit &cu = cus[i];
    u8 *base = buf + hdr.const_pool_offset;

    for (i64 j = 0; j < cu.nametypes.size(); j++) {
      ul32 *p = (ul32 *)(base + cu.entries[j]->type_offset);
      i64 idx = ++p[0];
      p[idx] = (cu.nametypes[j].type << 24) | i;
    }
  }

  // Write names
  tbb::parallel_for_each(entries, [&](MapValue *ent) {
    write_string(buf + hdr.const_pool_offset + ent->name_offset, ent->name);
  });

  // Update the section size and rewrite the section header
  if (ctx.shdr) {
    ctx.gdb_index->shdr.sh_size = bufsize;
    ctx.shdr->copy_buf(ctx);
  }
}

using E = MOLD_TARGET;

template void write_gdb_index(Context<E> &);

} // namespace mold::elf
