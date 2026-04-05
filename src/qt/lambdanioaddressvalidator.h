// Copyright (c) 2011-present The Lambdanio Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef LAMBDANIO_QT_LAMBDANIOADDRESSVALIDATOR_H
#define LAMBDANIO_QT_LAMBDANIOADDRESSVALIDATOR_H

#include <QValidator>

/** Base58 entry widget validator, checks for valid characters and
 * removes some whitespace.
 */
class LambdanioAddressEntryValidator : public QValidator
{
    Q_OBJECT

public:
    explicit LambdanioAddressEntryValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

/** Lambdanio address widget validator, checks for a valid lambdanio address.
 */
class LambdanioAddressCheckValidator : public QValidator
{
    Q_OBJECT

public:
    explicit LambdanioAddressCheckValidator(QObject *parent);

    State validate(QString &input, int &pos) const override;
};

#endif // LAMBDANIO_QT_LAMBDANIOADDRESSVALIDATOR_H
