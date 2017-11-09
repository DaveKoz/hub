/*
 * This file is part of the Flowee project
 * Copyright (C) 2009-2010 Satoshi Nakamoto
 * Copyright (C) 2009-2015 The Bitcoin Core developers
 * Copyright (C) 2016-2017 Tom Zander <tomz@freedommail.ch>
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

#ifndef BITCOIN_PRIMITIVES_TRANSACTION_H
#define BITCOIN_PRIMITIVES_TRANSACTION_H

#include "amount.h"
#include "script/script.h"
#include "serialize.h"
#include "uint256.h"

#include "../consensus/transactionv4.h"

/** An outpoint - a combination of a transaction hash and an index n into its vout */
class COutPoint
{
public:
    uint256 hash;
    uint32_t n;

    COutPoint() { SetNull(); }
    COutPoint(uint256 hashIn, uint32_t nIn) { hash = hashIn; n = nIn; }

    ADD_SERIALIZE_METHODS

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(hash);
        READWRITE(n);
    }

    void SetNull() { hash.SetNull(); n = (uint32_t) -1; }
    bool IsNull() const { return (hash.IsNull() && n == (uint32_t) -1); }

    friend bool operator<(const COutPoint& a, const COutPoint& b)
    {
        return (a.hash < b.hash || (a.hash == b.hash && a.n < b.n));
    }

    friend bool operator==(const COutPoint& a, const COutPoint& b)
    {
        return (a.hash == b.hash && a.n == b.n);
    }

    friend bool operator!=(const COutPoint& a, const COutPoint& b)
    {
        return !(a == b);
    }

    std::string ToString() const;
};

/** An input of a transaction.  It contains the location of the previous
 * transaction's output that it claims and a signature that matches the
 * output's public key.
 */
class CTxIn
{
public:
    COutPoint prevout;
    CScript scriptSig;
    uint32_t nSequence;

    /* Setting nSequence to this value for every input in a transaction
     * disables nLockTime. */
    static const uint32_t SEQUENCE_FINAL = 0xffffffff;

    /* Below flags apply in the context of BIP 68*/
    /* If this flag set, CTxIn::nSequence is NOT interpreted as a
     * relative lock-time. */
    static const uint32_t SEQUENCE_LOCKTIME_DISABLE_FLAG = (1 << 31);

    /* If CTxIn::nSequence encodes a relative lock-time and this flag
     * is set, the relative lock-time has units of 512 seconds,
     * otherwise it specifies blocks with a granularity of 1. */
    static const uint32_t SEQUENCE_LOCKTIME_TYPE_FLAG = (1 << 22);

    /* If CTxIn::nSequence encodes a relative lock-time, this mask is
     * applied to extract that lock-time from the sequence field. */
    static const uint32_t SEQUENCE_LOCKTIME_MASK = 0x0000ffff;

    /* In order to use the same number of bits to encode roughly the
     * same wall-clock duration, and because blocks are naturally
     * limited to occur every 600s on average, the minimum granularity
     * for time-based relative lock-time is fixed at 512 seconds.
     * Converting from CTxIn::nSequence to seconds is performed by
     * multiplying by 512 = 2^9, or equivalently shifting up by
     * 9 bits. */
    static const int SEQUENCE_LOCKTIME_GRANULARITY = 9;

    CTxIn()
    {
        nSequence = SEQUENCE_FINAL;
    }

    explicit CTxIn(COutPoint prevoutIn, CScript scriptSigIn=CScript(), uint32_t nSequenceIn=SEQUENCE_FINAL);
    CTxIn(uint256 hashPrevTx, uint32_t nOut, CScript scriptSigIn=CScript(), uint32_t nSequenceIn=SEQUENCE_FINAL);

    ADD_SERIALIZE_METHODS

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(prevout);
        READWRITE(*(CScriptBase*)(&scriptSig));
        READWRITE(nSequence);
    }

    friend bool operator==(const CTxIn& a, const CTxIn& b)
    {
        return (a.prevout   == b.prevout &&
                a.scriptSig == b.scriptSig &&
                a.nSequence == b.nSequence);
    }

    friend bool operator!=(const CTxIn& a, const CTxIn& b)
    {
        return !(a == b);
    }

    std::string ToString() const;
};

