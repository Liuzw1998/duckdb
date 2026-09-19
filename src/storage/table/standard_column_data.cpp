#include "duckdb/storage/table/standard_column_data.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/storage_compatibility.hpp"

namespace duckdb {

namespace {

struct MarkModifiedBlockIds : public BlockIdVisitor {
	void Visit(block_id_t block_id) override {
		block_manager->MarkBlockAsModified(block_id);
	}

	BlockManager *block_manager = nullptr;
};

struct AuxiliaryCheckpointResult {
	shared_ptr<ColumnData> column;
	optional<PersistentColumnData> descriptor;
};

static vector<Value> ReadPersistentDeltaValues(ColumnData &column) {
	vector<Value> result;
	result.reserve(column.count);
	for (idx_t offset = 0; offset < column.count; offset += STANDARD_VECTOR_SIZE) {
		idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, column.count - offset);
		Vector scan_vector(column.type);
		ColumnScanState scan_state(nullptr);
		column.ColumnData::InitializeScanWithOffset(scan_state, offset);
		column.ColumnData::Scan(TransactionData(0, VisibilityBound::AllCommitted()), offset / STANDARD_VECTOR_SIZE,
		                        scan_state, scan_vector, count);
		FlatVector::SetSize(scan_vector, count);
		scan_vector.Flatten();
		for (idx_t i = 0; i < count; i++) {
			if (!FlatVector::Validity(scan_vector).RowIsValid(i)) {
				throw SerializationException("Persistent delta auxiliary columns cannot contain NULL values");
			}
			switch (column.type.id()) {
			case LogicalTypeId::UBIGINT:
				result.push_back(Value::UBIGINT(FlatVector::GetData<uint64_t>(scan_vector)[i]));
				break;
			case LogicalTypeId::INTEGER:
				result.push_back(Value::INTEGER(FlatVector::GetData<int32_t>(scan_vector)[i]));
				break;
			case LogicalTypeId::BIGINT:
				result.push_back(Value::BIGINT(FlatVector::GetData<int64_t>(scan_vector)[i]));
				break;
			case LogicalTypeId::BOOLEAN:
				result.push_back(Value::BOOLEAN(FlatVector::GetData<bool>(scan_vector)[i]));
				break;
			default:
				throw SerializationException("Unsupported persistent delta auxiliary type");
			}
		}
	}
	return result;
}

static PersistentColumnData MakePersistentBase(const LogicalType &type, ColumnData &value,
                                               ValidityColumnData &validity) {
	PersistentColumnData result(type, value.GetDataPointers());
	result.child_columns.emplace_back(LogicalType(LogicalTypeId::VALIDITY), validity.GetDataPointers());
	return result;
}

static AuxiliaryCheckpointResult CheckpointAuxiliaryColumn(StandardColumnData &owner, const RowGroup &row_group,
                                                           ColumnCheckpointInfo &checkpoint_info,
                                                           const LogicalType &type, const vector<Value> &values) {
	if (values.empty()) {
		return AuxiliaryCheckpointResult();
	}
	auto source = ColumnData::CreateColumn(owner.GetBlockManager(), owner.GetTableInfo(), owner.column_index, type,
	                                       ColumnDataType::TRANSACTION_LOCAL);
	checkpoint_info.GetPartialBlockManager().RegisterCheckpointColumnOwner(source);
	ColumnAppendState append_state;
	source->InitializeAppend(append_state);
	Vector append_vector(type, values.size());
	for (idx_t i = 0; i < values.size(); i++) {
		if (values[i].IsNull()) {
			throw SerializationException("Persistent delta auxiliary columns cannot contain NULL values");
		}
		append_vector.SetValue(i, values[i]);
	}
	source->Append(append_state, append_vector, values.size());
	source->FinalizeAppend(nullptr, append_state);
	auto state = source->Checkpoint(row_group, checkpoint_info);
	AuxiliaryCheckpointResult result;
	result.column = state->GetFinalResult();
	result.descriptor = state->ToPersistentData();
	return result;
}

static optional<PersistentColumnData> SerializeAuxiliaryColumn(const shared_ptr<ColumnData> &column) {
	if (!column) {
		return nullopt;
	}
	auto result = column->Serialize();
	if (result.persistent_updates) {
		throw InternalException("Persistent delta auxiliary columns cannot contain nested persistent updates");
	}
	return result;
}

static bool SupportsPersistentDeltaFormat(const StandardColumnData &column) {
	if (column.HasParent()) {
		return false;
	}
	if (column.type.id() != LogicalTypeId::INTEGER && column.type.id() != LogicalTypeId::BIGINT) {
		return false;
	}
	return !StorageManager::IsPriorToVersion(StorageVersion::V2_1_0, column.GetStorageManager().GetStorageVersion());
}

static bool SupportsPersistentDelta(const StandardColumnData &column, const ColumnCheckpointInfo &info) {
	if (!SupportsPersistentDeltaFormat(column)) {
		return false;
	}
	if (info.GetCheckpointType() != CheckpointType::FULL_CHECKPOINT ||
	    info.GetPartialBlockType() != PartialBlockType::FULL_CHECKPOINT ||
	    info.GetVisibilityBound() == VisibilityBound::IncludingUncommitted()) {
		return false;
	}
	return true;
}

static bool HasReliableBaseSize(ColumnData &column, idx_t &total_size) {
	if (!column.IsPersistent()) {
		return false;
	}
	total_size = 0;
	for (auto &pointer : column.GetDataPointers()) {
		if (!pointer.byte_size.has_value()) {
			return false;
		}
		total_size += *pointer.byte_size;
	}
	return total_size != 0;
}

static bool TryPreparePersistentDelta(StandardColumnData &column, CheckpointUpdateData &value_updates,
                                      CheckpointUpdateData &validity_updates) {
	idx_t value_size;
	idx_t validity_size;
	if (!HasReliableBaseSize(column, value_size) || !HasReliableBaseSize(column.GetValidityData(), validity_size)) {
		return false;
	}
	idx_t max_entries = MinValue<idx_t>(column.count / 100, 2048);
	if (max_entries == 0 || !column.ExportCheckpointUpdates(value_updates, max_entries) ||
	    !column.GetValidityData().ExportCheckpointUpdates(validity_updates, max_entries)) {
		return false;
	}
	if (value_updates.positions.size() + validity_updates.positions.size() > max_entries) {
		return false;
	}
	if ((value_updates.positions.size() + validity_updates.positions.size()) * 32 > (value_size + validity_size) / 4) {
		return false;
	}
	return !value_updates.positions.empty() || !validity_updates.positions.empty();
}

} // namespace

