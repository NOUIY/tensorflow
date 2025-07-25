/* Copyright 2020 The OpenXLA Authors.

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

#include "xla/service/gpu/ir_emission_utils.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/container/inlined_vector.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/testlib/hlo_hardware_independent_test_base.h"
#include "xla/hlo/utils/hlo_traversal.h"
#include "xla/literal.h"
#include "xla/literal_util.h"
#include "xla/service/buffer_assignment.h"
#include "xla/service/gpu/backend_configs.pb.h"
#include "xla/service/gpu/ir_emission_utils.pb.h"
#include "xla/shape_util.h"
#include "xla/tsl/platform/status_matchers.h"
#include "xla/tsl/platform/statusor.h"
#include "xla/tsl/protobuf/dnn.pb.h"
#include "xla/types.h"

namespace xla {
namespace gpu {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::SizeIs;
using ::tsl::testing::IsOkAndHolds;

class IrEmissionUtilsTest : public HloHardwareIndependentTestBase {
 public:
  TransposeSpec GetTransposeSpecFromRoot(absl::string_view hlo_text) {
    auto module = ParseAndReturnVerifiedModule(hlo_text).value();
    auto* root = module->entry_computation()->root_instruction();
    return GetTransposeSpec(Cast<HloTransposeInstruction>(root));
  }
};

using InlinedVector = absl::InlinedVector<int64_t, 3>;

TEST_F(IrEmissionUtilsTest, FindTiledLogicalTranspose) {
  const char* hlo = R"(
HloModule module

ENTRY entry {
  p = f32[1536,64]{1,0} parameter(0)
  ROOT t = f32[64,1536]{1,0} transpose(p), dimensions={1,0}
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  HloInstruction* tr = module->entry_computation()->root_instruction();

  auto result = GetDescriptionForTiledTransposeEmitter(*tr);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, tr);
  EXPECT_EQ(result->dimensions, InlinedVector({64, 1536}));
  EXPECT_EQ(result->permutation, InlinedVector({1, 0}));
}

TEST_F(IrEmissionUtilsTest, FindTiledLogical102Transpose) {
  const char* hlo = R"(
HloModule module

ENTRY entry {
  p = f32[32,48,2]{2,1,0} parameter(0)
  ROOT t = f32[48,32,2]{2,1,0} transpose(p), dimensions={1,0,2}
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  HloInstruction* tr = module->entry_computation()->root_instruction();

  auto result = GetDescriptionForTiledTransposeEmitter(*tr);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, tr);
  EXPECT_EQ(result->dimensions, InlinedVector({48, 32, 2}));
  EXPECT_EQ(result->permutation, InlinedVector({1, 0, 2}));
}

TEST_F(IrEmissionUtilsTest, FindTiledLogical102TransposeTooMuchMemoryRequired) {
  const char* hlo = R"(
HloModule module

ENTRY entry {
  p = s8[32,48,9]{2,1,0} parameter(0)
  ROOT t = s8[48,32,9]{2,1,0} transpose(p), dimensions={1,0,2}
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  HloInstruction* tr = module->entry_computation()->root_instruction();

  auto result = GetDescriptionForTiledTransposeEmitter(*tr);
  EXPECT_FALSE(result.has_value());
}

TEST_F(IrEmissionUtilsTest, FindTiledLogical2103Transpose) {
  const char* hlo = R"(
HloModule module

ENTRY entry {
  p = f32[33,48,32,2]{3,2,1,0} parameter(0)
  ROOT t = f32[32,48,33,2]{3,2,1,0} transpose(p), dimensions={2,1,0,3}
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  HloInstruction* tr = module->entry_computation()->root_instruction();

  auto result = GetDescriptionForTiledTransposeEmitter(*tr);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, tr);
  EXPECT_EQ(result->dimensions, InlinedVector({32, 48, 33, 2}));
  EXPECT_EQ(result->permutation, InlinedVector({2, 1, 0, 3}));
}

TEST_F(IrEmissionUtilsTest, FindTiledLogical1320Transpose) {
  const char* hlo = R"(
HloModule module

ENTRY entry {
  p = f32[33,48,32,34]{3,2,1,0} parameter(0)
  ROOT t = f32[48,34,32,33]{3,2,1,0} transpose(p), dimensions={1,3,2,0}
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  HloInstruction* tr = module->entry_computation()->root_instruction();

  auto result = GetDescriptionForTiledTransposeEmitter(*tr);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, tr);
  EXPECT_EQ(result->dimensions, InlinedVector({48, 34, 32, 33}));
  EXPECT_EQ(result->permutation, InlinedVector({1, 3, 2, 0}));
}

TEST_F(IrEmissionUtilsTest, FindAnyTiledTranspose) {
  const char* hlo = R"(
HloModule module

ENTRY entry {
  p = f32[32,48,64]{2,1,0} parameter(0)
  ROOT t = f32[64,48,32]{2,1,0} transpose(p), dimensions={2,1,0}
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r = module->entry_computation()->root_instruction();
  auto result = GetDescriptionForTiledTransposeEmitter(*r);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, r);
  EXPECT_EQ(result->dimensions, InlinedVector({64, 48, 32}));
  EXPECT_EQ(result->permutation, InlinedVector({2, 1, 0}));
}

TEST_F(IrEmissionUtilsTest, FindAnyTiledTransposeWithIntermediateUnaryOp) {
  const char* hlo = R"(
HloModule module

ENTRY entry {
  p = f32[32,48,64]{2,1,0} parameter(0)
  t = f32[64,48,32]{2,1,0} transpose(p), dimensions={2,1,0}
  ROOT n = f32[64,48,32]{2,1,0} negate(t)
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r = module->entry_computation()->root_instruction();
  auto result = GetDescriptionForTiledTransposeEmitter(*r->operand(0));
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, r->operand(0));
  EXPECT_EQ(result->dimensions, InlinedVector({64, 48, 32}));
  EXPECT_EQ(result->permutation, InlinedVector({2, 1, 0}));
}

TEST_F(IrEmissionUtilsTest, FindAnyTiledTransposeWithIntermediateUnaryOpS8) {
  const char* hlo = R"(
HloModule module

fusion {
  p = f32[32,48,64]{2,1,0} parameter(0)
  t = f32[64,48,32]{2,1,0} transpose(p), dimensions={2,1,0}
  ROOT c = s8[64,48,32]{2,1,0} convert(t)
}

ENTRY main {
  p0 = f32[32,48,64]{2,1,0} parameter(0)
  ROOT f = s8[64,48,32]{2,1,0} fusion(p0), kind=kInput, calls=fusion
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r =
      module->entry_computation()->root_instruction()->fused_expression_root();
  auto result = GetDescriptionForTiledTransposeEmitter(*r->operand(0));
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, r->operand(0));
  EXPECT_EQ(result->dimensions, InlinedVector({64, 48, 32}));
  EXPECT_EQ(result->permutation, InlinedVector({2, 1, 0}));
}

TEST_F(IrEmissionUtilsTest, FindReduceHeroEpilogueFusion) {
  const char* hlo = R"(
    HloModule module

    %add {
      %x = f32[] parameter(0)
      %y = f32[] parameter(1)
      ROOT %add = f32[] add(%x, %y)
    }

    %fused_computation (param_0.4: f32[128,64], param_1.4: bf16[]) -> bf16[64] {
      %param_0 = f32[128,64]{1,0} parameter(0)
      %param_1 = bf16[] parameter(1)
      %convert.0 = f32[] convert(bf16[] %param_1)
      %reduce.0 = f32[64]{0} reduce(f32[128,64]{1,0} %param_0, f32[] %convert.0), dimensions={0}, to_apply=%add
      ROOT %convert.1 = bf16[64]{0} convert(f32[64]{0} %reduce.0)
    }

    ENTRY %main {
      %param_0 = f32[128,64]{1,0} parameter(0)
      %param_1 = bf16[] parameter(1)
      ROOT fusion = bf16[64]{0} fusion(%param_0, %param_1), kind=kInput, calls=fused_computation
    }
    )";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r = module->entry_computation()->root_instruction();
  auto fusion = HloFusionAdaptor::ForInstruction(r);
  const auto& result = FindNonTrivialHero(fusion->GetRoots()[0]);
  EXPECT_EQ(result.name(), "reduce.0");
}

TEST_F(IrEmissionUtilsTest, FindReduceHeroEpilogueFusionTwoRootUsers) {
  const char* hlo = R"(
    HloModule module

    Add {
      %x = f32[] parameter(0)
      %y = f32[] parameter(1)
      ROOT %add = f32[] add(%x, %y)
    }
    fused_computation {
      param_0 = f32[4,2]{1,0} parameter(0)
      neg = f32[4,2]{1,0} negate(param_0)
      constant_0 = f32[] constant(0)
      reduce.1 = f32[4]{0} reduce(param_0, constant_0), dimensions={1}, to_apply=Add
      bitcast.1 = f32[1,1,4]{2,1,0} bitcast(reduce.1)
      sign.1 = f32[1,1,4]{2,1,0} sign(bitcast.1)
      ROOT tuple.12 = (f32[4,2]{1,0}, f32[1,1,4]{2,1,0}, f32[1,1,4]{2,1,0}) tuple(neg, bitcast.1, sign.1)
    }

    ENTRY main.7749 {
      Arg_2.1 = f32[4,2]{1,0} parameter(0)
      ROOT fusion = (f32[4,2]{1,0}, f32[1,1,4]{2,1,0}, f32[1,1,4]{2,1,0}) fusion(Arg_2.1), kind=kInput, calls=fused_computation
    }
    )";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r = module->entry_computation()->root_instruction();
  auto fusion = HloFusionAdaptor::ForInstruction(r);
  const auto& result = FindNonTrivialHero(fusion->GetRoots()[1]);
  EXPECT_EQ(result.name(), "reduce.1");
  const auto& result2 = FindNonTrivialHero(fusion->GetRoots()[2]);
  EXPECT_EQ(result2.name(), "reduce.1");
}

TEST_F(IrEmissionUtilsTest, FindReduceHeroEpilogueFusionHeroAlsoUsedAsNonHero) {
  const char* hlo = R"(
    HloModule module

    Add {
      x = f32[] parameter(0)
      y = f32[] parameter(1)
      ROOT add = f32[] add(x, y)
    }

    fused_computation {
      p0 = f32[4]{0} parameter(0)
      zero = f32[] constant(0.0)
      reduce.0 = f32[] reduce(f32[4]{0} p0, f32[] zero), dimensions={0}, to_apply=Add
      broadcast = f32[4]{0} broadcast(f32[] reduce.0), dimensions={}
      reduce.1 = f32[] reduce(f32[4]{0} broadcast, f32[] zero), dimensions={0}, to_apply=Add
      bitcast = f32[1]{0} bitcast(f32[] reduce.0)
      ROOT tuple.1 = (f32[], f32[4]{0}, f32[1]{0}) tuple(f32[] reduce.1, f32[4]{0} broadcast, f32[1]{0} bitcast)
    }

    ENTRY main {
      Arg0 = f32[4]{0} parameter(0)
      ROOT fusion = (f32[], f32[4]{0}, f32[1]{0}) fusion(Arg0), kind=kInput, calls=fused_computation
    })";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r = module->entry_computation()->root_instruction();
  auto fusion = HloFusionAdaptor::ForInstruction(r);
  const auto& result = FindNonTrivialHero(fusion->GetRoots()[1]);
  // reduce.0 is also an operand of broadcast, but it is not a hero for that
  // root.
  EXPECT_EQ(result.name(), "broadcast");
  const auto& result2 = FindNonTrivialHero(fusion->GetRoots()[2]);
  EXPECT_EQ(result2.name(), "reduce.0");
}

TEST_F(IrEmissionUtilsTest, FindAnyTiledTransposeWithIntermediateBinaryOp) {
  const char* hlo = R"(
HloModule module

ENTRY entry {
  p = f32[32,48,64]{2,1,0} parameter(0)
  p2 = f32[64,48,32]{2,1,0} parameter(1)
  t = f32[64,48,32]{2,1,0} transpose(p), dimensions={2,1,0}
  ROOT add = f32[64,48,32]{2,1,0} add(t, p2)
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r = module->entry_computation()->root_instruction();

  auto result = GetDescriptionForTiledTransposeEmitter(*r->operand(0));
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, r->operand(0));
  EXPECT_EQ(result->dimensions, InlinedVector({64, 48, 32}));
  EXPECT_EQ(result->permutation, InlinedVector({2, 1, 0}));
}

TEST_F(IrEmissionUtilsTest, FindAnyTiledTransposeWithTwoIntermediateBinaryOps) {
  const char* hlo = R"(
HloModule module

fusion {
  p = f32[32,48,64]{2,1,0} parameter(0)
  p2 = f32[64,48,32]{2,1,0} parameter(1)
  t = f32[64,48,32]{2,1,0} transpose(p), dimensions={2,1,0}
  mul = f32[64,48,32]{2,1,0} multiply(t, p2)
  ROOT add = f32[64,48,32]{2,1,0} add(mul, p2)
}

ENTRY main {
  param0 = f32[32,48,64]{2,1,0} parameter(0)
  param1 = f32[64,48,32]{2,1,0} parameter(1)
  ROOT fusion = f32[64,48,32]{2,1,0} fusion(param0, param1), kind=kInput, calls=fusion
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r =
      module->entry_computation()->root_instruction()->fused_expression_root();
  auto result = GetDescriptionForTiledTransposeEmitter(FindNonTrivialHero(*r));
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, r->operand(0)->operand(0));
  EXPECT_EQ(result->dimensions, InlinedVector({64, 48, 32}));
  EXPECT_EQ(result->permutation, InlinedVector({2, 1, 0}));
}

TEST_F(IrEmissionUtilsTest,
       FindAnyTiledTransposeWithIntermediateBinaryOpTwoTransposes) {
  const char* hlo = R"(
HloModule module

fusion {
  p = f32[32,48,64]{2,1,0} parameter(0)
  p2 = f32[48,32,64]{2,1,0} parameter(1)
  t = f32[64,48,32]{2,1,0} transpose(p), dimensions={2,1,0}
  bc = f32[1,1536,64]{2,1,0} bitcast(p2)
  t2 = f32[1,64,1536]{2,1,0} transpose(bc), dimensions={0,2,1}
  bc2 = f32[64,48,32]{2,1,0} bitcast(t2)
  ROOT add = f32[64,48,32]{2,1,0} add(t, bc2)
}

ENTRY main {
  param0 = f32[32,48,64]{2,1,0} parameter(0)
  param1 = f32[48,32,64]{2,1,0} parameter(1)
  ROOT fusion = f32[64,48,32]{2,1,0} fusion(param0, param1), kind=kInput, calls=fusion
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r =
      module->entry_computation()->root_instruction()->fused_expression_root();
  EXPECT_FALSE(GetDescriptionForTiledTransposeEmitter(FindNonTrivialHero(*r))
                   .has_value());
  EXPECT_EQ(&FindNonTrivialHero(*r), r);
}

TEST_F(IrEmissionUtilsTest, FindNonTrivialHeroOutsideFusion) {
  const char* hlo = R"(
HloModule module

f {
  p0 = f32[100,200,300]{2,1,0} parameter(0)
  ROOT add = f32[100,200,300]{2,1,0} add(p0, p0)
}

ENTRY entry {
  p0 = f32[300,200,100]{2,1,0} parameter(0)
  t = f32[100,200,300]{2,1,0} transpose(p0), dimensions={2,1,0}
  fusion = f32[100,200,300]{2,1,0} fusion(t), kind=kLoop, calls=f
  ROOT add = f32[100,200,300]{2,1,0} add(t, fusion)
}
)";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* transpose =
      module->entry_computation()->GetInstructionWithName("t");
  HloInstruction* fusion =
      module->entry_computation()->GetInstructionWithName("fusion");
  auto fusion_adaptor =
      HloFusionAdaptor::ForProducerConsumer(transpose, fusion);
  HloInstructionAdaptor r(
      *module->GetComputationWithName("f")->root_instruction(),
      fusion_adaptor.get());
  EXPECT_EQ(&FindNonTrivialHero(r).instruction(), transpose);
}

TEST_F(IrEmissionUtilsTest, FindNonTrivialTransposeHeroInsideFusion) {
  const char* hlo = R"(
HloModule module

f {
  p0 = f32[300,200,100]{2,1,0} parameter(0)
  t = f32[100,200,300]{2,1,0} transpose(p0), dimensions={2,1,0}
  ROOT add = f32[100,200,300]{2,1,0} add(t, t)
}

ENTRY entry {
  p0 = f32[300,200,100]{2,1,0} parameter(0)
  p1 = f32[100,200,300]{2,1,0} parameter(1)
  fusion = f32[100,200,300]{2,1,0} fusion(p0), kind=kLoop, calls=f
  ROOT add = f32[100,200,300]{2,1,0} add(p1, fusion)
}
)";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r = module->entry_computation()->root_instruction();
  HloInstruction* transpose = module->GetComputationWithName("f")
                                  ->parameter_instruction(0)
                                  ->users()
                                  .front();
  HloInstruction* fusion =
      module->entry_computation()->GetInstructionWithName("fusion");
  auto fusion_adaptor = HloFusionAdaptor::ForProducerConsumer(fusion, r);
  EXPECT_EQ(&FindNonTrivialHero(HloInstructionAdaptor(*r, fusion_adaptor.get()))
                 .instruction(),
            transpose);
}

TEST_F(IrEmissionUtilsTest, TransposeReachableViaTrivialAndNontrivialOps) {
  const char* hlo = R"(
HloModule module

fusion {
  p = f64[16,16]{1,0} parameter(0)
  trans = f64[16,16]{1,0} transpose(p), dimensions={1,0}
  rev = f64[16,16]{1,0} reverse(trans), dimensions={0,1}
  sub = f64[16,16]{1,0} subtract(trans, trans)
  ROOT add = f64[16,16]{1,0} add(rev, sub)
}

ENTRY main {
  param = f64[16,16]{1,0} parameter(0)
  ROOT fusion = f64[16,16]{1,0} fusion(param), kind=kLoop, calls=fusion
}
)";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* r =
      module->entry_computation()->root_instruction()->fused_expression_root();
  EXPECT_FALSE(GetDescriptionForTiledTransposeEmitter(FindNonTrivialHero(*r))
                   .has_value());
  EXPECT_EQ(&FindNonTrivialHero(*r), r);
}

TEST_F(IrEmissionUtilsTest, FindTiledLogicalTransposeOneSwapDimIsSmall) {
  const char* hlo = R"(
HloModule module

fusion {
  p = f32[1100,12,8]{2,1,0} parameter(0)
  ROOT t = f32[8,12,1100]{2,1,0} transpose(p), dimensions={2,1,0}
}

ENTRY main {
  param = f32[1100,12,8]{2,1,0} parameter(0)
  ROOT fusion = f32[8,12,1100]{2,1,0} fusion(param), kind=kInput, calls=fusion
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* tr =
      module->entry_computation()->root_instruction()->fused_expression_root();
  auto result = GetDescriptionForTiledTransposeEmitter(FindNonTrivialHero(*tr));
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, tr);
  EXPECT_EQ(result->dimensions, InlinedVector({8, 12, 1100}));
  EXPECT_EQ(result->permutation, InlinedVector({2, 1, 0}));
}

TEST_F(IrEmissionUtilsTest, FindTiledLogicalTransposeOtherSwapDimIsSmall) {
  const char* hlo = R"(
HloModule module

fusion {
  p = f32[8,12,1100]{2,1,0} parameter(0)
  ROOT t = f32[1100,12,8]{2,1,0} transpose(p), dimensions={2,1,0}
}

ENTRY main {
  param = f32[8,12,1100]{2,1,0} parameter(0)
  ROOT fusion = f32[1100,12,8]{2,1,0} fusion(param), kind=kInput, calls=fusion
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* tr =
      module->entry_computation()->root_instruction()->fused_expression_root();
  auto result = GetDescriptionForTiledTransposeEmitter(FindNonTrivialHero(*tr));
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result->instr, tr);
  EXPECT_EQ(result->dimensions, InlinedVector({1100, 12, 8}));
  EXPECT_EQ(result->permutation, InlinedVector({2, 1, 0}));
}

TEST_F(IrEmissionUtilsTest, IsContiguousSlice) {
  const char* hlo = R"(
HloModule module

ENTRY entry {
  p0 = f32[8,12,100,11]{3,2,1,0} parameter(0)
  p1 = f32[4]{0} parameter(1)
  c = f32[8,12,100,11]{0,1,3,2} copy(p0)
  slice.1 = f32[2,12,100,11]{3,2,1,0} slice(p0), slice={[1:3], [0:12], [0:100], [0:11]}
  slice.2 = f32[1,1,1,11]{3,2,1,0} slice(p0), slice={[1:2], [0:1], [0:1], [0:11]}
  slice.3 = f32[1,1,10,11]{3,2,1,0} slice(p0), slice={[1:2], [0:1], [0:10], [0:11]}
  slice.4 = f32[1,2,10,11]{3,2,1,0} slice(p0), slice={[1:2], [0:2], [0:10], [0:11]}
  slice.5 = f32[8,2,100,11]{3,2,1,0} slice(p0), slice={[0:8], [10:12], [0:100], [0:11]}
  slice.6 = f32[8,12,40,11]{0,1,3,2} slice(c), slice={[0:8], [0:12], [10:50], [0:11]}
  slice.7 = f32[8,12,1,2]{0,1,3,2} slice(c), slice={[0:8], [0:12], [0:1], [0:2]}
  slice.8 = f32[8,2,100,11]{0,1,3,2} slice(c), slice={[0:8], [0:2], [0:100], [0:11]}
  slice.9 = f32[8,2,40,11]{0,1,3,2} slice(c), slice={[0:8], [10:12], [10:50], [0:11]}
  slice.10 = f32[8,2,50,11]{3,2,1,0} slice(p0), slice={[0:8:1], [10:12:1], [0:100:2], [0:11:1]}
  slice.11 = f32[2]{0} slice(p1), slice={[0:3:2]}
  slice.12 = f32[1]{0} slice(p1), slice={[0:1:2]}
  ROOT t = (f32[2,12,100,11]{3,2,1,0},
            f32[1,1,1,11]{3,2,1,0},
            f32[1,1,10,11]{3,2,1,0},
            f32[1,2,10,11]{3,2,1,0},
            f32[8,2,100,11]{3,2,1,0},
            f32[8,12,40,11]{0,1,3,2},
            f32[8,12,1,2]{0,1,3,2},
            f32[8,2,100,11]{0,1,3,2},
            f32[8,2,40,11]{0,1,3,2},
            f32[8,2,50,11]{3,2,1,0},
            f32[2]{0},
            f32[1]{0}) tuple(slice.1, slice.2, slice.3, slice.4, slice.5, slice.6, slice.7, slice.8, slice.9, slice.10, slice.11, slice.12)
}
)";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));

  HloInstruction* slice1 =
      module->entry_computation()->GetInstructionWithName("slice.1");
  HloInstruction* slice2 =
      module->entry_computation()->GetInstructionWithName("slice.2");
  HloInstruction* slice3 =
      module->entry_computation()->GetInstructionWithName("slice.3");
  HloInstruction* slice4 =
      module->entry_computation()->GetInstructionWithName("slice.4");
  HloInstruction* slice5 =
      module->entry_computation()->GetInstructionWithName("slice.5");
  HloInstruction* slice6 =
      module->entry_computation()->GetInstructionWithName("slice.6");
  HloInstruction* slice7 =
      module->entry_computation()->GetInstructionWithName("slice.7");
  HloInstruction* slice8 =
      module->entry_computation()->GetInstructionWithName("slice.8");
  HloInstruction* slice9 =
      module->entry_computation()->GetInstructionWithName("slice.9");
  HloInstruction* slice10 =
      module->entry_computation()->GetInstructionWithName("slice.10");
  HloInstruction* slice11 =
      module->entry_computation()->GetInstructionWithName("slice.11");
  HloInstruction* slice12 =
      module->entry_computation()->GetInstructionWithName("slice.12");
  EXPECT_TRUE(IsContiguousSlice(*slice1));
  EXPECT_TRUE(IsContiguousSlice(*slice2));
  EXPECT_TRUE(IsContiguousSlice(*slice3));
  EXPECT_FALSE(IsContiguousSlice(*slice4));
  EXPECT_FALSE(IsContiguousSlice(*slice5));
  EXPECT_TRUE(IsContiguousSlice(*slice6));
  EXPECT_TRUE(IsContiguousSlice(*slice7));
  EXPECT_FALSE(IsContiguousSlice(*slice8));
  EXPECT_FALSE(IsContiguousSlice(*slice9));
  EXPECT_FALSE(IsContiguousSlice(*slice10));
  EXPECT_FALSE(IsContiguousSlice(*slice11));
  EXPECT_TRUE(IsContiguousSlice(*slice12));
}

TEST_F(IrEmissionUtilsTest, LiteralToAttrToXlaFormat) {
  // int16, should be aliased.
  {
    Literal literal = LiteralUtil::CreateR2<int16_t>({{0, 1, 2}, {3, 4, 5}});

    TF_ASSERT_OK_AND_ASSIGN(DenseDataIntermediate data,
                            LiteralToXlaFormat(literal));
    EXPECT_EQ(data.span().size(), literal.size_bytes());
    EXPECT_EQ(reinterpret_cast<const char*>(data.span().data()),
              literal.untyped_data());
  }

  // int4, even, should be a new (unaliased) packed array.
  {
    Literal literal = LiteralUtil::CreateR2<s4>(
        {{s4(0), s4(1), s4(2)}, {s4(3), s4(4), s4(5)}});

    TF_ASSERT_OK_AND_ASSIGN(DenseDataIntermediate data,
                            LiteralToXlaFormat(literal));
    EXPECT_EQ(data.span(), std::vector<uint8_t>({0x10, 0x32, 0x54}));
    EXPECT_NE(reinterpret_cast<const void*>(data.span().data()),
              literal.untyped_data());
  }

  // int4, odd, should be a new (unaliased) packed array.
  {
    Literal literal = LiteralUtil::CreateR2<u4>(
        {{u4(0), u4(1), u4(2)}, {u4(3), u4(4), u4(5)}, {u4(6), u4(7), u4(8)}});

    TF_ASSERT_OK_AND_ASSIGN(DenseDataIntermediate data,
                            LiteralToXlaFormat(literal));
    EXPECT_EQ(data.span(),
              std::vector<uint8_t>({0x10, 0x32, 0x54, 0x76, 0x08}));
    EXPECT_NE(reinterpret_cast<const void*>(data.span().data()),
              literal.untyped_data());
  }
}

TEST_F(IrEmissionUtilsTest,
       CanEmitFusedDynamicUpdateSliceInPlaceForGpu_HandlesBitcasts) {
  const char* hlo = R"(
HloModule fusion, is_scheduled=true

fused_computation {
  param_0.1 = s32[6]{0} parameter(0)
  bitcast = s32[2,3]{1,0} bitcast(param_0.1)
  zero = s32[] constant(0)
  param_1.1 = s32[] parameter(1)
  dynamic-slice = s32[1,1]{1,0} dynamic-slice(bitcast, param_1.1, zero), dynamic_slice_sizes={1,1}
  one = s32[] constant(1)
  bitcasted_one = s32[1,1]{1,0} bitcast(one)
  add = s32[1,1] add(dynamic-slice, bitcasted_one)
  dynamic-update-slice = s32[2,3]{1,0} dynamic-update-slice(bitcast, add, param_1.1, zero)
  ROOT bitcast.1 = s32[6]{0} bitcast(dynamic-update-slice)
}

ENTRY main {
  param_0 = s32[6]{0} parameter(0)
  param_1 = s32[] parameter(1)
  ROOT fusion = s32[6]{0} fusion(param_0, param_1), kind=kInput, calls=fused_computation
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  auto fusion = module->entry_computation()->root_instruction();
  BufferAllocation alloc(/*index=*/0, /*size=*/1024, /*color=*/0);
  BufferAllocation::Slice slice0(&alloc, 0, 10);
  auto adaptor = HloFusionAdaptor::ForInstruction(fusion);
  EXPECT_THAT(CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
                  *adaptor,
                  [&slice0](const HloInstruction*, const ShapeIndex&) {
                    return slice0;
                  },
                  fusion),
              IsOkAndHolds(true));
}

