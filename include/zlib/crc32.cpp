/* crc32.c -- compute the CRC-32 of a data stream
 * Copyright (C) 1995-2022 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * This interleaved implementation of a CRC makes use of pipelined multiple
 * arithmetic-logic units, commonly found in modern CPU cores. It is due to
 * Kadatch and Jenkins (2010). See doc/crc-doc.1.0.pdf in this distribution.
 */

 /* @(#) $Id$ */

 /*
   Note on the use of DYNAMIC_CRC_TABLE: there is no mutex or semaphore
   protection on the static variables used to control the first-use generation
   of the crc tables. Therefore, if you #define DYNAMIC_CRC_TABLE, you should
   first call get_crc_table() to initialize the tables before allowing more than
   one thread to use crc32().

   MAKECRCH can be #defined to write out crc32.h. A main() routine is also
   produced, so that this one source file can be compiled to an executable.
  */

#ifdef MAKECRCH
#  include <stdio.h>
#  ifndef DYNAMIC_CRC_TABLE
#    define DYNAMIC_CRC_TABLE
#  endif /* !DYNAMIC_CRC_TABLE */
#endif /* MAKECRCH */

#include "zutil.hpp"      /* for Z_U4, Z_U8, z_crc_t, and FAR definitions */

  /*
   A CRC of a message is computed on N braids of words in the message, where
   each word consists of W bytes (4 or 8). If N is 3, for example, then three
   running sparse CRCs are calculated respectively on each braid, at these
   indices in the array of words: 0, 3, 6, ..., 1, 4, 7, ..., and 2, 5, 8, ...
   This is done starting at a word boundary, and continues until as many blocks
   of N * W bytes as are available have been processed. The results are combined
   into a single CRC at the end. For this code, N must be in the range 1..6 and
   W must be 4 or 8. The upper limit on N can be increased if desired by adding
   more #if blocks, extending the patterns apparent in the code. In addition,
   crc32.h would need to be regenerated, if the maximum N value is increased.

   N and W are chosen empirically by benchmarking the execution time on a given
   processor. The choices for N and W below were based on testing on Intel Kaby
   Lake i7, AMD Ryzen 7, ARM Cortex-A57, Sparc64-VII, PowerPC POWER9, and MIPS64
   Octeon II processors. The Intel, AMD, and ARM processors were all fastest
   with N=5, W=8. The Sparc, PowerPC, and MIPS64 were all fastest at N=5, W=4.
   They were all tested with either gcc or clang, all using the -O3 optimization
   level. Your mileage may vary.
  */

  /* Define N */
#ifdef Z_TESTN
#  define N Z_TESTN
#else
#  define N 5
#endif
#if N < 1 || N > 6
#  error N must be in 1..6
#endif

/*
  z_crc_t must be at least 32 bits. z_word_t must be at least as long as
  z_crc_t. It is assumed here that z_word_t is either 32 bits or 64 bits, and
  that bytes are eight bits.
 */

 /*
   Define W and the associated z_word_t type. If W is not defined, then a
   braided calculation is not used, and the associated tables and code are not
   compiled.
  */
#ifdef Z_TESTW
#  if Z_TESTW-1 != -1
#    define W Z_TESTW
#  endif
#else
#  ifdef MAKECRCH
#    define W 8         /* required for MAKECRCH */
#  else
#    if defined(__x86_64__) || defined(__aarch64__)
#      define W 8
#    else
#      define W 4
#    endif
#  endif
#endif
#ifdef W
#  if W == 8 && defined(Z_U8)
typedef Z_U8 z_word_t;
#  elif defined(Z_U4)
#    undef W
#    define W 4
typedef Z_U4 z_word_t;
#  else
#    undef W
#  endif
#endif

  /* If available, use the ARM processor CRC32 instruction. */
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32) && W == 8
#  define ARMCRC32
#endif

#if defined(W) && (!defined(ARMCRC32) || defined(DYNAMIC_CRC_TABLE))
/*
  Swap the bytes in a z_word_t to convert between little and big endian. Any
  self-respecting compiler will optimize this to a single machine byte-swap
  instruction, if one is available. This assumes that word_t is either 32 bits
  or 64 bits.
 */