StandardColumnData::StandardColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                       LogicalType type, ColumnDataType data_type, optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, std::move(type), data_type, parent) {
	if (data_type != ColumnDataType::CHECKPOINT_TARGET) {
		// don't initialize the child entry if this is a checkpoint target
		validity = make_shared_ptr<ValidityColumnData>(block_manager, info, 0, *this);
	}
}

void StandardColumnData::SetDataType(ColumnDataType data_type) {
	ColumnData::SetDataType(data_type);
	validity->SetDataType(data_type);
}

ScanVectorType StandardColumnData::GetVectorScanType(ColumnScanState &state, idx_t scan_count, Vector &result) {
	// if either the current column data, or the validity column data requires flat vectors, we scan flat vectors
	auto scan_type = ColumnData::GetVectorScanType(state, scan_count, result);
	if (scan_type == ScanVectorType::SCAN_FLAT_VECTOR) {
		return ScanVectorType::SCAN_FLAT_VECTOR;
	}
	if (state.child_states.empty()) {
		return scan_type;
	}
	return validity->GetVectorScanType(state.child_states[0], scan_count, result);
}

void StandardColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t rows) {
	ColumnData::InitializePrefetch(prefetch_state, scan_state, rows);
	validity->InitializePrefetch(prefetch_state, scan_state.child_states[0], rows);
}

void StandardColumnData::InitializeScan(ColumnScanState &state) {
	ColumnData::InitializeScan(state);

	// initialize the validity segment
	D_ASSERT(state.child_states.size() == 1);
	validity->InitializeScan(state.child_states[0]);
}

void StandardColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	ColumnData::InitializeScanWithOffset(state, row_idx);

	// initialize the validity segment
	D_ASSERT(state.child_states.size() == 1);
	validity->InitializeScanWithOffset(state.child_states[0], row_idx);
}

idx_t StandardColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                               idx_t target_count) {
	D_ASSERT(state.offset_in_column == state.child_states[0].offset_in_column);
	auto scan_type = GetVectorScanType(state, target_count, result);
	auto scan_count =
	    ScanVector(transaction, vector_index, state, result, target_count, scan_type, state.update_scan_type);
	validity->ScanVector(transaction, vector_index, state.child_states[0], result, target_count, scan_type,
	                     state.update_scan_type);
	return scan_count;
}

