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
#include <BlocksDB.h>

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QCoreApplication>
#include <chain.h>
#include <chainparamsbase.h>
#include <QVariant>
#include <QDebug>
#include <QTime>

namespace {
quint64 longFromBytes(const Streaming::ConstBuffer &buf) {
    assert(buf.size() >= 8);
    // this is fine as long as you don't change your endiannes accessing the same DB
    const quint64 *answer = reinterpret_cast<const quint64*>(buf.begin());
    return answer[0] >> 1;
}
quint64 longFromHash(const uint256 &sha) {
    const quint64 *answer = reinterpret_cast<const quint64*>(sha.begin());
    return answer[0] >> 1;
}
}

Importer::Importer(QObject *parent)
    : QObject(parent)
{

}

void Importer::start()
{
    logInfo() << "Init DB";
    if (!initDb()) {
        QCoreApplication::exit(1);
        return;
    }

    try {
        SelectBaseParams(CBaseChainParams::MAIN);
        Blocks::DB::createInstance(1000, false);
        logInfo() << "Reading blocksDB";
        Blocks::DB::instance()->CacheAllBlockInfos();
        logInfo() << "Finding blocks...";
        QTime time;
        time.start();
        const auto &chain = Blocks::DB::instance()->headerChain();
        CBlockIndex *index = chain.Genesis();
        if (index == nullptr) {
            logCritical() << "No blocks in DB. Not even genesis block!";
            QCoreApplication::exit(2);
            return;
        }
        int nextStop = 10000;
        int lastHeight = -1;
        while(true) {
            index = chain.Next(index); // we skip genesis, its not part of the utxo
            if (! index)
                break;
            lastHeight = index->nHeight;
            parseBlock(index, Blocks::DB::instance()->loadBlock(index->GetBlockPos()));

            if (m_txCount.load() > nextStop) {
                nextStop = m_txCount.load() + 25000;
                logCritical() << "Finished blocks 0..." << index->nHeight << ", tx count:" << m_txCount.load();
                logCritical() << "  parseBlocks" << m_parse.load() << "ms";
                logCritical() << "       select" << m_selects.load() << "ms";
                logCritical() << "       delete" << m_deletes.load() << "ms";
                logCritical() << "       insert" << m_inserts.load() << "ms";
                logCritical() << "   Wall-clock" << time.elapsed() << "ms";
            }
            if (index->nHeight >= 125000) // thats all for now
                break;
        }
        logCritical() << "Finished with block at height:" << lastHeight;
    } catch (const std::exception &e) {
        logFatal() << e;
        QCoreApplication::exit(1);
        return;
    }
    QCoreApplication::exit(0);
}

bool Importer::initDb()
{
#if 0
    m_db = QSqlDatabase::addDatabase("QMYSQL");
    if (!m_db.isValid()) {
        logFatal() << "Unknown database-type. MYSQL. Missing QSql plugins?";
        logCritical() << m_db.lastError().text();
        return false;
    }
    m_db.setConnectOptions(QString("UNIX_SOCKET=%1").arg(QDir::homePath() + "/utxo-test/mysqld.sock"));
    m_db.setDatabaseName("utxo");
    m_db.setUserName("root");
#else
    m_db = QSqlDatabase::addDatabase("QPSQL");
    if (!m_db.isValid()) {
        logFatal() << "Unknown database-type. PSQL. Missing QSql plugins?";
        logCritical() << m_db.lastError().text();
        return false;
    }
    m_db.setDatabaseName("utxo");
    m_db.setUserName("utxo");
#endif
    m_db.setHostName("localhost");
    if (m_db.isValid() && m_db.open()) {
        return createTables();
    } else {
        logFatal() << "Failed opening the database-connection" << m_db.lastError().text();
        return false;
    }
}

bool Importer::createTables()
{
    QSqlQuery query(m_db);
    query.exec("drop table utxo");

    QString q("create table utxo ( "
              "txid BIGINT, "			// the first 8 bytes of the (32-bytes) TXID (sha256)
              "outx INTEGER, "			// output-index
              // "txid_rest VARBINARY(25), "// the rest of the txid     // mysql
              "txid_rest bytea, "       // the rest of the txid         // postgresql
              //  "amount BIGINT, " 		// the amount held in this utxo.
              "offsetIB INTEGER, "		// byte-offset in block where the tx starts
              "b_height INTEGER "		// block-height
              // ", coinbase BOOLEAN" 	// true if this is a coinbase
              ")");


    if (!query.exec(q)) {
        logFatal() << "Failed to create table:" << query.lastError().text();
        return false;
    }
    if (!query.exec("create index utxo_basic on utxo (txid, outx)")) {
        logFatal() << "Failed to create index:" << query.lastError().text();
        return false;
    }
    return true;
}

void Importer::parseBlock(const CBlockIndex *index, FastBlock block)
{
    if (index->nHeight % 1000 == 0)
        logInfo() << "Parsing block" << index->nHeight << block.createHash();

    block.findTransactions();
    if (!m_db.transaction())
        throw std::runtime_error("Need transaction aware DB");

    try {
        bool first = true;
        for (auto tx : block.transactions()) {
            processTx(index, tx, first, tx.offsetInBlock(block));
            first = false;
        }
        m_db.commit();
    } catch (const std::exception &e) {
        m_db.rollback();
        throw;
    }

    m_txCount.fetchAndAddRelaxed(block.transactions().size());
}

