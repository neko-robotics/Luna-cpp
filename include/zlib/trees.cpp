#include "deflate.hpp"

#ifdef ZLIB_DEBUG
#  include <ctype.h>
#endif

#define MAX_BL_BITS 7
#define END_BLOCK 256
#define REP_3_6      16
#define REPZ_3_10    17
#define REPZ_11_138  18
local const int extra_lbits[LENGTH_CODES]
= { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };

local const int extra_dbits[D_CODES]
= { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

local const int extra_blbits[BL_CODES]
= { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,3,7 };

local const uch bl_order[BL_CODES]
= { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
#define DIST_CODE_LEN  512

#if defined(GEN_TREES_H) || !defined(STDC)
local ct_data static_ltree[L_CODES + 2];
local ct_data static_dtree[D_CODES];
uch _dist_code[DIST_CODE_LEN];
uch _length_code[MAX_MATCH - MIN_MATCH + 1];
local int base_length[LENGTH_CODES];
local int base_dist[D_CODES];
#else
#  include "trees.hpp"
#endif

struct static_tree_desc_s {
	const ct_data* static_tree;
	const intf* extra_bits;
	int     extra_base;
	int     elems;
	int     max_length;
};

#ifdef NO_INIT_GLOBAL_POINTERS
#  define TCONST
#else
#  define TCONST const
#endif

local TCONST static_tree_desc static_l_desc =
{ static_ltree, extra_lbits, LITERALS + 1, L_CODES, MAX_BITS };

local TCONST static_tree_desc static_d_desc =
{ static_dtree, extra_dbits, 0,          D_CODES, MAX_BITS };

local TCONST static_tree_desc static_bl_desc =
{ (const ct_data*)0, extra_blbits, 0,   BL_CODES, MAX_BL_BITS };

#define put_short(s, w) { \
    put_byte(s, (uch)((w) & 0xff)); \
    put_byte(s, (uch)((ush)(w) >> 8)); \
}

local unsigned bi_reverse(unsigned code, int len) {
	register unsigned res = 0;
	do {
		res |= code & 1;
		code >>= 1, res <<= 1;
	} while (--len > 0);
	return res >> 1;
}

local void bi_flush(deflate_state* s) {
	if (s->bi_valid == 16) {
		put_short(s, s->bi_buf);
		s->bi_buf = 0;
		s->bi_valid = 0;
	}
	else if (s->bi_valid >= 8) {
		put_byte(s, (Byte)s->bi_buf);
		s->bi_buf >>= 8;
		s->bi_valid -= 8;
	}
}

local void bi_windup(deflate_state* s) {
	if (s->bi_valid > 8) {
		put_short(s, s->bi_buf);
	}
	else if (s->bi_valid > 0) {
		put_byte(s, (Byte)s->bi_buf);
	}
	s->bi_buf = 0;
	s->bi_valid = 0;
#ifdef ZLIB_DEBUG
	s->bits_sent = (s->bits_sent + 7) & ~7;
#endif
}

local void gen_codes(ct_data* tree, int max_code, ushf* bl_count) {
	ush next_code[MAX_BITS + 1];
	unsigned code = 0;
	int bits;
	int n;

	for (bits = 1; bits <= MAX_BITS; bits++) {
		code = (code + bl_count[bits - 1]) << 1;
		next_code[bits] = (ush)code;
	}
	Assert(code + bl_count[MAX_BITS] - 1 == (1 << MAX_BITS) - 1,
		"inconsistent bit counts");
	Tracev((stderr, "\ngen_codes: max_code %d ", max_code));

	for (n = 0; n <= max_code; n++) {
		int len = tree[n].Len;
		if (len == 0) continue;
		tree[n].Code = (ush)bi_reverse(next_code[len]++, len);

		Tracecv(tree != static_ltree, (stderr, "\nn %3d %c l %2d c %4x (%x) ",
			n, (isgraph(n) ? n : ' '), len, tree[n].Code, next_code[len] - 1));
	}
}

#  define send_code(s, c, tree) send_bits(s, tree[c].Code, tree[c].Len)

#define send_bits(s, value, length) \
{ int len = length;\
  if (s->bi_valid > (int)Buf_size - len) {\
    int val = (int)value;\
    s->bi_buf |= (ush)val << s->bi_valid;\
    put_short(s, s->bi_buf);\
    s->bi_buf = (ush)val >> (Buf_size - s->bi_valid);\
    s->bi_valid += len - Buf_size;\
  } else {\
    s->bi_buf |= (ush)(value) << s->bi_valid;\
    s->bi_valid += len;\
  }\
}

local void init_block(deflate_state* s) {
	int n;

	for (n = 0; n < L_CODES; n++) s->dyn_ltree[n].Freq = 0;
	for (n = 0; n < D_CODES; n++) s->dyn_dtree[n].Freq = 0;
	for (n = 0; n < BL_CODES; n++) s->bl_tree[n].Freq = 0;

	s->dyn_ltree[END_BLOCK].Freq = 1;
	s->opt_len = s->static_len = 0L;
	s->sym_next = s->matches = 0;
}

void  _tr_init(deflate_state* s) {
	s->l_desc.dyn_tree = s->dyn_ltree;
	s->l_desc.stat_desc = &static_l_desc;

	s->d_desc.dyn_tree = s->dyn_dtree;
	s->d_desc.stat_desc = &static_d_desc;

	s->bl_desc.dyn_tree = s->bl_tree;
	s->bl_desc.stat_desc = &static_bl_desc;

	s->bi_buf = 0;
	s->bi_valid = 0;
#ifdef ZLIB_DEBUG
	s->compressed_len = 0L;
	s->bits_sent = 0L;
#endif

	init_block(s);
}

#define SMALLEST 1
#define pqremove(s, tree, top) \
{\
    top = s->heap[SMALLEST]; \
    s->heap[SMALLEST] = s->heap[s->heap_len--]; \
    pqdownheap(s, tree, SMALLEST); \
}

#define smaller(tree, n, m, depth) \
   (tree[n].Freq < tree[m].Freq || \
   (tree[n].Freq == tree[m].Freq && depth[n] <= depth[m]))

local void pqdownheap(deflate_state* s, ct_data* tree, int k) {
	int v = s->heap[k];
	int j = k << 1;
	while (j <= s->heap_len) {
		if (j < s->heap_len &&
			smaller(tree, s->heap[j + 1], s->heap[j], s->depth)) {
			j++;
		}
		if (smaller(tree, v, s->heap[j], s->depth)) break;

		s->heap[k] = s->heap[j];  k = j;

		j <<= 1;
	}
	s->heap[k] = v;
}

local void gen_bitlen(deflate_state* s, tree_desc* desc) {
	ct_data* tree = desc->dyn_tree;
	int max_code = desc->max_code;
	const ct_data* stree = desc->stat_desc->static_tree;
	const intf* extra = desc->stat_desc->extra_bits;
	int base = desc->stat_desc->extra_base;
	int max_length = desc->stat_desc->max_length;
	int h;
	int n, m;
	int bits;
	int xbits;
	ush f;
	int overflow = 0;

	for (bits = 0; bits <= MAX_BITS; bits++) s->bl_count[bits] = 0;

	tree[s->heap[s->heap_max]].Len = 0;

	for (h = s->heap_max + 1; h < HEAP_SIZE; h++) {
		n = s->heap[h];
		bits = tree[tree[n].Dad].Len + 1;
		if (bits > max_length) bits = max_length, overflow++;
		tree[n].Len = (ush)bits;
		if (n > max_code) continue;

		s->bl_count[bits]++;
		xbits = 0;
		if (n >= base) xbits = extra[n - base];
		f = tree[n].Freq;
		s->opt_len += (ulg)f * (unsigned)(bits + xbits);
		if (stree) s->static_len += (ulg)f * (unsigned)(stree[n].Len + xbits);
	}
	if (overflow == 0) return;

	Tracev((stderr, "\nbit length overflow\n"));
	do {
		bits = max_length - 1;
		while (s->bl_count[bits] == 0) bits--;
		s->bl_count[bits]--;
		s->bl_count[bits + 1] += 2;
		s->bl_count[max_length]--;
		overflow -= 2;
	} while (overflow > 0);

	for (bits = max_length; bits != 0; bits--) {
		n = s->bl_count[bits];
		while (n != 0) {
			m = s->heap[--h];
			if (m > max_code) continue;
			if ((unsigned)tree[m].Len != (unsigned)bits) {
				Tracev((stderr, "code %d bits %d->%d\n", m, tree[m].Len, bits));
				s->opt_len += ((ulg)bits - tree[m].Len) * tree[m].Freq;
				tree[m].Len = (ush)bits;
			}
			n--;
		}
	}
}

#ifdef DUMP_BL_TREE
#  include <stdio.h>
#endif

local void build_tree(deflate_state* s, tree_desc* desc) {
	ct_data* tree = desc->dyn_tree;
	const ct_data* stree = desc->stat_desc->static_tree;
	int elems = desc->stat_desc->elems;
	int n, m;
	int max_code = -1;
	int node;

	s->heap_len = 0, s->heap_max = HEAP_SIZE;

	for (n = 0; n < elems; n++) {
		if (tree[n].Freq != 0) {
			s->heap[++(s->heap_len)] = max_code = n;
			s->depth[n] = 0;
		}
		else {
			tree[n].Len = 0;
		}
	}

	while (s->heap_len < 2) {
		node = s->heap[++(s->heap_len)] = (max_code < 2 ? ++max_code : 0);
		tree[node].Freq = 1;
		s->depth[node] = 0;
		s->opt_len--; if (stree) s->static_len -= stree[node].Len;
	}
	desc->max_code = max_code;

	for (n = s->heap_len / 2; n >= 1; n--) pqdownheap(s, tree, n);

	node = elems;
	do {
		pqremove(s, tree, n);
		m = s->heap[SMALLEST];

		s->heap[--(s->heap_max)] = n;
		s->heap[--(s->heap_max)] = m;

		tree[node].Freq = tree[n].Freq + tree[m].Freq;
		s->depth[node] = (uch)((s->depth[n] >= s->depth[m] ?
			s->depth[n] : s->depth[m]) + 1);
		tree[n].Dad = tree[m].Dad = (ush)node;
#ifdef DUMP_BL_TREE
		if (tree == s->bl_tree) {
			fprintf(stderr, "\nnode %d(%d), sons %d(%d) %d(%d)",
				node, tree[node].Freq, n, tree[n].Freq, m, tree[m].Freq);
		}
#endif
		s->heap[SMALLEST] = node++;
		pqdownheap(s, tree, SMALLEST);
	} while (s->heap_len >= 2);

	s->heap[--(s->heap_max)] = s->heap[SMALLEST];

	gen_bitlen(s, (tree_desc*)desc);

	gen_codes((ct_data*)tree, max_code, s->bl_count);
}

local void scan_tree(deflate_state* s, ct_data* tree, int max_code) {
	int n;
	int prevlen = -1;
	int curlen;
	int nextlen = tree[0].Len;
	int count = 0;
	int max_count = 7;
	int min_count = 4;

	if (nextlen == 0) max_count = 138, min_count = 3;
	tree[max_code + 1].Len = (ush)0xffff;

	for (n = 0; n <= max_code; n++) {
		curlen = nextlen; nextlen = tree[n + 1].Len;
		if (++count < max_count && curlen == nextlen) {
			continue;
		}
		else if (count < min_count) {
			s->bl_tree[curlen].Freq += count;
		}
		else if (curlen != 0) {
			if (curlen != prevlen) s->bl_tree[curlen].Freq++;
			s->bl_tree[REP_3_6].Freq++;
		}
		else if (count <= 10) {
			s->bl_tree[REPZ_3_10].Freq++;
		}
		else {
			s->bl_tree[REPZ_11_138].Freq++;
		}
		count = 0; prevlen = curlen;
		if (nextlen == 0) {
			max_count = 138, min_count = 3;
		}
		else if (curlen == nextlen) {
			max_count = 6, min_count = 3;
		}
		else {
			max_count = 7, min_count = 4;
		}
	}
}

local void send_tree(deflate_state* s, ct_data* tree, int max_code) {
	int n;
	int prevlen = -1;
	int curlen;
	int nextlen = tree[0].Len;
	int count = 0;
	int max_count = 7;
	int min_count = 4;

	if (nextlen == 0) max_count = 138, min_count = 3;

	for (n = 0; n <= max_code; n++) {
		curlen = nextlen; nextlen = tree[n + 1].Len;
		if (++count < max_count && curlen == nextlen) {
			continue;
		}
		else if (count < min_count) {
			do { send_code(s, curlen, s->bl_tree); } while (--count != 0);
		}
		else if (curlen != 0) {
			if (curlen != prevlen) {
				send_code(s, curlen, s->bl_tree); count--;
			}
			Assert(count >= 3 && count <= 6, " 3_6?");
			send_code(s, REP_3_6, s->bl_tree); send_bits(s, count - 3, 2);
		}
		else if (count <= 10) {
			send_code(s, REPZ_3_10, s->bl_tree); send_bits(s, count - 3, 3);
		}
		else {
			send_code(s, REPZ_11_138, s->bl_tree); send_bits(s, count - 11, 7);
		}
		count = 0; prevlen = curlen;
		if (nextlen == 0) {
			max_count = 138, min_count = 3;
		}
		else if (curlen == nextlen) {
			max_count = 6, min_count = 3;
		}
		else {
			max_count = 7, min_count = 4;
		}
	}
}

local int build_bl_tree(deflate_state* s) {
	int max_blindex;

	scan_tree(s, (ct_data*)s->dyn_ltree, s->l_desc.max_code);
	scan_tree(s, (ct_data*)s->dyn_dtree, s->d_desc.max_code);

	build_tree(s, (tree_desc*)(&(s->bl_desc)));
	for (max_blindex = BL_CODES - 1; max_blindex >= 3; max_blindex--) {
		if (s->bl_tree[bl_order[max_blindex]].Len != 0) break;
	}
	s->opt_len += 3 * ((ulg)max_blindex + 1) + 5 + 5 + 4;
	Tracev((stderr, "\ndyn trees: dyn %lu, stat %lu",
		s->opt_len, s->static_len));

	return max_blindex;
}

local void send_all_trees(deflate_state* s, int lcodes, int dcodes,
	int blcodes) {
	int rank;

	Assert(lcodes >= 257 && dcodes >= 1 && blcodes >= 4, "not enough codes");
	Assert(lcodes <= L_CODES && dcodes <= D_CODES && blcodes <= BL_CODES,
		"too many codes");
	Tracev((stderr, "\nbl counts: "));
	send_bits(s, lcodes - 257, 5);
	send_bits(s, dcodes - 1, 5);
	send_bits(s, blcodes - 4, 4);
	for (rank = 0; rank < blcodes; rank++) {
		Tracev((stderr, "\nbl code %2d ", bl_order[rank]));
		send_bits(s, s->bl_tree[bl_order[rank]].Len, 3);
	}
	send_tree(s, (ct_data*)s->dyn_ltree, lcodes - 1);
	send_tree(s, (ct_data*)s->dyn_dtree, dcodes - 1);
}

void  _tr_stored_block(deflate_state* s, charf* buf,
	ulg stored_len, int last) {
	send_bits(s, (STORED_BLOCK << 1) + last, 3);
	bi_windup(s);
	put_short(s, (ush)stored_len);
	put_short(s, (ush)~stored_len);
	if (stored_len)
		zmemcpy(s->pending_buf + s->pending, (Bytef*)buf, stored_len);
	s->pending += stored_len;
}

void  _tr_flush_bits(deflate_state* s) {
	bi_flush(s);
}

void  _tr_align(deflate_state* s) {
	send_bits(s, STATIC_TREES << 1, 3);
	send_code(s, END_BLOCK, static_ltree);
#ifdef ZLIB_DEBUG
	s->compressed_len += 10L;
#endif
	bi_flush(s);
}

local void compress_block(deflate_state* s, const ct_data* ltree,
	const ct_data* dtree) {
	unsigned dist;
	int lc;
	unsigned sx = 0;
	unsigned code;
	int extra;

	if (s->sym_next != 0) do {
		dist = s->sym_buf[sx++] & 0xff;
		dist += (unsigned)(s->sym_buf[sx++] & 0xff) << 8;
		lc = s->sym_buf[sx++];
		if (dist == 0) {
			send_code(s, lc, ltree);
			Tracecv(isgraph(lc), (stderr, " '%c' ", lc));
		}
		else {
			code = _length_code[lc];
			send_code(s, code + static_cast<unsigned>(LITERALS) + static_cast<unsigned>(1), ltree);
			extra = extra_lbits[code];
			if (extra != 0) {
				lc -= base_length[code];
				send_bits(s, lc, extra);
			}
			dist--;
			code = d_code(dist);
			Assert(code < D_CODES, "bad d_code");

			send_code(s, code, dtree);
			extra = extra_dbits[code];
			if (extra != 0) {
				dist -= (unsigned)base_dist[code];
				send_bits(s, dist, extra);
			}
		}

		Assert(s->pending < s->lit_bufsize + sx, "pendingBuf overflow");
	} while (sx < s->sym_next);

	send_code(s, END_BLOCK, ltree);
}

local int detect_data_type(deflate_state* s) {
	unsigned long block_mask = 0xf3ffc07fUL;
	int n;

	for (n = 0; n <= 31; n++, block_mask >>= 1)
		if ((block_mask & 1) && (s->dyn_ltree[n].Freq != 0))
			return Z_BINARY;

	if (s->dyn_ltree[9].Freq != 0 || s->dyn_ltree[10].Freq != 0
		|| s->dyn_ltree[13].Freq != 0)
		return Z_TEXT;
	for (n = 32; n < LITERALS; n++)
		if (s->dyn_ltree[n].Freq != 0)
			return Z_TEXT;

	return Z_BINARY;
}

void  _tr_flush_block(deflate_state* s, charf* buf,
	ulg stored_len, int last) {
	ulg opt_lenb, static_lenb;
	int max_blindex = 0;

	if (s->level > 0) {
		if (s->strm->data_type == Z_UNKNOWN)
			s->strm->data_type = detect_data_type(s);

		build_tree(s, (tree_desc*)(&(s->l_desc)));
		Tracev((stderr, "\nlit data: dyn %lu, stat %lu", s->opt_len,
			s->static_len));

		build_tree(s, (tree_desc*)(&(s->d_desc)));
		Tracev((stderr, "\ndist data: dyn %lu, stat %lu", s->opt_len,
			s->static_len));
		max_blindex = build_bl_tree(s);

		opt_lenb = (s->opt_len + 3 + 7) >> 3;
		static_lenb = (s->static_len + 3 + 7) >> 3;

		Tracev((stderr, "\nopt %lu(%lu) stat %lu(%lu) stored %lu lit %u ",
			opt_lenb, s->opt_len, static_lenb, s->static_len, stored_len,
			s->sym_next / 3));

#ifndef FORCE_STATIC
		if (static_lenb <= opt_lenb || s->strategy == Z_FIXED)
#endif
			opt_lenb = static_lenb;
	}
	else {
		Assert(buf != (char*)0, "lost buf");
		opt_lenb = static_lenb = stored_len + 5;
	}

#ifdef FORCE_STORED
	if (buf != (char*)0) {
#else
	if (stored_len + 4 <= opt_lenb && buf != (char*)0) {
#endif
		_tr_stored_block(s, buf, stored_len, last);
	}
	else if (static_lenb == opt_lenb) {
		send_bits(s, (STATIC_TREES << 1) + last, 3);
		compress_block(s, (const ct_data*)static_ltree,
			(const ct_data*)static_dtree);
#ifdef ZLIB_DEBUG
		s->compressed_len += 3 + s->static_len;
#endif
	}
	else {
		send_bits(s, (DYN_TREES << 1) + last, 3);
		send_all_trees(s, s->l_desc.max_code + 1, s->d_desc.max_code + 1,
			max_blindex + 1);
		compress_block(s, (const ct_data*)s->dyn_ltree,
			(const ct_data*)s->dyn_dtree);
#ifdef ZLIB_DEBUG
		s->compressed_len += 3 + s->opt_len;
#endif
	}
	Assert(s->compressed_len == s->bits_sent, "bad compressed size");
	init_block(s);

	if (last) {
		bi_windup(s);
#ifdef ZLIB_DEBUG
		s->compressed_len += 7;
#endif
	}
	Tracev((stderr, "\ncomprlen %lu(%lu) ", s->compressed_len >> 3,
		s->compressed_len - 7 * last));
}

int  _tr_tally(deflate_state * s, unsigned dist, unsigned lc) {
	s->sym_buf[s->sym_next++] = (uch)dist;
	s->sym_buf[s->sym_next++] = (uch)(dist >> 8);
	s->sym_buf[s->sym_next++] = (uch)lc;
	if (dist == 0) {
		s->dyn_ltree[lc].Freq++;
	}
	else {
		s->matches++;
		dist--;
		Assert((ush)dist < (ush)MAX_DIST(s) &&
			(ush)lc <= (ush)(MAX_MATCH - MIN_MATCH) &&
			(ush)d_code(dist) < (ush)D_CODES, "_tr_tally: bad match");

		s->dyn_ltree[_length_code[lc] + LITERALS + 1].Freq++;
		s->dyn_dtree[d_code(dist)].Freq++;
	}
	return (s->sym_next == s->sym_end);
}