idx_t StandardColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) {
	auto scan_count = ColumnData::ScanCount(state, result, count, result_offset);
	validity->ScanCount(state.child_states[0], result, count, result_offset);
	return scan_count;
}

void StandardColumnData::Filter(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                                SelectionVector &sel, idx_t &count, const TableFilter &filter,
                                TableFilterState &filter_state) {
	// check if we can do a specialized select
	// the compression functions need to support this
	auto compression = GetCompressionFunction();
	bool has_filter = compression && compression->filter;
	bool filter_includes_validity = compression && compression->validity == CompressionValidity::NO_VALIDITY_REQUIRED;
	auto validity_compression = validity->GetCompressionFunction();
	bool validity_has_filter = filter_includes_validity || (validity_compression && validity_compression->filter);
	auto target_count = GetVectorCount(vector_index);
	auto scan_type = GetVectorScanType(state, target_count, result);
	bool scan_entire_vector = scan_type == ScanVectorType::SCAN_ENTIRE_VECTOR;
	bool verify_fetch_row = state.scan_options && state.scan_options->force_fetch_row;
	if (!has_filter || !validity_has_filter || !scan_entire_vector || verify_fetch_row || filter_state.can_throw) {
		// we are not scanning an entire vector - this can have several causes (updates, etc)
		// a filter that can throw is excluded as well: the compression-level filters evaluate it over the distinct
		// values of the segment (e.g. the dictionary, or the RLE runs), which includes values of rows that another
		// filter already removed - pushing the filter down there must not raise errors that would not occur otherwise
		ColumnData::Filter(transaction, vector_index, state, result, sel, count, filter, filter_state);
		return;
	}
	FilterVector(state, result, target_count, sel, count, filter, filter_state);
	if (!filter_includes_validity) {
		validity->FilterVector(state.child_states[0], result, target_count, sel, count, filter, filter_state);
	} else {
		validity->Skip(state.child_states[0], target_count);
	}
}

void StandardColumnData::Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                                SelectionVector &sel, idx_t sel_count) {
	// check if we can do a specialized select
	// the compression functions need to support this
	auto compression = GetCompressionFunction();
	bool has_select = compression && compression->select;
	auto validity_compression = validity->GetCompressionFunction();
	bool validity_has_select = validity_compression && validity_compression->select;
	auto target_count = GetVectorCount(vector_index);
	auto scan_type = GetVectorScanType(state, target_count, result);
	bool scan_entire_vector = scan_type == ScanVectorType::SCAN_ENTIRE_VECTOR;
	if (!has_select || !validity_has_select || !scan_entire_vector) {
		// we are not scanning an entire vector - this can have several causes (updates, etc)
		ColumnData::Select(transaction, vector_index, state, result, sel, sel_count);
		return;
	}
	SelectVector(state, result, target_count, sel, sel_count);
	validity->SelectVector(state.child_states[0], result, target_count, sel, sel_count);
}

void StandardColumnData::InitializeAppend(ColumnAppendState &state) {
	ColumnData::InitializeAppend(state);
	ColumnAppendState child_append;
	validity->InitializeAppend(child_append);
	state.child_appends.push_back(std::move(child_append));
}

void StandardColumnData::AppendData(ColumnAppendState &state, UnifiedVectorFormat &vdata, idx_t count) {
	ColumnData::AppendData(state, vdata, count);
	validity->AppendData(state.child_appends[0], vdata, count);
}

void StandardColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	ColumnData::FinalizeAppend(finalize_state, state);
	validity->FinalizeAppendLocked(finalize_state, state.child_appends[0]);
}

void StandardColumnData::RevertAppend(row_t new_count) {
	ColumnData::RevertAppend(new_count);
	validity->RevertAppend(new_count);
}

idx_t StandardColumnData::Fetch(ColumnScanState &state, row_t row_id, Vector &result) {
	// fetch validity mask
	if (state.child_states.empty()) {
		ColumnScanState child_state(state.parent);
		child_state.scan_options = state.scan_options;
		state.child_states.push_back(std::move(child_state));
	}
	auto scan_count = ColumnData::Fetch(state, row_id, result);
	validity->Fetch(state.child_states[0], row_id, result);
	return scan_count;
}

void StandardColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                                Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	ColumnScanState standard_state(nullptr);
	ColumnScanState validity_state(nullptr);
	Vector base_vector(type);

	FetchUpdateData(standard_state, row_ids, base_vector, row_group_start);
	validity->FetchUpdateData(validity_state, row_ids, base_vector, row_group_start);

	UpdateInternal(transaction, table_entry, column_index, update_vector, row_ids, update_count, base_vector,
	               row_group_start);
	validity->UpdateInternal(transaction, table_entry, column_index, update_vector, row_ids, update_count, base_vector,
	                         row_group_start);
}

void StandardColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                      const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                      idx_t update_count, idx_t depth, idx_t row_group_start) {
	if (depth >= column_path.size()) {
		// update this column
		ColumnData::Update(transaction, table_entry, column_path[0], update_vector, row_ids, update_count,
		                   row_group_start);
	} else {
		// update the child column (i.e. the validity column)
		validity->UpdateColumn(transaction, table_entry, column_path, update_vector, row_ids, update_count, depth + 1,
		                       row_group_start);
		validity->UpdateWithBase(transaction, table_entry, column_path[0], update_vector, row_ids, update_count, *this,
		                         row_group_start);
	}
}

unique_ptr<BaseStatistics> StandardColumnData::GetUpdateStatistics() {
	unique_ptr<BaseStatistics> stats;
	{
		lock_guard<mutex> update_guard(update_lock);
		stats = updates ? updates->GetStatistics() : nullptr;
	}
	auto validity_stats = validity->GetUpdateStatistics();
	if (!stats && !validity_stats) {
		return nullptr;
	}
	if (!stats) {
		stats = BaseStatistics::CreateEmpty(type).ToUnique();
	}
	if (validity_stats) {
		stats->Merge(*validity_stats);
	}
	return stats;
}

void StandardColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state,
                                   const StorageIndex &storage_index, const idx_t *offsets, const SelectionVector &sel,
                                   idx_t fetch_count, Vector &result, idx_t result_offset) {
	if (state.child_states.empty()) {
		state.child_states.emplace_back(make_uniq<ColumnFetchState>());
	}
	// Bulk fetch the data and the validity in two passes.
	FetchRowsAtSegmentLevel(transaction, state, offsets, sel, fetch_count, result, result_offset);
	validity->FetchRowsAtSegmentLevel(transaction, *state.child_states[0], offsets, sel, fetch_count, result,
	                                  result_offset);
}

void StandardColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	ColumnData::VisitBlockIds(visitor);
	validity->VisitBlockIds(visitor);
	if (persistent_updates) {
		if (persistent_updates->value_positions) {
			persistent_updates->value_positions->VisitBlockIds(visitor);
		}
		if (persistent_updates->value_data) {
			persistent_updates->value_data->VisitBlockIds(visitor);
		}
		if (persistent_updates->validity_positions) {
			persistent_updates->validity_positions->VisitBlockIds(visitor);
		}
		if (persistent_updates->validity_data) {
			persistent_updates->validity_data->VisitBlockIds(visitor);
		}
	}
}

void StandardColumnData::VisitPersistentDeltaBlockIds(BlockIdVisitor &visitor) const {
	if (persistent_update_snapshot) {
		persistent_update_snapshot->VisitBlockIds(visitor);
		return;
	}
	VisitPersistentValueDeltaBlockIds(visitor);
	VisitPersistentValidityDeltaBlockIds(visitor);
}

void StandardColumnData::VisitPersistentValueDeltaBlockIds(BlockIdVisitor &visitor) const {
	if (!persistent_updates) {
		return;
	}
	if (persistent_updates->value_positions) {
		persistent_updates->value_positions->VisitBlockIds(visitor);
	}
	if (persistent_updates->value_data) {
		persistent_updates->value_data->VisitBlockIds(visitor);
	}
}

void StandardColumnData::VisitPersistentValidityDeltaBlockIds(BlockIdVisitor &visitor) const {
	if (!persistent_updates) {
		return;
	}
	if (persistent_updates->validity_positions) {
		persistent_updates->validity_positions->VisitBlockIds(visitor);
	}
	if (persistent_updates->validity_data) {
		persistent_updates->validity_data->VisitBlockIds(visitor);
	}
}

