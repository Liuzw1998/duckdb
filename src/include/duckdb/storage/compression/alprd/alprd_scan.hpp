//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/alprd/alprd_scan.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/compression/alprd/algorithm/alprd.hpp"
#include "duckdb/storage/compression/alprd/alprd_constants.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/scan_state.hpp"

#include <algorithm>

namespace duckdb {

template <class T>
struct AlpRDVectorState {
public:
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

	void Reset() {
		index = 0;
	}

	void ValidateExceptionsCount(idx_t vector_size) const {
		if (exceptions_count > vector_size) {
			throw DataCorruptionException("Corrupted ALPRD segment: exceptions_count (%d) exceeds vector_size (%d)",
			                              exceptions_count, vector_size);
		}
	}

	// Scan of the data itself
	template <bool SKIP = false>
	void Scan(uint8_t *dest, idx_t count) {
		if (!SKIP) {
			memcpy(dest, (void *)(decoded_values + index), sizeof(T) * count);
		}
		index += count;
	}

	template <bool SKIP>
	void LoadValues(EXACT_TYPE *values_buffer, idx_t count) {
		if (SKIP) {
			return;
		}
		values_buffer[0] = (EXACT_TYPE)0;
		alp::AlpRDDecompression<T>::Decompress(left_encoded, right_encoded, left_parts_dict, values_buffer, count,
		                                       exceptions_count, exceptions, exceptions_positions, left_bit_width,
		                                       right_bit_width);
	}

	void DecodeSelected(const idx_t *offsets, idx_t fetch_count, idx_t vector_start, EXACT_TYPE *result) {
		if (uncompressed_mode) {
			for (idx_t i = 0; i < fetch_count; i++) {
				const idx_t offset_in_vector = offsets[i] - vector_start;
				result[i] = Load<EXACT_TYPE>(uncompressed_data + offset_in_vector * sizeof(EXACT_TYPE));
			}
			return;
		}
		uint16_t selected_exceptions[AlpRDConstants::ALP_VECTOR_SIZE] = {0};
		bool is_exception[AlpRDConstants::ALP_VECTOR_SIZE] = {false};
		for (idx_t exception_idx = 0; exception_idx < exceptions_count; exception_idx++) {
			const idx_t exception_offset = vector_start + exceptions_positions[exception_idx];
			const auto match = std::lower_bound(offsets, offsets + fetch_count, exception_offset);
			if (match != offsets + fetch_count && *match == exception_offset) {
				const idx_t result_idx = NumericCast<idx_t>(match - offsets);
				selected_exceptions[result_idx] = exceptions[exception_idx];
				is_exception[result_idx] = true;
			}
		}

		BitpackingPrimitives::ForEachSelectedAlgorithmGroup(
		    offsets, fetch_count, vector_start, [&](idx_t group_begin, idx_t group_end, idx_t group_start) {
			    uint16_t left_group[BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE] = {0};
			    EXACT_TYPE right_group[BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE] = {0};
			    if (left_bit_width > 0) {
				    BitpackingPrimitives::UnPackBlock<uint16_t>(
				        data_ptr_cast(left_group), left_encoded + group_start * left_bit_width / 8, left_bit_width);
			    }
			    if (right_bit_width > 0) {
				    BitpackingPrimitives::UnPackBlock<EXACT_TYPE>(
				        data_ptr_cast(right_group), right_encoded + group_start * right_bit_width / 8, right_bit_width);
			    }

			    for (idx_t i = group_begin; i < group_end; i++) {
				    const idx_t position = offsets[i] - vector_start;
				    const idx_t group_offset = position - group_start;
				    uint16_t left = selected_exceptions[i];
				    if (!is_exception[i]) {
					    const auto dictionary_index = left_group[group_offset];
					    if (dictionary_index >= dictionary_size) {
						    throw DataCorruptionException(
						        "Corrupted ALPRD segment: dictionary index exceeds dictionary size");
					    }
					    left = left_parts_dict[dictionary_index];
				    }
				    if (right_bit_width == sizeof(EXACT_TYPE) * 8) {
					    if (left != 0) {
						    throw DataCorruptionException("Corrupted ALPRD segment: left part exceeds value width");
					    }
					    result[i] = right_group[group_offset];
				    } else {
					    result[i] = (static_cast<EXACT_TYPE>(left) << right_bit_width) | right_group[group_offset];
				    }
			    }
		    });
	}

public:
	idx_t index;
	uint8_t left_encoded[AlpRDConstants::ALP_VECTOR_SIZE * 8];
	uint8_t right_encoded[AlpRDConstants::ALP_VECTOR_SIZE * 8];
	EXACT_TYPE decoded_values[AlpRDConstants::ALP_VECTOR_SIZE];
	uint16_t exceptions[AlpRDConstants::ALP_VECTOR_SIZE];
	uint16_t exceptions_positions[AlpRDConstants::ALP_VECTOR_SIZE];
	uint16_t exceptions_count;
	uint8_t right_bit_width;
	uint8_t left_bit_width;
	uint16_t left_parts_dict[AlpRDConstants::MAX_DICTIONARY_SIZE];
	uint8_t dictionary_size;
	bool uncompressed_mode;
	data_ptr_t uncompressed_data;
};

template <class T>
struct AlpRDScanState : public SegmentScanState {
public:
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

