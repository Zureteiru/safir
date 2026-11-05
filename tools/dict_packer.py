#!/usr/bin/env python3
"""
dict_packer.py - Compact Yomitan Dictionary Compiler for safir

Compiles Yomitan .zip dictionaries and test JSON fixtures into a compact,
disk-backed binary dictionary (`safir_dict.bin`) with 16 KB compressed glossary
blocks and sorted index tables suitable for sub-millisecond lookups under
strict RAM budgets (POOL_DICT_INDEX <= 100 KB on Switch).
"""

import sys
import os
import json
import zipfile
import zlib
import struct
import argparse
from typing import List, Dict, Tuple, Any

# Bitmask constants matching include/safir_dict.h
POS_OTHER        = 1 << 0
POS_NOUN         = 1 << 1
POS_VERB_ICHIDAN = 1 << 2  # v1
POS_VERB_GODAN   = 1 << 3  # v5
POS_VERB_SURU    = 1 << 4  # vs
POS_VERB_KURU    = 1 << 5  # vk
POS_ADJ_I        = 1 << 6  # adj-i
POS_ADJ_NA       = 1 << 7  # adj-na
POS_ADVERB       = 1 << 8  # adv
POS_PREFIX       = 1 << 9
POS_SUFFIX       = 1 << 10
POS_PARTICLE     = 1 << 11

POS_MAP = {
    "v1": POS_VERB_ICHIDAN,
    "ichidan": POS_VERB_ICHIDAN,
    "v5": POS_VERB_GODAN,
    "godan": POS_VERB_GODAN,
    "v5k": POS_VERB_GODAN,
    "v5s": POS_VERB_GODAN,
    "v5t": POS_VERB_GODAN,
    "v5n": POS_VERB_GODAN,
    "v5m": POS_VERB_GODAN,
    "v5r": POS_VERB_GODAN,
    "v5w": POS_VERB_GODAN,
    "v5g": POS_VERB_GODAN,
    "v5b": POS_VERB_GODAN,
    "vs": POS_VERB_SURU,
    "vs-i": POS_VERB_SURU,
    "vs-s": POS_VERB_SURU,
    "suru": POS_VERB_SURU,
    "vk": POS_VERB_KURU,
    "kuru": POS_VERB_KURU,
    "adj-i": POS_ADJ_I,
    "i-adj": POS_ADJ_I,
    "adj-na": POS_ADJ_NA,
    "na-adj": POS_ADJ_NA,
    "n": POS_NOUN,
    "noun": POS_NOUN,
    "adv": POS_ADVERB,
    "adverb": POS_ADVERB,
    "prt": POS_PARTICLE,
    "particle": POS_PARTICLE,
    "pref": POS_PREFIX,
    "prefix": POS_PREFIX,
    "suf": POS_SUFFIX,
    "suffix": POS_SUFFIX,
}

# Header flag: entries are ranked (score + JMdict priority, reading keys +32768). The
# overlay prefers a pack with this flag, so an older unranked dict.bin left on the SD
# card can never shadow a new one again.
DICT_FLAG_RANKED = 1

BLOCK_SIZE = 16384  # 16 KB uncompressed glossary block

def parse_pos_tags(tag_str: str) -> int:
    mask = 0
    if not tag_str:
        return POS_OTHER
    tokens = tag_str.replace(";", " ").replace(",", " ").split()
    for tok in tokens:
        tok_clean = tok.strip().lower()
        if tok_clean in POS_MAP:
            mask |= POS_MAP[tok_clean]
        elif tok_clean.startswith("v5"):
            mask |= POS_VERB_GODAN        # v5k-s, v5r-i, v5aru, v5u-s ...
        elif tok_clean.startswith("v1"):
            mask |= POS_VERB_ICHIDAN      # v1-s (くれる)
        elif tok_clean.startswith("vs") or tok_clean == "vz":
            mask |= POS_VERB_SURU         # vs-c; zuru verbs conjugate like suru
        elif tok_clean.startswith("vk"):
            mask |= POS_VERB_KURU
        elif tok_clean in ("adj-ix",):
            mask |= POS_ADJ_I             # いい / よい
    return mask if mask != 0 else POS_OTHER

