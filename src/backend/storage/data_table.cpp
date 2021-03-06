//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// data_table.cpp
//
// Identification: src/backend/storage/data_table.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <mutex>
#include <utility>

#include "backend/brain/clusterer.h"
#include "backend/storage/data_table.h"

#include "../benchmark/hyadapt/hyadapt_configuration.h"
#include "backend/storage/database.h"
#include "backend/common/exception.h"
#include "backend/common/logger.h"
#include "backend/index/index.h"
#include "backend/storage/tile_group.h"
#include "backend/storage/tuple.h"
#include "backend/storage/tile.h"
#include "backend/storage/tile_group_header.h"
#include "backend/storage/tile_group_factory.h"
#include "backend/concurrency/transaction_manager_factory.h"
#include "backend/concurrency/transaction_manager.h"

//===--------------------------------------------------------------------===//
// Configuration Variables
//===--------------------------------------------------------------------===//

std::vector<peloton::oid_t> hyadapt_column_ids;

double peloton_projectivity;

int peloton_num_groups;

bool peloton_fsm;

namespace peloton {
namespace storage {

/*
 * Constructor
 *
 * NOTE: In addition to standard data tile group, we also have a sampled tile
 * group which is used to hold materialized sampling result. The sampled tile
 * group OID is set to INVALID to avoid accidently usage.
 */
DataTable::DataTable(catalog::Schema *schema, const std::string &table_name,
                     const oid_t &database_oid, const oid_t &table_oid,
                     const size_t &tuples_per_tilegroup, const bool own_schema,
                     const bool adapt_table)
    : AbstractTable(database_oid, table_oid, table_name, schema, own_schema),
      tuples_per_tilegroup(tuples_per_tilegroup),
      sampled_tile_group_id{INVALID_OID},
      number_of_tuples{0.0},
      tuple_count_exact{0LU},
      adapt_table(adapt_table) {
  // Init default partition
  auto col_count = schema->GetColumnCount();
  // We only map inlined columns to sampling table
  oid_t sample_inline_column_it = 0;

  for (oid_t col_itr = 0; col_itr < col_count; col_itr++) {
    default_partition[col_itr] = std::make_pair(0, col_itr);

    ValueType column_type = schema->GetType(col_itr);
    // Only maps inlined columns (i.e. everything not a VARCHAR
    // or VARBINARY) into samples
    // NOTE: These two could potentially be inlined but we simply treat them
    // as always not inlined
    if(column_type != VALUE_TYPE_VARCHAR && \
       column_type != VALUE_TYPE_VARBINARY) {
      inline_column_map[col_itr] = sample_inline_column_it++;
      sample_column_mask.push_back(true);
    } else {
      LOG_INFO("Column is varchar or varbinary. Do not map into sample");

      // false means the column is not mapped into sample column
      sample_column_mask.push_back(false);
    }
  }

  // Build sample column map and sample schema
  // NOTE: These two are based on sample column layout, which is
  // different from data table layout
  BuildSampleSchema();

  // Create a tile group.
  AddDefaultTileGroup();
}

DataTable::~DataTable() {
  // clean up tile groups by dropping the references in the catalog
  oid_t tile_group_count = GetTileGroupCount();
  for (oid_t tile_group_itr = 0; tile_group_itr < tile_group_count;
       tile_group_itr++) {
    auto tile_group_id = tile_groups[tile_group_itr];
    catalog::Manager::GetInstance().DropTileGroup(tile_group_id);
  }

  // Also if there is a sampling tile group drop it
  if(sampled_tile_group_id != INVALID_OID) {
    catalog::Manager::GetInstance().DropTileGroup(sampled_tile_group_id);
  }

  // clean up indices
  for (auto index : indexes) {
    delete index;
  }

  // clean up foreign keys
  for (auto foreign_key : foreign_keys) {
    delete foreign_key;
  }

  // AbstractTable cleans up the schema
}

//===--------------------------------------------------------------------===//
// TUPLE HELPER OPERATIONS
//===--------------------------------------------------------------------===//

bool DataTable::CheckNulls(const storage::Tuple *tuple) const {
  assert(schema->GetColumnCount() == tuple->GetColumnCount());

  oid_t column_count = schema->GetColumnCount();
  for (oid_t column_itr = 0; column_itr < column_count; column_itr++) {
    if (tuple->IsNull(column_itr) && schema->AllowNull(column_itr) == false) {
      LOG_TRACE(
          "%lu th attribute in the tuple was NULL. It is non-nullable "
          "attribute.",
          column_itr);
      return false;
    }
  }

  return true;
}

bool DataTable::CheckConstraints(const storage::Tuple *tuple) const {
  // First, check NULL constraints
  if (CheckNulls(tuple) == false) {
    throw ConstraintException("Not NULL constraint violated : " +
                              std::string(tuple->GetInfo()));
    return false;
  }
  return true;
}

ItemPointer DataTable::GetTupleSlot(const storage::Tuple *tuple,
                                    bool check_constraint) {
  assert(tuple);
  if (check_constraint == true && CheckConstraints(tuple) == false) {
    return INVALID_ITEMPOINTER;
  }

  std::shared_ptr<storage::TileGroup> tile_group;
  oid_t tuple_slot = INVALID_OID;
  oid_t tile_group_offset = INVALID_OID;
  oid_t tile_group_id = INVALID_OID;

  while (tuple_slot == INVALID_OID) {
    // First, figure out last tile group
    {
      std::lock_guard<std::mutex> lock(table_mutex);
      assert(GetTileGroupCount() > 0);
      tile_group_offset = GetTileGroupCount() - 1;
      LOG_TRACE("Tile group offset :: %lu ", tile_group_offset);
    }

    // Then, try to grab a slot in the tile group header
    tile_group = GetTileGroup(tile_group_offset);

    tuple_slot = tile_group->InsertTuple(tuple);
    tile_group_id = tile_group->GetTileGroupId();

    if (tuple_slot == INVALID_OID) {
      // XXX Should we put this in a critical section?
      AddDefaultTileGroup();
    }
  }

  LOG_TRACE("tile group offset: %lu, tile group id: %lu, address: %p",
            tile_group_offset, tile_group->GetTileGroupId(), tile_group.get());

  // Set tuple location
  ItemPointer location(tile_group_id, tuple_slot);

  return location;
}

//===--------------------------------------------------------------------===//
// INSERT
//===--------------------------------------------------------------------===//
ItemPointer DataTable::InsertEmptyVersion(const storage::Tuple *tuple) {
  // First, do integrity checks and claim a slot
  ItemPointer location = GetTupleSlot(tuple, false);
  if (location.block == INVALID_OID) {
    LOG_WARN("Failed to get tuple slot.");
    return INVALID_ITEMPOINTER;
  }

  // Index checks and updates
  if (InsertInSecondaryIndexes(tuple, location) == false) {
    LOG_WARN("Index constraint violated");
    return INVALID_ITEMPOINTER;
  }

  LOG_TRACE("Location: %lu, %lu", location.block, location.offset);

  IncreaseNumberOfTuplesBy(1);
  tuple_count_exact++;

  return location;
}

ItemPointer DataTable::InsertVersion(const storage::Tuple *tuple) {
  // First, do integrity checks and claim a slot
  ItemPointer location = GetTupleSlot(tuple, true);
  if (location.block == INVALID_OID) {
    LOG_WARN("Failed to get tuple slot.");
    return INVALID_ITEMPOINTER;
  }

  // Index checks and updates
  if (InsertInSecondaryIndexes(tuple, location) == false) {
    LOG_WARN("Index constraint violated");
    return INVALID_ITEMPOINTER;
  }

  LOG_TRACE("Location: %lu, %lu", location.block, location.offset);

  IncreaseNumberOfTuplesBy(1);
  tuple_count_exact++;

  return location;
}

ItemPointer DataTable::InsertTuple(const storage::Tuple *tuple) {
  // First, do integrity checks and claim a slot
  ItemPointer location = GetTupleSlot(tuple);
  if (location.block == INVALID_OID) {
    LOG_WARN("Failed to get tuple slot.");
    return INVALID_ITEMPOINTER;
  }

  LOG_TRACE("Location: %lu, %lu", location.block, location.offset);

  // Index checks and updates
  if (InsertInIndexes(tuple, location) == false) {
    LOG_WARN("Index constraint violated");
    return INVALID_ITEMPOINTER;
  }

  // Increase the table's number of tuples by 1
  IncreaseNumberOfTuplesBy(1);
  // Also increase exact number of tuples
  tuple_count_exact++;

  // Increase the indexes' number of tuples by 1 as well
  for (auto index : indexes) index->IncreaseNumberOfTuplesBy(1);

  return location;
}

/**
 * @brief Insert a tuple into all indexes. If index is primary/unique,
 * check visibility of existing
 * index entries.
 * @warning This still doesn't guarantee serializability.
 *
 * @returns True on success, false if a visible entry exists (in case of
 *primary/unique).
 */
// TODO: this function MUST be rewritten!!! --Yingjun
bool DataTable::InsertInIndexes(const storage::Tuple *tuple,
                                ItemPointer location) {
  int index_count = GetIndexCount();

  // (A) Check existence for primary/unique indexes
  // FIXME Since this is NOT protected by a lock, concurrent insert may happen.
  for (int index_itr = index_count - 1; index_itr >= 0; --index_itr) {
    auto index = GetIndex(index_itr);
    auto index_schema = index->GetKeySchema();
    auto indexed_columns = index_schema->GetIndexedColumns();
    std::unique_ptr<storage::Tuple> key(new storage::Tuple(index_schema, true));
    key->SetFromTuple(tuple, indexed_columns, index->GetPool());

    switch (index->GetIndexType()) {
      case INDEX_CONSTRAINT_TYPE_PRIMARY_KEY:
      case INDEX_CONSTRAINT_TYPE_UNIQUE: {
        // TODO: get unique tuple from primary index.

        // auto locations = index->ScanKey(key.get());
        // auto exist_visible = ContainsVisibleEntry(locations, transaction);
        // if (exist_visible) {
        //   LOG_WARN("A visible index entry exists.");
        //   return false;
        // }
      } break;

      case INDEX_CONSTRAINT_TYPE_DEFAULT:
      default:
        break;
    }
    LOG_TRACE("Index constraint check on %s passed.", index->GetName().c_str());
  }

  // (B) Insert into index
  for (int index_itr = index_count - 1; index_itr >= 0; --index_itr) {
    auto index = GetIndex(index_itr);
    auto index_schema = index->GetKeySchema();
    auto indexed_columns = index_schema->GetIndexedColumns();
    std::unique_ptr<storage::Tuple> key(new storage::Tuple(index_schema, true));
    key->SetFromTuple(tuple, indexed_columns, index->GetPool());

    auto status = index->InsertEntry(key.get(), location);
    (void)status;
    assert(status);
  }

  return true;
}

bool DataTable::InsertInSecondaryIndexes(const storage::Tuple *tuple,
                                ItemPointer location) {
  int index_count = GetIndexCount();

  // (A) Check existence for primary/unique indexes
  // FIXME Since this is NOT protected by a lock, concurrent insert may happen.
  for (int index_itr = index_count - 1; index_itr >= 0; --index_itr) {
    auto index = GetIndex(index_itr);
    auto index_schema = index->GetKeySchema();
    auto indexed_columns = index_schema->GetIndexedColumns();
    std::unique_ptr<storage::Tuple> key(new storage::Tuple(index_schema, true));
    key->SetFromTuple(tuple, indexed_columns, index->GetPool());

    switch (index->GetIndexType()) {
      case INDEX_CONSTRAINT_TYPE_PRIMARY_KEY:
      case INDEX_CONSTRAINT_TYPE_UNIQUE: {
        // auto locations = index->ScanKey(key.get());
        // auto exist_visible = ContainsVisibleEntry(locations, transaction);
        // if (exist_visible) {
        //   LOG_WARN("A visible index entry exists.");
        //   return false;
        // }
      } break;

      case INDEX_CONSTRAINT_TYPE_DEFAULT:
      default:
        break;
    }
    LOG_TRACE("Index constraint check on %s passed.", index->GetName().c_str());
  }

  // (B) Insert into index
  for (int index_itr = index_count - 1; index_itr >= 0; --index_itr) {
    auto index = GetIndex(index_itr);
    auto index_schema = index->GetKeySchema();
    auto indexed_columns = index_schema->GetIndexedColumns();
    std::unique_ptr<storage::Tuple> key(new storage::Tuple(index_schema, true));
    key->SetFromTuple(tuple, indexed_columns, index->GetPool());

    switch (index->GetIndexType()) {
      case INDEX_CONSTRAINT_TYPE_PRIMARY_KEY:
      case INDEX_CONSTRAINT_TYPE_UNIQUE: {
        // auto locations = index->ScanKey(key.get());
        // auto exist_visible = ContainsVisibleEntry(locations, transaction);
        // if (exist_visible) {
        //   LOG_WARN("A visible index entry exists.");
        //   return false;
        // }
      } break;

      case INDEX_CONSTRAINT_TYPE_DEFAULT:
      default:
        auto status = index->InsertEntry(key.get(), location);
        (void)status;
        assert(status);
        break;
    }
  }
  return true;
}

//===--------------------------------------------------------------------===//
// STATS
//===--------------------------------------------------------------------===//

/**
 * @brief Increase the number of tuples in this table
 * @param amount amount to increase
 */
void DataTable::IncreaseNumberOfTuplesBy(const float &amount) {
  number_of_tuples += amount;
  dirty = true;
}

/**
 * @brief Decrease the number of tuples in this table
 * @param amount amount to decrease
 */
void DataTable::DecreaseNumberOfTuplesBy(const float &amount) {
  number_of_tuples -= amount;
  dirty = true;
}

/**
 * @brief Set the number of tuples in this table
 * @param num_tuples number of tuples
 */
void DataTable::SetNumberOfTuples(const float &num_tuples) {
  number_of_tuples = num_tuples;
  dirty = true;
}

/**
 * @brief Get the number of tuples in this table
 * @return number of tuples
 */
float DataTable::GetNumberOfTuples() const { return number_of_tuples; }

/**
 * @brief return dirty flag
 * @return dirty flag
 */
bool DataTable::IsDirty() const { return dirty; }

/**
 * @brief Reset dirty flag
 */
void DataTable::ResetDirty() { dirty = false; }

//===--------------------------------------------------------------------===//
// TILE GROUP
//===--------------------------------------------------------------------===//

/*
 * GetTileGroupWithLayout() - Return a tile group of some specified layout
 *
 * This function allocates memory for a tile group, which needs to be freed
 * implicitly by shared pointer when droping the tile group
 */
TileGroup *DataTable::GetTileGroupWithLayout(
    const column_map_type &partitioning) {
  std::vector<catalog::Schema> schema_list;
  oid_t tile_group_id = INVALID_OID;

  tile_group_id = catalog::Manager::GetInstance().GetNextOid();

  // Figure out the columns in each tile in new layout
  std::map<std::pair<oid_t, oid_t>, oid_t> tile_column_map;
  for (auto entry : partitioning) {
    // The ordering peoperty of std::map implicitly determines the order
    // of all elements inside: <partition #, column #>
    // When iterating through this map, we get the same partition
    // in a group, and within each partition the column ID is also ordered
    // Actual column ID are ordered such that they come one partition
    // at a time, and also come in column # order inside a partition
    tile_column_map[entry.second] = entry.first;
  }

  // Build the schema tile at a time
  // It maps partition ID to a list of columns
  std::map<oid_t, std::vector<catalog::Column>> tile_schemas;

  // entry comes in order: Same partition's column comes first
  // and also these columns are of the same order inside partition
  for (auto entry : tile_column_map) {
    tile_schemas[entry.first.first].push_back(schema->GetColumn(entry.second));
  }

  for (auto entry : tile_schemas) {
    catalog::Schema tile_schema(entry.second);
    schema_list.push_back(tile_schema);
  }

  TileGroup *tile_group = TileGroupFactory::GetTileGroup(
      database_oid,
      table_oid,
      tile_group_id,
      this,
      schema_list,
      partitioning,
      tuples_per_tilegroup);

  return tile_group;
}

column_map_type DataTable::GetTileGroupLayout(LayoutType layout_type) {
  column_map_type column_map;

  auto col_count = schema->GetColumnCount();
  if (adapt_table == false) layout_type = LAYOUT_ROW;

  // pure row layout map
  if (layout_type == LAYOUT_ROW) {
    for (oid_t col_itr = 0; col_itr < col_count; col_itr++) {
      column_map[col_itr] = std::make_pair(0, col_itr);
    }
  }
  // pure column layout map
  else if (layout_type == LAYOUT_COLUMN) {
    for (oid_t col_itr = 0; col_itr < col_count; col_itr++) {
      column_map[col_itr] = std::make_pair(col_itr, 0);
    }
  }
  // hybrid layout map
  else if (layout_type == LAYOUT_HYBRID) {
    // TODO: Fallback option for regular tables
    if (col_count < 10) {
      for (oid_t col_itr = 0; col_itr < col_count; col_itr++) {
        column_map[col_itr] = std::make_pair(0, col_itr);
      }
    } else {
      column_map = GetStaticColumnMap(table_name, col_count);
    }
  } else {
    throw Exception("Unknown tilegroup layout option : " +
                    std::to_string(layout_type));
  }

  return column_map;
}

oid_t DataTable::AddDefaultTileGroup() {
  column_map_type column_map;
  oid_t tile_group_id = INVALID_OID;

  // Figure out the partitioning for given tilegroup layout
  column_map = GetTileGroupLayout((LayoutType)peloton_layout_mode);

  // Create a tile group with that partitioning
  std::shared_ptr<TileGroup> tile_group(GetTileGroupWithLayout(column_map));
  assert(tile_group.get());
  tile_group_id = tile_group.get()->GetTileGroupId();

  LOG_TRACE("Trying to add a tile group ");
  {
    std::lock_guard<std::mutex> lock(table_mutex);

    // Check if we actually need to allocate a tile group

    // (A) no tile groups in table
    if (tile_groups.empty()) {
      LOG_TRACE("Added first tile group ");
      tile_groups.push_back(tile_group->GetTileGroupId());
      // add tile group metadata in locator
      catalog::Manager::GetInstance().AddTileGroup(tile_group_id, tile_group);
      LOG_TRACE("Recording tile group : %lu ", tile_group_id);
      return tile_group_id;
    }

    // (B) no slots in last tile group in table
    auto last_tile_group_offset = GetTileGroupCount() - 1;
    auto last_tile_group = GetTileGroup(last_tile_group_offset);

    oid_t active_tuple_count = last_tile_group->GetNextTupleSlot();
    oid_t allocated_tuple_count = last_tile_group->GetAllocatedTupleCount();
    if (active_tuple_count < allocated_tuple_count) {
      LOG_TRACE("Slot exists in last tile group :: %lu %lu ",
                active_tuple_count, allocated_tuple_count);
      return INVALID_OID;
    }

    LOG_TRACE("Added a tile group ");
    tile_groups.push_back(tile_group->GetTileGroupId());

    // add tile group metadata in locator
    catalog::Manager::GetInstance().AddTileGroup(tile_group_id, tile_group);
    LOG_TRACE("Recording tile group : %lu ", tile_group_id);
  }

  return tile_group_id;
}

oid_t DataTable::AddTileGroupWithOid(const oid_t &tile_group_id) {
  assert(tile_group_id);

  std::vector<catalog::Schema> schemas;
  schemas.push_back(*schema);

  column_map_type column_map;
  // default column map
  auto col_count = schema->GetColumnCount();
  for (oid_t col_itr = 0; col_itr < col_count; col_itr++) {
    column_map[col_itr] = std::make_pair(0, col_itr);
  }

  std::shared_ptr<TileGroup> tile_group(TileGroupFactory::GetTileGroup(
      database_oid, table_oid, tile_group_id, this, schemas, column_map,
      tuples_per_tilegroup));

  LOG_TRACE("Trying to add a tile group ");
  {
    std::lock_guard<std::mutex> lock(table_mutex);

    LOG_TRACE("Added a tile group ");
    tile_groups.push_back(tile_group->GetTileGroupId());

    // add tile group metadata in locator
    catalog::Manager::GetInstance().AddTileGroup(tile_group_id, tile_group);
    LOG_TRACE("Recording tile group : %lu ", tile_group_id);
  }

  return tile_group_id;
}

void DataTable::AddTileGroup(const std::shared_ptr<TileGroup> &tile_group) {
  {
    std::lock_guard<std::mutex> lock(table_mutex);

    tile_groups.push_back(tile_group->GetTileGroupId());
    oid_t tile_group_id = tile_group->GetTileGroupId();

    // add tile group in catalog
    catalog::Manager::GetInstance().AddTileGroup(tile_group_id, tile_group);
    LOG_TRACE("Recording tile group : %lu ", tile_group_id);
  }
}

size_t DataTable::GetTileGroupCount() const {
  size_t size = tile_groups.size();
  return size;
}

/*
 * GetTileGroup() - Get a pointer to TileGroup object by its offset
 *
 * This function calls GetTileGroupById() using the global ID
 */
std::shared_ptr<storage::TileGroup> DataTable::GetTileGroup(
    const oid_t &tile_group_offset) const {
  assert(tile_group_offset < GetTileGroupCount());

  auto tile_group_id = tile_groups[tile_group_offset];

  return GetTileGroupById(tile_group_id);
}

/*
 * GetTileGroupById() - Get a pointer to TileGroup object using the global ID
 *
 * This is a wrapper to catalog manager
 */
std::shared_ptr<storage::TileGroup> DataTable::GetTileGroupById(
    const oid_t &tile_group_id) const {
  auto &manager = catalog::Manager::GetInstance();

  return manager.GetTileGroup(tile_group_id);
}

const std::string DataTable::GetInfo() const {
  std::ostringstream os;

  os << "=====================================================\n";
  os << "TABLE :\n";

  oid_t tile_group_count = GetTileGroupCount();
  os << "Tile Group Count : " << tile_group_count << "\n";

  oid_t tuple_count = 0;
  for (oid_t tile_group_itr = 0; tile_group_itr < tile_group_count;
       tile_group_itr++) {
    auto tile_group = GetTileGroup(tile_group_itr);
    auto tile_tuple_count = tile_group->GetNextTupleSlot();

    os << "Tile Group Id  : " << tile_group_itr
       << " Tuple Count : " << tile_tuple_count << "\n";
    os << (*tile_group);

    tuple_count += tile_tuple_count;
  }

  os << "Table Tuple Count :: " << tuple_count << "\n";

  os << "=====================================================\n";

  return os.str();
}

//===--------------------------------------------------------------------===//
// INDEX
//===--------------------------------------------------------------------===//

void DataTable::AddIndex(index::Index *index) {
  {
    std::lock_guard<std::mutex> lock(table_mutex);
    indexes.push_back(index);
  }

  // Update index stats
  auto index_type = index->GetIndexType();
  if (index_type == INDEX_CONSTRAINT_TYPE_PRIMARY_KEY) {
    has_primary_key = true;
  } else if (index_type == INDEX_CONSTRAINT_TYPE_UNIQUE) {
    unique_constraint_count++;
  }
}

index::Index *DataTable::GetIndexWithOid(const oid_t &index_oid) const {
  for (auto index : indexes)
    if (index->GetOid() == index_oid) return index;

  return nullptr;
}

void DataTable::DropIndexWithOid(const oid_t &index_id) {
  {
    std::lock_guard<std::mutex> lock(table_mutex);

    oid_t index_offset = 0;
    for (auto index : indexes) {
      if (index->GetOid() == index_id) break;
      index_offset++;
    }
    assert(index_offset < indexes.size());

    // Drop the index
    indexes.erase(indexes.begin() + index_offset);
  }
}

index::Index *DataTable::GetIndex(const oid_t &index_offset) const {
  assert(index_offset < indexes.size());
  auto index = indexes.at(index_offset);
  return index;
}

oid_t DataTable::GetIndexCount() const { return indexes.size(); }

//===--------------------------------------------------------------------===//
// FOREIGN KEYS
//===--------------------------------------------------------------------===//

void DataTable::AddForeignKey(catalog::ForeignKey *key) {
  {
    std::lock_guard<std::mutex> lock(table_mutex);
    catalog::Schema *schema = this->GetSchema();
    catalog::Constraint constraint(CONSTRAINT_TYPE_FOREIGN,
                                   key->GetConstraintName());
    constraint.SetForeignKeyListOffset(GetForeignKeyCount());
    for (auto fk_column : key->GetFKColumnNames()) {
      schema->AddConstraint(fk_column, constraint);
    }
    // TODO :: We need this one..
    catalog::ForeignKey *fk = new catalog::ForeignKey(*key);
    foreign_keys.push_back(fk);
  }
}

catalog::ForeignKey *DataTable::GetForeignKey(const oid_t &key_offset) const {
  catalog::ForeignKey *key = nullptr;
  key = foreign_keys.at(key_offset);
  return key;
}

void DataTable::DropForeignKey(const oid_t &key_offset) {
  {
    std::lock_guard<std::mutex> lock(table_mutex);
    assert(key_offset < foreign_keys.size());
    foreign_keys.erase(foreign_keys.begin() + key_offset);
  }
}

oid_t DataTable::GetForeignKeyCount() const { return foreign_keys.size(); }

// Get the schema for the new transformed tile group
std::vector<catalog::Schema> TransformTileGroupSchema(
    storage::TileGroup *tile_group, const column_map_type &column_map) {
  std::vector<catalog::Schema> new_schema;
  oid_t orig_tile_offset, orig_tile_column_offset;
  oid_t new_tile_offset, new_tile_column_offset;

  // First, get info from the original tile group's schema
  std::map<oid_t, std::map<oid_t, catalog::Column>> schemas;
  auto orig_schemas = tile_group->GetTileSchemas();
  for (auto column_map_entry : column_map) {
    new_tile_offset = column_map_entry.second.first;
    new_tile_column_offset = column_map_entry.second.second;
    oid_t column_offset = column_map_entry.first;

    tile_group->LocateTileAndColumn(column_offset, orig_tile_offset,
                                    orig_tile_column_offset);

    // Get the column info from original schema
    auto orig_schema = orig_schemas[orig_tile_offset];
    auto column_info = orig_schema.GetColumn(orig_tile_column_offset);
    schemas[new_tile_offset][new_tile_column_offset] = column_info;
  }

  // Then, build the new schema
  for (auto schemas_tile_entry : schemas) {
    std::vector<catalog::Column> columns;
    for (auto schemas_column_entry : schemas_tile_entry.second)
      columns.push_back(schemas_column_entry.second);

    catalog::Schema tile_schema(columns);
    new_schema.push_back(tile_schema);
  }

  return new_schema;
}

// Set the transformed tile group column-at-a-time
void SetTransformedTileGroup(storage::TileGroup *orig_tile_group,
                             storage::TileGroup *new_tile_group) {
  // Check the schema of the two tile groups
  auto new_column_map = new_tile_group->GetColumnMap();
  auto orig_column_map = orig_tile_group->GetColumnMap();
  assert(new_column_map.size() == orig_column_map.size());

  oid_t orig_tile_offset, orig_tile_column_offset;
  oid_t new_tile_offset, new_tile_column_offset;

  auto column_count = new_column_map.size();
  auto tuple_count = orig_tile_group->GetAllocatedTupleCount();
  // Go over each column copying onto the new tile group
  for (oid_t column_itr = 0; column_itr < column_count; column_itr++) {
    // Locate the original base tile and tile column offset
    orig_tile_group->LocateTileAndColumn(column_itr, orig_tile_offset,
                                         orig_tile_column_offset);

    new_tile_group->LocateTileAndColumn(column_itr, new_tile_offset,
                                        new_tile_column_offset);

    auto orig_tile = orig_tile_group->GetTile(orig_tile_offset);
    auto new_tile = new_tile_group->GetTile(new_tile_offset);

    // Copy the column over to the new tile group
    for (oid_t tuple_itr = 0; tuple_itr < tuple_count; tuple_itr++) {
      auto val = orig_tile->GetValue(tuple_itr, orig_tile_column_offset);
      new_tile->SetValue(val, tuple_itr, new_tile_column_offset);
    }
  }

  // Finally, copy over the tile header
  auto header = orig_tile_group->GetHeader();
  auto new_header = new_tile_group->GetHeader();
  *new_header = *header;
}

storage::TileGroup *DataTable::TransformTileGroup(
    const oid_t &tile_group_offset, const double &theta) {
  // First, check if the tile group is in this table
  if (tile_group_offset >= tile_groups.size()) {
    LOG_ERROR("Tile group offset not found in table : %lu ", tile_group_offset);
    return nullptr;
  }

  auto tile_group_id = tile_groups[tile_group_offset];

  // Get orig tile group from catalog
  auto &catalog_manager = catalog::Manager::GetInstance();
  auto tile_group = catalog_manager.GetTileGroup(tile_group_id);
  auto diff = tile_group->GetSchemaDifference(default_partition);

  // Check threshold for transformation
  if (diff < theta) {
    return nullptr;
  }

  // Get the schema for the new transformed tile group
  auto new_schema =
      TransformTileGroupSchema(tile_group.get(), default_partition);

  // Allocate space for the transformed tile group
  std::shared_ptr<storage::TileGroup> new_tile_group(
      TileGroupFactory::GetTileGroup(
          tile_group->GetDatabaseId(), tile_group->GetTableId(),
          tile_group->GetTileGroupId(), tile_group->GetAbstractTable(),
          new_schema, default_partition, tile_group->GetAllocatedTupleCount()));

  // Set the transformed tile group column-at-a-time
  SetTransformedTileGroup(tile_group.get(), new_tile_group.get());

  // Set the location of the new tile group
  // and clean up the orig tile group
  catalog_manager.AddTileGroup(tile_group_id, new_tile_group);

  return new_tile_group.get();
}

void DataTable::RecordSample(const brain::Sample &sample) {
  // Add sample
  {
    std::lock_guard<std::mutex> lock(clustering_mutex);
    samples.push_back(sample);
  }
}

const column_map_type &DataTable::GetDefaultPartition() {
  return default_partition;
}

std::map<oid_t, oid_t> DataTable::GetColumnMapStats() {
  std::map<oid_t, oid_t> column_map_stats;

  // Cluster per-tile column count
  for (auto entry : default_partition) {
    auto tile_id = entry.second.first;
    auto column_map_itr = column_map_stats.find(tile_id);
    if (column_map_itr == column_map_stats.end())
      column_map_stats[tile_id] = 1;
    else
      column_map_stats[tile_id]++;
  }

  return std::move(column_map_stats);
}

void DataTable::UpdateDefaultPartition() {
  oid_t column_count = GetSchema()->GetColumnCount();

  // TODO: Number of clusters and new sample weight
  oid_t cluster_count = 4;
  double new_sample_weight = 0.01;

  brain::Clusterer clusterer(cluster_count, column_count, new_sample_weight);

  // Process all samples
  {
    std::lock_guard<std::mutex> lock(clustering_mutex);

    // Check if we have any samples
    if (samples.empty()) return;

    for (auto sample : samples) {
      clusterer.ProcessSample(sample);
    }

    samples.clear();
  }

  // TODO: Max number of tiles
  default_partition = clusterer.GetPartitioning(2);
}

//===--------------------------------------------------------------------===//
// UTILS
//===--------------------------------------------------------------------===//

column_map_type DataTable::GetStaticColumnMap(const std::string &table_name,
                                              const oid_t &column_count) {
  column_map_type column_map;

  // HYADAPT
  if (table_name == "HYADAPTTABLE") {
    // FSM MODE
    if (peloton_fsm == true) {
      for (oid_t column_id = 0; column_id < column_count; column_id++) {
        column_map[column_id] = std::make_pair(0, column_id);
      }
      return std::move(column_map);

      // TODO: ADD A FSM
      // return default_partition;
    }

    // DEFAULT
    if (peloton_num_groups == 0) {
      oid_t split_point = peloton_projectivity * (column_count - 1);
      oid_t rest_column_count = (column_count - 1) - split_point;

      column_map[0] = std::make_pair(0, 0);
      for (oid_t column_id = 0; column_id < split_point; column_id++) {
        auto hyadapt_column_id = hyadapt_column_ids[column_id];
        column_map[hyadapt_column_id] = std::make_pair(0, column_id + 1);
      }

      for (oid_t column_id = 0; column_id < rest_column_count; column_id++) {
        auto hyadapt_column_id = hyadapt_column_ids[split_point + column_id];
        column_map[hyadapt_column_id] = std::make_pair(1, column_id);
      }
    }
    // MULTIPLE GROUPS
    else {
      column_map[0] = std::make_pair(0, 0);
      oid_t tile_column_count = column_count / peloton_num_groups;

      for (oid_t column_id = 1; column_id < column_count; column_id++) {
        auto hyadapt_column_id = hyadapt_column_ids[column_id - 1];
        int tile_id = (column_id - 1) / tile_column_count;
        oid_t tile_column_id;
        if (tile_id == 0)
          tile_column_id = (column_id) % tile_column_count;
        else
          tile_column_id = (column_id - 1) % tile_column_count;

        if (tile_id >= peloton_num_groups) tile_id = peloton_num_groups - 1;

        column_map[hyadapt_column_id] = std::make_pair(tile_id, tile_column_id);
      }
    }

  }
  // YCSB
  else if (table_name == "USERTABLE") {
    column_map[0] = std::make_pair(0, 0);

    for (oid_t column_id = 1; column_id < column_count; column_id++) {
      column_map[column_id] = std::make_pair(1, column_id - 1);
    }
  }
  // FALLBACK
  else {
    for (oid_t column_id = 0; column_id < column_count; column_id++) {
      column_map[column_id] = std::make_pair(0, column_id);
    }
  }

  return std::move(column_map);
}

//===--------------------------------------------------------------------===//
// Query Optimizer Interface
//===--------------------------------------------------------------------===//

/*
 * GetSampleTileGroup() - Return sampling tile group pointer
 *
 * This function queries catalog manager for a shared_ptr object
 */
std::shared_ptr<storage::TileGroup> DataTable::GetSampleTileGroup() const {
  assert(sampled_tile_group_id != INVALID_OID);

  return catalog::Manager::GetInstance().GetTileGroup(sampled_tile_group_id);
}

/*
 * SampleRows() - This function samples rows in the physical table
 *
 * Since random number generating is a relatively expensive process, we
 * buffer the result in member "sample_for_optimizer" as ItemPointer vector
 * in order to reuse them for a different column next time
 *
 * NOTE 1: The number of samples may not be exact depending on the random
 * numbers. We return the actual number of samples taken
 */
size_t DataTable::SampleRows(size_t sample_size) {
  LOG_INFO("Start a new sampling, size = %lu ", sample_size);

  // This function should be mutual exclusive
  sample_mutex.lock();

  if(samples_for_optimizer.size() != 0) {
    LOG_INFO("Previous sample size not 0. Clear! ");

    // Clear previous sample results, if there is one
    samples_for_optimizer.clear();
  }

  // If there is an existing materialized sample tile group
  // drop it first to avoid data inconsistency
  if(sampled_tile_group_id != INVALID_OID) {
    LOG_INFO("Dropping an old sampled tile group... Prepare a new one.");

    catalog::Manager::GetInstance().DropTileGroup(sampled_tile_group_id);
  }

  // If there is old information in cardinality map also clear them
  if(cardinality_map.size() != 0) {
    LOG_INFO("Clearing existing cardinality map..\n");

    cardinality_map.clear();
  }

  // We want to make it ordered, and also want to detect for duplicates
  // so use a RB-Tree based ordered set
  std::set<oid_t> row_id_set{};

  // Get tile group count and tuple count
  // This is different from number of tuple which is just a float point
  // approximation - this is exact accurate number
  size_t total_tuple_number = tuple_count_exact;

  if(sample_size >= total_tuple_number) {
    LOG_INFO("Sample size too large! Adjust to fit actual table size %lu... ",
              total_tuple_number);

    sample_size = total_tuple_number;

    // Do not use random generator if we know we will cover all rows
    // Just insert them in increasing order
    for(size_t i = 0;i < sample_size;i++) {
      row_id_set.insert(i);
    }
  } else {
    // I copied this from the Internet...
    std::random_device rd;
    std::mt19937 generator(rd());

    // This is inclusive, [0, total_tuple_number - 1]
    std::uniform_int_distribution<> distribution(0, total_tuple_number - 1);

    int iter_count = 0;
    peloton::concurrency::TransactionManager &txn_mgr = \
      peloton::concurrency::TransactionManagerFactory::GetInstance();

    // outer loop detect whether there are duplicates
    while((row_id_set.size() < sample_size) && \
          (iter_count < 10)) {
      // Inner loop generates random numbers and insert into
      // the ordered set
      for(size_t i = 0;i < sample_size;i++) {
        oid_t random_row_id = distribution(generator);
        // This is the tile group ID inside the vector
        oid_t tile_group_offset = random_row_id / tuples_per_tilegroup;
        auto tile_group_p = GetTileGroup(tile_group_offset);

        // We need this header to check whether tuple is visible to
        // current thread
        TileGroupHeader *header_p = tile_group_p->GetHeader();

        // According to txn manager, this tuple is visible to current txn
        // so we take it as the sample
        if(txn_mgr.IsVisible(header_p, random_row_id)) {
          row_id_set.insert(random_row_id);
        }

        // If we have taken enough number of samples
        if(row_id_set.size() >= sample_size) {
          break;
        }
      }

      // Avoid looping for too many times
      iter_count++;

    } // while row id set size < sample size
  } // if sample size >= total tuple number

  // Iterate through all row ids and convert them into item pointers
  for(auto row_id : row_id_set) {
    oid_t tile_group_id = row_id / tuples_per_tilegroup;
    oid_t row_offset_in_tile_group = row_id % tuples_per_tilegroup;

    // Make it as ItemPointer
    // TODO: This is unsafe since any change to the table would potentially
    // make dangling pointers
    // NOTE: We do not worry about layout changes since tile group already
    // abstracts out layout information
    samples_for_optimizer.push_back(ItemPointer{tile_group_id, row_offset_in_tile_group});
  }

  // Also need to do this before early return
  sample_mutex.unlock();

  return row_id_set.size();
}

/*
 * BuildSampleSchema() - Build a column map and schema list for samples
 *
 * We always store all samples from different tile groups in a
 * uniformed format, which is defined by this function.
 *
 * TODO: Currently we only store it as pure column storage, since we omit
 * some columns in the base table, it would be peculiatr to store a
 * modified format of row for the base table
 *
 * NOTE: We filter out columns that are stored non-inlined
 */
void DataTable::BuildSampleSchema() {
  if(sample_column_map.size() != 0) {
    LOG_INFO("Sample column map size not zero. Clear it first");

    sample_column_map.clear();
  }

  assert(sample_column_map.size() == 0);
  assert(sample_schema_list.size() == 0);

  for(auto &item : inline_column_map) {
    oid_t table_column_id = item.first;
    oid_t sample_column_id = item.second;

    // Make each partition as a separate row (partition)
    sample_column_map[sample_column_id] = \
      std::pair<oid_t, oid_t>(sample_column_id, 0);

    // Need a vector of columns to construct new schema
    // for partition
    std::vector<catalog::Column> column_list{};
    column_list.push_back(schema->GetColumn(table_column_id));
    sample_schema_list.push_back(catalog::Schema(column_list));
  }

  return;
}

/*
 * BuildSampleTileGroup() - Build a tile group instance with sampling
 *                          arguments
 *
 * It uses the sampling table's column map (pure columnar), schema list
 * (one column per schema), and tuple count (must take sample first)
 *
 * NOTE: This function allocates new tile group ID and assign it to the
 * created tile group, and also assign it to class member variable
 */
TileGroup *DataTable::BuildSampleTileGroup() {
  if(GetOptimizerSampleSize() == 0LU) {
    LOG_INFO("Sample size is zero. Please take samples first");

    return nullptr;
  }

  // Allocate new tile group ID, and assign to class member
  sampled_tile_group_id = catalog::Manager::GetInstance().GetNextOid();

  // Create a tile group with column map, schema list and tuple count
  // of the sample table rather than data table
  TileGroup *tile_group = TileGroupFactory::GetTileGroup(
      database_oid,            // Stored in data table
      table_oid,               // Stored in data table
      sampled_tile_group_id,   // Allocated just now
      this,                    // Table reference
      sample_schema_list,      // Used by TileGroup to build its internal tile list
      sample_column_map,       // Used by TileGroup to locate table column
      GetOptimizerSampleSize() // Used by TileGroup to allocate internal storage
      );

  return tile_group;
}

/*
 * FillSampleTileGroup() - Fill sample tile group created elsewhere
 *                         with actual tuples
 *
 * We always store samples in columnar format
 *
 * NOTE 1: Currently we do not write data through the TileGroup interface
 * which means we directly go to Tile interface and GetValue()/SetValue()
 * This is really bad because it circumvents all controls provided by TileGroup
 * including MVCC
 *
 * NOTE 2: Despite the statements above, we do make use of GetNextEmptyTupleSlot()
 * to keep TileGroupHeader consistent with the sampling table
 */
void DataTable::FillSampleTileGroup() {
  if(GetOptimizerSampleSize() == 0LU) {
    LOG_INFO("Sample has not been taken");

    return;
  }

  // Make sure we have already materialized it
  assert(sampled_tile_group_id != INVALID_OID);

  // Get tile group for sampling table outside of
  // the loop
  auto sample_tile_group_p = GetSampleTileGroup();

  // Iterate through our sampled pointers
  for(ItemPointer &item : samples_for_optimizer) {
    // Offset of tile group (block offset)
    oid_t tile_group_offset = item.block;
    // Offset inside tile group (row offset)
    oid_t row_offset = item.offset;

    auto tile_group_p = GetTileGroup(tile_group_offset);

    // For each sampled row, we assign a row ID in the sample table
    oid_t next_sample_row_id = sample_tile_group_p->GetHeader()->GetNextEmptyTupleSlot();

    // For each column, find its position in data table, and copy its value
    // in a value object
    // Then find its position in the sample table, and copy it in
    for(auto &column_pair : inline_column_map) {
      // table overall column ID and sample table overall column ID
      oid_t table_column_id = column_pair.first;
      oid_t sample_column_id = column_pair.second;

      // Tile index inside TileGroup
      oid_t table_tile_offset = INVALID_OID;
      // Column index inside tile
      oid_t table_tile_column_offset = INVALID_OID;

      // This function modifies the last two arguments
      tile_group_p->LocateTileAndColumn(table_column_id,
                                        table_tile_offset,
                                        table_tile_column_offset);

      assert(table_tile_offset != INVALID_OID);
      assert(table_tile_column_offset != INVALID_OID);

      // Then fetch the tile and get value directly from the tile
      // since we only sample inlined data, value itself is enough
      // for representing it
      Tile *tile_p = tile_group_p->GetTile(table_tile_offset);
      Value temp_value = tile_p->GetValue(row_offset,
                                          table_tile_column_offset);

      oid_t sample_tile_offset = INVALID_OID;
      oid_t sample_tile_column_offset = INVALID_OID;

      sample_tile_group_p->LocateTileAndColumn(sample_column_id,
                                               sample_tile_offset,
                                               sample_tile_column_offset);
      // Because in sample tile group, all columns are stored in its own Tile
      // object, we only have column 0 inside tile, and tile offset equals
      // sample column offset
      assert(sample_tile_column_offset == 0);
      assert(sample_tile_offset == sample_column_id);

      Tile *sample_tile_p = sample_tile_group_p->GetTile(sample_tile_offset);

      // sample_tile_column_offset == 0
      sample_tile_p->SetValue(temp_value,
                              next_sample_row_id,
                              sample_tile_column_offset);
    }
  }

  return;
}

/*
 * MaterializeSample() - Store all samples as tuples in the tile group
 *
 * i.e. We add a new tile group to hold samples. The new tile group has
 * the same layout as the original one
 *
 * NOTE: It is required that sample vector is not empty
 *
 * NOTE 2: This operation must guarantee no data contention happens
 * which is achieved by using a lock to avoid multiple threads
 * planning different queries conflict with each other
 */
void DataTable::MaterializeSample() {
  // First check whether samples have already been taken or not
  if(GetOptimizerSampleSize() == 0LU) {
    LOG_INFO("Sample not taken yet. Please take sample first");

    return;
  }

  // We do not lock this in its child function
  sample_mutex.lock();

  // If there is an existing tile group, drop it to avoid memory leak
  if(sampled_tile_group_id != INVALID_OID) {
    LOG_INFO("Dropping an old sampled tile group... Prepare a new one.");

    catalog::Manager::GetInstance().DropTileGroup(sampled_tile_group_id);
  }

  // Create a tile group with sampling column map
  // sampled_tile_group_id is assigned inside BuildSampleTileGroup()
  std::shared_ptr<TileGroup> tile_group_p{BuildSampleTileGroup()};
  assert(tile_group_p.get());

  // Update current sample tile group ID, and register it with catalog manager
  sampled_tile_group_id = tile_group_p.get()->GetTileGroupId();
  catalog::Manager::GetInstance().AddTileGroup(sampled_tile_group_id,
                                               tile_group_p);

  // Copy actual data into the sample table
  FillSampleTileGroup();

  sample_mutex.unlock();

  return;
}

/*
 * ComputeTableCardinality() - Compute cardinality given a table
 *                             column ID
 *
 * This is a wrapper to ComputeSampleCardinality()
 */
void DataTable::ComputeTableCardinality(oid_t table_column_id) {
  auto it = inline_column_map.find(table_column_id);
  if(it == inline_column_map.end()) {
    LOG_ERROR("Table column %lu not sampled (varchar or binary?)",
              table_column_id);

    return;
  }

  // If the column sample exists just call wrapper with sample
  // column name
  ComputeSampleCardinality(it->second);

  return;
}

/*
 * ComputeSampleCardinality() - Given a sample column ID, compute the
 *                              cardinality
 *
 * Cardinality is computed using a hash table to aggregate inlined values
 *
 * NOTE: This function takes the argument as sample cardinality
 */
void DataTable::ComputeSampleCardinality(oid_t sample_column_id) {
  struct ValueEqualityFunc {
    size_t operator()(const Value &v) {
      return (size_t)v.MurmurHash3();
    }
  };

  std::unordered_set<Value,
                     ValueEqualityFunc> \
    aggregate_set{};

  auto sample_tile_group_p = GetSampleTileGroup();
  assert(sample_tile_group_p.get() != nullptr);

  oid_t tile_id = INVALID_OID;
  oid_t tile_column_id = INVALID_OID;

  // Last two arguments will be modified as return value
  // (this interface sucks - passing return value through output
  // parameters should be given a strong hint by using pointers)
  sample_tile_group_p->LocateTileAndColumn(sample_column_id,
                                           tile_id,
                                           tile_column_id);

  // Since sample tile group is totally column oriented
  // column offset inside a tile is always 0
  assert(tile_column_id == 0);
  // We always get tuple from this tile
  Tile *tile_p = sample_tile_group_p->GetTile(tile_id);

  size_t sample_row_count = GetOptimizerSampleSize();
  for(oid_t i = 0;i < (oid_t)sample_row_count;i++) {
    Value v = tile_p->GetValue(i, tile_column_id);

    aggregate_set.insert(v);
  }

  cardinality_map[sample_column_id] = aggregate_set.size();

  return;
}


/*
 * GetSmapleCardinality() - Return the cardinality of sample table
 *
 * NOTE: If the cardinality has not been taken, or the column does not
 * exist, this function always return 0 (which is considered as invalid)
 * to avoid crashing the system but the caller should take care
 */
size_t DataTable::GetSampleCardinality(oid_t sample_column_id) {
  if(cardinality_map.find(sample_column_id) == \
     cardinality_map.end()) {
    LOG_INFO("Sample column not found. Return 0 instead");

    return 0LU;
  }

  return cardinality_map[sample_column_id];
}

/*
 * GetTableCardinality() - Return cardinality of data table
 *
 * NOTE: This function returns 0 if the column has not been sampled
 * either because it is varchar/binary or because a wrong column ID
 * that does not exist in the table is provided.
 * Caller should check the return value to make sure the cardinality
 * returned is a valid number
 */
size_t DataTable::GetTableCardinality(oid_t table_column_id) {
  auto sample_column_it = inline_column_map.find(table_column_id);

  if(sample_column_it == inline_column_map.end()) {
    return 0;
  }

  return GetSampleCardinality(sample_column_it->second);
}


}  // End storage namespace
}  // End peloton namespace
