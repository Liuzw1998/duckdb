#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/storage/compression/dict_fsst/decompression.hpp"
#include "fsst.h"
#include "duckdb/common/fsst.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

#include <algorithm>

namespace duckdb {
namespace dict_fsst {

static constexpr idx_t DICTIONARY_INDEX_WINDOW_SIZE = 2048;
static constexpr idx_t SMALL_DICTIONARY_SIZE = 64;

static void ThrowInvalidDictionarySize() {
	throw IOException("Failed to scan DICT_FSST string segment: dictionary size was out of range. Database file "
	                  "appears to be corrupted.");
}

CompressedStringScanState::~CompressedStringScanState() {
	delete reinterpret_cast<duckdb_fsst_decoder_t *>(decoder);
}

string_t CompressedStringScanState::FetchStringFromDict(Vector &result, uint32_t dict_offset, idx_t dict_idx) {
	D_ASSERT(dict_offset <= NumericCast<uint32_t>(segment.GetBlockSize()));

	if (dict_idx == 0) {
		return string_t(nullptr, 0);
	}
	uint32_t string_len = string_lengths[dict_idx];

	// normal string: read string from this block
	auto dict_pos = dict_ptr + dict_offset;

	auto str_ptr = char_ptr_cast(dict_pos);
	switch (mode) {
	case DictFSSTMode::FSST_ONLY:
	case DictFSSTMode::DICT_FSST: {
		if (string_len == 0) {
			return string_t(nullptr, 0);
		}
		if (all_values_inlined) {
			return FSSTPrimitives::DecompressInlinedValue(decoder, str_ptr, string_len);
		} else {
			return FSSTPrimitives::DecompressValue(decoder, StringVector::GetStringAllocator(result), str_ptr,
			                                       string_len);
		}
	}
	default:
		return StringVector::AddString(result, str_ptr, string_len);
	}
}

void CompressedStringScanState::Initialize(bool initialize_dictionary) {
	baseptr = handle->GetDataMutable() + segment.GetBlockOffset();
	const auto &stats = segment.GetStats();
	if (stats.GetStatsType() == StatisticsType::STRING_STATS && StringStats::HasMaxStringLength(stats)) {
		all_values_inlined = StringStats::MaxStringLength(stats) <= string_t::INLINE_LENGTH;
	}

	// Load header values
	auto header_ptr = reinterpret_cast<dict_fsst_compression_header_t *>(baseptr);
	mode = header_ptr->mode;
	if (mode >= DictFSSTMode::COUNT) {
		throw FatalException("This block was written with a mode that is not recognized by this version, highest "
		                     "available mode %d, found mode: %d",
		                     static_cast<uint8_t>(DictFSSTMode::COUNT), static_cast<uint8_t>(mode));
	}

	dict_count = header_ptr->dict_count;
	auto symbol_table_size = header_ptr->symbol_table_size;
	dictionary_size = header_ptr->dict_size;

	dictionary_indices_width =
	    (bitpacking_width_t)(Load<uint8_t>(data_ptr_cast(&header_ptr->dictionary_indices_width)));
	string_lengths_width = (bitpacking_width_t)(Load<uint8_t>(data_ptr_cast(&header_ptr->string_lengths_width)));

	auto string_lengths_space = BitpackingPrimitives::GetRequiredSize(dict_count, string_lengths_width);
	auto dictionary_indices_space =
	    BitpackingPrimitives::GetRequiredSize(segment.count.load(), dictionary_indices_width);

	auto dictionary_dest = AlignValue<idx_t>(DictFSSTCompression::DICTIONARY_HEADER_SIZE);
	auto symbol_table_dest = AlignValue<idx_t>(dictionary_dest + dictionary_size);
	auto string_lengths_dest = AlignValue<idx_t>(symbol_table_dest + symbol_table_size);
	auto dictionary_indices_dest = AlignValue<idx_t>(string_lengths_dest + string_lengths_space);

	const auto total_space = segment.GetBlockOffset() + dictionary_indices_dest + dictionary_indices_space;
	if (total_space > segment.GetBlockSize()) {
		throw IOException(
		    "Failed to scan dictionary string - index was out of range. Database file appears to be corrupted.");
	}
	dict_ptr = data_ptr_cast(baseptr + dictionary_dest);
	dictionary_indices_ptr = data_ptr_cast(baseptr + dictionary_indices_dest);
	string_lengths_ptr = data_ptr_cast(baseptr + string_lengths_dest);

	switch (mode) {
	case DictFSSTMode::FSST_ONLY:
	case DictFSSTMode::DICT_FSST: {
		decoder = new duckdb_fsst_decoder_t;
		// in FSST_ONLY / DICT_FSST modes a symbol table is always present, so any failure (out-of-bounds or a
		// version mismatch) means the segment is corrupted
		auto ret = duckdb_fsst_import(reinterpret_cast<duckdb_fsst_decoder_t *>(decoder), baseptr + symbol_table_dest,
		                              symbol_table_size);
		if (ret == DUCKDB_FSST_IMPORT_VERSION_MISMATCH || ret == DUCKDB_FSST_IMPORT_OUT_OF_BOUNDS) {
			throw IOException("Failed to scan DICT_FSST string segment: invalid FSST symbol table. Database file "
			                  "appears to be corrupted.");
		}
		break;
	}
	default:
		break;
	}

	string_lengths.resize(AlignValue<uint32_t, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE>(dict_count));
	BitpackingPrimitives::UnPackBuffer<uint32_t>(data_ptr_cast(string_lengths.data()),
	                                             data_ptr_cast(string_lengths_ptr), dict_count, string_lengths_width);
	if (!initialize_dictionary || mode == DictFSSTMode::FSST_ONLY) {
		// Used by fetch, as fetch will never produce a DictionaryVector
		return;
	}

	dictionary = DictionaryVector::CreateReusableDictionary(segment.GetType(), dict_count);
	auto &dict_data = dictionary->data;
	auto dict_child_data = FlatVector::GetDataMutable<string_t>(dict_data);
	auto &validity = FlatVector::ValidityMutable(dict_data);
	D_ASSERT(dict_count >= 1);
	validity.SetInvalid(0);

	uint32_t offset = 0;
	for (uint32_t i = 0; i < dict_count; i++) {
		//! We can uncompress during fetching, we need the length of the string inside the dictionary
		auto string_len = string_lengths[i];
		dict_child_data[i] = FetchStringFromDict(dict_data, offset, i);
		offset += string_len;
	}
}

const SelectionVector &CompressedStringScanState::GetSelVec(idx_t start, idx_t scan_count) {
	switch (mode) {
	case DictFSSTMode::FSST_ONLY: {
		return *FlatVector::IncrementalSelectionVector();
	}
	default: {
		// Handling non-bitpacking-group-aligned start values;
		idx_t start_offset = start % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;

		// We will scan in blocks of BITPACKING_ALGORITHM_GROUP_SIZE, so we may scan some extra values.
		idx_t decompress_count = BitpackingPrimitives::RoundUpToAlgorithmGroupSize(scan_count + start_offset);

		if (!sel_vec || sel_vec_size < decompress_count) {
			sel_vec_size = decompress_count;
			sel_vec = make_buffer<SelectionVector>(decompress_count);
		}

		data_ptr_t sel_buf_src = &dictionary_indices_ptr[((start - start_offset) * dictionary_indices_width) / 8];
		sel_t *sel_vec_ptr = sel_vec->data();
		BitpackingPrimitives::UnPackBuffer<sel_t>(data_ptr_cast(sel_vec_ptr), sel_buf_src, decompress_count,
		                                          dictionary_indices_width);

		sel_vec->ShiftLeft(start_offset, scan_count);

		return *sel_vec;
	}
	}
}

void CompressedStringScanState::ScanToFlatVector(Vector &result, idx_t result_offset, idx_t start, idx_t scan_count) {
	// Create a decompression buffer of sufficient size if we don't already have one.
	auto &selvec = GetSelVec(start, scan_count);

	//! (index 0 is reserved for NULL, which we don't have in this mode)
	const idx_t start_offset = mode == DictFSSTMode::FSST_ONLY ? start + 1 : 0;

	auto result_data = FlatVector::Writer<string_t>(result, scan_count, result_offset);
	if (dictionary) {
		// We have prepared the full dictionary, we can reference these strings directly
		auto dictionary_values = FlatVector::GetData<string_t>(dictionary->data);
		for (idx_t i = 0; i < scan_count; i++) {
			// Lookup dict offset in index buffer
			auto string_number = selvec.get_index(i + start_offset);
			if (string_number == 0) {
				result_data.WriteNull();
				continue;
			}
			result_data.WriteStringRef(dictionary_values[string_number]);
		}
	} else {
		for (idx_t i = 0; i < scan_count; i++) {
			// Lookup dict offset in index buffer
			auto string_number = selvec.get_index(start_offset + i);
			if (string_number == 0) {
				result_data.WriteNull();
				continue;
			}
			if (decompress_position > string_number) {
				throw InternalException("DICT_FSST: not performing a sequential scan?");
			}
			for (; decompress_position < string_number; decompress_position++) {
				decompress_offset += string_lengths[decompress_position];
			}
			result_data.WriteStringRef(FetchStringFromDict(result, decompress_offset, string_number));
		}
	}
	result.Verify();
}

void CompressedStringScanState::FetchRows(Vector &result, idx_t result_offset, const idx_t *offsets,
                                          idx_t fetch_count) {
	D_ASSERT(fetch_count > 1);
	auto result_data = FlatVector::Writer<string_t>(result, fetch_count, result_offset);
	if (mode == DictFSSTMode::FSST_ONLY) {
		for (idx_t fetch_idx = 0; fetch_idx < fetch_count; fetch_idx++) {
			const idx_t string_number = offsets[fetch_idx] + 1;
			if (DUCKDB_UNLIKELY(string_number >= dict_count)) {
				throw IOException("Failed to scan DICT_FSST string segment: dictionary index was out of range. "
				                  "Database file appears to be corrupted.");
			}
			D_ASSERT(decompress_position <= string_number);
			for (; decompress_position < string_number; decompress_position++) {
				const uint64_t next_offset =
				    NumericCast<uint64_t>(decompress_offset) + string_lengths[decompress_position];
				if (DUCKDB_UNLIKELY(next_offset > dictionary_size)) {
					ThrowInvalidDictionarySize();
				}
				decompress_offset = NumericCast<uint32_t>(next_offset);
			}
			if (DUCKDB_UNLIKELY(NumericCast<uint64_t>(decompress_offset) + string_lengths[string_number] >
			                    dictionary_size)) {
				ThrowInvalidDictionarySize();
			}
			result_data.WriteStringRef(FetchStringFromDict(result, decompress_offset, string_number));
		}
		result.Verify();
		return;
	}

	auto for_each_string_number = [&](auto &&callback) {
		idx_t fetch_idx = 0;
		while (fetch_idx < fetch_count) {
			const idx_t window_start = offsets[fetch_idx];
			idx_t window_end = fetch_idx + 1;
			while (window_end < fetch_count && offsets[window_end] - window_start < DICTIONARY_INDEX_WINDOW_SIZE) {
				window_end++;
			}

			const idx_t window_count = offsets[window_end - 1] - window_start + 1;
			auto &selvec = GetSelVec(window_start, window_count);
			for (; fetch_idx < window_end; fetch_idx++) {
				const idx_t string_number = selvec.get_index(offsets[fetch_idx] - window_start);
				if (DUCKDB_UNLIKELY(string_number >= dict_count)) {
					throw IOException("Failed to scan DICT_FSST string segment: dictionary index was out of range. "
					                  "Database file appears to be corrupted.");
				}
				callback(fetch_idx, string_number);
			}
		}
	};

	// Materializing a small offset table is cheaper than sorting selected dictionary indices.
	if (dict_count <= SMALL_DICTIONARY_SIZE) {
		vector<uint32_t> dictionary_offsets(dict_count);
		uint64_t dictionary_offset = 0;
		for (idx_t dictionary_idx = 0; dictionary_idx < dict_count; dictionary_idx++) {
			dictionary_offsets[dictionary_idx] = NumericCast<uint32_t>(dictionary_offset);
			dictionary_offset += string_lengths[dictionary_idx];
			if (DUCKDB_UNLIKELY(dictionary_offset > dictionary_size)) {
				ThrowInvalidDictionarySize();
			}
		}
		if (DUCKDB_UNLIKELY(dictionary_offset != dictionary_size)) {
			ThrowInvalidDictionarySize();
		}
		for_each_string_number([&](idx_t, idx_t string_number) {
			if (string_number == 0) {
				result_data.WriteNull();
			} else {
				result_data.WriteStringRef(
				    FetchStringFromDict(result, dictionary_offsets[string_number], string_number));
			}
		});
		result.Verify();
		return;
	}

	vector<idx_t> string_numbers(fetch_count);
	for_each_string_number([&](idx_t fetch_idx, idx_t string_number) { string_numbers[fetch_idx] = string_number; });

	vector<idx_t> fetch_order(fetch_count);
	for (idx_t idx = 0; idx < fetch_count; idx++) {
		fetch_order[idx] = idx;
	}
	std::sort(fetch_order.begin(), fetch_order.end(),
	          [&](idx_t left, idx_t right) { return string_numbers[left] < string_numbers[right]; });

	vector<uint32_t> dictionary_offsets(fetch_count);
	uint64_t dictionary_offset = 0;
	idx_t dictionary_idx = 0;
	for (const auto ordered_idx : fetch_order) {
		const idx_t string_number = string_numbers[ordered_idx];
		while (dictionary_idx < string_number) {
			dictionary_offset += string_lengths[dictionary_idx++];
			if (DUCKDB_UNLIKELY(dictionary_offset > dictionary_size)) {
				ThrowInvalidDictionarySize();
			}
		}
		const uint64_t string_end = dictionary_offset + string_lengths[string_number];
		if (DUCKDB_UNLIKELY(string_end > dictionary_size ||
		                    (string_number + 1 == dict_count && string_end != dictionary_size))) {
			ThrowInvalidDictionarySize();
		}
		dictionary_offsets[ordered_idx] = NumericCast<uint32_t>(dictionary_offset);
	}

	for (idx_t fetch_idx = 0; fetch_idx < fetch_count; fetch_idx++) {
		const idx_t string_number = string_numbers[fetch_idx];
		if (string_number == 0) {
			result_data.WriteNull();
			continue;
		}
		result_data.WriteStringRef(FetchStringFromDict(result, dictionary_offsets[fetch_idx], string_number));
	}
	result.Verify();
}

void CompressedStringScanState::Select(Vector &result, idx_t start, const SelectionVector &sel, idx_t sel_count) {
	D_ASSERT(!dictionary);
	D_ASSERT(mode == DictFSSTMode::FSST_ONLY);
	idx_t start_offset = start + 1;
	auto result_data = FlatVector::Writer<string_t>(result, sel_count);
	for (idx_t i = 0; i < sel_count; i++) {
		// Lookup dict offset in index buffer
		auto string_number = start_offset + sel.get_index(i);
		if (decompress_position > string_number) {
			throw InternalException("DICT_FSST: not performing a sequential scan?");
		}
		for (; decompress_position < string_number; decompress_position++) {
			decompress_offset += string_lengths[decompress_position];
		}
		result_data.WriteValue(FetchStringFromDict(result, decompress_offset, string_number));
	}
}

bool CompressedStringScanState::AllowDictionaryScan(idx_t scan_count) {
	if (mode == DictFSSTMode::FSST_ONLY) {
		return false;
	}
	if (scan_count != STANDARD_VECTOR_SIZE) {
		return false;
	}
	if (!dictionary) {
		return false;
	}
	return true;
}

void CompressedStringScanState::ScanToDictionaryVector(ColumnSegment &segment, Vector &result, idx_t result_offset,
                                                       idx_t start, idx_t scan_count) {
	D_ASSERT(scan_count == STANDARD_VECTOR_SIZE);
	D_ASSERT(result_offset == 0);

	auto &selvec = GetSelVec(start, scan_count);
	result.Dictionary(dictionary, selvec, scan_count);
	result.Verify();
}

} // namespace dict_fsst
} // namespace duckdb
