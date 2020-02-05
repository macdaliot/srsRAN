/*
 * Copyright 2013-2019 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srslte/srslte.h"
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "srslte/phy/ch_estimation/refsignal_ul.h"
#include "srslte/phy/common/phy_common.h"
#include "srslte/phy/common/sequence.h"
#include "srslte/phy/mimo/precoding.h"
#include "srslte/phy/modem/demod_soft.h"
#include "srslte/phy/phch/pucch.h"
#include "srslte/phy/scrambling/scrambling.h"
#include "srslte/phy/utils/debug.h"
#include "srslte/phy/utils/vector.h"

#define MAX_PUSCH_RE(cp) (2 * SRSLTE_CP_NSYMB(cp) * 12)

/** Initializes the PUCCH transmitter and receiver */
int srslte_pucch_init_(srslte_pucch_t* q, bool is_ue)
{
  int ret = SRSLTE_ERROR_INVALID_INPUTS;
  if (q != NULL) {
    ret = SRSLTE_ERROR;
    bzero(q, sizeof(srslte_pucch_t));

    if (srslte_modem_table_lte(&q->mod, SRSLTE_MOD_QPSK)) {
      return SRSLTE_ERROR;
    }

    q->is_ue = is_ue;

    q->users = calloc(sizeof(srslte_pucch_user_t*), q->is_ue ? 1 : (1 + SRSLTE_SIRNTI));
    if (!q->users) {
      perror("malloc");
      goto clean_exit;
    }

    if (srslte_sequence_init(&q->tmp_seq, 20)) {
      goto clean_exit;
    }

    srslte_uci_cqi_pucch_init(&q->cqi);

    q->z     = srslte_vec_malloc(sizeof(cf_t) * SRSLTE_PUCCH_MAX_SYMBOLS);
    q->z_tmp = srslte_vec_malloc(sizeof(cf_t) * SRSLTE_PUCCH_MAX_SYMBOLS);

    if (!q->is_ue) {
      q->ce = srslte_vec_malloc(sizeof(cf_t) * SRSLTE_PUCCH_MAX_SYMBOLS);
    }

    ret = SRSLTE_SUCCESS;
  }
clean_exit:
  if (ret == SRSLTE_ERROR) {
    srslte_pucch_free(q);
  }
  return ret;
}

int srslte_pucch_init_ue(srslte_pucch_t* q)
{
  return srslte_pucch_init_(q, true);
}

int srslte_pucch_init_enb(srslte_pucch_t* q)
{
  return srslte_pucch_init_(q, false);
}

void srslte_pucch_free(srslte_pucch_t* q)
{
  if (q->users) {
    if (q->is_ue) {
      srslte_pucch_free_rnti(q, 0);
    } else {
      for (int rnti = 0; rnti <= SRSLTE_SIRNTI; rnti++) {
        srslte_pucch_free_rnti(q, rnti);
      }
    }
    free(q->users);
  }

  srslte_sequence_free(&q->tmp_seq);

  srslte_uci_cqi_pucch_free(&q->cqi);
  if (q->z) {
    free(q->z);
  }
  if (q->z_tmp) {
    free(q->z_tmp);
  }
  if (q->ce) {
    free(q->ce);
  }

  srslte_modem_table_free(&q->mod);
  bzero(q, sizeof(srslte_pucch_t));
}

int srslte_pucch_set_cell(srslte_pucch_t* q, srslte_cell_t cell)
{
  int ret = SRSLTE_ERROR_INVALID_INPUTS;
  if (q != NULL && srslte_cell_isvalid(&cell)) {

    if (cell.id != q->cell.id || q->cell.nof_prb == 0) {
      q->cell = cell;

      // Precompute group hopping values u.
      if (srslte_group_hopping_f_gh(q->f_gh, q->cell.id)) {
        return SRSLTE_ERROR;
      }

      if (srslte_pucch_n_cs_cell(q->cell, q->n_cs_cell)) {
        return SRSLTE_ERROR;
      }
    }

    ret = SRSLTE_SUCCESS;
  }
  return ret;
}

void srslte_pucch_free_rnti(srslte_pucch_t* q, uint16_t rnti)
{
  uint32_t rnti_idx = q->is_ue ? 0 : rnti;

  if (q->users[rnti_idx]) {
    for (int i = 0; i < SRSLTE_NOF_SF_X_FRAME; i++) {
      srslte_sequence_free(&q->users[rnti_idx]->seq_f2[i]);
    }
    free(q->users[rnti_idx]);
    q->users[rnti_idx] = NULL;
    q->ue_rnti         = 0;
  }
}

int srslte_pucch_set_rnti(srslte_pucch_t* q, uint16_t rnti)
{

  uint32_t rnti_idx = q->is_ue ? 0 : rnti;
  if (!q->users[rnti_idx] || q->is_ue) {
    if (!q->users[rnti_idx]) {
      q->users[rnti_idx] = calloc(1, sizeof(srslte_pucch_user_t));
      if (!q->users[rnti_idx]) {
        perror("calloc");
        return -1;
      }
    }
    q->users[rnti_idx]->sequence_generated = false;
    for (uint32_t sf_idx = 0; sf_idx < SRSLTE_NOF_SF_X_FRAME; sf_idx++) {
      // Precompute scrambling sequence for pucch format 2
      if (srslte_sequence_pucch(&q->users[rnti_idx]->seq_f2[sf_idx], rnti, 2 * sf_idx, q->cell.id)) {
        ERROR("Error computing PUCCH Format 2 scrambling sequence\n");
        srslte_pucch_free_rnti(q, rnti);
        return SRSLTE_ERROR;
      }
    }
    q->ue_rnti                             = rnti;
    q->users[rnti_idx]->cell_id            = q->cell.id;
    q->users[rnti_idx]->sequence_generated = true;
  } else {
    ERROR("Error generating PUSCH sequence: rnti=0x%x already generated\n", rnti);
  }
  return SRSLTE_SUCCESS;
}

static cf_t uci_encode_format1()
{
  return 1.0;
}

static cf_t uci_encode_format1a(uint8_t bit)
{
  return bit ? -1.0 : 1.0;
}

static cf_t uci_encode_format1b(uint8_t bits[2])
{
  if (bits[0] == 0) {
    if (bits[1] == 0) {
      return 1;
    } else {
      return -I;
    }
  } else {
    if (bits[1] == 0) {
      return I;
    } else {
      return -1.0;
    }
  }
}

static srslte_sequence_t* get_user_sequence(srslte_pucch_t* q, uint16_t rnti, uint32_t sf_idx)
{
  uint32_t rnti_idx = q->is_ue ? 0 : rnti;

  // The scrambling sequence is pregenerated for all RNTIs in the eNodeB but only for C-RNTI in the UE
  if (rnti >= SRSLTE_CRNTI_START && rnti < SRSLTE_CRNTI_END) {
    if (q->users[rnti_idx] && q->users[rnti_idx]->sequence_generated && q->users[rnti_idx]->cell_id == q->cell.id &&
        (!q->is_ue || q->ue_rnti == rnti)) {
      return &q->users[rnti_idx]->seq_f2[sf_idx];
    } else {
      if (srslte_sequence_pucch(&q->tmp_seq, rnti, 2 * sf_idx, q->cell.id)) {
        ERROR("Error computing PUCCH Format 2 scrambling sequence\n");
        return NULL;
      }
      return &q->tmp_seq;
    }
  } else {
    ERROR("Invalid RNTI=0x%x\n", rnti);
    return NULL;
  }
}

