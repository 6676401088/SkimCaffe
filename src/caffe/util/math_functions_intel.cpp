#include <cfloat>
#include <cstdlib>
#include <sys/time.h>

#include <boost/math/special_functions/next.hpp>
#include <boost/random.hpp>

#include <limits>
#include <omp.h>
#ifdef __INTEL_COMPILER
#include <immintrin.h>
#endif

#include "caffe/common.hpp"
#include "caffe/util/math_functions_intel.hpp"
#include "caffe/layers/conv_relu_pool_lrn_layer.hpp"
#include "caffe/util/sconv.hpp"

extern unsigned long long conv_cycles_of_this_batch[1024*16], transpose_cycle, pool_cycle;

synk::Barrier *barriers[256];
int flop_cnt = 0;

static const double DEFAULT_CPU_FREQ = 3.33e9;
double get_cpu_freq()
{
  static double freq = DBL_MAX;
  if (DBL_MAX == freq) {
    volatile double a = rand()%1024, b = rand()%1024;
    struct timeval tv1, tv2;
    gettimeofday(&tv1, NULL);
    unsigned long long t1 = __rdtsc();
    for (size_t i = 0; i < 1024L*1024; i++) {
      a += a*b + b/a;
    }
    unsigned long long dt = __rdtsc() - t1;
    gettimeofday(&tv2, NULL);
    freq = dt/((tv2.tv_sec - tv1.tv_sec) + (tv2.tv_usec - tv1.tv_usec)/1.e6);
  }

  return freq;
}

namespace caffe {

// JSP: AlexNet conv1
// Input: 3 x 227 x 227 => 201 KB per channel, 604 KB total
// Output: 96 x 55 x 55 => 12 KB per channel, 1.1 MB total
// Weight: 96 x 3 x 11 x 11 => 0.5 KB per channel pair, 1.5 KB per output channel, 45 KB per input channel, 136 KB total
//         No matter what we do, there's no reuse on weight across different channels (only reuse is within a channel pair)
// FLOPS: 2 x 96 x 3 x 55 x 55 x 11 x 11 = 211 MFLOPS

/**
 * Direct dense convolution optimized for first layer of AlexNet
 */
static void dconv1(
    const float *weight, const float *input, float *output,
    const float *bias,
    float *pool_top, int *mask)
{
  int tid = omp_get_thread_num();

  const int M = 96;
  const int WIDTH = 227;
  const int STRIDE = 4;
  const int K = 11;
  const int WOUT = (WIDTH - K)/STRIDE + 1; // 55

  const int WOUT_BLOCK = 55;
  const int KERNEL_SIZE_ALIGNED = 176;

  int c_begin = 0;
  int c_end = M/8;

  for (int out_channel = c_begin*8; out_channel < c_end*8; out_channel += 8) {

    unsigned long long t = __rdtsc();

    __m256 bias_v = _mm256_loadu_ps(bias + out_channel);

    for (int output_row = 0; output_row < WOUT; ++output_row) {
      __m256 sum_v[8];

      int output_col;
      for (output_col = 0; output_col < 48; output_col += 8) {

        sum_v[0] = bias_v;
        sum_v[1] = bias_v;
        sum_v[2] = bias_v;
        sum_v[3] = bias_v;
        sum_v[4] = bias_v;
        sum_v[5] = bias_v;
        sum_v[6] = bias_v;
        sum_v[7] = bias_v;

        for (int in_channel = 0; in_channel < 3; ++in_channel) {
          const float *input_temp = input + (output_row * WIDTH + output_col) * STRIDE + in_channel * WIDTH * WIDTH;
          const float *weight_temp = weight + (3 * out_channel/8 + in_channel) * K * K * 8;

          for (int kernel_row = 0; kernel_row < K; ++kernel_row) {
            for (int kernel_col = 0; kernel_col < K; ++kernel_col) {
              __m256 w_v = _mm256_load_ps(weight_temp + (kernel_row*K + kernel_col)*8);

              sum_v[0] = _mm256_fmadd_ps(
                  w_v,
                  _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col]),
                  sum_v[0]);

              sum_v[1] = _mm256_fmadd_ps(
                  w_v,
                  _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 4]),
                  sum_v[1]);

              sum_v[2] = _mm256_fmadd_ps(
                  w_v,
                  _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 8]),
                  sum_v[2]);

              sum_v[3] = _mm256_fmadd_ps(
                  w_v,
                  _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 12]),
                  sum_v[3]);

              sum_v[4] = _mm256_fmadd_ps(
                  w_v,
                  _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 16]),
                  sum_v[4]);

              sum_v[5] = _mm256_fmadd_ps(
                  w_v,
                  _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 20]),
                  sum_v[5]);

              sum_v[6] = _mm256_fmadd_ps(
                  w_v,
                  _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 24]),
                  sum_v[6]);

              sum_v[7] = _mm256_fmadd_ps(
                  w_v,
                  _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 28]),
                  sum_v[7]);
            }
          }
        }

        _mm256_store_ps(output + (output_row*WOUT + output_col + 0)*8, sum_v[0]);
        _mm256_store_ps(output + (output_row*WOUT + output_col + 1)*8, sum_v[1]);
        _mm256_store_ps(output + (output_row*WOUT + output_col + 2)*8, sum_v[2]);
        _mm256_store_ps(output + (output_row*WOUT + output_col + 3)*8, sum_v[3]);
        _mm256_store_ps(output + (output_row*WOUT + output_col + 4)*8, sum_v[4]);
        _mm256_store_ps(output + (output_row*WOUT + output_col + 5)*8, sum_v[5]);
        _mm256_store_ps(output + (output_row*WOUT + output_col + 6)*8, sum_v[6]);
        _mm256_store_ps(output + (output_row*WOUT + output_col + 7)*8, sum_v[7]);
      }

      sum_v[0] = bias_v;
      sum_v[1] = bias_v;
      sum_v[2] = bias_v;
      sum_v[3] = bias_v;
      sum_v[4] = bias_v;
      sum_v[5] = bias_v;
      sum_v[6] = bias_v;

      for (int in_channel = 0; in_channel < 3; ++in_channel) {
        const float *input_temp = input + (output_row * WIDTH + output_col) * STRIDE + in_channel * WIDTH * WIDTH;
        const float *weight_temp = weight + (3 * out_channel/8 + in_channel) * K * K * 8;

        for (int kernel_row = 0; kernel_row < K; ++kernel_row) {
          for (int kernel_col = 0; kernel_col < K; ++kernel_col) {
            __m256 w_v = _mm256_load_ps(weight_temp + (kernel_row*K + kernel_col)*8);

            sum_v[0] = _mm256_fmadd_ps(
                w_v,
                _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col]),
                sum_v[0]);

            sum_v[1] = _mm256_fmadd_ps(
                w_v,
                _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 4]),
                sum_v[1]);

            sum_v[2] = _mm256_fmadd_ps(
                w_v,
                _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 8]),
                sum_v[2]);

            sum_v[3] = _mm256_fmadd_ps(
                w_v,
                _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 12]),
                sum_v[3]);

            sum_v[4] = _mm256_fmadd_ps(
                w_v,
                _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 16]),
                sum_v[4]);

            sum_v[5] = _mm256_fmadd_ps(
                w_v,
                _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 20]),
                sum_v[5]);

            sum_v[6] = _mm256_fmadd_ps(
                w_v,
                _mm256_set1_ps(input_temp[kernel_row*WIDTH + kernel_col + 24]),
                sum_v[6]);
          }
        }
      }

      _mm256_store_ps(output + (output_row*WOUT + output_col + 0)*8, sum_v[0]);
      _mm256_store_ps(output + (output_row*WOUT + output_col + 1)*8, sum_v[1]);
      _mm256_store_ps(output + (output_row*WOUT + output_col + 2)*8, sum_v[2]);
      _mm256_store_ps(output + (output_row*WOUT + output_col + 3)*8, sum_v[3]);
      _mm256_store_ps(output + (output_row*WOUT + output_col + 4)*8, sum_v[4]);
      _mm256_store_ps(output + (output_row*WOUT + output_col + 5)*8, sum_v[5]);
      _mm256_store_ps(output + (output_row*WOUT + output_col + 6)*8, sum_v[6]);
    } // for each output row

    conv_cycles_of_this_batch[tid*16] += __rdtsc() - t;
    t = __rdtsc();

    const int STRIDE_POOL = 2;
    const int K_POOL = 3;
    const int POOLED_WIDTH = (WOUT - K_POOL + STRIDE_POOL - 1) / STRIDE_POOL + 1; // (55 - 3 + 1)/2 + 1 = 27

    // compute offset
    const float *conv_top_data_cur = output;
    float *pool_top_data_cur = pool_top + out_channel*POOLED_WIDTH*POOLED_WIDTH;
    int *mask_cur = mask + out_channel*POOLED_WIDTH*POOLED_WIDTH;

    __declspec(aligned(64)) int mask[8];
    __declspec(aligned(64)) float maximum[8];

    __declspec(aligned(64)) int identity[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    __m256i identity_v = _mm256_load_si256((__m256i *)identity);

    for (int ph = 0; ph < POOLED_WIDTH; ++ph) {
      int hstart = ph * STRIDE_POOL;
      int hend = hstart + K_POOL;

      for (int pw = 0; pw < POOLED_WIDTH; ++pw) {
        int wstart = pw * STRIDE_POOL;
        __m256 maximum_v = _mm256_setzero_ps(); // JSP: using 0 instead of -FLT_MAX does ReLU for us.
        __m256 mask_v = _mm256_setzero_ps();
        __m256 cmp_v, in_v;

        int index = hstart * WOUT + wstart;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = hstart * WOUT + wstart + 1;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = hstart * WOUT + wstart + 2;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 1) * WOUT + wstart;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 1) * WOUT + wstart + 1;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 1) * WOUT + wstart + 2;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 2) * WOUT + wstart;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 2) * WOUT + wstart + 1;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 2) * WOUT + wstart + 2;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        _mm256_store_ps(maximum, maximum_v);
        _mm256_store_ps((float *)mask, mask_v);

        const int pool_index = ph * POOLED_WIDTH + pw;
        for (int j = 0; j < 8; ++j) {
          pool_top_data_cur[pool_index + j*POOLED_WIDTH*POOLED_WIDTH] = maximum[j];
          mask_cur[pool_index + j*POOLED_WIDTH*POOLED_WIDTH] = mask[j];
        }
      }
    }

    if (0 == tid) pool_cycle += __rdtsc() - t;
  } // for each out channel
}

