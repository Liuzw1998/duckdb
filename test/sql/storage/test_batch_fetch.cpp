#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/storage/compression/alp/alp_scan.hpp"
#include "duckdb/storage/compression/alprd/alprd_scan.hpp"
#include "duckdb/storage/compression/dict_fsst/decompression.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

using namespace duckdb;

TEST_CASE("Batch fetch preserves arbitrary row ID order", "[storage]") {
	DuckDB db(nullptr);
	Connection con(db);

	REQUIRE_NO_FAIL(con.Query("CREATE TABLE fetch_order ("
	                          "id BIGINT USING COMPRESSION BitPacking, "
	                          "payload VARCHAR USING COMPRESSION DICT_FSST, "
	                          "repeated_payload VARCHAR USING COMPRESSION DICT_FSST, "
	                          "dictionary_payload VARCHAR USING COMPRESSION DICT_FSST)"));
	// Force the dictionary past the threshold where DICT_FSST encoding is selected.
	REQUIRE_NO_FAIL(con.Query("INSERT INTO fetch_order "
	                          "SELECT i, repeat(i::VARCHAR || '_', 16), "
	                          "CASE WHEN i % 17 = 0 THEN NULL "
	                          "     WHEN i % 19 = 0 THEN '' "
	                          "     ELSE 'duck:goose token token duck:goose' || "
	                          "          lpad((i % 4096)::VARCHAR, 6, '0') || "
	                          "          'duck:goose token-token duck:goose' END, "
	                          "CASE WHEN i % 17 = 0 THEN NULL "
	                          "     WHEN i % 19 = 0 THEN '' "
	                          "     ELSE chr(65 + (i % 8)::INTEGER) END "
	                          "FROM range(20000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("FORCE CHECKPOINT"));
	auto storage_modes =
	    con.Query("SELECT count(*) FILTER (WHERE column_name = 'payload' AND segment_info LIKE 'FSST_ONLY:%') > 0, "
	              "count(*) FILTER (WHERE column_name = 'repeated_payload' AND segment_info LIKE 'DICT_FSST:%') > 0, "
	              "count(*) FILTER (WHERE column_name = 'dictionary_payload' "
	              "                         AND segment_info LIKE 'DICTIONARY:%') > 0, "
	              "count(*) FILTER (WHERE column_name = 'id' AND lower(compression) = 'bitpacking') > 0 "
	              "FROM pragma_storage_info('fetch_order', include_segment_info=true)");
	REQUIRE_NO_FAIL(*storage_modes);
	for (idx_t column_idx = 0; column_idx < 4; column_idx++) {
		CAPTURE(column_idx);
		REQUIRE(storage_modes->GetValue(column_idx, 0).GetValue<bool>());
	}
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE fetch_dictionary_lifetime ("
	                          "payload VARCHAR USING COMPRESSION DICT_FSST)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO fetch_dictionary_lifetime "
	                          "SELECT repeat(chr(65 + (i % 8)::INTEGER), 64) "
	                          "FROM range(20000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("FORCE CHECKPOINT"));
	REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));

	auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(*con.context, QualifiedName(Identifier("fetch_order")));
	auto &storage = table_entry.GetStorage();
	auto &transaction = DuckTransaction::Get(*con.context, storage.GetAttached());

	auto check_rows = [&](const vector<row_t> &requested_row_ids, const vector<row_t> &expected_row_ids) {
		REQUIRE(requested_row_ids.size() <= STANDARD_VECTOR_SIZE);
		Vector row_ids(LogicalType::ROW_TYPE);
		for (auto row_id : requested_row_ids) {
			row_ids.Append(Value::BIGINT(row_id));
		}

		DataChunk result;
		result.Initialize(*con.context,
		                  {LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR});
		vector<StorageIndex> column_ids {StorageIndex(0), StorageIndex(1), StorageIndex(2), StorageIndex(3)};
		ColumnFetchState fetch_state;
		fetch_state.context = *con.context;
		storage.Fetch(transaction, result, column_ids, row_ids, requested_row_ids.size(), fetch_state);

		REQUIRE(result.size() == expected_row_ids.size());
		for (idx_t i = 0; i < expected_row_ids.size(); i++) {
			const auto row_id = expected_row_ids[i];
			REQUIRE(result.GetValue(0, i).GetValue<int64_t>() == row_id);
			REQUIRE(result.GetValue(1, i).GetValue<string>() == StringUtil::Repeat(to_string(row_id) + "_", 16));
			if (row_id % 17 == 0) {
				REQUIRE(result.GetValue(2, i).IsNull());
				REQUIRE(result.GetValue(3, i).IsNull());
			} else if (row_id % 19 == 0) {
				REQUIRE(result.GetValue(2, i).GetValue<string>().empty());
				REQUIRE(result.GetValue(3, i).GetValue<string>().empty());
			} else {
				REQUIRE(result.GetValue(2, i).GetValue<string>() == "duck:goose token token duck:goose" +
				                                                        StringUtil::Format("%06d", row_id % 4096) +
				                                                        "duck:goose token-token duck:goose");
				REQUIRE(result.GetValue(3, i).GetValue<string>() == string(1, static_cast<char>('A' + row_id % 8)));
			}
		}
	};

	check_rows({2}, {2});
	check_rows({2, 17}, {2, 17});
	check_rows({19, 102}, {19, 102});
	check_rows({102, 102}, {102, 102});
	check_rows({102, 101}, {102, 101});
	check_rows({0, 19999}, {0, 19999});

	auto row_groups = storage.GetRowGroupCollection()->GetRowGroups();
	auto row_group = row_groups->GetSegment(0);
	REQUIRE(row_group);
	auto &id_column = row_group->GetNode().GetRawColumnData(StorageIndex(0));
	auto id_segment = id_column.GetSegmentTree().GetSegment(0);
	REQUIRE(id_segment);
	auto &segment = id_segment->GetNode();
	const idx_t segment_count = segment.count.load();
	REQUIRE(segment_count > STANDARD_VECTOR_SIZE);

	ColumnFetchState segment_fetch_state;
	segment_fetch_state.context = *con.context;
	idx_t dense_offsets[STANDARD_VECTOR_SIZE];
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		dense_offsets[i] = i;
	}
	Vector dense_result(LogicalType::BIGINT);
	segment.FetchRows(segment_fetch_state, dense_offsets, STANDARD_VECTOR_SIZE, dense_result, 0);
	REQUIRE(dense_result.GetValue(0).GetValue<int64_t>() == 0);
	REQUIRE(dense_result.GetValue(STANDARD_VECTOR_SIZE - 1).GetValue<int64_t>() == STANDARD_VECTOR_SIZE - 1);

	idx_t sparse_offsets[] {0, segment_count - 1};
	Vector sparse_result(LogicalType::BIGINT);
	segment.FetchRows(segment_fetch_state, sparse_offsets, 2, sparse_result, STANDARD_VECTOR_SIZE - 2);
	REQUIRE(sparse_result.GetValue(STANDARD_VECTOR_SIZE - 2).GetValue<int64_t>() == 0);
	REQUIRE(sparse_result.GetValue(STANDARD_VECTOR_SIZE - 1).GetValue<int64_t>() == segment_count - 1);

	idx_t duplicate_offsets[] {2, 2};
	Vector duplicate_result(LogicalType::BIGINT, 2);
	segment.FetchRows(segment_fetch_state, duplicate_offsets, 2, duplicate_result, 0);
	REQUIRE(duplicate_result.GetValue(0).GetValue<int64_t>() == 2);
	REQUIRE(duplicate_result.GetValue(1).GetValue<int64_t>() == 2);

	idx_t reverse_offsets[] {3, 1};
	Vector reverse_result(LogicalType::BIGINT, 2);
	segment.FetchRows(segment_fetch_state, reverse_offsets, 2, reverse_result, 0);
	REQUIRE(reverse_result.GetValue(0).GetValue<int64_t>() == 3);
	REQUIRE(reverse_result.GetValue(1).GetValue<int64_t>() == 1);

	if (STANDARD_VECTOR_SIZE >= 3) {
		idx_t unordered_offsets[] {4, 1, 3};
		Vector unordered_result(LogicalType::BIGINT, 3);
		segment.FetchRows(segment_fetch_state, unordered_offsets, 3, unordered_result, 0);
		REQUIRE(unordered_result.GetValue(0).GetValue<int64_t>() == 4);
		REQUIRE(unordered_result.GetValue(1).GetValue<int64_t>() == 1);
		REQUIRE(unordered_result.GetValue(2).GetValue<int64_t>() == 3);
	}

	idx_t single_offset = 5;
	Vector single_result(LogicalType::BIGINT, 2);
	segment.FetchRows(segment_fetch_state, &single_offset, 1, single_result, 1);
	REQUIRE(single_result.GetValue(1).GetValue<int64_t>() == 5);

	Vector small_result(LogicalType::BIGINT, 1);
	small_result.SetValue(0, Value::BIGINT(42));
	REQUIRE_THROWS(segment.FetchRows(segment_fetch_state, sparse_offsets, 2, small_result, 0));
	REQUIRE(small_result.GetValue(0).GetValue<int64_t>() == 42);

	const idx_t large_capacity = STANDARD_VECTOR_SIZE + 3;
	Vector large_result(LogicalType::BIGINT, large_capacity);
	segment.FetchRows(segment_fetch_state, sparse_offsets, 2, large_result, large_capacity - 2);
	REQUIRE(large_result.GetValue(large_capacity - 2).GetValue<int64_t>() == 0);
	REQUIRE(large_result.GetValue(large_capacity - 1).GetValue<int64_t>() == segment_count - 1);

	idx_t scan_offsets[] {5, 6, 20, 21};
	Vector scan_result(LogicalType::BIGINT, 4);
	ColumnSegment::FetchRowsUsingScan(segment, segment_fetch_state, scan_offsets, 4, scan_result, 0);
	for (idx_t idx = 0; idx < 4; idx++) {
		REQUIRE(scan_result.GetValue(idx).GetValue<int64_t>() == NumericCast<int64_t>(scan_offsets[idx]));
	}

	auto fetch_string_segment = [&](const StorageIndex &column_id, const idx_t *offsets) {
		auto &column = row_group->GetNode().GetRawColumnData(column_id);
		auto segment_node = column.GetSegmentTree().GetSegment(0);
		REQUIRE(segment_node);
		REQUIRE(segment_node->GetRowStart() == 0);
		REQUIRE(segment_node->GetNode().count > 19);

		ColumnFetchState string_fetch_state;
		string_fetch_state.context = *con.context;
		Vector string_result(LogicalType::VARCHAR);
		segment_node->GetNode().FetchRows(string_fetch_state, offsets, 2, string_result, STANDARD_VECTOR_SIZE - 2);
		return vector<Value> {string_result.GetValue(STANDARD_VECTOR_SIZE - 2),
		                      string_result.GetValue(STANDARD_VECTOR_SIZE - 1)};
	};

	idx_t string_offsets[] {0, 19};
	auto payload_values = fetch_string_segment(StorageIndex(1), string_offsets);
	REQUIRE(payload_values[0].GetValue<string>() == StringUtil::Repeat("0_", 16));
	REQUIRE(payload_values[1].GetValue<string>() == StringUtil::Repeat("19_", 16));

	auto repeated_values = fetch_string_segment(StorageIndex(2), string_offsets);
	REQUIRE(repeated_values[0].IsNull());
	REQUIRE(repeated_values[1].GetValue<string>().empty());
	auto dictionary_values = fetch_string_segment(StorageIndex(3), string_offsets);
	REQUIRE(dictionary_values[0].IsNull());
	REQUIRE(dictionary_values[1].GetValue<string>().empty());

	auto check_dictionary_batches = [&](idx_t column_idx, bool high_cardinality) {
		auto &column = row_group->GetNode().GetRawColumnData(StorageIndex(column_idx));
		auto segment_node = column.GetSegmentTree().GetSegment(0);
		REQUIRE(segment_node);
		auto &dict_segment = segment_node->GetNode();
		const idx_t dict_segment_count = dict_segment.count.load();
		REQUIRE(dict_segment_count > 10);

		ColumnFetchState dictionary_state;
		dictionary_state.context = *con.context;
		dict_fsst::CompressedStringScanState scan_state(dict_segment, dictionary_state.GetOrInsertHandle(dict_segment));
		scan_state.Initialize(false);
		if (high_cardinality) {
			REQUIRE(scan_state.mode == dict_fsst::DictFSSTMode::DICT_FSST);
			REQUIRE(scan_state.dict_count > 1024);
		} else {
			REQUIRE(scan_state.mode == dict_fsst::DictFSSTMode::DICTIONARY);
			REQUIRE(scan_state.dict_count <= 16);
		}

		auto &dictionary_indices = scan_state.GetSelVec(0, dict_segment_count);
		vector<pair<idx_t, idx_t>> rows_by_dictionary;
		for (idx_t row_offset = 0; row_offset < dict_segment_count; row_offset++) {
			const idx_t dictionary_idx = dictionary_indices.get_index(row_offset);
			if (dictionary_idx != 0) {
				rows_by_dictionary.emplace_back(dictionary_idx, row_offset);
			}
		}
		std::sort(rows_by_dictionary.begin(), rows_by_dictionary.end());
		REQUIRE(rows_by_dictionary.size() > 10);

		for (const idx_t match_count : {2, 4, 10}) {
			for (idx_t position = 0; position < 3; position++) {
				CAPTURE(column_idx, match_count, position);
				vector<idx_t> selected_offsets;
				for (idx_t match_idx = 0; match_idx < match_count; match_idx++) {
					idx_t selected_idx;
					if (position == 0) {
						selected_idx = match_idx;
					} else if (position == 1) {
						selected_idx = rows_by_dictionary.size() - match_count + match_idx;
					} else {
						selected_idx = (match_idx + 1) * rows_by_dictionary.size() / (match_count + 1);
					}
					selected_offsets.push_back(rows_by_dictionary[selected_idx].second);
				}
				std::sort(selected_offsets.begin(), selected_offsets.end());

				ColumnFetchState fetch_state;
				fetch_state.context = *con.context;
				Vector result(LogicalType::VARCHAR, match_count);
				dict_segment.FetchRows(fetch_state, selected_offsets.data(), match_count, result, 0);
				for (idx_t match_idx = 0; match_idx < match_count; match_idx++) {
					const idx_t row_id = segment_node->GetRowStart() + selected_offsets[match_idx];
					if (row_id % 19 == 0) {
						REQUIRE(result.GetValue(match_idx).GetValue<string>().empty());
					} else if (high_cardinality) {
						REQUIRE(result.GetValue(match_idx).GetValue<string>() ==
						        "duck:goose token token duck:goose" + StringUtil::Format("%06d", row_id % 4096) +
						            "duck:goose token-token duck:goose");
					} else {
						REQUIRE(result.GetValue(match_idx).GetValue<string>() ==
						        string(1, static_cast<char>('A' + row_id % 8)));
					}
				}
			}
		}

		if (high_cardinality) {
			vector<idx_t> oversized_offsets(STANDARD_VECTOR_SIZE + 1);
			for (idx_t idx = 0; idx < oversized_offsets.size(); idx++) {
				oversized_offsets[idx] = idx + 1;
			}
			REQUIRE(dict_segment_count > oversized_offsets.back());
			ColumnFetchState fetch_state;
			fetch_state.context = *con.context;
			Vector result(LogicalType::VARCHAR, oversized_offsets.size());
			dict_segment.FetchRows(fetch_state, oversized_offsets.data(), oversized_offsets.size(), result, 0);
			for (const idx_t idx : {idx_t(0), idx_t(STANDARD_VECTOR_SIZE - 1), idx_t(STANDARD_VECTOR_SIZE)}) {
				const idx_t row_id = segment_node->GetRowStart() + oversized_offsets[idx];
				REQUIRE(result.GetValue(idx).GetValue<string>() == "duck:goose token token duck:goose" +
				                                                       StringUtil::Format("%06d", row_id % 4096) +
				                                                       "duck:goose token-token duck:goose");
			}
		}
	};
	check_dictionary_batches(2, true);
	check_dictionary_batches(3, false);

	auto &payload_column = row_group->GetNode().GetRawColumnData(StorageIndex(1));
	auto payload_segment_node = payload_column.GetSegmentTree().GetSegment(0);
	REQUIRE(payload_segment_node);
	auto &payload_segment = payload_segment_node->GetNode();
	const idx_t payload_segment_end = payload_segment_node->GetRowStart() + payload_segment.count.load();
	REQUIRE(payload_segment_end > 0);
	REQUIRE(payload_segment_end < id_column.count.load());
	auto fetch_after_state_destruction = [&](const StorageIndex &column_id) {
		auto &column = row_group->GetNode().GetRawColumnData(column_id);
		auto segment_node = column.GetSegmentTree().GetSegment(0);
		REQUIRE(segment_node);
		REQUIRE(segment_node->GetNode().count > 3);

		Vector detached_result(LogicalType::VARCHAR, 2);
		{
			ColumnFetchState detached_fetch_state;
			detached_fetch_state.context = *con.context;
			idx_t detached_offsets[] {2, 3};
			segment_node->GetNode().FetchRows(detached_fetch_state, detached_offsets, 2, detached_result, 0);
		}
		return vector<Value> {detached_result.GetValue(0), detached_result.GetValue(1)};
	};

	auto detached_payload = fetch_after_state_destruction(StorageIndex(1));
	REQUIRE(detached_payload[0].GetValue<string>() == StringUtil::Repeat("2_", 16));
	REQUIRE(detached_payload[1].GetValue<string>() == StringUtil::Repeat("3_", 16));
	auto detached_repeated = fetch_after_state_destruction(StorageIndex(2));
	REQUIRE(detached_repeated[0].GetValue<string>() ==
	        "duck:goose token token duck:goose000002duck:goose token-token duck:goose");
	REQUIRE(detached_repeated[1].GetValue<string>() ==
	        "duck:goose token token duck:goose000003duck:goose token-token duck:goose");
	auto detached_dictionary = fetch_after_state_destruction(StorageIndex(3));
	REQUIRE(detached_dictionary[0].GetValue<string>() == "C");
	REQUIRE(detached_dictionary[1].GetValue<string>() == "D");

	auto &lifetime_entry =
	    Catalog::GetEntry<TableCatalogEntry>(*con.context, QualifiedName(Identifier("fetch_dictionary_lifetime")));
	auto lifetime_row_group = lifetime_entry.GetStorage().GetRowGroupCollection()->GetRowGroups()->GetSegment(0);
	REQUIRE(lifetime_row_group);
	auto &lifetime_column = lifetime_row_group->GetNode().GetRawColumnData(StorageIndex(0));
	auto lifetime_segment_node = lifetime_column.GetSegmentTree().GetSegment(0);
	REQUIRE(lifetime_segment_node);
	auto &lifetime_segment = lifetime_segment_node->GetNode();
	REQUIRE(lifetime_segment.count > 3);
	{
		ColumnFetchState mode_fetch_state;
		mode_fetch_state.context = *con.context;
		dict_fsst::CompressedStringScanState mode_state(lifetime_segment,
		                                                mode_fetch_state.GetOrInsertHandle(lifetime_segment));
		mode_state.Initialize(false);
		REQUIRE(mode_state.mode == dict_fsst::DictFSSTMode::DICTIONARY);
	}
	Vector detached_long_dictionary(LogicalType::VARCHAR, 2);
	{
		ColumnFetchState detached_fetch_state;
		detached_fetch_state.context = *con.context;
		idx_t detached_offsets[] {2, 3};
		lifetime_segment.FetchRows(detached_fetch_state, detached_offsets, 2, detached_long_dictionary, 0);
	}
	REQUIRE(detached_long_dictionary.GetValue(0).GetValue<string>() == StringUtil::Repeat("C", 64));
	REQUIRE(detached_long_dictionary.GetValue(1).GetValue<string>() == StringUtil::Repeat("D", 64));

	const auto payload_segment_boundary = NumericCast<row_t>(payload_segment_end);
	check_rows({payload_segment_boundary, payload_segment_boundary - 1},
	           {payload_segment_boundary, payload_segment_boundary - 1});
	const idx_t payload_column_count = payload_column.count.load();
	REQUIRE(payload_segment_end + 3 < payload_column_count);
	idx_t second_segment_offsets[] {payload_segment_end + 2, payload_segment_end + 3};
	Vector second_segment_result(LogicalType::VARCHAR, 2);
	payload_column.FetchRowsAtSegmentLevel(transaction, segment_fetch_state, second_segment_offsets, 2,
	                                       second_segment_result, 0);
	for (idx_t idx = 0; idx < 2; idx++) {
		REQUIRE(second_segment_result.GetValue(idx).GetValue<string>() ==
		        StringUtil::Repeat(to_string(second_segment_offsets[idx]) + "_", 16));
	}
	auto payload_second_segment_node = payload_column.GetSegmentTree().GetSegment(payload_segment_end);
	REQUIRE(payload_second_segment_node);
	REQUIRE(payload_second_segment_node->GetRowStart() == payload_segment_end);
	REQUIRE(payload_second_segment_node->GetNode().count > 21);
	idx_t second_segment_scan_offsets[] {5, 6, 20, 21};
	Vector second_segment_scan_result(LogicalType::VARCHAR, 4);
	ColumnSegment::FetchRowsUsingScan(payload_second_segment_node->GetNode(), segment_fetch_state,
	                                  second_segment_scan_offsets, 4, second_segment_scan_result, 0);
	for (idx_t idx = 0; idx < 4; idx++) {
		const idx_t row_id = payload_segment_end + second_segment_scan_offsets[idx];
		REQUIRE(second_segment_scan_result.GetValue(idx).GetValue<string>() ==
		        StringUtil::Repeat(to_string(row_id) + "_", 16));
	}

	// An invalid offset after a valid segment run must still fail.
	REQUIRE(payload_segment_end < payload_column_count);
	idx_t late_invalid_offsets[] {0, payload_column_count};
	Vector late_invalid_result(LogicalType::VARCHAR, 2);
	late_invalid_result.SetValue(0, Value("left"));
	late_invalid_result.SetValue(1, Value("right"));
	REQUIRE_THROWS(payload_column.FetchRowsAtSegmentLevel(transaction, segment_fetch_state, late_invalid_offsets, 2,
	                                                      late_invalid_result, 0));
	REQUIRE(late_invalid_result.GetValue(0).GetValue<string>() == "left");
	REQUIRE(late_invalid_result.GetValue(1).GetValue<string>() == "right");

	idx_t cross_segment_offsets[] {0, payload_segment_end};
	Vector unchanged_tail(LogicalType::VARCHAR);
	unchanged_tail.SetValue(STANDARD_VECTOR_SIZE - 1, Value("unchanged"));
	REQUIRE_THROWS(payload_column.FetchRowsAtSegmentLevel(transaction, segment_fetch_state, cross_segment_offsets, 2,
	                                                      unchanged_tail, STANDARD_VECTOR_SIZE - 1));
	REQUIRE(unchanged_tail.GetValue(STANDARD_VECTOR_SIZE - 1).GetValue<string>() == "unchanged");

	REQUIRE_NO_FAIL(con.Query("DELETE FROM fetch_order WHERE id = 17000"));
	check_rows({17000}, {});
	check_rows({17000, 2}, {2});

	// A corrupt FSST_ONLY length must fail before reading beyond the dictionary.
	ColumnFetchState corrupt_fetch_state;
	corrupt_fetch_state.context = *con.context;
	dict_fsst::CompressedStringScanState corrupt_scan_state(payload_segment,
	                                                        corrupt_fetch_state.GetOrInsertHandle(payload_segment));
	corrupt_scan_state.Initialize(false);
	REQUIRE(corrupt_scan_state.mode == dict_fsst::DictFSSTMode::FSST_ONLY);
	REQUIRE(corrupt_scan_state.string_lengths.size() > 1);
	const auto original_length = corrupt_scan_state.string_lengths[1];
	corrupt_scan_state.string_lengths[1] = NumericCast<uint32_t>(corrupt_scan_state.dictionary_size + 1);
	idx_t corrupt_offsets[] {0, 1};
	Vector corrupt_result(LogicalType::VARCHAR);
	REQUIRE_THROWS(corrupt_scan_state.FetchRows(corrupt_result, 0, corrupt_offsets, 2));
	corrupt_scan_state.string_lengths[1] = original_length;

	Vector empty_result(LogicalType::BIGINT);
	segment.FetchRows(segment_fetch_state, nullptr, 0, empty_result, 0);
	REQUIRE_THROWS(segment.FetchRows(segment_fetch_state, nullptr, 1, empty_result, 0));
	vector<idx_t> oversized_offsets(STANDARD_VECTOR_SIZE + 1);
	for (idx_t idx = 0; idx < oversized_offsets.size(); idx++) {
		oversized_offsets[idx] = idx;
	}
	Vector oversized_result(LogicalType::BIGINT, oversized_offsets.size());
	segment.FetchRows(segment_fetch_state, oversized_offsets.data(), oversized_offsets.size(), oversized_result, 0);
	REQUIRE(oversized_result.GetValue(0).GetValue<int64_t>() == 0);
	REQUIRE(oversized_result.GetValue(STANDARD_VECTOR_SIZE).GetValue<int64_t>() == STANDARD_VECTOR_SIZE);
	idx_t valid_offsets[] {0, 1};
	REQUIRE_THROWS(segment.FetchRows(segment_fetch_state, valid_offsets, 2, empty_result, STANDARD_VECTOR_SIZE - 1));
	REQUIRE_THROWS(segment.FetchRows(segment_fetch_state, valid_offsets, 2, empty_result, STANDARD_VECTOR_SIZE + 1));
	REQUIRE_THROWS(
	    segment.FetchRows(segment_fetch_state, valid_offsets, 2, empty_result, NumericLimits<idx_t>::Maximum()));
	REQUIRE_THROWS(id_column.FetchRowsAtSegmentLevel(transaction, segment_fetch_state, oversized_offsets.data(),
	                                                 oversized_offsets.size(), empty_result, 0));
	const idx_t column_count = id_column.count.load();
	REQUIRE_THROWS(
	    id_column.FetchRowsAtSegmentLevel(transaction, segment_fetch_state, &column_count, 1, empty_result, 0));
	idx_t invalid_column_offsets[] {0, column_count};
	REQUIRE_THROWS(id_column.FetchRowsAtSegmentLevel(transaction, segment_fetch_state, invalid_column_offsets, 2,
	                                                 empty_result, 0));
	idx_t invalid_offset = segment_count;
	REQUIRE_THROWS(segment.FetchRows(segment_fetch_state, &invalid_offset, 1, empty_result, 0));
	idx_t invalid_offsets[] {0, segment_count};
	Vector unchanged_result(LogicalType::BIGINT, 2);
	unchanged_result.SetValue(0, Value::BIGINT(41));
	unchanged_result.SetValue(1, Value::BIGINT(42));
	REQUIRE_THROWS(segment.FetchRows(segment_fetch_state, invalid_offsets, 2, unchanged_result, 0));
	REQUIRE(unchanged_result.GetValue(0).GetValue<int64_t>() == 41);
	REQUIRE(unchanged_result.GetValue(1).GetValue<int64_t>() == 42);
	idx_t maximum_offset = NumericLimits<idx_t>::Maximum();
	REQUIRE_THROWS(segment.FetchRows(segment_fetch_state, &maximum_offset, 1, unchanged_result, 0));

	Vector constant_result(LogicalType::BIGINT, 2);
	constant_result.SetValue(0, Value::BIGINT(43));
	constant_result.SetVectorType(VectorType::CONSTANT_VECTOR);
	REQUIRE_THROWS(segment.FetchRows(segment_fetch_state, valid_offsets, 2, constant_result, 0));
	REQUIRE(constant_result.GetVectorType() == VectorType::CONSTANT_VECTOR);

	Vector dictionary_result(LogicalType::BIGINT, 2);
	SelectionVector dictionary_selection = SelectionVector::Incremental(2);
	dictionary_result.Slice(dictionary_selection, 2);
	REQUIRE(dictionary_result.GetVectorType() == VectorType::DICTIONARY_VECTOR);
	REQUIRE_THROWS(segment.FetchRows(segment_fetch_state, valid_offsets, 2, dictionary_result, 0));
	REQUIRE(dictionary_result.GetVectorType() == VectorType::DICTIONARY_VECTOR);
	REQUIRE_THROWS(segment.FetchRow(segment_fetch_state, NumericCast<row_t>(segment_count), empty_result, 0));
	REQUIRE_THROWS(segment.FetchRow(segment_fetch_state, -1, empty_result, 0));

	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
}

