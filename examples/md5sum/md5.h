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
#pragma once

#include <l4/sys/types.h>

#include <string>
#include <cstring>

class Md5_hash
{
  l4_uint32_t _state[4];
  l4_uint64_t _count;
  unsigned char _buffer[64];

public:
  Md5_hash() { init_state(); }

  void update(unsigned char const *input, l4_umword_t len);
  std::string get();

  std::string string_to_md5(std::string const &input)
  {
    init_state();
    update(reinterpret_cast<unsigned char const *>(input.c_str()),
           input.size());
    return get();
  }

private:
  void init_state()
  {
    _count = 0;
    _state[0] = 0x67452301;
    _state[1] = 0xefcdab89;
    _state[2] = 0x98badcfe;
    _state[3] = 0x10325476;
    memset(_buffer, 0, sizeof(_buffer));
  }

  void md5transform(unsigned char const block[64]);

  l4_uint32_t f(l4_uint32_t x, l4_uint32_t y, l4_uint32_t z)
  {
    return z ^ (x & (y ^ z));
  }

  l4_uint32_t g(l4_uint32_t x, l4_uint32_t y, l4_uint32_t z)
  {
    return y ^ (z & (x ^ y));
  }

  l4_uint32_t h(l4_uint32_t x, l4_uint32_t y, l4_uint32_t z)
  {
    return x ^ y ^ z;
  }

  l4_uint32_t i(l4_uint32_t x, l4_uint32_t y, l4_uint32_t z)
  {
    return y ^ (x | ~z);
  }

  l4_uint32_t rotate_left(l4_uint32_t x, unsigned n)
  {
    return (x << n) | (x >> (32 - n));
  }

  l4_uint32_t ff(l4_uint32_t a, l4_uint32_t b, l4_uint32_t c,
                 l4_uint32_t d, l4_uint32_t x, unsigned s,
                 l4_uint32_t ac)
  {
    l4_uint32_t im;
    im = a + f(b, c, d) + x + ac;
    im = rotate_left(im, s);
    return im + b;

  }

  l4_uint32_t gg(l4_uint32_t a, l4_uint32_t b, l4_uint32_t c,
                 l4_uint32_t d, l4_uint32_t x, unsigned s,
                 l4_uint32_t ac)
  {
    l4_uint32_t im;
    im = a + g(b, c, d) + x + ac;
    im = rotate_left(im, s);
    return im + b;
  }


  l4_uint32_t hh(l4_uint32_t a, l4_uint32_t b, l4_uint32_t c,
                 l4_uint32_t d, l4_uint32_t x, unsigned s,
                 l4_uint32_t ac)
  {
    l4_uint32_t im;
    im = a + h(b, c, d) + x + ac;
    im = rotate_left(im, s);
    return im + b;
  }


  l4_uint32_t ii(l4_uint32_t a, l4_uint32_t b, l4_uint32_t c,
                 l4_uint32_t d, l4_uint32_t x, unsigned s,
                 l4_uint32_t ac)
  {
    l4_uint32_t im;
    im = a + i(b, c, d) + x + ac;
    im = rotate_left(im, s);
    return im + b;
  }
};
