// Copyright (c) 2023 CINN Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cinn/auto_schedule/search_space/auto_gen_rule/auto_bind.h"

#include <glog/logging.h>

#include "cinn/ir/collect_ir_nodes.h"
#include "cinn/ir/ir_printer.h"
#include "cinn/ir/ir_schedule.h"
#include "cinn/optim/ir_copy.h"

namespace cinn {
namespace auto_schedule {

static constexpr uint32_t kMaxThreadBlocks = 256;

bool IsSpatialLoop(const ir::For* for_node) {
  if (for_node->for_type() != ir::ForType::Serial) return false;

  const auto& loop_var      = for_node->loop_var;
  auto used_for_reduce_axis = ir::CollectIRNodesWithoutTensor(for_node->body, [&loop_var](const Expr* x) {
    const auto* block_realize = x->As<ir::ScheduleBlockRealize>();
    if (!block_realize) return false;

    const auto* schedule_block = block_realize->schedule_block.As<ir::ScheduleBlock>();
    CHECK(schedule_block) << "schedule_block field is not a ScheduleBlock";
    CHECK_EQ(block_realize->iter_values.size(), schedule_block->iter_vars.size());
    for (int i = 0; i < block_realize->iter_values.size(); ++i) {
      const ir::Var& iter_var = schedule_block->iter_vars[i];
      const ir::Expr& binding = block_realize->iter_values[i];
      if (iter_var->is_reduce_axis || iter_var->name.substr(0, 6) == "reduce") {
        auto used_exprs = ir::CollectIRNodesWithoutTensor(binding, [&loop_var](const Expr* x) {
          const ir::_Var_* var = x->As<ir::_Var_>();
          if (var && (x->same_as(loop_var) || var->name == loop_var->name)) {
            return true;
          }
          return false;
        });
        if (!used_exprs.empty()) return true;
      }
    }
  });

  if (!used_for_reduce_axis.empty()) return false;
  return true;
}

// count the number of loops that can be binded from the input for_node to bottom
int CountLoopCanBinded(const ir::For* for_node) {
  int cnt = 0;
  while (for_node) {
    if (for_node->is_binded()) break;
    if (!IsSpatialLoop(for_node)) break;

    cnt += 1;

    CHECK(for_node->body.defined() && for_node->body.As<ir::Block>()) << "Body is not defined";
    const ir::Block* body = for_node->body.As<ir::Block>();
    for_node              = body->stmts.size() == 1 ? body->stmts[0].As<ir::For>() : nullptr;
  }
  return cnt;
}

void BindGPUIndex(ir::IRSchedule* ir_schedule,
                  ir::Expr& applied_block,
                  int num_loops_to_bind,
                  int max_blocks_num,
                  int max_threads_num) {
  auto all_loops = ir_schedule->GetLoops(applied_block);
  CHECK_LE(num_loops_to_bind, all_loops.size()) << "The number of loops to be bind is greater than size of all_loops";
  // check whether it is the case that threadIdx has been binded but blockIdx not,
  // the threadIdx can only be binded in the first loop after num_loops_to_bind loops
  // because we has excluded other cases in CountLoopCanBinded
  bool gpu_thread_has_binded =
      num_loops_to_bind < all_loops.size() && all_loops[num_loops_to_bind].As<ir::For>()->is_gpu_thread_binded();
  Expr fused_loop = ir_schedule->Fuse({all_loops.begin(), all_loops.begin() + num_loops_to_bind});
  int32_t extent  = fused_loop.As<ir::For>()->extent.as_int32();
  if (gpu_thread_has_binded) {
    ir_schedule->Bind(fused_loop, "blockIdx.x");
    return;
  }

  if (extent <= max_threads_num) {
    ir_schedule->Bind(fused_loop, "threadIdx.x");
    return;
  }

  if (extent <= max_blocks_num * max_threads_num) {
    auto splits = ir_schedule->Split(fused_loop, {-1, max_threads_num});
    CHECK_EQ(splits.size(), 2);
    ir_schedule->Bind(splits[0], "blockIdx.x");
    ir_schedule->Bind(splits[1], "threadIdx.x");
  } else {
    auto splits = ir_schedule->Split(fused_loop, {-1, max_blocks_num, max_threads_num});
    CHECK_EQ(splits.size(), 3);
    ir_schedule->Reorder({splits[1], splits[2], splits[0]});
    ir_schedule->Bind(splits[1], "blockIdx.x");
    ir_schedule->Bind(splits[2], "threadIdx.x");
  }
}

RuleApplyType AutoBind::Init(ir::IRSchedule* ir_schedule) {
  ir_schedule_ = ir_schedule;

  for (auto&& block_realize : ir_schedule->GetAllBlocks()) {
    auto all_loops = ir_schedule->GetLoops(block_realize);
    if (CountLoopCanBinded(all_loops[0].As<ir::For>()) > 0) {
      applicable_schedule_blocks_.emplace_back(block_realize);
    }
  }
  num_applicable_ = applicable_schedule_blocks_.size();
  VLOG(6) << "Collect applicable_schedule_blocks_:" << num_applicable_;
  return num_applicable_ > 0 ? RuleApplyType::kApplyAndPruneOtherRules : RuleApplyType::kCannotApply;
}

void AutoBind::Apply(int index) {
  CHECK_LT(index, applicable_schedule_blocks_.size()) << "invalid apply index:" << index;
  auto applied_block = applicable_schedule_blocks_.at(index);
  auto all_loops     = ir_schedule_->GetLoops(applied_block);
  BindGPUIndex(ir_schedule_,
               applied_block,
               CountLoopCanBinded(all_loops[0].As<ir::For>()),
               kMaxThreadBlocks,
               target_->max_num_threads());
  return;
}

RuleApplyType AutoBind::AnalyseApplyType(SearchState state, const std::string& block_name) const {
  Expr block_expr = state->ir_schedule.GetBlock(block_name);
  auto all_loops  = state->ir_schedule.GetLoops(block_expr);
  return CountLoopCanBinded(all_loops[0].As<ir::For>()) > 0 ? RuleApplyType::kApplyAndPruneOtherRules
                                                            : RuleApplyType::kCannotApply;
}

std::vector<SearchState> AutoBind::ApplyOnBlock(SearchState state, const std::string& block_name) {
  SearchState new_state = state.Copy();
  Expr applied_block    = new_state->ir_schedule.GetBlock(block_name);
  auto all_loops        = state->ir_schedule.GetLoops(applied_block);
  BindGPUIndex(&new_state->ir_schedule,
               applied_block,
               CountLoopCanBinded(all_loops[0].As<ir::For>()),
               kMaxThreadBlocks,
               target_->max_num_threads());
  return {new_state};
}

}  // namespace auto_schedule
}  // namespace cinn
