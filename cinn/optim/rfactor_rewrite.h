#pragma once

#include "cinn/ir/ir.h"

namespace cinn::optim {

void RFactorRewrite(Expr* e, poly::StageMap stages);

}  // namespace cinn::optim
