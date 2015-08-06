/*-------------------------------------------------------------------------
 *
 * merge_join.cpp
 * file description
 *
 * Copyright(c) 2015, CMU
 *
 * /peloton/src/executor/merge_join_executor.cpp
 *
 *-------------------------------------------------------------------------
 */

#include <vector>

#include "backend/common/types.h"
#include "backend/common/logger.h"
#include "backend/executor/logical_tile_factory.h"
#include "backend/executor/merge_join_executor.h"
#include "backend/expression/abstract_expression.h"
#include "backend/expression/container_tuple.h"

namespace peloton {
namespace executor {

/**
 * @brief Constructor for nested loop join executor.
 * @param node Nested loop join node corresponding to this executor.
 */
MergeJoinExecutor::MergeJoinExecutor(planner::AbstractPlanNode *node,
                                     ExecutorContext *executor_context)
    : AbstractJoinExecutor(node, executor_context) {
}

bool MergeJoinExecutor::DInit() {
  auto status = AbstractJoinExecutor::Init();
  if (status == false)
    return status;

  const planner::MergeJoinNode &node = GetPlanNode<planner::MergeJoinNode>();

  join_clause_ = node.GetJoinClauses();

  return true;
}

/**
 * @brief Creates logical tiles from the two input logical tiles after applying
 * join predicate.
 * @return true on success, false otherwise.
 */
bool MergeJoinExecutor::DExecute() {
  LOG_TRACE("********** Merge Join executor :: 2 children \n");

  bool right_scan_end = false;
  // Try to get next tile from RIGHT child
  if (children_[1]->Execute() == false) {
    LOG_ERROR("Did not get right tile \n");
    return false;
  }

  LOG_TRACE("Got right tile \n");

  // Try to get next tile from LEFT child
  if (children_[0]->Execute() == false) {
    LOG_TRACE("Did not get left tile \n");
    return false;
  }
  LOG_TRACE("Got left tile \n");

  std::unique_ptr<LogicalTile> left_tile(children_[0]->GetOutput());
  std::unique_ptr<LogicalTile> right_tile(children_[1]->GetOutput());

  // Check the input logical tiles.
  assert(left_tile.get() != nullptr);
  assert(right_tile.get() != nullptr);

  // Construct output logical tile.
  std::unique_ptr<LogicalTile> output_tile(LogicalTileFactory::GetTile());

  auto left_tile_schema = left_tile.get()->GetSchema();
  auto right_tile_schema = right_tile.get()->GetSchema();

  for (auto &col : right_tile_schema) {
    col.position_list_idx += left_tile.get()->GetPositionLists().size();
  }

  /* build the schema given the projection */
  auto output_tile_schema = BuildSchema(left_tile_schema, right_tile_schema);

  // Set the output logical tile schema
  output_tile.get()->SetSchema(std::move(output_tile_schema));

  // Now, let's compute the position lists for the output tile

  // Cartesian product

  // Add everything from two logical tiles
  auto left_tile_position_lists = left_tile.get()->GetPositionLists();
  auto right_tile_position_lists = right_tile.get()->GetPositionLists();

  // Compute output tile column count
  size_t left_tile_column_count = left_tile_position_lists.size();
  size_t right_tile_column_count = right_tile_position_lists.size();
  size_t output_tile_column_count = left_tile_column_count
      + right_tile_column_count;

  assert(left_tile_column_count > 0);
  assert(right_tile_column_count > 0);

  // Compute output tile row count
  size_t left_tile_row_count = left_tile_position_lists[0].size();
  size_t right_tile_row_count = right_tile_position_lists[0].size();

  // Construct position lists for output tile
  std::vector<std::vector<oid_t> > position_lists;
  for (size_t column_itr = 0; column_itr < output_tile_column_count;
      column_itr++)
    position_lists.push_back(std::vector<oid_t>());

  LOG_TRACE("left col count: %lu, right col count: %lu", left_tile_column_count,
            right_tile_column_count);
  LOG_TRACE("left col count: %lu, right col count: %lu",
            left_tile.get()->GetColumnCount(),
            right_tile.get()->GetColumnCount());
  LOG_TRACE("left row count: %lu, right row count: %lu", left_tile_row_count,
            right_tile_row_count);

  size_t left_start_row = 0;
  size_t right_start_row = 0;

  size_t left_end_row = Advance(left_tile.get(), left_start_row, true);
  size_t right_end_row = Advance(right_tile.get(), right_start_row, false);

  while (left_end_row > left_start_row && right_end_row > right_start_row) {

    expression::ContainerTuple<executor::LogicalTile> left_tuple(
        left_tile.get(), left_start_row);
    expression::ContainerTuple<executor::LogicalTile> right_tuple(
        right_tile.get(), right_start_row);
    bool equal = false;

    // try to match the join clauses
    for (auto &clause : join_clause_) {
      auto left_value = clause.left_.get()->Evaluate(&left_tuple, &right_tuple,
                                                     nullptr);
      auto right_value = clause.right_.get()->Evaluate(&left_tuple,
                                                       &right_tuple, nullptr);
      int ret = left_value.Compare(right_value);

      if (ret < 0) {
        // Left key < Right key, advance left
        left_start_row = left_end_row;
        left_end_row = Advance(left_tile.get(), left_start_row, true);
        equal = false;
        break;
      } else if (ret > 0) {
        // Left key > Right key, advance right
        right_start_row = right_end_row;
        right_end_row = Advance(right_tile.get(), right_start_row, false);
        equal = false;
        break;
      }
      // Left key == Right key, go check next join clause
    }

    if (!equal) {
      // join clauses are not matched, one of the tile has been advanced
      continue;
    }

    // join clauses are matched, try to match predicate

    // Join predicate exists
    if (predicate_ != nullptr) {
      // Join predicate is false. Advance both.
      if (predicate_->Evaluate(&left_tuple, &right_tuple, executor_context_)
          .IsFalse()) {
        left_start_row = left_end_row;
        left_end_row = Advance(left_tile.get(), left_start_row, true);
        right_start_row = right_end_row;
        right_end_row = Advance(right_tile.get(), right_start_row, false);
      }
    }

    // subtile matched, do a cartesian product
    // Go over every pair of tuples in left and right logical tiles
    for (size_t left_tile_row_itr = left_start_row; left_tile_row_itr < left_end_row;
        left_tile_row_itr++) {
      for (size_t right_tile_row_itr = right_start_row;
          right_tile_row_itr < right_end_row; right_tile_row_itr++) {

        // Insert a tuple into the output logical tile
        // First, copy the elements in left logical tile's tuple
        for (size_t output_tile_column_itr = 0;
            output_tile_column_itr < left_tile_column_count;
            output_tile_column_itr++) {
          position_lists[output_tile_column_itr].push_back(
              left_tile_position_lists[output_tile_column_itr][left_tile_row_itr]);
        }

        // Then, copy the elements in left logical tile's tuple
        for (size_t output_tile_column_itr = 0;
            output_tile_column_itr < right_tile_column_count;
            output_tile_column_itr++) {
          position_lists[left_tile_column_count + output_tile_column_itr]
              .push_back(
              right_tile_position_lists[output_tile_column_itr][right_tile_row_itr]);
        }
      }
    }

    // then Advance both
    left_start_row = left_end_row;
    left_end_row = Advance(left_tile.get(), left_start_row, true);
    right_start_row = right_end_row;
    right_end_row = Advance(right_tile.get(), right_start_row, false);
  }

  for (auto col : position_lists) {
    LOG_TRACE("col");
    for (auto elm : col) {
      (void) elm;  // silent compiler
      LOG_TRACE("elm: %u", elm);
    }
  }

  // Check if we have any matching tuples.
  if (position_lists[0].size() > 0) {
    output_tile.get()->SetPositionListsAndVisibility(std::move(position_lists));
    SetOutput(output_tile.release());
    return true;
  }
  // Try again
  else {
    // If we are out of any more pairs of child tiles to examine,
    // then we will return false earlier in this function.
    // So, we don't have to return false here.
    DExecute();
  }

  return true;
}

/**
 * @brief Advance the row iterator until value changes in terms of the join clauses
 * @return the end row number, [start_row, end_row) are the rows of the same value
 *         if the end_row == start_row, the subset is empty
 */
size_t MergeJoinExecutor::Advance(LogicalTile *tile, size_t start_row,
                                  bool is_left) {
  size_t end_row = start_row + 1;
  size_t this_row = start_row;
  size_t tuple_count = tile->GetTupleCount();
  if (start_row >= tuple_count)
    return start_row;

  while (end_row < tuple_count) {
    expression::ContainerTuple<executor::LogicalTile> this_tuple(tile,
                                                                 this_row);
    expression::ContainerTuple<executor::LogicalTile> next_tuple(tile, end_row);

    for (auto &clause : join_clause_) {
      // Go thru each join clauses
      auto expr = is_left ? clause.left_.get() : clause.right_.get();
      peloton::Value this_value = expr->Evaluate(&this_tuple, &this_tuple,
                                                 nullptr);
      peloton::Value next_value = expr->Evaluate(&next_tuple, &next_tuple,
                                                 nullptr);
      if (0 != this_value.Compare(next_value)) {
        break;
      }
    }
    // the two tuples are the same, we advance by 1
    end_row++;
    this_row = end_row;
  }
  return end_row;
}

}  // namespace executor
}  // namespace peloton