local z_word_t byte_swap(z_word_t word) {
#  if W == 8
	return
		(word & 0xff00000000000000) >> 56 |
		(word & 0xff000000000000) >> 40 |
		(word & 0xff0000000000) >> 24 |
		(word & 0xff00000000) >> 8 |
		(word & 0xff000000) << 8 |
		(word & 0xff0000) << 24 |
		(word & 0xff00) << 40 |
		(word & 0xff) << 56;
#  else   /* W == 4 */
	return
		(word & 0xff000000) >> 24 |
		(word & 0xff0000) >> 8 |
		(word & 0xff00) << 8 |
		(word & 0xff) << 24;
#  endif
}
#endif

#ifdef DYNAMIC_CRC_TABLE
/* =========================================================================
 * Table of powers of x for combining CRC-32s, filled in by make_crc_table()
 * below.
 */
local z_crc_t FAR x2n_table[32];
#else
/* =========================================================================
 * Tables for byte-wise and braided CRC-32 calculations, and a table of powers
 * of x for combining CRC-32s, all made by make_crc_table().
 */
#  include "crc32.hpp"
#endif

 /* CRC polynomial. */
#define POLY 0xedb88320         /* p(x) reflected, with x^32 implied */

/*
  Return a(x) multiplied by b(x) modulo p(x), where p(x) is the CRC polynomial,
  reflected. For speed, this requires that a not be zero.
 */
local z_crc_t multmodp(z_crc_t a, z_crc_t b) {
	z_crc_t m, p;

	m = (z_crc_t)1 << 31;
	p = 0;
	for (;;) {
		if (a & m) {
			p ^= b;
			if ((a & (m - 1)) == 0)
				break;
		}
		m >>= 1;
		b = b & 1 ? (b >> 1) ^ POLY : b >> 1;
	}
	return p;
}

/*
  Return x^(n * 2^k) modulo p(x). Requires that x2n_table[] has been
  initialized.
 */
local z_crc_t x2nmodp(z_off64_t n, unsigned k) {
	z_crc_t p;

	p = (z_crc_t)1 << 31;           /* x^0 == 1 */
	while (n) {
		if (n & 1)
			p = multmodp(x2n_table[k & 31], p);
		n >>= 1;
		k++;
	}
	return p;
}

/* =========================================================================
 * This function can be used by asm versions of crc32(), and to force the
 * generation of the CRC tables in a threaded application.
 */
const z_crc_t FAR*  get_crc_table(void) {
#ifdef DYNAMIC_CRC_TABLE
	once(&made, make_crc_table);
#endif /* DYNAMIC_CRC_TABLE */
	return (const z_crc_t FAR*)crc_table;
}

#ifdef W

 /*
   Return the CRC of the W bytes in the word_t data, taking the
   least-significant byte of the word as the first byte of data, without any pre
   or post conditioning. This is used to combine the CRCs of each braid.
  */
local z_crc_t crc_word(z_word_t data) {
	int k;
	for (k = 0; k < W; k++)
		data = (data >> 8) ^ crc_table[data & 0xff];
	return (z_crc_t)data;
}

local z_word_t crc_word_big(z_word_t data) {
	int k;
	for (k = 0; k < W; k++)
		data = (data << 8) ^
		crc_big_table[(data >> ((W - 1) << 3)) & 0xff];
	return data;
}

#endif