template<>
void caffe_cpu_dconv(
    // input features
    const float *input,
    int in_channels, int height, int width,
    int pad_h, int pad_w,
    int stride_h, int stride_w,
    int dilation_h, int dilation_w,
    // weights
    const float *weight,
    int kernel_h, int kernel_w,
    // bias (for the case when bias is fused with convolution)
    const float *bias, const float *bias_multiplier,
    // pooling (for the case when pooling is fused with convolution)
    float *pool_top, int *mask,
    // output features
    float *output,
    int out_channels)
{
  const int output_h = (height + 2 * pad_h -
      (dilation_h * (kernel_h - 1) + 1)) / stride_h + 1;
  const int output_w = (width + 2 * pad_w -
      (dilation_w * (kernel_w - 1) + 1)) / stride_w + 1;

  // JSP: optimized path for first layer of AlexNet. Can be auto-generated later?
  if (height == 227 && width == 227 && pad_h == 0 && pad_w == 0 && stride_h == 4 && stride_w == 4 && kernel_w == 11 && kernel_h == 11 && dilation_h == 1 && dilation_w == 1 && in_channels == 3) {
    dconv1(weight, input, output, bias, pool_top, mask);
  }
  else {
    for (int out_channel = 0; out_channel < out_channels; ++out_channel) {
      for (int output_row = 0; output_row < output_h; ++output_row) {
        for (int output_col = 0; output_col < output_w; ++output_col) {
          float sum = 0;
          for (int in_channel = 0; in_channel < in_channels; ++in_channel) {
            for (int kernel_row = 0; kernel_row < kernel_h; ++kernel_row) {
              int input_row = output_row * stride_h - pad_h + kernel_row * dilation_h;
              for (int kernel_col = 0; kernel_col < kernel_w; ++kernel_col) {
                int input_col = output_col * stride_w - pad_w + kernel_col * dilation_w;
                if (input_row >= 0 && input_row < height && input_col >= 0 && input_col < width) {
                  int input_idx = (in_channel * height + input_row) * width + input_col;
                  assert(input_idx >= 0);
//                  assert(input_idx < this->bottom_dim_);
                  int weight_idx = ((in_channels * out_channel + in_channel) * kernel_h + kernel_row) * kernel_w + kernel_col;
                  assert(weight_idx >= 0);
//                      assert(weight_offset * g + weight_idx < this->blobs_[0]->count());
                  sum += input[input_idx] * weight[weight_idx];
                }
              } // for each kernel col
            } // for each kernel row
          } // for each in channel
          output[(out_channel * output_h + output_row) * output_w + output_col] = sum;
        } // for each output col
      } // for each output row
    } // for each out channel

    assert(false);
    // TODO: put bias and pooling here
  }
}

