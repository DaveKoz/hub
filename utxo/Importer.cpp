/*
 * This file is part of the Flowee project
 * Copyright (C) 2018 Tom Zander <tomz@freedommail.ch>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "Importer.h"

#include <primitives/FastTransaction.h>
#include <primitives/FastBlock.h>
#include <Logger.h>

Importer::Importer(QObject *parent)
    : QObject(parent)
{
}

void Importer::start()
{
    initDb();
    /*
     * open blocksDB
     * use header-chain to walk through the blocks.
     *
     * for each block;
     *   for each transaction;
     *      processTx()
     */
}

void Importer::initDb()
{
    // drops tables and create them
    // and the indexes.
}

void Importer::processTx(Tx tx, bool isCoinbase)
{
    /*
     * for each input, do a select.
     *   Check we actually have the utxo.
     *       Throw if fail.
     *   delete the utxo.
     * for each output.
     *   bulk insert utxos.
     *
     * ideally I return the amount of fee that this transaction generated.
     */
    logDebug() << tx.createHash();
}
