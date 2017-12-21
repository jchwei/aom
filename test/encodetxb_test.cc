/*
 * Copyright (c) 2017, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "third_party/googletest/src/googletest/include/gtest/gtest.h"

#include "./aom_config.h"
#include "./av1_rtcd.h"
#include "aom_ports/aom_timer.h"
#include "aom_ports/mem.h"
#include "av1/common/idct.h"
#include "av1/common/onyxc_int.h"
#include "av1/common/scan.h"
#include "av1/common/txb_common.h"
#include "av1/decoder/decodeframe.h"
#include "test/acm_random.h"
#include "test/clear_system_state.h"
#include "test/register_state_check.h"
#include "test/util.h"

namespace {
using libaom_test::ACMRandom;

typedef void (*GetNzMapContextsFunc)(const uint8_t *const levels,
                                     const int16_t *const scan,
                                     const uint16_t eob, const TX_SIZE tx_size,
                                     const TX_TYPE tx_type,
                                     int8_t *const coeff_contexts);

class EncodeTxbTest : public ::testing::TestWithParam<GetNzMapContextsFunc> {
 public:
  EncodeTxbTest() : get_nz_map_contexts_func_(GetParam()) {}

  virtual ~EncodeTxbTest() {}

  virtual void SetUp() {
    coeff_contexts_ref_ = reinterpret_cast<int8_t *>(
        aom_memalign(16, sizeof(*coeff_contexts_ref_) * MAX_TX_SQUARE));
    ASSERT_TRUE(coeff_contexts_ref_ != NULL);
    coeff_contexts_ = reinterpret_cast<int8_t *>(
        aom_memalign(16, sizeof(*coeff_contexts_) * MAX_TX_SQUARE));
    ASSERT_TRUE(coeff_contexts_ != NULL);
  }

  virtual void TearDown() {
    aom_free(coeff_contexts_ref_);
    aom_free(coeff_contexts_);
    libaom_test::ClearSystemState();
  }

  void GetNzMapContextsRun() {
    const int kNumTests = 10;
    int result = 0;

    for (int is_inter = 0; is_inter < 2; ++is_inter) {
      for (int tx_type = DCT_DCT; tx_type < TX_TYPES; ++tx_type) {
        for (int tx_size = TX_4X4; tx_size < TX_SIZES_ALL; ++tx_size) {
          const int bwl = get_txb_bwl((TX_SIZE)tx_size);
          const int width = get_txb_wide((TX_SIZE)tx_size);
          const int height = get_txb_high((TX_SIZE)tx_size);
          const int real_width = tx_size_wide[tx_size];
          const int real_height = tx_size_high[tx_size];
          const int16_t *const scan =
              is_inter ? av1_inter_scan_orders[tx_size][tx_type].scan
                       : av1_intra_scan_orders[tx_size][tx_type].scan;

          levels_ = set_levels(levels_buf_, width);
          for (int i = 0; i < kNumTests && !result; ++i) {
            for (int eob = 1; eob <= width * height && !result; ++eob) {
              InitDataWithEob(scan, bwl, eob);

              av1_get_nz_map_contexts_c(levels_, scan, eob, (TX_SIZE)tx_size,
                                        (TX_TYPE)tx_type, coeff_contexts_ref_);
              get_nz_map_contexts_func_(levels_, scan, eob, (TX_SIZE)tx_size,
                                        (TX_TYPE)tx_type, coeff_contexts_);

              result = Compare(scan, eob);

              EXPECT_EQ(result, 0)
                  << " tx_class " << tx_type_to_class[tx_type] << " width "
                  << real_width << " height " << real_height << " eob " << eob;
            }
          }
        }
      }
    }
  }

  void SpeedTestGetNzMapContextsRun() {
    const int kNumTests = 2000000000;
    aom_usec_timer timer;

    printf("Note: Only test the largest possible eob case!\n");
    for (int tx_size = TX_4X4; tx_size < TX_SIZES_ALL; ++tx_size) {
      const int bwl = get_txb_bwl((TX_SIZE)tx_size);
      const int width = get_txb_wide((TX_SIZE)tx_size);
      const int height = get_txb_high((TX_SIZE)tx_size);
      const int real_width = tx_size_wide[tx_size];
      const int real_height = tx_size_high[tx_size];
      const TX_TYPE tx_type = DCT_DCT;
      const int16_t *const scan = av1_inter_scan_orders[tx_size][tx_type].scan;
      const int eob = width * height;
      const int numTests = kNumTests / (width * height);

      levels_ = set_levels(levels_buf_, width);
      InitDataWithEob(scan, bwl, eob);

      aom_usec_timer_start(&timer);
      for (int i = 0; i < numTests; ++i) {
        get_nz_map_contexts_func_(levels_, scan, eob, (TX_SIZE)tx_size, tx_type,
                                  coeff_contexts_);
      }
      aom_usec_timer_mark(&timer);

      const int elapsed_time = static_cast<int>(aom_usec_timer_elapsed(&timer));
      printf("get_nz_map_contexts_%2dx%2d: %7.1f ms\n", real_width, real_height,
             elapsed_time / 1000.0);
    }
  }

 private:
  void InitDataWithEob(const int16_t *const scan, const int bwl,
                       const int eob) {
    memset(levels_buf_, 0, sizeof(levels_buf_));
    memset(coeff_contexts_, 0, sizeof(*coeff_contexts_) * MAX_TX_SQUARE);

    for (int c = 0; c < eob; ++c) {
      levels_[get_padded_idx(scan[c], bwl)] =
          static_cast<uint8_t>(clamp(rnd_.Rand8(), 0, INT8_MAX));
      coeff_contexts_[scan[c]] = rnd_.Rand16() >> 1;
    }

    memcpy(coeff_contexts_ref_, coeff_contexts_,
           sizeof(*coeff_contexts_) * MAX_TX_SQUARE);
  }

  bool Compare(const int16_t *const scan, const int eob) const {
    bool result = false;
    if (memcmp(coeff_contexts_, coeff_contexts_ref_,
               sizeof(*coeff_contexts_ref_) * MAX_TX_SQUARE)) {
      for (int i = 0; i < eob; i++) {
        const int pos = scan[i];
        if (coeff_contexts_ref_[pos] != coeff_contexts_[pos]) {
          printf("coeff_contexts_[%d] diff:%6d (ref),%6d (opt)\n", pos,
                 coeff_contexts_ref_[pos], coeff_contexts_[pos]);
          result = true;
          break;
        }
      }
    }
    return result;
  }

  GetNzMapContextsFunc get_nz_map_contexts_func_;
  ACMRandom rnd_;
  uint8_t levels_buf_[TX_PAD_2D];
  uint8_t *levels_;
  int8_t *coeff_contexts_ref_;
  int8_t *coeff_contexts_;
};

TEST_P(EncodeTxbTest, GetNzMapContexts) { GetNzMapContextsRun(); }

TEST_P(EncodeTxbTest, DISABLED_SpeedTestGetNzMapContexts) {
  SpeedTestGetNzMapContextsRun();
}

#if HAVE_SSE2
INSTANTIATE_TEST_CASE_P(SSE2, EncodeTxbTest,
                        ::testing::Values(av1_get_nz_map_contexts_sse2));
#endif
}  // namespace