static inline void transpose8_ps(__m256 &row0, __m256 &row1, __m256 &row2, __m256 &row3, __m256 &row4, __m256 &row5, __m256 &row6, __m256 &row7) {
  __m256 __t0, __t1, __t2, __t3, __t4, __t5, __t6, __t7;
  __m256 __tt0, __tt1, __tt2, __tt3, __tt4, __tt5, __tt6, __tt7;
  __t0 = _mm256_unpacklo_ps(row0, row1);
  __t1 = _mm256_unpackhi_ps(row0, row1);
  __t2 = _mm256_unpacklo_ps(row2, row3);
  __t3 = _mm256_unpackhi_ps(row2, row3);
  __t4 = _mm256_unpacklo_ps(row4, row5);
  __t5 = _mm256_unpackhi_ps(row4, row5);
  __t6 = _mm256_unpacklo_ps(row6, row7);
  __t7 = _mm256_unpackhi_ps(row6, row7);
  __tt0 = _mm256_shuffle_ps(__t0,__t2,_MM_SHUFFLE(1,0,1,0));
  __tt1 = _mm256_shuffle_ps(__t0,__t2,_MM_SHUFFLE(3,2,3,2));
  __tt2 = _mm256_shuffle_ps(__t1,__t3,_MM_SHUFFLE(1,0,1,0));
  __tt3 = _mm256_shuffle_ps(__t1,__t3,_MM_SHUFFLE(3,2,3,2));
  __tt4 = _mm256_shuffle_ps(__t4,__t6,_MM_SHUFFLE(1,0,1,0));
  __tt5 = _mm256_shuffle_ps(__t4,__t6,_MM_SHUFFLE(3,2,3,2));
  __tt6 = _mm256_shuffle_ps(__t5,__t7,_MM_SHUFFLE(1,0,1,0));
  __tt7 = _mm256_shuffle_ps(__t5,__t7,_MM_SHUFFLE(3,2,3,2));
  row0 = _mm256_permute2f128_ps(__tt0, __tt4, 0x20);
  row1 = _mm256_permute2f128_ps(__tt1, __tt5, 0x20);
  row2 = _mm256_permute2f128_ps(__tt2, __tt6, 0x20);
  row3 = _mm256_permute2f128_ps(__tt3, __tt7, 0x20);
  row4 = _mm256_permute2f128_ps(__tt0, __tt4, 0x31);
  row5 = _mm256_permute2f128_ps(__tt1, __tt5, 0x31);
  row6 = _mm256_permute2f128_ps(__tt2, __tt6, 0x31);
  row7 = _mm256_permute2f128_ps(__tt3, __tt7, 0x31);
}

// JSP: AlexNet each group of conv2
// Input: 48 x 27 x 27 => 6 KB per channel, 273 KB total
// Output: 128 x 27 x 27 => 6 KB per channel, 729 KB total
// Weight: 128 x 48 x 5 x 5 => 0.2 KB per channel pair, 9 KB per output channel, 25 KB per input channel, 1.2 MB total
//         No matter what we do, there's no reuse on weight across different channels (only reuse is within a channel pair)
// FLOPS: 2 x 128 x 48 x 27 x 27 x 5 x 5 = 224 MFLOPS

/**
 * Direct sparse convolution optimized for 2nd layer of AlexNet fused with bias and pooling layer
 */
static void __attribute__((noinline)) sconv2_fused(
    // input features
    const float *input,
    // weights
    const int *rowptr, const int *colidx, const float *values,
    // bias (for the case when bias is fused with convolution)
    const float *bias,
    // pooling (for the case when pooling is fused with convolution)
    float *pool_top, int *mask,
    // output features
    float *output, int out_channels)
{
  int nthreads = omp_get_num_threads();
  int tid = omp_get_thread_num();

  int WIDTH = 27;
  int WOUT = 27;
  int PAD = 2;

  int nthread_groups = nthreads;
#ifdef __AVX512F__
//  nthread_groups = NTILES;
#endif
  assert(nthreads%nthread_groups == 0);
  int nthreads_per_group = nthreads/nthread_groups;
  int gid = tid/nthreads_per_group;
  int tid_in_group = tid%nthreads_per_group;

  int c_per_thread = (out_channels/8 + nthreads_per_group - 1)/nthreads_per_group;
  int c_begin = std::min(c_per_thread*tid_in_group, out_channels/8);
  int c_end = std::min(c_begin + c_per_thread, out_channels/8);

  for (int oc = c_begin*8; oc < c_end*8; ++oc) {
    unsigned long long t = __rdtsc();

    int out_channel_offset = tid_in_group*8 + oc%8;

    int j;
    int jbegin = rowptr[oc];
    int jend = rowptr[oc + 1];
    int off;

#ifdef __AVX512F__
    __m512 sum[2][14];
    __m512 w_v;
    __m512 bias_v = _mm512_set1_ps(bias[oc]);

    // upper half
    int hbegin = 0, hend = 14;
#pragma unroll(14)
    for (int h = hbegin; h < hend; ++h) {
      sum[0][h - hbegin] = bias_v;
      sum[1][h - hbegin] = bias_v;
    }

    for (j = jbegin; j < jend; ++j) {
      w_v = _mm512_set1_ps(values[j]);
      off = colidx[j];

#pragma unroll(14)
      for (int h = hbegin; h < hend; ++h) {
        sum[0][h - hbegin] = _mm512_fmadd_ps(w_v, _mm512_loadu_ps(input + off + h*(WIDTH + PAD)), sum[0][h - hbegin]);
        sum[1][h - hbegin] = _mm512_fmadd_ps(w_v, _mm512_loadu_ps(input + off + h*(WIDTH + PAD) + 16), sum[1][h - hbegin]);
      }
    }

#pragma unroll(14)
    for (int h = hbegin; h < hend; ++h) {
      _mm512_store_ps(output + (out_channel_offset*WOUT + h)*32, sum[0][h - hbegin]);
      _mm512_store_ps(output + (out_channel_offset*WOUT + h)*32 + 16, sum[1][h - hbegin]);
    }

    // lower half
    hbegin = 14; hend = 27;

#pragma unroll(13)
    for (int h = hbegin; h < hend; ++h) {
      sum[0][h - hbegin] = bias_v;
      sum[1][h - hbegin] = bias_v;
    }

    for (j = jbegin; j < jend; ++j) {
      w_v = _mm512_set1_ps(values[j]);
      off = colidx[j];

#pragma unroll(13)
      for (int h = hbegin; h < hend; ++h) {
        sum[0][h - hbegin] = _mm512_fmadd_ps(w_v, _mm512_loadu_ps(input + off + h*(WIDTH + PAD)), sum[0][h - hbegin]);
        sum[1][h - hbegin] = _mm512_fmadd_ps(w_v, _mm512_loadu_ps(input + off + h*(WIDTH + PAD) + 16), sum[1][h - hbegin]);
      }
    }

#pragma unroll(13)
    for (int h = hbegin; h < hend; ++h) {
      _mm512_store_ps(output + (out_channel_offset*WOUT + h)*32, sum[0][h - hbegin]);
      _mm512_store_ps(output + (out_channel_offset*WOUT + h)*32 + 16, sum[1][h - hbegin]);
    }
#elif defined(__AVX2__)
    __m256 sum[(WOUT + 3)/4][2]; // [7][2]
    __m256 w_v;
    __m256 bias_v = _mm256_set1_ps(bias[oc]);

    // (0, 0) block

#undef MY_FMADD
#define MY_FMADD(HBEGIN, WBEGIN) \
    sum[0][0] = bias_v; \
    sum[0][1] = bias_v; \
    sum[1][0] = bias_v; \
    sum[1][1] = bias_v; \
    sum[2][0] = bias_v; \
    sum[2][1] = bias_v; \
    sum[3][0] = bias_v; \
    sum[3][1] = bias_v; \
    sum[4][0] = bias_v; \
    sum[4][1] = bias_v; \
    sum[5][0] = bias_v; \
    sum[5][1] = bias_v; \
    sum[6][0] = bias_v; \
    sum[6][1] = bias_v; \
\
    for (j = jbegin; j < jend; ++j) { \
      w_v = _mm256_set1_ps(values[j]); \
      off = colidx[j]; \
\
      sum[0][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 0)*(WIDTH + PAD) + WBEGIN), sum[0][0]); \
      sum[0][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 0)*(WIDTH + PAD) + WBEGIN + 8), sum[0][1]); \
      sum[1][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 1)*(WIDTH + PAD) + WBEGIN), sum[1][0]); \
      sum[1][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 1)*(WIDTH + PAD) + WBEGIN + 8), sum[1][1]); \
      sum[2][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 2)*(WIDTH + PAD) + WBEGIN), sum[2][0]); \
      sum[2][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 2)*(WIDTH + PAD) + WBEGIN + 8), sum[2][1]); \
      sum[3][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 3)*(WIDTH + PAD) + WBEGIN), sum[3][0]); \
      sum[3][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 3)*(WIDTH + PAD) + WBEGIN + 8), sum[3][1]); \
      sum[4][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 4)*(WIDTH + PAD) + WBEGIN), sum[4][0]); \
      sum[4][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 4)*(WIDTH + PAD) + WBEGIN + 8), sum[4][1]); \
      sum[5][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 5)*(WIDTH + PAD) + WBEGIN), sum[5][0]); \
      sum[5][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 5)*(WIDTH + PAD) + WBEGIN + 8), sum[5][1]); \
      sum[6][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 6)*(WIDTH + PAD) + WBEGIN), sum[6][0]); \
      sum[6][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 6)*(WIDTH + PAD) + WBEGIN + 8), sum[6][1]); \
    } \
