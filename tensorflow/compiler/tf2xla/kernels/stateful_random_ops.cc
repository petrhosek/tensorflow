/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/kernels/stateful_random_ops.h"

#include <cmath>

#include "tensorflow/compiler/tf2xla/kernels/random_ops_util.h"
#include "tensorflow/compiler/tf2xla/lib/random.h"
#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/tf2xla/type_util.h"
#include "tensorflow/compiler/tf2xla/xla_helpers.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#include "tensorflow/compiler/xla/client/lib/constants.h"
#include "tensorflow/compiler/xla/client/lib/math.h"
#include "tensorflow/compiler/xla/client/lib/prng.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/rng_alg.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/lib/math/math_util.h"

namespace tensorflow {
namespace {

xla::BitGeneratorTy BitGen(Algorithm alg) {
  if (alg == RNG_ALG_PHILOX) {
    return [=](xla::XlaOp key, xla::XlaOp state, const xla::Shape& shape) {
      state =
          xla::ConcatInDim(key.builder(), {xla::Reshape(key, {1}), state}, 0);
      xla::XlaOp result =
          xla::RngBitGenerator(xla::RandomAlgorithm::RNG_PHILOX, state, shape);
      xla::XlaOp data = xla::GetTupleElement(result, 1);
      xla::XlaOp new_state =
          xla::Slice(xla::GetTupleElement(result, 0), {1}, {3}, {1});
      return xla::RngOutput{data, new_state};
    };
  } else {
    return [=](xla::XlaOp key, xla::XlaOp state, const xla::Shape& shape) {
      state = xla::ConcatScalars(key.builder(), {key, state});
      xla::XlaOp result = xla::RngBitGenerator(
          xla::RandomAlgorithm::RNG_THREE_FRY, state, shape);
      xla::XlaOp data = xla::GetTupleElement(result, 1);
      xla::XlaOp new_state = xla::Reshape(
          xla::Slice(xla::GetTupleElement(result, 0), {1}, {2}, {1}), {});
      return xla::RngOutput{data, new_state};
    };
  }
}

xla::RngOutput StatefulRngUniform(Algorithm alg, xla::XlaOp key,
                                  xla::XlaOp initial_state,
                                  const xla::Shape& shape, xla::XlaOp minval,
                                  xla::XlaOp maxval) {
  xla::PrimitiveType type = shape.element_type();
  switch (type) {
    case xla::F32:
    case xla::F64:
      return xla::UniformFloatingPointDistribution(
          key, initial_state, BitGen(alg), minval, maxval, shape);
    case xla::U32:
    case xla::S32:
    case xla::U64:
    case xla::S64:
      return UniformIntDistribution(key, initial_state, BitGen(alg), minval,
                                    maxval, shape);
    default:
      return {key.builder()->ReportError(xla::Unimplemented(
                  "Types other than F32, U32, S32, U64 and S64 "
                  "are not implemented by "
                  "StatefulRngUniform; got %s",
                  xla::primitive_util::LowercasePrimitiveTypeName(type))),
              initial_state};
  }
}

xla::RngOutput StatefulRngUniformFullInt(Algorithm alg, xla::XlaOp key,
                                         xla::XlaOp initial_state,
                                         const xla::Shape& shape) {
  xla::PrimitiveType type = shape.element_type();
  xla::RngOutput output = BitGen(alg)(key, initial_state, shape);
  switch (type) {
    case xla::U32:
    case xla::U64:
      return output;
    case xla::S32:
    case xla::S64:
      output.value = BitcastConvertType(output.value, type);
      return output;
    default:
      return {
          key.builder()->ReportError(xla::Unimplemented(
              "Types other than U32, S32, U64 and S64 are not implemented by "
              "StatefulRngUniformFullInt; got: %s",
              xla::primitive_util::LowercasePrimitiveTypeName(type))),
          initial_state};
  }
}

using SamplerReturnType = StatusOr<xla::RngOutput>;

int64_t GetMinStateSize(Algorithm alg) {
  if (alg == RNG_ALG_PHILOX) {
    return PHILOX_MIN_STATE_SIZE;
  }
  return THREEFRY_MIN_STATE_SIZE;
}

Status CheckStateShape(Algorithm alg, const TensorShape& shape) {
  if (shape.dims() != 1) {
    return errors::InvalidArgument(
        "RNG state must have one and only one dimension, not ", shape.dims());
  }
  auto state_size = shape.dim_size(0);
  auto min_state_size = GetMinStateSize(alg);
  if (state_size < min_state_size) {
    return errors::InvalidArgument("The size of the state must be at least ",
                                   min_state_size, "; got ", state_size);
  }
  return OkStatus();
}

std::pair<xla::XlaOp, xla::XlaOp> StateAndKeyFromVariable(Algorithm alg,
                                                          xla::XlaOp var) {
  if (alg == RNG_ALG_THREEFRY) {
    static constexpr int kStateSize = 1;
    auto state = BitcastConvertType(
        xla::Reshape(xla::Slice(var, {0}, {kStateSize}, {1}), {}), xla::U64);
    auto key = BitcastConvertType(
        xla::Reshape(xla::Slice(var, {kStateSize}, {kStateSize + 1}, {1}), {}),
        xla::U64);
    return std::make_pair(state, key);
  } else {
    static constexpr int kStateSize = 2;
    auto state =
        BitcastConvertType(xla::Slice(var, {0}, {kStateSize}, {1}), xla::U64);
    auto key = xla::Reshape(
        BitcastConvertType(xla::Slice(var, {kStateSize}, {kStateSize + 1}, {1}),
                           xla::U64),
        {});
    return std::make_pair(state, key);
  }
}

xla::XlaOp StateAndKeyToVariable(Algorithm alg, xla::XlaOp state,
                                 xla::XlaOp key) {
  auto builder = state.builder();
  if (alg == RNG_ALG_THREEFRY) {
    return ConcatScalars(builder, {state, key});
  } else {
    return ConcatInDim(builder, {state, xla::Reshape(key, {1})}, 0);
  }
}

// A helper function containing the common part of several kernels below.
// Precondition: 'algorithm' and 'shape' are compile-time constants.
Status CompileImpl(
    XlaOpKernelContext* ctx, int state_input_idx, int alg_input_idx,
    int shape_input_idx,
    std::function<SamplerReturnType(Algorithm, xla::XlaOp, xla::XlaOp,
                                    TensorShape)> const& sampler) {
  auto alg_shape = ctx->InputShape(alg_input_idx);
  if (alg_shape.dims() != 0) {
    return errors::InvalidArgument("algorithm must be of shape [], not ",
                                   alg_shape.DebugString());
  }
  xla::Literal alg_literal;
  TF_RETURN_IF_ERROR(ctx->ConstantInput(alg_input_idx, &alg_literal));
  Algorithm alg = Algorithm(alg_literal.Get<int64_t>({}));
  if (!(alg == RNG_ALG_THREEFRY || alg == RNG_ALG_PHILOX)) {
    return errors::InvalidArgument("Unsupported algorithm id: ", alg);
  }

  xla::XlaOp var;
  TensorShape var_shape;
  TF_RETURN_IF_ERROR(ctx->ReadVariableInput(
      state_input_idx, STATE_ELEMENT_DTYPE, &var_shape, &var));
  TF_RETURN_IF_ERROR(CheckStateShape(alg, var_shape));
  TensorShape shape;
  TF_RETURN_IF_ERROR(ctx->ConstantInputAsShape(shape_input_idx, &shape));
  xla::XlaOp state;
  xla::XlaOp key;
  std::tie(state, key) = StateAndKeyFromVariable(alg, var);
  auto status_or_value = sampler(alg, state, key, shape);
  if (!status_or_value.ok()) {
    return status_or_value.status();
  }
  xla::RngOutput value_state = status_or_value.ConsumeValueOrDie();
  state = value_state.state;
  ctx->SetOutput(0, value_state.value);
  var = StateAndKeyToVariable(alg, state, key);
  xla::PrimitiveType state_element_type;
  TF_RETURN_IF_ERROR(
      DataTypeToPrimitiveType(STATE_ELEMENT_DTYPE, &state_element_type));
  var = BitcastConvertType(var, state_element_type);
  TF_RETURN_IF_ERROR(
      ctx->AssignVariable(state_input_idx, STATE_ELEMENT_DTYPE, var));
  return OkStatus();
}

class StatefulUniformOp : public XlaOpKernel {
 public:
  explicit StatefulUniformOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    xla::XlaBuilder* builder = ctx->builder();
    auto sampler = [builder, this](Algorithm alg, xla::XlaOp state,
                                   xla::XlaOp key,
                                   TensorShape shape) -> SamplerReturnType {
      xla::Shape xla_shape;
      DataType rng_dtype = dtype_ == DT_DOUBLE ? DT_DOUBLE : DT_FLOAT;
      TF_RETURN_IF_ERROR(TensorShapeToXLAShape(rng_dtype, shape, &xla_shape));
      xla::PrimitiveType rng_primitive_type = xla_shape.element_type();
      xla::RngOutput uniform_state = StatefulRngUniform(
          alg, key, state, xla_shape,
          xla::ConstantR0WithType(builder, rng_primitive_type, 0.0),
          xla::ConstantR0WithType(builder, rng_primitive_type, 1.0));
      xla::XlaOp uniform = uniform_state.value;
      state = uniform_state.state;
      uniform = MaybeConvertF32ToBF16(uniform, dtype_);
      return {{uniform, state}};
    };
    OP_REQUIRES_OK(ctx,
                   CompileImpl(ctx, /*state_input_idx=*/0, /*alg_input_idx=*/1,
                               /*shape_input_idx=*/2, sampler));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(StatefulUniformOp);
};

// TODO(wangpeng): Support plain float16 to get rid of the `TypeConstraint`.
REGISTER_XLA_OP(Name("StatefulUniform")
                    .CompileTimeConstantInput("algorithm")
                    .CompileTimeConstantInput("shape")
                    .TypeConstraint("dtype",
                                    {DT_DOUBLE, DT_FLOAT, DT_BFLOAT16}),
                StatefulUniformOp);

class StatefulStandardNormalOp : public XlaOpKernel {
 public:
  explicit StatefulStandardNormalOp(OpKernelConstruction* ctx)
      : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    auto sampler =
        // Needs explicit lambda return type because it fails to be inferred.
        [this](Algorithm alg, xla::XlaOp state, xla::XlaOp key,
               TensorShape shape) -> SamplerReturnType {
      xla::Shape xla_shape;
      DataType rng_dtype = dtype_ == DT_DOUBLE ? DT_DOUBLE : DT_FLOAT;
      TF_RETURN_IF_ERROR(TensorShapeToXLAShape(rng_dtype, shape, &xla_shape));
      xla::RngOutput value_state = xla::NormalFloatingPointDistribution(
          key, state, BitGen(alg), xla_shape);
      xla::XlaOp normal = MaybeConvertF32ToBF16(value_state.value, dtype_);
      return {{normal, value_state.state}};
    };
    OP_REQUIRES_OK(ctx,
                   CompileImpl(ctx, /*state_input_idx=*/0, /*alg_input_idx=*/1,
                               /*shape_input_idx=*/2, sampler));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(StatefulStandardNormalOp);
};

// TODO(wangpeng): Support plain float16 to get rid of the `TypeConstraint`.
REGISTER_XLA_OP(Name("StatefulStandardNormalV2")
                    .CompileTimeConstantInput("algorithm")
                    .CompileTimeConstantInput("shape")
                    .TypeConstraint("dtype",
                                    {DT_DOUBLE, DT_FLOAT, DT_BFLOAT16}),
                StatefulStandardNormalOp);

class StatefulTruncatedNormalOp : public XlaOpKernel {
 public:
  explicit StatefulTruncatedNormalOp(OpKernelConstruction* ctx)
      : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    xla::XlaBuilder* builder = ctx->builder();
    auto sampler =
        // Needs explicit lambda return type because it fails to be inferred.
        [builder, this](Algorithm alg, xla::XlaOp state, xla::XlaOp key,
                        TensorShape shape) -> SamplerReturnType {
      xla::Shape xla_shape;
      DataType rng_dtype = dtype_ == DT_DOUBLE ? DT_DOUBLE : DT_FLOAT;
      TF_RETURN_IF_ERROR(TensorShapeToXLAShape(rng_dtype, shape, &xla_shape));

      xla::RngOutput uniform_result = StatefulRngUniform(
          alg, key, state, xla_shape,
          xla::MinPositiveNormalValue(builder, xla_shape.element_type()),
          xla::One(builder, xla_shape.element_type()));
      xla::XlaOp uniform = uniform_result.value;
      state = uniform_result.state;
      xla::XlaOp truncated_normal = TruncatedNormal(uniform);
      truncated_normal = MaybeConvertF32ToBF16(truncated_normal, dtype_);
      return {{truncated_normal, state}};
    };
    OP_REQUIRES_OK(ctx,
                   CompileImpl(ctx, /*state_input_idx=*/0, /*alg_input_idx=*/1,
                               /*shape_input_idx=*/2, sampler));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(StatefulTruncatedNormalOp);
};

// TODO(wangpeng): Support plain float16 to get rid of the `TypeConstraint`.
REGISTER_XLA_OP(Name("StatefulTruncatedNormal")
                    .CompileTimeConstantInput("algorithm")
                    .CompileTimeConstantInput("shape")
                    .TypeConstraint("dtype",
                                    {DT_DOUBLE, DT_FLOAT, DT_BFLOAT16}),
                StatefulTruncatedNormalOp);

class StatefulUniformIntOp : public XlaOpKernel {
 public:
  explicit StatefulUniformIntOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    xla::XlaOp minval = ctx->Input(3);
    xla::XlaOp maxval = ctx->Input(4);
    auto sample_with_threefry =
        [minval, maxval, this](Algorithm alg, xla::XlaOp state, xla::XlaOp key,
                               TensorShape shape) -> SamplerReturnType {
      xla::Shape xla_shape;
      TF_RETURN_IF_ERROR(TensorShapeToXLAShape(dtype_, shape, &xla_shape));
      return StatefulRngUniform(alg, key, state, xla_shape, minval, maxval);
    };
    OP_REQUIRES_OK(ctx,
                   CompileImpl(ctx, /*state_input_idx=*/0, /*alg_input_idx=*/1,
                               /*shape_input_idx=*/2, sample_with_threefry));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(StatefulUniformIntOp);
};

REGISTER_XLA_OP(Name("StatefulUniformInt")
                    .CompileTimeConstantInput("algorithm")
                    .CompileTimeConstantInput("shape")
                    .TypeConstraint("dtype",
                                    {DT_INT32, DT_UINT32, DT_INT64, DT_UINT64}),
                StatefulUniformIntOp);

class StatefulUniformFullIntOp : public XlaOpKernel {
 public:
  explicit StatefulUniformFullIntOp(OpKernelConstruction* ctx)
      : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    auto sample_with_threefry = [this](Algorithm alg, xla::XlaOp state,
                                       xla::XlaOp key,
                                       TensorShape shape) -> SamplerReturnType {
      xla::Shape xla_shape;
      TF_RETURN_IF_ERROR(TensorShapeToXLAShape(dtype_, shape, &xla_shape));
      return StatefulRngUniformFullInt(alg, key, state, xla_shape);
    };
    OP_REQUIRES_OK(ctx,
                   CompileImpl(ctx, /*state_input_idx=*/0, /*alg_input_idx=*/1,
                               /*shape_input_idx=*/2, sample_with_threefry));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(StatefulUniformFullIntOp);
};

REGISTER_XLA_OP(Name("StatefulUniformFullInt")
                    .CompileTimeConstantInput("algorithm")
                    .CompileTimeConstantInput("shape")
                    .TypeConstraint("dtype",
                                    {DT_INT32, DT_UINT32, DT_INT64, DT_UINT64}),
                StatefulUniformFullIntOp);

xla::XlaOp IncreaseCounter(Algorithm const& alg, xla::XlaOp counter,
                           xla::XlaOp delta) {
  // Multiplying 256 to be consistent with the CPU/GPU kernels
  delta = delta * ConstantR0WithType(delta.builder(), xla::U64, 256);
  if (alg == RNG_ALG_PHILOX) {
    return xla::PhiloxIncreaseCounter(counter, delta);
  } else {
    return counter + delta;
  }
}

xla::XlaOp PadRight(xla::XlaOp a, int n) {
  return xla::Pad(a, xla::ScalarLike(a, 0),
                  xla::MakeEdgePaddingConfig({{0, n}}));
}

template <typename AlgEnumType = int64_t, bool read_old_value = false>
class RngSkipOp : public XlaOpKernel {
 public:
  explicit RngSkipOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {}