/* Encode PUCCH bits according to Table 5.4.1-1 in Section 5.4.1 of 36.211 */
static int
uci_mod_bits(srslte_pucch_t* q, srslte_ul_sf_cfg_t* sf, srslte_pucch_cfg_t* cfg, uint8_t bits[SRSLTE_PUCCH_MAX_BITS])
{
  uint8_t            tmp[2];
  srslte_sequence_t* seq;
  switch (cfg->format) {
    case SRSLTE_PUCCH_FORMAT_1:
      q->d[0] = uci_encode_format1();
      break;
    case SRSLTE_PUCCH_FORMAT_1A:
      q->d[0] = uci_encode_format1a(bits[0]);
      break;
    case SRSLTE_PUCCH_FORMAT_1B:
      tmp[0]  = bits[0];
      tmp[1]  = bits[1];
      q->d[0] = uci_encode_format1b(tmp);
      break;
    case SRSLTE_PUCCH_FORMAT_2:
    case SRSLTE_PUCCH_FORMAT_2A:
    case SRSLTE_PUCCH_FORMAT_2B:
      seq = get_user_sequence(q, cfg->rnti, sf->tti % 10);
      if (seq) {
        memcpy(q->bits_scram, bits, SRSLTE_PUCCH2_NOF_BITS * sizeof(uint8_t));
        srslte_scrambling_b_offset(seq, q->bits_scram, 0, SRSLTE_PUCCH2_NOF_BITS);
        srslte_mod_modulate(&q->mod, q->bits_scram, q->d, SRSLTE_PUCCH2_NOF_BITS);
      } else {
        ERROR("Error modulating PUCCH2 bits: could not generate sequence\n");
        return -1;
      }
      break;
    case SRSLTE_PUCCH_FORMAT_3:
      seq = get_user_sequence(q, cfg->rnti, sf->tti % 10);
      if (seq) {
        memcpy(q->bits_scram, bits, SRSLTE_PUCCH3_NOF_BITS * sizeof(uint8_t));
        srslte_scrambling_b_offset(seq, q->bits_scram, 0, SRSLTE_PUCCH3_NOF_BITS);
        srslte_mod_modulate(&q->mod, q->bits_scram, q->d, SRSLTE_PUCCH3_NOF_BITS);
      } else {
        fprintf(stderr, "Error modulating PUCCH3 bits: rnti not set\n");
        return -1;
      }
      break;
    default:
      ERROR("PUCCH format %s not supported\n", srslte_pucch_format_text(cfg->format));
      return SRSLTE_ERROR;
  }
  return SRSLTE_SUCCESS;
}

// Declare this here, since we can not include refsignal_ul.h
void srslte_refsignal_r_uv_arg_1prb(float* arg, uint32_t u);

/* 3GPP 36211 Table 5.5.2.2.2-1: Demodulation reference signal location for different PUCCH formats. */
static const uint32_t pucch_symbol_format1_cpnorm[4]   = {0, 1, 5, 6};
static const uint32_t pucch_symbol_format1_cpext[4]    = {0, 1, 4, 5};
static const uint32_t pucch_symbol_format2_3_cpnorm[5] = {0, 2, 3, 4, 6};
static const uint32_t pucch_symbol_format2_3_cpext[5]  = {0, 1, 2, 4, 5};

static const float w_n_oc[2][3][4] = {
    // Table 5.4.1-2 Orthogonal sequences w for N_sf=4 (complex argument)
    {{0, 0, 0, 0}, {0, M_PI, 0, M_PI}, {0, M_PI, M_PI, 0}},
    // Table 5.4.1-3 Orthogonal sequences w for N_sf=3
    {{0, 0, 0, 0}, {0, 2 * M_PI / 3, 4 * M_PI / 3, 0}, {0, 4 * M_PI / 3, 2 * M_PI / 3, 0}},

};

#if defined(__clang__)

/* Precomputed constants printed with printf %a specifier (hexadecimal notation) for maximum precision
 *    cf_t val = cexpf(I * 2 * M_PI / 5));
 *    printf("%a + %aI\n", str, creal(val), cimag(val));
 *
 * clang expects compile-time contant expressions in the initializer list and using cexpf results
 * in the following error
 *    error: initializer element is not a compile-time constant
 */
static const cf_t cexpf_i_2_mpi_5 = 0x1.3c6ef2p-2 + 0x1.e6f0e2p-1I;
static const cf_t cexpf_i_4_mpi_5 = -0x1.9e377cp-1 + 0x1.2cf22ep-1I;
static const cf_t cexpf_i_6_mpi_5 = -0x1.9e3778p-1 + -0x1.2cf234p-1I;
static const cf_t cexpf_i_8_mpi_5 = 0x1.3c6efcp-2 + -0x1.e6f0ep-1I;

static cf_t pucch3_w_n_oc_5[5][5] = {
    {1, 1, 1, 1, 1},
    {1, cexpf_i_2_mpi_5, cexpf_i_4_mpi_5, cexpf_i_6_mpi_5, cexpf_i_8_mpi_5},
    {1, cexpf_i_4_mpi_5, cexpf_i_8_mpi_5, cexpf_i_2_mpi_5, cexpf_i_6_mpi_5},
    {1, cexpf_i_6_mpi_5, cexpf_i_2_mpi_5, cexpf_i_8_mpi_5, cexpf_i_4_mpi_5},
    {1, cexpf_i_8_mpi_5, cexpf_i_6_mpi_5, cexpf_i_4_mpi_5, cexpf_i_2_mpi_5},
};

#else // defined(__clang__)

static const cf_t pucch3_w_n_oc_5[5][5] = {
    {1, 1, 1, 1, 1},
    {1, cexpf(I * 2 * M_PI / 5), cexpf(I * 4 * M_PI / 5), cexpf(I * 6 * M_PI / 5), cexpf(I * 8 * M_PI / 5)},
    {1, cexpf(I * 4 * M_PI / 5), cexpf(I * 8 * M_PI / 5), cexpf(I * 2 * M_PI / 5), cexpf(I * 6 * M_PI / 5)},
    {1, cexpf(I * 6 * M_PI / 5), cexpf(I * 2 * M_PI / 5), cexpf(I * 8 * M_PI / 5), cexpf(I * 4 * M_PI / 5)},
    {1, cexpf(I * 8 * M_PI / 5), cexpf(I * 6 * M_PI / 5), cexpf(I * 4 * M_PI / 5), cexpf(I * 2 * M_PI / 5)}};

#endif // defined(__clang__)

static const cf_t pucch3_w_n_oc_4[4][4] = {{+1, +1, +1, +1}, {+1, -1, +1, -1}, {+1, +1, -1, -1}, {+1, -1, -1, +1}};

static uint32_t get_N_sf(srslte_pucch_format_t format, uint32_t slot_idx, bool shortened)
{
  switch (format) {
    case SRSLTE_PUCCH_FORMAT_1:
    case SRSLTE_PUCCH_FORMAT_1A:
    case SRSLTE_PUCCH_FORMAT_1B:
      if (!slot_idx) {
        return 4;
      } else {
        return shortened ? 3 : 4;
      }
    case SRSLTE_PUCCH_FORMAT_2:
    case SRSLTE_PUCCH_FORMAT_2A:
    case SRSLTE_PUCCH_FORMAT_2B:
      return 5;
    case SRSLTE_PUCCH_FORMAT_3:
      if (!slot_idx) {
        return 5;
      } else {
        return shortened ? 4 : 5;
      }
    default:
      return 0;
  }
  return 0;
}

static uint32_t get_pucch_symbol(uint32_t m, srslte_pucch_format_t format, srslte_cp_t cp)
{
  switch (format) {
    case SRSLTE_PUCCH_FORMAT_1:
    case SRSLTE_PUCCH_FORMAT_1A:
    case SRSLTE_PUCCH_FORMAT_1B:
      if (m < 4) {
        if (SRSLTE_CP_ISNORM(cp)) {
          return pucch_symbol_format1_cpnorm[m];
        } else {
          return pucch_symbol_format1_cpext[m];
        }
      }
      break;
    case SRSLTE_PUCCH_FORMAT_2:
    case SRSLTE_PUCCH_FORMAT_2A:
    case SRSLTE_PUCCH_FORMAT_2B:
      if (m < 5) {
        if (SRSLTE_CP_ISNORM(cp)) {
          return pucch_symbol_format2_3_cpnorm[m];
        } else {
          return pucch_symbol_format2_3_cpext[m];
        }
      }
      break;
    case SRSLTE_PUCCH_FORMAT_3:
      if (SRSLTE_CP_ISNORM(cp)) {
        return pucch_symbol_format2_3_cpnorm[m % 5];
      } else {
        return pucch_symbol_format2_3_cpext[m % 5];
      }
      break;
    default:
      return 0;
  }
  return 0;
}