\
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 0)*32 + WBEGIN, sum[0][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 0)*32 + WBEGIN + 8, sum[0][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 1)*32 + WBEGIN, sum[1][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 1)*32 + WBEGIN + 8, sum[1][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 2)*32 + WBEGIN, sum[2][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 2)*32 + WBEGIN + 8, sum[2][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 3)*32 + WBEGIN, sum[3][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 3)*32 + WBEGIN + 8, sum[3][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 4)*32 + WBEGIN, sum[4][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 4)*32 + WBEGIN + 8, sum[4][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 5)*32 + WBEGIN, sum[5][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 5)*32 + WBEGIN + 8, sum[5][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 6)*32 + WBEGIN, sum[6][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 6)*32 + WBEGIN + 8, sum[6][1]);

    MY_FMADD(0, 0);
    MY_FMADD(0, 16);

    MY_FMADD(7, 0);
    MY_FMADD(7, 16);

    MY_FMADD(14, 0);
    MY_FMADD(14, 16);
#undef MY_FMADD

#undef MY_FMADD_REMAINDER
#define MY_FMADD_REMAINDER(HBEGIN, WBEGIN) \
    sum[0][0] = bias_v; \
    sum[0][1] = bias_v; \
    sum[1][0] = bias_v; \
    sum[1][1] = bias_v; \
    sum[2][0] = bias_v; \
    sum[2][1] = bias_v; \
    sum[3][0] = bias_v; \
    sum[3][1] = bias_v; \
    sum[4][0] = bias_v; \
    sum[4][1] = bias_v; \
    sum[5][0] = bias_v; \
    sum[5][1] = bias_v; \
\
    for (j = jbegin; j < jend; ++j) { \
      w_v = _mm256_set1_ps(values[j]); \
      off = colidx[j]; \
\
      sum[0][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 0)*(WIDTH + PAD) + WBEGIN), sum[0][0]); \
      sum[0][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 0)*(WIDTH + PAD) + WBEGIN + 8), sum[0][1]); \
      sum[1][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 1)*(WIDTH + PAD) + WBEGIN), sum[1][0]); \
      sum[1][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 1)*(WIDTH + PAD) + WBEGIN + 8), sum[1][1]); \
      sum[2][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 2)*(WIDTH + PAD) + WBEGIN), sum[2][0]); \
      sum[2][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 2)*(WIDTH + PAD) + WBEGIN + 8), sum[2][1]); \
      sum[3][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 3)*(WIDTH + PAD) + WBEGIN), sum[3][0]); \
      sum[3][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 3)*(WIDTH + PAD) + WBEGIN + 8), sum[3][1]); \
      sum[4][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 4)*(WIDTH + PAD) + WBEGIN), sum[4][0]); \
      sum[4][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 4)*(WIDTH + PAD) + WBEGIN + 8), sum[4][1]); \
      sum[5][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 5)*(WIDTH + PAD) + WBEGIN), sum[5][0]); \
      sum[5][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 5)*(WIDTH + PAD) + WBEGIN + 8), sum[5][1]); \
    } \
\
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 0)*32 + WBEGIN, sum[0][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 0)*32 + WBEGIN + 8, sum[0][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 1)*32 + WBEGIN, sum[1][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 1)*32 + WBEGIN + 8, sum[1][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 2)*32 + WBEGIN, sum[2][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 2)*32 + WBEGIN + 8, sum[2][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 3)*32 + WBEGIN, sum[3][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 3)*32 + WBEGIN + 8, sum[3][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 4)*32 + WBEGIN, sum[4][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 4)*32 + WBEGIN + 8, sum[4][1]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 5)*32 + WBEGIN, sum[5][0]); \
    _mm256_store_ps(output + (out_channel_offset*WOUT + HBEGIN + 5)*32 + WBEGIN + 8, sum[5][1]);

    MY_FMADD_REMAINDER(21, 0);
    MY_FMADD_REMAINDER(21, 16);
#undef MY_FMADD_REMAINDER
#else
    // !__AVX2__ && !__AVX512__
    __m128 sum[2][7];
    __declspec(aligned(64)) int mask_temp[4] = { -1, -1, -1, 0 };
    __m128i mask_v = _mm_load_si128((__m128i *)mask_temp);

    for (int h = 0; h < 26; h += 2) {
#pragma unroll(7)
      for (int w = 0; w < 7; ++w) {
        sum[0][w] = _mm_set1_ps(bias[oc]);
        sum[1][w] = _mm_set1_ps(bias[oc]);
      }

      for (int j = rowptr[oc]; j < rowptr[oc + 1]; ++j) {
        __m128 w_v = _mm_set1_ps(values[j]);
        int off = colidx[j];

#pragma unroll(7)
        for (int w = 0; w < 7; ++w) {
          sum[0][w] = _mm_add_ps(_mm_mul_ps(w_v, _mm_loadu_ps(input + off + h*(WIDTH + PAD) + w*4)), sum[0][w]);
          sum[1][w] = _mm_add_ps(_mm_mul_ps(w_v, _mm_loadu_ps(input + off + (h + 1)*(WIDTH + PAD) + w*4)), sum[1][w]);
        }
      }

#pragma unroll(6)
      for (int w = 0; w < 6; ++w) {
        _mm_storeu_ps(output + (oc*WOUT + h)*WOUT + w*4, sum[0][w]);
        _mm_storeu_ps(output + (oc*WOUT + h + 1)*WOUT + w*4, sum[1][w]);
      }
      _mm_maskmoveu_si128(_mm_castps_si128(sum[0][6]), mask_v, (char *)(output + (oc*WOUT + h)*WOUT + 24));
      _mm_maskmoveu_si128(_mm_castps_si128(sum[1][6]), mask_v, (char *)(output + (oc*WOUT + h + 1)*WOUT + 24));
    }

    int h = 26;
