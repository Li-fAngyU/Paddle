/* Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <string>
#include <vector>

#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/framework/tensor_util.h"
#include "paddle/fluid/memory/malloc.h"
#include "paddle/fluid/operators/fake_quantize_op.h"
#include "paddle/phi/common/data_type.h"
#include "paddle/phi/common/transform.h"
#include "paddle/phi/core/ddim.h"
#include "paddle/phi/core/hostdevice.h"
#include "paddle/phi/kernels/cast_kernel.h"

namespace paddle {
namespace operators {

template <typename DeviceContext, typename T>
struct DequantizeFunctor {
  void operator()(const DeviceContext& dev_ctx,
                  const phi::DenseTensor* in,
                  const phi::DenseTensor* scale,
                  T max_range,
                  phi::DenseTensor* out);
};

template <typename DeviceContext, typename T>
struct ChannelDequantizeFunctorV2 {
  void operator()(const DeviceContext& dev_ctx,
                  const phi::DenseTensor* in,
                  const phi::DenseTensor** scales,
                  const int scale_num,
                  T max_range,
                  const int quant_axis,
                  phi::DenseTensor* out);
};

template <typename DeviceContext, typename T>
class QuantizeLinearKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& context) const override {
    auto* in = context.Input<phi::DenseTensor>("X");
    auto* in_scale = context.Input<phi::DenseTensor>("Scale");

    auto* out = context.Output<phi::DenseTensor>("Y");
    out->mutable_data<T>(context.GetPlace());
    int bit_length = context.Attr<int>("bit_length");
    int round_type = context.Attr<int>("round_type");
    int bin_cnt = std::pow(2, bit_length - 1) - 1;
    int quant_axis = context.Attr<int>("quant_axis");
    bool is_test = context.Attr<bool>("is_test");
    bool only_observer = context.Attr<bool>("only_observer");
    auto& dev_ctx = context.template device_context<DeviceContext>();

    if (quant_axis < 0) {
      if (!is_test) {
        // training
        auto* in_accum = context.Input<phi::DenseTensor>("InAccum");
        auto* in_state = context.Input<phi::DenseTensor>("InState");
        phi::DenseTensor tmp_scale;
        tmp_scale.Resize(phi::make_dim(1));
        T* cur_scale_data = dev_ctx.template Alloc<T>(&tmp_scale);

        FindAbsMaxFunctor<DeviceContext, T>()(
            dev_ctx, in->data<T>(), in->numel(), cur_scale_data);

        auto* out_state = context.Output<phi::DenseTensor>("OutState");
        auto* out_accum = context.Output<phi::DenseTensor>("OutAccum");
        auto* out_scale = context.Output<phi::DenseTensor>("OutScale");
        out_state->mutable_data<T>(context.GetPlace());
        out_accum->mutable_data<T>(context.GetPlace());
        out_scale->mutable_data<T>(context.GetPlace());
        float moving_rate = context.Attr<float>("moving_rate");

        FindMovingAverageAbsMaxFunctor<DeviceContext, T>()(dev_ctx,
                                                           *in_accum,
                                                           *in_state,
                                                           cur_scale_data,
                                                           moving_rate,
                                                           out_state,
                                                           out_accum,
                                                           out_scale);
        if (only_observer) {
          framework::TensorCopy(*in, context.GetPlace(), dev_ctx, out);
        } else {
          ClipAndFakeQuantFunctor<DeviceContext, T>()(
              dev_ctx, *in, *out_scale, bin_cnt, round_type, out);
        }
      } else {
        if (only_observer) {
          framework::TensorCopy(*in, context.GetPlace(), dev_ctx, out);
        } else {
          ClipAndFakeQuantFunctor<DeviceContext, T>()(
              dev_ctx, *in, *in_scale, bin_cnt, round_type, out);
        }
      }
    } else {
      if (!is_test) {
        auto* out_scale = context.Output<phi::DenseTensor>("OutScale");
        T* out_scale_data = out_scale->mutable_data<T>(context.GetPlace());
        FindChannelAbsMaxFunctor<DeviceContext, T>()(
            dev_ctx, *in, quant_axis, out_scale_data);
        if (only_observer) {
          framework::TensorCopy(*in, context.GetPlace(), dev_ctx, out);
        } else {
          ChannelClipAndFakeQuantFunctor<DeviceContext, T>()(
              dev_ctx, *in, *out_scale, bin_cnt, round_type, quant_axis, out);
        }
      } else {
        if (only_observer) {
          framework::TensorCopy(*in, context.GetPlace(), dev_ctx, out);
        } else {
          ChannelClipAndFakeQuantFunctor<DeviceContext, T>()(
              dev_ctx, *in, *in_scale, bin_cnt, round_type, quant_axis, out);
        }
      }
    }
  }
};

template <typename DeviceContext, typename T>
class DeQuantizeLinearKernel : public framework::OpKernel<T> {
 public:
  template <typename D>
  void ComputeImpl(const framework::ExecutionContext& context) const {
    auto& dev_ctx = context.template device_context<DeviceContext>();
    auto* in = context.Input<phi::DenseTensor>("X");

    auto in_tmp = phi::Cast<T>(
        static_cast<const typename paddle::framework::ConvertToPhiContext<
            DeviceContext>::TYPE&>(dev_ctx),
        *in,
        phi::CppTypeToDataType<D>::Type());

    auto* scale = context.Input<phi::DenseTensor>("Scale");
    auto* out = context.Output<phi::DenseTensor>("Y");
    int bit_length = context.Attr<int>("bit_length");
    auto quant_axis = context.Attr<int>("quant_axis");
    dev_ctx.template Alloc<D>(out, out->numel() * sizeof(D));
    bool only_observer = context.Attr<bool>("only_observer");

    if (only_observer) {
      framework::TensorCopy(*in, context.GetPlace(), dev_ctx, out);
      return;
    }

    if (quant_axis < 0) {
      float max_range = (std::pow(2, bit_length - 1) - 1);
      DequantizeFunctor<DeviceContext, D>()(
          dev_ctx, &in_tmp, scale, static_cast<D>(max_range), out);
    } else {
      PADDLE_ENFORCE_EQ(
          scale->numel(),
          in_tmp.dims()[quant_axis],
          platform::errors::PreconditionNotMet(
              "The number of first scale values must be the same with "
              "quant_axis dimension value of Input(X) when the `scale` has "
              "only one element, but %ld != %ld here.",
              scale->numel(),
              in_tmp.dims()[quant_axis]));
      int max_range = (std::pow(2, bit_length - 1) - 1);

      ChannelDequantizeFunctorV2<DeviceContext, D>()(
          dev_ctx, &in_tmp, scale, static_cast<D>(max_range), quant_axis, out);
    }
  }

  void Compute(const framework::ExecutionContext& context) const override {
    auto* scale = context.Input<phi::DenseTensor>("Scale");
    switch (scale->dtype()) {
      case phi::DataType::FLOAT64:
        ComputeImpl<double>(context);
        break;
      case phi::DataType::FLOAT32:
        ComputeImpl<float>(context);
        break;
      case phi::DataType::FLOAT16:
        ComputeImpl<paddle::platform::float16>(context);
        break;
      default:
        PADDLE_THROW(platform::errors::Unimplemented(
            "In DeQuantizeLinearKernel, "
            "data type %d for scale/output is not supported ",
            scale->dtype()));
        break;
    }
  }
};

}  // namespace operators
}  // namespace paddle
