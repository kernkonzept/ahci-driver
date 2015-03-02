/*
 * (c) 2014 Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * Based on reference implementation in RFC 1321:
 *
 * Copyright (C) 1991-2, RSA Data Security, Inc. Created 1991. All
 * rights reserved.
 *
 * License to copy and use this software is granted provided that it
 * is identified as the "RSA Data Security, Inc. MD5 Message-Digest
 * Algorithm" in all material mentioning or referencing this software
 * or this function.
 *
 * License is also granted to make and use derivative works provided
 * that such works are identified as "derived from the RSA Data
 * Security, Inc. MD5 Message-Digest Algorithm" in all material
 * mentioning or referencing the derived work.
 *
 * RSA Data Security, Inc. makes no representations concerning either
 * the merchantability of this software or the suitability of this
 * software for any particular purpose. It is provided "as is"
 * without express or implied warranty of any kind.
 *
 * These notices must be retained in any copies of any part of this
 * documentation and/or software.
 */

#include "md5.h"

enum
{
  S11 = 7,
  S12 = 12,
  S13 = 17,
  S14 = 22,
  S21 = 5,
  S22 = 9,
  S23 = 14,
  S24 = 20,
  S31 = 4,
  S32 = 11,
  S33 = 16,
  S34 = 23,
  S41 = 6,
  S42 = 10,
  S43 = 15,
  S44 = 21,
};

static unsigned char const _padding[64] =
  {0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  };

void Md5_hash::update(unsigned char const *input, l4_umword_t len)
{
  unsigned index = (_count >> 3) & 0x3F;
  unsigned partlen = 64 - index;
  unsigned i;

  _count += (l4_uint64_t) len << 3; // modulo 2^64 expected

  if (len >= partlen)
    {
      memcpy(&_buffer[index], input, partlen);
      md5transform(_buffer);

      for (i = partlen; i + 63 < len; i += 64)
        md5transform(&input[i]);

      index = 0;
    }
  else
    i = 0;

  memcpy(&_buffer[index], &input[i], len - i);
}

std::string Md5_hash::get()
{
  unsigned char buf[8];
  for (unsigned i = 0; i < 8; ++i)
    buf[i] = (_count >> (8 * i)) & 0xff;

  unsigned index = (_count >> 3) & 0x3f;
  unsigned padlen = (index < 56) ? (56 - index) : (120 - index);
  update(_padding, padlen);
  update(buf, 8);

  char digest[33];
  char *pos = digest;
  for (int s = 0; s < 4; ++s)
    for (int b = 0; b < 4; ++b)
      {
        snprintf(pos, 3, "%02x",
                 (unsigned char) ((_state[s] >> (8 * b)) & 0xff));
        pos += 2;
      }

  init_state();

  return std::string(digest, 32);
}