#pragma unroll(7)
    for (int w = 0; w < 7; ++w) {
      sum[0][w] = _mm_set1_ps(bias[oc]);
    }

    for (int j = rowptr[oc]; j < rowptr[oc + 1]; ++j) {
      __m128 w_v = _mm_set1_ps(values[j]);
      int off = colidx[j];

#pragma unroll(7)
      for (int w = 0; w < 7; ++w) {
        sum[0][w] = _mm_add_ps(_mm_mul_ps(w_v, _mm_loadu_ps(input + off + h*(WIDTH + PAD) + w*4)), sum[0][w]);
      }
    }

#pragma unroll(6)
    for (int w = 0; w < 6; ++w) {
      _mm_storeu_ps(output + (oc*WOUT + h)*WOUT + w*4, sum[0][w]);
    }
    _mm_maskmoveu_si128(_mm_castps_si128(sum[0][6]), mask_v, (char *)(output + (oc*WOUT + h)*WOUT + 24));

//    for (int output_row = 0; output_row < WOUT; ++output_row) {
//      for (int output_col = 0; output_col < WOUT; ++output_col) {
//
//        const float *in = input + output_row * (WIDTH + PAD) + output_col;
//
//        float sum = bias[oc];
//        for (int j = rowptr[oc]; j < rowptr[oc + 1]; ++j) {
//          sum += values[j]*in[colidx[j]];
//        }
//
//        output[(oc*WOUT + output_row)*WOUT + output_col] = sum;
//      }
//    }
#endif

    conv_cycles_of_this_batch[tid*16] += __rdtsc() - t;

#if defined(__AVX2__) || defined(__AVX512__)
    if (oc%8 != 7) continue;

    t = __rdtsc();

    int t_offset = 8*tid_in_group;

    // transpose to vectorize pooling layer over multiple channels
    for (int h = 0; h < WOUT; ++h) {
      for (int w = 0; w < WOUT/8*8; w += 8) {
        __m256 v0 = _mm256_load_ps(output + (t_offset*WOUT + h)*32 + w);
        __m256 v1 = _mm256_load_ps(output + ((t_offset + 1)*WOUT + h)*32 + w);
        __m256 v2 = _mm256_load_ps(output + ((t_offset + 2)*WOUT + h)*32 + w);
        __m256 v3 = _mm256_load_ps(output + ((t_offset + 3)*WOUT + h)*32 + w);
        __m256 v4 = _mm256_load_ps(output + ((t_offset + 4)*WOUT + h)*32 + w);
        __m256 v5 = _mm256_load_ps(output + ((t_offset + 5)*WOUT + h)*32 + w);
        __m256 v6 = _mm256_load_ps(output + ((t_offset + 6)*WOUT + h)*32 + w);
        __m256 v7 = _mm256_load_ps(output + ((t_offset + 7)*WOUT + h)*32 + w);

        transpose8_ps(v0, v1, v2, v3, v4, v5, v6, v7);

        _mm256_store_ps(output + (((nthreads_per_group + tid_in_group)*32 + h)*WOUT + w)*8, v0);
        _mm256_store_ps(output + (((nthreads_per_group + tid_in_group)*32 + h)*WOUT + (w + 1))*8, v1);
        _mm256_store_ps(output + (((nthreads_per_group + tid_in_group)*32 + h)*WOUT + (w + 2))*8, v2);
        _mm256_store_ps(output + (((nthreads_per_group + tid_in_group)*32 + h)*WOUT + (w + 3))*8, v3);
        _mm256_store_ps(output + (((nthreads_per_group + tid_in_group)*32 + h)*WOUT + (w + 4))*8, v4);
        _mm256_store_ps(output + (((nthreads_per_group + tid_in_group)*32 + h)*WOUT + (w + 5))*8, v5);
        _mm256_store_ps(output + (((nthreads_per_group + tid_in_group)*32 + h)*WOUT + (w + 6))*8, v6);
        _mm256_store_ps(output + (((nthreads_per_group + tid_in_group)*32 + h)*WOUT + (w + 7))*8, v7);
      }
      for (int w = WOUT/8*8; w < WOUT; ++w) {
        for (int i = 0; i < 8; ++i) {
          output[(((nthreads_per_group + tid_in_group)*32 + h)*WOUT + w)*8 + i] = output[((t_offset + i)*WOUT + h)*32 + w];
        }
      }
    }

    if (0 == tid) transpose_cycle += __rdtsc() - t;
    t = __rdtsc();

    const int STRIDE_POOL = 2;
    const int K_POOL = 3;
    const int POOLED_WIDTH = (WOUT - K_POOL + STRIDE_POOL - 1) / STRIDE_POOL + 1; // (27 - 3 + 1)/2 + 1 = 13

    const float *conv_top_data_cur = output + (nthreads_per_group + tid_in_group)*32*WOUT*8;
    float *pool_top_data_cur = pool_top + (oc - 7)*POOLED_WIDTH*POOLED_WIDTH;
    int *mask_cur = mask + (oc - 7)*POOLED_WIDTH*POOLED_WIDTH;

    __declspec(aligned(64)) float maximum[8];

    __declspec(aligned(64)) int identity[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    __m256i identity_v = _mm256_load_si256((__m256i *)identity);

    for (int ph = 0; ph < POOLED_WIDTH; ++ph) {
      __declspec(aligned(64)) int mask[8];

      int hstart = ph * STRIDE_POOL;
      int hend = hstart + K_POOL;

      for (int pw = 0; pw < POOLED_WIDTH; ++pw) {
        int wstart = pw * STRIDE_POOL;
        __m256 maximum_v = _mm256_setzero_ps(); // JSP: using 0 instead of -FLT_MAX does ReLU for us.
        __m256 mask_v = _mm256_setzero_ps();
        __m256 cmp_v, in_v;

        int index = hstart * WOUT + wstart;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = hstart * WOUT + wstart + 1;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = hstart * WOUT + wstart + 2;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 1) * WOUT + wstart;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 1) * WOUT + wstart + 1;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 1) * WOUT + wstart + 2;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 2) * WOUT + wstart;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 2) * WOUT + wstart + 1;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        index = (hstart + 2) * WOUT + wstart + 2;
        in_v = _mm256_load_ps(conv_top_data_cur + index*8);
        cmp_v = _mm256_cmp_ps(in_v, maximum_v, _CMP_LE_OQ);
        maximum_v =  _mm256_blendv_ps(in_v, maximum_v, cmp_v);
        mask_v = _mm256_blendv_ps(
            _mm256_castsi256_ps(_mm256_add_epi32(_mm256_set1_epi32(index*8), identity_v)),
            mask_v,
            cmp_v);

        _mm256_store_ps(maximum, maximum_v);
        _mm256_store_ps((float *)mask, mask_v);

        const int pool_index = ph * POOLED_WIDTH + pw;
        for (int j = 0; j < 8; ++j) {
          pool_top_data_cur[pool_index + j*POOLED_WIDTH*POOLED_WIDTH] = maximum[j];
          mask_cur[pool_index + j*POOLED_WIDTH*POOLED_WIDTH] = mask[j];
        }
      }
    }

    if (0 == tid) pool_cycle += __rdtsc() - t;