void StandardColumnData::SetValidityData(shared_ptr<ValidityColumnData> validity_p) {
	if (validity) {
		throw InternalException("StandardColumnData::SetValidityData cannot be used to overwrite existing validity");
	}
	validity_p->SetParent(this);
	this->validity = std::move(validity_p);
}

ValidityColumnData &StandardColumnData::GetValidityData() {
	D_ASSERT(validity);
	return *validity;
}

struct StandardColumnCheckpointState : public ColumnCheckpointState {
	StandardColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data,
	                              PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, partial_block_manager) {
	}

	unique_ptr<ColumnCheckpointState> validity_state;

public:
	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<StandardColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                           original_column.column_index, original_column.type,
		                                           ColumnDataType::CHECKPOINT_TARGET, nullptr);
	}

	shared_ptr<ColumnData> GetFinalResult() override {
		if (result_column) {
			auto &column_data = result_column->Cast<StandardColumnData>();
			if (!column_data.HasValidityData()) {
				auto validity_child = validity_state->GetFinalResult();
				column_data.SetValidityData(shared_ptr_cast<ColumnData, ValidityColumnData>(std::move(validity_child)));
			}
		}
		return ColumnCheckpointState::GetFinalResult();
	}

	unique_ptr<BaseStatistics> GetStatistics() override {
		D_ASSERT(global_stats);
		global_stats->Merge(*validity_state->GetStatistics());
		return std::move(global_stats);
	}

	PersistentColumnData ToPersistentData() override {
		auto data = ColumnCheckpointState::ToPersistentData();
		data.child_columns.push_back(validity_state->ToPersistentData());
		auto serialize_updates = [&](const shared_ptr<PersistentUpdateColumns> &source_updates) {
			data.persistent_updates = make_uniq<PersistentUpdateData>();
			data.persistent_updates->value_positions = SerializeAuxiliaryColumn(source_updates->value_positions);
			data.persistent_updates->value_data = SerializeAuxiliaryColumn(source_updates->value_data);
			data.persistent_updates->validity_positions = SerializeAuxiliaryColumn(source_updates->validity_positions);
			data.persistent_updates->validity_data = SerializeAuxiliaryColumn(source_updates->validity_data);
		};
		if (result_column) {
			auto &standard_source = result_column->Cast<StandardColumnData>();
			if (auto snapshot = standard_source.TakePersistentUpdateSnapshot()) {
				data.persistent_updates = std::move(snapshot);
			} else if (auto &source_updates = standard_source.GetPersistentUpdateColumns()) {
				serialize_updates(source_updates);
			}
		} else {
			auto &standard_source = original_column.Cast<StandardColumnData>();
			if (auto &source_updates = standard_source.GetPersistentUpdateColumns()) {
				serialize_updates(source_updates);
			}
		}
		return data;
	}
};

unique_ptr<ColumnCheckpointState>
StandardColumnData::CreateCheckpointState(const RowGroup &row_group, PartialBlockManager &partial_block_manager) {
	return make_uniq<StandardColumnCheckpointState>(row_group, *this, partial_block_manager);
}

