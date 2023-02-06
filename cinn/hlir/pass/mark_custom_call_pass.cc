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

#include "cinn/common/type.h"
#include "cinn/hlir/framework/pass.h"
#include "cinn/hlir/op/external_api_registry.h"
#include "cinn/utils/string.h"

DECLARE_string(cinn_custom_call_mark_excluded_ops);

namespace cinn {
namespace hlir {
namespace pass {

using cinn::hlir::op::ExternalApiRegistry;
using framework::Graph;
using framework::Node;

class GraphAlterHelper {
 public:
  GraphAlterHelper(Graph* graph) : graph_(graph), excluded_ops_(nullptr) {
    if (graph_->HasAttr("custom_call_excluded_ops")) {
      excluded_ops_ = &graph_->GetAttrs<std::unordered_set<std::string>>("custom_call_excluded_ops");
    }
  }
  void MarkCustomCallOps(const common::Target& target) {
    auto mark_nodes = graph_->CollectNodes([this, &target](const common::GraphNode* graph_node) -> bool {
      if (graph_node->safe_as<Node>()) {
        auto node      = graph_node->safe_as<Node>();
        auto&& op_name = node->op()->name;
        if (!IsExcluded(op_name) && ExternalApiRegistry::Global()->Has(op_name, target)) {
          VLOG(4) << "Op:" << op_name << " will not use custom_call";
          return true;
        }
      }

      return false;
    });

    for (auto* graph_node : mark_nodes) {
      auto* node                                   = graph_node->safe_as<Node>();
      node->attrs.attr_store["enable_custom_call"] = true;
    }
  }

 private:
  Graph* graph_;
  const std::unordered_set<std::string>* excluded_ops_;

  bool IsExcluded(const std::string& op_name) { return excluded_ops_ && excluded_ops_->count(op_name); }
};

void MarkCustomCallOpsInternal(Graph* graph) {
  VLOG(3) << "MarkCustomCallOps...!";
  GraphAlterHelper(graph).MarkCustomCallOps(graph->target_);
  VLOG(3) << "MarkCustomCallOps Finish...!";
}

}  // namespace pass
}  // namespace hlir
}  // namespace cinn

CINN_REGISTER_HELPER(MarkCustomCallOpsPass) {
  CINN_REGISTER_PASS(MarkCustomCallOps)
      .describe(
          "This pass which mark all ops with external_api registered on the specified target, "
          "except the blacklist specified by FLAGS_cinn_custom_call_mark_excluded_ops")
      .set_change_structure(false)
      .set_body(cinn::hlir::pass::MarkCustomCallOpsInternal);
  return true;
}