	explicit AlpRDScanState(ColumnSegment &segment) : segment(segment), count(segment.count) {
		auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());

		handle = buffer_manager.Pin(segment.GetBlockHandle());
		// ScanStates never exceed the boundaries of a Segment,
		// but are not guaranteed to start at the beginning of the Block
		segment_data = handle.GetDataMutable() + segment.GetBlockOffset();
		const auto block_size = segment.GetBlockSize();

		idx_t total_segment_offset = segment.GetBlockOffset();
		auto metadata_offset = Load<uint32_t>(segment_data);
		auto segment_ptr = segment_data + AlpRDConstants::METADATA_POINTER_SIZE;
		total_segment_offset += AlpRDConstants::METADATA_POINTER_SIZE;

		metadata_ptr = segment_data + metadata_offset;
		const idx_t metadata_ptr_offset = segment.GetBlockOffset() + metadata_offset;
		if (metadata_ptr_offset > block_size) {
			throw DataCorruptionException("Corrupted ALPRD segment: metadata_offset value is corrupted");
		}

		if (total_segment_offset + AlpRDConstants::HEADER_SIZE > block_size) {
			throw DataCorruptionException("Corrupted ALPRD segment: reading header bytes would exceed block space");
		}

		// Load the Right Bit Width which is in the segment header after the pointer to the first metadata
		vector_state.right_bit_width = Load<uint8_t>(segment_ptr);
		segment_ptr += AlpRDConstants::RIGHT_BIT_WIDTH_SIZE;

		vector_state.left_bit_width = Load<uint8_t>(segment_ptr);
		segment_ptr += AlpRDConstants::LEFT_BIT_WIDTH_SIZE;
		if (vector_state.right_bit_width > sizeof(EXACT_TYPE) * 8 ||
		    vector_state.left_bit_width > sizeof(uint16_t) * 8) {
			throw DataCorruptionException("Corrupted ALPRD segment: bit width exceeds value width");
		}

		uint8_t actual_dictionary_size = Load<uint8_t>(segment_ptr);
		segment_ptr += AlpRDConstants::N_DICTIONARY_ELEMENTS_SIZE;

		total_segment_offset += AlpRDConstants::HEADER_SIZE;

		if (actual_dictionary_size > AlpRDConstants::MAX_DICTIONARY_SIZE) {
			throw DataCorruptionException("Corrupt database file: ALPRD dictionary size exceeds maximum");
		}
		vector_state.dictionary_size = actual_dictionary_size;
		idx_t actual_dictionary_size_bytes =
		    static_cast<idx_t>(actual_dictionary_size) * AlpRDConstants::DICTIONARY_ELEMENT_SIZE;