/* Map PUCCH symbols to physical resources according to 5.4.3 in 36.211 */
static int pucch_cp(srslte_pucch_t*     q,
                    srslte_ul_sf_cfg_t* sf,
                    srslte_pucch_cfg_t* cfg,
                    cf_t*               source,
                    cf_t*               dest,
                    bool                source_is_grid)
{
  int ret = SRSLTE_ERROR_INVALID_INPUTS;
  if (q && source && dest) {
    ret               = SRSLTE_ERROR;
    uint32_t nsymbols = SRSLTE_CP_ISNORM(q->cell.cp) ? SRSLTE_CP_NORM_NSYMB : SRSLTE_CP_EXT_NSYMB;

    uint32_t n_re   = 0;
    uint32_t N_sf_0 = get_N_sf(cfg->format, 0, sf->shortened);
    for (uint32_t ns = 0; ns < 2; ns++) {
      uint32_t N_sf = get_N_sf(cfg->format, ns % 2, sf->shortened);

      // Determine n_prb
      uint32_t n_prb = srslte_pucch_n_prb(&q->cell, cfg, ns);
      if (n_prb < q->cell.nof_prb) {
        for (uint32_t i = 0; i < N_sf; i++) {
          uint32_t l = get_pucch_symbol(i, cfg->format, q->cell.cp);
          if (!source_is_grid) {
            memcpy(&dest[SRSLTE_RE_IDX(q->cell.nof_prb, l + ns * nsymbols, n_prb * SRSLTE_NRE)],
                   &source[i * SRSLTE_NRE + ns * N_sf_0 * SRSLTE_NRE],
                   SRSLTE_NRE * sizeof(cf_t));
          } else {
            memcpy(&dest[i * SRSLTE_NRE + ns * N_sf_0 * SRSLTE_NRE],
                   &source[SRSLTE_RE_IDX(q->cell.nof_prb, l + ns * nsymbols, n_prb * SRSLTE_NRE)],
                   SRSLTE_NRE * sizeof(cf_t));
          }
          n_re += SRSLTE_NRE;
        }
      } else {
        ERROR("Invalid PUCCH n_prb=%d\n", n_prb);
        return SRSLTE_ERROR;
      }
    }
    ret = n_re;
  }
  return ret;
}

static int pucch_put(srslte_pucch_t* q, srslte_ul_sf_cfg_t* sf, srslte_pucch_cfg_t* cfg, cf_t* z, cf_t* output)
{
  return pucch_cp(q, sf, cfg, z, output, false);
}

static int pucch_get(srslte_pucch_t* q, srslte_ul_sf_cfg_t* sf, srslte_pucch_cfg_t* cfg, cf_t* input, cf_t* z)
{
  return pucch_cp(q, sf, cfg, input, z, true);
}

static int encode_signal_format12(srslte_pucch_t*     q,
                                  srslte_ul_sf_cfg_t* sf,
                                  srslte_pucch_cfg_t* cfg,
                                  uint8_t             bits[SRSLTE_PUCCH_MAX_BITS],
                                  cf_t                z[SRSLTE_PUCCH_MAX_SYMBOLS],
                                  bool                signal_only)
{
  if (!signal_only) {
    if (uci_mod_bits(q, sf, cfg, bits)) {
      ERROR("Error encoding PUCCH bits\n");
      return SRSLTE_ERROR;
    }
  } else {
    for (int i = 0; i < SRSLTE_PUCCH_MAX_BITS / 2; i++) {
      q->d[i] = 1.0;
    }
  }
  uint32_t N_sf_0 = get_N_sf(cfg->format, 0, sf->shortened);
  for (uint32_t ns = 2 * (sf->tti % 10); ns < 2 * ((sf->tti % 10) + 1); ns++) {
    uint32_t N_sf = get_N_sf(cfg->format, ns % 2, sf->shortened);
    DEBUG("ns=%d, N_sf=%d\n", ns, N_sf);
    // Get group hopping number u
    uint32_t f_gh = 0;
    if (cfg->group_hopping_en) {
      f_gh = q->f_gh[ns];
    }
    uint32_t u = (f_gh + (q->cell.id % 30)) % 30;

    srslte_refsignal_r_uv_arg_1prb(q->tmp_arg, u);
    uint32_t N_sf_widx = N_sf == 3 ? 1 : 0;
    for (uint32_t m = 0; m < N_sf; m++) {
      uint32_t l     = get_pucch_symbol(m, cfg->format, q->cell.cp);
      float    alpha = 0;
      if (cfg->format >= SRSLTE_PUCCH_FORMAT_2) {
        alpha = srslte_pucch_alpha_format2(q->n_cs_cell, cfg, ns, l);
        for (uint32_t n = 0; n < SRSLTE_PUCCH_N_SEQ; n++) {
          z[(ns % 2) * N_sf * SRSLTE_PUCCH_N_SEQ + m * SRSLTE_PUCCH_N_SEQ + n] =
              q->d[(ns % 2) * N_sf + m] * cexpf(I * (q->tmp_arg[n] + alpha * n));
        }
      } else {
        uint32_t n_prime_ns = 0;
        uint32_t n_oc       = 0;
        alpha      = srslte_pucch_alpha_format1(q->n_cs_cell, cfg, q->cell.cp, true, ns, l, &n_oc, &n_prime_ns);
        float S_ns = 0;
        if (n_prime_ns % 2) {
          S_ns = M_PI / 2;
        }
        DEBUG("PUCCH d_0: %.1f+%.1fi, alpha: %.1f, n_oc: %d, n_prime_ns: %d, n_rb_2=%d\n",
              __real__ q->d[0],
              __imag__ q->d[0],
              alpha,
              n_oc,
              n_prime_ns,
              cfg->n_rb_2);

        for (uint32_t n = 0; n < SRSLTE_PUCCH_N_SEQ; n++) {
          z[(ns % 2) * N_sf_0 * SRSLTE_PUCCH_N_SEQ + m * SRSLTE_PUCCH_N_SEQ + n] =
              q->d[0] * cexpf(I * (w_n_oc[N_sf_widx][n_oc % 3][m] + q->tmp_arg[n] + alpha * n + S_ns));
        }
      }
    }
  }
  return SRSLTE_SUCCESS;
}

static int encode_signal_format3(srslte_pucch_t*     q,
                                 srslte_ul_sf_cfg_t* sf,
                                 srslte_pucch_cfg_t* cfg,
                                 uint8_t             bits[SRSLTE_PUCCH_MAX_BITS],
                                 cf_t                z[SRSLTE_PUCCH_MAX_SYMBOLS],
                                 bool                signal_only)
{
  if (!signal_only) {
    if (uci_mod_bits(q, sf, cfg, bits)) {
      ERROR("Error encoding PUCCH bits\n");
      return SRSLTE_ERROR;
    }
  } else {
    for (int i = 0; i < SRSLTE_PUCCH_MAX_BITS / 2; i++) {
      q->d[i] = 1.0;
    }
  }

  uint32_t N_sf_0 = get_N_sf(cfg->format, 0, sf->shortened);
  uint32_t N_sf_1 = get_N_sf(cfg->format, 1, sf->shortened);

  uint32_t n_oc_0 = cfg->n_pucch % N_sf_1;
  uint32_t n_oc_1 = (N_sf_1 == 5) ? ((3 * cfg->n_pucch) % N_sf_1) : (n_oc_0 % N_sf_1);

  cf_t* w_n_oc_0 = (cf_t*)pucch3_w_n_oc_5[n_oc_0];
  cf_t* w_n_oc_1 = (cf_t*)((N_sf_1 == 5) ? pucch3_w_n_oc_5[n_oc_1] : pucch3_w_n_oc_4[n_oc_1]);

  for (uint32_t n = 0; n < N_sf_0 + N_sf_1; n++) {
    uint32_t l         = get_pucch_symbol(n, cfg->format, q->cell.cp);
    uint32_t n_cs_cell = q->n_cs_cell[(2 * (sf->tti % 10) + ((n < N_sf_0) ? 0 : 1)) % SRSLTE_NSLOTS_X_FRAME][l];

    cf_t y_n[SRSLTE_NRE];
    bzero(y_n, sizeof(cf_t) * SRSLTE_NRE);

    cf_t h;
    if (n < N_sf_0) {
      h = w_n_oc_0[n] * cexpf(I * M_PI * floorf(n_cs_cell / 64.0f) / 2);
      for (uint32_t i = 0; i < SRSLTE_NRE; i++) {
        y_n[i] = h * q->d[(i + n_cs_cell) % SRSLTE_NRE];
      }
    } else {
      h = w_n_oc_1[n - N_sf_0] * cexpf(I * M_PI * floorf(n_cs_cell / 64.0f) / 2);
      for (uint32_t i = 0; i < SRSLTE_NRE; i++) {
        y_n[i] = h * q->d[((i + n_cs_cell) % SRSLTE_NRE) + SRSLTE_NRE];
      }
    }

    for (int k = 0; k < SRSLTE_NRE; k++) {
      cf_t acc = 0.0f;
      for (int i = 0; i < SRSLTE_NRE; i++) {
        acc += y_n[i] * cexpf(-I * 2.0 * M_PI * i * k / (float)SRSLTE_NRE);
      }
      z[n * SRSLTE_NRE + k] = acc / sqrtf(SRSLTE_NRE);
    }
  }

  return SRSLTE_SUCCESS;
}