TEST_CASE("ZSTD batch fetch owns result strings", "[storage]") {
	auto database_path = TestCreatePath("zstd_batch_fetch.db");
	TestDeleteFile(database_path);
	{
		DuckDB db(database_path);
		Connection con(db);

		REQUIRE_NO_FAIL(con.Query("SET force_compression='zstd'"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE zstd_cursor (payload VARCHAR)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO zstd_cursor "
		                          "SELECT repeat(lpad(i::VARCHAR, 8, '0'), 8) FROM range(20000) t(i)"));
		REQUIRE_NO_FAIL(con.Query("FORCE CHECKPOINT"));
		auto compression =
		    con.Query("SELECT bool_and(lower(compression)='zstd') FROM pragma_storage_info('zstd_cursor') "
		              "WHERE column_name='payload' AND segment_type!='VALIDITY'");
		REQUIRE_NO_FAIL(*compression);
		REQUIRE(compression->GetValue(0, 0).GetValue<bool>());
		REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));

		auto &table_entry =
		    Catalog::GetEntry<TableCatalogEntry>(*con.context, QualifiedName(Identifier("zstd_cursor")));
		auto row_group = table_entry.GetStorage().GetRowGroupCollection()->GetRowGroups()->GetSegment(0);
		REQUIRE(row_group);
		auto &column = row_group->GetNode().GetRawColumnData(StorageIndex(0));
		auto segment_node = column.GetSegmentTree().GetSegment(0);
		REQUIRE(segment_node);
		REQUIRE(segment_node->GetRowStart() == 0);
		auto &segment = segment_node->GetNode();
		REQUIRE(segment.count > 2051);
		REQUIRE(segment.GetCompressionFunction().type == CompressionType::COMPRESSION_ZSTD);
		REQUIRE(segment.GetCompressionFunction().fetch_rows != nullptr);

		idx_t offsets[] {5, 6, 2047, 2048, 2050, 2051};
		Vector result(LogicalType::VARCHAR, 6);
		{
			ColumnFetchState fetch_state;
			fetch_state.context = *con.context;
			segment.FetchRows(fetch_state, offsets, 6, result, 0);
		}
		for (idx_t idx = 0; idx < 6; idx++) {
			REQUIRE(result.GetValue(idx).GetValue<string>() ==
			        StringUtil::Repeat(StringUtil::Format("%08d", offsets[idx]), 8));
		}

		Vector reused_result(LogicalType::VARCHAR, 2);
		FlatVector::ValidityMutable(reused_result).SetInvalid(0);
		FlatVector::ValidityMutable(reused_result).SetInvalid(1);
		idx_t reused_offsets[] {7, 8};
		{
			ColumnFetchState fetch_state;
			fetch_state.context = *con.context;
			segment.FetchRows(fetch_state, reused_offsets, 2, reused_result, 0);
		}
		FlatVector::ValidityMutable(reused_result).SetValid(0);
		FlatVector::ValidityMutable(reused_result).SetValid(1);
		REQUIRE(reused_result.GetValue(0).GetValue<string>() == StringUtil::Repeat("00000007", 8));
		REQUIRE(reused_result.GetValue(1).GetValue<string>() == StringUtil::Repeat("00000008", 8));

		vector<idx_t> oversized_offsets(STANDARD_VECTOR_SIZE + 1);
		for (idx_t idx = 0; idx < oversized_offsets.size(); idx++) {
			oversized_offsets[idx] = idx;
		}
		Vector oversized_result(LogicalType::VARCHAR, oversized_offsets.size());
		ColumnFetchState oversized_fetch_state;
		oversized_fetch_state.context = *con.context;
		segment.FetchRows(oversized_fetch_state, oversized_offsets.data(), oversized_offsets.size(), oversized_result,
		                  0);
		for (const idx_t idx : {idx_t(0), idx_t(STANDARD_VECTOR_SIZE - 1), idx_t(STANDARD_VECTOR_SIZE)}) {
			REQUIRE(oversized_result.GetValue(idx).GetValue<string>() ==
			        StringUtil::Repeat(StringUtil::Format("%08d", idx), 8));
		}

		REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	}
	TestDeleteFile(database_path);
}

