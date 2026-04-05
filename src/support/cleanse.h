// Copyright (c) 2009-2010 GraviDuck
// Copyright (c) 2009-present The Lambdanio Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef LAMBDANIO_SUPPORT_CLEANSE_H
#define LAMBDANIO_SUPPORT_CLEANSE_H

#include <cstdlib>

/** Secure overwrite a buffer (possibly containing secret data) with zero-bytes. The write
 * operation will not be optimized out by the compiler. */
void memory_cleanse(void *ptr, size_t len);

#endif // LAMBDANIO_SUPPORT_CLEANSE_H