		const idx_t left_parts_dict_max_size = sizeof(vector_state.left_parts_dict);
		if (total_segment_offset + actual_dictionary_size_bytes > metadata_ptr_offset ||
		    actual_dictionary_size_bytes > left_parts_dict_max_size) {
			throw DataCorruptionException("Corrupted ALPRD segment: actual_dictionary_size is corrupted");
		}
		// Load the left parts dictionary which is after the segment header and is of a fixed size
		memcpy(vector_state.left_parts_dict, segment_ptr, actual_dictionary_size_bytes);
	}

	BufferHandle handle;
	data_ptr_t metadata_ptr;
	data_ptr_t segment_data;
	idx_t total_value_count = 0;
	AlpRDVectorState<T> vector_state;

	ColumnSegment &segment;
	idx_t count;

	idx_t LeftInVector() const {
		return AlpRDConstants::ALP_VECTOR_SIZE - (total_value_count % AlpRDConstants::ALP_VECTOR_SIZE);
	}

	inline bool VectorFinished() const {
		return (total_value_count % AlpRDConstants::ALP_VECTOR_SIZE) == 0;
	}

	// Scan up to a vector boundary
	template <class EXACT_TYPE, bool SKIP = false>
	void ScanVector(EXACT_TYPE *values, idx_t vector_size) {
		D_ASSERT(vector_size <= AlpRDConstants::ALP_VECTOR_SIZE);
		D_ASSERT(vector_size <= LeftInVector());
		if (VectorFinished() && total_value_count < count) {
			if (vector_size == AlpRDConstants::ALP_VECTOR_SIZE) {
				LoadVector<SKIP>(values);
				total_value_count += vector_size;
				return;
			} else {
				// Even if SKIP is given, the vector size is not big enough to be able to fully skip the entire vector
				LoadVector<false>(vector_state.decoded_values);
			}
		}
		vector_state.template Scan<SKIP>((uint8_t *)values, vector_size);

		total_value_count += vector_size;
	}

	// Using the metadata, we can avoid loading any of the data if we don't care about the vector at all
	void SkipVector() {
		// Skip the offset indicating where the data starts
		metadata_ptr -= AlpRDConstants::METADATA_POINTER_SIZE;
		idx_t vector_size = MinValue((idx_t)AlpRDConstants::ALP_VECTOR_SIZE, count - total_value_count);
		total_value_count += vector_size;
	}