unique_ptr<ColumnCheckpointState> StandardColumnData::Checkpoint(const RowGroup &row_group,
                                                                 ColumnCheckpointInfo &checkpoint_info,
                                                                 const BaseStatistics &stats) {
	if (persistent_updates && !HasUncheckpointedChanges() && SupportsPersistentDeltaFormat(*this)) {
		// Metadata may need to be rewritten while the complete clean snapshot remains reusable.
		auto base_state = CreateCheckpointState(row_group, checkpoint_info.GetPartialBlockManager());
		base_state->global_stats = GetStatistics();
		base_state->data_pointers = GetDataPointers();
		auto validity_state = validity->CreateCheckpointState(row_group, checkpoint_info.GetPartialBlockManager());
		validity_state->global_stats = BaseStatistics::CreateEmpty(validity->type).ToUnique();
		validity_state->data_pointers = validity->GetDataPointers();
		base_state->Cast<StandardColumnCheckpointState>().validity_state = std::move(validity_state);
		return base_state;
	}

	bool can_write_delta = SupportsPersistentDelta(*this, checkpoint_info);
	if (can_write_delta && (HasUncheckpointedUpdates() || validity->HasUncheckpointedUpdates())) {
		CheckpointUpdateData value_updates(type);
		CheckpointUpdateData validity_updates(LogicalType::BOOLEAN);
		can_write_delta = TryPreparePersistentDelta(*this, value_updates, validity_updates);
		if (can_write_delta) {
			vector<Value> value_positions;
			vector<Value> validity_positions;
			for (auto position : value_updates.positions) {
				value_positions.push_back(Value::UBIGINT(position));
			}
			for (auto position : validity_updates.positions) {
				validity_positions.push_back(Value::UBIGINT(position));
			}
			vector<Value> value_data = value_updates.values;
			vector<Value> validity_data = validity_updates.values;
			auto result_column = make_shared_ptr<StandardColumnData>(GetBlockManager(), GetTableInfo(), column_index,
			                                                         type, ColumnDataType::TRANSACTION_LOCAL, nullptr);
			auto base_data = MakePersistentBase(type, *this, *validity);
			result_column->ColumnData::InitializeColumn(base_data);
			result_column->RestoreCheckpointUpdates(value_updates);
			result_column->GetValidityData().RestoreCheckpointUpdates(validity_updates);
			if (auto update_stats = result_column->GetUpdateStatistics()) {
				result_column->MergeStatistics(*update_stats);
			}
			auto runtime_updates = make_shared_ptr<PersistentUpdateColumns>();
			AuxiliaryCheckpointResult value_positions_result;
			AuxiliaryCheckpointResult value_data_result;
			AuxiliaryCheckpointResult validity_positions_result;
			AuxiliaryCheckpointResult validity_data_result;
			if (persistent_updates && !HasUncheckpointedUpdates()) {
				runtime_updates->value_positions = persistent_updates->value_positions;
				runtime_updates->value_data = persistent_updates->value_data;
			} else {
				value_positions_result =
				    CheckpointAuxiliaryColumn(*this, row_group, checkpoint_info, LogicalType::UBIGINT, value_positions);
				value_data_result = CheckpointAuxiliaryColumn(*this, row_group, checkpoint_info, type, value_data);
				runtime_updates->value_positions = std::move(value_positions_result.column);
				runtime_updates->value_data = std::move(value_data_result.column);
			}
			if (persistent_updates && !validity->HasUncheckpointedUpdates()) {
				runtime_updates->validity_positions = persistent_updates->validity_positions;
				runtime_updates->validity_data = persistent_updates->validity_data;
			} else {
				validity_positions_result = CheckpointAuxiliaryColumn(*this, row_group, checkpoint_info,
				                                                      LogicalType::UBIGINT, validity_positions);
				validity_data_result =
				    CheckpointAuxiliaryColumn(*this, row_group, checkpoint_info, LogicalType::BOOLEAN, validity_data);
				runtime_updates->validity_positions = std::move(validity_positions_result.column);
				runtime_updates->validity_data = std::move(validity_data_result.column);
			}
			result_column->SetPersistentUpdateColumns(runtime_updates);
			auto persistent_update_snapshot = make_uniq<PersistentUpdateData>();
			if (persistent_updates && !HasUncheckpointedUpdates()) {
				persistent_update_snapshot->value_positions =
				    SerializeAuxiliaryColumn(runtime_updates->value_positions);
				persistent_update_snapshot->value_data = SerializeAuxiliaryColumn(runtime_updates->value_data);
			} else {
				persistent_update_snapshot->value_positions = std::move(value_positions_result.descriptor);
				persistent_update_snapshot->value_data = std::move(value_data_result.descriptor);
			}
			if (persistent_updates && !validity->HasUncheckpointedUpdates()) {
				persistent_update_snapshot->validity_positions =
				    SerializeAuxiliaryColumn(runtime_updates->validity_positions);
				persistent_update_snapshot->validity_data = SerializeAuxiliaryColumn(runtime_updates->validity_data);
			} else {
				persistent_update_snapshot->validity_positions = std::move(validity_positions_result.descriptor);
				persistent_update_snapshot->validity_data = std::move(validity_data_result.descriptor);
			}
			persistent_update_snapshot->ValidateDescriptorTypes(type);
			result_column->SetPersistentUpdateSnapshot(std::move(persistent_update_snapshot));
			MarkModifiedBlockIds old_delta_blocks;
			old_delta_blocks.block_manager = &GetBlockManager();
			if (persistent_updates && HasUncheckpointedUpdates()) {
				VisitPersistentValueDeltaBlockIds(old_delta_blocks);
			}
			if (persistent_updates && validity->HasUncheckpointedUpdates()) {
				VisitPersistentValidityDeltaBlockIds(old_delta_blocks);
			}

			auto base_state = CreateCheckpointState(row_group, checkpoint_info.GetPartialBlockManager());
			base_state->SetResultColumn(result_column);
			base_state->global_stats = result_column->GetStatistics();
			base_state->data_pointers = result_column->GetDataPointers();
			auto validity_state = validity->CreateCheckpointState(row_group, checkpoint_info.GetPartialBlockManager());
			validity_state->global_stats = BaseStatistics::CreateEmpty(validity->type).ToUnique();
			validity_state->data_pointers = result_column->GetValidityData().GetDataPointers();
			base_state->Cast<StandardColumnCheckpointState>().validity_state = std::move(validity_state);
			return base_state;
		}
	}

	// we need to checkpoint the main column data first
	// that is because the checkpointing of the main column data ALSO scans the validity data
	// to prevent reading the validity data immediately after it is checkpointed we first checkpoint the main column
	// this is necessary for concurrent checkpointing as due to the partial block manager checkpointed data might be
	// flushed to disk by a different thread than the one that wrote it, causing a data race
	auto &partial_block_manager = checkpoint_info.GetPartialBlockManager();
	auto base_state = CreateCheckpointState(row_group, partial_block_manager);
	base_state->global_stats = BaseStatistics::CreateEmpty(type).ToUnique();
	auto validity_state_p = validity->CreateCheckpointState(row_group, partial_block_manager);
	validity_state_p->global_stats = BaseStatistics::CreateEmpty(validity->type).ToUnique();

	auto &validity_state = *validity_state_p;
	auto &checkpoint_state = base_state->Cast<StandardColumnCheckpointState>();
	checkpoint_state.validity_state = std::move(validity_state_p);

	if (!data.GetRootSegment()) {
		// empty table: flush the empty list
		return base_state;
	}

	vector<reference<ColumnCheckpointState>> checkpoint_states;
	checkpoint_states.emplace_back(checkpoint_state);
	checkpoint_states.emplace_back(validity_state);

	ColumnDataCheckpointer checkpointer(checkpoint_states, GetStorageManager(), row_group, checkpoint_info);
	checkpointer.Checkpoint();
	checkpointer.FinalizeCheckpoint();

	// merge validity stats into base stats
	base_state->global_stats->Merge(*validity_state.global_stats);

	return base_state;
}