/* ========================================================================= */
unsigned long  crc32_z(unsigned long crc, const unsigned char FAR* buf,
	z_size_t len) {
	/* Return initial CRC, if requested. */
	if (buf == Z_NULL) return 0;

#ifdef DYNAMIC_CRC_TABLE
	once(&made, make_crc_table);
#endif /* DYNAMIC_CRC_TABLE */

	/* Pre-condition the CRC */
	crc = (~crc) & 0xffffffff;

#ifdef W

	/* If provided enough bytes, do a braided CRC calculation. */
	if (len >= N * W + W - 1) {
		z_size_t blks;
		z_word_t const* words;
		unsigned endian;
		int k;

		/* Compute the CRC up to a z_word_t boundary. */
		while (len && ((z_size_t)buf & (W - 1)) != 0) {
			len--;
			crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
		}

		/* Compute the CRC on as many N z_word_t blocks as are available. */
		blks = len / (N * W);
		len -= blks * N * W;
		words = (z_word_t const*)buf;

		/* Do endian check at execution time instead of compile time, since ARM
		   processors can change the endianness at execution time. If the
		   compiler knows what the endianness will be, it can optimize out the
		   check and the unused branch. */
		endian = 1;
		if (*(unsigned char*)&endian) {
			/* Little endian. */

			z_crc_t crc0;
			z_word_t word0;
#if N > 1
			z_crc_t crc1;
			z_word_t word1;
#if N > 2
			z_crc_t crc2;
			z_word_t word2;
#if N > 3
			z_crc_t crc3;
			z_word_t word3;
#if N > 4
			z_crc_t crc4;
			z_word_t word4;
#if N > 5
			z_crc_t crc5;
			z_word_t word5;
#endif
#endif
#endif
#endif
#endif

			/* Initialize the CRC for each braid. */
			crc0 = crc;
#if N > 1
			crc1 = 0;
#if N > 2
			crc2 = 0;
#if N > 3
			crc3 = 0;
#if N > 4
			crc4 = 0;
#if N > 5
			crc5 = 0;
#endif
#endif
#endif
#endif
#endif

			/*
			  Process the first blks-1 blocks, computing the CRCs on each braid
			  independently.
			 */
			while (--blks) {
				/* Load the word for each braid into registers. */
				word0 = crc0 ^ words[0];
#if N > 1
				word1 = crc1 ^ words[1];
#if N > 2
				word2 = crc2 ^ words[2];
#if N > 3
				word3 = crc3 ^ words[3];
#if N > 4
				word4 = crc4 ^ words[4];
#if N > 5
				word5 = crc5 ^ words[5];
#endif
#endif
#endif
#endif
#endif
				words += N;

				/* Compute and update the CRC for each word. The loop should
				   get unrolled. */
				crc0 = crc_braid_table[0][word0 & 0xff];
#if N > 1
				crc1 = crc_braid_table[0][word1 & 0xff];
#if N > 2
				crc2 = crc_braid_table[0][word2 & 0xff];
#if N > 3
				crc3 = crc_braid_table[0][word3 & 0xff];
#if N > 4
				crc4 = crc_braid_table[0][word4 & 0xff];
#if N > 5
				crc5 = crc_braid_table[0][word5 & 0xff];
#endif
#endif
#endif
#endif
#endif
				for (k = 1; k < W; k++) {
					crc0 ^= crc_braid_table[k][(word0 >> (k << 3)) & 0xff];
#if N > 1
					crc1 ^= crc_braid_table[k][(word1 >> (k << 3)) & 0xff];
#if N > 2
					crc2 ^= crc_braid_table[k][(word2 >> (k << 3)) & 0xff];
#if N > 3
					crc3 ^= crc_braid_table[k][(word3 >> (k << 3)) & 0xff];
#if N > 4
					crc4 ^= crc_braid_table[k][(word4 >> (k << 3)) & 0xff];
#if N > 5
					crc5 ^= crc_braid_table[k][(word5 >> (k << 3)) & 0xff];
#endif
#endif
#endif
#endif
#endif
				}
			}

			/*
			  Process the last block, combining the CRCs of the N braids at the
			  same time.
			 */
			crc = crc_word(crc0 ^ words[0]);
#if N > 1
			crc = crc_word(crc1 ^ words[1] ^ crc);
#if N > 2
			crc = crc_word(crc2 ^ words[2] ^ crc);
#if N > 3
			crc = crc_word(crc3 ^ words[3] ^ crc);
#if N > 4
			crc = crc_word(crc4 ^ words[4] ^ crc);
#if N > 5
			crc = crc_word(crc5 ^ words[5] ^ crc);
#endif
#endif
#endif
#endif
#endif
			words += N;
		}
		else {
			/* Big endian. */

			z_word_t crc0, word0, comb;
#if N > 1
			z_word_t crc1, word1;
#if N > 2
			z_word_t crc2, word2;
#if N > 3
			z_word_t crc3, word3;
#if N > 4
			z_word_t crc4, word4;
#if N > 5
			z_word_t crc5, word5;
#endif
#endif
#endif
#endif
#endif

			/* Initialize the CRC for each braid. */
			crc0 = byte_swap(crc);
#if N > 1
			crc1 = 0;
#if N > 2
			crc2 = 0;
#if N > 3
			crc3 = 0;
#if N > 4
			crc4 = 0;
#if N > 5
			crc5 = 0;
#endif
#endif
#endif
#endif
#endif

			/*
			  Process the first blks-1 blocks, computing the CRCs on each braid
			  independently.
			 */
			while (--blks) {
				/* Load the word for each braid into registers. */
				word0 = crc0 ^ words[0];
#if N > 1
				word1 = crc1 ^ words[1];
#if N > 2
				word2 = crc2 ^ words[2];
#if N > 3
				word3 = crc3 ^ words[3];
#if N > 4
				word4 = crc4 ^ words[4];
#if N > 5
				word5 = crc5 ^ words[5];
#endif
#endif
#endif
#endif
#endif
				words += N;

				/* Compute and update the CRC for each word. The loop should
				   get unrolled. */
				crc0 = crc_braid_big_table[0][word0 & 0xff];
#if N > 1
				crc1 = crc_braid_big_table[0][word1 & 0xff];
#if N > 2
				crc2 = crc_braid_big_table[0][word2 & 0xff];
#if N > 3
				crc3 = crc_braid_big_table[0][word3 & 0xff];
#if N > 4
				crc4 = crc_braid_big_table[0][word4 & 0xff];
#if N > 5
				crc5 = crc_braid_big_table[0][word5 & 0xff];
#endif
#endif
#endif
#endif
#endif
				for (k = 1; k < W; k++) {
					crc0 ^= crc_braid_big_table[k][(word0 >> (k << 3)) & 0xff];
#if N > 1
					crc1 ^= crc_braid_big_table[k][(word1 >> (k << 3)) & 0xff];
#if N > 2
					crc2 ^= crc_braid_big_table[k][(word2 >> (k << 3)) & 0xff];
#if N > 3
					crc3 ^= crc_braid_big_table[k][(word3 >> (k << 3)) & 0xff];
#if N > 4
					crc4 ^= crc_braid_big_table[k][(word4 >> (k << 3)) & 0xff];
#if N > 5
					crc5 ^= crc_braid_big_table[k][(word5 >> (k << 3)) & 0xff];
#endif
#endif
#endif
#endif
#endif
				}
			}

			/*
			  Process the last block, combining the CRCs of the N braids at the
			  same time.
			 */
			comb = crc_word_big(crc0 ^ words[0]);
#if N > 1
			comb = crc_word_big(crc1 ^ words[1] ^ comb);
#if N > 2
			comb = crc_word_big(crc2 ^ words[2] ^ comb);
#if N > 3
			comb = crc_word_big(crc3 ^ words[3] ^ comb);
#if N > 4
			comb = crc_word_big(crc4 ^ words[4] ^ comb);
#if N > 5
			comb = crc_word_big(crc5 ^ words[5] ^ comb);
#endif
#endif
#endif
#endif
#endif
			words += N;
			crc = byte_swap(comb);
		}

		/*
		  Update the pointer to the remaining bytes to process.
		 */
		buf = (unsigned char const*)words;
	}

#endif /* W */

	/* Complete the computation of the CRC on any remaining bytes. */
	while (len >= 8) {
		len -= 8;
		crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
		crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
		crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
		crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
		crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
		crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
		crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
		crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
	}
	while (len) {
		len--;
		crc = (crc >> 8) ^ crc_table[(crc ^ *buf++) & 0xff];
	}

	/* Return the CRC, post-conditioned. */
	return crc ^ 0xffffffff;
}

