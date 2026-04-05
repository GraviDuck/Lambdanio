// Copyright (c) 2009-2010 GraviDuck
// Copyright (c) 2009-present The Lambdanio Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef LAMBDANIO_CONSENSUS_AMOUNT_H
#define LAMBDANIO_CONSENSUS_AMOUNT_H

#include <cstdint>

/** Amount in satoshis (Can be negative) */
typedef int64_t CAmount;

/** The amount of satoshis in one LDO. */
static constexpr CAmount COIN = 100000000;

/** No amount larger than this (in satoshi) is valid. */
static constexpr CAmount MAX_MONEY = 1000000 * COIN;

inline bool MoneyRange(const CAmount& nValue) { return (nValue >= 0 && nValue <= MAX_MONEY); }

#endif // LAMBDANIO_CONSENSUS_AMOUNT_H