void StandardColumnData::CheckpointScan(ColumnSegment &segment, ColumnScanState &state, idx_t count,
                                        Vector &scan_vector) const {
	ColumnData::CheckpointScan(segment, state, count, scan_vector);

	idx_t offset_in_row_group = state.offset_in_column;
	validity->ScanCommittedRange(0, offset_in_row_group, count, scan_vector);
}

bool StandardColumnData::IsPersistent() {
	return ColumnData::IsPersistent() && validity->IsPersistent();
}

bool StandardColumnData::HasAnyChanges() const {
	return ColumnData::HasAnyChanges() || validity->HasAnyChanges();
}

bool StandardColumnData::HasUncheckpointedChanges() const {
	if (!persistent_updates) {
		return HasAnyChanges();
	}
	for (auto &segment : data.SegmentNodes()) {
		if (segment.GetNode().GetSegmentType() == ColumnSegmentType::TRANSIENT) {
			return true;
		}
	}
	for (auto &segment : validity->data.SegmentNodes()) {
		if (segment.GetNode().GetSegmentType() == ColumnSegmentType::TRANSIENT) {
			return true;
		}
	}
	return HasUncheckpointedUpdates() || validity->HasUncheckpointedUpdates();
}

PersistentColumnData StandardColumnData::Serialize() {
	if (!persistent_updates) {
		auto persistent_data = ColumnData::Serialize();
		persistent_data.child_columns.push_back(validity->Serialize());
		return persistent_data;
	}
	auto persistent_data = PersistentColumnData(type, GetDataPointers());
	persistent_data.has_updates = HasUncheckpointedUpdates() || validity->HasUncheckpointedUpdates();
	persistent_data.child_columns.emplace_back(LogicalType(LogicalTypeId::VALIDITY), validity->GetDataPointers());
	if (persistent_update_snapshot) {
		persistent_data.persistent_updates = std::move(persistent_update_snapshot);
	} else {
		persistent_data.persistent_updates = make_uniq<PersistentUpdateData>();
		persistent_data.persistent_updates->value_positions =
		    SerializeAuxiliaryColumn(persistent_updates->value_positions);
		persistent_data.persistent_updates->value_data = SerializeAuxiliaryColumn(persistent_updates->value_data);
		persistent_data.persistent_updates->validity_positions =
		    SerializeAuxiliaryColumn(persistent_updates->validity_positions);
		persistent_data.persistent_updates->validity_data = SerializeAuxiliaryColumn(persistent_updates->validity_data);
	}
	return persistent_data;
}

void StandardColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	ColumnData::InitializeColumn(column_data, target_stats);
	validity->InitializeColumn(column_data.child_columns[0], target_stats);
	if (!column_data.persistent_updates) {
		return;
	}
	column_data.persistent_updates->ValidateDescriptorTypes(type);
	auto runtime_updates = make_shared_ptr<PersistentUpdateColumns>();
	auto load = [&](optional<PersistentColumnData> &descriptor, const LogicalType &type) -> shared_ptr<ColumnData> {
		if (!descriptor) {
			return nullptr;
		}
		auto result = ColumnData::CreateColumn(GetBlockManager(), GetTableInfo(), column_index, type,
		                                       ColumnDataType::TRANSACTION_LOCAL);
		result->InitializeColumn(*descriptor);
		return result;
	};
	runtime_updates->value_positions = load(column_data.persistent_updates->value_positions, LogicalType::UBIGINT);
	runtime_updates->value_data = load(column_data.persistent_updates->value_data, type);
	runtime_updates->validity_positions =
	    load(column_data.persistent_updates->validity_positions, LogicalType::UBIGINT);
	runtime_updates->validity_data = load(column_data.persistent_updates->validity_data, LogicalType::BOOLEAN);
	if (runtime_updates->HasValueDelta()) {
		auto positions = ReadPersistentDeltaValues(*runtime_updates->value_positions);
		auto values = ReadPersistentDeltaValues(*runtime_updates->value_data);
		if (positions.size() != values.size()) {
			throw SerializationException("Persistent value delta streams have different lengths");
		}
		CheckpointUpdateData snapshot(type);
		for (auto &position : positions) {
			snapshot.positions.push_back(position.GetValue<uint64_t>());
		}
		snapshot.values = std::move(values);
		snapshot.Validate(count);
		RestoreCheckpointUpdates(snapshot);
	}
	if (runtime_updates->HasValidityDelta()) {
		auto positions = ReadPersistentDeltaValues(*runtime_updates->validity_positions);
		auto values = ReadPersistentDeltaValues(*runtime_updates->validity_data);
		if (positions.size() != values.size()) {
			throw SerializationException("Persistent validity delta streams have different lengths");
		}
		CheckpointUpdateData snapshot(LogicalType::BOOLEAN);
		for (auto &position : positions) {
			snapshot.positions.push_back(position.GetValue<uint64_t>());
		}
		snapshot.values = std::move(values);
		snapshot.Validate(count);
		validity->RestoreCheckpointUpdates(snapshot);
	}
	persistent_updates = std::move(runtime_updates);
	if (auto update_stats = GetUpdateStatistics()) {
		MergeStatistics(*update_stats);
	}
}

void StandardColumnData::SetPersistentUpdateColumns(shared_ptr<PersistentUpdateColumns> updates) {
	persistent_updates = std::move(updates);
}

void StandardColumnData::SetPersistentUpdateSnapshot(unique_ptr<PersistentUpdateData> snapshot) {
	persistent_update_snapshot = std::move(snapshot);
}

unique_ptr<PersistentUpdateData> StandardColumnData::TakePersistentUpdateSnapshot() {
	return std::move(persistent_update_snapshot);
}

void StandardColumnData::GetColumnSegmentInfo(const QueryContext &context, duckdb::idx_t row_group_index,
                                              vector<duckdb::idx_t> col_path, vector<duckdb::ColumnSegmentInfo> &result,
                                              const ColumnSegmentInfoScanOptions &options) {
	ColumnData::GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	col_path.push_back(0);
	validity->GetColumnSegmentInfo(context, row_group_index, std::move(col_path), result, options);
}

void StandardColumnData::Verify(RowGroup &parent) {
#ifdef DEBUG
	ColumnData::Verify(parent);
	validity->Verify(parent);
#endif
}

} // namespace duckdb
