//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// data_table_test.cpp
//
// Identification: tests/storage/data_table_test.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "harness.h"

#include "backend/storage/data_table.h"
#include "backend/storage/tile_group.h"
#include "backend/concurrency/transaction_manager_factory.h"
#include "executor/executor_tests_util.h"

namespace peloton {
namespace test {

//===--------------------------------------------------------------------===//
// Data Table Tests
//===--------------------------------------------------------------------===//

class DataTableTests : public PelotonTest {};

TEST_F(DataTableTests, TransformTileGroupTest) {
  const int tuple_count = TESTS_TUPLES_PER_TILEGROUP;

  // Create a table and wrap it in logical tiles
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();
  std::unique_ptr<storage::DataTable> data_table(
      ExecutorTestsUtil::CreateTable(tuple_count, false));
  ExecutorTestsUtil::PopulateTable(txn, data_table.get(), tuple_count, false,
                                   false, true);
  txn_manager.CommitTransaction();

  // Create the new column map
  storage::column_map_type column_map;
  column_map[0] = std::make_pair(0, 0);
  column_map[1] = std::make_pair(0, 1);
  column_map[2] = std::make_pair(1, 0);
  column_map[3] = std::make_pair(1, 1);

  auto theta = 0.0;

  // Transform the tile group
  data_table->TransformTileGroup(0, theta);

  // Create the another column map
  column_map[0] = std::make_pair(0, 0);
  column_map[1] = std::make_pair(0, 1);
  column_map[2] = std::make_pair(0, 2);
  column_map[3] = std::make_pair(1, 0);

  // Transform the tile group
  data_table->TransformTileGroup(0, theta);

  // Create the another column map
  column_map[0] = std::make_pair(0, 0);
  column_map[1] = std::make_pair(1, 0);
  column_map[2] = std::make_pair(1, 1);
  column_map[3] = std::make_pair(1, 2);

  // Transform the tile group
  data_table->TransformTileGroup(0, theta);
}

/*
 * SamplingForOptimizerTest - Tests basic table basic sampling
 */
TEST_F(DataTableTests, SamplingForOptimizerTest) {
  const int tuple_count = 1000;

  // Create a table and wrap it in logical tiles
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();
  std::unique_ptr<storage::DataTable> data_table(
      ExecutorTestsUtil::CreateTable(tuple_count, false));
  ExecutorTestsUtil::PopulateTable(txn, data_table.get(), tuple_count, false,
                                   true, true);
  txn_manager.CommitTransaction();

  size_t sample_size = data_table->SampleRows(100);

  LOG_INFO("Retake sample to see whether old ones are dropped correctly...");

  sample_size = data_table->SampleRows(100);


  LOG_INFO("Sample size = %lu; actual size = %lu",
           sample_size,
           data_table->GetOptimizerSampleSize());

  data_table->MaterializeSample();

  LOG_INFO("Re-materialization to check whether old tile group is dropped");

  data_table->MaterializeSample();

  LOG_INFO("Finished materialization of samples!");

  data_table->ComputeTableCardinality(0);
  data_table->ComputeTableCardinality(1);
  data_table->ComputeTableCardinality(2);
  // This should print an error under debug mode
  data_table->ComputeTableCardinality(3);

  LOG_INFO("Finished computing cardinality for columns");

  size_t c0 = data_table->GetTableCardinality(0);
  size_t c1 = data_table->GetTableCardinality(1);
  size_t c2 = data_table->GetTableCardinality(2);
  size_t c3 = data_table->GetTableCardinality(3);

  LOG_INFO("Cardinality: %lu, %lu, %lu, %lu", c0, c1, c2, c3);
  LOG_INFO("Take entire table as sample to check accuracy");

  // Take whole table as sample to see accuracy
  sample_size = data_table->SampleRows(1000);

  // Do not forget this; otherwise seg fault
  data_table->MaterializeSample();

  data_table->ComputeTableCardinality(0);
  data_table->ComputeTableCardinality(1);
  data_table->ComputeTableCardinality(2);
  // This should print an error under debug mode
  data_table->ComputeTableCardinality(3);

  LOG_INFO("Finished computing cardinality for columns");

  c0 = data_table->GetTableCardinality(0);
  c1 = data_table->GetTableCardinality(1);
  c2 = data_table->GetTableCardinality(2);
  c3 = data_table->GetTableCardinality(3);

  LOG_INFO("Cardinality: %lu, %lu, %lu, %lu", c0, c1, c2, c3);

  return;
}

}  // End test namespace
}  // End peloton namespace