def compress_block(data: bytes, comp_type: int) -> bytes:
    if comp_type == 0:
        return data
    # Raw DEFLATE with wbits=-15 (RFC 1951)
    compressor = zlib.compressobj(level=9, wbits=-15)
    return compressor.compress(data) + compressor.flush()

class TermEntry:
    def __init__(self, term: str, reading: str, glossary: str, pos_mask: int = POS_OTHER,
                 pitch_mask: int = 0, freq_rank: int = 0):
        self.term = term.strip()
        self.reading = reading.strip() if reading else term.strip()
        self.glossary = glossary.strip()
        self.pos_mask = pos_mask
        self.pitch_mask = pitch_mask
        self.freq_rank = min(freq_rank, 65535)

# Yomitan structured-content -> plain text. The Switch overlay draws plain UTF-8,
# so tags/attributes must never leak into the glossary (audit: the old code stored
# Python repr() strings like "{'content': ..., 'tag': 'li'}").
_SC_SKIP_TAGS = {"rt", "rp", "img", "br"}
_SC_BLOCK_TAGS = {"li", "div", "ol", "ul", "table", "tr", "details", "summary"}


def _sc_text(node, out: List[str]) -> None:
    if node is None:
        return
    if isinstance(node, str):
        out.append(node)
        return
    if isinstance(node, (int, float)):
        out.append(str(node))
        return
    if isinstance(node, list):
        for child in node:
            _sc_text(child, out)
        return
    if isinstance(node, dict):
        ntype = node.get("type")
        if ntype == "image":
            return
        if ntype == "text":
            out.append(str(node.get("text", "")))
            return
        if ntype == "structured-content":
            _sc_text(node.get("content"), out)
            return
        tag = node.get("tag")
        if tag in _SC_SKIP_TAGS:
            return
        if tag in _SC_BLOCK_TAGS:
            out.append("\x1f")
        _sc_text(node.get("content"), out)
        if tag in _SC_BLOCK_TAGS:
            out.append("\x1f")


def flatten_glossary(defs) -> str:
    """Turn a Yomitan term-bank definitions field into one readable line."""
    items = defs if isinstance(defs, list) else [defs]
    parts: List[str] = []
    for d in items:
        buf: List[str] = []
        _sc_text(d, buf)
        for piece in "".join(buf).split("\x1f"):
            piece = " ".join(piece.split())
            if piece and piece not in parts:
                parts.append(piece)
    return "; ".join(parts)


def jmdict_priority(term_tags: str) -> int:
    """Smaller = more common, from JMdict priority tags (news1k ... news48k, ichi, spec, gai)."""
    best = 99
    for t in term_tags.split():
        if t.startswith("news") and t.endswith("k") and t[4:-1].isdigit():
            best = min(best, int(t[4:-1]))      # news1k = top 1,000 words of the newspaper corpus
        elif t in ("ichi", "spec"):
            best = min(best, 12)                 # in a common-word list, about news12k
        elif t == "gai":
            best = min(best, 20)
    return best