#endif
  } // for each out channel
}

static void __attribute__((noinline)) sconv2_overfeat(
    // input features
    const float *input,
    // weights
    const int *rowptr, const int *colidx, const float *values,
    // bias (for the case when bias is fused with convolution)
    const float *bias,
    // pooling (for the case when pooling is fused with convolution)
    float *pool_top, int *mask,
    // output features
    float *output, int out_channels)
{
  int nthreads = omp_get_num_threads();
  int tid = omp_get_thread_num();

  int WIDTH = 28;
  int WOUT = 24;
  int PAD = 0;

  int nthread_groups = nthreads;
#ifdef __AVX512F__
//  nthread_groups = NTILES;
#endif
  assert(nthreads%nthread_groups == 0);
  int nthreads_per_group = nthreads/nthread_groups;
  int gid = tid/nthreads_per_group;
  int tid_in_group = tid%nthreads_per_group;

  int c_per_thread = (out_channels/8 + nthreads_per_group - 1)/nthreads_per_group;
  int c_begin = std::min(c_per_thread*tid_in_group, out_channels/8);
  int c_end = std::min(c_begin + c_per_thread, out_channels/8);

  for (int out_channel = c_begin*8; out_channel < c_end*8; ++out_channel) {
    unsigned long long t = __rdtsc();

    int out_channel_offset = tid_in_group*8 + out_channel%8;

    int j;
    int jbegin = rowptr[out_channel];
    int jend = rowptr[out_channel + 1];
    int off;

#ifdef __AVX512F__
    __m512 sum[2][12];
    __m512 w_v;
    __m512 bias_v = _mm512_set1_ps(bias[out_channel]);

    // upper half
    int hbegin = 0, hend = 12;
#pragma unroll(12)
    for (int h = hbegin; h < hend; ++h) {
      sum[0][h - hbegin] = bias_v;
      sum[1][h - hbegin] = bias_v;
    }

    for (j = jbegin; j < jend; ++j) {
      w_v = _mm512_set1_ps(values[j]);
      off = colidx[j];

#pragma unroll(12)
      for (int h = hbegin; h < hend; ++h) {
        sum[0][h - hbegin] = _mm512_fmadd_ps(w_v, _mm512_loadu_ps(input + off + h*(WIDTH + PAD)), sum[0][h - hbegin]);
        sum[1][h - hbegin] = _mm512_fmadd_ps(w_v, _mm512_loadu_ps(input + off + h*(WIDTH + PAD) + 16), sum[1][h - hbegin]);
      }
    }

#pragma unroll(12)
    for (int h = hbegin; h < hend; ++h) {
      _mm512_storeu_ps(output + (out_channel*WOUT + h)*WOUT, sum[0][h - hbegin]);
      _mm512_mask_storeu_ps(output + (out_channel*WOUT + h)*WOUT + 16, 0x00ff, sum[1][h - hbegin]);
    }

    // lower half
    hbegin = 12; hend = 24;

#pragma unroll(12)
    for (int h = hbegin; h < hend; ++h) {
      sum[0][h - hbegin] = bias_v;
      sum[1][h - hbegin] = bias_v;
    }

    for (j = jbegin; j < jend; ++j) {
      w_v = _mm512_set1_ps(values[j]);
      off = colidx[j];

#pragma unroll(12)
      for (int h = hbegin; h < hend; ++h) {
        sum[0][h - hbegin] = _mm512_fmadd_ps(w_v, _mm512_loadu_ps(input + off + h*(WIDTH + PAD)), sum[0][h - hbegin]);
        sum[1][h - hbegin] = _mm512_fmadd_ps(w_v, _mm512_loadu_ps(input + off + h*(WIDTH + PAD) + 16), sum[1][h - hbegin]);
      }
    }

#pragma unroll(12)
    for (int h = hbegin; h < hend; ++h) {
      _mm512_storeu_ps(output + (out_channel*WOUT + h)*WOUT, sum[0][h - hbegin]);
      _mm512_mask_storeu_ps(output + (out_channel*WOUT + h)*WOUT + 16, 0x00ff, sum[1][h - hbegin]);
    }
#else
    __m256 sum[5][3];
    __m256 w_v;
    __m256 bias_v = _mm256_set1_ps(bias[out_channel]);

    // (0, 0) block

#undef MY_FMADD
#define MY_FMADD(HBEGIN, WBEGIN) \
    sum[0][0] = bias_v; \
    sum[0][1] = bias_v; \
    sum[0][2] = bias_v; \
    sum[1][0] = bias_v; \
    sum[1][1] = bias_v; \
    sum[1][2] = bias_v; \
    sum[2][0] = bias_v; \
    sum[2][1] = bias_v; \
    sum[2][2] = bias_v; \
    sum[3][0] = bias_v; \
    sum[3][1] = bias_v; \
    sum[3][2] = bias_v; \
    sum[4][0] = bias_v; \
    sum[4][1] = bias_v; \
    sum[4][2] = bias_v; \
\
    for (j = jbegin; j < jend; ++j) { \
      w_v = _mm256_set1_ps(values[j]); \
      off = colidx[j]; \
\
      sum[0][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 0)*(WIDTH + PAD) + WBEGIN), sum[0][0]); \
      sum[0][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 0)*(WIDTH + PAD) + WBEGIN + 8), sum[0][1]); \
      sum[0][2] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 0)*(WIDTH + PAD) + WBEGIN + 16), sum[0][2]); \
      sum[1][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 1)*(WIDTH + PAD) + WBEGIN), sum[1][0]); \
      sum[1][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 1)*(WIDTH + PAD) + WBEGIN + 8), sum[1][1]); \
      sum[1][2] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 1)*(WIDTH + PAD) + WBEGIN + 16), sum[1][2]); \
      sum[2][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 2)*(WIDTH + PAD) + WBEGIN), sum[2][0]); \
      sum[2][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 2)*(WIDTH + PAD) + WBEGIN + 8), sum[2][1]); \
      sum[2][2] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 2)*(WIDTH + PAD) + WBEGIN + 16), sum[2][2]); \
      sum[3][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 3)*(WIDTH + PAD) + WBEGIN), sum[3][0]); \
      sum[3][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 3)*(WIDTH + PAD) + WBEGIN + 8), sum[3][1]); \
      sum[3][2] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 3)*(WIDTH + PAD) + WBEGIN + 16), sum[3][2]); \
      sum[4][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 4)*(WIDTH + PAD) + WBEGIN), sum[4][0]); \
      sum[4][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 4)*(WIDTH + PAD) + WBEGIN + 8), sum[4][1]); \
      sum[4][2] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 4)*(WIDTH + PAD) + WBEGIN + 16), sum[4][2]); \
    } \