TEST_F(
    IrEmissionUtilsTest,
    CanEmitFusedDynamicUpdateSliceInPlaceForGpu_ElementwiseOnPathToParameter) {
  const char* hlo = R"(
HloModule fusion, is_scheduled=true

fused_computation {
  param_0.1 = s32[2,3]{1,0} parameter(0)
  bitcast = s32[2,3]{1,0} negate(param_0.1)
  zero = s32[] constant(0)
  param_1.1 = s32[] parameter(1)
  dynamic-slice = s32[1,1]{1,0} dynamic-slice(bitcast, param_1.1, zero), dynamic_slice_sizes={1,1}
  one = s32[] constant(1)
  bitcasted_one = s32[1,1]{1,0} bitcast(one)
  add = s32[1,1] add(dynamic-slice, bitcasted_one)
  dynamic-update-slice = s32[2,3]{1,0} dynamic-update-slice(bitcast, add, param_1.1, zero)
  ROOT bitcast.1 = s32[6]{0} bitcast(dynamic-update-slice)
}

ENTRY main {
  param_0 = s32[2,3]{1,0} parameter(0)
  param_1 = s32[] parameter(1)
  ROOT fusion = s32[6]{0} fusion(param_0, param_1), kind=kInput, calls=fused_computation
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  auto fusion = module->entry_computation()->root_instruction();
  BufferAllocation alloc(/*index=*/0, /*size=*/1024, /*color=*/0);
  BufferAllocation::Slice slice0(&alloc, 0, 10);
  auto adaptor = HloFusionAdaptor::ForInstruction(fusion);
  EXPECT_THAT(CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
                  *adaptor,
                  [&slice0](const HloInstruction*, const ShapeIndex&) {
                    return slice0;
                  },
                  fusion),
              IsOkAndHolds(false));
}