  void Compile(XlaOpKernelContext* ctx) override {
    const int state_input_idx = 0;
    const int alg_input_idx = 1;
    const int delta_input_idx = 2;
    xla::XlaOp var;
    TensorShape var_shape;
    OP_REQUIRES_OK(ctx,
                   ctx->ReadVariableInput(state_input_idx, STATE_ELEMENT_DTYPE,
                                          &var_shape, &var));
    xla::Literal alg_literal;
    OP_REQUIRES_OK(ctx, ctx->ConstantInput(alg_input_idx, &alg_literal));
    Algorithm alg = Algorithm(alg_literal.Get<AlgEnumType>({}));
    OP_REQUIRES(ctx, alg == RNG_ALG_THREEFRY || alg == RNG_ALG_PHILOX,
                errors::InvalidArgument("Unsupported algorithm id: ", alg));
    OP_REQUIRES_OK(ctx, CheckStateShape(alg, var_shape));
    if (read_old_value) {
      auto counter_size = GetCounterSize(alg);
      xla::XlaOp output = var;
      if (RNG_MAX_COUNTER_SIZE > counter_size) {
        // Because the size of `var` depends on the algorithm while we want the
        // output to have a fixed size (to help shape inference), we fix the
        // output size to be the maximal state size among algorithms, and right-
        // pad it with zeros if var's size is smaller than that.
        output = PadRight(output, RNG_MAX_COUNTER_SIZE - counter_size);
      }
      ctx->SetOutput(0, output);
    }
    xla::XlaOp counter;
    xla::XlaOp key;
    std::tie(counter, key) = StateAndKeyFromVariable(alg, var);
    xla::XlaOp delta = ctx->Input(delta_input_idx);
    delta = BitcastConvertType(delta, xla::U64);
    auto new_counter = IncreaseCounter(alg, counter, delta);
    var = StateAndKeyToVariable(alg, new_counter, key);
    xla::PrimitiveType state_element_type;
    OP_REQUIRES_OK(
        ctx, DataTypeToPrimitiveType(STATE_ELEMENT_DTYPE, &state_element_type));
    var = BitcastConvertType(var, state_element_type);
    OP_REQUIRES_OK(
        ctx, ctx->AssignVariable(state_input_idx, STATE_ELEMENT_DTYPE, var));
  }

 private:
  TF_DISALLOW_COPY_AND_ASSIGN(RngSkipOp);
};

REGISTER_XLA_OP(Name("RngSkip").CompileTimeConstantInput("algorithm"),
                RngSkipOp<>);

using RngReadAndSkipOp = RngSkipOp<int32, true>;

REGISTER_XLA_OP(Name("RngReadAndSkip").CompileTimeConstantInput("alg"),
                RngReadAndSkipOp);

}  // namespace
}  // namespace tensorflow
