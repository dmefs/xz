///////////////////////////////////////////////////////////////////////////////
//
/// \file       lzma2_decoder.c
/// \brief      LZMA2 decoder
//
//  Copyright (C) 1999-2008 Igor Pavlov
//  Copyright (C) 2008 Lasse Collin
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
///////////////////////////////////////////////////////////////////////////////

#include "lzma2_decoder.h"
#include "lz_decoder.h"
#include "lzma_decoder.h"


struct lzma_coder_s {
	enum sequence {
		SEQ_CONTROL,
		SEQ_UNCOMPRESSED_1,
		SEQ_UNCOMPRESSED_2,
		SEQ_COMPRESSED_0,
		SEQ_COMPRESSED_1,
		SEQ_PROPERTIES,
		SEQ_LZMA,
		SEQ_COPY,
	} sequence;

	/// Sequence after the size fields have been decoded.
	enum sequence next_sequence;

	/// LZMA decoder
	lzma_lz_decoder lzma;

	/// Uncompressed size of LZMA chunk
	size_t uncompressed_size;

	/// Compressed size of the chunk (naturally equals to uncompressed
	/// size of uncompressed chunk)
	size_t compressed_size;

	/// True if properties are needed. This is false before the
	/// first LZMA chunk.
	bool need_properties;

	/// True if dictionary reset is needed. This is false before the
	/// first chunk (LZMA or uncompressed).
	bool need_dictionary_reset;

	lzma_options_lzma options;
};


static lzma_ret
lzma2_decode(lzma_coder *restrict coder, lzma_dict *restrict dict,
		const uint8_t *restrict in, size_t *restrict in_pos,
		size_t in_size)
{
	// With SEQ_LZMA it is possible that no new input is needed to do
	// some progress. The rest of the sequences assume that there is
	// at least one byte of input.
	while (*in_pos < in_size || coder->sequence == SEQ_LZMA)
	switch (coder->sequence) {
	case SEQ_CONTROL:
		if (in[*in_pos] & 0x80) {
			// Get the highest five bits of uncompressed size.
			coder->uncompressed_size
					= (uint32_t)(in[*in_pos] & 0x1F) << 16;
			coder->sequence = SEQ_UNCOMPRESSED_1;

			// See if we need to reset dictionary or state.
			switch ((in[(*in_pos)++] >> 5) & 3) {
			case 3:
				dict_reset(dict);
				coder->need_dictionary_reset = false;

			// Fall through

			case 2:
				if (coder->need_dictionary_reset)
					return LZMA_DATA_ERROR;

				coder->need_properties = false;
				coder->next_sequence = SEQ_PROPERTIES;
				break;

			case 1:
				if (coder->need_properties)
					return LZMA_DATA_ERROR;

				coder->lzma.reset(coder->lzma.coder,
						&coder->options);

				coder->next_sequence = SEQ_LZMA;
				break;

			case 0:
				if (coder->need_properties)
					return LZMA_DATA_ERROR;

				coder->next_sequence = SEQ_LZMA;
				break;
			}

		} else {
			switch (in[(*in_pos)++]) {
			case 0:
				// End of payload marker
				return LZMA_STREAM_END;

			case 1:
				// Dictionary reset
				dict_reset(dict);
				coder->need_dictionary_reset = false;

			// Fall through

			case 2:
				if (coder->need_dictionary_reset)
					return LZMA_DATA_ERROR;

				// Uncompressed chunk; we need to read total
				// size first.
				coder->sequence = SEQ_COMPRESSED_0;
				coder->next_sequence = SEQ_COPY;
				break;

			default:
				return LZMA_DATA_ERROR;
			}
		}

		break;

	case SEQ_UNCOMPRESSED_1:
		coder->uncompressed_size += (uint32_t)(in[(*in_pos)++]) << 8;
		coder->sequence = SEQ_UNCOMPRESSED_2;
		break;

	case SEQ_UNCOMPRESSED_2:
		coder->uncompressed_size += in[(*in_pos)++] + 1;
		coder->sequence = SEQ_COMPRESSED_0;
		coder->lzma.set_uncompressed(coder->lzma.coder,
				coder->uncompressed_size);
		break;

	case SEQ_COMPRESSED_0:
		coder->compressed_size = (uint32_t)(in[(*in_pos)++]) << 8;
		coder->sequence = SEQ_COMPRESSED_1;
		break;

	case SEQ_COMPRESSED_1:
		coder->compressed_size += in[(*in_pos)++] + 1;
		coder->sequence = coder->next_sequence;
		break;

	case SEQ_PROPERTIES:
		if (lzma_lzma_lclppb_decode(&coder->options, in[(*in_pos)++]))
			return LZMA_DATA_ERROR;

		coder->lzma.reset(coder->lzma.coder, &coder->options);

		coder->sequence = SEQ_LZMA;
		break;

	case SEQ_LZMA: {
		// Store the start offset so that we can update
		// coder->compressed_size later.
		const size_t in_start = *in_pos;

		// Decode from in[] to *dict.
		const lzma_ret ret = coder->lzma.code(coder->lzma.coder,
				dict, in, in_pos, in_size);

		// Validate and update coder->compressed_size.
		const size_t in_used = *in_pos - in_start;
		if (in_used > coder->compressed_size)
			return LZMA_DATA_ERROR;

		coder->compressed_size -= in_used;

		// Return if we didn't finish the chunk, or an error occurred.
		if (ret != LZMA_STREAM_END)
			return ret;

		// The LZMA decoder must have consumed the whole chunk now.
		// We don't need to worry about uncompressed size since it
		// is checked by the LZMA decoder.
		if (coder->compressed_size != 0)
			return LZMA_DATA_ERROR;

		coder->sequence = SEQ_CONTROL;
		break;
	}

	case SEQ_COPY: {
		// Copy from input to the dictionary as is.
		// FIXME Can copy too much?
		dict_write(dict, in, in_pos, in_size, &coder->compressed_size);
		if (coder->compressed_size != 0)
			return LZMA_OK;

		coder->sequence = SEQ_CONTROL;
		break;
	}

	default:
		assert(0);
		return LZMA_PROG_ERROR;
	}

	return LZMA_OK;
}