\
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 0)*WOUT + WBEGIN, sum[0][0]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 0)*WOUT + WBEGIN + 8, sum[0][1]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 0)*WOUT + WBEGIN + 16, sum[0][2]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 1)*WOUT + WBEGIN, sum[1][0]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 1)*WOUT + WBEGIN + 8, sum[1][1]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 1)*WOUT + WBEGIN + 16, sum[1][2]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 2)*WOUT + WBEGIN, sum[2][0]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 2)*WOUT + WBEGIN + 8, sum[2][1]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 2)*WOUT + WBEGIN + 16, sum[2][2]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 3)*WOUT + WBEGIN, sum[3][0]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 3)*WOUT + WBEGIN + 8, sum[3][1]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 3)*WOUT + WBEGIN + 16, sum[3][2]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 4)*WOUT + WBEGIN, sum[4][0]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 4)*WOUT + WBEGIN + 8, sum[4][1]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 4)*WOUT + WBEGIN + 16, sum[4][2]);

    MY_FMADD(0, 0);
    MY_FMADD(5, 0);
    MY_FMADD(10, 0);
    MY_FMADD(15, 0);
#undef MY_FMADD

#undef MY_FMADD_REMAINDER
#define MY_FMADD_REMAINDER(HBEGIN, WBEGIN) \
    sum[0][0] = bias_v; \
    sum[0][1] = bias_v; \
    sum[0][2] = bias_v; \
    sum[1][0] = bias_v; \
    sum[1][1] = bias_v; \
    sum[1][2] = bias_v; \
    sum[2][0] = bias_v; \
    sum[2][1] = bias_v; \
    sum[2][2] = bias_v; \
    sum[3][0] = bias_v; \
    sum[3][1] = bias_v; \
    sum[3][2] = bias_v; \
\
    for (j = jbegin; j < jend; ++j) { \
      w_v = _mm256_set1_ps(values[j]); \
      off = colidx[j]; \
\
      sum[0][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 0)*(WIDTH + PAD) + WBEGIN), sum[0][0]); \
      sum[0][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 0)*(WIDTH + PAD) + WBEGIN + 8), sum[0][1]); \
      sum[0][2] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 0)*(WIDTH + PAD) + WBEGIN + 16), sum[0][2]); \
      sum[1][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 1)*(WIDTH + PAD) + WBEGIN), sum[1][0]); \
      sum[1][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 1)*(WIDTH + PAD) + WBEGIN + 8), sum[1][1]); \
      sum[1][2] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 1)*(WIDTH + PAD) + WBEGIN + 16), sum[1][2]); \
      sum[2][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 2)*(WIDTH + PAD) + WBEGIN), sum[2][0]); \
      sum[2][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 2)*(WIDTH + PAD) + WBEGIN + 8), sum[2][1]); \
      sum[2][2] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 2)*(WIDTH + PAD) + WBEGIN + 16), sum[2][2]); \
      sum[3][0] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 3)*(WIDTH + PAD) + WBEGIN), sum[3][0]); \
      sum[3][1] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 3)*(WIDTH + PAD) + WBEGIN + 8), sum[3][1]); \
      sum[3][2] = _mm256_fmadd_ps(w_v, _mm256_loadu_ps(input + off + (HBEGIN + 3)*(WIDTH + PAD) + WBEGIN + 16), sum[3][2]); \
    } \
\
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 0)*WOUT + WBEGIN, sum[0][0]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 0)*WOUT + WBEGIN + 8, sum[0][1]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 0)*WOUT + WBEGIN + 16, sum[0][2]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 1)*WOUT + WBEGIN, sum[1][0]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 1)*WOUT + WBEGIN + 8, sum[1][1]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 1)*WOUT + WBEGIN + 16, sum[1][2]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 2)*WOUT + WBEGIN, sum[2][0]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 2)*WOUT + WBEGIN + 8, sum[2][1]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 2)*WOUT + WBEGIN + 16, sum[2][2]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 3)*WOUT + WBEGIN, sum[3][0]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 3)*WOUT + WBEGIN + 8, sum[3][1]); \
    _mm256_store_ps(output + (out_channel*WOUT + HBEGIN + 3)*WOUT + WBEGIN + 16, sum[3][2]);

    MY_FMADD_REMAINDER(20, 0);
#undef MY_FMADD_REMAINDER
#endif

    conv_cycles_of_this_batch[tid*16] += __rdtsc() - t;
  } // for each out channel
}

