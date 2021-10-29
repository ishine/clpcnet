/* Copyright (c) 2017-2019 Mozilla */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "celt_lpc.h"
#include "common.h"
#include "freq.h"
#include "kiss_fft.h"
#include "lpcnet.h"
#include "lpcnet_private.h"
#include "pitch.h"

//#define NB_FEATURES (2*NB_BANDS+3+LPC_ORDER)

int interp_search(const float *x, const float *left, const float *right, float *dist_out)
{
  int i, k;
  float min_dist = 1e15;
  int best_pred = 0;
  float pred[4 * NB_BANDS];
  for (i = 0; i < NB_BANDS; i++)
    pred[i] = pred[NB_BANDS + i] = .5 * (left[i] + right[i]);
  for (i = 0; i < NB_BANDS; i++)
    pred[2 * NB_BANDS + i] = left[i];
  for (i = 0; i < NB_BANDS; i++)
    pred[3 * NB_BANDS + i] = right[i];

  for (k = 1; k < 4; k++)
  {
    float dist = 0;
    for (i = 0; i < NB_BANDS; i++)
      dist += (x[i] - pred[k * NB_BANDS + i]) * (x[i] - pred[k * NB_BANDS + i]);
    dist_out[k - 1] = dist;
    if (dist < min_dist)
    {
      min_dist = dist;
      best_pred = k;
    }
  }
  return best_pred - 1;
}

int double_interp_search(float features[4][NB_TOTAL_FEATURES], const float *mem)
{
  int i, j;
  int best_id = 0;
  float min_dist = 1e15;
  float dist[2][3];
  interp_search(features[0], mem, features[1], dist[0]);
  interp_search(features[2], features[1], features[3], dist[1]);
  for (i = 0; i < 3; i++)
  {
    for (j = 0; j < 3; j++)
    {
      float d;
      int id;
      id = 3 * i + j;
      d = dist[0][i] + dist[1][j];
      if (d < min_dist && id != FORBIDDEN_INTERP)
      {
        min_dist = d;
        best_id = id;
      }
    }
  }
  return best_id - (best_id >= FORBIDDEN_INTERP);
}


int lpcnet_encoder_get_size() {
  return sizeof(LPCNetEncState);
}

int lpcnet_encoder_init(LPCNetEncState *st) {
  memset(st, 0, sizeof(*st));
  return 0;
}

LPCNetEncState *lpcnet_encoder_create() {
  LPCNetEncState *st;
  st = malloc(lpcnet_encoder_get_size());
  lpcnet_encoder_init(st);
  return st;
}

static void frame_analysis(LPCNetEncState *st, kiss_fft_cpx *X, float *Ex, const float *in) {
  float x[WINDOW_SIZE];
  RNN_COPY(x, st->analysis_mem, OVERLAP_SIZE);
  RNN_COPY(&x[OVERLAP_SIZE], in, FRAME_SIZE);
  RNN_COPY(st->analysis_mem, &in[FRAME_SIZE - OVERLAP_SIZE], OVERLAP_SIZE);
  apply_window(x);
  forward_transform(X, x);
  compute_band_energy(Ex, X);
}