static void
lzma2_decoder_end(lzma_coder *coder, lzma_allocator *allocator)
{
	assert(coder->lzma.end == NULL);
	lzma_free(coder->lzma.coder, allocator);

	lzma_free(coder, allocator);

	return;
}


static lzma_ret
lzma2_decoder_init(lzma_lz_decoder *lz, lzma_allocator *allocator,
		const void *options, size_t *dict_size)
{
	if (lz->coder == NULL) {
		lz->coder = lzma_alloc(sizeof(lzma_coder), allocator);
		if (lz->coder == NULL)
			return LZMA_MEM_ERROR;

		lz->code = &lzma2_decode;
		lz->end = &lzma2_decoder_end;

		lz->coder->lzma = LZMA_LZ_DECODER_INIT;
	}

	lz->coder->sequence = SEQ_CONTROL;
	lz->coder->need_properties = true;
	lz->coder->need_dictionary_reset = true;

	return lzma_lzma_decoder_create(&lz->coder->lzma,
			allocator, options, dict_size);
}


extern lzma_ret
lzma_lzma2_decoder_init(lzma_next_coder *next, lzma_allocator *allocator,
		const lzma_filter_info *filters)
{
	// LZMA2 can only be the last filter in the chain. This is enforced
	// by the raw_decoder initialization.
	assert(filters[1].init == NULL);

	return lzma_lz_decoder_init(next, allocator, filters,
			&lzma2_decoder_init);
}


extern uint64_t
lzma_lzma2_decoder_memusage(const void *options)
{
	const uint64_t lzma_memusage = lzma_lzma_decoder_memusage(options);
	if (lzma_memusage == UINT64_MAX)
		return UINT64_MAX;

	return sizeof(lzma_coder) + lzma_memusage;
}


extern lzma_ret
lzma_lzma2_props_decode(void **options, lzma_allocator *allocator,
		const uint8_t *props, size_t props_size)
{
	if (props_size != 1)
		return LZMA_OPTIONS_ERROR;

	// Check that reserved bits are unset.
	if (props[0] & 0xC0)
		return LZMA_OPTIONS_ERROR;

	// Decode the dictionary size.
	if (props[0] > 40)
		return LZMA_OPTIONS_ERROR;

	lzma_options_lzma *opt = lzma_alloc(
			sizeof(lzma_options_lzma), allocator);
	if (opt == NULL)
		return LZMA_MEM_ERROR;

	if (props[0] == 40) {
		opt->dictionary_size = UINT32_MAX;
	} else {
		opt->dictionary_size = 2 | (props[0] & 1);
		opt->dictionary_size <<= props[0] / 2 + 11;
	}

	opt->preset_dictionary = NULL;
	opt->preset_dictionary_size = 0;

	*options = opt;

	return LZMA_OK;
}