// Same test as above, but different allocation slices for parameter and output.
TEST_F(IrEmissionUtilsTest,
       CanEmitFusedDynamicUpdateSliceInPlaceForGpu_SlicesDifferent) {
  const char* hlo = R"(
HloModule fusion, is_scheduled=true

fused_computation {
  param_0.1 = s32[6]{0} parameter(0)
  bitcast = s32[2,3]{1,0} bitcast(param_0.1)
  zero = s32[] constant(0)
  param_1.1 = s32[] parameter(1)
  dynamic-slice = s32[1,1]{1,0} dynamic-slice(bitcast, param_1.1, zero), dynamic_slice_sizes={1,1}
  one = s32[] constant(1)
  bitcasted_one = s32[1,1]{1,0} bitcast(one)
  add = s32[1,1] add(dynamic-slice, bitcasted_one)
  dynamic-update-slice = s32[2,3]{1,0} dynamic-update-slice(bitcast, add, param_1.1, zero)
  ROOT bitcast.1 = s32[6]{0} bitcast(dynamic-update-slice)
}

ENTRY main {
  param_0 = s32[6]{0} parameter(0)
  param_1 = s32[] parameter(1)
  ROOT fusion = s32[6]{0} fusion(param_0, param_1), kind=kInput, calls=fused_computation
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  auto fusion = module->entry_computation()->root_instruction();
  BufferAllocation alloc(/*index=*/0, /*size=*/1024, /*color=*/0);
  BufferAllocation::Slice slice0(&alloc, 0, 10);
  BufferAllocation::Slice slice1(&alloc, 10, 20);
  auto adaptor = HloFusionAdaptor::ForInstruction(fusion);
  EXPECT_THAT(CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
                  *adaptor,
                  [fusion, &slice0, &slice1](const HloInstruction* instr,
                                             const ShapeIndex&) {
                    if (instr == fusion) {
                      return slice0;
                    }
                    return slice1;
                  },
                  fusion),
              IsOkAndHolds(false));
}