void compute_frame_features(LPCNetEncState *st, const float *in) {
  float aligned_in[FRAME_SIZE];
  int i;
  float E = 0;
  float Ly[NB_BANDS];
  float follow, logMax;
  float g;
  kiss_fft_cpx X[FREQ_SIZE];
  float Ex[NB_BANDS];
  float xcorr[PITCH_MAX_PERIOD];
  float ener0;
  int sub;
  float ener;
  RNN_COPY(aligned_in, &st->analysis_mem[OVERLAP_SIZE - TRAINING_OFFSET], TRAINING_OFFSET);

  // Compute bark-scale cepstrum
  frame_analysis(st, X, Ex, in);
  logMax = -2;
  follow = -2;
  for (i = 0; i < NB_BANDS; i++)
  {
    Ly[i] = log10(1e-2 + Ex[i]);
    Ly[i] = MAX16(logMax - 8, MAX16(follow - 2.5, Ly[i]));
    logMax = MAX16(logMax, Ly[i]);
    follow = MAX16(follow - 2.5, Ly[i]);
    E += Ex[i];
  }

  // Compute coefficients from bark-scale cepstrum
  dct(st->features[st->pcount], Ly);
  st->features[st->pcount][0] -= 4;

  // Compute lpcs from cepstral coefficients
  g = lpc_from_cepstrum(st->lpc, st->features[st->pcount]);

  // Store lpcs in features
  st->features[st->pcount][2 * NB_BANDS + 2] = log10(g);
  for (i = 0; i < LPC_ORDER; i++)
    st->features[st->pcount][2 * NB_BANDS + 3 + i] = st->lpc[i];

  // Move excitation by one frame
  RNN_MOVE(st->exc_buf, &st->exc_buf[FRAME_SIZE], PITCH_MAX_PERIOD);

  // Perform yin pitch-tracking
  RNN_COPY(&aligned_in[TRAINING_OFFSET], in, FRAME_SIZE - TRAINING_OFFSET);
  for (i = 0; i < FRAME_SIZE; i++)
  {
    int j;
    float sum = aligned_in[i];
    for (j = 0; j < LPC_ORDER; j++)
      sum += st->lpc[j] * st->pitch_mem[j];
    RNN_MOVE(st->pitch_mem + 1, st->pitch_mem, LPC_ORDER - 1);
    st->pitch_mem[0] = aligned_in[i];
    st->exc_buf[PITCH_MAX_PERIOD + i] = sum + .7 * st->pitch_filt;
    st->pitch_filt = sum;
  }
  /* Cross-correlation on half-frames. */
  for (sub = 0; sub < 2; sub++)
  {
    int off = sub * FRAME_SIZE / 2;
    celt_pitch_xcorr(&st->exc_buf[PITCH_MAX_PERIOD + off], st->exc_buf + off, xcorr, FRAME_SIZE / 2, PITCH_MAX_PERIOD);
    ener0 = celt_inner_prod(&st->exc_buf[PITCH_MAX_PERIOD + off], &st->exc_buf[PITCH_MAX_PERIOD + off], FRAME_SIZE / 2);
    st->frame_weight[2 + 2 * st->pcount + sub] = ener0;
    for (i = 0; i < PITCH_MAX_PERIOD; i++)
    {
      ener = (1 + ener0 + celt_inner_prod(&st->exc_buf[i + off], &st->exc_buf[i + off], FRAME_SIZE / 2));
      st->xc[2 + 2 * st->pcount + sub][i] = 2 * xcorr[i] / ener;
    }
  }
}