static int decode_signal_format3(srslte_pucch_t*     q,
                                 srslte_ul_sf_cfg_t* sf,
                                 srslte_pucch_cfg_t* cfg,
                                 uint8_t             bits[SRSLTE_PUCCH_MAX_BITS],
                                 cf_t                z[SRSLTE_PUCCH_MAX_SYMBOLS])
{
  uint32_t N_sf_0 = get_N_sf(cfg->format, 0, sf->shortened);
  uint32_t N_sf_1 = get_N_sf(cfg->format, 1, sf->shortened);

  uint32_t n_oc_0 = cfg->n_pucch % N_sf_1;
  uint32_t n_oc_1 = (N_sf_1 == 5) ? ((3 * cfg->n_pucch) % N_sf_1) : (n_oc_0 % N_sf_1);

  cf_t* w_n_oc_0 = (cf_t*)pucch3_w_n_oc_5[n_oc_0];
  cf_t* w_n_oc_1 = (cf_t*)((N_sf_1 == 5) ? pucch3_w_n_oc_5[n_oc_1] : pucch3_w_n_oc_4[n_oc_1]);

  memset(q->d, 0, sizeof(cf_t) * 2 * SRSLTE_NRE);

  for (uint32_t n = 0; n < N_sf_0 + N_sf_1; n++) {
    uint32_t l         = get_pucch_symbol(n, cfg->format, q->cell.cp);
    uint32_t n_cs_cell = q->n_cs_cell[(2 * (sf->tti % 10) + ((n < N_sf_0) ? 0 : 1)) % SRSLTE_NSLOTS_X_FRAME][l];

    cf_t y_n[SRSLTE_NRE];
    bzero(y_n, sizeof(cf_t) * SRSLTE_NRE);

    // Do FFT
    for (int k = 0; k < SRSLTE_NRE; k++) {
      cf_t acc = 0.0f;
      for (int i = 0; i < SRSLTE_NRE; i++) {
        acc += z[n * SRSLTE_NRE + i] * cexpf(I * 2.0 * M_PI * i * k / (float)SRSLTE_NRE);
      }
      y_n[k] = acc / sqrtf(SRSLTE_NRE);
    }

    if (n < N_sf_0) {
      cf_t h = w_n_oc_0[n] * cexpf(-I * M_PI * floorf(n_cs_cell / 64.0f) / 2);
      for (uint32_t i = 0; i < SRSLTE_NRE; i++) {
        q->d[(i + n_cs_cell) % SRSLTE_NRE] += h * y_n[i];
      }
    } else {
      cf_t h = w_n_oc_1[n - N_sf_0] * cexpf(-I * M_PI * floorf(n_cs_cell / 64.0f) / 2);
      for (uint32_t i = 0; i < SRSLTE_NRE; i++) {
        q->d[((i + n_cs_cell) % SRSLTE_NRE) + SRSLTE_NRE] += h * y_n[i];
      }
    }
  }

  srslte_vec_sc_prod_cfc(q->d, 2.0f / (N_sf_0 + N_sf_1), q->d, SRSLTE_NRE * 2);

  srslte_sequence_t* seq = get_user_sequence(q, cfg->rnti, sf->tti % 10);
  if (seq) {
    srslte_demod_soft_demodulate_s(SRSLTE_MOD_QPSK, q->d, q->llr, SRSLTE_PUCCH3_NOF_BITS);

    srslte_scrambling_s_offset(seq, q->llr, 0, SRSLTE_PUCCH3_NOF_BITS);

    return (int)srslte_uci_decode_ack_sr_pucch3(q->llr, bits);
  } else {
    fprintf(stderr, "Error modulating PUCCH3 bits: rnti not set\n");
    return -1;
  }

  return SRSLTE_SUCCESS;
}

static int encode_signal(srslte_pucch_t*     q,
                         srslte_ul_sf_cfg_t* sf,
                         srslte_pucch_cfg_t* cfg,
                         uint8_t             bits[SRSLTE_PUCCH_MAX_BITS],
                         cf_t                z[SRSLTE_PUCCH_MAX_SYMBOLS])
{
  if (cfg->format == SRSLTE_PUCCH_FORMAT_3) {
    return encode_signal_format3(q, sf, cfg, bits, z, false);
  } else {
    return encode_signal_format12(q, sf, cfg, bits, z, false);
  }
}

// Encode bits from uci_data
static int encode_bits(srslte_pucch_cfg_t*   cfg,
                       srslte_uci_value_t*   uci_data,
                       srslte_pucch_format_t format,
                       uint8_t               pucch_bits[SRSLTE_PUCCH_MAX_BITS],
                       uint8_t               pucch2_bits[SRSLTE_PUCCH_MAX_BITS])
{
  if (format < SRSLTE_PUCCH_FORMAT_2) {
    memcpy(pucch_bits, uci_data->ack.ack_value, srslte_uci_cfg_total_ack(&cfg->uci_cfg) * sizeof(uint8_t));
  } else if (format >= SRSLTE_PUCCH_FORMAT_2 && format < SRSLTE_PUCCH_FORMAT_3) {
    /* Put RI (goes alone) */
    if (cfg->uci_cfg.cqi.ri_len) {
      uint8_t temp[2] = {uci_data->ri, 0};
      srslte_uci_encode_cqi_pucch(temp, cfg->uci_cfg.cqi.ri_len, pucch_bits);
    } else {
      /* Put CQI Report*/
      uint8_t buff[SRSLTE_CQI_MAX_BITS];
      int     uci_cqi_len = srslte_cqi_value_pack(&cfg->uci_cfg.cqi, &uci_data->cqi, buff);
      if (uci_cqi_len < 0) {
        ERROR("Error encoding CQI\n");
        return SRSLTE_ERROR;
      }
      srslte_uci_encode_cqi_pucch(buff, (uint32_t)uci_cqi_len, pucch_bits);
    }
    if (format > SRSLTE_PUCCH_FORMAT_2) {
      pucch2_bits[0] = uci_data->ack.ack_value[0];
      pucch2_bits[1] = uci_data->ack.ack_value[1]; // this will be ignored in format 2a
    }
  } else if (format == SRSLTE_PUCCH_FORMAT_3) {
    uint8_t  temp[SRSLTE_UCI_MAX_ACK_BITS + 1];
    uint32_t k = 0;
    for (; k < srslte_uci_cfg_total_ack(&cfg->uci_cfg); k++) {
      temp[k] = (uint8_t)((uci_data->ack.ack_value[k] == 1) ? 1 : 0);
    }
    if (cfg->uci_cfg.is_scheduling_request_tti) {
      temp[k] = (uint8_t)(uci_data->scheduling_request ? 1 : 0);
      k++;
    }
    srslte_uci_encode_ack_sr_pucch3(temp, k, pucch_bits);
    for (k = 32; k < SRSLTE_PUCCH3_NOF_BITS; k++) {
      pucch_bits[k] = pucch_bits[k % 32];
    }
  }
  return SRSLTE_SUCCESS;
}

