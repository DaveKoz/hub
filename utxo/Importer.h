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
#ifndef UTXO_IMPORTER_H
#define UTXO_IMPORTER_H

#include <QObject>
#include <QSqlDatabase>

class Tx;
class FastBlock;
class CBlockIndex;

class Importer : public QObject
{
    Q_OBJECT
public:
    Importer(QObject *parent = nullptr);


public slots:
    void start();

private:
    bool initDb();
    bool createTables();
    void parseBlock(const CBlockIndex *index, FastBlock block);

    void processTx(const CBlockIndex *index, Tx tx, bool isCoinbase, int offsetInBlock);

    QSqlDatabase m_db;

    QAtomicInt m_selects;
    QAtomicInt m_inserts;
    QAtomicInt m_deletes;
    QAtomicInt m_parse;
    QAtomicInteger<qint64> m_txCount;
};

#endif
