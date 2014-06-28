/*
 * ghooklist.c: API for manipulating a list of hook functions
 *
 * Copyright (C) 2014 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Robert Bragg <robert@linux.intel.com>
 */

#include <config.h>

#include <clib.h>

static c_hash_table_t *_quark_hash_table;
static uint32_t _next_quark;

c_quark_t
c_quark_from_static_string(const char *string)
{
    void *quark_ptr;

    if (C_UNLIKELY(_quark_hash_table == NULL)) {
        _quark_hash_table = c_hash_table_new(c_str_hash, c_str_equal);
        _next_quark++;
    }

    quark_ptr = c_hash_table_lookup(_quark_hash_table, string);
    if (!quark_ptr) {
        c_quark_t new_quark = _next_quark++;
        c_hash_table_insert(
            _quark_hash_table, (void *)string, C_UINT_TO_POINTER(new_quark));
        return new_quark;
    } else
        return C_POINTER_TO_UINT(quark_ptr);
}