/* ========================================================================= */
unsigned long  crc32(unsigned long crc, const unsigned char FAR* buf,
	uInt len) {
	return crc32_z(crc, buf, len);
}

/* ========================================================================= */
uLong  crc32_combine64(uLong crc1, uLong crc2, z_off64_t len2) {
#ifdef DYNAMIC_CRC_TABLE
	once(&made, make_crc_table);
#endif /* DYNAMIC_CRC_TABLE */
	return multmodp(x2nmodp(len2, 3), crc1) ^ (crc2 & 0xffffffff);
}

/* ========================================================================= */
uLong  crc32_combine(uLong crc1, uLong crc2, z_off_t len2) {
	return crc32_combine64(crc1, crc2, (z_off64_t)len2);
}

/* ========================================================================= */
uLong  crc32_combine_gen64(z_off64_t len2) {
#ifdef DYNAMIC_CRC_TABLE
	once(&made, make_crc_table);
#endif /* DYNAMIC_CRC_TABLE */
	return x2nmodp(len2, 3);
}

/* ========================================================================= */
uLong  crc32_combine_gen(z_off_t len2) {
	return crc32_combine_gen64((z_off64_t)len2);
}

/* ========================================================================= */
uLong  crc32_combine_op(uLong crc1, uLong crc2, uLong op) {
	return multmodp(op, crc1) ^ (crc2 & 0xffffffff);
}