	idx_t LoadVectorData() {
		vector_state.Reset();

		// Load the offset (metadata) indicating where the vector data starts
		metadata_ptr -= AlpRDConstants::METADATA_POINTER_SIZE;
		auto data_byte_offset = Load<uint32_t>(metadata_ptr);
		const auto block_size = segment.GetBlockSize();
		if (data_byte_offset >= block_size) {
			throw IOException(
			    "Corrupted ALPRD segment: stored data_byte_offset (%d) exceeds the segments block size (%d)",
			    data_byte_offset, block_size);
		}

		idx_t vector_size = MinValue((idx_t)AlpRDConstants::ALP_VECTOR_SIZE, (count - total_value_count));

		data_ptr_t vector_ptr = segment_data + data_byte_offset;

		// Load the vector data
		vector_state.exceptions_count = Load<uint16_t>(vector_ptr);
		vector_ptr += AlpRDConstants::EXCEPTIONS_COUNT_SIZE;

		vector_state.uncompressed_mode = vector_state.exceptions_count == AlpRDConstants::UNCOMPRESSED_MODE_SENTINEL;
		if (vector_state.uncompressed_mode) {
			const idx_t value_buffer_copy_size = sizeof(T) * vector_size;
			if (vector_ptr + value_buffer_copy_size > segment_data + block_size) {
				const auto bytes_remaining_in_block = (segment_data + block_size) - vector_ptr;
				throw DataCorruptionException("Corrupted ALPRD segment: stored vector_size is invalid, to-copy bytes "
				                              "(%d) would exceed bytes remaining in the block (%d)",
				                              value_buffer_copy_size, bytes_remaining_in_block);
			}
			vector_state.uncompressed_data = vector_ptr;
			return vector_size;
		}
		auto left_bp_size = BitpackingPrimitives::GetRequiredSize(vector_size, vector_state.left_bit_width);
		auto right_bp_size = BitpackingPrimitives::GetRequiredSize(vector_size, vector_state.right_bit_width);

		idx_t read_bytes = 0;
		const idx_t max_left_encoded_size = sizeof(vector_state.left_encoded);
		if (left_bp_size > max_left_encoded_size || data_byte_offset + read_bytes + left_bp_size > block_size) {
			throw DataCorruptionException("Corrupted ALPRD segment: left_encoded payload too large");
		}
		memcpy(vector_state.left_encoded, (void *)vector_ptr, left_bp_size);
		vector_ptr += left_bp_size;
		read_bytes += left_bp_size;

		const idx_t max_right_encoded_size = sizeof(vector_state.right_encoded);
		if (right_bp_size > max_right_encoded_size || data_byte_offset + read_bytes + right_bp_size > block_size) {
			throw DataCorruptionException("Corrupted ALPRD segment: left_encoded payload too large");
		}
		memcpy(vector_state.right_encoded, (void *)vector_ptr, right_bp_size);
		vector_ptr += right_bp_size;
		read_bytes += right_bp_size;

		if (vector_state.exceptions_count > 0) {
			//! Load the exceptions
			const idx_t max_exceptions_size = sizeof(vector_state.exceptions);
			const idx_t exceptions_copy_size = AlpRDConstants::EXCEPTION_SIZE * vector_state.exceptions_count;
			if (exceptions_copy_size > max_exceptions_size ||
			    data_byte_offset + read_bytes + exceptions_copy_size > block_size) {
				throw DataCorruptionException("Corrupted ALPRD segment: exceptions payload too large");
			}
			vector_state.ValidateExceptionsCount(vector_size);
			memcpy(vector_state.exceptions, (void *)vector_ptr, exceptions_copy_size);
			vector_ptr += exceptions_copy_size;
			read_bytes += exceptions_copy_size;

			//! Load the exceptions_positions
			const idx_t max_exceptions_positions_size = sizeof(vector_state.exceptions_positions);
			const idx_t exceptions_positions_copy_size =
			    AlpRDConstants::EXCEPTION_POSITION_SIZE * vector_state.exceptions_count;
			if (exceptions_positions_copy_size > max_exceptions_positions_size ||
			    data_byte_offset + read_bytes + exceptions_positions_copy_size > block_size) {
				throw DataCorruptionException("Corrupted ALPRD segment: exceptions_positions payload too large");
			}
			memcpy(vector_state.exceptions_positions, (void *)vector_ptr, exceptions_positions_copy_size);
			vector_ptr += exceptions_positions_copy_size;
			read_bytes += exceptions_positions_copy_size;

			//! The exception positions index into the decoded vector, so they must stay within its bounds
			for (idx_t i = 0; i < vector_state.exceptions_count; i++) {
				if (vector_state.exceptions_positions[i] >= vector_size) {
					throw IOException("Corrupted ALPRD segment: exception position (%d) exceeds vector_size (%d)",
					                  vector_state.exceptions_positions[i], vector_size);
				}
			}
		}

		return vector_size;
	}

	template <bool SKIP = false>
	void LoadVector(EXACT_TYPE *value_buffer) {
		const idx_t vector_size = LoadVectorData();
		if (vector_state.uncompressed_mode) {
			if (!SKIP) {
				memcpy(value_buffer, vector_state.uncompressed_data, sizeof(T) * vector_size);
			}
			return;
		}
		vector_state.template LoadValues<SKIP>(value_buffer, vector_size);
	}