def load_yomitan_zip(zip_path: str) -> List[TermEntry]:
    entries: List[TermEntry] = []
    pitches: Dict[Tuple[str, str], int] = {}
    freqs: Dict[Tuple[str, str], int] = {}

    with zipfile.ZipFile(zip_path, 'r') as z:
        namelist = z.namelist()

        # Check metadata banks
        for name in namelist:
            if name.startswith("term_meta_bank_") and name.endswith(".json"):
                try:
                    data = json.loads(z.read(name).decode('utf-8'))
                    for item in data:
                        # Format: [term, mode, value]
                        if len(item) >= 3:
                            term, mode, val = item[0], item[1], item[2]
                            if mode == "pitch":
                                reading = term
                                pos_num = 0
                                if isinstance(val, dict):
                                    reading = val.get("reading", term)
                                    plist = val.get("pitches", [])
                                    if plist and isinstance(plist[0], dict):
                                        pos_num = plist[0].get("position", 0)
                                elif isinstance(val, int):
                                    pos_num = val
                                mask = 1 << min(pos_num, 15)
                                pitches[(term, reading)] = mask
                                pitches[(term, "")] = mask
                            elif mode == "freq":
                                reading = term
                                rank = 0
                                if isinstance(val, dict):
                                    reading = val.get("reading", term)
                                    rank = val.get("frequency", val.get("ranking", 0))
                                elif isinstance(val, (int, float)):
                                    rank = int(val)
                                freqs[(term, reading)] = rank
                                freqs[(term, "")] = rank
                except Exception as e:
                    print(f"Warning parsing meta bank {name}: {e}", file=sys.stderr)

        # Parse term banks. Some exports (this JMdict one) store every SENSE as its own
        # row: [term, reading, "1 v5r vt", ...], [term, reading, "2 v5r vt", ...]. Rows of one
        # word share (term, reading, sequence), so merge them into one numbered glossary.
        # Rows tagged "forms" are inflection/spelling tables, not definitions: skipped.
        groups: Dict[Tuple[str, str, int], dict] = {}
        order: List[Tuple[str, str, int]] = []
        for name in sorted(namelist):
            if name.startswith("term_bank_") and name.endswith(".json"):
                try:
                    bank_data = json.loads(z.read(name).decode('utf-8'))
                    for item in bank_data:
                        # Yomitan item: [term, reading, def_tags, rules, score, definitions, seq, term_tags]
                        if len(item) < 6:
                            continue
                        term = str(item[0])
                        reading = str(item[1]) if item[1] else term
                        def_tags = str(item[2]) if item[2] else ""
                        rules = str(item[3]) if item[3] else ""
                        score = int(item[4]) if isinstance(item[4], (int, float)) else 0
                        seq = int(item[6]) if len(item) > 6 and isinstance(item[6], (int, float)) else -1
                        tag_list = def_tags.split()
                        if "forms" in tag_list:
                            continue
                        sense_no = int(tag_list[0]) if tag_list and tag_list[0].isdigit() else 0
                        glossary = flatten_glossary(item[5])
                        if not glossary:
                            continue
                        key = (term, reading, seq if seq >= 0 else len(order))
                        g = groups.get(key)
                        if g is None:
                            g = {"senses": [], "pos": 0, "score": score}
                            groups[key] = g
                            order.append(key)
                        g["senses"].append((sense_no, glossary))
                        g["pos"] |= parse_pos_tags(def_tags + " " + rules)
                        g["score"] = max(g["score"], score)
                        g["prio"] = min(g.get("prio", 99), jmdict_priority(str(item[7]) if len(item) > 7 else ""))
                except Exception as e:
                    print(f"Warning parsing term bank {name}: {e}", file=sys.stderr)

        # Rank: a frequency bank wins when present; otherwise the dictionary's own score
        # (higher = more important) becomes a rank, 1 = best. Ranks stay below 32768 so the
        # index can mark reading-only keys (see pack_dictionary).
        # Ties in score are common (every "common" word scores the same), so JMdict's own
        # priority tags break them: 私 (ichi news1k) before 渡し (news11k) for わたし.
        by_score = sorted(order, key=lambda k: (-groups[k]["score"], groups[k].get("prio", 99)))
        score_rank = {k: min(1 + i // 8, 32767) for i, k in enumerate(by_score)}
        for key in order:
            term, reading, _ = key
            g = groups[key]
            senses = sorted(g["senses"], key=lambda t: t[0])
            if len(senses) == 1:
                glossary = senses[0][1]
            else:
                glossary = " ".join(f"{i + 1}) {gl}" for i, (_, gl) in enumerate(senses))
            pitch_mask = pitches.get((term, reading), pitches.get((term, ""), 0))
            fr = freqs.get((term, reading), freqs.get((term, ""), 0))
            freq_rank = min(int(fr), 32767) if fr else score_rank[key]
            entries.append(TermEntry(term, reading, glossary, g["pos"], pitch_mask, freq_rank))

    return entries

def load_json_fixture(json_path: str) -> List[TermEntry]:
    entries: List[TermEntry] = []
    with open(json_path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    if isinstance(data, dict) and "entries" in data:
        data = data["entries"]

    for item in data:
        if isinstance(item, dict):
            term = item.get("term", item.get("headword", ""))
            reading = item.get("reading", term)
            glossary = item.get("glossary", item.get("definition", ""))
            pos = item.get("pos", item.get("pos_mask", "other"))
            if isinstance(pos, int):
                pos_mask = pos
            else:
                pos_mask = parse_pos_tags(str(pos))
            pitch = item.get("pitch", item.get("pitch_mask", 0))
            if isinstance(pitch, int) and pitch < 16 and (item.get("pitch_position") is not None or "pitch" in item):
                pitch_mask = (1 << pitch) if pitch > 0 else 1
            else:
                pitch_mask = int(pitch)
            freq = item.get("freq", item.get("freq_rank", 0))
            entries.append(TermEntry(term, reading, glossary, pos_mask, pitch_mask, freq))
        elif isinstance(item, list) and len(item) >= 6:
            # Yomitan array format
            term = str(item[0])
            reading = str(item[1]) if item[1] else term
            tags = (str(item[2]) or "") + " " + (str(item[3]) or "")
            pos_mask = parse_pos_tags(tags)
            defs = item[5]
            glossary = "; ".join(defs) if isinstance(defs, list) else str(defs)
            score = int(item[4]) if isinstance(item[4], (int, float)) else 0
            entries.append(TermEntry(term, reading, glossary, pos_mask, 0, max(0, -score)))

    return entries

def pack_dictionary(entries: List[TermEntry], output_path: str, compression_type: int = 1):
    print(f"[PACKER] Compiling {len(entries)} dictionary entries into '{output_path}'...")

    # Step 1: Pack glossary blocks (16 KB chunks)
    # Each entry in chunk: headword\0reading\0glossary\0
    blocks: List[bytes] = []
    block_metas: List[Tuple[int, int, int]] = [] # (file_offset, comp_size, uncomp_size)

    current_chunk = bytearray()
    entry_locations: List[Tuple[int, int, int]] = [] # (block_id, offset_in_block, gloss_len)

    for e in entries:
        hw_bytes = e.term.encode('utf-8') + b'\0'
        rd_bytes = e.reading.encode('utf-8') + b'\0'
        gl_bytes = e.glossary.encode('utf-8') + b'\0'
        record = hw_bytes + rd_bytes + gl_bytes

        if len(current_chunk) + len(record) > BLOCK_SIZE and len(current_chunk) > 0:
            # Flush current chunk
            comp = compress_block(bytes(current_chunk), compression_type)
            blocks.append(comp)
            block_metas.append((0, len(comp), len(current_chunk))) # file_offset filled later
            current_chunk = bytearray()

        offset_in_block = len(current_chunk)
        current_chunk.extend(record)
        entry_locations.append((len(blocks), offset_in_block, len(gl_bytes) - 1))

    if len(current_chunk) > 0:
        comp = compress_block(bytes(current_chunk), compression_type)
        blocks.append(comp)
        block_metas.append((0, len(comp), len(current_chunk)))

    print(f"  Generated {len(blocks)} glossary block(s) (16 KB target size, comp_type={compression_type}).")

    # Step 2: Build search keys (both Headword and Kana Reading if distinct)
    IndexItem = Tuple[bytes, int, int, int, int, int, int] # (key_bytes, block_id, offset_in_block, gloss_len, pos_mask, pitch_mask, freq_rank)
    index_items: List[IndexItem] = []

    for i, e in enumerate(entries):
        block_id, offset, gloss_len = entry_locations[i]
        key_term = e.term.encode('utf-8')
        index_items.append((key_term, block_id, offset, gloss_len, e.pos_mask, e.pitch_mask, e.freq_rank))

        if e.reading and e.reading != e.term:
            key_reading = e.reading.encode('utf-8')
            # A key that only matches the READING (擦る under する) ranks behind every entry
            # whose written form is the key (する "to do"), as Yomitan prefers for kana text.
            reading_rank = min(e.freq_rank + 32768, 65535) if e.freq_rank else 0
            index_items.append((key_reading, block_id, offset, gloss_len, e.pos_mask, e.pitch_mask, reading_rank))

    # Sort index items lexicographically by key_bytes (matches C strcmp)
    index_items.sort(key=lambda item: item[0])

    # Step 3: Build Two-Level Sparse Index (Dynamic stride so L1 anchors <= 80 KB)
    max_anchors = 1400
    L1_PAGE_ENTRIES = max(64, (len(index_items) + max_anchors - 1) // max_anchors)
    pages: List[Tuple[bytes, bytearray]] = [] # (headword_44, page_payload)
    l1_meta_list: List[Tuple[bytes, int, int]] = [] # (hw44, entry_count, global_idx)

    for p_start in range(0, len(index_items), L1_PAGE_ENTRIES):
        page_items = index_items[p_start : p_start + L1_PAGE_ENTRIES]
        first_term_bytes = page_items[0][0]
        # Null-padded 44-byte anchor headword
        hw_slice = first_term_bytes[:43]
        hw44 = hw_slice + b'\0' * (44 - len(hw_slice))

        page_str_table = bytearray()
        page_str_offsets: Dict[bytes, int] = {}
        page_entries = bytearray()

        for item in page_items:
            key_bytes, block_id, offset, gloss_len, pos_mask, pitch_mask, freq_rank = item
            if key_bytes not in page_str_offsets:
                off = len(page_str_table)
                page_str_table.extend(key_bytes + b'\0')
                page_str_offsets[key_bytes] = off
            else:
                off = page_str_offsets[key_bytes]

            packed_e = struct.pack(
                "<IHHHHHH",
                off,
                block_id,
                offset,
                gloss_len,
                pos_mask,
                pitch_mask,
                freq_rank
            )
            page_entries.extend(packed_e)

        # SafirL2PageHeader: <HHI (entry_count, string_bytes, reserved) = 8 bytes
        page_hdr = struct.pack("<HHI", len(page_items), len(page_str_table), 0)
        page_payload = page_hdr + page_entries + page_str_table
        # Must match SAFIR_L2_PAGE_SIZE in include/safir_dict.h: the reader rejects larger pages.
        MAX_L2_PAGE_BYTES = 32768
        if len(page_payload) > MAX_L2_PAGE_BYTES:
            raise SystemExit(
                f"dict_packer: L2 page starting at entry {p_start} is {len(page_payload)} B, "
                f"over the reader's {MAX_L2_PAGE_BYTES} B limit. Raise max_anchors "
                f"(currently {max_anchors}) so pages get smaller.")
        pages.append((hw44, page_payload))
        l1_meta_list.append((hw44, len(page_items), p_start))

    # Step 4: Calculate file layout offsets
    HEADER_SIZE = 64
    l1_anchors_offset = HEADER_SIZE
    L1_ANCHOR_SIZE = 56
    l1_anchors_bytes = len(pages) * L1_ANCHOR_SIZE

    l2_pages_offset = l1_anchors_offset + l1_anchors_bytes
    l2_pages_bytes = sum(len(p[1]) for p in pages)

    blocks_offset = l2_pages_offset + l2_pages_bytes
    block_metas_bytes = len(blocks) * 8
    block_data_start = blocks_offset + block_metas_bytes

    # Build Level-1 Anchors binary with exact page file offsets
    packed_l1_anchors = bytearray()
    cur_page_file_off = l2_pages_offset
    for i, (hw44, payload) in enumerate(pages):
        _, entry_cnt, g_idx = l1_meta_list[i]
        # SafirL1Anchor: <44sIHHI
        anchor_bytes = struct.pack(
            "<44sIHHI",
            hw44,
            cur_page_file_off,
            len(payload),
            entry_cnt,
            g_idx
        )
        packed_l1_anchors.extend(anchor_bytes)
        cur_page_file_off += len(payload)

    # Update block offsets
    cur_block_offset = block_data_start
    final_block_metas = bytearray()
    for i, comp in enumerate(blocks):
        _, comp_len, uncomp_len = block_metas[i]
        meta_bytes = struct.pack("<IHH", cur_block_offset, comp_len, uncomp_len)
        final_block_metas.extend(meta_bytes)
        cur_block_offset += comp_len

    total_blocks_bytes = block_metas_bytes + sum(len(b) for b in blocks)

    # Step 5: Write Header (Version 2)
    header = struct.pack(
        "<4sIIHH10I2I",
        b"VNPD",
        2,                     # version 2 (Two-Level Sparse Index)
        HEADER_SIZE,           # header_size
        compression_type,      # compression_type
        DICT_FLAG_RANKED,      # flags: ranked pack (reader prefers it over older files)
        len(index_items),      # entry_count
        len(blocks),           # block_count
        BLOCK_SIZE,            # block_size
        len(pages),            # l1_anchor_count
        l1_anchors_offset,     # l1_anchors_offset
        l1_anchors_bytes,      # l1_anchors_bytes
        l2_pages_offset,       # l2_pages_offset
        l2_pages_bytes,        # l2_pages_bytes
        blocks_offset,         # blocks_offset
        total_blocks_bytes,    # blocks_bytes
        0, 0                   # reserved[2]
    )
    assert len(header) == HEADER_SIZE, f"Header size mismatch: {len(header)}"

    # Step 6: Output binary file
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(header)
        f.write(packed_l1_anchors)
        for _, payload in pages:
            f.write(payload)
        f.write(final_block_metas)
        for b in blocks:
            f.write(b)

    file_size = os.path.getsize(output_path)
    print(f"[SUCCESS] Dictionary successfully compiled!")
    print(f"  - Total entries indexed: {len(index_items):,}")
    print(f"  - Level-1 page anchors:  {len(pages):,} (page size: {L1_PAGE_ENTRIES} entries)")
    print(f"  - Total glossary blocks: {len(blocks):,}")
    print(f"  - Output binary size:   {file_size:,} bytes ({file_size / 1024:.1f} KB)")
    print(f"  - In-RAM L1 Anchors:    {l1_anchors_bytes + block_metas_bytes:,} bytes needed in POOL_DICT_INDEX (Switch budget: 100,000 B; anchors are required, the block table spills to SD if it does not fit)")
    print(f"  - Largest L2 page:      {max(len(p[1]) for p in pages):,} bytes (reader limit 32,768)")

def main():
    parser = argparse.ArgumentParser(description="Compile Yomitan dictionaries into safir binary format.")
    parser.add_argument("inputs", nargs="+", help="Input Yomitan .zip dictionaries or test JSON fixture files")
    parser.add_argument("-o", "--output", default="safir_dict.bin", help="Output binary file path (default: safir_dict.bin)")
    parser.add_argument("--uncompressed", action="store_true", help="Store glossary blocks uncompressed")

    args = parser.parse_args()

    all_entries: List[TermEntry] = []
    for inp in args.inputs:
        if inp.endswith(".zip"):
            print(f"Loading Yomitan zip dictionary: {inp}")
            all_entries.extend(load_yomitan_zip(inp))
        elif inp.endswith(".json"):
            print(f"Loading JSON fixture dictionary: {inp}")
            all_entries.extend(load_json_fixture(inp))
        else:
            print(f"Unknown input file format: {inp}", file=sys.stderr)
            sys.exit(1)

    if not all_entries:
        print("ERROR: No dictionary entries found in input files!", file=sys.stderr)
        sys.exit(1)

    comp_type = 0 if args.uncompressed else 1
    pack_dictionary(all_entries, args.output, comp_type)

if __name__ == "__main__":
    main()
