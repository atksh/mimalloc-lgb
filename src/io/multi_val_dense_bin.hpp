/*!
 * Copyright (c) 2020 Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License. See LICENSE file in the project root for license information.
 */
#ifndef LIGHTGBM_IO_MULTI_VAL_DENSE_BIN_HPP_
#define LIGHTGBM_IO_MULTI_VAL_DENSE_BIN_HPP_

#include <LightGBM/bin.h>
#include <LightGBM/utils/openmp_wrapper.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <mimalloc.h>

namespace LightGBM
{

  template <typename VAL_T>
  class MultiValDenseBin : public MultiValBin
  {
  public:
    explicit MultiValDenseBin(data_size_t num_data, int num_bin, int num_feature)
        : num_data_(num_data), num_bin_(num_bin), num_feature_(num_feature)
    {
      data_.resize(static_cast<size_t>(num_data_) * num_feature_, static_cast<VAL_T>(0));
    }

    ~MultiValDenseBin()
    {
    }

    data_size_t num_data() const override
    {
      return num_data_;
    }

    int num_bin() const override
    {
      return num_bin_;
    }

    double num_element_per_row() const override { return num_feature_; }

    void PushOneRow(int, data_size_t idx, const std::vector<uint32_t, mi_stl_allocator<uint32_t>> &values) override
    {
      auto start = RowPtr(idx);
      for (auto i = 0; i < num_feature_; ++i)
      {
        data_[start + i] = static_cast<VAL_T>(values[i]);
      }
    }

    void FinishLoad() override
    {
    }

    bool IsSparse() override
    {
      return false;
    }

    template <bool USE_INDICES, bool USE_PREFETCH, bool ORDERED>
    void ConstructHistogramInner(const data_size_t *data_indices, data_size_t start, data_size_t end,
                                 const score_t *gradients, const score_t *hessians, hist_t *out) const
    {
      data_size_t i = start;
      hist_t *grad = out;
      hist_t *hess = out + 1;
      if (USE_PREFETCH)
      {
        const data_size_t pf_offset = 32 / sizeof(VAL_T);
        const data_size_t pf_end = end - pf_offset;

        for (; i < pf_end; ++i)
        {
          const auto idx = USE_INDICES ? data_indices[i] : i;
          const auto pf_idx = USE_INDICES ? data_indices[i + pf_offset] : i + pf_offset;
          if (!ORDERED)
          {
            PREFETCH_T0(gradients + pf_idx);
            PREFETCH_T0(hessians + pf_idx);
          }
          PREFETCH_T0(data_.data() + RowPtr(pf_idx));
          const auto j_start = RowPtr(idx);
          for (auto j = j_start; j < j_start + num_feature_; ++j)
          {
            const auto ti = static_cast<uint32_t>(data_[j]) << 1;
            if (ORDERED)
            {
              grad[ti] += gradients[i];
              hess[ti] += hessians[i];
            }
            else
            {
              grad[ti] += gradients[idx];
              hess[ti] += hessians[idx];
            }
          }
        }
      }
      for (; i < end; ++i)
      {
        const auto idx = USE_INDICES ? data_indices[i] : i;
        const auto j_start = RowPtr(idx);
        for (auto j = j_start; j < j_start + num_feature_; ++j)
        {
          const auto ti = static_cast<uint32_t>(data_[j]) << 1;
          if (ORDERED)
          {
            grad[ti] += gradients[i];
            hess[ti] += hessians[i];
          }
          else
          {
            grad[ti] += gradients[idx];
            hess[ti] += hessians[idx];
          }
        }
      }
    }

    void ConstructHistogram(const data_size_t *data_indices, data_size_t start,
                            data_size_t end, const score_t *gradients,
                            const score_t *hessians, hist_t *out) const override
    {
      ConstructHistogramInner<true, true, false>(data_indices, start, end,
                                                 gradients, hessians, out);
    }

    void ConstructHistogram(data_size_t start, data_size_t end,
                            const score_t *gradients, const score_t *hessians,
                            hist_t *out) const override
    {
      ConstructHistogramInner<false, false, false>(
          nullptr, start, end, gradients, hessians, out);
    }

    void ConstructHistogramOrdered(const data_size_t *data_indices,
                                   data_size_t start, data_size_t end,
                                   const score_t *gradients,
                                   const score_t *hessians,
                                   hist_t *out) const override
    {
      ConstructHistogramInner<true, true, true>(data_indices, start, end,
                                                gradients, hessians, out);
    }

    MultiValBin *CreateLike(data_size_t num_data, int num_bin, int num_feature, double) const override
    {
      return new MultiValDenseBin<VAL_T>(num_data, num_bin, num_feature);
    }