	void FetchRows(const idx_t *offsets, idx_t fetch_count, EXACT_TYPE *result) {
		idx_t fetch_idx = 0;
		while (fetch_idx < fetch_count) {
			const idx_t vector_idx = offsets[fetch_idx] / AlpRDConstants::ALP_VECTOR_SIZE;
			while (total_value_count / AlpRDConstants::ALP_VECTOR_SIZE < vector_idx) {
				SkipVector();
			}

			const idx_t vector_start = total_value_count;
			const idx_t vector_size = LoadVectorData();
			idx_t vector_fetch_end = fetch_idx + 1;
			while (vector_fetch_end < fetch_count && offsets[vector_fetch_end] < vector_start + vector_size) {
				vector_fetch_end++;
			}
			vector_state.DecodeSelected(offsets + fetch_idx, vector_fetch_end - fetch_idx, vector_start,
			                            result + fetch_idx);
			total_value_count += vector_size;
			fetch_idx = vector_fetch_end;
		}
	}

public:
	//! Skip the next 'skip_count' values, we don't store the values
	void Skip(ColumnSegment &col_segment, idx_t skip_count) {
		if (total_value_count != 0 && !VectorFinished()) {
			// Finish skipping the current vector
			idx_t to_skip = MinValue<idx_t>(skip_count, LeftInVector());
			ScanVector<EXACT_TYPE, true>(nullptr, to_skip);
			skip_count -= to_skip;
		}
		// Figure out how many entire vectors we can skip
		// For these vectors, we don't even need to process the metadata or values
		idx_t vectors_to_skip = skip_count / AlpRDConstants::ALP_VECTOR_SIZE;
		for (idx_t i = 0; i < vectors_to_skip; i++) {
			SkipVector();
		}
		skip_count -= AlpRDConstants::ALP_VECTOR_SIZE * vectors_to_skip;
		if (skip_count == 0) {
			return;
		}
		// For the last vector that this skip (partially) touches, we do need to
		// load the metadata and values into the vector_state because
		// we don't know exactly how many they are
		ScanVector<EXACT_TYPE, true>(nullptr, skip_count);
	}
};

template <class T>
unique_ptr<SegmentScanState> AlpRDInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto result = make_uniq_base<SegmentScanState, AlpRDScanState<T>>(segment);
	return result;
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <class T>
void AlpRDScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                      idx_t result_offset) {
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;
	auto &scan_state = (AlpRDScanState<T> &)*state.scan_state;

	// Get the pointer to the result values
	auto current_result_ptr = FlatVector::GetDataMutableUnsafe<EXACT_TYPE>(result);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	current_result_ptr += result_offset;

	idx_t scanned = 0;
	while (scanned < scan_count) {
		const auto remaining = scan_count - scanned;
		const idx_t to_scan = MinValue(remaining, scan_state.LeftInVector());

		scan_state.template ScanVector<EXACT_TYPE>(current_result_ptr + scanned, to_scan);
		scanned += to_scan;
	}
}

template <class T>
void AlpRDSkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
	auto &scan_state = (AlpRDScanState<T> &)*state.scan_state;
	scan_state.Skip(segment, skip_count);
}

template <class T>
void AlpRDScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	AlpRDScanPartial<T>(segment, state, scan_count, result, 0);
}

template <class T>
void AlpRDFetchRows(ColumnSegment &segment, ColumnFetchState &state, const idx_t *offsets, idx_t fetch_count,
                    Vector &result, idx_t result_offset) {
	D_ASSERT(fetch_count > 1);
	const bool consecutive = offsets[fetch_count - 1] - offsets[0] == fetch_count - 1;
	if (consecutive) {
		ColumnSegment::FetchRowsUsingScan(segment, state, offsets, fetch_count, result, result_offset);
		return;
	}

	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;
	auto result_data = FlatVector::GetDataMutableUnsafe<EXACT_TYPE>(result);
	AlpRDScanState<T> scan_state(segment);
	scan_state.FetchRows(offsets, fetch_count, result_data + result_offset);
}

} // namespace duckdb
