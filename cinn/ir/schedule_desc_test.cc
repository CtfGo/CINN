// Copyright (c) 2022 CINN Authors. All Rights Reserved.
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

#include "cinn/ir/schedule_desc.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "cinn/cinn.h"
#include "cinn/ir/ir_printer.h"
#include "cinn/lang/lower.h"
#include "cinn/optim/ir_copy.h"
#include "cinn/utils/string.h"
#include "cinn/utils/type_defs.h"

namespace cinn {
namespace ir {

std::vector<ir::LoweredFunc> ElementwiseCopyExpr(const std::vector<int>& shape,
                                                 const Target& target,
                                                 const std::string& func_name) {
  CHECK_EQ(shape.size(), 2) << "size of shape shoule be 2";
  Expr M(shape[0]);
  Expr N(shape[1]);

  Placeholder<float> A("A", {M, N});
  auto B = Compute(
      {M, N}, [&](Var i, Var j) { return A(i, j); }, "B");

  auto stages = CreateStages({A, B});
  return cinn::lang::LowerVec(func_name, stages, {A, B}, {}, {}, nullptr, target, true);
}

IRSchedule MakeIRSchedule(const std::vector<ir::LoweredFunc>& lowered_funcs) {
  std::vector<Expr> exprs;
  for (auto&& func : lowered_funcs) {
    exprs.emplace_back(optim::IRCopy(func->body));
  }
  return ir::IRSchedule(ir::ModuleExpr(exprs));
}

std::string SourceCodeGen(const ModuleExpr& module_expr,
                          const Target& target,
                          std::vector<ir::LoweredFunc>& lowered_funcs) {
  auto exprs = module_expr.GetExprs();
  CHECK_EQ(exprs.size(), lowered_funcs.size()) << "size of func is not euqal";
  Module::Builder builder("test_module", target);
  for (auto i = 0; i < lowered_funcs.size(); ++i) {
    auto&& func = lowered_funcs.at(i);
    func->body  = exprs.at(i);
    builder.AddFunction(func);
  }
  auto module = builder.Build();
  CodeGenC codegen(target);
  codegen.SetInlineBuiltinCodes(false);
  return codegen.Compile(module, CodeGenC::OutputKind::CImpl);
}

class TestScheduleDesc : public ::testing::Test {
 public:
  void SetUp() override { Context::Global().ResetNameId(); }
  Target target;
  std::vector<ir::LoweredFunc> lowered_funcs;

  void CheckTracingOutputs(const std::vector<Expr>& base, const ScheduleDesc& trace_desc) {
    ir::IRSchedule replay_sch = MakeIRSchedule(lowered_funcs);
    auto traced_outputs       = ScheduleDesc::ReplayWithProto(trace_desc.ToProto(), &replay_sch);
    ASSERT_EQ(base.size(), traced_outputs.size());
    for (auto i = 0; i < base.size(); ++i) {
      ASSERT_EQ(utils::GetStreamCnt(base.at(i)), utils::GetStreamCnt(traced_outputs.at(i)));
    }
  }

  void CheckReplayResult(const ir::IRSchedule& ir_sch, const ScheduleDesc& trace_desc) {
    ir::IRSchedule replay_sch = MakeIRSchedule(lowered_funcs);
    trace_desc.Replay(&replay_sch);

    // check the equality of module expr between original schedule
    // and the schedule generated by replaying with trace_desc
    auto lhs_exprs = ir_sch.GetModule().GetExprs();
    auto rhs_exprs = replay_sch.GetModule().GetExprs();
    ASSERT_EQ(lhs_exprs.size(), rhs_exprs.size());
    for (auto i = 0; i < lhs_exprs.size(); ++i) {
      ASSERT_EQ(utils::GetStreamCnt(lhs_exprs.at(i)), utils::GetStreamCnt(rhs_exprs.at(i)));
    }

    // check the equality of source code between them
    ASSERT_EQ(utils::Trim(SourceCodeGen(ir_sch.GetModule(), target, lowered_funcs)),
              utils::Trim(SourceCodeGen(replay_sch.GetModule(), target, lowered_funcs)));
  }
};

TEST_F(TestScheduleDesc, Append_Replay) {
  target        = common::DefaultHostTarget();
  lowered_funcs = ElementwiseCopyExpr({32, 32}, target, "test1");

  ir::IRSchedule ir_sch = MakeIRSchedule(lowered_funcs);
  ScheduleDesc desc;

  auto fused = ir_sch.Fuse("B", {0, 1});
  desc.Append(ScheduleDesc::Step(
      "FuseWithBlockName", {}, {{"block_name", std::string("B")}, {"loops_index", std::vector<int>({0, 1})}}, {fused}));
  auto splited = ir_sch.Split(fused, {4, -1});
  desc.Append(ScheduleDesc::Step(
      "Split", {{"loop", std::vector<Expr>({fused})}}, {{"factors", std::vector<int>({4, -1})}}, splited));

  auto loops = ir_sch.GetLoops("B");
  desc.Append(ScheduleDesc::Step("GetLoopsWithName", {}, {{"block_name", std::string("B")}}, loops));
  fused = ir_sch.Fuse(loops);
  desc.Append(ScheduleDesc::Step("Fuse", {{"loops", loops}}, {}, {fused}));
  splited = ir_sch.Split(fused, {256, -1});
  desc.Append(ScheduleDesc::Step(
      "Split", {{"loop", std::vector<Expr>({fused})}}, {{"factors", std::vector<int>({256, -1})}}, splited));

  CheckTracingOutputs(splited, desc);
  CheckReplayResult(ir_sch, desc);
}

TEST_F(TestScheduleDesc, StepKind_GetAllBlocks) {
  target                    = common::DefaultHostTarget();
  lowered_funcs             = ElementwiseCopyExpr({32, 32}, target, "test1");
  ir::IRSchedule ir_sch     = MakeIRSchedule(lowered_funcs);
  ir::IRSchedule replay_sch = MakeIRSchedule(lowered_funcs);
  ScheduleDesc desc;

  auto all_blocks = ir_sch.GetAllBlocks();
  desc.Append(ScheduleDesc::Step("GetAllBlocks", {}, {}, {all_blocks}));
  CheckTracingOutputs(all_blocks, desc);
}

TEST_F(TestScheduleDesc, StepKind_GetLoops) {
  target                = common::DefaultHostTarget();
  lowered_funcs         = ElementwiseCopyExpr({32, 32}, target, "test1");
  ir::IRSchedule ir_sch = MakeIRSchedule(lowered_funcs);
  ScheduleDesc desc;

  auto all_blocks = ir_sch.GetAllBlocks();
  desc.Append(ScheduleDesc::Step("GetAllBlocks", {}, {}, {all_blocks}));
  auto loops = ir_sch.GetLoops(all_blocks[0]);
  desc.Append(ScheduleDesc::Step("GetLoops", {{"block", std::vector<Expr>({all_blocks[0]})}}, {}, {loops}));
  CheckTracingOutputs(loops, desc);
}

}  // namespace ir
}  // namespace cinn