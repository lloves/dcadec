/*
 * This file is part of libdcadec.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "common.h"
#include "bitstream.h"
#include "fixed_math.h"
#include "xll_decoder.h"
#include "exss_parser.h"
#include "dmix_tables.h"

#include "xll_tables.h"

#define XLL_PBR_SIZE    (240 << 10)

static int parse_dmix_coeffs(struct xll_chset *chs)
{
    struct xll_decoder *xll = chs->decoder;
    int m, n;

    if (chs->primary_chset) {
        m = dmix_primary_nch[chs->dmix_type];
        n = chs->nchannels;
    } else {
        m = chs->dmix_m;
        n = chs->nchannels + 2; // Two extra columns for scales
    }

    // Reallocate downmix coefficients buffer
    if (ta_zalloc_fast(xll->chset, &chs->dmix_coeff, m * n * 2, sizeof(int)) < 0)
        return -DCADEC_ENOMEM;

    // Setup buffer pointers for current and previous frame
    bool valid = (chs->dmix_coeffs_signature == XLL_DMIX_SIGNATURE(chs));
    chs->dmix_coeff_cur = chs->dmix_coeff + m * n * chs->dmix_coeffs_parity;
    chs->dmix_coeff_pre = chs->dmix_coeff + m * n * (chs->dmix_coeffs_parity ^ valid);

    if (chs->primary_chset) {
        chs->dmix_scale_cur = chs->dmix_scale_pre = NULL;
        chs->dmix_scale_inv_cur = chs->dmix_scale_inv_pre = NULL;
    } else {
        chs->dmix_scale_cur = chs->dmix_coeff_cur + m * chs->nchannels;
        chs->dmix_scale_pre = chs->dmix_coeff_pre + m * chs->nchannels;
        chs->dmix_scale_inv_cur = chs->dmix_coeff_cur + m * (chs->nchannels + 1);
        chs->dmix_scale_inv_pre = chs->dmix_coeff_pre + m * (chs->nchannels + 1);
    }

    int *coeff_ptr = chs->dmix_coeff_cur;
    for (int i = 0; i < m; i++) {
        int scale_inv = 0;

        // Downmix scale
        // Only for non-primary channel sets
        if (!chs->primary_chset) {
            int code = bits_get(&xll->bits, 9);
            int sign = (code >> 8) - 1; code &= 0xff;
            if (code > 0) {
                unsigned int index = code - 1;
                if (index < 40 || index >= dca_countof(dmix_table)) {
                    xll_err("Invalid XLL downmix scale index");
                    return -DCADEC_EBADDATA;
                }
                int scale = dmix_table[index];
                scale_inv = dmix_table_inv[index - 40];
                chs->dmix_scale_cur[i] = (scale ^ sign) - sign;
                chs->dmix_scale_inv_cur[i] = (scale_inv ^ sign) - sign;
            } else {
                chs->dmix_scale_cur[i] = 0;
                chs->dmix_scale_inv_cur[i] = 0;
            }
        }

        // Downmix coefficients
        for (int j = 0; j < chs->nchannels; j++) {
            int code = bits_get(&xll->bits, 9);
            int sign = (code >> 8) - 1; code &= 0xff;
            if (code > 0) {
                unsigned int index = code - 1;
                if (index >= dca_countof(dmix_table)) {
                    xll_err("Invalid XLL downmix coefficient index");
                    return -DCADEC_EBADDATA;
                }
                int coeff = dmix_table[index];
                if (!chs->primary_chset)
                    // Multiply by |InvDmixScale| to get |UndoDmixScale|
                    coeff = mul16(scale_inv, coeff);
                // Convert sign
                *coeff_ptr++ = (coeff ^ sign) - sign;
            } else {
                *coeff_ptr++ = 0;
            }
        }
    }

    return 0;
}

static int chs_parse_header(struct xll_chset *chs, struct exss_asset *asset)
{
    struct xll_decoder *xll = chs->decoder;
    int i, j, k, ret, header_pos = xll->bits.index;

    // Size of channel set sub-header
    int header_size = bits_get(&xll->bits, 10) + 1;

    // Check CRC
    if ((ret = bits_check_crc(&xll->bits, header_pos, header_pos + header_size * 8)) < 0) {
        xll_err("Invalid XLL sub-header checksum");
        return ret;
    }

    // Number of channels in the channel set
    chs->nchannels = bits_get(&xll->bits, 4) + 1;
    if (chs->nchannels > XLL_MAX_CHANNELS) {
        xll_err_once("Unsupported number of XLL channels (%d)", chs->nchannels);
        return -DCADEC_ENOSUP;
    }

    // Residual type
    chs->residual_encode = bits_get(&xll->bits, chs->nchannels);

    // PCM bit resolution
    chs->pcm_bit_res = bits_get(&xll->bits, 5) + 1;

    // Storage unit width
    chs->storage_bit_res = bits_get(&xll->bits, 5) + 1;

    // Original sampling frequency
    chs->freq = exss_sample_rates[bits_get(&xll->bits, 4)];
    if (chs->freq > 192000) {
        xll_err_once("Unsupported XLL sampling frequency");
        return -DCADEC_ENOSUP;
    }

    // Sampling frequency modifier
    chs->interpolate = bits_get(&xll->bits, 2);

    // Which replacement set this channel set is member of
    chs->replace_set_index = bits_get(&xll->bits, 2);
    if (chs->replace_set_index) {
        xll_err_once("Replacement sets are not supported");
        return -DCADEC_ENOSUP;
    }

    // Default replacement set flag
    if (chs->replace_set_index)
        bits_skip1(&xll->bits);

    if (asset->one_to_one_map_ch_to_spkr) {
        // Primary channel set flag
        chs->primary_chset = bits_get1(&xll->bits);

        // Downmix coefficients present in stream
        chs->dmix_coeffs_present = bits_get1(&xll->bits);

        // Downmix already performed by encoder
        chs->dmix_embedded = chs->dmix_coeffs_present && bits_get1(&xll->bits);

        // Downmix type
        if (chs->dmix_coeffs_present && chs->primary_chset) {
            chs->dmix_type = bits_get(&xll->bits, 3);
            if (chs->dmix_type >= DMIX_TYPE_COUNT) {
                xll_err("Invalid XLL primary channel set downmix type");
                return -DCADEC_EBADDATA;
            }
        }

        // Whether the channel set is part of a hierarchy
        chs->hier_chset = bits_get1(&xll->bits);

        // Downmix coefficients
        if (chs->dmix_coeffs_present && (ret = parse_dmix_coeffs(chs)) < 0)
            return ret;

        // Channel mask enabled
        chs->ch_mask_enabled = bits_get1(&xll->bits);
        if (chs->ch_mask_enabled) {
            // Channel mask for set
            chs->ch_mask = bits_get(&xll->bits, xll->ch_mask_nbits);
            if (dca_popcount(chs->ch_mask) != chs->nchannels) {
                xll_err("Invalid XLL channel mask");
                return -DCADEC_EBADDATA;
            }
        } else {
            chs->ch_mask = 0;
            // Angular speaker position table
            for (i = 0; i < chs->nchannels; i++) {
                bits_skip(&xll->bits, 9);
                bits_skip(&xll->bits, 9);
                bits_skip(&xll->bits, 7);
            }
        }
    } else {
        chs->primary_chset = true;
        chs->dmix_coeffs_present = false;
        chs->dmix_embedded = false;
        chs->hier_chset = true;
        chs->ch_mask_enabled = false;
        chs->ch_mask = 0;

        // Mapping coeffs present flag
        if (bits_get1(&xll->bits)) {
            xll_warn_once("Stream with speaker mapping coefficients");

            // Number of bits used to pack each
            // channel-to-speaker mapping coefficient
            int nchspkrcoefbits = 6 + 2 * bits_get(&xll->bits, 3);

            // Number of loudspeaker configurations
            int nspkrconfigs = bits_get(&xll->bits, 2) + 1;

            for (i = 0; i < nspkrconfigs; i++) {
                // Active channel mask for current configuration
                int active_ch_mask = bits_get(&xll->bits, chs->nchannels);

                // Number of speakers in current configuration
                int nspeakers = bits_get(&xll->bits, 6) + 1;

                // Speaker mask or polar coordinates flag
                if (bits_get1(&xll->bits)) {
                    // Speaker mask for current configuration
                    bits_skip(&xll->bits, xll->ch_mask_nbits);
                } else {
                    // Angular speaker position table for current configuration
                    for (j = 0; j < nspeakers; j++) {
                        bits_skip(&xll->bits, 9);
                        bits_skip(&xll->bits, 9);
                        bits_skip(&xll->bits, 7);
                    }
                }

                // Channel to loudspeaker mapping coefficients
                for (j = 0; j < chs->nchannels; j++)
                    if (active_ch_mask & (1 << j))
                        bits_skip(&xll->bits, nchspkrcoefbits);
            }
        }
    }

    // Mark downmix coefficients invalid until filtering
    chs->dmix_coeffs_signature = 0;

    if (chs->freq > 96000)
        // Extra frequency bands flag
        chs->nfreqbands = 2 * (1 + bits_get1(&xll->bits));
    else
        chs->nfreqbands = 1;

    if (chs->nfreqbands > XLL_MAX_BANDS) {
        xll_err_once("Extra frequency bands are not supported");
        return -DCADEC_ENOSUP;
    }

    // Clamp the sampling frequency to 96 kHz. The rest of the code will use
    // the number of bands to determine maximum frequency.
    if (chs->freq > 96000)
        chs->freq = 96000;

    if (chs->storage_bit_res > 16)
        chs->nabits = 5;
    else if (chs->storage_bit_res > 8)
        chs->nabits = 4;
    else
        chs->nabits = 3;
    if ((xll->nchsets > 1 || chs->nfreqbands > 1) && chs->nabits < 5)
        chs->nabits++;

    for (int band = 0; band < chs->nfreqbands; band++) {
        // Pairwise channel decorrelation
        chs->decor_enabled[band] = bits_get1(&xll->bits);
        if (chs->decor_enabled[band] && chs->nchannels > 1) {
            // Original channel order
            for (i = 0; i < chs->nchannels; i++) {
                chs->orig_order[band][i] = bits_get(&xll->bits, ch_nbits[chs->nchannels - 1]);
                if (chs->orig_order[band][i] >= chs->nchannels) {
                    xll_err("Invalid original channel order");
                    return -DCADEC_EBADDATA;
                }
            }
            // Pairwise channel coefficients
            for (i = 0; i < chs->nchannels / 2; i++) {
                if (bits_get1(&xll->bits))
                    chs->decor_coeff[band][i] = bits_get_signed_linear(&xll->bits, 7);
                else
                    chs->decor_coeff[band][i] = 0;
            }
        } else {
            for (i = 0; i < chs->nchannels; i++)
                chs->orig_order[band][i] = i;
            for (i = 0; i < chs->nchannels / 2; i++)
                chs->decor_coeff[band][i] = 0;
        }

        // Adaptive predictor order
        chs->highest_pred_order[band] = 0;
        for (i = 0; i < chs->nchannels; i++) {
            chs->adapt_pred_order[band][i] = bits_get(&xll->bits, 4);
            if (chs->adapt_pred_order[band][i] > chs->highest_pred_order[band])
                chs->highest_pred_order[band] = chs->adapt_pred_order[band][i];
        }
        if (chs->highest_pred_order[band] > xll->nsegsamples) {
            xll_err("Invalid adaptive predicition order");
            return -DCADEC_EBADDATA;
        }

        // Fixed predictor order
        for (i = 0; i < chs->nchannels; i++) {
            if (chs->adapt_pred_order[band][i] == 0)
                chs->fixed_pred_order[band][i] = bits_get(&xll->bits, 2);
            else
                chs->fixed_pred_order[band][i] = 0;
        }

        // Adaptive predictor quantized reflection coefficients
        for (i = 0; i < chs->nchannels; i++) {
            for (j = 0; j < chs->adapt_pred_order[band][i]; j++) {
                k = bits_get_signed_linear(&xll->bits, 8);
                if (k == -128) {
                    xll_err("Invalid reflection coefficient index");
                    return -DCADEC_EBADDATA;
                }
                if (k < 0)
                    chs->adapt_refl_coeff[band][i][j] = -(int)refl_coeff_table[-k];
                else
                    chs->adapt_refl_coeff[band][i][j] =  (int)refl_coeff_table[ k];
            }
        }

        // Downmix performed by encoder in extension frequency band
        chs->band_dmix_embedded[band] = chs->dmix_embedded && (band == 0 || bits_get1(&xll->bits));

        // MSB/LSB split flag in extension frequency band
        if ((band == 0 && xll->scalable_lsbs) || (band != 0 && bits_get1(&xll->bits))) {
            // Size of LSB section in any segment
            chs->lsb_section_size[band] = bits_get(&xll->bits, xll->seg_size_nbits);
            if (chs->lsb_section_size[band] < 0 || chs->lsb_section_size[band] > xll->frame_size) {
                xll_err("Invalid LSB section size");
                return -DCADEC_EBADDATA;
            }

            // Account for optional CRC bytes after LSB section
            if (chs->lsb_section_size[band] && (xll->band_crc_present > 2 ||
                                                (band == 0 && xll->band_crc_present > 1)))
                chs->lsb_section_size[band] += 2;

            // Number of bits to represent the samples in LSB part
            for (i = 0; i < chs->nchannels; i++) {
                chs->nscalablelsbs[band][i] = bits_get(&xll->bits, 4);
                if (chs->nscalablelsbs[band][i] && !chs->lsb_section_size[band]) {
                    xll_err("LSB section missing with non-zero LSB width");
                    return -DCADEC_EBADDATA;
                }
            }
        } else {
            chs->lsb_section_size[band] = 0;
            for (i = 0; i < chs->nchannels; i++)
                chs->nscalablelsbs[band][i] = 0;
        }

        // Scalable resolution flag in extension frequency band
        if ((band == 0 && xll->scalable_lsbs) || (band != 0 && bits_get1(&xll->bits))) {
            // Number of bits discarded by authoring
            for (i = 0; i < chs->nchannels; i++)
                chs->bit_width_adjust[band][i] = bits_get(&xll->bits, 4);
        } else {
            for (i = 0; i < chs->nchannels; i++)
                chs->bit_width_adjust[band][i] = 0;
        }
    }

    // Reserved
    // Byte align
    // CRC16 of channel set sub-header
    if ((ret = bits_seek(&xll->bits, header_pos + header_size * 8)) < 0)
        xll_err("Read past end of XLL sub-header");
    return ret;
}

static int chs_alloc_msb_band_data(struct xll_chset *chs)
{
    struct xll_decoder *xll = chs->decoder;

    // Reallocate MSB sample buffer
    if (ta_zalloc_fast(xll->chset, &chs->sample_buffer1,
                       (xll->nframesamples + XLL_DECI_HISTORY) *
                       chs->nchannels * chs->nfreqbands, sizeof(int)) < 0)
        return -DCADEC_ENOMEM;

    int *ptr = chs->sample_buffer1 + XLL_DECI_HISTORY;
    for (int band = 0; band < chs->nfreqbands; band++) {
        for (int i = 0; i < chs->nchannels; i++) {
            chs->msb_sample_buffer[band][i] = ptr;
            ptr += xll->nframesamples + XLL_DECI_HISTORY;
        }
    }

    return 0;
}

static int chs_alloc_lsb_band_data(struct xll_chset *chs)
{
    struct xll_decoder *xll = chs->decoder;

    // Number of frequency bands that have MSB/LSB split
    int nfreqbands = 0;
    for (int band = 0; band < chs->nfreqbands; band++)
        if (chs->lsb_section_size[band])
            nfreqbands++;
    if (!nfreqbands)
        return 0;

    // Reallocate LSB sample buffer
    if (ta_zalloc_fast(xll->chset, &chs->sample_buffer2,
                       xll->nframesamples * chs->nchannels * nfreqbands,
                       sizeof(int)) < 0)
        return -DCADEC_ENOMEM;

    int *ptr = chs->sample_buffer2;
    for (int band = 0; band < chs->nfreqbands; band++) {
        if (chs->lsb_section_size[band]) {
            for (int i = 0; i < chs->nchannels; i++) {
                chs->lsb_sample_buffer[band][i] = ptr;
                ptr += xll->nframesamples;
            }
        } else {
            for (int i = 0; i < chs->nchannels; i++)
                chs->lsb_sample_buffer[band][i] = NULL;
        }
    }

    return 0;
}

static int chs_parse_band_data(struct xll_chset *chs, int band, int seg, int band_data_nbytes)
{
    struct xll_decoder *xll = chs->decoder;
    int i, j, ret;

    // Calculate bit index where band data ends
    int band_data_end = xll->bits.index + band_data_nbytes * 8;

    // Skip decoding inactive channel sets
    if (chs >= &xll->chset[xll->nactivechsets])
        return bits_seek(&xll->bits, band_data_end);

    // Start unpacking MSB portion of the segment
    if (seg == 0 || bits_get1(&xll->bits) == false) {
        // Unpack segment type
        // 0 - distinct coding parameters for each channel
        // 1 - common coding parameters for all channels
        chs->seg_type = bits_get1(&xll->bits);

        // Determine number of coding parameters encoded in segment
        int nparamsets = (chs->seg_type == false) ? chs->nchannels : 1;

        // Unpack Rice coding parameters
        for (i = 0; i < nparamsets; i++) {
            // Unpack Rice coding flag
            // 0 - linear code, 1 - Rice code
            chs->rice_code_flag[i] = bits_get1(&xll->bits);
            if (chs->seg_type == false && chs->rice_code_flag[i] == true) {
                // Unpack Hybrid Rice coding flag
                // 0 - Rice code, 1 - Hybrid Rice code
                if (bits_get1(&xll->bits))
                    // Unpack binary code length for isolated samples
                    chs->bitalloc_hybrid_linear[i] = bits_get(&xll->bits, chs->nabits) + 1;
                else
                    // 0 indicates no Hybrid Rice coding
                    chs->bitalloc_hybrid_linear[i] = 0;
            } else {
                // 0 indicates no Hybrid Rice coding
                chs->bitalloc_hybrid_linear[i] = 0;
            }
        }

        // Unpack coding parameters
        for (i = 0; i < nparamsets; i++) {
            if (seg == 0) {
                // Unpack coding parameter for part A of segment 0
                chs->bitalloc_part_a[i] = bits_get(&xll->bits, chs->nabits);

                // Adjust for the linear code
                if (chs->rice_code_flag[i] == false && chs->bitalloc_part_a[i])
                    chs->bitalloc_part_a[i]++;

                if (chs->seg_type == false)
                    chs->nsamples_part_a[i] = chs->adapt_pred_order[band][i];
                else
                    chs->nsamples_part_a[i] = chs->highest_pred_order[band];
            } else {
                chs->bitalloc_part_a[i] = 0;
                chs->nsamples_part_a[i] = 0;
            }

            // Unpack coding parameter for part B of segment
            chs->bitalloc_part_b[i] = bits_get(&xll->bits, chs->nabits);

            // Adjust for the linear code
            if (chs->rice_code_flag[i] == false && chs->bitalloc_part_b[i])
                chs->bitalloc_part_b[i]++;
        }
    }

    // Unpack entropy codes
    for (i = 0; i < chs->nchannels; i++) {
        // Select index of coding parameters
        int k = (chs->seg_type == false) ? i : 0;

        // Slice the segment into parts A and B
        int *part_a = chs->msb_sample_buffer[band][i] + seg * xll->nsegsamples;
        int *part_b = part_a + chs->nsamples_part_a[k];
        int nsamples_part_b = xll->nsegsamples - chs->nsamples_part_a[k];

        if (chs->rice_code_flag[k] == false) {
            // Linear codes
            // Unpack all residuals of part A of segment 0
            bits_get_signed_linear_array(&xll->bits, part_a,
                                         chs->nsamples_part_a[k],
                                         chs->bitalloc_part_a[k]);

            // Unpack all residuals of part B of segment 0 and others
            bits_get_signed_linear_array(&xll->bits, part_b,
                                         nsamples_part_b,
                                         chs->bitalloc_part_b[k]);
        } else {
            // Rice codes
            // Unpack all residuals of part A of segment 0
            bits_get_signed_rice_array(&xll->bits, part_a,
                                       chs->nsamples_part_a[k],
                                       chs->bitalloc_part_a[k]);

            if (chs->bitalloc_hybrid_linear[k]) {
                // Hybrid Rice codes
                // Unpack the number of isolated samples
                int nisosamples = bits_get(&xll->bits, xll->nsegsamples_log2);

                // Set all locations to 0
                memset(part_b, 0, sizeof(*part_b) * nsamples_part_b);

                // Extract the locations of isolated samples and flag by -1
                for (j = 0; j < nisosamples; j++) {
                    int loc = bits_get(&xll->bits, xll->nsegsamples_log2);
                    if (loc >= nsamples_part_b) {
                        xll_err("Invalid isolated sample location");
                        return -DCADEC_EBADDATA;
                    }
                    part_b[loc] = -1;
                }

                // Unpack all residuals of part B of segment 0 and others
                for (j = 0; j < nsamples_part_b; j++) {
                    if (part_b[j] == -1)
                        part_b[j] = bits_get_signed_linear(&xll->bits,
                                                           chs->bitalloc_hybrid_linear[k]);
                    else
                        part_b[j] = bits_get_signed_rice(&xll->bits,
                                                         chs->bitalloc_part_b[k]);
                }
            } else {
                // Rice codes
                // Unpack all residuals of part B of segment 0 and others
                bits_get_signed_rice_array(&xll->bits, part_b,
                                           nsamples_part_b,
                                           chs->bitalloc_part_b[k]);
            }
        }
    }

    // Unpack decimator history for frequency band 1
    if (seg == 0 && band == 1) {
        int nbits = bits_get(&xll->bits, 5) + 1;
        for (i = 0; i < chs->nchannels; i++)
            for (j = 1; j < XLL_DECI_HISTORY; j++)
                chs->deci_history[i][j] = bits_get_signed(&xll->bits, nbits);
    }

    // Start unpacking LSB portion of the segment
    if (chs->lsb_section_size[band]) {
        // Skip to the start of LSB portion
        if ((ret = bits_seek(&xll->bits, band_data_end -
                             chs->lsb_section_size[band] * 8)) < 0) {
            xll_err("Read past end of band data");
            return ret;
        }

        // Unpack all LSB parts of residuals of this segment
        for (i = 0; i < chs->nchannels; i++) {
            if (chs->nscalablelsbs[band][i]) {
                bits_get_array(&xll->bits,
                               chs->lsb_sample_buffer[band][i] +
                               seg * xll->nsegsamples,
                               xll->nsegsamples,
                               chs->nscalablelsbs[band][i]);
            }
        }
    }

    // Skip to the end of band data
    if ((ret = bits_seek(&xll->bits, band_data_end)) < 0)
        xll_err("Read past end of band data");
    return ret;
}

static void chs_clear_band_data(struct xll_chset *chs, int band, int seg)
{
    struct xll_decoder *xll = chs->decoder;

    for (int i = 0; i < chs->nchannels; i++)
        memset(chs->msb_sample_buffer[band][i] +
               seg * xll->nsegsamples, 0, xll->nsegsamples * sizeof(int));

    if (seg == 0 && band == 1)
        memset(chs->deci_history, 0, sizeof(chs->deci_history));

    if (chs->lsb_section_size[band])
        for (int i = 0; i < chs->nchannels; i++)
            memset(chs->lsb_sample_buffer[band][i] +
                   seg * xll->nsegsamples, 0, xll->nsegsamples * sizeof(int));
}

void xll_clear_band_data(struct xll_chset *chs, int band)
{
    struct xll_decoder *xll = chs->decoder;

    for (int i = 0; i < chs->nchannels; i++)
        memset(chs->msb_sample_buffer[band][i], 0, xll->nframesamples * sizeof(int));

    if (chs->lsb_section_size[band])
        for (int i = 0; i < chs->nchannels; i++)
            memset(chs->lsb_sample_buffer[band][i], 0, xll->nframesamples * sizeof(int));
}

void xll_filter_band_data(struct xll_chset *chs, int band)
{
    struct xll_decoder *xll = chs->decoder;
    int nsamples = xll->nframesamples;
    int i, j, k;

    // Inverse adaptive or fixed prediction
    for (i = 0; i < chs->nchannels; i++) {
        int *buf = chs->msb_sample_buffer[band][i];
        int order = chs->adapt_pred_order[band][i];
        if (order > 0) {
            int coeff[16];
            // Conversion from reflection coefficients to direct form coefficients
            for (j = 0; j < order; j++) {
                int rc = chs->adapt_refl_coeff[band][i][j];
                for (k = 0; k < (j + 1) / 2; k++) {
                    int tmp1 = coeff[    k    ];
                    int tmp2 = coeff[j - k - 1];
                    coeff[    k    ] = tmp1 + mul16(rc, tmp2);
                    coeff[j - k - 1] = tmp2 + mul16(rc, tmp1);
                }
                coeff[j] = rc;
            }
            for (j = 0; j < nsamples - order; j++) {
                int64_t err = INT64_C(0);
                for (k = 0; k < order; k++)
                    err += (int64_t)buf[j + k] * coeff[order - k - 1];
                // Round and scale the prediction
                // Calculate the original sample
                buf[j + k] -= clip23(norm16(err));
            }
        } else {
            // Inverse fixed coefficient prediction
            for (j = 0; j < chs->fixed_pred_order[band][i]; j++)
                for (k = 1; k < nsamples; k++)
                    buf[k] += buf[k - 1];
        }
    }

    // Inverse pairwise channel decorrellation
    if (chs->decor_enabled[band]) {
        for (i = 0; i < chs->nchannels / 2; i++) {
            int coeff = chs->decor_coeff[band][i];
            if (coeff) {
                int *src = chs->msb_sample_buffer[band][i * 2 + 0];
                int *dst = chs->msb_sample_buffer[band][i * 2 + 1];
                for (j = 0; j < nsamples; j++)
                    dst[j] += mul3(src[j], coeff);
            }
        }

        // Reorder channel pointers to the original order
        int *tmp[XLL_MAX_CHANNELS];
        for (i = 0; i < chs->nchannels; i++)
            tmp[i] = chs->msb_sample_buffer[band][i];
        for (i = 0; i < chs->nchannels; i++)
            chs->msb_sample_buffer[band][chs->orig_order[band][i]] = tmp[i];
    }

    // Map output channel pointers for frequency band 0
    if (band == XLL_BAND_0)
        for (i = 0; i < chs->nchannels; i++)
            chs->out_sample_buffer[i] = chs->msb_sample_buffer[band][i];
}

int xll_get_lsb_width(struct xll_chset *chs, int band, int ch)
{
    struct xll_decoder *xll = chs->decoder;
    int adj = chs->bit_width_adjust[band][ch];
    int shift = chs->nscalablelsbs[band][ch];

    if (xll->fixed_lsb_width)
        shift = xll->fixed_lsb_width;
    else if (shift && adj)
        shift += adj - 1;
    else
        shift += adj;

    assert(shift < chs->pcm_bit_res);
    return shift;
}

void xll_assemble_msbs_lsbs(struct xll_chset *chs, int band)
{
    struct xll_decoder *xll = chs->decoder;
    int nsamples = xll->nframesamples;

    for (int ch = 0; ch < chs->nchannels; ch++) {
        int shift = xll_get_lsb_width(chs, band, ch);
        if (shift) {
            int *msb = chs->msb_sample_buffer[band][ch];
            if (chs->nscalablelsbs[band][ch]) {
                int *lsb = chs->lsb_sample_buffer[band][ch];
                int adj = chs->bit_width_adjust[band][ch];
                for (int n = 0; n < nsamples; n++)
                    msb[n] = (msb[n] * (1 << shift)) + (lsb[n] << adj);
            } else {
                for (int n = 0; n < nsamples; n++)
                    msb[n] *= 1 << shift;
            }
        }
    }
}

static void filter0(int *dst, const int *src, int nsamples)
{
    for (int n = 0; n < nsamples; n++)
        dst[n] -= src[n];
}

static void filter1(int *dst, const int *src, int nsamples, int32_t coeff)
{
    for (int n = 0; n < nsamples; n++)
        dst[n] -= mul22(src[n], coeff);
}

static void filter2(int *dst, const int *src, int nsamples, int32_t coeff)
{
    for (int n = 0; n < nsamples; n++)
        dst[n] -= mul23(src[n], coeff);
}

int xll_assemble_freq_bands(struct xll_chset *chs)
{
    struct xll_decoder *xll = chs->decoder;
    int nsamples = xll->nframesamples;

    // Reallocate frequency band assembly buffer
    if (ta_alloc_fast(xll->chset, &chs->sample_buffer3,
                      2 * nsamples * chs->nchannels, sizeof(int)) < 0)
        return -DCADEC_ENOMEM;

    // Assemble frequency bands 0 and 1
    int *ptr = chs->sample_buffer3;
    for (int ch = 0; ch < chs->nchannels; ch++) {
        // Remap output channel pointer to assembly buffer
        chs->out_sample_buffer[ch] = ptr;

        int *band0 = chs->msb_sample_buffer[XLL_BAND_0][ch];
        int *band1 = chs->msb_sample_buffer[XLL_BAND_1][ch];

        // Copy decimator history
        for (int i = 1; i < XLL_DECI_HISTORY; i++)
            band0[i - XLL_DECI_HISTORY] = chs->deci_history[ch][i];

        // Filter
        filter1(band0, band1, nsamples, band_coeff_table0[0]);
        filter1(band1, band0, nsamples, band_coeff_table0[1]);
        filter1(band0, band1, nsamples, band_coeff_table0[2]);
        filter0(band1, band0, nsamples);

        for (int i = 0; i < XLL_DECI_HISTORY; i++) {
            filter2(band0, band1, nsamples, band_coeff_table1[i]);
            filter2(band1, band0, nsamples, band_coeff_table2[i]);
            filter2(band0, band1, nsamples, band_coeff_table1[i]);
            band0--;
        }

        // Assemble
        for (int i = 0; i < nsamples; i++) {
            *ptr++ = *band1++;
            *ptr++ = *++band0;
        }
    }

    return 0;
}

int xll_map_ch_to_spkr(struct xll_chset *chs, int ch)
{
    struct xll_decoder *xll = chs->decoder;

    if (chs->ch_mask_enabled) {
        for (int spkr = 0, pos = 0; spkr < xll->ch_mask_nbits; spkr++)
            if (chs->ch_mask & (1U << spkr))
                if (pos++ == ch)
                    return spkr;
        return -1;  // Invalid
    }

    // Map to LtRt
    if (chs->nchannels == 2) {
        if (ch == 0)
            return SPEAKER_L;
        if (ch == 1)
            return SPEAKER_R;
    }

    return -1;
}

static int parse_common_header(struct xll_decoder *xll)
{
    int ret;

    // XLL extension sync word
    if (bits_get(&xll->bits, 32) != SYNC_WORD_XLL) {
        xll_verbose("Invalid XLL sync word");
        return -DCADEC_ENOSYNC;
    }

    // Version number
    int stream_ver = bits_get(&xll->bits, 4) + 1;
    if (stream_ver > 1) {
        xll_err_once("Unsupported XLL stream version (%d)", stream_ver);
        return -DCADEC_ENOSUP;
    }

    // Lossless frame header length
    int header_size = bits_get(&xll->bits, 8) + 1;

    // Check CRC
    if ((ret = bits_check_crc(&xll->bits, 32, header_size * 8)) < 0) {
        xll_err("Invalid XLL common header checksum");
        return ret;
    }

    // Number of bits used to read frame size
    int frame_size_nbits = bits_get(&xll->bits, 5) + 1;

    // Number of bytes in a lossless frame
    xll->frame_size = bits_get(&xll->bits, frame_size_nbits);
    if (xll->frame_size < 0 || xll->frame_size >= XLL_PBR_SIZE) {
        xll_err("Invalid XLL frame size");
        return -DCADEC_EBADDATA;
    }
    xll->frame_size++;

    // Number of channels sets per frame
    xll->nchsets = bits_get(&xll->bits, 4) + 1;
    if (xll->nchsets > 3)
        xll_warn_once("Stream with %d channel sets", xll->nchsets);

    // Number of segments per frame
    int nframesegs_log2 = bits_get(&xll->bits, 4);
    xll->nframesegs = 1 << nframesegs_log2;
    if (xll->nframesegs > 1024) {
        xll_err("Too many segments per frame");
        return -DCADEC_EBADDATA;
    }

    // Samples in segment per one frequency band for the first channel set
    // Maximum value is 256 for sampling frequencies <= 48 kHz
    // Maximum value is 512 for sampling frequencies > 48 kHz
    xll->nsegsamples_log2 = bits_get(&xll->bits, 4);
    if (!xll->nsegsamples_log2) {
        xll_err("Too few samples per segment");
        return -DCADEC_EBADDATA;
    }
    xll->nsegsamples = 1 << xll->nsegsamples_log2;
    if (xll->nsegsamples > 512) {
        xll_err("Too many samples per segment");
        return -DCADEC_EBADDATA;
    }

    // Samples in frame per one frequency band for the first channel set
    xll->nframesamples_log2 = xll->nsegsamples_log2 + nframesegs_log2;
    xll->nframesamples = 1 << xll->nframesamples_log2;
    if (xll->nframesamples > 65536) {
        xll_err("Too many samples per frame");
        return -DCADEC_EBADDATA;
    }

    // Number of bits used to read segment size
    xll->seg_size_nbits = bits_get(&xll->bits, 5) + 1;

    // Presence of CRC16 within each frequency band
    // 0 - No CRC16 within band
    // 1 - CRC16 placed at the end of MSB0
    // 2 - CRC16 placed at the end of MSB0 and LSB0
    // 3 - CRC16 placed at the end of MSB0 and LSB0 and other frequency bands
    xll->band_crc_present = bits_get(&xll->bits, 2);
    if (xll->band_crc_present)
        xll_warn_once("Stream with band CRC present (%d)", xll->band_crc_present);

    // MSB/LSB split flag
    xll->scalable_lsbs = bits_get1(&xll->bits);

    // Channel position mask
    xll->ch_mask_nbits = bits_get(&xll->bits, 5) + 1;

    // Fixed LSB width
    if (xll->scalable_lsbs)
        xll->fixed_lsb_width = bits_get(&xll->bits, 4);
    else
        xll->fixed_lsb_width = 0;
    if (xll->fixed_lsb_width)
        xll_warn_once("Stream with fixed LSB width (%d)", xll->fixed_lsb_width);

    // Reserved
    // Byte align
    // Header CRC16 protection
    if ((ret = bits_seek(&xll->bits, header_size * 8)) < 0)
        xll_err("Read past end of XLL common header");
    return ret;
}

static int parse_sub_headers(struct xll_decoder *xll, struct exss_asset *asset)
{
    int ret;

    // Reallocate channel sets
    if (ta_zalloc_fast(xll, &xll->chset, xll->nchsets, sizeof(struct xll_chset)) < 0)
        return -DCADEC_ENOMEM;

    // Parse channel set headers
    xll->nfreqbands = 0;
    xll->nchannels = 0;
    for_each_chset(xll, chs) {
        chs->decoder = xll;
        chs->dmix_m = xll->nchannels;
        if ((ret = chs_parse_header(chs, asset)) < 0)
            return ret;
        if (chs->nfreqbands > xll->nfreqbands)
            xll->nfreqbands = chs->nfreqbands;
        if (chs->hier_chset)
            xll->nchannels += chs->nchannels;
    }

    // Number of active channel sets to decode
    if (xll->flags & DCADEC_FLAG_KEEP_DMIX_2CH)
        xll->nactivechsets = 1;
    else if (xll->flags & DCADEC_FLAG_KEEP_DMIX_6CH)
        xll->nactivechsets = (xll->chset->nchannels < 5 && xll->nchsets > 1) ? 2 : 1;
    else
        xll->nactivechsets = xll->nchsets;

    return 0;
}

static int parse_navi_table(struct xll_decoder *xll)
{
    int navi_nb = xll->nfreqbands * xll->nframesegs * xll->nchsets;
    if (navi_nb > 1024) {
        xll_err("Too many NAVI entries");
        return -DCADEC_EBADDATA;
    }

    // Reallocate NAVI table
    if (ta_alloc_fast(xll, &xll->navi, navi_nb, sizeof(*xll->navi)) < 0)
        return -DCADEC_ENOMEM;

    // Parse NAVI
    int navi_pos = xll->bits.index;
    int *navi_ptr = xll->navi;
    int navi_size = 0;
    for (int band = 0; band < xll->nfreqbands; band++) {
        for (int seg = 0; seg < xll->nframesegs; seg++) {
            for_each_chset(xll, chs) {
                int size = 0;
                if (chs->nfreqbands > band) {
                    size = bits_get(&xll->bits, xll->seg_size_nbits);
                    if (size < 0 || size >= xll->frame_size) {
                        xll_err("Invalid NAVI size");
                        return -DCADEC_EBADDATA;
                    }
                    size++;
                }
                *navi_ptr++ = size;
                navi_size += size;
            }
        }
    }

    if (navi_size > xll->frame_size) {
        xll_err("Invalid NAVI size");
        return -DCADEC_EBADDATA;
    }

    // Byte align
    // CRC16
    bits_align1(&xll->bits);
    bits_skip(&xll->bits, 16);

    // Check CRC
    int ret;
    if ((ret = bits_check_crc(&xll->bits, navi_pos, xll->bits.index)) < 0)
        xll_err("Invalid NAVI checksum");
    return ret;
}

static int parse_band_data(struct xll_decoder *xll)
{
    int ret;
    for_each_active_chset(xll, chs) {
        if ((ret = chs_alloc_msb_band_data(chs)) < 0)
            return ret;
        if ((ret = chs_alloc_lsb_band_data(chs)) < 0)
            return ret;
    }

    xll->nfailedsegs = 0;

    int navi_pos = xll->bits.index;
    int *navi_ptr = xll->navi;
    for (int band = 0; band < xll->nfreqbands; band++) {
        for (int seg = 0; seg < xll->nframesegs; seg++) {
            for_each_chset(xll, chs) {
                if (chs->nfreqbands > band) {
                    navi_pos += *navi_ptr * 8;
                    if ((ret = chs_parse_band_data(chs, band, seg, *navi_ptr)) < 0) {
                        if (xll->flags & DCADEC_FLAG_STRICT)
                            return ret;
                        // Zero band data and advance to next segment
                        chs_clear_band_data(chs, band, seg);
                        xll->bits.index = navi_pos;
                        xll->nfailedsegs++;
                    }
                }
                navi_ptr++;
            }
        }
    }

    return 0;
}

static int parse_frame(struct xll_decoder *xll, uint8_t *data, int size, struct exss_asset *asset)
{
    int ret;

    bits_init(&xll->bits, data, size);
    if ((ret = parse_common_header(xll)) < 0)
        return ret;
    if ((ret = parse_sub_headers(xll, asset)) < 0)
        return ret;
    if ((ret = parse_navi_table(xll)) < 0)
        return ret;
    if ((ret = parse_band_data(xll)) < 0)
        return ret;
    if ((ret = bits_seek(&xll->bits, xll->frame_size * 8)) < 0)
        xll_err("Read past end of XLL frame");
    return ret;
}

static void clear_pbr(struct xll_decoder *xll)
{
    ta_free(xll->pbr_buffer);
    xll->pbr_buffer = NULL;
    xll->pbr_length = 0;
    xll->pbr_delay = 0;
}

static int copy_to_pbr(struct xll_decoder *xll, uint8_t *data, int size, int delay)
{
    if (size > XLL_PBR_SIZE) {
        xll_err("PBR smoothing buffer overflow");
        return -DCADEC_EINVAL;
    }
    if (!(xll->pbr_buffer = ta_zalloc_size(xll, XLL_PBR_SIZE + DCADEC_BUFFER_PADDING)))
        return -DCADEC_ENOMEM;
    memcpy(xll->pbr_buffer, data, size);
    xll->pbr_length = size;
    xll->pbr_delay = delay;
    return 0;
}

static int parse_frame_no_pbr(struct xll_decoder *xll, uint8_t *data, int size, struct exss_asset *asset)
{
    int ret = parse_frame(xll, data, size, asset);

    // If XLL packet data didn't start with a sync word, we must have jumped
    // right into the middle of PBR smoothing period
    if (ret == -DCADEC_ENOSYNC && asset->xll_sync_present) {
        if (asset->xll_sync_offset > size) {
            xll_err("Invalid XLL sync word offset");
            return -DCADEC_EINVAL;
        }
        if (asset->xll_delay_nframes < 0) {
            xll_err("Invalid XLL decoding delay");
            return -DCADEC_EINVAL;
        }

        // Skip to the next sync word in this packet
        data += asset->xll_sync_offset;
        size -= asset->xll_sync_offset;

        // If decoding delay is set, put the frame into PBR buffer and return
        // failure code. Higher level decoder is expected to switch to lossy
        // core decoding or mute its output until decoding delay expires.
        if (asset->xll_delay_nframes > 0) {
            if ((ret = copy_to_pbr(xll, data, size, asset->xll_delay_nframes)) < 0)
                return ret;
            return -DCADEC_ENOSYNC;
        }

        // No decoding delay, just parse the frame in place
        ret = parse_frame(xll, data, size, asset);
    }

    if (ret < 0)
        return ret;

    if (xll->frame_size > size)
        return -DCADEC_EINVAL;

    // If the XLL decoder didn't consume full packet, start PBR smoothing period
    if (xll->frame_size < size)
        if ((ret = copy_to_pbr(xll, data + xll->frame_size, size - xll->frame_size, 0)) < 0)
            return ret;

    return 0;
}

static int parse_frame_pbr(struct xll_decoder *xll, uint8_t *data, int size, struct exss_asset *asset)
{
    int ret;

    if (size > XLL_PBR_SIZE - xll->pbr_length) {
        xll_err("PBR smoothing buffer overflow");
        ret = -DCADEC_EINVAL;
        goto fail;
    }

    memcpy(xll->pbr_buffer + xll->pbr_length, data, size);
    xll->pbr_length += size;

    // Respect decoding delay after synchronization error
    if (xll->pbr_delay > 0) {
        if (--xll->pbr_delay > 0) {
            xll_verbose("Waiting until decoding delay expires (%d)", xll->pbr_delay);
            return -DCADEC_ENOSYNC;
        }
    }

    if ((ret = parse_frame(xll, xll->pbr_buffer, xll->pbr_length, asset)) < 0)
        goto fail;

    if (xll->frame_size > xll->pbr_length) {
        ret = -DCADEC_EINVAL;
        goto fail;
    }

    if (xll->frame_size == xll->pbr_length) {
        // End of PBR smoothing period
        clear_pbr(xll);
    } else {
        xll->pbr_length -= xll->frame_size;
        memmove(xll->pbr_buffer, xll->pbr_buffer + xll->frame_size, xll->pbr_length);
    }

    return 0;

fail:
    // For now, throw out all PBR state on failure.
    // Perhaps we can be smarter and try to at least resync.
    clear_pbr(xll);
    return ret;
}

static void clear_chs(struct xll_decoder *xll)
{
    if (xll->chset) {
        for_each_chset(xll, chs) {
            chs->dmix_coeffs_signature = 0;
            chs->dmix_coeffs_parity = false;
        }
    }
}

int xll_parse(struct xll_decoder *xll, uint8_t *data, struct exss_asset *asset)
{
    int ret;

    if (xll->hd_stream_id != asset->hd_stream_id) {
        xll_clear(xll);
        xll->hd_stream_id = asset->hd_stream_id;
    }

    if (xll->pbr_buffer)
        ret = parse_frame_pbr(xll, data + asset->xll_offset, asset->xll_size, asset);
    else
        ret = parse_frame_no_pbr(xll, data + asset->xll_offset, asset->xll_size, asset);

    if (ret < 0)
        clear_chs(xll);

    return ret;
}

void xll_clear(struct xll_decoder *xll)
{
    if (xll) {
        clear_pbr(xll);
        clear_chs(xll);
    }
}