/** An output of a transaction.  It contains the public key that the next input
 * must be able to sign with to claim it.
 */
class CTxOut
{
public:
    CAmount nValue;
    CScript scriptPubKey;

    CTxOut()
    {
        SetNull();
    }

    CTxOut(const CAmount& nValueIn, CScript scriptPubKeyIn);

    ADD_SERIALIZE_METHODS

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(nValue);
        READWRITE(*(CScriptBase*)(&scriptPubKey));
    }

    void SetNull()
    {
        nValue = -1;
        scriptPubKey.clear();
    }

    bool IsNull() const
    {
        return (nValue == -1);
    }

    uint256 GetHash() const;

    CAmount GetDustThreshold(const CFeeRate &minRelayTxFee) const
    {
        // "Dust" is defined in terms of ::minRelayTxFee,
        // which has units satoshis-per-kilobyte.
        // If you'd pay more than 1/3 in fees
        // to spend something, then we consider it dust.
        // A typical spendable txout is 34 bytes big, and will
        // need a CTxIn of at least 148 bytes to spend:
        // so dust is a spendable txout less than
        // 546*minRelayTxFee/1000 (in satoshis)
        if (scriptPubKey.IsUnspendable())
            return 0;

        size_t nSize = GetSerializeSize(SER_DISK,0)+148u;
        return 3*minRelayTxFee.GetFee(nSize);
    }

    bool IsDust(const CFeeRate &minRelayTxFee) const
    {
        return (nValue < GetDustThreshold(minRelayTxFee));
    }

    friend bool operator==(const CTxOut& a, const CTxOut& b)
    {
        return (a.nValue       == b.nValue &&
                a.scriptPubKey == b.scriptPubKey);
    }

    friend bool operator!=(const CTxOut& a, const CTxOut& b)
    {
        return !(a == b);
    }

    std::string ToString() const;
};

#include <boost/atomic.hpp>
extern boost::atomic<bool> flexTransActive;
struct CMutableTransaction;

template<typename Stream>
void SerialiseScriptSig4(const std::vector<CTxIn> &inputs, Stream &s, int nType, int nVersion)
{
    for (auto in : inputs) {
        bool first = true;
        unsigned int i = 0; // use both iterator and int b/c the iterator doesn't do bounds-checks.
        auto iter = in.scriptSig.begin();
        while (i < in.scriptSig.size()) {
            uint8_t k = *iter;
            if ((k > 0 && k < 76) || (k >= 76 && k <= 78)) {
                uint32_t size = k;
                if (k >= 76) { // OP_PUSHDATA
                    const int width = k - 75; // address width
                    size = *(++iter);
                    if (width > 1) {
                        size = (size << 8) + (uint8_t) *(++iter);
                        if (width > 2) {
                            size = (size << 8) + (uint8_t) *(++iter);
                            size = (size << 8) + (uint8_t) *(++iter);
                            ++i;
                        }
                    }
                    i += width;
                }
                if (size + i >= in.scriptSig.size())
                    throw std::runtime_error("Signatures malformed");
                auto iterBegin = ++iter;
                iter += size;
                CMFToken token(first ? Consensus::TxInputStackItem : Consensus::TxInputStackItemContinued,
                    std::vector<char>(iterBegin, iter));
                STORECMF(token);
                i += size;
            } else if (k == OP_FALSE) {
                CMFToken token(first ? Consensus::TxInputStackItem : Consensus::TxInputStackItemContinued,
                               std::vector<char>(1, OP_FALSE));
                STORECMF(token);
                ++iter;
            } else
                throw std::runtime_error("Signatures malformed");
            i++;
            first = false;
        }
    }
}