void Md5_hash::md5transform(unsigned char const block[64])
{
  l4_uint32_t a = _state[0];
  l4_uint32_t b = _state[1];
  l4_uint32_t c = _state[2];
  l4_uint32_t d = _state[3];
  l4_uint32_t const *x = reinterpret_cast<l4_uint32_t const *>(block);

  /* Round 1 */
  a = ff(a, b, c, d, x[ 0], S11, 0xd76aa478); /* 1 */
  d = ff(d, a, b, c, x[ 1], S12, 0xe8c7b756); /* 2 */
  c = ff(c, d, a, b, x[ 2], S13, 0x242070db); /* 3 */
  b = ff(b, c, d, a, x[ 3], S14, 0xc1bdceee); /* 4 */
  a = ff(a, b, c, d, x[ 4], S11, 0xf57c0faf); /* 5 */
  d = ff(d, a, b, c, x[ 5], S12, 0x4787c62a); /* 6 */
  c = ff(c, d, a, b, x[ 6], S13, 0xa8304613); /* 7 */
  b = ff(b, c, d, a, x[ 7], S14, 0xfd469501); /* 8 */
  a = ff(a, b, c, d, x[ 8], S11, 0x698098d8); /* 9 */
  d = ff(d, a, b, c, x[ 9], S12, 0x8b44f7af); /* 10 */
  c = ff(c, d, a, b, x[10], S13, 0xffff5bb1); /* 11 */
  b = ff(b, c, d, a, x[11], S14, 0x895cd7be); /* 12 */
  a = ff(a, b, c, d, x[12], S11, 0x6b901122); /* 13 */
  d = ff(d, a, b, c, x[13], S12, 0xfd987193); /* 14 */
  c = ff(c, d, a, b, x[14], S13, 0xa679438e); /* 15 */
  b = ff(b, c, d, a, x[15], S14, 0x49b40821); /* 16 */

 /* Round 2 */
  a = gg(a, b, c, d, x[ 1], S21, 0xf61e2562); /* 17 */
  d = gg(d, a, b, c, x[ 6], S22, 0xc040b340); /* 18 */
  c = gg(c, d, a, b, x[11], S23, 0x265e5a51); /* 19 */
  b = gg(b, c, d, a, x[ 0], S24, 0xe9b6c7aa); /* 20 */
  a = gg(a, b, c, d, x[ 5], S21, 0xd62f105d); /* 21 */
  d = gg(d, a, b, c, x[10], S22,  0x2441453); /* 22 */
  c = gg(c, d, a, b, x[15], S23, 0xd8a1e681); /* 23 */
  b = gg(b, c, d, a, x[ 4], S24, 0xe7d3fbc8); /* 24 */
  a = gg(a, b, c, d, x[ 9], S21, 0x21e1cde6); /* 25 */
  d = gg(d, a, b, c, x[14], S22, 0xc33707d6); /* 26 */
  c = gg(c, d, a, b, x[ 3], S23, 0xf4d50d87); /* 27 */
  b = gg(b, c, d, a, x[ 8], S24, 0x455a14ed); /* 28 */
  a = gg(a, b, c, d, x[13], S21, 0xa9e3e905); /* 29 */
  d = gg(d, a, b, c, x[ 2], S22, 0xfcefa3f8); /* 30 */
  c = gg(c, d, a, b, x[ 7], S23, 0x676f02d9); /* 31 */
  b = gg(b, c, d, a, x[12], S24, 0x8d2a4c8a); /* 32 */

  /* Round 3 */
  a = hh(a, b, c, d, x[ 5], S31, 0xfffa3942); /* 33 */
  d = hh(d, a, b, c, x[ 8], S32, 0x8771f681); /* 34 */
  c = hh(c, d, a, b, x[11], S33, 0x6d9d6122); /* 35 */
  b = hh(b, c, d, a, x[14], S34, 0xfde5380c); /* 36 */
  a = hh(a, b, c, d, x[ 1], S31, 0xa4beea44); /* 37 */
  d = hh(d, a, b, c, x[ 4], S32, 0x4bdecfa9); /* 38 */
  c = hh(c, d, a, b, x[ 7], S33, 0xf6bb4b60); /* 39 */
  b = hh(b, c, d, a, x[10], S34, 0xbebfbc70); /* 40 */
  a = hh(a, b, c, d, x[13], S31, 0x289b7ec6); /* 41 */
  d = hh(d, a, b, c, x[ 0], S32, 0xeaa127fa); /* 42 */
  c = hh(c, d, a, b, x[ 3], S33, 0xd4ef3085); /* 43 */
  b = hh(b, c, d, a, x[ 6], S34,  0x4881d05); /* 44 */
  a = hh(a, b, c, d, x[ 9], S31, 0xd9d4d039); /* 45 */
  d = hh(d, a, b, c, x[12], S32, 0xe6db99e5); /* 46 */
  c = hh(c, d, a, b, x[15], S33, 0x1fa27cf8); /* 47 */
  b = hh(b, c, d, a, x[ 2], S34, 0xc4ac5665); /* 48 */

  /* Round 4 */
  a = ii(a, b, c, d, x[ 0], S41, 0xf4292244); /* 49 */
  d = ii(d, a, b, c, x[ 7], S42, 0x432aff97); /* 50 */
  c = ii(c, d, a, b, x[14], S43, 0xab9423a7); /* 51 */
  b = ii(b, c, d, a, x[ 5], S44, 0xfc93a039); /* 52 */
  a = ii(a, b, c, d, x[12], S41, 0x655b59c3); /* 53 */
  d = ii(d, a, b, c, x[ 3], S42, 0x8f0ccc92); /* 54 */
  c = ii(c, d, a, b, x[10], S43, 0xffeff47d); /* 55 */
  b = ii(b, c, d, a, x[ 1], S44, 0x85845dd1); /* 56 */
  a = ii(a, b, c, d, x[ 8], S41, 0x6fa87e4f); /* 57 */
  d = ii(d, a, b, c, x[15], S42, 0xfe2ce6e0); /* 58 */
  c = ii(c, d, a, b, x[ 6], S43, 0xa3014314); /* 59 */
  b = ii(b, c, d, a, x[13], S44, 0x4e0811a1); /* 60 */
  a = ii(a, b, c, d, x[ 4], S41, 0xf7537e82); /* 61 */
  d = ii(d, a, b, c, x[11], S42, 0xbd3af235); /* 62 */
  c = ii(c, d, a, b, x[ 2], S43, 0x2ad7d2bb); /* 63 */
  b = ii(b, c, d, a, x[ 9], S44, 0xeb86d391); /* 64 */

  _state[0] += a;
  _state[1] += b;
  _state[2] += c;
  _state[3] += d;
}
