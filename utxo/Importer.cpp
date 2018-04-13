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
#include <QCoreApplication>
#include <chain.h>
#include <chainparamsbase.h>
#include <QVariant>
#include <QDebug>
#include <QTime>

namespace {
quint64 longFromHash(const uint256 &sha) {
    const quint64 *answer = reinterpret_cast<const quint64*>(sha.begin());
    return answer[0] >> 1;
}

struct Input {
    uint256 txid;
    int index;
};

std::list<Input> findInputs(Tx::Iterator &iter) {
    std::list<Input> inputs;

    Input curInput;
    auto content = iter.next();
    while (content != Tx::End) {
        if (content == Tx::PrevTxHash) { // only read inputs for non-coinbase txs
            if (iter.byteData().size() != 32)
                throw std::runtime_error("Failed to understand PrevTxHash");
            curInput.txid = iter.uint256Data();
            content = iter.next(Tx::PrevTxIndex);
            if (content != Tx::PrevTxIndex)
                throw std::runtime_error("Failed to find PrevTxIndex");
            curInput.index = iter.intData();
            inputs.push_back(curInput);
        }
        else if (content == Tx::OutputValue) {
            break;
        }
        content = iter.next();
    }
    return inputs;
}

QString escapeBytes(const unsigned char *data, int length) {
    static const QString answer ("E'\\\\x%1'");
    return answer.arg(QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(data), length).toHex()));
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
    m_selectQuery = QSqlQuery(m_db);
    m_selectQuery.prepare("select txid_rest, oid from utxo where txid=:txid and outx=:index");
    m_insertQuery = QSqlQuery(m_db);
    m_insertQuery.prepare("insert into utxo (txid, outx, txid_rest, offsetIB, b_height) VALUES (?, ?, ?, ?, ?)");

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

        if (!m_db.transaction())
            throw std::runtime_error("Need transaction aware DB");
        int nextStop = 50000;
        int lastHeight = -1;
        while(true) {
            index = chain.Next(index); // we skip genesis, its not part of the utxo
            if (! index)
                break;
            lastHeight = index->nHeight;
            parseBlock(index, Blocks::DB::instance()->loadBlock(index->GetBlockPos()));

            if (m_txCount.load() > nextStop) {
                nextStop = m_txCount.load() + 50000;
                auto elapsed = time.elapsed();
                logCritical().nospace() << "Finished blocks 0..." << index->nHeight << ", tx count: " << m_txCount.load();
                logCritical() << "  parseBlocks" << m_parse.load() << "ms\t" << (m_parse.load() * 100 / elapsed) << '%';
                logCritical() << "       select" << m_selects.load() << "ms\t" << (m_selects.load() * 100 / elapsed) << '%';
                logCritical() << "       delete" << m_deletes.load() << "ms\t" << (m_deletes.load() * 100 / elapsed) << '%';
                logCritical() << "       insert" << m_inserts.load() << "ms\t" << (m_inserts.load() * 100 / elapsed) << '%';
                logCritical() << "    filter-tx" << m_filterTx.load() << "ms\t" << (m_filterTx.load() * 100 / elapsed) << '%';
                logCritical() << "   Wall-clock" << elapsed << "ms";

                m_db.commit();
                m_db.transaction();
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
              ") WITH (OIDS)");

    if (!query.exec(q)) {
        logFatal() << "Failed to create table:" << query.lastError().text();
        return false;
    }
    if (!query.exec("create index utxo_basic on utxo (txid, outx)")) {
        logFatal() << "Failed to create index:" << query.lastError().text();
        return false;
    }
    if (!query.exec("create index utxo_oids on utxo (oid)")) {
        logFatal() << "Failed to create index:" << query.lastError().text();
        return false;
    }
    return true;
}