template<typename Stream, typename TxType>
inline void SerializeTransaction(TxType& tx, Stream& s, int nType, int nVersion, bool withSignatures = true) {
    ser_writedata32(s, tx.nVersion);
    nVersion = tx.nVersion;
    if (flexTransActive && nVersion == 4) {
        const bool isCoinbaseTx = tx.vin.size() == 1 && tx.vin.at(0).prevout.IsNull() && !tx.vin.at(0).scriptSig.empty();
        if (isCoinbaseTx) {
            // coinbase is a little special. If you use the same output address in different blocks, you'd quite easy get
            // a duplicate txid since we no longer use the scriptSig. We can't have two TXs with the same
            // txid in the chain, that would break Bitcoin. This code stores the unique data in a CoinbaseMessage token.
            const CTxIn &in = tx.vin[0];
            CMFToken msg(Consensus::CoinbaseMessage, std::vector<char>(in.scriptSig.begin(), in.scriptSig.end()));
            STORECMF(msg);
        } else {
            for (auto in : tx.vin) {
                CMFToken hash(Consensus::TxInPrevHash, in.prevout.hash);
                STORECMF(hash);
                if (in.prevout.n > 0) {
                    CMFToken index(Consensus::TxInPrevIndex, (uint64_t) in.prevout.n);
                    STORECMF(index);
                }
                if ((in.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) == 0) {
                    const bool timeBased = in.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG; // time, as opposed to block-based
                    CMFToken lock(timeBased ? Consensus::TxRelativeTimeLock : Consensus::TxRelativeBlockLock, (int) (in.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK));
                    STORECMF(lock);
                }
            }
        }
        for (auto out : tx.vout) {
            CMFToken token(Consensus::TxOutValue, (uint64_t) out.nValue);
            STORECMF(token);
            std::vector<char> script(out.scriptPubKey.begin(), out.scriptPubKey.end());
            token = CMFToken(Consensus::TxOutScript, script);
            STORECMF(token);
        }
        if (withSignatures) {
            if (!isCoinbaseTx)
                SerialiseScriptSig4(tx.vin, s, nType, nVersion);
            CMFToken end(Consensus::TxEnd);
            STORECMF(end);
        }
    } else {
        CSerActionSerialize ser_action;
        READWRITE(tx.vin);
        READWRITE(tx.vout);
        READWRITE(tx.nLockTime);
    }
}

std::vector<char> loadTransaction(const std::vector<CMFToken>& tokens, std::vector<CTxIn> &inputs, std::vector<CTxOut> &outputs, int nVersion);

template<typename Stream, typename TxType>
inline std::vector<char> UnSerializeTransaction(TxType& tx, Stream& s, int nType, int nVersion) {
    *const_cast<int32_t*>(&tx.nVersion) = ser_readdata32(s);
    nVersion = tx.nVersion;
    if (nVersion == 4 && flexTransActive) {
        auto cmfs = UnserializeCMFs(s, Consensus::TxEnd, nType, nVersion);
        return loadTransaction(cmfs,
            *const_cast<std::vector<CTxIn>*>(&tx.vin),
            *const_cast<std::vector<CTxOut>*>(&tx.vout), nVersion);
    } else {
        CSerActionUnserialize ser_action;
        READWRITE(*const_cast<std::vector<CTxIn>*>(&tx.vin));
        READWRITE(*const_cast<std::vector<CTxOut>*>(&tx.vout));
        READWRITE(*const_cast<uint32_t*>(&tx.nLockTime));
    }
    return std::vector<char>();
}

/** The basic transaction that is broadcasted on the network and contained in
 * blocks.  A transaction can contain multiple inputs and outputs.
 */
class CTransaction
{
private:
    /** Memory only. */
    uint256 hash;
    void UpdateHash();
    std::vector<char> txData;

public:
    // Default transaction version.
    static const int32_t CURRENT_VERSION=1;

    // Changing the default transaction version requires a two step process: first
    // adapting relay policy by bumping MAX_STANDARD_VERSION, and then later date
    // bumping the default CURRENT_VERSION at which point both CURRENT_VERSION and
    // MAX_STANDARD_VERSION will be equal.
    static const int32_t MAX_STANDARD_VERSION=2;