TEST_F(
    IrEmissionUtilsTest,
    CanEmitFusedDynamicUpdateSliceInPlaceForGpu_DynamicUpdateSliceWithDifferentDynamicSliceAccess) {  // NOLINT
  const char* hlo = R"(
HloModule fusion, input_output_alias={ {}: (0, {}) }

fused_computation {
  param_0.1 = s32[6]{0} parameter(0)
  bitcast = s32[2,3]{1,0} bitcast(param_0.1)
  zero = s32[] constant(0)
  one = s32[] constant(1)
  param_1.1 = s32[] parameter(1)
  dynamic-slice = s32[2,2]{1,0} dynamic-slice(bitcast, param_1.1, one), dynamic_slice_sizes={2,2}
  broadcasted_one = s32[2,2]{1,0} broadcast(one), dimensions={}
  add = s32[2,2] add(dynamic-slice, broadcasted_one)
  dynamic-update-slice = s32[2,3]{1,0} dynamic-update-slice(bitcast, add, param_1.1, zero)
  ROOT bitcast.1 = s32[6]{0} bitcast(dynamic-update-slice)
}

ENTRY main {
  param_0 = s32[6]{0} parameter(0)
  param_1 = s32[] parameter(1)
  ROOT fusion = s32[6]{0} fusion(param_0, param_1), kind=kInput, calls=fused_computation
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  auto fusion = module->entry_computation()->root_instruction();
  BufferAllocation alloc(/*index=*/0, /*size=*/1024, /*color=*/0);
  BufferAllocation::Slice slice0(&alloc, 0, 10);
  auto adaptor = HloFusionAdaptor::ForInstruction(fusion);
  EXPECT_THAT(CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
                  *adaptor,
                  [&slice0](const HloInstruction*, const ShapeIndex&) {
                    return slice0;
                  },
                  fusion),
              IsOkAndHolds(false));
}