void Importer::parseBlock(const CBlockIndex *index, FastBlock block)
{
    if (index->nHeight % 1000 == 0)
        logInfo() << "Parsing block" << index->nHeight << block.createHash() << "tx-count" << m_txCount.load();

    block.findTransactions();
    const auto transactions = block.transactions();

    QSet<int> ordered;
    QTime time;
    time.start();
    {
        /* Filter the transactions.
        * Transactions by consensus are sequential, tx 2 can't spend a UTXO that is created in tx 3.
        *
        * This means that our ordering is Ok, we just want to be able to remove all the transactions
        * that spend transactions NOT created in this block, which we can then process in parallel.
        * Notice that the fact that the transactions are sorted now is awesome as that makes the process
        * much much faster.
        *
        * Additionally we check for double-spends. No 2 transactions are allowed to spend the same UTXO inside this block.
        */
        typedef boost::unordered_map<uint256, int, Blocks::BlockHashShortener> TXMap;
        TXMap txMap;

        typedef boost::unordered_map<uint256, std::vector<bool>, Blocks::BlockHashShortener> MiniUTXO;
        MiniUTXO miniUTXO;

        bool first = true;
        int txNum = 1;
        for (auto tx : transactions) {
            if (first) { // skip coinbase
                first = false;
                continue;
            }
            uint256 hash = tx.createHash();
            bool ocd = false; // if true, tx requires order.

            auto i = Tx::Iterator(tx);
            auto inputs = findInputs(i);
            for (auto input : inputs) {
                auto ti = txMap.find(input.txid);
                if (ti != txMap.end()) {
                    ocd = true;
                    /*
                     * ok, so we spend a tx also in this block.
                     * to make sure we don't hit a double-spend here I have to actually check the outputs of the prev-tx.
                     *
                     * at this time this isn't unit tested, as such you should assume it is broken.
                     * the point of this code is to make clear how we can avoid processing our transactions serially
                     * and we can avoid the need for 'rollback()' (when a block fails half-way through) because we
                     * detect in-block double-spends without touching the DB.
                     *
                     * so I can do all sql deletes in one go when the block is done validating.
                     */
                    auto prevIter = miniUTXO.find(input.txid);
                    if (prevIter == miniUTXO.end()) {
                        /*
                         *  insert into the miniUTXO the prevtx outputs.
                         * we **could** have done this at the more logical code-place for all transactions,
                         * but since we expect less than 1% of the transactions to spend inside of the same block,
                         * that would waste resources.
                         */
                        auto iter = Tx::Iterator(transactions.at(ti->second));
                        Tx::Component component;
                        std::vector<bool> outputs;
                        while (true) {
                            component = iter.next(Tx::OutputValue);
                            if (component == Tx::End)
                                break;
                            outputs.push_back(iter.longData() > 0);
                        }

                        prevIter = miniUTXO.insert(std::make_pair(input.txid, outputs)).first;

                        ordered.insert(ti->second);
                    }
                    if (prevIter->second.size() <= input.index)
                        throw std::runtime_error("spending utxo output out of range");
                    if (prevIter->second[input.index] == false)
                        throw std::runtime_error("spending utxo in-block double-spend");
                    prevIter->second[input.index] = false;
                }
            }

            if (ocd)
                ordered.insert(txNum);
            txMap.insert(std::make_pair(hash, txNum++));
        }
    }
    m_filterTx.fetchAndAddRelaxed(time.elapsed());

    ProcessTxResult commandsForallTransactions;

    for (size_t i = 0; i < transactions.size(); ++i) {
        Tx tx = transactions.at(i);
        if (ordered.contains(i))
            commandsForallTransactions.rowsToDelete += processTx(index, tx, i == 0, tx.offsetInBlock(block), true).rowsToDelete;
        else
            //   process in thread. ??
            commandsForallTransactions += processTx(index, tx, i == 0, tx.offsetInBlock(block), false);
    }

    Q_ASSERT(commandsForallTransactions.outx.size() > 0); // at minimum the coinbase has an output.

    time.start();
    m_insertQuery.addBindValue(commandsForallTransactions.txid);
    m_insertQuery.addBindValue(commandsForallTransactions.outx);
    m_insertQuery.addBindValue(commandsForallTransactions.txid2);
    m_insertQuery.addBindValue(commandsForallTransactions.offsetInBlock);
    m_insertQuery.addBindValue(commandsForallTransactions.blockHeight);
    if (!m_insertQuery.execBatch())
        throw std::runtime_error(m_insertQuery.lastError().text().toStdString());

    m_inserts.fetchAndAddRelaxed(time.elapsed());

    time.start();
    QSqlQuery query(m_db);
    while (!commandsForallTransactions.rowsToDelete.isEmpty()) {
        QByteArray buffer;
        {
            QTextStream stream(&buffer);
            stream << "delete from utxo where oid in (";
            bool first = true;
            while (stream.pos() < 500000 && !commandsForallTransactions.rowsToDelete.isEmpty()) {
                if (!first)
                    stream << ',';
                stream << commandsForallTransactions.rowsToDelete.takeFirst();
                first = false;
            }
            stream << ")";
        }
        QString q = QString::fromLatin1(buffer);
        query.prepare(q);

        if (!query.exec()) {
            throw std::runtime_error(query.lastError().text().toStdString());
        }
    }
    m_deletes.fetchAndAddRelaxed(time.elapsed());

    m_txCount.fetchAndAddRelaxed(block.transactions().size());
}

