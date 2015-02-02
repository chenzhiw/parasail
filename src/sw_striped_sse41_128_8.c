/**
 * @file
 *
 * @author jeff.daily@pnnl.gov
 *
 * Copyright (c) 2014 Battelle Memorial Institute.
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#include "config.h"

#include <stdint.h>
#include <stdlib.h>

#include <emmintrin.h>
#include <smmintrin.h>

#include "parasail.h"
#include "parasail_internal.h"
#include "parasail_internal_sse.h"
#include "blosum/blosum_map.h"

#define NEG_INF_8 (INT8_MIN)

#ifdef PARASAIL_TABLE
static inline void arr_store_si128(
        int *array,
        __m128i vH,
        int32_t t,
        int32_t seglen,
        int32_t d,
        int32_t dlen,
        int32_t bias)
{
    array[( 0*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH,  0) - bias;
    array[( 1*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH,  1) - bias;
    array[( 2*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH,  2) - bias;
    array[( 3*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH,  3) - bias;
    array[( 4*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH,  4) - bias;
    array[( 5*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH,  5) - bias;
    array[( 6*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH,  6) - bias;
    array[( 7*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH,  7) - bias;
    array[( 8*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH,  8) - bias;
    array[( 9*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH,  9) - bias;
    array[(10*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH, 10) - bias;
    array[(11*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH, 11) - bias;
    array[(12*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH, 12) - bias;
    array[(13*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH, 13) - bias;
    array[(14*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH, 14) - bias;
    array[(15*seglen+t)*dlen + d] = (int8_t)_mm_extract_epi8(vH, 15) - bias;
}
#endif

#ifdef PARASAIL_TABLE
#define FNAME sw_table_striped_sse41_128_8
#else
#define FNAME sw_striped_sse41_128_8
#endif

parasail_result_t* FNAME(
        const char * const restrict s1, const int s1Len,
        const char * const restrict s2, const int s2Len,
        const int open, const int gap, const int matrix[24][24])
{
    int32_t i = 0;
    int32_t j = 0;
    int32_t k = 0;
    int32_t segNum = 0;
    const int32_t n = 24; /* number of amino acids in table */
    const int32_t segWidth = 16; /* number of values in vector unit */
    const int32_t segLen = (s1Len + segWidth - 1) / segWidth;
    __m128i* const restrict vProfile = parasail_memalign_m128i(16, n * segLen);
    __m128i* restrict pvHStore = parasail_memalign_m128i(16, segLen);
    __m128i* restrict pvHLoad =  parasail_memalign_m128i(16, segLen);
    __m128i* const restrict pvE = parasail_memalign_m128i(16, segLen);
    int score = NEG_INF_8;
    __m128i vGapO = _mm_set1_epi8(open);
    __m128i vGapE = _mm_set1_epi8(gap);
    __m128i vNegInf = _mm_set1_epi8(INT8_MIN);
    int8_t bias = INT8_MIN;
    __m128i vMaxH = vNegInf;
#ifdef PARASAIL_TABLE
    parasail_result_t *result = parasail_result_new_table1(segLen*segWidth, s2Len);
#else
    parasail_result_t *result = parasail_result_new();