TEST_F(IrEmissionUtilsTest,
       CanEmitFusedDynamicUpdateSliceInPlaceForGpu_HandlesMultiOutputFusion) {
  const char* hlo = R"(
HloModule MultipleInplaceDus, is_scheduled=true, input_output_alias={ {0}: (0, {}), {1}: (2, {}) }

fused_computation {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[8,11,12] parameter(2)
  p3 = bf16[1,11,12] parameter(3)
  p4 = s32[] parameter(4)
  c0 = s32[] constant(0)
  cmp = pred[] compare(p4, c0), direction=EQ
  broadcast = pred[1,11,12] broadcast(cmp), dimensions={}
  select = bf16[1,11,12] select(broadcast, p1, p3)
  dus0 = bf16[10,11,12] dynamic-update-slice(p0, select, c0, c0, c0)
  dus1 = bf16[8,11,12] dynamic-update-slice(p2, select, c0, c0, c0)
  ROOT tuple = (bf16[10,11,12], bf16[8,11,12]) tuple(dus0, dus1)
}

ENTRY main {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[8,11,12] parameter(2)
  p3 = bf16[1,11,12] parameter(3)
  p4 = s32[] parameter(4)
  ROOT fusion_root_multiple = (bf16[10,11,12], bf16[8,11,12]) fusion(p0, p1, p2, p3, p4), kind=kLoop, calls=fused_computation
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  auto fusion = module->entry_computation()->root_instruction();
  BufferAllocation alloc(/*index=*/0, /*size=*/1024, /*color=*/0);
  BufferAllocation::Slice slice0(&alloc, 0, 10);
  auto adaptor = HloFusionAdaptor::ForInstruction(fusion);
  EXPECT_THAT(CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
                  *adaptor,
                  [&slice0](const HloInstruction*, const ShapeIndex&) {
                    return slice0;
                  },
                  fusion),
              IsOkAndHolds(true));
}

TEST_F(
    IrEmissionUtilsTest,
    CanEmitFusedDynamicUpdateSliceInPlaceForGpu_HandlesMultiOutputFusionSharedParameter) {  // NOLINT
  const char* hlo = R"(
HloModule MultipleInplaceDus, is_scheduled=true, input_output_alias={ {0}: (0, {}), {1}: (2, {}) }

fused_computation {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[1,11,12] parameter(2)
  p3 = s32[] parameter(3)
  c0 = s32[] constant(0)
  cmp = pred[] compare(p3, c0), direction=EQ
  broadcast = pred[1,11,12] broadcast(cmp), dimensions={}
  select = bf16[1,11,12] select(broadcast, p1, p2)
  dus0 = bf16[10,11,12] dynamic-update-slice(p0, select, c0, c0, c0)
  dus1 = bf16[10,11,12] dynamic-update-slice(p0, select, c0, c0, c0)
  ROOT tuple = (bf16[10,11,12], bf16[10,11,12]) tuple(dus0, dus1)
}

ENTRY main {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[1,11,12] parameter(2)
  p3 = s32[] parameter(3)
  ROOT fusion_root_multiple = (bf16[10,11,12], bf16[10,11,12]) fusion(p0, p1, p2, p3), kind=kLoop, calls=fused_computation
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  auto fusion = module->entry_computation()->root_instruction();
  BufferAllocation alloc(/*index=*/0, /*size=*/1024, /*color=*/0);
  BufferAllocation::Slice slice0(&alloc, 0, 10);
  auto adaptor = HloFusionAdaptor::ForInstruction(fusion);
  EXPECT_THAT(CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
                  *adaptor,
                  [&slice0](const HloInstruction*, const ShapeIndex&) {
                    return slice0;
                  },
                  fusion),
              IsOkAndHolds(false));
}

TEST_F(
    IrEmissionUtilsTest,
    CanEmitFusedDynamicUpdateSliceInPlaceForGpu_HandlesMultiOutputFusionWithTransposeBitcasts) {  // NOLINT
  const char* hlo = R"(
HloModule MultipleInplaceDusWithTransposeBitcastToTheRoot, is_scheduled=true, input_output_alias={ {0}: (0, {}), {1}: (2, {}) }

fused_computation {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[8,11,12] parameter(2)
  p3 = bf16[1,11,12] parameter(3)
  p4 = s32[] parameter(4)
  c0 = s32[] constant(0)
  cmp = pred[] compare(p4, c0), direction=EQ
  broadcast = pred[1,11,12] broadcast(cmp), dimensions={}
  select = bf16[1,11,12] select(broadcast, p1, p3)
  dus0 = bf16[10,11,12] dynamic-update-slice(p0, select, c0, c0, c0)
  bitcasted_dus0 = bf16[11,10,12] bitcast(dus0)
  dus1 = bf16[8,11,12] dynamic-update-slice(p2, select, c0, c0, c0)
  ROOT tuple = (bf16[11,10,12], bf16[8,11,12]) tuple(bitcasted_dus0, dus1)
}

ENTRY main {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[8,11,12] parameter(2)
  p3 = bf16[1,11,12] parameter(3)
  p4 = s32[] parameter(4)
  ROOT fusion_root_multiple_transpose_bitcast = (bf16[11,10,12], bf16[8,11,12]) fusion(p0, p1, p2, p3, p4), kind=kLoop, calls=fused_computation
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  auto fusion = module->entry_computation()->root_instruction();
  BufferAllocation alloc(/*index=*/0, /*size=*/1024, /*color=*/0);
  BufferAllocation::Slice slice0(&alloc, 0, 10);
  auto adaptor = HloFusionAdaptor::ForInstruction(fusion);
  EXPECT_THAT(CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
                  *adaptor,
                  [&slice0](const HloInstruction*, const ShapeIndex&) {
                    return slice0;
                  },
                  fusion),
              IsOkAndHolds(true));
}