Importer::ProcessTxResult Importer::processTx(const CBlockIndex *index, Tx tx, bool isCoinbase, int offsetInBlock, bool insertDirect)
{
    std::list<Input> inputs;
    int outputCount = 0;
    ProcessTxResult result;
    QTime time;
    time.start();
    {
        auto iter = Tx::Iterator(tx);
        if (!isCoinbase)
            inputs = findInputs(iter);
        auto content = iter.tag();
        while (content != Tx::End) {
            if (content == Tx::OutputValue) {
                if (iter.longData() > 0)
                    result.outx.append(QVariant(outputCount));
                outputCount++;
            }
            content = iter.next();
        }
    }
    m_parse.fetchAndAddRelaxed(time.elapsed());

    if (!inputs.empty()) {
        QTime selectTime;
        selectTime.start();
        for (auto input : inputs) {
            m_selectQuery.bindValue(":txid", QVariant(longFromHash(input.txid)));
            m_selectQuery.bindValue(":index", input.index);
            if (!m_selectQuery.exec())
                throw std::runtime_error(m_selectQuery.lastError().text().toStdString());

            bool found = false;
            QByteArray partialTxId(reinterpret_cast<const char*>(input.txid.begin()) + 7, 25);
            while (m_selectQuery.next()) { // we may get multiple results in case of a short-txid collision.
                if (m_selectQuery.value(0) == partialTxId) { // got it!
                    found = true;

                    // TODO
                    // we could use offset-in-block to read the actual transaction so we can
                    // dig out the amount and the script.

                    // TODO we could use the b_height to validate if this is a mature coin (may need an additional boolean 'iscoinbase' in DB)

                    break;
                }
            }
            if (!found) {
                logFatal() << "block" << index->nHeight << "tx" << tx.createHash() << "tries to find input" << HexStr(input.txid) << input.index;
                logInfo() << "    " << QString::number(longFromHash(input.txid), 16).toStdString();
                throw std::runtime_error("UTXO not found");
            }

            result.rowsToDelete.append(m_selectQuery.value(1).toLongLong());
        }
        m_selects.fetchAndAddRelaxed(selectTime.elapsed());
    }

    const uint256 myHash = tx.createHash();
    const auto txid = longFromHash(myHash);
    const QByteArray txid2(reinterpret_cast<const char*>(myHash.begin()) + 7, 25);
    for (int i = 0; i < result.outx.size(); ++i) {
        result.blockHeight.append(index->nHeight);
        result.offsetInBlock.append(offsetInBlock);
        result.txid.append(txid);
        result.txid2.append(txid2);
    }

    if (insertDirect) {
        time.start();
        // other transactions in this block require these to be in the DB
        m_insertQuery.addBindValue(result.txid);
        m_insertQuery.addBindValue(result.outx);
        m_insertQuery.addBindValue(result.txid2);
        m_insertQuery.addBindValue(result.offsetInBlock);
        m_insertQuery.addBindValue(result.blockHeight);
        if (!m_insertQuery.execBatch())
            throw std::runtime_error(m_insertQuery.lastError().text().toStdString());

        result.blockHeight.clear();
        result.offsetInBlock.clear();
        result.txid.clear();
        result.txid2.clear();
        result.outx.clear();
        m_inserts.fetchAndAddRelaxed(time.elapsed());
    }

    return result;
}

Importer::ProcessTxResult &Importer::ProcessTxResult::operator+=(const Importer::ProcessTxResult &other)
{
    txid += other.txid;
    txid2 += other.txid2;
    offsetInBlock += other.offsetInBlock;
    blockHeight += other.blockHeight;
    rowsToDelete += other.rowsToDelete;
    outx += other.outx;
    return *this;
}