static bool decode_signal(srslte_pucch_t*     q,
                          srslte_ul_sf_cfg_t* sf,
                          srslte_pucch_cfg_t* cfg,
                          uint8_t             pucch_bits[SRSLTE_CQI_MAX_BITS],
                          uint32_t            nof_re,
                          uint32_t            nof_uci_bits,
                          float*              correlation)
{
  int16_t llr_pucch2[SRSLTE_CQI_MAX_BITS];
  bool    detected = false;
  float   corr = 0, corr_max = -1e9;
  uint8_t b_max = 0, b2_max = 0; // default bit value, eg. HI is NACK

  srslte_sequence_t* seq;
  cf_t               ref[SRSLTE_PUCCH_MAX_SYMBOLS];

  switch (cfg->format) {
    case SRSLTE_PUCCH_FORMAT_1:
      encode_signal(q, sf, cfg, pucch_bits, q->z_tmp);
      corr = srslte_vec_corr_ccc(q->z, q->z_tmp, nof_re);
      if (corr >= cfg->threshold_format1) {
        detected = true;
      }
      DEBUG("format1 corr=%f, nof_re=%d, th=%f\n", corr, nof_re, cfg->threshold_format1);
      break;
    case SRSLTE_PUCCH_FORMAT_1A:
      detected = 0;
      for (uint8_t b = 0; b < 2; b++) {
        pucch_bits[0] = b;
        encode_signal(q, sf, cfg, pucch_bits, q->z_tmp);
        corr = srslte_vec_corr_ccc(q->z, q->z_tmp, nof_re);
        if (corr > corr_max) {
          corr_max = corr;
          b_max    = b;
        }
        if (corr_max > cfg->threshold_format1) { // check with format1 in case ack+sr because ack only is binary
          detected = true;
        }
        DEBUG("format1a b=%d, corr=%f, nof_re=%d\n", b, corr, nof_re);
      }
      corr          = corr_max;
      pucch_bits[0] = b_max;
      break;
    case SRSLTE_PUCCH_FORMAT_1B:
      detected = 0;
      for (uint8_t b = 0; b < 2; b++) {
        for (uint8_t b2 = 0; b2 < 2; b2++) {
          pucch_bits[0] = b;
          pucch_bits[1] = b2;
          encode_signal(q, sf, cfg, pucch_bits, q->z_tmp);
          corr = srslte_vec_corr_ccc(q->z, q->z_tmp, nof_re);
          if (corr > corr_max) {
            corr_max = corr;
            b_max    = b;
            b2_max   = b2;
          }
          if (corr_max > cfg->threshold_format1) { // check with format1 in case ack+sr because ack only is binary
            detected = true;
          }
          DEBUG("format1b b=%d, corr=%f, nof_re=%d\n", b, corr, nof_re);
        }
      }
      corr          = corr_max;
      pucch_bits[0] = b_max;
      pucch_bits[1] = b2_max;
      break;
    case SRSLTE_PUCCH_FORMAT_2:
    case SRSLTE_PUCCH_FORMAT_2A:
    case SRSLTE_PUCCH_FORMAT_2B:
      seq = get_user_sequence(q, cfg->rnti, sf->tti % 10);
      if (seq) {
        encode_signal_format12(q, sf, cfg, NULL, ref, true);
        srslte_vec_prod_conj_ccc(q->z, ref, q->z_tmp, SRSLTE_PUCCH_MAX_SYMBOLS);
        for (int i = 0; i < SRSLTE_PUCCH2_NOF_BITS / 2; i++) {
          q->z[i] = 0;
          for (int j = 0; j < SRSLTE_NRE; j++) {
            q->z[i] += q->z_tmp[i * SRSLTE_NRE + j] / SRSLTE_NRE;
          }
        }
        srslte_demod_soft_demodulate_s(SRSLTE_MOD_QPSK, q->z, llr_pucch2, SRSLTE_PUCCH2_NOF_BITS / 2);
        srslte_scrambling_s_offset(seq, llr_pucch2, 0, SRSLTE_PUCCH2_NOF_BITS);
        corr     = (float)srslte_uci_decode_cqi_pucch(&q->cqi, llr_pucch2, pucch_bits, nof_uci_bits) / 2000;
        detected = true;
      } else {
        ERROR("Decoding PUCCH2: could not generate sequence\n");
        return -1;
      }
      break;
    case SRSLTE_PUCCH_FORMAT_3:
      corr     = (float)decode_signal_format3(q, sf, cfg, pucch_bits, q->z) / 4800.0f;
      detected = true;
      break;
    default:
      ERROR("PUCCH format %d not implemented\n", cfg->format);
      return SRSLTE_ERROR;
  }
  if (correlation) {
    *correlation = corr;
  }
  return detected;
}

static void decode_bits(srslte_pucch_cfg_t* cfg,
                        bool                pucch_found,
                        uint8_t             pucch_bits[SRSLTE_PUCCH_MAX_BITS],
                        uint8_t             pucch2_bits[SRSLTE_PUCCH_MAX_BITS],
                        srslte_uci_value_t* uci_data)
{
  if (cfg->format == SRSLTE_PUCCH_FORMAT_3) {
    memcpy(uci_data->ack.ack_value, pucch_bits, srslte_uci_cfg_total_ack(&cfg->uci_cfg));
    uci_data->scheduling_request = pucch_bits[srslte_uci_cfg_total_ack(&cfg->uci_cfg)] == 1;
    uci_data->ack.valid          = true;
  } else {
    // If was looking for scheduling request, update value
    if (cfg->uci_cfg.is_scheduling_request_tti) {
      uci_data->scheduling_request = pucch_found;
    }

    // Save ACK bits
    for (uint32_t a = 0; a < srslte_pucch_nof_ack_format(cfg->format); a++) {
      if (cfg->uci_cfg.cqi.data_enable || cfg->uci_cfg.cqi.ri_len) {
        uci_data->ack.ack_value[a] = pucch2_bits[a];
      } else {
        uci_data->ack.ack_value[a] = pucch_bits[a];
      }
    }

    // PUCCH2 CQI bits are already decoded
    if (cfg->uci_cfg.cqi.data_enable) {
      srslte_cqi_value_unpack(&cfg->uci_cfg.cqi, pucch_bits, &uci_data->cqi);
    }

    if (cfg->uci_cfg.cqi.ri_len) {
      uci_data->ri = pucch_bits[0]; /* Assume only one bit of RI */
    }
  }
}

/* Encode, modulate and resource mapping of UCI data over PUCCH */
int srslte_pucch_encode(srslte_pucch_t*     q,
                        srslte_ul_sf_cfg_t* sf,
                        srslte_pucch_cfg_t* cfg,
                        srslte_uci_value_t* uci_data,
                        cf_t*               sf_symbols)
{
  uint8_t pucch_bits[SRSLTE_PUCCH_MAX_BITS];
  bzero(pucch_bits, SRSLTE_PUCCH_MAX_BITS * sizeof(uint8_t));

  int ret = SRSLTE_ERROR_INVALID_INPUTS;
  if (q != NULL && sf_symbols != NULL) {
    // Encode bits from UCI data for this format
    encode_bits(cfg, uci_data, cfg->format, pucch_bits, cfg->pucch2_drs_bits);

    if (encode_signal(q, sf, cfg, pucch_bits, q->z)) {
      return SRSLTE_ERROR;
    }

    if (pucch_put(q, sf, cfg, q->z, sf_symbols) < 0) {
      ERROR("Error putting PUCCH symbols\n");
      return SRSLTE_ERROR;
    }
    ret = SRSLTE_SUCCESS;
  }

  return ret;
}

