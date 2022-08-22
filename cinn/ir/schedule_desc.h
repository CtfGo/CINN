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

#pragma once
#include <absl/container/flat_hash_map.h>

#include <map>
#include <string>
#include <vector>

#include "cinn/ir/schedule_desc.pb.h"
#include "cinn/utils/type_defs.h"

namespace cinn {
namespace ir {

class ScheduleDesc {
 public:
  using ExprNameMap = absl::flat_hash_map<std::string, Expr>;
  struct Step {
    std::string type;
    absl::flat_hash_map<std::string, std::vector<std::string>> inputs;
    absl::flat_hash_map<std::string, std::vector<std::string>> outputs;
    AttributeMap attrs;
  };

  ScheduleDesc() = default;
  explicit ScheduleDesc(const proto::ScheduleDesc& desc_proto);
  void Append(Step&& step);
  void Replay(IRSchedule* schedule);
  proto::ScheduleDesc ToProto() const;

 private:
  std::vector<Step> steps_;
  ExprNameMap name2expr_;
};

}  // namespace ir
}  // namespace cinn