    void ReSize(data_size_t num_data, int num_bin, int num_feature,
                double) override
    {
      num_data_ = num_data;
      num_bin_ = num_bin;
      num_feature_ = num_feature;
      size_t new_size = static_cast<size_t>(num_feature_) * num_data_;
      if (data_.size() < new_size)
      {
        data_.resize(new_size, 0);
      }
    }

    template <bool SUBROW, bool SUBCOL>
    void CopyInner(const MultiValBin *full_bin, const data_size_t *used_indices,
                   data_size_t num_used_indices,
                   const std::vector<int, mi_stl_allocator<int>> &used_feature_index,
                   const std::vector<uint32_t, mi_stl_allocator<uint32_t>> &delta)
    {
      const auto other_bin =
          reinterpret_cast<const MultiValDenseBin<VAL_T> *>(full_bin);
      if (SUBROW)
      {
        CHECK_EQ(num_data_, num_used_indices);
      }
      int n_block = 1;
      data_size_t block_size = num_data_;
      Threading::BlockInfo<data_size_t>(num_data_, 1024, &n_block,
                                        &block_size);
#pragma omp parallel for schedule(static, 1)
      for (int tid = 0; tid < n_block; ++tid)
      {
        data_size_t start = tid * block_size;
        data_size_t end = std::min(num_data_, start + block_size);
        for (data_size_t i = start; i < end; ++i)
        {
          const auto j_start = RowPtr(i);
          const auto other_j_start =
              SUBROW ? other_bin->RowPtr(used_indices[i]) : other_bin->RowPtr(i);
          for (int j = 0; j < num_feature_; ++j)
          {
            if (SUBCOL)
            {
              if (other_bin->data_[other_j_start + used_feature_index[j]] > 0)
              {
                data_[j_start + j] = static_cast<VAL_T>(
                    other_bin->data_[other_j_start + used_feature_index[j]] -
                    delta[j]);
              }
              else
              {
                data_[j_start + j] = 0;
              }
            }
            else
            {
              data_[j_start + j] =
                  static_cast<VAL_T>(other_bin->data_[other_j_start + j]);
            }
          }
        }
      }
    }

    void CopySubrow(const MultiValBin *full_bin, const data_size_t *used_indices,
                    data_size_t num_used_indices) override
    {
      CopyInner<true, false>(full_bin, used_indices, num_used_indices,
                             std::vector<int, mi_stl_allocator<int>>(), std::vector<uint32_t, mi_stl_allocator<uint32_t>>());
    }

    void CopySubcol(const MultiValBin *full_bin,
                    const std::vector<int, mi_stl_allocator<int>> &used_feature_index,
                    const std::vector<uint32_t, mi_stl_allocator<uint32_t>> &,
                    const std::vector<uint32_t, mi_stl_allocator<uint32_t>> &,
                    const std::vector<uint32_t, mi_stl_allocator<uint32_t>> &delta) override
    {
      CopyInner<false, true>(full_bin, nullptr, num_data_, used_feature_index,
                             delta);
    }

    void CopySubrowAndSubcol(const MultiValBin *full_bin,
                             const data_size_t *used_indices,
                             data_size_t num_used_indices,
                             const std::vector<int, mi_stl_allocator<int>> &used_feature_index,
                             const std::vector<uint32_t, mi_stl_allocator<uint32_t>> &,
                             const std::vector<uint32_t, mi_stl_allocator<uint32_t>> &,
                             const std::vector<uint32_t, mi_stl_allocator<uint32_t>> &delta) override
    {
      CopyInner<true, true>(full_bin, used_indices, num_used_indices,
                            used_feature_index, delta);
    }

    inline size_t RowPtr(data_size_t idx) const
    {
      return static_cast<size_t>(idx) * num_feature_;
    }

    MultiValDenseBin<VAL_T> *Clone() override;

  private:
    data_size_t num_data_;
    int num_bin_;
    int num_feature_;
    std::vector<VAL_T, Common::AlignmentAllocator<VAL_T, 32>> data_;

    MultiValDenseBin<VAL_T>(const MultiValDenseBin<VAL_T> &other)
        : num_data_(other.num_data_), num_bin_(other.num_bin_), num_feature_(other.num_feature_), data_(other.data_)
    {
    }
  };

  template <typename VAL_T>
  MultiValDenseBin<VAL_T> *MultiValDenseBin<VAL_T>::Clone()
  {
    return new MultiValDenseBin<VAL_T>(*this);
  }

} // namespace LightGBM
#endif // LIGHTGBM_IO_MULTI_VAL_DENSE_BIN_HPP_