TEST_F(
    IrEmissionUtilsTest,
    CanEmitFusedDynamicUpdateSliceInPlaceForGpu_HandlesTransposeBitcastToTheRoot) {  // NOLINT
  const char* hlo = R"(
HloModule SingleInplaceDusWithTransposeBitcastToTheRoot, is_scheduled=true, input_output_alias={ {}: (0, {}) }

single_inplace_dus_with_transpose_bitcast {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[1,11,12] parameter(2)
  p3 = s32[] parameter(3)
  c0 = s32[] constant(0)
  cmp = pred[] compare(p3, c0), direction=EQ
  broadcast = pred[1,11,12] broadcast(cmp), dimensions={}
  select = bf16[1,11,12] select(broadcast, p1, p2)
  dus0 = bf16[10,11,12] dynamic-update-slice(p0, select, c0, c0, c0)
  ROOT bitcasted_dus0 = bf16[11,10,12] bitcast(dus0)
}

ENTRY main {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[1,11,12] parameter(2)
  p3 = s32[] parameter(3)
  ROOT fusion_root_transpose_bitcast = bf16[11,10,12] fusion(p0, p1, p2, p3), kind=kLoop, calls=single_inplace_dus_with_transpose_bitcast
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  auto fusion = module->entry_computation()->root_instruction();
  BufferAllocation alloc(/*index=*/0, /*size=*/1024, /*color=*/0);
  BufferAllocation::Slice slice0(&alloc, 0, 10);
  auto adaptor = HloFusionAdaptor::ForInstruction(fusion);
  EXPECT_THAT(CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
                  *adaptor,
                  [&slice0](const HloInstruction*, const ShapeIndex&) {
                    return slice0;
                  },
                  fusion),
              IsOkAndHolds(true));
}

TEST_F(
    IrEmissionUtilsTest,
    CanEmitFusedDynamicUpdateSliceInPlaceForGpu_HandlesReshapeBitcastToTheRoot) {  // NOLINT
  const char* hlo = R"(
HloModule SingleInplaceDusWithReshapeBitcastToTheRoot, is_scheduled=true, input_output_alias={ {}: (0, {}) }

single_inplace_dus_with_reshape_bitcast {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[1,11,12] parameter(2)
  p3 = s32[] parameter(3)
  c0 = s32[] constant(0)
  cmp = pred[] compare(p3, c0), direction=EQ
  broadcast = pred[1,11,12] broadcast(cmp), dimensions={}
  select = bf16[1,11,12] select(broadcast, p1, p2)
  dus0 = bf16[10,11,12] dynamic-update-slice(p0, select, c0, c0, c0)
  ROOT bitcasted_dus0 = bf16[10,11,6,2] bitcast(dus0)
}

ENTRY main {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[1,11,12] parameter(2)
  p3 = s32[] parameter(3)
  ROOT fusion_root_reshape_bitcast = bf16[10,11,6,2] fusion(p0, p1, p2, p3), kind=kLoop, calls=single_inplace_dus_with_reshape_bitcast
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  auto fusion = module->entry_computation()->root_instruction();
  BufferAllocation alloc(/*index=*/0, /*size=*/1024, /*color=*/0);
  BufferAllocation::Slice slice0(&alloc, 0, 10);
  auto adaptor = HloFusionAdaptor::ForInstruction(fusion);
  EXPECT_THAT(CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
                  *adaptor,
                  [&slice0](const HloInstruction*, const ShapeIndex&) {
                    return slice0;
                  },
                  fusion),
              IsOkAndHolds(true));
}

TEST_F(
    IrEmissionUtilsTest,
    CanEmitFusedDynamicUpdateSliceInPlaceForGpu_HandlesBitcastToTheRootAndFromParameter) {  // NOLINT
  const char* hlo = R"(
HloModule SingleInplaceDusWithBitcastToTheRootAndFromTheParameter, is_scheduled=true, input_output_alias={ {}: (0, {}) }

single_inplace_dus_with_bitcast_to_the_root_and_from_the_parameter {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[1,11,12] parameter(2)
  p3 = s32[] parameter(3)
  c0 = s32[] constant(0)
  cmp = pred[] compare(p3, c0), direction=EQ
  broadcast = pred[1,11,12] broadcast(cmp), dimensions={}
  select = bf16[1,11,12] select(broadcast, p1, p2)
  bitcasted_p0 = bf16[10,6,2,11] bitcast(p0)
  bitcasted_select = bf16[1,6,2,11] bitcast(select)
  dus0 = bf16[10,6,2,11] dynamic-update-slice(bitcasted_p0, bitcasted_select, c0, c0, c0, c0)
  ROOT bitcasted_dus0 = bf16[10,11,6,2] bitcast(dus0)
}

ENTRY main {
  p0 = bf16[10,11,12] parameter(0)
  p1 = bf16[1,11,12] parameter(1)
  p2 = bf16[1,11,12] parameter(2)
  p3 = s32[] parameter(3)
  ROOT fusion_root_bitcast_both_ways = bf16[10,11,6,2] fusion(p0, p1, p2, p3), kind=kLoop, calls=single_inplace_dus_with_bitcast_to_the_root_and_from_the_parameter
}
)";
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo));
  auto fusion = module->entry_computation()->root_instruction();
  BufferAllocation alloc(/*index=*/0, /*size=*/1024, /*color=*/0);
  BufferAllocation::Slice slice0(&alloc, 0, 10);
  auto adaptor = HloFusionAdaptor::ForInstruction(fusion);
  EXPECT_THAT(CanEmitFusedDynamicUpdateSliceInPlaceForGpu(
                  *adaptor,
                  [&slice0](const HloInstruction*, const ShapeIndex&) {
                    return slice0;
                  },
                  fusion),
              IsOkAndHolds(true));
}

gpu::GpuBackendConfig CreateTestProto() {
  gpu::GpuBackendConfig proto;
  auto& knobs = *proto.mutable_cudnn_fmha_backend_config()
                     ->mutable_algorithm()
                     ->mutable_tuning_knobs();
  for (int i = 0; i < 10; ++i) {
    knobs[i] = i;
  }
  return proto;
}

constexpr absl::string_view kTestProtoFingerprint =
    "Sj5CPCIECAAQACIECAEQASIECAIQAiIECAMQAyIECAQQBCIECAUQBSIECAYQBiIECAcQByIECA"
    "gQCCIECAkQCQ";

TEST_F(IrEmissionUtilsTest, ProtoFingerprintIsDeterministic) {
  TF_ASSERT_OK_AND_ASSIGN(std::string fingerprint,
                          GetProtoFingerprint(CreateTestProto()));
  EXPECT_EQ(fingerprint, kTestProtoFingerprint);
}

TEST_F(IrEmissionUtilsTest,
       InstructionFingerprintWithBackendConfigIsDeterministic) {
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(R"(
ENTRY e {
  ROOT _ = u8[0] custom-call(), custom_call_target="", backend_config={"cudnn_fmha_backend_config": {"algorithm": {"tuning_knobs": {"0": "0", "1": "1", "2": "2", "3": "3", "4": "4", "5": "5", "6": "6", "7": "7", "8": "8", "9": "9"}}}}
})"));
  const HloInstruction& hlo = *module->entry_computation()->root_instruction();
  TF_ASSERT_OK_AND_ASSIGN(std::string fingerprint,
                          FingerprintWithBackendConfig<GpuBackendConfig>(hlo));
  EXPECT_EQ(fingerprint,
            absl::StrCat("u8[0]{0} custom-call(), custom_call_target=\"\", "
                         "backend_config_fingerprint=",
                         kTestProtoFingerprint));
}

