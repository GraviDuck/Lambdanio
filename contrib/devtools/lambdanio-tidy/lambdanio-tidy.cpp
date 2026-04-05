// Copyright (c) 2023 Lambdanio Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nontrivial-threadlocal.h"

#include <clang-tidy/ClangTidyModule.h>

class LambdanioModule final : public clang::tidy::ClangTidyModule
{
public:
    void addCheckFactories(clang::tidy::ClangTidyCheckFactories& CheckFactories) override
    {
        CheckFactories.registerCheck<lambdanio::NonTrivialThreadLocal>("lambdanio-nontrivial-threadlocal");
    }
};

static clang::tidy::ClangTidyModuleRegistry::Add<LambdanioModule>
    X("lambdanio-module", "Adds lambdanio checks.");

volatile int LambdanioModuleAnchorSource = 0;