/* Equalize, demodulate and decode PUCCH bits according to Section 5.4.1 of 36.211 */
int srslte_pucch_decode(srslte_pucch_t*        q,
                        srslte_ul_sf_cfg_t*    sf,
                        srslte_pucch_cfg_t*    cfg,
                        srslte_chest_ul_res_t* channel,
                        cf_t*                  sf_symbols,
                        srslte_pucch_res_t*    data)
{
  uint8_t pucch_bits[SRSLTE_CQI_MAX_BITS];
  bzero(pucch_bits, SRSLTE_CQI_MAX_BITS * sizeof(uint8_t));

  int ret = SRSLTE_ERROR_INVALID_INPUTS;

  if (q != NULL && cfg != NULL && channel != NULL && data != NULL) {

    uint32_t nof_cqi_bits = srslte_cqi_size(&cfg->uci_cfg.cqi);
    uint32_t nof_uci_bits = cfg->uci_cfg.cqi.ri_len ? cfg->uci_cfg.cqi.ri_len : nof_cqi_bits;

    int nof_re = pucch_get(q, sf, cfg, sf_symbols, q->z_tmp);
    if (nof_re < 0) {
      ERROR("Error getting PUCCH symbols\n");
      return SRSLTE_ERROR;
    }

    if (pucch_get(q, sf, cfg, channel->ce, q->ce) < 0) {
      ERROR("Error getting PUCCH symbols\n");
      return SRSLTE_ERROR;
    }

    // Equalization
    srslte_predecoding_single(q->z_tmp, q->ce, q->z, NULL, nof_re, 1.0f, channel->noise_estimate);

    // Perform ML-decoding
    bool pucch_found = decode_signal(q, sf, cfg, pucch_bits, nof_re, nof_uci_bits, &data->correlation);

    // Convert bits to UCI data
    decode_bits(cfg, pucch_found, pucch_bits, cfg->pucch2_drs_bits, &data->uci_data);

    data->detected = pucch_found;

    // Accept ACK and CQI only if correlation above threshold
    switch (cfg->format) {
      case SRSLTE_PUCCH_FORMAT_1A:
      case SRSLTE_PUCCH_FORMAT_1B:
        data->uci_data.ack.valid = data->correlation > cfg->threshold_data_valid_format1a;
        break;
      case SRSLTE_PUCCH_FORMAT_2:
      case SRSLTE_PUCCH_FORMAT_2A:
      case SRSLTE_PUCCH_FORMAT_2B:
        data->uci_data.ack.valid    = data->correlation > cfg->threshold_data_valid_format2;
        data->uci_data.cqi.data_crc = data->correlation > cfg->threshold_data_valid_format2;
        break;
      case SRSLTE_PUCCH_FORMAT_1:
      case SRSLTE_PUCCH_FORMAT_3:
      default:
        /* Not considered, do nothing */;
    }

    ret = SRSLTE_SUCCESS;
  }

  return ret;
}

char* srslte_pucch_format_text(srslte_pucch_format_t format)
{
  char* ret = NULL;

  switch (format) {

    case SRSLTE_PUCCH_FORMAT_1:
      ret = "Format 1";
      break;
    case SRSLTE_PUCCH_FORMAT_1A:
      ret = "Format 1A";
      break;
    case SRSLTE_PUCCH_FORMAT_1B:
      ret = "Format 1B";
      break;
    case SRSLTE_PUCCH_FORMAT_2:
      ret = "Format 2";
      break;
    case SRSLTE_PUCCH_FORMAT_2A:
      ret = "Format 2A";
      break;
    case SRSLTE_PUCCH_FORMAT_2B:
      ret = "Format 2B";
      break;
    case SRSLTE_PUCCH_FORMAT_3:
      ret = "Format 3";
      break;
    case SRSLTE_PUCCH_FORMAT_ERROR:
    default:
      ret = "Format Error";
  }

  return ret;
}

char* srslte_pucch_format_text_short(srslte_pucch_format_t format)
{
  char* ret = NULL;

  switch (format) {

    case SRSLTE_PUCCH_FORMAT_1:
      ret = "1";
      break;
    case SRSLTE_PUCCH_FORMAT_1A:
      ret = "1a";
      break;
    case SRSLTE_PUCCH_FORMAT_1B:
      ret = "1b";
      break;
    case SRSLTE_PUCCH_FORMAT_2:
      ret = "2";
      break;
    case SRSLTE_PUCCH_FORMAT_2A:
      ret = "2a";
      break;
    case SRSLTE_PUCCH_FORMAT_2B:
      ret = "2b";
      break;
    case SRSLTE_PUCCH_FORMAT_3:
      ret = "3";
      break;
    case SRSLTE_PUCCH_FORMAT_ERROR:
    default:
      ret = "Err";
      break;
  }

  return ret;
}

uint32_t srslte_pucch_nof_ack_format(srslte_pucch_format_t format)
{
  uint32_t ret = 0;

  switch (format) {

    case SRSLTE_PUCCH_FORMAT_1A:
    case SRSLTE_PUCCH_FORMAT_2A:
      ret = 1;
      break;
    case SRSLTE_PUCCH_FORMAT_1B:
    case SRSLTE_PUCCH_FORMAT_2B:
      ret = 2;
      break;
    default:
      // Keep default
      break;
  }

  return ret;
}

/* Verify PUCCH configuration as given in Section 5.4 36.211 */
bool srslte_pucch_cfg_isvalid(srslte_pucch_cfg_t* cfg, uint32_t nof_prb)
{
  if (cfg->delta_pucch_shift > 0 && cfg->delta_pucch_shift < 4 && cfg->N_cs < 8 &&
      (cfg->N_cs % cfg->delta_pucch_shift) == 0 && cfg->n_rb_2 <= nof_prb) {
    return true;
  } else {
    return false;
  }
}

uint32_t srslte_pucch_n_prb(srslte_cell_t* cell, srslte_pucch_cfg_t* cfg, uint32_t ns)
{
  uint32_t m = srslte_pucch_m(cfg, cell->cp);
  // Determine n_prb
  uint32_t n_prb = m / 2;
  if ((m + ns) % 2) {
    n_prb = cell->nof_prb - 1 - m / 2;
  }
  return n_prb;
}

// Compute m according to Section 5.4.3 of 36.211
uint32_t srslte_pucch_m(srslte_pucch_cfg_t* cfg, srslte_cp_t cp)
{
  uint32_t m = 0;
  switch (cfg->format) {
    case SRSLTE_PUCCH_FORMAT_1:
    case SRSLTE_PUCCH_FORMAT_1A:
    case SRSLTE_PUCCH_FORMAT_1B:
      m = cfg->n_rb_2;

      uint32_t c = SRSLTE_CP_ISNORM(cp) ? 3 : 2;
      if (cfg->n_pucch >= c * cfg->N_cs / cfg->delta_pucch_shift) {
        m = (cfg->n_pucch - c * cfg->N_cs / cfg->delta_pucch_shift) / (c * SRSLTE_NRE / cfg->delta_pucch_shift) +
            cfg->n_rb_2 + (uint32_t)ceilf((float)cfg->N_cs / 8);
      }
      break;
    case SRSLTE_PUCCH_FORMAT_2:
    case SRSLTE_PUCCH_FORMAT_2A:
    case SRSLTE_PUCCH_FORMAT_2B:
      m = cfg->n_pucch / SRSLTE_NRE;
      break;
    case SRSLTE_PUCCH_FORMAT_3:
      m = cfg->n_pucch / 5;
      break;
    default:
      m = 0;
      break;
  }
  return m;
}

/* Generates n_cs_cell according to Sec 5.4 of 36.211 */
int srslte_pucch_n_cs_cell(srslte_cell_t cell, uint32_t n_cs_cell[SRSLTE_NSLOTS_X_FRAME][SRSLTE_CP_NORM_NSYMB])
{
  srslte_sequence_t seq;
  bzero(&seq, sizeof(srslte_sequence_t));

  srslte_sequence_LTE_pr(&seq, 8 * SRSLTE_CP_NSYMB(cell.cp) * SRSLTE_NSLOTS_X_FRAME, cell.id);

  for (uint32_t ns = 0; ns < SRSLTE_NSLOTS_X_FRAME; ns++) {
    for (uint32_t l = 0; l < SRSLTE_CP_NSYMB(cell.cp); l++) {
      n_cs_cell[ns][l] = 0;
      for (uint32_t i = 0; i < 8; i++) {
        n_cs_cell[ns][l] += seq.c[8 * SRSLTE_CP_NSYMB(cell.cp) * ns + 8 * l + i] << i;
      }
    }
  }
  srslte_sequence_free(&seq);
  return SRSLTE_SUCCESS;
}

