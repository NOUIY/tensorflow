/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/spmd/shardy/utils.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "mhlo/IR/register.h"
#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/Dialect/Func/Extensions/AllExtensions.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeRange.h"
#include "mlir/Support/LLVM.h"
#include "shardy/dialect/sdy/ir/dialect.h"
#include "shardy/dialect/sdy/ir/register.h"
#include "shardy/dialect/sdy/ir/utils.h"
#include "stablehlo/dialect/StablehloOps.h"
#include "xla/mlir_hlo/mhlo/IR/register.h"
#include "xla/service/spmd/shardy/constants.h"
#include "xla/service/spmd/shardy/extensions/mhlo_extensions.h"

namespace xla {
namespace sdy {

using ::mlir::ArrayRef;
using ::mlir::Attribute;
using ::mlir::DictionaryAttr;
using ::mlir::NamedAttribute;
using ::mlir::Operation;
using ::mlir::SmallVector;
using ::mlir::StringAttr;
using ::mlir::StringRef;
using xla::sdy::kFrontendAttributesAttr;

using ::mlir::func::FuncOp;
using ::mlir::sdy::AxisRefAttr;
using ::mlir::sdy::AxisRefListAttr;
using ::mlir::sdy::DimensionShardingAttr;
using ::mlir::sdy::MeshAttr;
using ::mlir::sdy::MeshAxisAttr;
using ::mlir::sdy::SubAxisInfoAttr;
using ::mlir::sdy::TensorShardingAttr;
using ::mlir::sdy::TensorShardingPerValueAttr;
using ::mlir::stablehlo::CustomCallOp;

absl::string_view toStringView(mlir::StringRef sr) {
  return absl::string_view(sr.data(), sr.size());
}

DictionaryAttr getFrontendAttrs(Operation* op) {
  return op->getAttrOfType<DictionaryAttr>(kFrontendAttributesAttr);
}

DictionaryAttr getFuncArgFrontendAttrs(FuncOp funcOp, unsigned int index) {
  return funcOp.getArgAttrOfType<DictionaryAttr>(index,
                                                 kFrontendAttributesAttr);
}

namespace {

mlir::StringAttr getStringAttribute(Attribute attr, mlir::OpBuilder& builder) {
  std::string value;
  if (auto stringAttr = mlir::dyn_cast<StringAttr>(attr)) {
    return stringAttr;
  }
  return builder.getStringAttr(mlir::sdy::attributeToString(attr));
}

SmallVector<NamedAttribute> getExistingFrontendAttributes(
    DictionaryAttr frontendAttributes, StringRef excludedAttribute) {
  SmallVector<NamedAttribute> dictEntries;
  if (!frontendAttributes) {
    return dictEntries;
  }
  for (NamedAttribute entry : frontendAttributes) {
    if (entry.getName() != excludedAttribute) {
      dictEntries.push_back(entry);
    }
  }
  return dictEntries;
}

void setFrontendAttribute(SmallVector<NamedAttribute>& existingAttributes,
                          StringRef name, Attribute value) {
  mlir::OpBuilder builder(value.getContext());
  StringAttr stringValue = getStringAttribute(value, builder);
  for (auto* it = existingAttributes.begin(); it != existingAttributes.end();
       ++it) {
    if (it->getName() == name) {
      if (it->getValue() == stringValue) {
        return;
      }
      existingAttributes.erase(it);
      break;
    }
  }
  existingAttributes.emplace_back(
      NamedAttribute(builder.getStringAttr(name), stringValue));
}

void removeFrontendAttribute(
    DictionaryAttr frontendAttributes, StringRef attributeName,
    std::function<void(ArrayRef<NamedAttribute>)> setAttr,
    std::function<void()> removeAttr) {
  SmallVector<NamedAttribute> existingAttributes =
      getExistingFrontendAttributes(frontendAttributes, attributeName);
  if (!existingAttributes.empty()) {
    setAttr(existingAttributes);
  } else {
    removeAttr();
  }
}

void setFrontendAttrs(Operation* op, ArrayRef<NamedAttribute> frontendAttrs) {
  return op->setAttr(kFrontendAttributesAttr,
                     DictionaryAttr::get(op->getContext(), frontendAttrs));
}

void setFuncArgFrontendAttrs(FuncOp funcOp, unsigned int index,
                             ArrayRef<NamedAttribute> frontendAttrs) {
  funcOp.setArgAttr(index, kFrontendAttributesAttr,
                    DictionaryAttr::get(funcOp.getContext(), frontendAttrs));
}

}  // namespace

void setFrontendAttribute(Operation* op, StringRef name, Attribute value) {
  SmallVector<NamedAttribute> existingAttributes =
      getExistingFrontendAttributes(getFrontendAttrs(op), "");
  setFrontendAttribute(existingAttributes, name, value);
  setFrontendAttrs(op, existingAttributes);
}

void setFrontendAttribute(FuncOp funcOp, StringRef name, Attribute value,
                          int64_t argNum) {
  SmallVector<NamedAttribute> existingAttributes =
      getExistingFrontendAttributes(getFuncArgFrontendAttrs(funcOp, argNum),
                                    "");
  setFrontendAttribute(existingAttributes, name, value);
  setFuncArgFrontendAttrs(funcOp, argNum, existingAttributes);
}

void removeFrontendAttribute(Operation* op, StringRef attributeName) {
  removeFrontendAttribute(
      getFrontendAttrs(op), attributeName,
      [&](ArrayRef<NamedAttribute> newDict) { setFrontendAttrs(op, newDict); },
      [&]() { op->removeAttr(kFrontendAttributesAttr); });
}

void removeFrontendAttribute(FuncOp funcOp, StringRef attributeName,
                             int64_t argNum) {
  removeFrontendAttribute(
      getFuncArgFrontendAttrs(funcOp, argNum), attributeName,
      [&](ArrayRef<NamedAttribute> newDict) {
        setFuncArgFrontendAttrs(funcOp, argNum, newDict);
      },
      [&]() { funcOp.removeArgAttr(argNum, kFrontendAttributesAttr); });
}

bool hasFrontendAttr(mlir::Operation* op, mlir::StringRef key) {
  return hasKey(getFrontendAttrs(op), key);
}

bool hasKey(mlir::DictionaryAttr dictAttr, mlir::StringRef key) {
  return dictAttr && dictAttr.contains(key);
}

void loadAllRequiredDialects(mlir::MLIRContext* context) {
  mlir::DialectRegistry registry;
  mlir::func::registerAllExtensions(registry);
  mlir::mhlo::registerAllMhloDialects(registry);
  registerMhloExtensions(registry);
  mlir::sdy::registerAllDialects(registry);
  context->appendDialectRegistry(registry);
  context->loadAllAvailableDialects();
}

CustomCallOp cloneCustomCallWithNewResultTypes(CustomCallOp op,
                                               mlir::TypeRange resultTypes,
                                               mlir::IRRewriter& rewriter) {
  auto customCallOp = rewriter.create<CustomCallOp>(
      op.getLoc(), resultTypes, op.getOperands(), op.getCallTargetNameAttr(),
      op.getHasSideEffectAttr(), op.getBackendConfigAttr(),
      op.getApiVersionAttr(), op.getCalledComputations(),
      op.getOperandLayoutsAttr(), op.getResultLayoutsAttr(),
      op.getOutputOperandAliases());
  customCallOp->setDiscardableAttrs(mlir::DictionaryAttr::get(
      op->getContext(), llvm::to_vector(op->getDiscardableAttrs())));
  return customCallOp;
};

bool isPythonCallbackCustomCall(mlir::stablehlo::CustomCallOp op) {
  mlir::StringRef targetName = op.getCallTargetName();
  return targetName == kPythonCpuCallbackCustomCallTargetName ||
         targetName == kPythonGpuCallbackCustomCallTargetName ||
         targetName == kFFIPythonCpuCallbackCustomCallTargetName ||
         targetName == kFFIPythonGpuCallbackCustomCallTargetName;
}

std::string duplicateShardingsAtIndices(
    mlir::StringRef shardingsFrontendAttr,
    const llvm::BitVector& indicesToDuplicate) {
  auto context = std::make_unique<mlir::MLIRContext>(
      mlir::MLIRContext::Threading::DISABLED);
  context->loadDialect<mlir::sdy::SdyDialect>();
  auto shardingPerValue = parseStringAttr<TensorShardingPerValueAttr>(
      shardingsFrontendAttr, context.get());
  CHECK(shardingPerValue);
  SmallVector<TensorShardingAttr> newShardings;
  newShardings.reserve(shardingPerValue.size());
  for (auto [index, sharding] :
       llvm::enumerate(shardingPerValue.getShardings())) {
    newShardings.push_back(sharding);
    if (indicesToDuplicate.test(index)) {
      newShardings.push_back(sharding);
    }
  }
  return mlir::sdy::attributeToString(
      TensorShardingPerValueAttr::get(context.get(), newShardings));
}

SmallVector<AxisRefAttr> getOrderedAxisRefs(Attribute shardingOrAxisList,
                                            MeshAttr mesh) {
  // We use a map vector to maintain the order of mesh axes.
  llvm::MapVector<StringRef, SmallVector<int64_t>> axisNameToPreSizes;
  axisNameToPreSizes.reserve(mesh.getAxes().size());
  for (MeshAxisAttr meshAxis : mesh.getAxes()) {
    SmallVector<int64_t>& preSizes = axisNameToPreSizes[meshAxis.getName()];
    preSizes.push_back(1);
    preSizes.push_back(meshAxis.getSize());
  }

  auto consumeAxisRefList = [&](ArrayRef<AxisRefAttr> axisRefs) {
    for (AxisRefAttr axisRef : axisRefs) {
      // Add sub-axis pre-sizes to `axisNameToPreSizes`. We'll dedup later.
      if (axisRef.getSubAxisInfo()) {
        SmallVector<int64_t>& preSizes = axisNameToPreSizes[axisRef.getName()];
        preSizes.push_back(axisRef.getSubAxisInfo().getPreSize());
        preSizes.push_back(axisRef.getSubAxisInfo().getNextPreSize());
      }
    }
  };

  if (auto sharding = mlir::dyn_cast<TensorShardingAttr>(shardingOrAxisList)) {
    for (DimensionShardingAttr dimSharding : sharding.getDimShardings()) {
      consumeAxisRefList(dimSharding.getAxes());
    }
  } else {
    consumeAxisRefList(
        mlir::cast<AxisRefListAttr>(shardingOrAxisList).getValue());
  }

  SmallVector<AxisRefAttr> axisRefs;
  mlir::MLIRContext* ctx = mesh.getContext();
  for (auto& [axisName, preSizes] : axisNameToPreSizes) {
    if (preSizes.size() == 2) {
      // Full axis
      axisRefs.push_back(AxisRefAttr::get(ctx, axisName));
      continue;
    }
    llvm::sort(preSizes);
    preSizes.erase(std::unique(preSizes.begin(), preSizes.end()),
                   preSizes.end());
    for (int64_t i = 0; i < preSizes.size() - 1; ++i) {
      int64_t preSize = preSizes[i];
      int64_t size = preSizes[i + 1] / preSize;
      axisRefs.push_back(AxisRefAttr::get(
          ctx, axisName, SubAxisInfoAttr::get(ctx, preSize, size)));
    }
  }

  return axisRefs;
}

}  // namespace sdy
}  // namespace xla