void process_superframe(LPCNetEncState *st, FILE *ffeat) {
  int i;
  int sub;
  int best_i;
  int best[10];
  int pitch_prev[8][PITCH_MAX_PERIOD];
  float best_a = 0;
  float best_b = 0;
  float w;
  float sx = 0, sxx = 0, sxy = 0, sy = 0, sw = 0;
  float frame_corr;
  int voiced;
  float frame_weight_sum = 1e-15;
  float center_pitch;
  int main_pitch;
  int modulation;
  for (sub = 0; sub < 8; sub++)
    frame_weight_sum += st->frame_weight[2 + sub];
  for (sub = 0; sub < 8; sub++)
    st->frame_weight[2 + sub] *= (8.f / frame_weight_sum);
  for (sub = 0; sub < 8; sub++)
  {
    float max_path_all = -1e15;
    best_i = 0;
    for (i = 0; i < PITCH_MAX_PERIOD - 2 * PITCH_MIN_PERIOD; i++)
    {
      float xc_half = MAX16(MAX16(st->xc[2 + sub][(PITCH_MAX_PERIOD + i) / 2], st->xc[2 + sub][(PITCH_MAX_PERIOD + i + 2) / 2]), st->xc[2 + sub][(PITCH_MAX_PERIOD + i - 1) / 2]);
      if (st->xc[2 + sub][i] < xc_half * 1.1)
        st->xc[2 + sub][i] *= .8;
    }
    for (i = 0; i < PITCH_MAX_PERIOD - PITCH_MIN_PERIOD; i++)
    {
      int j;
      float max_prev;
      max_prev = st->pitch_max_path_all - 6.f;
      pitch_prev[sub][i] = st->best_i;
      for (j = IMIN(0, 4 - i); j <= 4 && i + j < PITCH_MAX_PERIOD - PITCH_MIN_PERIOD; j++)
      {
        if (st->pitch_max_path[0][i + j] > max_prev)
        {
          max_prev = st->pitch_max_path[0][i + j] - .02f * abs(j) * abs(j);
          pitch_prev[sub][i] = i + j;
        }
      }
      st->pitch_max_path[1][i] = max_prev + st->frame_weight[2 + sub] * st->xc[2 + sub][i];
      if (st->pitch_max_path[1][i] > max_path_all)
      {
        max_path_all = st->pitch_max_path[1][i];
        best_i = i;
      }
    }
    /* Renormalize. */
    for (i = 0; i < PITCH_MAX_PERIOD - PITCH_MIN_PERIOD; i++)
      st->pitch_max_path[1][i] -= max_path_all;
    RNN_COPY(&st->pitch_max_path[0][0], &st->pitch_max_path[1][0], PITCH_MAX_PERIOD);
    st->pitch_max_path_all = max_path_all;
    st->best_i = best_i;
  }
  best_i = st->best_i;
  frame_corr = 0;

  /* Backward pass. */
  for (sub = 7; sub >= 0; sub--)
  {
    best[2 + sub] = PITCH_MAX_PERIOD - best_i;
    frame_corr += st->frame_weight[2 + sub] * st->xc[2 + sub][best_i];
    best_i = pitch_prev[sub][best_i];
  }

  frame_corr /= 8;

  for (sub = 2; sub < 10; sub++)
  {
    w = st->frame_weight[sub];
    sw += w;
    sx += w * sub;
    sxx += w * sub * sub;
    sxy += w * sub * best[sub];
    sy += w * best[sub];
  }
  voiced = frame_corr >= .3;

  /* Linear regression to figure out the pitch contour. */
  best_a = (sw * sxy - sx * sy) / (sw * sxx - sx * sx);
  if (voiced)
  {
    float max_a;
    float mean_pitch = sy / sw;

    /* Allow a relative variation of up to 1/4 over 8 sub-frames. */
    max_a = mean_pitch / 32;
    best_a = MIN16(max_a, MAX16(-max_a, best_a));
  }
  else
  {
    best_a = 0;
  }

  best_b = (sy - best_a * sx) / sw;

  /* Quantizing the pitch as "main" pitch + slope. */
  center_pitch = best_b + 5.5 * best_a;
  main_pitch = (int)floor(.5 + 21. * log2(center_pitch / PITCH_MIN_PERIOD));
  main_pitch = IMAX(0, IMIN(63, main_pitch));
  modulation = (int)floor(.5 + 16 * 7 * best_a / center_pitch);
  modulation = IMAX(-3, IMIN(3, modulation));

  for (sub = 0; sub < 4; sub++)
  {
    st->features[sub][2 * NB_BANDS] = .01 * (best[2 + 2 * sub] + best[2 + 2 * sub + 1] - 200);
    st->features[sub][2 * NB_BANDS + 1] = frame_corr - .5;
  }
  RNN_COPY(&st->xc[0][0], &st->xc[8][0], PITCH_MAX_PERIOD);
  RNN_COPY(&st->xc[1][0], &st->xc[9][0], PITCH_MAX_PERIOD);
  for (sub = 0; sub < 4; sub++)
  {
    float g = lpc_from_cepstrum(st->lpc, st->features[sub]);
    st->features[sub][2 * NB_BANDS + 2] = log10(g);
    for (i = 0; i < LPC_ORDER; i++)
      st->features[sub][2 * NB_BANDS + 3 + i] = st->lpc[i];
  }
  if (ffeat)
  {
    for (i = 0; i < 4; i++)
    {
      fwrite(st->features[i], sizeof(float), NB_TOTAL_FEATURES, ffeat);
    }
  }
}