/* Calculates alpha for format 1/a/b according to 5.5.2.2.2 (is_dmrs=true) or 5.4.1 (is_dmrs=false) of 36.211 */
float srslte_pucch_alpha_format1(uint32_t            n_cs_cell[SRSLTE_NSLOTS_X_FRAME][SRSLTE_CP_NORM_NSYMB],
                                 srslte_pucch_cfg_t* cfg,
                                 srslte_cp_t         cp,
                                 bool                is_dmrs,
                                 uint32_t            ns,
                                 uint32_t            l,
                                 uint32_t*           n_oc_ptr,
                                 uint32_t*           n_prime_ns)
{
  uint32_t c       = SRSLTE_CP_ISNORM(cp) ? 3 : 2;
  uint32_t N_prime = (cfg->n_pucch < c * cfg->N_cs / cfg->delta_pucch_shift) ? cfg->N_cs : SRSLTE_NRE;

  uint32_t n_prime = cfg->n_pucch;
  if (cfg->n_pucch >= c * cfg->N_cs / cfg->delta_pucch_shift) {
    n_prime = (cfg->n_pucch - c * cfg->N_cs / cfg->delta_pucch_shift) % (c * SRSLTE_NRE / cfg->delta_pucch_shift);
  }
  if (ns % 2) {
    if (cfg->n_pucch >= c * cfg->N_cs / cfg->delta_pucch_shift) {
      n_prime = (c * (n_prime + 1)) % (c * SRSLTE_NRE / cfg->delta_pucch_shift + 1) - 1;
    } else {
      uint32_t d = SRSLTE_CP_ISNORM(cp) ? 2 : 0;
      uint32_t h = (n_prime + d) % (c * N_prime / cfg->delta_pucch_shift);
      n_prime    = (h / c) + (h % c) * N_prime / cfg->delta_pucch_shift;
    }
  }

  if (n_prime_ns) {
    *n_prime_ns = n_prime;
  }

  uint32_t n_oc_div = (!is_dmrs && SRSLTE_CP_ISEXT(cp)) ? 2 : 1;

  uint32_t n_oc = n_prime * cfg->delta_pucch_shift / N_prime;
  if (!is_dmrs && SRSLTE_CP_ISEXT(cp)) {
    n_oc *= 2;
  }
  if (n_oc_ptr) {
    *n_oc_ptr = n_oc;
  }
  uint32_t n_cs = 0;
  if (SRSLTE_CP_ISNORM(cp)) {
    n_cs = (n_cs_cell[ns][l] + (n_prime * cfg->delta_pucch_shift + (n_oc % cfg->delta_pucch_shift)) % N_prime) %
           SRSLTE_NRE;
  } else {
    n_cs = (n_cs_cell[ns][l] + (n_prime * cfg->delta_pucch_shift + n_oc / n_oc_div) % N_prime) % SRSLTE_NRE;
  }

  DEBUG("n_cs=%d, N_prime=%d, delta_pucch=%d, n_prime=%d, ns=%d, l=%d, ns_cs_cell=%d\n",
        n_cs,
        N_prime,
        cfg->delta_pucch_shift,
        n_prime,
        ns,
        l,
        n_cs_cell[ns][l]);

  return 2 * M_PI * (n_cs) / SRSLTE_NRE;
}

/* Calculates alpha for format 2/a/b according to 5.4.2 of 36.211 */
float srslte_pucch_alpha_format2(uint32_t            n_cs_cell[SRSLTE_NSLOTS_X_FRAME][SRSLTE_CP_NORM_NSYMB],
                                 srslte_pucch_cfg_t* cfg,
                                 uint32_t            ns,
                                 uint32_t            l)
{
  uint32_t n_prime = cfg->n_pucch % SRSLTE_NRE;
  if (cfg->n_pucch >= SRSLTE_NRE * cfg->n_rb_2) {
    n_prime = (cfg->n_pucch + cfg->N_cs + 1) % SRSLTE_NRE;
  }
  if (ns % 2) {
    n_prime = (SRSLTE_NRE * (n_prime + 1)) % (SRSLTE_NRE + 1) - 1;
    if (cfg->n_pucch >= SRSLTE_NRE * cfg->n_rb_2) {
      int x = (SRSLTE_NRE - 2 - (int)cfg->n_pucch) % SRSLTE_NRE;
      if (x >= 0) {
        n_prime = (uint32_t)x;
      } else {
        n_prime = SRSLTE_NRE + x;
      }
    }
  }
  uint32_t n_cs  = (n_cs_cell[ns][l] + n_prime) % SRSLTE_NRE;
  float    alpha = 2 * M_PI * (n_cs) / SRSLTE_NRE;
  DEBUG("n_pucch: %d, ns: %d, l: %d, n_prime: %d, n_cs: %d, alpha=%f\n", cfg->n_pucch, ns, l, n_prime, n_cs, alpha);
  return alpha;
}

/* Modulates bit 20 and 21 for Formats 2a and 2b as in Table 5.4.2-1 in 36.211 */
int srslte_pucch_format2ab_mod_bits(srslte_pucch_format_t format, uint8_t bits[2], cf_t* d_10)
{
  if (d_10) {
    if (format == SRSLTE_PUCCH_FORMAT_2A) {
      *d_10 = bits[0] ? -1.0 : 1.0;
      return SRSLTE_SUCCESS;
    } else if (format == SRSLTE_PUCCH_FORMAT_2B) {
      if (bits[0] == 0) {
        if (bits[1] == 0) {
          *d_10 = 1.0;
        } else {
          *d_10 = -I;
        }
      } else {
        if (bits[1] == 0) {
          *d_10 = I;
        } else {
          *d_10 = -1.0;
        }
      }
      return SRSLTE_SUCCESS;
    } else {
      return SRSLTE_ERROR;
    }
  } else {
    return SRSLTE_ERROR;
  }
}

void srslte_pucch_tx_info(srslte_pucch_cfg_t* cfg, srslte_uci_value_t* uci_data, char* str, uint32_t str_len)
{
  uint32_t n = srslte_print_check(str,
                                  str_len,
                                  0,
                                  "rnti=0x%x, f=%s, n_pucch=%d",
                                  cfg->rnti,
                                  srslte_pucch_format_text_short(cfg->format),
                                  cfg->n_pucch);

  if (uci_data) {
    srslte_uci_data_info(&cfg->uci_cfg, uci_data, &str[n], str_len - n);
  }
}

void srslte_pucch_rx_info(srslte_pucch_cfg_t* cfg, srslte_uci_value_t* uci_data, char* str, uint32_t str_len)
{
  uint32_t n = srslte_print_check(str,
                                  str_len,
                                  0,
                                  "rnti=0x%x, f=%s, n_pucch=%d",
                                  cfg->rnti,
                                  srslte_pucch_format_text_short(cfg->format),
                                  cfg->n_pucch);

  if (uci_data) {
    srslte_uci_data_info(&cfg->uci_cfg, uci_data, &str[n], str_len - n);
  }
}

srslte_pucch_format_t srslte_pucch_select_format(srslte_pucch_cfg_t* cfg, srslte_uci_cfg_t* uci_cfg, srslte_cp_t cp)
{
  srslte_pucch_format_t format = SRSLTE_PUCCH_FORMAT_ERROR;
  // No CQI data
  if (!uci_cfg->cqi.data_enable && uci_cfg->cqi.ri_len == 0) {
    // PUCCH Format 3 condition specified in:
    // 3GPP 36.213 10.1.2.2.2 PUCCH format 3 HARQ-ACK procedure
    if (cfg->ack_nack_feedback_mode == SRSLTE_PUCCH_ACK_NACK_FEEDBACK_MODE_PUCCH3 &&
        srslte_uci_cfg_total_ack(uci_cfg) > 1) {
      format = SRSLTE_PUCCH_FORMAT_3;
    }
    // 1-bit ACK + optional SR
    else if (srslte_uci_cfg_total_ack(uci_cfg) == 1) {
      format = SRSLTE_PUCCH_FORMAT_1A;
    }
    // 2-bit ACK + optional SR
    else if (srslte_uci_cfg_total_ack(uci_cfg) >= 2 && srslte_uci_cfg_total_ack(uci_cfg) <= 4) {
      format = SRSLTE_PUCCH_FORMAT_1B; // with channel selection if > 2
    }
    // If UCI value is provided, use SR signal only, otherwise SR request opportunity
    else if (uci_cfg->is_scheduling_request_tti) {
      format = SRSLTE_PUCCH_FORMAT_1;
    } else {
      ERROR("Error selecting PUCCH format: Unsupported number of ACK bits %d\n", srslte_uci_cfg_total_ack(uci_cfg));
    }
  }
  // CQI data
  else {
    // CQI and no ack
    if (srslte_uci_cfg_total_ack(uci_cfg) == 0) {
      format = SRSLTE_PUCCH_FORMAT_2;
    }
    // CQI + 1-bit ACK
    else if (srslte_uci_cfg_total_ack(uci_cfg) == 1 && SRSLTE_CP_ISNORM(cp)) {
      format = SRSLTE_PUCCH_FORMAT_2A;
    }
    // CQI + 2-bit ACK
    else if (srslte_uci_cfg_total_ack(uci_cfg) == 2) {
      format = SRSLTE_PUCCH_FORMAT_2B;
    }
    // CQI + 2-bit ACK + cyclic prefix
    else if (srslte_uci_cfg_total_ack(uci_cfg) == 1 && SRSLTE_CP_ISEXT(cp)) {
      format = SRSLTE_PUCCH_FORMAT_2B;
    }
  }
  return format;
}

