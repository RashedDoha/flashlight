/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "flashlight/fl/contrib/modules/Conformer.h"
#include "flashlight/fl/autograd/Functions.h"
#include "flashlight/fl/nn/Init.h"
#include "flashlight/fl/nn/Utils.h"

namespace fl {

Conformer::Conformer(
    int32_t modelDim,
    int32_t headDim,
    int32_t mlpDim,
    int32_t nHeads,
    int32_t posEmbContextSize,
    int32_t convKernelSize,
    float pDropout,
    float pLayerDropout /* = 0. */)
    : nHeads_(nHeads),
      posEmbContextSize_(posEmbContextSize),
      convKernelSize_(convKernelSize),
      pDropout_(pDropout),
      pLayerDropout_(pLayerDropout),
      w11_(std::make_shared<Linear>(conformerInitLinear(modelDim, mlpDim))),
      w12_(std::make_shared<Linear>(conformerInitLinear(mlpDim, modelDim))),
      w21_(std::make_shared<Linear>(conformerInitLinear(modelDim, mlpDim))),
      w22_(std::make_shared<Linear>(conformerInitLinear(mlpDim, modelDim))),
      wq_(std::make_shared<Linear>(
          conformerInitLinear(modelDim, headDim * nHeads))),
      wk_(std::make_shared<Linear>(
          conformerInitLinear(modelDim, headDim * nHeads))),
      wv_(std::make_shared<Linear>(
          conformerInitLinear(modelDim, headDim * nHeads))),
      wf_(std::make_shared<Linear>(
          conformerInitLinear(headDim * nHeads, modelDim))),
      norm1_(std::make_shared<LayerNorm>(std::vector<int>({0}))),
      norm2_(std::make_shared<LayerNorm>(std::vector<int>({0}))),
      normMhsa_(std::make_shared<LayerNorm>(std::vector<int>({0}))),
      normConv1_(std::make_shared<LayerNorm>(std::vector<int>({2}))),
      normConv2_(std::make_shared<LayerNorm>(std::vector<int>({2}))),
      norm3_(std::make_shared<LayerNorm>(std::vector<int>({0}))),
      conv1_(std::make_shared<Conv2D>(modelDim, modelDim * 2, 1, 1)),
      conv2_(std::make_shared<Conv2D>(modelDim, modelDim, 1, 1)),
      convDepthWiseStep1_(std::make_shared<Conv2D>(
          modelDim,
          modelDim,
          convKernelSize,
          1,
          1,
          1,
          fl::PaddingMode::SAME,
          0,
          1,
          1,
          true,
          modelDim)),
      convDepthWiseStep2_(std::make_shared<Conv2D>(modelDim, modelDim, 1, 1)) {
  if (posEmbContextSize_ > 0) {
    params_.push_back(uniform(2 * posEmbContextSize_ - 1, headDim, -0.1, 0.1));
  }
  // first feed-forward module
  add(w11_);
  add(w12_);
  add(norm1_);
  // second feed-forward module
  add(w21_);
  add(w22_);
  add(norm2_);
  // multihead attention module
  add(wq_);
  add(wk_);
  add(wv_);
  add(wf_);
  add(normMhsa_),
      // conv module
      add(conv1_);
  add(conv2_);
  add(convDepthWiseStep1_);
  add(convDepthWiseStep2_);
  add(normConv1_);
  add(normConv2_);
  // final layer norm of conformer block
  add(norm3_);
}

Variable Conformer::conformerInitLinear(int32_t inDim, int32_t outDim) {
  float std = std::sqrt(1.0 / float(inDim));
  return fl::uniform(outDim, inDim, -std, std);
}

Variable Conformer::mhsa(const Variable& input) {
  float pDropout = train_ ? pDropout_ : 0.0;
  int bsz = input.dims(2);

  auto normedInput = (*normMhsa_)(input);
  auto q = transpose((*wq_)(normedInput));
  auto k = transpose((*wk_)(normedInput));
  auto v = transpose((*wv_)(normedInput));

  Variable mask, posEmb;
  if (posEmbContextSize_ > 0) {
    posEmb = tile(params_[0].as(input.type()), af::dim4(1, 1, nHeads_ * bsz));
  }
  auto result = multiheadAttention(q, k, v, posEmb, mask, nHeads_, pDropout, 0);
  result = (*wf_)(transpose(result));
  result = input + dropout(result, pDropout);
  return result;
}

Variable Conformer::conv(const Variable& input) {
  float pDropout = train_ ? pDropout_ : 0.0;
  // input C x T x B x 1
  auto result = reorder(input, 1, 3, 0, 2);
  // T x 1 x C x B
  // apply first pointwise conv
  result =
      gatedlinearunit((*conv1_)(((*normConv1_)(result)).as(input.type())), 2);
  // apply depthwise separable convolutions
  result = (*convDepthWiseStep2_)((*convDepthWiseStep1_)(result));
  result = fl::swish(((*normConv2_)(result)).as(input.type()), 1.);
  // apply second pointwise conv
  result = dropout((*conv2_)(result), pDropout);
  result = reorder(result, 2, 0, 3, 1);
  // C x T x B x 1
  return result + input;
}

std::vector<Variable> Conformer::forward(const std::vector<Variable>& input) {
  float pDropout = train_ ? pDropout_ : 0.0;
  float f = 1.0;
  if (train_ && (af::randu(1).scalar<float>() < pLayerDropout_)) {
    f = 0.0;
  }
  auto x = input[0];
  // apply first feed-forward module
  auto ffn1 =
      x +
      dropout(
          (*w12_)(dropout(
              fl::swish((*w11_)(((*norm1_)(x)).as(x.type())), 1.), pDropout)),
          pDropout);
  x = x + f * 0.5 * ffn1;
  // apply multihead attention module
  x = x + f * mhsa(x);
  // apply conv module
  x = x + f * conv(x);
  // apply second feed-forward module
  auto ffn2 =
      x +
      dropout(
          (*w22_)(dropout(
              fl::swish((*w21_)(((*norm2_)(x)).as(x.type())), 1.), pDropout)),
          pDropout);
  x = x + f * 0.5 * ffn2;
  x = ((*norm3_)(x)).as(x.type());
  return {x};
}

std::string Conformer::prettyString() const {
  std::ostringstream ss;
  ss << "Conformer "
     << "(modelDim: " << params_[1].dims(1) << "), "
     << "(mlpDim: " << params_[1].dims(0) << "), "
     << "(nHeads: " << nHeads_ << "), "
     << "(pDropout: " << pDropout_ << "), "
     << "(pLayerDropout: " << pLayerDropout_ << "), "
     << "(posEmbContextSize: " << posEmbContextSize_ << "), "
     << "(convKernelSize: " << convKernelSize_ << ") ";
  return ss.str();
}

} // namespace fl
