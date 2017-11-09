/*
 * This file is part of the Flowee project
 * Copyright (C) 2009-2010 Satoshi Nakamoto
 * Copyright (C) 2009-2015 The Bitcoin Core developers
 * Copyright (C) 2017 Tom Zander <tomz@freedommail.ch>
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

#include "primitives/transaction.h"

#include "hash.h"
#include "tinyformat.h"
#include "utilstrencodings.h"

#include <streams.h>

boost::atomic<bool> flexTransActive(false);

std::string COutPoint::ToString() const
{
    return strprintf("COutPoint(%s, %u)", hash.ToString().substr(0,10), n);
}

CTxIn::CTxIn(COutPoint prevoutIn, CScript scriptSigIn, uint32_t nSequenceIn)
{
    prevout = prevoutIn;
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

CTxIn::CTxIn(uint256 hashPrevTx, uint32_t nOut, CScript scriptSigIn, uint32_t nSequenceIn)
{
    prevout = COutPoint(hashPrevTx, nOut);
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

std::string CTxIn::ToString() const
{
    std::string str;
    str += "CTxIn(";
    str += prevout.ToString();
    if (prevout.IsNull())
        str += strprintf(", coinbase %s", HexStr(scriptSig));
    else
        str += strprintf(", scriptSig=%s", HexStr(scriptSig).substr(0, 24));
    if (nSequence != SEQUENCE_FINAL)
        str += strprintf(", nSequence=%u", nSequence);
    str += ")";
    return str;
}

CTxOut::CTxOut(const CAmount& nValueIn, CScript scriptPubKeyIn)
{
    nValue = nValueIn;
    scriptPubKey = scriptPubKeyIn;
}

uint256 CTxOut::GetHash() const
{
    return SerializeHash(*this);
}

std::string CTxOut::ToString() const
{
    return strprintf("CTxOut(nValue=%d.%08d, scriptPubKey=%s)", nValue / COIN, nValue % COIN, HexStr(scriptPubKey).substr(0, 30));
}

CMutableTransaction::CMutableTransaction() : nVersion(CTransaction::CURRENT_VERSION), nLockTime(0) {}
CMutableTransaction::CMutableTransaction(const CTransaction& tx) : nVersion(tx.nVersion), vin(tx.vin), vout(tx.vout), nLockTime(tx.nLockTime) {}

uint256 CMutableTransaction::GetHash() const
{
    CHashWriter ss(0, 0);
    SerializeTransaction(*this, ss, 0, 0, false);
    return ss.GetHash();
}

void CTransaction::UpdateHash()
{
    if (nVersion == 4 && flexTransActive.load()) {
        if (txData.empty()) {
            CDataStream stream(0, 0);
            SerializeTransaction(*this, stream, 0, 0, false);
            txData = std::vector<char>(stream.begin(), stream.end());
        }
        CHashWriter ss(0, 0);
        ss.write(&txData[0], txData.size());
        hash = ss.GetHash();
    } else {
        hash = SerializeHash(*this);
    }
}

CTransaction::CTransaction() : nVersion(CTransaction::CURRENT_VERSION), vin(), vout(), nLockTime(0) { }

CTransaction::CTransaction(const CMutableTransaction &tx) : nVersion(tx.nVersion), vin(tx.vin), vout(tx.vout), nLockTime(tx.nLockTime) {
    UpdateHash();
}

CTransaction& CTransaction::operator=(const CTransaction &tx) {
    *const_cast<int*>(&nVersion) = tx.nVersion;
    *const_cast<std::vector<CTxIn>*>(&vin) = tx.vin;
    *const_cast<std::vector<CTxOut>*>(&vout) = tx.vout;
    *const_cast<unsigned int*>(&nLockTime) = tx.nLockTime;
    hash = tx.hash;
    txData = tx.txData;
    return *this;
}

uint256 CTransaction::CalculateSignaturesHash() const
{
    CHashWriter ss(0, 0);
    ss << hash;
    SerialiseScriptSig4(vin, ss, 0, 0);
    CMFToken end(Consensus::TxEnd);
    end.Serialize<CHashWriter>(ss, 0, 0);
    return ss.GetHash();
}

std::vector<char> loadTransaction(const std::vector<CMFToken> &tokens, std::vector<CTxIn> &inputs, std::vector<CTxOut> &outputs, int nVersion)
{
    assert(inputs.empty());
    assert(outputs.empty());
    int signatureCount = -1;
    bool storedOutValue = false, storedOutScript = false;
    bool seenCoinbaseMessage = false;
    int64_t outValue = 0;
    std::vector<char> txData;
    bool inMainTx = true;

    for (unsigned int index = 0; index < tokens.size(); ++index) {
        const auto token = tokens[index];
        switch (token.tag) {
        case Consensus::TxInPrevHash: {
            auto data = boost::get<std::vector<char> >(token.data);
            if (data.size() != 256/8) throw std::runtime_error("PrevHash size wrong");
            if (!inMainTx) throw std::runtime_error("wrong section");
            if (seenCoinbaseMessage) throw std::runtime_error("No input allowed on coinbase");
            inputs.push_back(CTxIn(COutPoint(uint256(&data[0]), 0)));
            break;
        }
        case Consensus::TxInPrevIndex: {
            if (inputs.empty()) throw std::runtime_error("TxInPrevIndex before TxInPrevHash");
            if (!inMainTx) throw std::runtime_error("wrong section");
            if (seenCoinbaseMessage) throw std::runtime_error("No input allowed on coinbase");
            inputs[inputs.size()-1].prevout.n = (uint32_t) token.longData();
            break;
        }
        case Consensus::CoinbaseMessage: {
            if (!inputs.empty()) throw std::runtime_error("CoinbaseMessage not allowed when there are inputs");
            if (!inMainTx) throw std::runtime_error("wrong section");
            inputs.push_back(CTxIn());
            auto data = token.unsignedByteArray();
            inputs[0].scriptSig = CScript(data.begin(), data.end());
            seenCoinbaseMessage = true;
            break;
        }
        case Consensus::TxEnd:
        case Consensus::TxInputStackItem:
        case Consensus::TxInputStackItemContinued: {
            if (signatureCount == -1) { // copy all of the input tags
                CDataStream stream(0, 4);
                ser_writedata32(stream, nVersion);
                for (unsigned int i = 0; i < index; ++i) {
                    tokens[i].Serialize(stream, 0, 4);
                }
                txData = std::vector<char>(stream.begin(), stream.end());
            }
            if (token.tag == Consensus::TxEnd)
                return txData;
            if (signatureCount < 0 || token.tag == Consensus::TxInputStackItem)
                signatureCount++;
            if (static_cast<int>(inputs.size()) <= signatureCount)
                throw std::runtime_error("TxInputStackItem* before TxInPrevHash");

            inMainTx = false;
            auto data = token.unsignedByteArray();
            if (data.size() == 1)
                inputs[signatureCount].scriptSig << data.at(0);
            else
                inputs[signatureCount].scriptSig << data;
            break;
        }
            // TxOut* don't have a pre-defined order, just that both are required so they always have to come in pairs.
        case Consensus::TxOutValue:
            if (!inMainTx) throw std::runtime_error("wrong section");
            if (storedOutScript) { // add it.
                outputs[outputs.size() -1].nValue = token.longData();
                storedOutScript = storedOutValue = false;
            } else { // store it.
                outValue = token.longData();
                storedOutValue = true;
            }
            break;
        case Consensus::TxOutScript: {
            if (!inMainTx) throw std::runtime_error("wrong section");
            auto data = token.unsignedByteArray();
            outputs.push_back(CTxOut(outValue, CScript(data.begin(), data.end())));
            if (storedOutValue)
                storedOutValue = false;
            else
                storedOutScript = true;
            break;
        }
        case Consensus::TxRelativeBlockLock:
        case Consensus::TxRelativeTimeLock:
            if (inputs.empty()) throw std::runtime_error("Transaction needs inputs");
            if (token.longData() > CTxIn::SEQUENCE_LOCKTIME_MASK) throw std::runtime_error("out of range");
            if (inputs.back().nSequence != CTxIn::SEQUENCE_FINAL) throw std::runtime_error("Too many locks for input");
            if (!inMainTx) throw std::runtime_error("wrong section");
            if (token.tag == Consensus::TxRelativeBlockLock)
                inputs.back().nSequence = token.longData();
            else
                inputs.back().nSequence = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | token.longData();
            break;
        default:
            if (token.tag > 19)
                throw std::runtime_error("Illegal tag in transaction");
        }
    }
    return txData;
}

CAmount CTransaction::GetValueOut() const
{
    CAmount nValueOut = 0;
    for (std::vector<CTxOut>::const_iterator it(vout.begin()); it != vout.end(); ++it)
    {
        nValueOut += it->nValue;
        if (!MoneyRange(it->nValue) || !MoneyRange(nValueOut))
            throw std::runtime_error("CTransaction::GetValueOut(): value out of range");
    }
    return nValueOut;
}

double CTransaction::ComputePriority(double dPriorityInputs, unsigned int nTxSize) const
{
    nTxSize = CalculateModifiedSize(nTxSize);
    if (nTxSize == 0) return 0.0;

    return dPriorityInputs / nTxSize;
}

unsigned int CTransaction::CalculateModifiedSize(unsigned int nTxSize) const
{
    // In order to avoid disincentivizing cleaning up the UTXO set we don't count
    // the constant overhead for each txin and up to 110 bytes of scriptSig (which
    // is enough to cover a compressed pubkey p2sh redemption) for priority.
    // Providing any more cleanup incentive than making additional inputs free would
    // risk encouraging people to create junk outputs to redeem later.
    if (nTxSize == 0)
        nTxSize = ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
    for (std::vector<CTxIn>::const_iterator it(vin.begin()); it != vin.end(); ++it)
    {
        unsigned int offset = 41U + std::min(110U, (unsigned int)it->scriptSig.size());
        if (nTxSize > offset)
            nTxSize -= offset;
    }
    return nTxSize;
}

std::string CTransaction::ToString() const
{
    std::string str;
    str += strprintf("CTransaction(hash=%s, ver=%d, vin.size=%u, vout.size=%u, nLockTime=%u)\n",
        GetHash().ToString().substr(0,10),
        nVersion,
        vin.size(),
        vout.size(),
        nLockTime);
    for (unsigned int i = 0; i < vin.size(); i++)
        str += "    " + vin[i].ToString() + "\n";
    for (unsigned int i = 0; i < vout.size(); i++)
        str += "    " + vout[i].ToString() + "\n";
    return str;
}

Log::Item operator<<(Log::Item item, const COutPoint &p) {
    if (item.isEnabled()) {
        const bool old = item.useSpace();
        item.nospace() << "Tx*[" << p.hash << "-" << p.n << ']';
        if (old)
            return item.space();
    }
    return item;
}

Log::Item operator<<(Log::Item item, const CTxOut &out)
{
    if (item.isEnabled()) {
        const bool old = item.useSpace();
        item.nospace() << "Out[" << out.nValue << " sat]";
        if (old)
            return item.space();
    }
    return item;

}

Log::Item operator<<(Log::Item item, const CTxIn &in)
{
    // ok, this may be a bit lazy. But I don't think I'll need anything else, ever.
    return item.maybespace() << in.prevout;
}