TEST_CASE("ALP batch fetch handles selected and uncompressed vectors", "[storage]") {
	auto database_path = TestCreatePath("alp_selected_batch_fetch.db");
	TestDeleteFile(database_path);
	{
		DuckDB db(database_path);
		Connection con(db);

		REQUIRE_NO_FAIL(con.Query("SET force_compression='alp'"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE alp_modes ("
		                          "id BIGINT USING COMPRESSION BitPacking, payload DOUBLE)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO alp_modes "
		                          "SELECT i, hash(i)::BIT::DOUBLE FROM range(100000) t(i)"));
		REQUIRE_NO_FAIL(con.Query("FORCE CHECKPOINT"));

		REQUIRE_NO_FAIL(con.Query("SET force_compression='alprd'"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE alprd_modes ("
		                          "id BIGINT USING COMPRESSION BitPacking, payload DOUBLE)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO alprd_modes "
		                          "SELECT i, hash(i)::BIT::DOUBLE FROM range(100000) t(i)"));
		REQUIRE_NO_FAIL(con.Query("FORCE CHECKPOINT"));

		auto compression =
		    con.Query("SELECT (SELECT bool_and(lower(compression)='alp') FROM pragma_storage_info('alp_modes') "
		              "        WHERE column_name='payload' AND segment_type!='VALIDITY'), "
		              "       (SELECT bool_and(lower(compression)='alprd') FROM pragma_storage_info('alprd_modes') "
		              "        WHERE column_name='payload' AND segment_type!='VALIDITY')");
		REQUIRE_NO_FAIL(*compression);
		REQUIRE(compression->GetValue(0, 0).GetValue<bool>());
		REQUIRE(compression->GetValue(1, 0).GetValue<bool>());
		REQUIRE_NO_FAIL(con.Query("BEGIN TRANSACTION"));

		auto check_table = [&](const string &table_name, bool alprd) {
			auto &table_entry =
			    Catalog::GetEntry<TableCatalogEntry>(*con.context, QualifiedName(Identifier(table_name)));
			auto &storage = table_entry.GetStorage();
			auto row_group = storage.GetRowGroupCollection()->GetRowGroups()->GetSegment(0);
			REQUIRE(row_group);
			auto &payload_column = row_group->GetNode().GetRawColumnData(StorageIndex(1));
			auto segment_node = payload_column.GetSegmentTree().GetSegment(0);
			REQUIRE(segment_node);
			REQUIRE(segment_node->GetRowStart() == 0);
			auto &segment = segment_node->GetNode();
			const idx_t segment_count = segment.count.load();
			REQUIRE(segment_count > AlpConstants::ALP_VECTOR_SIZE);

			bool found_uncompressed_vector = false;
			if (alprd) {
				AlpRDScanState<double> scan_state(segment);
				while (scan_state.total_value_count < scan_state.count) {
					const idx_t vector_size = scan_state.LoadVectorData();
					found_uncompressed_vector |= scan_state.vector_state.uncompressed_mode;
					scan_state.total_value_count += vector_size;
				}
			} else {
				AlpScanState<double> scan_state(segment);
				while (scan_state.total_value_count < scan_state.count) {
					const idx_t vector_size = scan_state.LoadVectorData();
					found_uncompressed_vector |= scan_state.vector_state.uncompressed_mode;
					scan_state.total_value_count += vector_size;
				}
			}
			REQUIRE(found_uncompressed_vector);

			vector<idx_t> offsets {0, 2, 31, 32, 1023, 1024, segment_count - 1};
			string id_list;
			for (idx_t i = 0; i < offsets.size(); i++) {
				if (i > 0) {
					id_list += ',';
				}
				id_list += to_string(offsets[i]);
			}
			auto expected = con.Query("SELECT payload::BIT::UBIGINT FROM " + table_name + " WHERE id IN (" + id_list +
			                          ") ORDER BY id");
			REQUIRE_NO_FAIL(*expected);
			REQUIRE(expected->RowCount() == offsets.size());

			ColumnFetchState fetch_state;
			fetch_state.context = *con.context;
			idx_t fetched = 0;
			while (fetched < offsets.size()) {
				const idx_t batch_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, offsets.size() - fetched);
				const idx_t result_offset = STANDARD_VECTOR_SIZE - batch_count;
				Vector result(LogicalType::DOUBLE);
				segment.FetchRows(fetch_state, offsets.data() + fetched, batch_count, result, result_offset);

				auto result_data = FlatVector::GetData<double>(result);
				for (idx_t i = 0; i < batch_count; i++) {
					uint64_t result_bits;
					memcpy(&result_bits, result_data + result_offset + i, sizeof(result_bits));
					REQUIRE(result_bits == expected->GetValue(0, fetched + i).GetValue<uint64_t>());
				}
				fetched += batch_count;
			}

			const idx_t oversized_count = STANDARD_VECTOR_SIZE + 1;
			vector<idx_t> oversized_offsets(oversized_count);
			for (idx_t idx = 0; idx < oversized_count; idx++) {
				oversized_offsets[idx] = idx * 2;
			}
			REQUIRE(oversized_offsets.back() < segment_count);
			auto oversized_expected = con.Query("SELECT payload::BIT::UBIGINT FROM " + table_name +
			                                    " WHERE id % 2 = 0 ORDER BY id LIMIT " + to_string(oversized_count));
			REQUIRE_NO_FAIL(*oversized_expected);
			REQUIRE(oversized_expected->RowCount() == oversized_count);
			Vector oversized_result(LogicalType::DOUBLE, oversized_count);
			segment.FetchRows(fetch_state, oversized_offsets.data(), oversized_count, oversized_result, 0);
			auto oversized_result_data = FlatVector::GetData<double>(oversized_result);
			for (idx_t idx = 0; idx < oversized_count; idx++) {
				uint64_t result_bits;
				memcpy(&result_bits, oversized_result_data + idx, sizeof(result_bits));
				REQUIRE(result_bits == oversized_expected->GetValue(0, idx).GetValue<uint64_t>());
			}
		};

		check_table("alp_modes", false);
		check_table("alprd_modes", true);
		REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	}
	TestDeleteFile(database_path);
}

TEST_CASE("ALP selected decode matches full vector decode", "[storage]") {
	auto require_same_bits = [](const double *expected, const double *actual, const idx_t *offsets, idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			REQUIRE(memcmp(expected + offsets[i], actual + i, sizeof(double)) == 0);
		}
	};

	SECTION("ALP bit widths and exceptions") {
		AlpVectorState<double> state {};
		state.v_exponent = 0;
		state.v_factor = 0;
		state.frame_of_reference = 0;
		state.bit_width = 64;
		state.uncompressed_mode = false;

		uint64_t encoded[BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE];
		for (idx_t i = 0; i < BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE; i++) {
			encoded[i] = i == 31 ? NumericLimits<uint64_t>::Maximum() : i * 17;
		}
		BitpackingPrimitives::PackBuffer<uint64_t>(
		    state.for_encoded, encoded, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE, state.bit_width);
		state.exceptions_count = 3;
		state.exceptions_positions[0] = 31;
		state.exceptions_positions[1] = 1;
		state.exceptions_positions[2] = 31;
		state.exceptions[0] = std::numeric_limits<double>::infinity();
		state.exceptions[1] = -std::numeric_limits<double>::infinity();
		state.exceptions[2] = std::numeric_limits<double>::quiet_NaN();

		double full[BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE];
		state.LoadValues<false>(full, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE);
		idx_t local_offsets[] {0, 1, 7, 31};
		const idx_t vector_start = AlpConstants::ALP_VECTOR_SIZE * 2;
		idx_t offsets[] {vector_start, vector_start + 1, vector_start + 7, vector_start + 31};
		double selected[4];
		state.DecodeSelected(offsets, 4, vector_start, selected);
		require_same_bits(full, selected, local_offsets, 4);

		state.bit_width = 0;
		state.frame_of_reference = static_cast<uint64_t>(int64_t(-42));
		state.exceptions_count = 0;
		state.LoadValues<false>(full, 7);
		const idx_t constant_vector_start = AlpConstants::ALP_VECTOR_SIZE * 3;
		idx_t constant_offsets[] {constant_vector_start, constant_vector_start + 6};
		double constant_selected[2];
		state.DecodeSelected(constant_offsets, 2, constant_vector_start, constant_selected);
		idx_t constant_local_offsets[] {0, 6};
		require_same_bits(full, constant_selected, constant_local_offsets, 2);

		state.v_exponent = NumericCast<uint8_t>(sizeof(AlpTypedConstants<double>::FRAC_ARR) / sizeof(double));
		REQUIRE_THROWS(state.DecodeSelected(constant_offsets, 2, 3072, constant_selected));
	}

	SECTION("ALPRD bit widths, dictionary and exceptions") {
		AlpRDVectorState<double> state {};
		state.left_bit_width = 3;
		state.right_bit_width = 48;
		state.dictionary_size = 4;
		state.uncompressed_mode = false;
		state.left_parts_dict[0] = 0x3FF0;
		state.left_parts_dict[1] = 0xBFF0;
		state.left_parts_dict[2] = 0x4000;
		state.left_parts_dict[3] = 0xC000;

		uint16_t left_parts[BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE];
		uint64_t right_parts[BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE];
		for (idx_t i = 0; i < BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE; i++) {
			left_parts[i] = NumericCast<uint16_t>(i % state.dictionary_size);
			right_parts[i] = (i * 31337) & ((uint64_t(1) << state.right_bit_width) - 1);
		}
		left_parts[1] = state.dictionary_size;
		left_parts[31] = state.dictionary_size;
		BitpackingPrimitives::PackBuffer<uint16_t>(state.left_encoded, left_parts,
		                                           BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE,
		                                           state.left_bit_width);
		BitpackingPrimitives::PackBuffer<uint64_t>(state.right_encoded, right_parts,
		                                           BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE,
		                                           state.right_bit_width);
		state.exceptions_count = 3;
		state.exceptions_positions[0] = 31;
		state.exceptions_positions[1] = 1;
		state.exceptions_positions[2] = 31;
		state.exceptions[0] = 0x7FF0;
		state.exceptions[1] = 0xFFF0;
		state.exceptions[2] = 0x7FF8;

		uint64_t full[BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE];
		state.LoadValues<false>(full, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE);
		idx_t local_offsets[] {0, 1, 7, 31};
		const idx_t vector_start = AlpRDConstants::ALP_VECTOR_SIZE;
		idx_t offsets[] {vector_start, vector_start + 1, vector_start + 7, vector_start + 31};
		uint64_t selected[4];
		state.DecodeSelected(offsets, 4, vector_start, selected);
		for (idx_t i = 0; i < 4; i++) {
			REQUIRE(selected[i] == full[local_offsets[i]]);
		}

		state.exceptions_count = 0;
		state.dictionary_size = 1;
		state.left_bit_width = 0;
		state.right_bit_width = 64;
		state.left_parts_dict[0] = 0;
		for (idx_t i = 0; i < BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE; i++) {
			right_parts[i] = i == 31 ? NumericLimits<uint64_t>::Maximum() : i * 65537;
		}
		BitpackingPrimitives::PackBuffer<uint64_t>(state.right_encoded, right_parts,
		                                           BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE,
		                                           state.right_bit_width);
		idx_t full_width_offsets[] {0, 31};
		uint64_t full_width_selected[2];
		state.DecodeSelected(full_width_offsets, 2, 0, full_width_selected);
		REQUIRE(full_width_selected[0] == right_parts[0]);
		REQUIRE(full_width_selected[1] == right_parts[31]);

		state.left_parts_dict[0] = 1;
		REQUIRE_THROWS(state.DecodeSelected(full_width_offsets, 2, 0, full_width_selected));
	}

	SECTION("ALPRD validates dictionary indices") {
		AlpRDVectorState<double> state {};
		state.left_bit_width = 1;
		state.right_bit_width = 0;
		state.dictionary_size = 1;
		state.uncompressed_mode = false;
		uint16_t invalid_indices[BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE];
		std::fill_n(invalid_indices, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE, uint16_t(1));
		BitpackingPrimitives::PackBuffer<uint16_t>(state.left_encoded, invalid_indices,
		                                           BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE,
		                                           state.left_bit_width);
		idx_t offsets[] {0, 31};
		uint64_t selected[2];
		REQUIRE_THROWS(state.DecodeSelected(offsets, 2, 0, selected));

		state.left_bit_width = 0;
		state.dictionary_size = 0;
		REQUIRE_THROWS(state.DecodeSelected(offsets, 2, 0, selected));
		state.exceptions_count = 2;
		state.exceptions_positions[0] = 31;
		state.exceptions_positions[1] = 0;
		state.exceptions[0] = 11;
		state.exceptions[1] = 7;
		state.DecodeSelected(offsets, 2, 0, selected);
		REQUIRE(selected[0] == 7);
		REQUIRE(selected[1] == 11);
	}

	SECTION("ALPRD rejects too many exceptions") {
		AlpRDVectorState<double> state {};
		state.exceptions_count = 2;
		REQUIRE_THROWS(state.ValidateExceptionsCount(1));
		REQUIRE_NOTHROW(state.ValidateExceptionsCount(2));
	}
}
