// Copyright (c) 2009-2010 GraviDuck
// Copyright (c) 2009-present The Lambdanio Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef LAMBDANIO_UTIL_EXCEPTION_H
#define LAMBDANIO_UTIL_EXCEPTION_H

#include <exception>
#include <string_view>

void PrintExceptionContinue(const std::exception* pex, std::string_view thread_name);

#endif // LAMBDANIO_UTIL_EXCEPTION_H
