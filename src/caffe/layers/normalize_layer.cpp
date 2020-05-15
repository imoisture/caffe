/*
All modification made by Cambricon Corporation: © 2018-2019 Cambricon Corporation
All rights reserved.
All other contributions:
Copyright (c) 2014--2019, the respective contributors
All rights reserved.
For the list of contributors go to https://github.com/BVLC/caffe/blob/master/CONTRIBUTORS.md
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <memory>
#include <vector>

#include "caffe/filler.hpp"
#include "caffe/layers/normalize_layer.hpp"

namespace caffe {

template <typename Dtype>
void NormalizeLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  CHECK_GE(bottom[0]->num_axes(), 2)
      << "Number of axes of bottom blob must be >=2.";
  buffer_.Reshape(1, bottom[0]->channels(),
                   bottom[0]->height(), bottom[0]->width());
  buffer_channel_.Reshape(1, bottom[0]->channels(), 1, 1);
  buffer_spatial_.Reshape(1, 1, bottom[0]->height(), bottom[0]->width());
  NormalizeParameter norm_param = this->layer_param().norm_param();
  across_spatial_ = norm_param.across_spatial();
  if (across_spatial_) {
    norm_.Reshape(bottom[0]->num(), 1, 1, 1);
  } else {
    norm_.Reshape(bottom[0]->num(), 1, bottom[0]->height(), bottom[0]->width());
  }
  eps_ = norm_param.eps();
  int channels = bottom[0]->channels();
  int spatial_dim = bottom[0]->width() * bottom[0]->height();
  sum_channel_multiplier_.Reshape(1, channels, 1, 1);
  caffe_set(channels, Dtype(1), sum_channel_multiplier_.mutable_cpu_data());
  sum_spatial_multiplier_.Reshape(
      1, 1, bottom[0]->height(), bottom[0]->width());
  caffe_set(spatial_dim, Dtype(1), sum_spatial_multiplier_.mutable_cpu_data());
  channel_shared_ = norm_param.channel_shared();
  if (this->blobs_.size() > 0) {
    LOG(INFO) << "Skipping parameter initialization";
  } else {
    this->blobs_.resize(1);
    if (channel_shared_) {
      this->blobs_[0].reset(new Blob<Dtype>(vector<int>(0)));
    } else {
      this->blobs_[0].reset(new Blob<Dtype>(vector<int>(1, channels)));
    }
    shared_ptr<Filler<Dtype> > scale_filler;
    if (norm_param.has_scale_filler()) {
      scale_filler.reset(GetFiller<Dtype>(norm_param.scale_filler()));
    } else {
      FillerParameter filler_param;
      filler_param.set_type("constant");
      filler_param.set_value(1.0);
      scale_filler.reset(GetFiller<Dtype>(filler_param));
    }
    scale_filler->Fill(this->blobs_[0].get());
  }
  if (channel_shared_) {
    CHECK_EQ(this->blobs_[0]->count(), 1)
        << "Scale size is inconsistent with prototxt config";
  } else {
    CHECK_EQ(this->blobs_[0]->count(), channels)
        << "Scale size is inconsistent with prototxt config";
  }
  this->param_propagate_down_.resize(this->blobs_.size(), true);
}

template <typename Dtype>
void NormalizeLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  CHECK_GE(bottom[0]->num_axes(), 2)
      << "Number of axes of bottom blob must be >=2.";
  top[0]->ReshapeLike(*bottom[0]);
  buffer_.Reshape(1, bottom[0]->channels(),
                   bottom[0]->height(), bottom[0]->width());
  buffer_channel_.Reshape(1, bottom[0]->channels(), 1, 1);
  buffer_spatial_.Reshape(1, 1, bottom[0]->height(), bottom[0]->width());
  if (!across_spatial_) {
    norm_.Reshape(bottom[0]->num(), 1, bottom[0]->height(), bottom[0]->width());
  } else {
    norm_.Reshape(bottom[0]->num(), 1, 1, 1);
  }
  int channels = bottom[0]->channels();
  int spatial_dim = bottom[0]->height() * bottom[0]->width();
  sum_channel_multiplier_.Reshape(1, channels, 1, 1);
  caffe_set(channels, Dtype(1), sum_channel_multiplier_.mutable_cpu_data());
  sum_spatial_multiplier_.Reshape(
      1, 1, bottom[0]->height(), bottom[0]->width());
  caffe_set(spatial_dim, Dtype(1),
            sum_spatial_multiplier_.mutable_cpu_data());
}

template <typename Dtype>
void NormalizeLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  const Dtype* bottom_data = bottom[0]->cpu_data();
  Dtype* top_data = top[0]->mutable_cpu_data();
  const Dtype* scale = this->blobs_[0]->cpu_data();
  Dtype* buffer_data = buffer_.mutable_cpu_data();
  Dtype* norm_data = norm_.mutable_cpu_data();
  // add eps to avoid overflow
  caffe_set<Dtype>(norm_.count(), Dtype(eps_), norm_data);
  const Dtype* sum_channel_multiplier = sum_channel_multiplier_.cpu_data();
  const Dtype* sum_spatial_multiplier = sum_spatial_multiplier_.cpu_data();
  int num = bottom[0]->num();
  int dim = bottom[0]->count() / num;
  int spatial_dim = bottom[0]->height() * bottom[0]->width();
  int channels = bottom[0]->channels();
  for (int n = 0; n < num; ++n) {
    caffe_sqr<Dtype>(dim, bottom_data, buffer_data);
    if (across_spatial_) {
      // add eps to avoid overflow
      norm_data[n] = pow(caffe_cpu_asum<Dtype>(dim, buffer_data)+eps_,
                         Dtype(0.5));
      caffe_cpu_scale<Dtype>(dim, Dtype(1.0 / norm_data[n]), bottom_data,
                             top_data);
    } else {
      caffe_cpu_gemv<Dtype>(CblasTrans, channels, spatial_dim, Dtype(1),
                            buffer_data, sum_channel_multiplier, Dtype(1),
                            norm_data);
      // compute norm
      caffe_powx<Dtype>(spatial_dim, norm_data, Dtype(0.5), norm_data);
      // LOG(INFO) << "after sqrt " << norm_data[0];
      // scale the layer
      caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, channels, spatial_dim,
                            1, Dtype(1), sum_channel_multiplier, norm_data,
                            Dtype(0), buffer_data);
      // LOG(INFO) << "after gemm1 " << buffer_data[0];
      caffe_div<Dtype>(dim, bottom_data, buffer_data, top_data);
      // LOG(INFO) << "after div " << top_data[0];
      norm_data += spatial_dim;
    }
    // scale the output
    if (channel_shared_) {
      caffe_scal<Dtype>(dim, scale[0], top_data);
    } else {
      caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, channels, spatial_dim,
                            1, Dtype(1), scale, sum_spatial_multiplier,
                            Dtype(0),
                            buffer_data);
      // LOG(INFO) << "after gemm2 " << buffer_data[0];
      caffe_mul<Dtype>(dim, top_data, buffer_data, top_data);
      // LOG(INFO) << "after mul " << top_data[0];
    }
    bottom_data += dim;
    top_data += dim;
  }
}

template <typename Dtype>
void NormalizeLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  const Dtype* top_diff = top[0]->cpu_diff();
  const Dtype* top_data = top[0]->cpu_data();
  const Dtype* bottom_data = bottom[0]->cpu_data();
  Dtype* bottom_diff = bottom[0]->mutable_cpu_diff();
  const Dtype* scale = this->blobs_[0]->cpu_data();
  const Dtype* norm_data = norm_.cpu_data();
  Dtype* buffer_data = buffer_.mutable_cpu_data();
  Dtype* buffer_channel = buffer_channel_.mutable_cpu_data();
  Dtype* buffer_spatial = buffer_spatial_.mutable_cpu_data();
  const Dtype* sum_channel_multiplier = sum_channel_multiplier_.cpu_data();
  const Dtype* sum_spatial_multiplier = sum_spatial_multiplier_.cpu_data();
  int count = top[0]->count();
  int num = top[0]->num();
  int dim = count / num;
  int spatial_dim = top[0]->height() * top[0]->width();
  int channels = top[0]->channels();

  // Propagate to param
  if (this->param_propagate_down_[0]) {
    Dtype* scale_diff = this->blobs_[0]->mutable_cpu_diff();
    if (channel_shared_) {
      scale_diff[0] +=
          caffe_cpu_dot<Dtype>(count, top_data, top_diff) / scale[0];
    } else {
      for (int n = 0; n < num; ++n) {
        caffe_mul<Dtype>(dim, top_data+n*dim, top_diff+n*dim, buffer_data);
        caffe_cpu_gemv<Dtype>(CblasNoTrans, channels, spatial_dim, Dtype(1),
                              buffer_data, sum_spatial_multiplier, Dtype(0),
                              buffer_channel);
        // store a / scale[i] in buffer_data temporary
        caffe_div<Dtype>(channels, buffer_channel, scale, buffer_channel);
        caffe_add<Dtype>(channels, buffer_channel, scale_diff, scale_diff);
      }
    }
  }

  // Propagate to bottom
  if (propagate_down[0]) {
    for (int n = 0; n < num; ++n) {
      if (across_spatial_) {
        Dtype a = caffe_cpu_dot<Dtype>(dim, bottom_data, top_diff);
        caffe_cpu_scale<Dtype>(dim, a / norm_data[n] / norm_data[n],
                               bottom_data, bottom_diff);
        caffe_sub<Dtype>(dim, top_diff, bottom_diff, bottom_diff);
        caffe_scal<Dtype>(dim, Dtype(1.0 / norm_data[n]), bottom_diff);
      } else {
        // dot product between bottom_data and top_diff
        caffe_mul<Dtype>(dim, bottom_data, top_diff, buffer_data);
        caffe_cpu_gemv<Dtype>(CblasTrans, channels, spatial_dim, Dtype(1),
                              buffer_data, sum_channel_multiplier, Dtype(0),
                              buffer_spatial);
        // scale bottom_diff
        caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, channels, spatial_dim,
                              1, Dtype(1), sum_channel_multiplier,
                              buffer_spatial, Dtype(0), buffer_data);
        caffe_mul<Dtype>(dim, bottom_data, buffer_data, bottom_diff);
        // divide by square of norm
        caffe_powx<Dtype>(spatial_dim, norm_data, Dtype(2), buffer_spatial);
        caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, channels, spatial_dim,
                              1, Dtype(1), sum_channel_multiplier,
                              buffer_spatial, Dtype(0), buffer_data);
        caffe_div<Dtype>(dim, bottom_diff, buffer_data, bottom_diff);
        // subtract
        caffe_sub<Dtype>(dim, top_diff, bottom_diff, bottom_diff);
        // divide by norm
        caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, channels, spatial_dim,
                              1, Dtype(1), sum_channel_multiplier, norm_data,
                              Dtype(0), buffer_data);
        caffe_div<Dtype>(dim, bottom_diff, buffer_data, bottom_diff);
        norm_data += spatial_dim;
      }
      // scale the diff
      if (channel_shared_) {
        caffe_scal<Dtype>(dim, scale[0], bottom_diff);
      } else {
        caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, channels, spatial_dim,
                              1, Dtype(1), scale, sum_spatial_multiplier,
                              Dtype(0), buffer_data);
        caffe_mul<Dtype>(dim, bottom_diff, buffer_data, bottom_diff);
      }
      bottom_data += dim;
      top_diff += dim;
      bottom_diff += dim;
    }
  }
}


#ifndef USE_CUDA
STUB_GPU(NormalizeLayer);
#endif

INSTANTIATE_CLASS(NormalizeLayer);

}  // namespace caffe