template<>
void caffe_cpu_sconv(
    // input features
    const float *input_padded, int in_channels,
    int height, int width,
    int pad_h, int pad_w,
    int stride_h, int stride_w,
    int dilation_h, int dilation_w,
    // weights
    const int *rowptr, const int *colidx, const float *values,
    int kernel_h, int kernel_w,
    const int **rowptr_blocked, const int **colidx_blocked, const float **values_blocked,
    int ncolblocks,
    // bias (for the case when bias is fused with convolution)
    const float *bias,
    // pooling (for the case when pooling is fused with convolution)
    float *pool_top, int *mask,
    // output features
    float *output,
    int out_channels,
    float *output_scratch)
{
  const int output_h = (height + 2 * pad_h -
      (dilation_h * (kernel_h - 1) + 1)) / stride_h + 1;
  const int output_w = (width + 2 * pad_w -
      (dilation_w * (kernel_w - 1) + 1)) / stride_w + 1;

  if (dilation_h != 1 || dilation_w != 1) {
    // fall through to the default path
  }
  else if (height == 27 && width == 27 && pad_h == 2 && pad_w == 2 && stride_h == 1 && stride_w == 1 && kernel_w == 5 && kernel_h == 5) {
      sconv2_fused(
        input_padded,
        rowptr, colidx, values,
        bias,
        pool_top, mask,
        output,
        out_channels);
      return;
  }
  else if (height == 28 && width == 28 && pad_h == 0 && pad_w == 0 && stride_h == 1 && stride_w == 1 && kernel_w == 5 && kernel_h == 5) {
    sconv2_overfeat(
      input_padded,
      rowptr, colidx, values,
      bias,
      pool_top, mask,
      output,
      out_channels);
    return;
  }
  else if (pad_h == 1 && pad_w == 1 && stride_h == 1 && stride_w == 1 && kernel_h == 3 && kernel_w == 3 && height == width) {
    if (width == 12) {
      // overfeat
      sconv_3x3_pad1<12>(
          input_padded,
          rowptr_blocked, colidx_blocked, values_blocked, ncolblocks,
          bias,
          output, out_channels, output_scratch);
      return;
    }
    else if (width == 13) {
      // alexnet conv3-5
      sconv_3x3_pad1<13>(
          input_padded,
          rowptr_blocked, colidx_blocked, values_blocked, ncolblocks,
          bias,
          output, out_channels, output_scratch);
      return;
    }
    // the following sizes are used by GoogLeNet
    else if (width == 7) {
      sconv_3x3_pad1<7>(
          input_padded,
          rowptr_blocked, colidx_blocked, values_blocked, ncolblocks,
          bias,
          output, out_channels, output_scratch);
      return;
    }
    else if (width == 14) {
      sconv_3x3_pad1<14>(
          input_padded,
          rowptr_blocked, colidx_blocked, values_blocked, ncolblocks,
          bias,
          output, out_channels, output_scratch);
      return;
    }
    else if (width == 28) {
      sconv_3x3_pad1<28>(
          input_padded,
          rowptr_blocked, colidx_blocked, values_blocked, ncolblocks,
          bias,
          output, out_channels, output_scratch);
      return;
    }
    else if (width == 56) {
      sconv_3x3_pad1<56>(
          input_padded,
          rowptr_blocked, colidx_blocked, values_blocked, ncolblocks,
          bias,
          output, out_channels, output_scratch);
      return;
    }
  }
  else if (height == 227 && width == 227 && pad_h == 0 && pad_w == 0 && stride_h == 4 && stride_w == 4 && kernel_w == 11 && kernel_h == 11) {
    int WIDTH = 227;
    int STRIDE = 4;
    int K = 11;
    int WOUT = (WIDTH - K)/STRIDE + 1; // 55
    const int JBLOCK = 128;
    const int HBLOCK = 8;
    const int WBLOCK = 9;

    __declspec(aligned(64)) float sum[WOUT*WOUT];

    for (int out_channel = 0; out_channel < out_channels; ++out_channel) {
      int jbegin = rowptr[out_channel];
      int jend = std::min(jbegin + JBLOCK, rowptr[out_channel + 1]);

      for (int hbegin = 0; hbegin < WOUT; hbegin += HBLOCK) {
        int hend = std::min(hbegin + HBLOCK, WOUT);

        for (int wbegin = 0; wbegin < WOUT; wbegin += WBLOCK) {
          int wend = std::min(wbegin + WBLOCK, WOUT);

          for (int k = 0; k < (hend - hbegin) * (wend - wbegin); ++k) {
            sum[k] = 0;
          }

          for (int j = jbegin; j < jend; ++j) {
            float c = values[j];
            int off = colidx[j];
            int k = 0;
            for (int h = hbegin; h < hend; ++h) {
              for (int w = wbegin; w < wend; ++w, ++k) {
                sum[k] += c*input_padded[off + (h*WIDTH + w)*STRIDE];
              }
            }
          }

          int k = 0;
          for (int h = hbegin; h < hend; ++h) {
            for (int w = wbegin; w < wend; ++w, ++k) {
              output[(out_channel*WOUT + h)*WOUT + w] = sum[k];
            }
          }
        }
      }
      jbegin += JBLOCK;

      for ( ; jbegin < rowptr[out_channel + 1]; jbegin += JBLOCK) {
        int jend = std::min(jbegin + JBLOCK, rowptr[out_channel + 1]);

        for (int hbegin = 0; hbegin < WOUT; hbegin += HBLOCK) {
          int hend = std::min(hbegin + HBLOCK, WOUT);

          for (int wbegin = 0; wbegin < WOUT; wbegin += WBLOCK) {
            int wend = std::min(wbegin + WBLOCK, WOUT);

            for (int k = 0; k < (hend - hbegin) * (wend - wbegin); ++k) {
              sum[k] = 0;
            }

            for (int j = jbegin; j < jend; ++j) {
              float c = values[j];
              int off = colidx[j];
              int k = 0;
              for (int h = hbegin; h < hend; ++h) {
                for (int w = wbegin; w < wend; ++w, ++k) {
                  sum[k] += c*input_padded[off + (h*WIDTH + w)*STRIDE];
                }
              }
            }

            int k = 0;
            for (int h = hbegin; h < hend; ++h) {
              for (int w = wbegin; w < wend; ++w, ++k) {
                output[(out_channel*WOUT + h)*WOUT + w] += sum[k];
              }
            }
          }
        }
      }

//            for (int j = rowptr[out_channel]; j < rowptr[out_channel + 1]; ++j) {
//              float c = values[j];
//              int off = colidx[j];
//              for (int h = 0; h < WOUT/2; ++h) {
//                for (int w = WOUT/2; w < WOUT; ++w) {
//                  sum[h*WOUT + w] += c*in_temp[off + (h*WIDTH + w)*STRIDE];
//                }
//              }
//            }
//
//            for (int h = 0; h < WOUT/2; ++h) {
//              for (int w = WOUT/2; w < WOUT; ++w) {
//                output[(out_channel*WOUT + h)*WOUT + w] = sum[h*WOUT + w];
//              }
//            }
//
//            for (int h = WOUT/2; h < WOUT; ++h) {
//              for (int w = 0; w < WOUT/2; ++w) {
//                sum[h*WOUT + w] = 0;
//              }
//            }
//
//            for (int j = rowptr[out_channel]; j < rowptr[out_channel + 1]; ++j) {
//              float c = values[j];
//              int off = colidx[j];
//              for (int h = WOUT/2; h < WOUT; ++h) {
//                for (int w = 0; w < WOUT/2; ++w) {
//                  sum[h*WOUT + w] += c*in_temp[off + (h*WIDTH + w)*STRIDE];
//                }
//              }
//            }
//
//            for (int h = WOUT/2; h < WOUT; ++h) {
//              for (int w = 0; w < WOUT/2; ++w) {
//                output[(out_channel*WOUT + h)*WOUT + w] = sum[h*WOUT + w];
//              }
//            }
//
//            for (int h = WOUT/2; h < WOUT; ++h) {
//              for (int w = WOUT/2; w < WOUT; ++w) {
//                sum[h*WOUT + w] = 0;
//              }
//            }
//
//            for (int j = rowptr[out_channel]; j < rowptr[out_channel + 1]; ++j) {
//              float c = values[j];
//              int off = colidx[j];
//              for (int h = WOUT/2; h < WOUT; ++h) {
//                for (int w = WOUT/2; w < WOUT; ++w) {
//                  sum[h*WOUT + w] += c*in_temp[off + (h*WIDTH + w)*STRIDE];
//                }
//              }
//            }
//
//            for (int h = WOUT/2; h < WOUT; ++h) {
//              for (int w = WOUT/2; w < WOUT; ++w) {
//                output[(out_channel*WOUT + h)*WOUT + w] = sum[h*WOUT + w];
//              }
//            }
    }
    return;
  }

  LOG(WARNING) <<
      "Inefficient code path: kernel " << kernel_w << "x" << kernel_h <<
      " image " << width << "x" << height <<
      " pad " << pad_w << "x" << pad_h <<
      " stride " << stride_w << "x" << stride_h <<
      " dilation " << dilation_w << "x" << dilation_h;

  caffe_cpu_sconv_default(
      // input features
      input_padded, in_channels,
      height, width,
      pad_h, pad_w,
      stride_h, stride_w,
      dilation_h, dilation_w,
      // weights
      rowptr, colidx, values,
      kernel_h, kernel_w,
      bias,
      // output features
      output, out_channels);
}

}  // namespace caffe