constexpr absl::string_view kWhileLoopTestModule = R"(
    plus_one {
      p0 = s32[] parameter(0)
      p1 = s32[] parameter(1)
      c1 = s32[] constant(0)
      sum = s32[] add(p0, c1)
      ROOT tuple = (s32[], s32[]) tuple(sum, p1)
    }
    identity2 {
      ROOT p0 = s32[] parameter(0)
    }

    remainder {
      p0 = s32[] parameter(0)
      c4 = s32[] constant(4)
      ROOT remainder = s32[] remainder(p0, c4)
    }

    call_body {
      p0 = s32[] parameter(0)
      p1 = s32[] parameter(1)
      p2 = s32[] parameter(2)
      sum = s32[] add(p0, p2)
      called_fusion = (s32[], s32[]) fusion(p1, sum), kind=kLoop, calls=plus_one
      ROOT gte = s32[] get-tuple-element(called_fusion), index=0
    }

    add_values {
      p0 = s32[] parameter(0)
      p1 = s32[] parameter(1)
      ROOT sum = s32[] add(p0, p1)
    }

    while_body {
      p0 = (s32[], s32[]) parameter(0)
      ivar = s32[] get-tuple-element(p0), index=0
      ivar_copy = s32[] copy(ivar)

      side_effect = s32[] custom-call(), custom_call_target=""

      derived = s32[] fusion(ivar_copy), kind=kLoop, calls=remainder
      call = s32[] call(side_effect, derived, ivar), to_apply=call_body

      // `derived_with_invalid_dep` and `not_functionally_dependent` are not, because
      // they have a custom call in their transitive dependencies.
      derived_with_invalid_dep = s32[] fusion(ivar_copy, side_effect), kind=kLoop,
        calls=add_values
      not_functionally_dependent = s32[] fusion(derived_with_invalid_dep),
        kind=kLoop, calls=identity2

      c1 = s32[] constant(1)
      next_ivar = s32[] add(ivar_copy, c1)
      use = s32[] add(call, not_functionally_dependent)

      ROOT result = (s32[], s32[]) tuple(next_ivar, use)
    }

    compare {
      p0 = s32[] parameter(0)
      c5 = s32[] constant(5)
      ROOT cmp = pred[] compare(p0, c5), direction=LT
    }

    condition {
      p0 = (s32[], s32[]) parameter(0)
      ivar = s32[] get-tuple-element(p0), index=0
      ROOT cmp = pred[] fusion(ivar), kind=kLoop, calls=compare
    }

    ENTRY main {
      c0 = s32[] constant(0)
      tuple = (s32[], s32[]) tuple(c0, c0)
      ROOT while = (s32[], s32[]) while(tuple),
          condition=condition, body=while_body,
          backend_config={"known_induction_variable":{"tuple_index":"0"}}
    }
)";

TEST_F(IrEmissionUtilsTest, ResolveWhileLoopDependency) {
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(kWhileLoopTestModule));

  HloComputation* while_body = module->GetComputationWithName("while_body");
  HloComputation* plus_one = module->GetComputationWithName("plus_one");
  HloComputation* call_body = module->GetComputationWithName("call_body");

  const HloInstruction* loop = module->entry_computation()->root_instruction();
  auto result = ResolveFunctionalDependencyOnInductionVariable(
      plus_one->GetInstructionWithName("sum"));

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->loop, loop);
  EXPECT_EQ(result->induction_var, while_body->GetInstructionWithName("ivar"));

  EXPECT_THAT(result->required_parameters, SizeIs(2));
  EXPECT_THAT(result->required_parameters[plus_one], ElementsAre(true, false));
  EXPECT_THAT(result->required_parameters[call_body],
              ElementsAre(false, true, false));
}

TEST_F(IrEmissionUtilsTest,
       ResolveWhileLoopDependencyUnknownInductionVariable) {
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(kWhileLoopTestModule));

  HloInstruction* loop = module->entry_computation()->root_instruction();
  loop->clear_backend_config();
  auto result = ResolveFunctionalDependencyOnInductionVariable(
      module->GetComputationWithName("plus_one")->root_instruction());

  ASSERT_FALSE(result.has_value());
}

TEST_F(IrEmissionUtilsTest, ResolveWhileLoopDependencySideEffect) {
  // Verifies that we detect `not_functionally_dependent` depends on an
  // instruction that has a side effect.
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(kWhileLoopTestModule));

  auto* while_body = module->GetComputationWithName("while_body");
  const HloInstruction* called_fusion =
      while_body->GetInstructionWithName("not_functionally_dependent");
  auto result = ResolveFunctionalDependencyOnInductionVariable(
      called_fusion->called_computations()[0]->root_instruction());

  ASSERT_FALSE(result.has_value());
}

TEST_F(IrEmissionUtilsTest, InternalTuple) {
  // Verifies that we can resolve dependencies that involve internal tuples.
  constexpr absl::string_view kHlo = R"(
      add12 {
        p0 = s32[] parameter(0)
        c1 = s32[] constant(1)
        c2 = s32[] constant(2)
        p0p1 = s32[] add(p0, c1)
        p0p2 = s32[] add(p0, c2)
        ROOT tuple = (s32[], s32[]) tuple(p0p1, p0p2)
      }

      call_body {
        p0 = s32[] parameter(0)
        ROOT sum = s32[] add(p0, p0)
      }

      while_body {
        p0 = (s32[], s32[]) parameter(0)
        ivar = s32[] get-tuple-element(p0), index=0
        ivar_copy = s32[] copy(ivar)

        side_effect = s32[] custom-call(), custom_call_target=""

        derived = (s32[], s32[]) fusion(ivar_copy), kind=kLoop, calls=add12
        val = get-tuple-element(derived), index=1

        c1 = s32[] constant(1)
        next_ivar = s32[] add(ivar_copy, c1)
        use = s32[] call(val), to_apply=call_body

        ROOT result = (s32[], s32[]) tuple(next_ivar, use)
      }

      condition {
        p0 = (s32[], s32[]) parameter(0)
        ivar = s32[] get-tuple-element(p0), index=0
        c5 = s32[] constant(5)
        ROOT cmp = pred[] compare(ivar, c5), direction=LT
      }

      ENTRY main {
        c0 = s32[] constant(0)
        tuple = (s32[], s32[]) tuple(c0, c0)
        ROOT while = (s32[], s32[]) while(tuple),
            condition=condition, body=while_body,
            backend_config={"known_induction_variable":{"tuple_index":"0"}}
      }
  )";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(kHlo));
  auto result = ResolveFunctionalDependencyOnInductionVariable(
      module->GetComputationWithName("call_body")->root_instruction());

  ASSERT_TRUE(result.has_value());

  HloComputation* while_body = module->GetComputationWithName("while_body");
  const HloComputation* call_body = module->GetComputationWithName("call_body");
  const HloInstruction* loop = module->entry_computation()->root_instruction();

  EXPECT_EQ(result->loop, loop);
  EXPECT_EQ(result->induction_var, while_body->GetInstructionWithName("ivar"));

  EXPECT_THAT(result->required_parameters, SizeIs(1));
  EXPECT_THAT(result->required_parameters[call_body], ElementsAre(true));
}

TEST_F(IrEmissionUtilsTest, NonInductionVariableLoopCarriedVariable) {
  // Verifies that we detect when there is a dependency on a non-induction
  // variable loop-carried variable.
  constexpr absl::string_view kHlo = R"(
      while_body {
        p0 = (s32[], s32[]) parameter(0)
        ivar = s32[] get-tuple-element(p0), index=0
        lcv = s32[] get-tuple-element(p0), index=1

        c1 = s32[] constant(1)
        next_ivar = s32[] add(ivar, c1)
        next_lcv = s32[] add(ivar, lcv)

        ROOT result = (s32[], s32[]) tuple(next_ivar, next_lcv)
      }

      condition {
        p0 = (s32[], s32[]) parameter(0)
        ivar = s32[] get-tuple-element(p0), index=0
        c5 = s32[] constant(5)
        ROOT cmp = pred[] compare(ivar, c5), direction=LT
      }

      ENTRY main {
        c0 = s32[] constant(0)
        tuple = (s32[], s32[]) tuple(c0, c0)
        ROOT while = (s32[], s32[]) while(tuple),
            condition=condition, body=while_body,
            backend_config={"known_induction_variable":{"tuple_index":"0"}}
      }
  )";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(kHlo));
  HloComputation* while_body = module->GetComputationWithName("while_body");

  // Sanity check to ensure there isn't something wrong with the loop.
  ASSERT_TRUE(ResolveFunctionalDependencyOnInductionVariable(
                  while_body->GetInstructionWithName("next_ivar"))
                  .has_value());

  // This must be false, since it depends on tuple index 1, which is not the
  // induction variable.
  ASSERT_FALSE(ResolveFunctionalDependencyOnInductionVariable(
                   while_body->GetInstructionWithName("next_lcv"))
                   .has_value());
}