int srslte_pucch_cs_resources(srslte_pucch_cfg_t* cfg, srslte_uci_cfg_t* uci_cfg, uint32_t n_pucch_i[4])
{
  int ret = SRSLTE_ERROR_INVALID_INPUTS;

  if (cfg && uci_cfg && n_pucch_i) {
    // Determine the 4 PUCCH resources n_pucch_j associated with HARQ-ACK(j)
    uint32_t k = 0;
    for (int i = 0; i < SRSLTE_MAX_CARRIERS && k < SRSLTE_PUCCH_CS_MAX_ACK; i++) {
      // If grant has been scheduled in PCell
      if (uci_cfg->ack[i].grant_cc_idx == 0) {
        for (uint32_t j = 0; j < uci_cfg->ack[i].nof_acks && k < SRSLTE_PUCCH_CS_MAX_ACK; j++) {
          if (k % 2 == 0) {
            n_pucch_i[k] = cfg->n1_pucch_an_cs[uci_cfg->ack[i].tpc_for_pucch][k / 2];
          } else {
            n_pucch_i[k] = uci_cfg->ack[i].ncce[0] + cfg->N_pucch_1 + 1;
          }
          k++;
        }
      } else {
        for (uint32_t j = 0; j < uci_cfg->ack[i].nof_acks; j++) {
          if (k < 4) {
            n_pucch_i[k++] = cfg->n1_pucch_an_cs[uci_cfg->ack[i].tpc_for_pucch % SRSLTE_PUCCH_SIZE_AN_CS]
                                                [j % SRSLTE_PUCCH_NOF_AN_CS];
          } else {
            fprintf(stderr, "get_npucch_cs(): Too many ack bits\n");
            return SRSLTE_ERROR;
          }
        }
      }
    }

    ret = (int)k;
  }

  return ret;
}

#define PUCCH_CS_SET_ACK(J, B0, B1, ...)                                                                               \
  do {                                                                                                                 \
    if (j == J && b[0] == B0 && b[1] == B1) {                                                                          \
      uint8_t pos[] = {__VA_ARGS__};                                                                                   \
      for (uint32_t i = 0; i < sizeof(pos) && pos[i] < SRSLTE_PUCCH_CS_MAX_ACK; i++) {                                 \
        uci_value[pos[i]] = 1;                                                                                         \
      }                                                                                                                \
      ret = SRSLTE_SUCCESS;                                                                                            \
    }                                                                                                                  \
  } while (false)

static int puccch_cs_get_ack_a2(uint32_t j, const uint8_t b[2], uint8_t uci_value[SRSLTE_UCI_MAX_ACK_BITS])
{
  int ret = SRSLTE_ERROR;

  PUCCH_CS_SET_ACK(1, 1, 1, 0, 1);
  PUCCH_CS_SET_ACK(0, 1, 1, 0);
  PUCCH_CS_SET_ACK(1, 0, 0, 1);
  PUCCH_CS_SET_ACK(1, 0, 0, SRSLTE_PUCCH_CS_MAX_ACK);

  return ret;
}

static int puccch_cs_get_ack_a3(uint32_t j, const uint8_t b[2], uint8_t uci_value[SRSLTE_UCI_MAX_ACK_BITS])
{
  int ret = SRSLTE_ERROR;

  PUCCH_CS_SET_ACK(1, 1, 1, 0, 1, 2);
  PUCCH_CS_SET_ACK(1, 1, 0, 0, 2);
  PUCCH_CS_SET_ACK(1, 0, 1, 1, 2);
  PUCCH_CS_SET_ACK(2, 1, 1, 2);
  PUCCH_CS_SET_ACK(0, 1, 1, 0, 1);
  PUCCH_CS_SET_ACK(0, 1, 0, 0);
  PUCCH_CS_SET_ACK(0, 0, 1, 1);
  PUCCH_CS_SET_ACK(1, 0, 0, SRSLTE_PUCCH_CS_MAX_ACK);

  return ret;
}

static int puccch_cs_get_ack_a4(uint32_t j, const uint8_t b[2], uint8_t uci_value[SRSLTE_UCI_MAX_ACK_BITS])
{
  int ret = SRSLTE_ERROR;

  PUCCH_CS_SET_ACK(1, 1, 1, 0, 1, 2, 3);
  PUCCH_CS_SET_ACK(2, 0, 1, 0, 2, 3);
  PUCCH_CS_SET_ACK(1, 0, 1, 1, 2, 3);
  PUCCH_CS_SET_ACK(3, 1, 1, 2, 3);
  PUCCH_CS_SET_ACK(1, 1, 0, 0, 1, 2);
  PUCCH_CS_SET_ACK(2, 0, 0, 0, 2);
  PUCCH_CS_SET_ACK(1, 0, 0, 1, 2);
  PUCCH_CS_SET_ACK(3, 1, 0, 2);
  PUCCH_CS_SET_ACK(2, 1, 1, 0, 1, 3);
  PUCCH_CS_SET_ACK(2, 1, 0, 0, 3);
  PUCCH_CS_SET_ACK(3, 0, 1, 1, 3);
  PUCCH_CS_SET_ACK(3, 0, 0, 3);
  PUCCH_CS_SET_ACK(0, 1, 1, 0, 1);
  PUCCH_CS_SET_ACK(0, 1, 0, 0);
  PUCCH_CS_SET_ACK(0, 0, 1, 1);
  PUCCH_CS_SET_ACK(0, 0, 0, SRSLTE_PUCCH_CS_MAX_ACK);

  return ret;
}

int srslte_pucch_cs_get_ack(srslte_pucch_cfg_t* cfg,
                            srslte_uci_cfg_t*   uci_cfg,
                            uint32_t            j,
                            uint8_t             b[2],
                            srslte_uci_value_t* uci_value)
{
  int ret = SRSLTE_ERROR_INVALID_INPUTS;

  if (cfg && uci_cfg && uci_value) {
    // Set bits to 0 by default
    memset(uci_value->ack.ack_value, 0, SRSLTE_UCI_MAX_ACK_BITS);
    uci_value->ack.valid = true;

    uint32_t nof_ack = srslte_uci_cfg_total_ack(uci_cfg);
    switch (nof_ack) {
      case 2:
        // A = 2
        ret = puccch_cs_get_ack_a2(j, b, uci_value->ack.ack_value);
        break;
      case 3:
        // A = 3
        ret = puccch_cs_get_ack_a3(j, b, uci_value->ack.ack_value);
        break;
      case 4:
        // A = 4
        ret = puccch_cs_get_ack_a4(j, b, uci_value->ack.ack_value);
        break;
      default:
        // Unhandled case
        ERROR("Unexpected number of ACK (%d)\n", nof_ack);
        ret = SRSLTE_ERROR;
    }
  }

  return ret;
}

int srslte_pucch_pucch3_resources(const srslte_pucch_cfg_t* cfg, const srslte_uci_cfg_t* uci_cfg, uint32_t n_pucch_i[4])
{
  int ret = SRSLTE_ERROR_INVALID_INPUTS;

  if (cfg && uci_cfg && n_pucch_i) {
    n_pucch_i[0] = cfg->n3_pucch_an_list[uci_cfg->ack[0].tpc_for_pucch % SRSLTE_PUCCH_SIZE_AN_CS];
    ret          = 1;
  }

  return ret;
}