struct Input {
    Streaming::ConstBuffer txid;
    int index;
};

void Importer::processTx(const CBlockIndex *index, Tx tx, bool isCoinbase, int offsetInBlock)
{
     // TODO return the amount of fee that this transaction generated so we can 'validate' the coinbase.

    std::list<Input> inputs;
    int outputCount = 0;
    QVariantList spendableOutputs;
    QTime time;
    time.start();
    {
        Input curInput;
        auto iter = Tx::Iterator(tx);
        auto content = iter.next();
        while (content != Tx::End) {
            if (!isCoinbase && content == Tx::PrevTxHash) { // only read inputs for non-coinbase txs
                curInput.txid = iter.byteData();
                if (curInput.txid.size() != 32)
                    throw std::runtime_error("Failed to understand PrevTxHash");
                content = iter.next(Tx::PrevTxIndex);
                if (content != Tx::PrevTxIndex)
                    throw std::runtime_error("Failed to find PrevTxIndex");
                curInput.index = iter.intData();
                inputs.push_back(curInput);
            }
            else if (content == Tx::OutputValue) {
                if (iter.longData() > 0)
                    spendableOutputs.append(QVariant(outputCount));
                outputCount++;
            }
            content = iter.next();
        }
    }
    m_parse.fetchAndAddRelaxed(time.elapsed());

    if (!inputs.empty()){
        time.start();
        QSqlQuery selectQuery(m_db);
        // selectQuery.prepare("select txid_rest, offsetIB, b_height from utxo where txid=:txid and outx=:index");
        selectQuery.prepare("select txid_rest from utxo where txid=:txid and outx=:index");
        QSqlQuery delQuery(m_db);
        delQuery.prepare("delete from utxo where txid=:txid and outx=:index and txid_rest=:txid2");
        for (auto input : inputs) {
            selectQuery.bindValue(":txid", QVariant(longFromBytes(input.txid)));
            selectQuery.bindValue(":index", input.index);
            if (!selectQuery.exec())
                throw std::runtime_error(selectQuery.lastError().text().toStdString());

            bool found = false;
            QByteArray partialTxId(input.txid.begin() + 7, 25);
            while (selectQuery.next()) { // we may get multiple results in case of a short-txid collision.
                if (selectQuery.value(0) == partialTxId) { // got it!
                    found = true;

                    // TODO
                    // we could use offset-in-block to read the actual transaction so we can
                    // dig out the amount and the script.

                    // TODO we could use the b_height to validate if this is a mature coin (may need an additional boolean 'iscoinbase' in DB)

                    break;
                }
            }
            m_selects.fetchAndAddRelaxed(time.elapsed());
            if (!found) {
                logFatal() << "block" << index->nHeight << "tx" << tx.createHash() << "tries to find input" << HexStr(input.txid) << input.index;
                logInfo() << "    " << QString::number(longFromBytes(input.txid), 16).toStdString();
                throw std::runtime_error("UTXO not found");
            }

            time.start();
            // now delete the row we just spent
            delQuery.bindValue(":txid", QVariant(longFromBytes(input.txid)));
            delQuery.bindValue(":index", input.index);
            delQuery.bindValue(":txid2", partialTxId);
            if (!delQuery.exec())
                throw std::runtime_error("Failed to run the utxo delete query");
            if (delQuery.numRowsAffected() != 1) {
                logFatal() << "Delete UTXO ended up removing" << delQuery.numRowsAffected() << "rows. Should always be 1.";
                throw std::runtime_error("UTXO delete didn't respond as expected");
            }
            m_deletes.fetchAndAddRelaxed(time.elapsed());
#if 0
            // check the delete actually worked.
            if (!selectQuery.exec())
                throw std::runtime_error(selectQuery.lastError().text().toStdString());
            if (selectQuery.size() > 0)
                logFatal() << "Still one in the UTXO!";
            while (selectQuery.next()) {
                logFatal() << selectQuery.value(0).toString();
            }
#endif
        }
    }

    if (spendableOutputs.isEmpty())
        return;
    time.start();

    QString insert("insert into utxo (txid, outx, txid_rest, offsetIB, b_height) VALUES (%1, ?, ?, %2, %3)");
    const uint256 myHash = tx.createHash();
    insert = insert.arg(longFromHash(myHash));
    insert = insert.arg(offsetInBlock);
    insert = insert.arg(index->nHeight);

    // ok, this looks a bit ugly. I don't know how to insert a byte-array in sql, so I'll just do this the hard way for now.
    const QByteArray txid2(reinterpret_cast<const char*>(myHash.begin()) + 7, 25);
    QVariantList txid2_list;
    for (int i = 0; i < spendableOutputs.length(); ++i) {
        txid2_list.append(txid2);
        // logInfo() << "insert" << QString::number(longFromHash(myHash), 16).toStdString() << "/" << HexStr(txid2) << spendableOutputs.at(i).toInt();
    }

    QSqlQuery query(m_db);
    query.prepare(insert);
    query.addBindValue(spendableOutputs);
    query.addBindValue(txid2_list);

    if (!query.execBatch())
        throw std::runtime_error(query.lastError().text().toStdString());
    m_inserts.fetchAndAddRelaxed(time.elapsed());
}