TEST_F(IrEmissionUtilsTest, Transpose_10) {
  auto spec = GetTransposeSpecFromRoot(R"(ENTRY entry {
    p0 = f32[8, 32] parameter(0)
    ROOT transpose_p0 = f32[32, 8] transpose(p0), dimensions={1, 0}
  })");
  EXPECT_THAT(spec.permutation, ElementsAre(1, 0));
  EXPECT_THAT(spec.inv_permutation, ElementsAre(1, 0));
  EXPECT_THAT(spec.canonical_input_shape, ElementsAre(8, 1, 32, 1));
  EXPECT_THAT(spec.canonical_output_shape, ElementsAre(32, 1, 8, 1));
  EXPECT_THAT(spec.canonical_permutation, ElementsAre(2, 1, 0, 3));
  EXPECT_THAT(spec.canonical_inv_permutation, ElementsAre(2, 1, 0, 3));
}

TEST_F(IrEmissionUtilsTest, Transpose_210) {
  auto spec = GetTransposeSpecFromRoot(R"(ENTRY entry {
    p0 = f32[8, 2, 32] parameter(0)
    ROOT transpose_p0 = f32[32, 2, 8] transpose(p0), dimensions={2, 1, 0}
  })");
  EXPECT_THAT(spec.canonical_input_shape, ElementsAre(8, 2, 32, 1));
  EXPECT_THAT(spec.canonical_output_shape, ElementsAre(32, 2, 8, 1));
  EXPECT_THAT(spec.canonical_permutation, ElementsAre(2, 1, 0, 3));
  EXPECT_THAT(spec.canonical_inv_permutation, ElementsAre(2, 1, 0, 3));
}

TEST_F(IrEmissionUtilsTest, Transpose_102) {
  auto spec = GetTransposeSpecFromRoot(R"(ENTRY entry {
    p0 = f32[8, 2, 32, 7, 6] parameter(0)
    ROOT transpose_p0 = f32[6, 32, 2, 7, 8] transpose(p0),
      dimensions={4, 2, 1, 3, 0}
  })");
  EXPECT_THAT(spec.canonical_input_shape, ElementsAre(8, 2, 32, 7, 6, 1));
  EXPECT_THAT(spec.canonical_output_shape, ElementsAre(6, 32, 2, 7, 8, 1));
  EXPECT_THAT(spec.canonical_permutation, ElementsAre(4, 2, 1, 3, 0, 5));
  EXPECT_THAT(spec.canonical_inv_permutation, ElementsAre(4, 2, 1, 3, 0, 5));
}

TEST(DenseDataIntermediateTest, OwnedDataToProto) {
  const std::vector<uint8_t> data = {1, 2, 3, 4};
  DenseDataIntermediate constant = DenseDataIntermediate::Own(data);

  DenseDataIntermediateProto proto = constant.ToProto();
  EXPECT_THAT(proto.data(), ElementsAreArray(data));
}

TEST(DenseDataIntermediateTest, BorrowedDataToProto) {
  constexpr std::array<uint8_t, 4> kData = {5, 6, 7, 8};
  DenseDataIntermediate constant = DenseDataIntermediate::Alias(kData);
  DenseDataIntermediateProto proto = constant.ToProto();
  EXPECT_THAT(proto.data(), ElementsAreArray(kData));
}

TEST(DenseDataIntermediateTest, FromProto) {
  constexpr std::array<uint8_t, 4> kData = {1, 2, 3, 4};
  DenseDataIntermediateProto proto;
  proto.mutable_data()->assign(kData.begin(), kData.end());

  DenseDataIntermediate constant = DenseDataIntermediate::FromProto(proto);
  EXPECT_THAT(constant.span(), ElementsAreArray(kData));
}

TEST_F(IrEmissionUtilsTest, OrdinaryMatmul) {
  const char* hlo_string = R"(
  HloModule t

  ENTRY entry {
    p0 = f32[10,20,30,40] parameter(0)
    p1 = f32[10,20,50,40] parameter(1)
    ROOT t = f32[10,20,30,50] dot(p0, p1),
        lhs_batch_dims={0,1}, lhs_contracting_dims={3},
        rhs_batch_dims={0,1}, rhs_contracting_dims={3}
  })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(IsCublasSupportedMatMul(*root, true), IsOkAndHolds(true));
  EXPECT_THAT(IsCublasSupportedMatMul(*root, false), IsOkAndHolds(true));
}

TEST_F(IrEmissionUtilsTest, SingletonNoncontractingDim) {
  const char* hlo_string = R"(
  HloModule t

  ENTRY entry {
    p0 = f32[10,20,1,40] parameter(0)
    p1 = f32[10,20,50,40] parameter(1)
    ROOT t = f32[10,20,1,50] dot(p0, p1),
        lhs_batch_dims={0,1}, lhs_contracting_dims={3},
        rhs_batch_dims={0,1}, rhs_contracting_dims={3}
  })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(IsCublasSupportedMatMul(*root, true), IsOkAndHolds(true));
  EXPECT_THAT(IsCublasSupportedMatMul(*root, false), IsOkAndHolds(false));
}

TEST_F(IrEmissionUtilsTest, BothOperandsHaveSingletonNoncontractingDims) {
  const char* hlo_string = R"(
  HloModule t

  ENTRY entry {
    p0 = f32[10,20,1,40] parameter(0)
    p1 = f32[10,20,1,40] parameter(1)
    ROOT t = f32[10,20,1,1] dot(p0, p1),
        lhs_batch_dims={0,1}, lhs_contracting_dims={3},
        rhs_batch_dims={0,1}, rhs_contracting_dims={3}
  })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(IsCublasSupportedMatMul(*root, true), IsOkAndHolds(false));
  EXPECT_THAT(IsCublasSupportedMatMul(*root, false), IsOkAndHolds(false));
}

TEST_F(IrEmissionUtilsTest, OneSideDoesntHaveNoncontractingDims) {
  const char* hlo_string = R"(
  HloModule t

  ENTRY entry {
    p0 = f32[10,20,40] parameter(0)
    p1 = f32[10,20,2,40] parameter(1)
    ROOT t = f32[10,20,2] dot(p0, p1),
        lhs_batch_dims={0,1}, lhs_contracting_dims={2},
        rhs_batch_dims={0,1}, rhs_contracting_dims={3}
  })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(IsCublasSupportedMatMul(*root, true), IsOkAndHolds(true));
  EXPECT_THAT(IsCublasSupportedMatMul(*root, false), IsOkAndHolds(false));
}

TEST_F(IrEmissionUtilsTest, OneSideMissesNoncontractingDimsOtherIsSingleton) {
  const char* hlo_string = R"(
  HloModule t

  ENTRY entry {
    p0 = f32[10,20,40] parameter(0)
    p1 = f32[10,20,1,40] parameter(1)
    ROOT t = f32[10,20,1] dot(p0, p1),
        lhs_batch_dims={0,1}, lhs_contracting_dims={2},
        rhs_batch_dims={0,1}, rhs_contracting_dims={3}
  })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(IsCublasSupportedMatMul(*root, true), IsOkAndHolds(false));
  EXPECT_THAT(IsCublasSupportedMatMul(*root, false), IsOkAndHolds(false));
}

TEST_F(IrEmissionUtilsTest, NoNonContractingDims) {
  const char* hlo_string = R"(
  HloModule t

  ENTRY entry {
    p0 = f32[10,20,40] parameter(0)
    p1 = f32[10,20,40] parameter(1)
    ROOT t = f32[10,20] dot(p0, p1),
        lhs_batch_dims={0,1}, lhs_contracting_dims={2},
        rhs_batch_dims={0,1}, rhs_contracting_dims={2}
  })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(IsCublasSupportedMatMul(*root, true), IsOkAndHolds(false));
  EXPECT_THAT(IsCublasSupportedMatMul(*root, false), IsOkAndHolds(false));
}

}  // namespace gpu
}  // namespace xla