    // The local variables are made const to prevent unintended modification
    // without updating the cached hash value. However, CTransaction is not
    // actually immutable; deserialization and assignment are implemented,
    // and bypass the constness. This is safe, as they update the entire
    // structure, including the hash.
    const int32_t nVersion;
    const std::vector<CTxIn> vin;
    const std::vector<CTxOut> vout;
    const uint32_t nLockTime;

    /** Construct a CTransaction that qualifies as IsNull() */
    CTransaction();

    /** Convert a CMutableTransaction into a CTransaction. */
    CTransaction(const CMutableTransaction &tx);

    CTransaction& operator=(const CTransaction& tx);

    size_t GetSerializeSize(int nType, int nVersion) const {
        CSizeComputer s(nType, nVersion);
        Serialize(s, nType, nVersion);
        return s.size();
    }
    template<typename Stream>
    void Serialize(Stream& s, int nType, int version) const {
        if (!txData.empty()) {
            s.write(&txData[0], txData.size());
            const bool isCoinbaseTx = vin.size() == 1 && vin.at(0).prevout.IsNull() && !vin.at(0).scriptSig.empty();
            if (!isCoinbaseTx)
                SerialiseScriptSig4(vin, s, nType, version);
            STORECMF(CMFToken(Consensus::TxEnd));
        } else {
            SerializeTransaction(*this, s, nType, nVersion);
        }
    }

    template<typename Stream>
    void Unserialize(Stream& s, int nType, int version) {
        txData = UnSerializeTransaction(*const_cast<CTransaction*>(this), s, nType, version);
        UpdateHash();
    }

    bool IsNull() const {
        return vin.empty() && vout.empty();
    }

    const uint256& GetHash() const {
        return hash;
    }

    /// for transactions that separate their signatures (like v4) calculate the hash of this data.
    uint256 CalculateSignaturesHash() const;

    // Return sum of txouts.
    CAmount GetValueOut() const;
    // GetValueIn() is a method on CCoinsViewCache, because
    // inputs must be known to compute value in.

    // Compute priority, given priority of inputs and (optionally) tx size
    double ComputePriority(double dPriorityInputs, unsigned int nTxSize=0) const;

    // Compute modified tx size for priority calculation (optionally given tx size)
    unsigned int CalculateModifiedSize(unsigned int nTxSize=0) const;

    bool IsCoinBase() const
    {
        return (vin.size() == 1 && vin[0].prevout.IsNull());
    }

    friend bool operator==(const CTransaction& a, const CTransaction& b)
    {
        return a.hash == b.hash;
    }

    friend bool operator!=(const CTransaction& a, const CTransaction& b)
    {
        return a.hash != b.hash;
    }

    std::string ToString() const;
};

/** A mutable version of CTransaction. */
struct CMutableTransaction
{
    int32_t nVersion;
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    uint32_t nLockTime;

    CMutableTransaction();
    CMutableTransaction(const CTransaction& tx);

    size_t GetSerializeSize(int nType, int nVersion) const {
        CSizeComputer s(nType, nVersion);
        Serialize(s, nType, nVersion);
        return s.size();
    }
    template<typename Stream>
    void Serialize(Stream& s, int nType, int version) const {
        SerializeTransaction(*this, s, nType, nVersion);
    }

    template<typename Stream>
    void Unserialize(Stream& s, int nType, int version) {
        (void) UnSerializeTransaction(*this, s, nType, nVersion);
    }

    /** Compute the hash of this CMutableTransaction. This is computed on the
     * fly, as opposed to GetHash() in CTransaction, which uses a cached result.
     */
    uint256 GetHash() const;
};

Log::Item operator<<(Log::Item item, const COutPoint &p);
inline Log::SilentItem operator<<(Log::SilentItem item, const COutPoint &) {
    return item;
}
Log::Item operator<<(Log::Item item, const CTxIn &in);
inline Log::SilentItem operator<<(Log::SilentItem item, const CTxIn &) {
    return item;
}
Log::Item operator<<(Log::Item item, const CTxOut &in);
inline Log::SilentItem operator<<(Log::SilentItem item, const CTxOut &) {
    return item;
}


#endif // BITCOIN_PRIMITIVES_TRANSACTION_H