#endif

    /* Generate query profile.
     * Rearrange query sequence & calculate the weight of match/mismatch.
     * Don't alias. */
    {
        int32_t index = 0;
        for (k=0; k<n; ++k) {
            for (i=0; i<segLen; ++i) {
                __m128i_8_t t;
                j = i;
                for (segNum=0; segNum<segWidth; ++segNum) {
                    t.v[segNum] = j >= s1Len ? 0 : matrix[k][MAP_BLOSUM_[(unsigned char)s1[j]]];
                    j += segLen;
                }
                _mm_store_si128(&vProfile[index], t.m);
                ++index;
            }
        }
    }

    /* initialize H and E */
    {
        int32_t index = 0;
        for (i=0; i<segLen; ++i) {
            __m128i_8_t h;
            __m128i_8_t e;
            for (segNum=0; segNum<segWidth; ++segNum) {
                h.v[segNum] = bias;
                e.v[segNum] = NEG_INF_8;
            }
            _mm_store_si128(&pvHStore[index], h.m);
            _mm_store_si128(&pvE[index], e.m);
            ++index;
        }
    }

    /* outer loop over database sequence */
    for (j=0; j<s2Len; ++j) {
        __m128i vE;
        /* Initialize F value to 0.  Any errors to vH values will be corrected
         * in the Lazy_F loop.  */
        __m128i vF = vNegInf;

        /* load final segment of pvHStore and shift left by 2 bytes */
        __m128i vH = _mm_slli_si128(pvHStore[segLen - 1], 1);
        vH = _mm_insert_epi8(vH, bias, 0);

        /* Correct part of the vProfile */
        const __m128i* vP = vProfile + MAP_BLOSUM_[(unsigned char)s2[j]] * segLen;

        /* Swap the 2 H buffers. */
        __m128i* pv = pvHLoad;
        pvHLoad = pvHStore;
        pvHStore = pv;

        /* inner loop to process the query sequence */
        for (i=0; i<segLen; ++i) {
            vH = _mm_adds_epi8(vH, _mm_load_si128(vP + i));
            vE = _mm_load_si128(pvE + i);

            /* Get max from vH, vE and vF. */
            vH = _mm_max_epi8(vH, vE);
            vH = _mm_max_epi8(vH, vF);
            /*vH = _mm_max_epi8(vH, vZero);*/
            /* Save vH values. */
            _mm_store_si128(pvHStore + i, vH);
#ifdef PARASAIL_TABLE
            arr_store_si128(result->score_table, vH, i, segLen, j, s2Len, bias);
#endif

            /* update max vector seen so far */
            {
                vMaxH = _mm_max_epi8(vH, vMaxH);
            }

            /* Update vE value. */
            vH = _mm_subs_epi8(vH, vGapO);
            vE = _mm_subs_epi8(vE, vGapE);
            vE = _mm_max_epi8(vE, vH);
            _mm_store_si128(pvE + i, vE);

            /* Update vF value. */
            vF = _mm_subs_epi8(vF, vGapE);
            vF = _mm_max_epi8(vF, vH);

            /* Load the next vH. */
            vH = _mm_load_si128(pvHLoad + i);
        }

        /* Lazy_F loop: has been revised to disallow adjecent insertion and
         * then deletion, so don't update E(i, i), learn from SWPS3 */
        for (k=0; k<segWidth; ++k) {
            vF = _mm_slli_si128(vF, 1);
            vF = _mm_insert_epi8(vF, bias, 0);
            for (i=0; i<segLen; ++i) {
                vH = _mm_load_si128(pvHStore + i);
                vH = _mm_max_epi8(vH,vF);
                _mm_store_si128(pvHStore + i, vH);
#ifdef PARASAIL_TABLE
                arr_store_si128(result->score_table, vH, i, segLen, j, s2Len, bias);
#endif
                vH = _mm_subs_epi8(vH, vGapO);
                vF = _mm_subs_epi8(vF, vGapE);
                if (! _mm_movemask_epi8(_mm_cmpgt_epi8(vF, vH))) goto end;
                /*vF = _mm_max_epi8(vF, vH);*/
            }
        }
end:
        {
        }
    }

    /* max in vec */
    for (j=0; j<segWidth; ++j) {
        int value = (int8_t) _mm_extract_epi8(vMaxH, 15) - (int)bias;
        if (value > score) {
            score = value;
        }
        vMaxH = _mm_slli_si128(vMaxH, 1);
    }

    /* check for saturation */
    if (score == INT8_MAX) {
        result->saturated = 1;
    }

    result->score = score;

    parasail_free(pvE);
    parasail_free(pvHLoad);
    parasail_free(pvHStore);
    parasail_free(vProfile);

    return result;
}

