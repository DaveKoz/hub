/*
 * This file is part of the flowee project
 * Copyright (C) 2017-2018 Tom Zander <tomz@freedommail.ch>
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

#include "Engine.h"
#include <SettingsDefaults.h>
#include "BlockValidation_p.h"
#include "TxValidation_p.h"
#include "ValidationException.h"
#include <consensus/consensus.h>
#include <Application.h>
#include <checkpoints.h>
#include <init.h> // for StartShutdown
#include <main.h>
#include <txorphancache.h>
#include <policy/policy.h>
#include <timedata.h>
#include <UiInterface.h>
#include <Logger.h>
#include <validationinterface.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <consensus/merkle.h>
#include <fstream>
#include <streaming/BufferPool.h>

// #define DEBUG_BLOCK_VALIDATION
#ifdef DEBUG_BLOCK_VALIDATION
# define DEBUGBV logCritical(Log::BlockValidation)
#else
# define DEBUGBV BTC_NO_DEBUG_MACRO()
#endif

using Validation::Exception;

namespace {
/**
 * Apply the undo operation of a CTxInUndo to the given chain state.
 * @param undo The undo object.
 * @param view The coins view to which to apply the changes.
 * @param out The out point that corresponds to the tx input.
 * @return True on success.
 */
static bool applyTxInUndo(const CTxInUndo &undo, CCoinsViewCache *view, const COutPoint &out)
{
    bool clean = true;

    CCoinsModifier coins = view->ModifyCoins(out.hash);
    if (undo.nHeight != 0) {
        // undo data contains height: this is the last output of the prevout tx being spent
        if (!coins->IsPruned()) {
            if (clean)
                logCritical("Undo data overwriting existing transaction");
            clean = false;
        }
        coins->Clear();
        coins->fCoinBase = undo.fCoinBase;
        coins->nHeight = undo.nHeight;
        coins->nVersion = undo.nVersion;
    } else {
        if (coins->IsPruned()) {
            if (clean)
                logCritical("Undo data adding output to missing transaction");
            clean = false;
        }
    }
    if (coins->IsAvailable(out.n)) {
        if (clean)
            logCritical("Undo data overwriting existing output");
        clean = false;
    }
    if (coins->vout.size() < out.n+1)
        coins->vout.resize(out.n+1);
    coins->vout[out.n] = undo.txout;

    return clean;
}

}


//---------------------------------------------------------

ValidationEnginePrivate::ValidationEnginePrivate(Validation::EngineType type)
    : strand(Application::instance()->ioService()),
    shuttingDown(false),
    issuedWarningForVersion(false),
    headersInFlight(0),
    blocksInFlight(0),
    blockchain(0),
    mempool(0),
    recentTxRejects(120000, 0.000001),
    engineType(type),
    lastFullBlockScheduled(-1)
#ifdef ENABLE_BENCHMARKS
    , m_headerCheckTime(0),
    m_basicValidityChecks(0),
    m_contextCheckTime(0),
    m_utxoTime(0),
    m_validationTime(0),
    m_loadingTime(0),
    m_mempoolTime(0),
    m_walletTime(0)
#endif
{
}

/**
 * @brief All blocks offered will get back here, where we check the output of the header checks.
 * @param state the actual block and its parsing state.
 *
 * This method is called in a strand, which means we can avoid locking for our member vars
 * additionally, the state object is guarenteed (by design, not by locks) to not change any of its
 * members during this call. Except for the state atomic as further validation of transactions
 * can be happening in another thread at the same time.
 */
void ValidationEnginePrivate::blockHeaderValidated(std::shared_ptr<BlockValidationState> state)
{
    assert(strand.running_in_this_thread());
    assert(state->m_blockIndex == nullptr);
    if (state->m_block.size() < 80) {// malformed block, without header we can't report issues, just return
        state->error = "Malformed block (too short)";
        return;
    }

    struct RAII {
        RAII(const std::shared_ptr<BlockValidationState> &state)
            : m_state(state),
              finished(true) { }

        ~RAII() {
            std::shared_ptr<ValidationSettingsPrivate> settingsPriv = m_state->m_settings.lock();
            if (settingsPriv) {
                if (!error.empty())
                    settingsPriv->error = error;
                if (m_state->m_ownsIndex) {
                    settingsPriv->blockHash = m_state->m_block.createHash();
                    if (!m_state->m_blockIndex->phashBlock) {
                        // make sure a non-global index still has a working blockhash pointer
                        m_state->m_blockIndex->phashBlock = &settingsPriv->blockHash;
                    }
                } else {
                    // destructor of state will not delete index, so we can safely deref
                    settingsPriv->state.reset();
                }
                settingsPriv->setBlockIndex(m_state->m_blockIndex);
                if (finished)
                    settingsPriv->markFinished();
            }
        }

        std::shared_ptr<BlockValidationState> m_state;
        bool finished;
        std::string error;
    };
    RAII raii(state);

    if (shuttingDown)
        return;
    const bool hasFailed = state->m_validationStatus.load() & BlockValidationState::BlockInvalid;
    const FastBlock &block = state->m_block;
    assert(block.size() >= 80);
    const uint256 hash = block.createHash();

    DEBUGBV << hash << "Parent:" << state->m_block.previousBlockId() << (state->m_block.isFullBlock() ? "":"[header]");
    if (!hasFailed) {
        auto iter = blocksBeingValidated.find(hash);
        if (iter != blocksBeingValidated.end()) {
            // we are already validating the block. And since the merkle root was found OK, this means
            // that its a duplicate.
            state->m_blockIndex = iter->second->m_blockIndex;
            DEBUGBV << "  + already in the blocksBeingValidated, ignoring";
            return;
        }
    }

    // check past work.
    CBlockIndex *index = nullptr;
    if (state->m_checkMerkleRoot) // We have no proper block-hash if we don't have a merkle-root.
        index = Blocks::Index::get(hash);
    if (index) { // we already parsed it...
        state->m_blockIndex = index;
        state->m_ownsIndex = false;
        DEBUGBV << "  already known at height:" << index->nHeight;
        DEBUGBV << "  current height:" << blockchain->Height();
        if (index->nStatus & BLOCK_FAILED_MASK) {
            DEBUGBV << "  Block failed previously. Ignoring";
            raii.error = "Block failed previously, not rechecking";
            return;
        }
        DEBUGBV << "  pprev:" << index->pprev;
        if (index->pprev)
            DEBUGBV << "  pprev has completed:" << index->pprev->IsValid(BLOCK_VALID_TRANSACTIONS);
        assert(index);
        assert(index->phashBlock);
        if (blockchain->Contains(index)) { // its already in the chain...
            DEBUGBV << "  Already in the chain. Exiting";
            return;
        }
        if (index->pprev) {
            auto miPrev = blocksBeingValidated.find(block.previousBlockId());
            if (miPrev != blocksBeingValidated.end()) {
                miPrev->second->m_chainChildren.push_back(state);
                DEBUGBV << "  + adding me to parents chain-children" << miPrev->second->m_block.createHash();
            } else if (index->pprev->IsValid(BLOCK_VALID_TRANSACTIONS)) {
                state->m_validationStatus.fetch_or(BlockValidationState::BlockValidParent);
                DEBUGBV << "  Setting BlockValidParent";
            }
        }
        if (index->nHeight == 0) {
            state->m_validationStatus.fetch_or(BlockValidationState::BlockValidParent);
        }
    }

    createBlockIndexFor(state);
    index = state->m_blockIndex;
    if (hasFailed || index->nStatus & BLOCK_FAILED_MASK) {
        logInfo(Log::BlockValidation) << "Block" << hash << "rejected with error:" << state->error;
        if (!state->m_checkValidityOnly && state->m_checkMerkleRoot) {
            handleFailedBlock(state);
            if (index)
                MarkIndexUnsaved(index);
        }
        return;
    }

    state->flags = tipFlags;
    const bool receivedFromPeer = state->m_originatingNodeId  >= 0;
    bool wasRequested = false;
    if (receivedFromPeer) {
        LOCK(cs_main);
        wasRequested = IsBlockInFlight(hash);
    }

    if (receivedFromPeer && !wasRequested) {
        // Blocks that are too out-of-order needlessly limit the effectiveness of
        // pruning, because pruning will not delete block files that contain any
        // blocks which are too close in height to the tip.  Apply this test
        // regardless of whether pruning is enabled; it should generally be safe to
        // not process unrequested blocks.
        const bool tooFarAhead = (index->nHeight > int(blockchain->Height() + MIN_BLOCKS_TO_KEEP));
        if (tooFarAhead) {
            logCritical(Log::BlockValidation) << "BlockHeaderValidated: Dropping received block as it was too far ahead" << index->nHeight  << '/' <<  blockchain->Height();
            return;
        }
    }

    if (index->nHeight == -1) { // is an orphan for now.
        if (state->m_checkValidityOnly) {
            state->blockFailed(100, "Block is an orphan, can't check", Validation::RejectInternal);
        } else {
            orphanBlocks.push_back(state);
            blockLanded(ValidationEnginePrivate::CheckingHeader); // a block is no longer in flight if its in the orphans cache
            DEBUGBV << " adding it to orphan blocks" << orphanBlocks.size() << "headers in flight now;" << headersInFlight.load();
            raii.finished = false;
        }
        return;
    }

    // recursively check all orphans to see if the addition of our new block attached them to the genesis.
    std::list<std::shared_ptr<BlockValidationState> > adoptees;
    if (!state->m_checkValidityOnly)
        startOrphanWithParent(adoptees, state);
    DEBUGBV << "  Found" << adoptees.size() << "adoptees";
    if (adoptees.size())
        headersInFlight.fetch_add(adoptees.size());
#ifdef DEBUG_BLOCK_VALIDATION
    for (auto state : adoptees) {
        assert(state->m_checkingHeader);
    }
#endif

    CBlockIndex *currentHeaderTip = Blocks::DB::instance()->headerChain().Tip();
    adoptees.insert(adoptees.begin(), state);
    for (auto item : adoptees) {
        if (item->m_checkValidityOnly)
            continue;
        item->m_ownsIndex = false; // the CBlockIndex is now owned by the Blocks::DB
        item->m_blockIndex->RaiseValidity(BLOCK_VALID_TREE);

        item->m_blockIndex->phashBlock = Blocks::Index::insert(item->m_block.createHash(), item->m_blockIndex);
        MarkIndexUnsaved(item->m_blockIndex); // Save it to the DB.
        assert(item->m_blockIndex->pprev || item->m_blockIndex->nHeight == 0);
        Blocks::DB::instance()->appendHeader(item->m_blockIndex);
        DEBUGBV << "appendHeader block at height:" << item->m_blockIndex->nHeight;

        if (item->m_block.isFullBlock()) {
            assert(!item->m_block.transactions().empty()); // assume we already had findTransactions called before here.
            item->m_blockIndex->nTx = item->m_block.transactions().size();
            try { // Write block to history file
                if (item->m_onResultFlags & Validation::SaveGoodToDisk) {
                    item->m_block = Blocks::DB::instance()->writeBlock(item->m_block, item->m_blockPos);
                }
                if (!item->m_blockPos.IsNull()) {
                    item->m_blockIndex->nDataPos = item->m_blockPos.nPos;
                    item->m_blockIndex->nFile = item->m_blockPos.nFile;
                    item->m_blockIndex->nStatus |= BLOCK_HAVE_DATA;
                }
            } catch (const std::exception &e) {
                fatal(e.what());
            }
        }
    }

    CBlockIndex *tip = Blocks::DB::instance()->headerChain().Tip();
    assert(tip);
    if (currentHeaderTip != tip)
        logCritical(Log::BlockValidation).nospace() << "new best header=" << *tip->phashBlock << " height=" << tip->nHeight << " orphans=" << orphanBlocks.size();

    if (currentHeaderTip && !Blocks::DB::instance()->headerChain().Contains(currentHeaderTip)) { // re-org happened in headers.
        logInfo(Log::BlockValidation) << "Header-reorg detected. Old-tip" << *currentHeaderTip->phashBlock << "@" << currentHeaderTip->nHeight;
        prepareChain();
        lastFullBlockScheduled = -1;
        findMoreJobs();
        return;
    }

    const int diff = index->nHeight - blockchain->Height();
    if (diff < blocksInFlightLimit()) { // if block is recent, then continue immediately.
        bool forward = false;
        bool first = true;
        // adoptees are sorted
        for (auto iter = adoptees.cbegin(); iter != adoptees.cend(); ++iter) {
            const std::shared_ptr<BlockValidationState>&item = *iter;
            if (!item->m_checkValidityOnly && !Blocks::DB::instance()->headerChain().Contains(item->m_blockIndex))
                continue;
            if (first) {
                first = false;
                // Check the first blocks BlockValidTree by checking its parents are all Ok.
                assert(item->m_blockIndex);
                if (item->m_blockIndex->nHeight <= 1 || blockchain->Contains(item->m_blockIndex->pprev)) {
                    forward = true;
                } else {
                    auto iter = blocksBeingValidated.find(item->m_blockIndex->pprev->GetBlockHash());
                    if (iter != blocksBeingValidated.end()) {
                        if (iter->second->m_validationStatus.load() & BlockValidationState::BlockValidTree) {
                            forward = true;
                        }
                    }
                }
            }
            forward = forward && item->m_block.isFullBlock();

            if (forward) {
                if (!item->m_checkValidityOnly) {
                    DEBUGBV << "moving block on to checks2. Block at height:" << item->m_blockIndex->nHeight;
                    blocksBeingValidated.insert(std::make_pair(item->m_block.createHash(), item));
                    lastFullBlockScheduled = std::max(lastFullBlockScheduled, item->m_blockIndex->nHeight);
                }

                item->m_validationStatus.fetch_or(BlockValidationState::BlockValidTree);
                item->m_coinView.reset(new CCoinsViewCache(mempool->coins()));
                Application::instance()->ioService().post(std::bind(&BlockValidationState::checks2HaveParentHeaders, item));
            }
        }
        raii.finished = !forward;
    }
}

void ValidationEnginePrivate::createBlockIndexFor(const std::shared_ptr<BlockValidationState> &state)
{
    DEBUGBV << state->m_block.createHash();
    if (state->m_blockIndex)
        return;
    const FastBlock &block = state->m_block;
    CBlockIndex *index = new CBlockIndex();
    state->m_blockIndex = index;
    state->m_ownsIndex = true;
    index->nHeight = -1;
    index->nVersion = block.blockVersion();
    index->hashMerkleRoot = block.merkleRoot();
    index->nBits = block.bits();
    index->nTime = block.timestamp();
    index->nNonce = block.nonce();
    index->nFile = state->m_blockPos.nFile;
    index->nStatus = BLOCK_VALID_HEADER;
    if (!state->m_blockPos.IsNull()) // likely found during reindex
        index->nStatus |= BLOCK_HAVE_DATA;

    auto miPrev = blocksBeingValidated.find(block.previousBlockId());
    bool hasKnownParent = miPrev != blocksBeingValidated.end();
    if (hasKnownParent) {
        index->pprev = miPrev->second->m_blockIndex;
        miPrev->second->m_chainChildren.push_back(state);
    } else {
        index->pprev = Blocks::Index::get(block.previousBlockId());
        hasKnownParent = index->pprev;
        if (hasKnownParent) {
            if (index->pprev->IsValid(BLOCK_VALID_TRANSACTIONS))
                state->m_validationStatus.fetch_or(BlockValidationState::BlockValidParent);
        } else {
            for (auto *headersTip : Blocks::DB::instance()->headerChainTips()) {
                if (headersTip->GetBlockHash() == block.previousBlockId()) {
                    index->pprev = headersTip;
                    break;
                }
            }
        }
    }
    if (index->pprev && index->pprev->nHeight != -1) {
        index->nHeight = index->pprev->nHeight + 1;
        index->nChainWork = index->pprev->nChainWork + GetBlockProof(*index);
        index->BuildSkip();
        DEBUGBV << "   block" << index->nHeight << "has pprev";
        if (index->pprev->nStatus & BLOCK_FAILED_MASK) {
            DEBUGBV << "      + Which failed!";
            index->nStatus |= BLOCK_FAILED_CHILD;
            state->blockFailed(0, "bad-parent", Validation::RejectInvalid);
        }
    }
    else if (index->pprev == nullptr && block.createHash() == Params().GetConsensus().hashGenesisBlock) {
        index->nHeight = 0;
        state->m_validationStatus.fetch_or(BlockValidationState::BlockValidParent);
        DEBUGBV.nospace() << "   is genesis block (height=" << index->nHeight << ")";
    }
}

/**
 * This method is called when the validation engine is in the process of shutting down.
 * The validation engine has shared pointers to the State objects that are being validated,
 * clearing those will causse the validation to stop, which is the wanted effect.
 * The settings objects also have a shared pointer to the State objects, so we make all
 * of those error out in order to entice other parts of the app to also delete those Settings
 * objects, which will delete the State objects and then when all State objects are deleted
 * will the BlockValidationPrivate finally be deleted too.
 */
void ValidationEnginePrivate::cleanup()
{
    assert(strand.running_in_this_thread());
    assert(shuttingDown);
    auto iter = orphanBlocks.begin();
    while (iter != orphanBlocks.end()) {
        const std::shared_ptr<BlockValidationState> &orphan = *iter;
        auto settings = orphan->m_settings.lock();
        if (settings) {
            settings->error = "shutdown";
            settings->markFinished();
        }
        ++iter;
    }
    orphanBlocks.clear();
    auto iter2 = blocksBeingValidated.begin();
    while (iter2 != blocksBeingValidated.end()) {
        const std::shared_ptr<BlockValidationState> &block = iter2->second;
        auto settings = block->m_settings.lock();
        if (settings) {
            settings->error = "shutdown";
            settings->markFinished();
        }
        ++iter2;
    }
    blocksBeingValidated.clear();
    std::unique_lock<decltype(lock)> waitLock(lock);
    waitVariable.notify_all();
}

/**
 * @brief We have a block that has tracable ancestry to our genesis. We start processing it.
 * This first starts by finding all the orphans that now can be de-orphaned because the block
 * might be their parent.
 *
 * Additionally, we now can look at the POW to see how this block relates to the main-chain.
 * @param state the block.
 */
void ValidationEnginePrivate::startOrphanWithParent(std::list<std::shared_ptr<BlockValidationState> > &adoptedItems, const std::shared_ptr<BlockValidationState> &state)
{
    assert(strand.running_in_this_thread());
    const uint256 hash = state->m_block.createHash();
    DEBUGBV << state->m_block.createHash();

    std::list<std::shared_ptr<BlockValidationState> > adoptees;
    auto iter = orphanBlocks.begin();
    while (iter != orphanBlocks.end()) {
        const std::shared_ptr<BlockValidationState> &orphan = *iter;
        if (orphan->m_block.previousBlockId() == hash) {
            adoptees.push_back(orphan);
            iter = orphanBlocks.erase(iter);
        } else {
            ++iter;
        }
    }

    // remember them.
    iter = adoptees.begin();
    while (iter != adoptees.end()) {
        adoptedItems.push_back(*iter) ;
        ++iter;
    }

    // due to recursive-ness, act on results outside of the iterator of the orphanBlocks list.
    iter = adoptees.begin();
    while (iter != adoptees.end()) {
        std::shared_ptr<BlockValidationState> orphan = *iter;
        DEBUGBV << "    + orphan:" << orphan->m_block.createHash();
        bool alreadyThere = false;
        for (auto child : state->m_chainChildren) {
            if (child.lock() == orphan) {
                alreadyThere = true;
                break;
            }
        }
        if (!alreadyThere)
            state->m_chainChildren.push_back(std::weak_ptr<BlockValidationState>(orphan));
        CBlockIndex *index = orphan->m_blockIndex;
        index->pprev = state->m_blockIndex;
        index->nHeight = state->m_blockIndex->nHeight + 1;
        index->nChainWork = state->m_blockIndex->nChainWork + GetBlockProof(*index);
        index->BuildSkip();

        startOrphanWithParent(adoptedItems, orphan);
        ++iter;
    }
}

/*
 * When a block gets passed to this method we know  the block is fully validated for
 * correctness, and so are all of the parent blocks.
 *
 * We don't know yet if the transactions inputs are actually unspent, a validation we keep
 * back because we need to have an up to date UTXO first.
 */
void ValidationEnginePrivate::processNewBlock(std::shared_ptr<BlockValidationState> state)
{
    assert(strand.running_in_this_thread());
    if (shuttingDown)
        return;
    if (state->m_blockIndex == 0) // already handled.
        return;

    struct RAII {
        uint256 m_hash;
        ValidationEnginePrivate *m_parent;
        std::shared_ptr<ValidationSettingsPrivate> m_priv;

        RAII(ValidationEnginePrivate *parent, const std::shared_ptr<BlockValidationState> &state)
            : m_hash(state->m_block.createHash()), m_parent(parent), m_priv(state->m_settings.lock()) { }
        ~RAII() {
            m_parent->blocksBeingValidated.erase(m_hash);
            if (m_priv)
                m_priv->markFinished();
        }
    };
    RAII raii(this, state);

    if (state->m_checkValidityOnly)
        return;

    CBlockIndex *index = state->m_blockIndex;
    const uint256 hash = state->m_block.createHash();
    DEBUGBV << hash.ToString() << state->m_blockIndex->nHeight;
    DEBUGBV << "   chain:" << blockchain->Height();

    assert(blockchain->Height() == -1 || index->nChainWork >= blockchain->Tip()->nChainWork); // the new block has more POW.

    const bool isNextChainTip = index->nHeight <= blockchain->Height() + 1; // If a parent was rejected for some reason, this is false
    const bool blockValid = isNextChainTip && (state->m_validationStatus.load() & BlockValidationState::BlockInvalid) == 0 ;
    bool addToChain = blockValid && Blocks::DB::instance()->headerChain().Contains(index);
    try {
        if (!isNextChainTip)
            index->nStatus |= BLOCK_FAILED_CHILD;
        if (addToChain) {
            CCoinsViewCache *view = state->m_coinView.get();

            DEBUGBV << "UTXO best block is" << view->GetBestBlock() << "my parent is" <<  state->m_block.previousBlockId();
            if (view->GetBestBlock() != state->m_block.previousBlockId()) {
                throw Exception("View DB inconsistent!");
            }
            assert(view->GetBestBlock() == state->m_block.previousBlockId());

            index->nChainTx = index->nTx + (index->pprev ? index->pprev->nChainTx : 0); // pprev is only null if this is the genesisblock.

            index->RaiseValidity(BLOCK_VALID_CHAIN);

            if (index->nHeight == 0) { // is genesis block
                view->SetBestBlock(hash);
                blockchain->SetTip(index);
                index->RaiseValidity(BLOCK_VALID_SCRIPTS); // done
                const bool flushOk = view->Flush();
                if (!flushOk)
                    fatal("Failed to flush UTXO to disk.");
                state->signalChildren();
            } else {
                std::vector<std::pair<uint256, CDiskTxPos> > vPos;
                if (fTxIndex) {
                    vPos.reserve(state->m_block.transactions().size());
                    CDiskTxPos pos(index->GetBlockPos(), GetSizeOfCompactSize(state->m_block.transactions().size()));
                    const std::vector<Tx> &transactions = state->m_block.transactions();
                    for (size_t i = 0; i < transactions.size(); ++i) {
                        const Tx &tx = transactions.at(i);
                        vPos.push_back(std::make_pair(tx.createHash(), pos));
                        pos.nTxOffset += tx.size();
                    }
                }
                const uint64_t maxSigOps = Policy::blockSigOpAcceptLimit(state->m_block.size());
                if (state->m_sigOpsCounted > maxSigOps)
                    throw Exception("bad-blk-sigops");

                CBlock block = state->m_block.createOldBlock();
                if (state->flags.enableValidation) {
                    CAmount blockReward = state->m_blockFees.load() + GetBlockSubsidy(index->nHeight, Params().GetConsensus());
                    if (block.vtx[0].GetValueOut() > blockReward)
                        throw Exception("bad-cb-amount");
                }
                if (fTxIndex) {
                    if (!Blocks::DB::instance()->WriteTxIndex(vPos))
                        fatal("Failed to write transaction index");
                }

                uint32_t *bench = nullptr;
#ifdef ENABLE_BENCHMARKS
                int64_t end, start; start = GetTimeMicros();
                uint32_t utxoTime;
                bench = &utxoTime;
#endif
                view->SetBestBlock(state->m_block.createHash());
                std::list<CTransaction> txConflicted;
                mempool->removeForBlock(block.vtx, index->nHeight, txConflicted, orphanBlocks.size() > 3, view, bench);
                index->RaiseValidity(BLOCK_VALID_SCRIPTS); // done
                state->signalChildren(); // start tx-validation of next one.

                blockchain->SetTip(index);
                tip.store(index);
                mempool->AddTransactionsUpdated(1);
                cvBlockChange.notify_all();
#ifdef ENABLE_BENCHMARKS
                end = GetTimeMicros();
                m_utxoTime.fetch_add(utxoTime);
                m_mempoolTime.fetch_add(end - start - utxoTime);
                start = end;
#endif
                {
                    std::lock_guard<std::mutex> rejects(recentRejectsLock);
                    recentTxRejects.reset();
                }

                // Tell wallet about transactions that went from mempool to conflicted:
                Streaming::BufferPool pool;
                for(const CTransaction &tx : txConflicted) {
                    ValidationNotifier().SyncTransaction(tx);
                    ValidationNotifier().SyncTx(Tx::fromOldTransaction(tx, &pool));
                }
                ValidationNotifier().SyncAllTransactionsInBlock(state->m_block); // ... and about transactions that got confirmed:
                ValidationNotifier().SyncAllTransactionsInBlock(&block);

#ifdef ENABLE_BENCHMARKS
                end = GetTimeMicros();
                m_walletTime.fetch_add(end - start);
#endif
            }
        } else {
            logDebug(Log::BlockValidation) << "Not appending: isNextChainTip" << isNextChainTip << "blockValid:" << blockValid << "addToChain" << addToChain;
        }
    } catch (const Exception &e) {
        state->blockFailed(100, e.what(), e.rejectCode(), e.corruptionPossible());
        addToChain = false;
    }

    if (state->m_validationStatus.load() & BlockValidationState::BlockInvalid) {
        logCritical(Log::BlockValidation) << "block failed validation" << state->error << index->nHeight << hash;
        if (index->pprev == nullptr) // genesis block, all bets are off after this
            return;
        handleFailedBlock(state);
        if (state->m_blockIndex->nHeight == lastFullBlockScheduled)
            --lastFullBlockScheduled;
    }

    chainTipChildren = state->m_chainChildren;
    state->m_blockIndex = 0;
    MarkIndexUnsaved(index);
    if (!addToChain)
        return;

    // Check the BIP9 activation options and generate possible warnings.
    if (!IsInitialBlockDownload()) {
        int nUpgraded = 0;
        const CBlockIndex* pindex = index;
        for (int bit = 0; bit < VERSIONBITS_NUM_BITS; bit++) {
            WarningBitsConditionChecker checker(bit);
            ThresholdState state = checker.GetStateFor(pindex, Params().GetConsensus(), warningcache[bit]);
            if (state == THRESHOLD_ACTIVE) {
                strMiscWarning = strprintf(_("Warning: unknown new rules activated (versionbit %i)"), bit);
                if (!issuedWarningForVersion) {
                    AlertNotify(strMiscWarning, true);
                    issuedWarningForVersion = true;
                }
            } else if (state == THRESHOLD_LOCKED_IN) {
                logInfo(Log::BlockValidation).nospace() << "unknown new rules are about to activate (versionbit " << bit << ")";
            }
        }
        for (int i = 0; i < 100 && pindex != NULL; i++) {
            int32_t nExpectedVersion = WarningBitsConditionChecker::computeBlockVersion(pindex->pprev);
            if (pindex->nVersion > VERSIONBITS_LAST_OLD_BLOCK_VERSION && (pindex->nVersion & ~nExpectedVersion) != 0)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 20)
            logInfo(Log::BlockValidation) << nUpgraded << "of last 100 blocks have unexpected version";
        if (nUpgraded > 100/2) {
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: Unknown block versions being mined! It's possible unknown rules are in effect");
            if (!issuedWarningForVersion) {
                AlertNotify(strMiscWarning, true);
                issuedWarningForVersion = true;
            }
        }
    }

    if (state->flags.uahfRules && Application::uahfChainState() == Application::UAHFWaiting) {
        Application::setUahfChainState(Application::UAHFRulesActive);
        // next block is the big, fork-block.
    } else if (state->flags.uahfRules && Application::uahfChainState() == Application::UAHFRulesActive) {
        logInfo(8002) << "UAHF block found that activates the chain" << state->m_block.createHash();
        // enable UAHF (aka BCC) on first block after the calculated timestamp
        Application::setUahfChainState(Application::UAHFActive);
    }
    tipFlags = state->flags;

    CValidationState val;
    if (!FlushStateToDisk(val, FLUSH_STATE_IF_NEEDED))
        fatal(val.GetRejectReason().c_str());

    logCritical(Log::BlockValidation).nospace() << "new best=" << hash << " height=" << index->nHeight
            << " tx=" << blockchain->Tip()->nChainTx
            << " date=" << DateTimeStrFormat("%Y-%m-%d %H:%M:%S", index->GetBlockTime()).c_str()
            << Log::Fixed << Log::precision(1) << " cache=" << mempool->coins()->DynamicMemoryUsage() * (1.0 / (1<<20))
            << "MiB(" << mempool->coins()->GetCacheSize() << "txo)";
#ifdef ENABLE_BENCHMARKS
    if ((index->nHeight % 1000) == 0) {
        logCritical(Log::Bench) << "Times. Header:" << m_headerCheckTime
                                << "Structure:" << m_basicValidityChecks
                                << "Context:" << m_contextCheckTime
                                << "UTOX:" << m_utxoTime
                                << "validation:" << m_validationTime
                                << "loading:" << m_loadingTime
                                << "mempool:" << m_mempoolTime
                                << "wallet:" << m_walletTime;
    }
    int64_t start = GetTimeMicros();
#endif
    mempool->check();
    uiInterface.NotifyBlockTip(orphanBlocks.size() > 3, index);
    {
        LOCK(cs_main);
        ValidationNotifier().UpdatedTransaction(hashPrevBestCoinBase);
    }
    hashPrevBestCoinBase = state->m_block.transactions().at(0).createHash();
#ifdef ENABLE_BENCHMARKS
    m_mempoolTime.fetch_add(GetTimeMicros() - start);
#endif
    if (state->m_onResultFlags & Validation::ForwardGoodToPeers) {
        int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate(Params().Checkpoints());
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes) {
            if (blockchain->Height() > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate)) {
                pnode->PushBlockHash(hash);
            }
        }
    }
}

void ValidationEnginePrivate::handleFailedBlock(const std::shared_ptr<BlockValidationState> &state)
{
    assert(strand.running_in_this_thread());
    state->recursivelyMark(BlockValidationState::BlockInvalid);
    if (!state->isCorruptionPossible && state->m_blockIndex && state->m_checkMerkleRoot) {
        auto index = state->m_blockIndex;
        state->m_blockIndex->nStatus |= BLOCK_FAILED_VALID;
        // Mark all children as failed too
        for (auto tip : Blocks::DB::instance()->headerChainTips()) {
            if (tip->GetAncestor(index->nHeight) == index) {
                while (tip != index) {
                    tip->nStatus |= BLOCK_FAILED_CHILD;
                    MarkIndexUnsaved(tip);
                    tip = tip->pprev;
                }
            }
        }
        if (index->phashBlock == nullptr) {
            // transfer ownership so we can remember this failed block.
            index->phashBlock = Blocks::Index::insert(state->m_block.createHash(), index);
            state->m_ownsIndex = false;
            MarkIndexUnsaved(index);
        }

        auto currentHeaderTip = Blocks::DB::instance()->headerChain().Tip();
        const bool changed = Blocks::DB::instance()->appendHeader(index); // Processes that the block actually is invalid.
        if (changed) {
            auto tip = Blocks::DB::instance()->headerChain().Tip();
            logCritical(Log::BlockValidation).nospace() << "new best header=" << *tip->phashBlock << " height=" << tip->nHeight;
            logInfo(Log::BlockValidation) << "Header-reorg detected. Old-tip" << *currentHeaderTip->phashBlock << "@" << currentHeaderTip->nHeight;
            prepareChain();
        }
    }

    if (state->m_originatingNodeId >= 0) {
        LOCK(cs_main);
        if (state->errorCode < 0xFF)
            queueRejectMessage(state->m_originatingNodeId, state->m_block.createHash(),
                    static_cast<std::uint8_t>(state->errorCode), state->error);
        if (state->m_onResultFlags & Validation::PunishBadNode)
            Misbehaving(state->m_originatingNodeId, state->punishment);
    }

    // TODO rememeber to ignore this blockhash in the 'recently failed' list
}

/*
 * The 'main chain' is determined by the Blocks::DB::headersChain()
 * This method does nothing more than update the real chain to remove blocks that are
 * no longer on the headersChain (due to reorgs, mostly).
 */
void ValidationEnginePrivate::prepareChain()
{
    if (blockchain->Height() <= 0)
        return;
    if (Blocks::DB::instance()->headerChain().Contains(blockchain->Tip()))
        return;

    std::vector<FastBlock> revertedBlocks;

    LOCK(mempool->cs);
    CCoinsViewCache view(mempool->coins());
    while (!Blocks::DB::instance()->headerChain().Contains(blockchain->Tip())) {
        CBlockIndex *index = blockchain->Tip();
        DEBUGBV << "Removing (rollback) chain tip at" << index->nHeight << index->GetBlockHash();
        FastBlock block;
        try {
            block = Blocks::DB::instance()->loadBlock(index->GetBlockPos());
            revertedBlocks.push_back(block);
        } catch (const std::runtime_error &error) {
            logFatal(Log::BlockValidation) << "ERROR: Can't undo the tip because I can't find it on disk";
            fatal(error.what());
        }
        if (block.size() == 0)
            fatal("BlockValidationPrivate::prepareChainForBlock: got no block, can't continue.");
        if (!disconnectTip(block, index, &view))
            fatal("Failed to disconnect block");

        blockchain->SetTip(index->pprev);
        tip.store(index->pprev);
    }
    view.Flush();
    mempool->removeForReorg(blockchain->Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);

    // Add transactions. Only after we have flushed our removal of transactions from the UTXO view.
    // Otherwise the mempool would object because they would be in conflict with themselves.
    Streaming::BufferPool pool;
    for (int index = revertedBlocks.size() - 1; index >= 0; --index) {
        FastBlock block = revertedBlocks.at(index);
        block.findTransactions();
        for (size_t txIndex = 1; txIndex < block.transactions().size(); txIndex++) {
            Tx tx = block.transactions().at(txIndex);
            std::list<CTransaction> deps;
            mempool->remove(tx.createOldTransaction(), deps, true);

            std::shared_ptr<TxValidationState> state(new TxValidationState(me, tx, TxValidationState::FromMempool));
            state->checkTransaction();

            for (CTransaction tx2 : deps) {// dependent transactions
                state.reset(new TxValidationState(me, Tx::fromOldTransaction(tx2, &pool), TxValidationState::FromMempool));
                state->checkTransaction();
            }
            // Let wallets know transactions went from 1-confirmed to
            // 0-confirmed or conflicted:
            ValidationNotifier().SyncTransaction(tx.createOldTransaction());
            ValidationNotifier().SyncTx(tx);
        }
    }
    mempool->AddTransactionsUpdated(1);
    LimitMempoolSize(*mempool, GetArg("-maxmempool", Settings::DefaultMaxMempoolSize) * 1000000, GetArg("-mempoolexpiry", Settings::DefaultMempoolExpiry) * 60 * 60);
}

void ValidationEnginePrivate::fatal(const char *error)
{
    logFatal(Log::Bitcoin) << "***" << error;
    StartShutdown();
    throw std::runtime_error("App stopping, killing task");
}

void ValidationEnginePrivate::blockLanded(ProcessingType type)
{
    std::unique_lock<decltype(lock)> waitLock(lock);
    int beforeCount;
    if (type == CheckingHeader)
        beforeCount = headersInFlight.fetch_sub(1);
    else
        beforeCount = blocksInFlight.fetch_sub(1);

    DEBUGBV << "headers:" << headersInFlight << "blocks:" << blocksInFlight << "orphans" << orphanBlocks.size();
    if (beforeCount <= blocksInFlightLimit()) {
        waitVariable.notify_all();
        if (!shuttingDown)
            strand.post(std::bind(&ValidationEnginePrivate::findMoreJobs, me.lock()));
    }
}

void ValidationEnginePrivate::findMoreJobs()
{
    assert(strand.running_in_this_thread());
    DEBUGBV << "last scheduled:" << lastFullBlockScheduled;
    if (shuttingDown || engineType == Validation::SkipAutoBlockProcessing)
        return;
    if (lastFullBlockScheduled == -1)
        lastFullBlockScheduled = std::max(1, blockchain->Height());
    while (true) {
        CBlockIndex *index = Blocks::DB::instance()->headerChain()[lastFullBlockScheduled + 1];
        DEBUGBV << "  next:" << index;
        if (index) DEBUGBV << "  has data:" << (bool) (index->nStatus & BLOCK_HAVE_DATA);
        if (!(index && (index->nStatus & BLOCK_HAVE_DATA)))
            return;
        assert(index->pprev);
        assert(index->nHeight == lastFullBlockScheduled + 1);
        int currentCount = blocksInFlight.load();
        if (currentCount >= blocksInFlightLimit())
            return;
        int newCount = currentCount + 1;
        if (!blocksInFlight.compare_exchange_weak(currentCount, newCount, std::memory_order_relaxed, std::memory_order_relaxed))
            continue;
        // If we have 1008 validated headers on top of the block, turn off loads of validation of the actual block.
        const bool enableValidation = index->nHeight + 1008 > Blocks::DB::instance()->headerChain().Height();
        int onResultFlags = enableValidation ? Validation::ForwardGoodToPeers : 0;
        if ((index->nStatus & BLOCK_HAVE_UNDO) == 0)
            onResultFlags |= Validation::SaveGoodToDisk;
        std::shared_ptr<BlockValidationState> state = std::make_shared<BlockValidationState>(me, FastBlock(), onResultFlags);
        state->m_blockIndex = index;
        state->m_blockPos = index->GetBlockPos();
        try {
            state->load();
            if (state->m_block.size() <= 90)
                throw std::runtime_error("Excpected full block");
        } catch (const std::runtime_error &e) {
            logWarning(Log::BlockValidation) << "Failed to load block" << state->m_blockPos << "got exception:" << e;
            index->nStatus ^= BLOCK_HAVE_DATA; // obviously not...
            return;
        }
        state->flags.enableValidation = enableValidation;
        state->m_validationStatus = BlockValidationState::BlockValidHeader | BlockValidationState::BlockValidTree;
        state->m_coinView.reset(new CCoinsViewCache(mempool->coins()));
        state->m_checkingHeader = false;
        blocksBeingValidated.insert(std::make_pair(state->m_block.createHash(), state));

        auto iter = blocksBeingValidated.find(state->m_block.previousBlockId());
        if (iter != blocksBeingValidated.end()) {
            iter->second->m_chainChildren.push_back(state);
        } else if (index->pprev->nChainTx != 0) {
            state->m_validationStatus |= BlockValidationState::BlockValidParent;
        }
        DEBUGBV << "scheduling" <<  lastFullBlockScheduled + 1 << "for validation"
                << (state->flags.enableValidation ? "(full)" : "(shallow)") << "Blocks in flight:" << newCount;
        Application::instance()->ioService().post(std::bind(&BlockValidationState::checks2HaveParentHeaders, state));
        ++lastFullBlockScheduled;
    }
}

bool ValidationEnginePrivate::disconnectTip(const FastBlock &tip, CBlockIndex *index, CCoinsViewCache *view, bool *userClean, bool *error)
{
    assert(index);
    assert(view);
    assert(tip.createHash() == view->GetBestBlock());

    CDiskBlockPos pos = index->GetUndoPos();
    if (pos.IsNull()) {
        logFatal(Log::BlockValidation) << "No undo data available to disconnectBlock";
        if (error) *error = true;
        return false;
    }
    FastUndoBlock blockUndoFast = Blocks::DB::instance()->loadUndoBlock(pos, index->pprev->GetBlockHash());
    if (blockUndoFast.size() == 0) {
        logFatal(Log::BlockValidation) << "Failed reading undo data";
        if (error) *error = true;
        return false;
    }
    CBlockUndo blockUndo = blockUndoFast.createOldBlock();
    CBlock block = tip.createOldBlock();
    if (blockUndo.vtxundo.size() + 1 != block.vtx.size()) {
        logFatal(Log::BlockValidation) << "block and undo data inconsistent";
        if (error) *error = true;
        return false;
    }

    bool clean = true;

    // undo transactions in reverse order
    for (int i = block.vtx.size() - 1; i >= 0; i--) {
        const CTransaction &tx = block.vtx[i];
        const uint256 hash = tx.GetHash();

        // Check that all outputs are available and match the outputs in the block itself
        // exactly.
        {
        CCoinsModifier outs = view->ModifyCoins(hash);
        outs->ClearUnspendable();

        CCoins outsBlock(tx, index->nHeight);
        // The CCoins serialization does not serialize negative numbers.
        // No network rules currently depend on the version here, so an inconsistency is harmless
        // but it must be corrected before txout nversion ever influences a network rule.
        if (outsBlock.nVersion < 0)
            outs->nVersion = outsBlock.nVersion;
        if (*outs != outsBlock) {
            if (clean)
                logCritical(Log::BlockValidation) << "added transaction mismatch? database corruption suspected";
            clean = false;
        }

        // remove outputs
        outs->Clear();
        }

        // restore inputs
        if (i > 0) { // not coinbases
            const CTxUndo &txundo = blockUndo.vtxundo[i-1];
            if (txundo.vprevout.size() != tx.vin.size()) {
                logCritical(Log::BlockValidation) << "transaction and undo data inconsistent";
                clean = false;
            }
            for (unsigned int j = tx.vin.size(); j-- > 0;) {
                const COutPoint &out = tx.vin[j].prevout;
                const CTxInUndo &undo = txundo.vprevout[j];
                if (!applyTxInUndo(undo, view, out))
                    clean = false;
            }
        }
    }

    // move best block pointer to prevout block
    view->SetBestBlock(index->pprev->GetBlockHash());

    if (userClean) {
        *userClean = clean;
        return true;
    }

    return clean;
}


//---------------------------------------------------------

ValidationFlags::ValidationFlags()
    : strictPayToScriptHash(false),
    enforceBIP30(false),
    enableValidation(true),
    scriptVerifyDerSig(false),
    scriptVerifyLockTimeVerify(false),
    scriptVerifySequenceVerify(false),
    nLocktimeVerifySequence(false),
    uahfRules(false)
{
}

uint32_t ValidationFlags::scriptValidationFlags() const
{
    uint32_t flags = strictPayToScriptHash ? SCRIPT_VERIFY_P2SH : SCRIPT_VERIFY_NONE;
    if (scriptVerifyDerSig)
        flags |= SCRIPT_VERIFY_DERSIG;
    if (scriptVerifyLockTimeVerify)
        flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    if (scriptVerifySequenceVerify)
        flags |= SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;
    if (uahfRules) {
        flags |= SCRIPT_VERIFY_STRICTENC;
        flags |= SCRIPT_ENABLE_SIGHASH_FORKID;
    }
    return flags;
}

void ValidationFlags::updateForBlock(CBlockIndex *index, const uint256 &blkHash)
{
    if (index->pprev == nullptr) // skip for genesis block
        return;

    // BIP16 didn't become active until Apr 1 2012
    const int64_t BIP16SwitchTime = 1333238400;
    strictPayToScriptHash = (index->nTime >= BIP16SwitchTime);


    // Do not allow blocks that contain transactions which 'overwrite' older transactions,
    // unless those are already completely spent.
    // If such overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance -- even after
    // being sent to another address.
    // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
    // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
    // already refuses previously-known transaction ids entirely.
    // This rule was originally applied to all blocks with a timestamp after March 15, 2012, 0:00 UTC.
    // Now that the whole chain is irreversibly beyond that time it is applied to all blocks except the
    // two in the chain that violate it. This prevents exploiting the issue against nodes during their
    // initial block download.
    enforceBIP30 =
            !((index->nHeight==91842 && blkHash == uint256S("0x00000000000a4d0a398161ffc163c503763b1f4360639393e0e4c8e300e0caec"))
              || (index->nHeight==91880 && blkHash == uint256S("0x00000000000743f190a18c5577a3c2d2a1f610ae9601ac046a38084ccb7cd721")));

    const auto chainparams = Params();

    // Once BIP34 activated it was not possible to create new duplicate coinbases and thus other than starting
    // with the 2 existing duplicate coinbase pairs, not possible to create overwriting txs.  But by the
    // time BIP34 activated, in each of the existing pairs the duplicate coinbase had overwritten the first
    // before the first had been spent.  Since those coinbases are sufficiently buried its no longer possible to create further
    // duplicate transactions descending from the known pairs either.
    // If we're on the known chain at height greater than where BIP34 activated, we can save the db accesses needed for the BIP30 check.
    CBlockIndex *pindexBIP34height = index->pprev->GetAncestor(chainparams.GetConsensus().BIP34Height);
    //Only continue to enforce if we're below BIP34 activation height or the block hash at that height doesn't correspond.
    enforceBIP30 = enforceBIP30 && (!pindexBIP34height || !(pindexBIP34height->GetBlockHash() == chainparams.GetConsensus().BIP34Hash));

    // Start enforcing the DERSIG (BIP66) rules, for block.nVersion=3 blocks,
    // when 75% of the network has upgraded:
    if (scriptVerifyDerSig
            || (index->nVersion >= 3
                && IsSuperMajority(3, index->pprev, chainparams.GetConsensus().nMajorityEnforceBlockUpgrade, chainparams.GetConsensus())))
        scriptVerifyDerSig = true;

    // Start enforcing CHECKLOCKTIMEVERIFY, (BIP65) for block.nVersion=4
    // blocks, when 75% of the network has upgraded:
    if (scriptVerifyLockTimeVerify
            || (index->nVersion >= 4
                && IsSuperMajority(4, index->pprev, chainparams.GetConsensus().nMajorityEnforceBlockUpgrade, chainparams.GetConsensus())))
        scriptVerifyLockTimeVerify = true;

    if (scriptVerifySequenceVerify == false) {
        LOCK(cs_main); // for versionBitsState :(
        // Start enforcing BIP68 (sequence locks) and BIP112 (CHECKSEQUENCEVERIFY) using versionbits logic.
        nLocktimeVerifySequence = 0;
        if (VersionBitsState(index->pprev, chainparams.GetConsensus(), Consensus::DEPLOYMENT_CSV, versionbitscache) == THRESHOLD_ACTIVE) {
            scriptVerifySequenceVerify = true;
            nLocktimeVerifySequence = true;
        }
    }

    if (uahfRules
            || ((Application::uahfChainState() == Application::UAHFWaiting
                 && index->GetMedianTimePast() >= Application::uahfStartTime())
                || Application::uahfChainState() >= Application::UAHFRulesActive)) {
        uahfRules = true;
    }
}

/* TODO Expire orphans.
 * Have a bool in the State object.
 * Have a once-an-hour timer which walks over all orphans. Either set the bool, or delete the orphan if its bool is already set.
 * Unset issuedWarningForVersion every so many hours.
 */

//---------------------------------------------------------

BlockValidationState::BlockValidationState(const std::weak_ptr<ValidationEnginePrivate> &parent, const FastBlock &block, uint32_t onResultFlags, int originatingNodeId)
    : m_block(block),
      m_blockIndex(nullptr),
      m_onResultFlags(onResultFlags),
      m_originatingNodeId(originatingNodeId),
      m_txChunkLeftToStart(-1),
      m_txChunkLeftToFinish(-1),
      m_validationStatus(BlockValidityUnknown),
      m_blockFees(0),
      m_parent(parent)
{
}

BlockValidationState::~BlockValidationState()
{
    std::shared_ptr<ValidationEnginePrivate> parent = m_parent.lock();
    if (parent)
        parent->blockLanded(m_checkingHeader ? ValidationEnginePrivate::CheckingHeader : ValidationEnginePrivate::CheckingBlock);
    if (m_originatingNodeId != -1 && m_block.isFullBlock()) {
        LOCK(cs_main);
        MarkBlockAsReceived(m_block.createHash());
    }
    if (m_ownsIndex)
        delete m_blockIndex;
    if (m_block.size() >= 80)
        DEBUGBV << "Finished" << m_block.createHash();
}

void BlockValidationState::load()
{
#ifdef ENABLE_BENCHMARKS
    int64_t start = GetTimeMicros();
#endif
    m_block = Blocks::DB::instance()->loadBlock(m_blockPos);
#ifdef ENABLE_BENCHMARKS
    int64_t end = GetTimeMicros();
    auto parent = m_parent.lock();
    if (parent)
        parent->m_loadingTime.fetch_add(end - start);
#endif
    DEBUGBV << "succeeded;" << m_block.createHash() << '/' << m_block.size();
}

void BlockValidationState::blockFailed(int punishment, const std::string &error, Validation::RejectCodes code, bool corruptionPossible)
{
    this->punishment = punishment;
    this->error = error;
    errorCode = code;
    isCorruptionPossible = corruptionPossible;
    m_validationStatus.fetch_or(BlockInvalid);
    auto validationSettings = m_settings.lock();
    if (validationSettings) {
        validationSettings->error = error;
        validationSettings->markFinished();
    }
}

void BlockValidationState::signalChildren() const
{
    for (auto child_weak : m_chainChildren) {
        std::shared_ptr<BlockValidationState> child = child_weak.lock();
        if (child.get()) {
            int status = child->m_validationStatus.load();
            while (true) {
                int newStatus = status | BlockValidationState::BlockValidParent;
                assert(newStatus != status);
                if  (child->m_validationStatus.compare_exchange_weak(status, newStatus, std::memory_order_relaxed, std::memory_order_relaxed)) {
                    if (status & BlockValidationState::BlockValidChainHeaders)
                        Application::instance()->ioService().post(std::bind(&BlockValidationState::updateUtxoAndStartValidation, child));
                    break;
                }
            }
        }
    }
}

void BlockValidationState::recursivelyMark(BlockValidationStatus value, RecursiveOption option)
{
    if (option == AddFlag)
        m_validationStatus.fetch_or(value);
    else
        m_validationStatus.fetch_and(0xFF^value);
    for (auto child : m_chainChildren) {
        std::shared_ptr<BlockValidationState> state = child.lock();
        if (state)
            state->recursivelyMark(value, option);
    }
}

void BlockValidationState::finishUp()
{
    std::shared_ptr<ValidationEnginePrivate> parent = m_parent.lock();
    if (parent)
        parent->strand.post(std::bind(&ValidationEnginePrivate::processNewBlock, parent, shared_from_this()));
}

void BlockValidationState::checks1NoContext()
{
    try {
        if (m_block.size() == 0)
            load();
    } catch (const std::exception &e) {
        logInfo() << "BlockValidationState: Failed to load block, ignoring. Error:" << e.what();
        return;
    }
#ifdef ENABLE_BENCHMARKS
    int64_t start2, end2, end, start; start2 = end2 = start = end = GetTimeMicros();
#endif

    DEBUGBV << "Starting" << m_block.createHash() << "CheckPOW:" << m_checkPow << "CheckMerkleRoot:" << m_checkMerkleRoot << "ValidityOnly:" << m_checkValidityOnly;

    try { // These are checks that are independent of context.
        // Check proof of work matches claimed amount
        if (m_checkPow && !CheckProofOfWork(m_block.createHash(), m_block.bits(), Params().GetConsensus()))
            throw Exception("high-hash", 50);

        // Check timestamp
        if (m_block.timestamp() > GetAdjustedTime() + 2 * 60 * 60)
            throw Exception("time-too-new");

#ifdef ENABLE_BENCHMARKS
        start2 = end = GetTimeMicros();
#endif
        // if this is a full block, test the transactions too.
        if (m_block.isFullBlock() && m_checkTransactionValidity) {
            m_block.findTransactions(); // find out if the block and its transactions are well formed and parsable.

            CBlock block = m_block.createOldBlock();
            if (m_checkMerkleRoot) { // Check the merkle root.
                bool mutated;
                uint256 hashMerkleRoot2 = BlockMerkleRoot(block, &mutated);
                if (block.hashMerkleRoot != hashMerkleRoot2)
                    throw Exception("bad-txnmrklroot", Validation::InvalidNotFatal);

                // Check for merkle tree malleability (CVE-2012-2459): repeating sequences
                // of transactions in a block without affecting the merkle root of a block,
                // while still invalidating it.
                if (mutated)
                    throw Exception("bad-txns-duplicate", Validation::InvalidNotFatal);
            }

            // Size limits
            if (block.vtx.empty())
                throw Exception("bad-blk-length");

            const std::uint32_t blockSizeAcceptLimit = Policy::blockSizeAcceptLimit();
            const std::uint32_t blockSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
            if (block.vtx.size() > blockSizeAcceptLimit || blockSize > blockSizeAcceptLimit) {
                const float punishment = (blockSize - blockSizeAcceptLimit) / (float) blockSizeAcceptLimit;
                throw Exception("bad-blk-length", Validation::RejectExceedsLimit, 10 * punishment + 0.5);
            }

            // All potential-corruption validation must be done before we do any
            // transaction validation, as otherwise we may mark the header as invalid
            // because we receive the wrong transactions for it.

            assert(!block.vtx.empty());
            // First transaction must be coinbase, the rest must not be
            if (!block.vtx[0].IsCoinBase())
                throw Exception("bad-cb-missing");
            for (unsigned int i = 1; i < block.vtx.size(); i++) {
                if (block.vtx[i].IsCoinBase())
                    throw Exception("bad-cb-multiple");
            }

            // Check transactions
            for (const CTransaction &tx : block.vtx) {
                Validation::checkTransaction(tx);
            }
        }

        m_validationStatus.fetch_or(BlockValidHeader);
#ifdef ENABLE_BENCHMARKS
        end2 = GetTimeMicros();
#endif
    } catch (const Exception &ex) {
        blockFailed(ex.punishment(), ex.what(), ex.rejectCode(), ex.corruptionPossible());
    } catch (const std::runtime_error &ex) {
        assert(false);
        blockFailed(100, ex.what(), Validation::RejectInternal);
    }

    std::shared_ptr<ValidationEnginePrivate> parent = m_parent.lock();
    if (parent) {
#ifdef ENABLE_BENCHMARKS
        parent->m_headerCheckTime.fetch_add(end - start);
        parent->m_basicValidityChecks.fetch_add(end2 - start2);
#endif
        parent->strand.dispatch(std::bind(&ValidationEnginePrivate::blockHeaderValidated, parent, shared_from_this()));
    }
}

void BlockValidationState::checks2HaveParentHeaders()
{
    assert(m_blockIndex);
    assert(m_blockIndex->nHeight >= 0);
    assert(m_block.isFullBlock());
    DEBUGBV << m_blockIndex->nHeight << m_block.createHash();

#ifdef ENABLE_BENCHMARKS
    int64_t start = GetTimeMicros();
#endif
    try {
        m_block.findTransactions();
        CBlock block = m_block.createOldBlock();
        if (m_blockIndex->pprev) { // not genesis
            const auto consensusParams = Params().GetConsensus();
            // Check proof of work
            if (block.nBits != GetNextWorkRequired(m_blockIndex->pprev, &block, consensusParams))
                throw Exception("bad-diffbits");

            // Check timestamp against prev
            if (block.GetBlockTime() <= m_blockIndex->pprev->GetMedianTimePast())
                throw Exception("time-too-old");

            // Reject block.nVersion=1 blocks when 95% (75% on testnet) of the network has upgraded:
            if (block.nVersion < 2 && IsSuperMajority(2, m_blockIndex->pprev, consensusParams.nMajorityRejectBlockOutdated, consensusParams))
                throw Exception("bad-version", Validation::RejectObsolete);

            // Reject block.nVersion=2 blocks when 95% (75% on testnet) of the network has upgraded:
            if (block.nVersion < 3 && IsSuperMajority(3, m_blockIndex->pprev, consensusParams.nMajorityRejectBlockOutdated, consensusParams))
                throw Exception("bad-version", Validation::RejectObsolete);

            // Reject block.nVersion=3 blocks when 95% (75% on testnet) of the network has upgraded:
            if (block.nVersion < 4 && IsSuperMajority(4, m_blockIndex->pprev, consensusParams.nMajorityRejectBlockOutdated, consensusParams))
                throw Exception("bad-version", Validation::RejectObsolete);
        }

        {
            CValidationState state;
            LOCK(cs_main);
            if (!ContextualCheckBlock(block, state, m_blockIndex->pprev))
                throw Exception(state.GetRejectReason());
        }

        // Enforce rule that the coinbase starts with serialized block height
        const int bip34Height = Params().GetConsensus().BIP34Height;
        if (bip34Height > 0 && m_blockIndex->nHeight >= bip34Height) {
            CScript expect = CScript() << m_blockIndex->nHeight;
            if (block.vtx[0].vin[0].scriptSig.size() < expect.size()
                    || !std::equal(expect.begin(), expect.end(), block.vtx[0].vin[0].scriptSig.begin()))
                throw Exception("bad-cb-height");
        }

        // Sigops.
        // Notice that we continue counting in validateTransaction and do one last check in processNewBlock()
        assert(m_sigOpsCounted == 0);
        const uint64_t maxSigOps = Policy::blockSigOpAcceptLimit(m_block.size());
        for (const CTransaction &tx : block.vtx) {
            const uint32_t sigops = GetLegacySigOpCount(tx);
            if (sigops > MAX_BLOCK_SIGOPS_PER_MB)
                throw Exception("bad-tx-sigops");
            m_sigOpsCounted += sigops;
            if (m_sigOpsCounted > maxSigOps)
                throw Exception("bad-blk-sigops");
        }
    } catch (const Exception &e) {
        blockFailed(e.punishment(), e.what(), e.rejectCode(), e.corruptionPossible());
        finishUp();
        return;
    } catch (std::runtime_error &e) {
        assert(false);
        blockFailed(100, e.what(), Validation::RejectInternal);
        finishUp();
        return;
    }

    flags.updateForBlock(m_blockIndex, m_block.createHash());
#ifdef ENABLE_BENCHMARKS
    int64_t end = GetTimeMicros();
#endif

    int status = m_validationStatus.load();
    auto parent = m_parent.lock();
    while (parent) {
#ifdef ENABLE_BENCHMARKS
        parent->m_contextCheckTime.fetch_add(end - start);
#endif
        int newStatus = status | BlockValidChainHeaders;
        if  (m_validationStatus.compare_exchange_weak(status, newStatus, std::memory_order_relaxed, std::memory_order_relaxed)) {
            if ((status & BlockValidParent) || (status & BlockInvalid)) { // we just added the last bit.
                Application::instance()->ioService().post(std::bind(&BlockValidationState::updateUtxoAndStartValidation, shared_from_this()));
            } else {
                assert(!m_checkValidityOnly); // why did we get here if the
                DEBUGBV << "  saving block for later, no parent yet" << m_block.createHash();
            }
            return;
        }
    }
}

void BlockValidationState::updateUtxoAndStartValidation()
{
    DEBUGBV << m_block.createHash();
    assert(m_txChunkLeftToStart.load() < 0); // this method should get called only once
    assert(m_undoBlock.vtxundo.empty());

    if (m_blockIndex->pprev == nullptr) { // genesis
        finishUp();
        return;
    }
    auto view = m_coinView.get();
    try {
#ifdef ENABLE_BENCHMARKS
        int64_t start = GetTimeMicros();
#endif
        m_coins = view->processBlock(m_block, m_undoBlock, m_blockIndex->nHeight,
                (flags.enforceBIP30 ? CCoinsViewCache::CheckDuplicateTxId : CCoinsViewCache::SkipDuplicateTxIdCheck));

#ifdef ENABLE_BENCHMARKS
        int64_t end = GetTimeMicros();
        auto parent = m_parent.lock();
        if (parent)
            parent->m_utxoTime.fetch_add(end - start);
        // logDebug(Log::BlockValication).nospace() << "updateUtxo duration: " << (end - start)/1000. << " ms (height: " << m_blockIndex->nHeight << ")";
#endif

        if (!flags.enableValidation) {
            CBlockIndex *index = m_blockIndex;
            finishUp();
            storeUndoBlock(index);
            return;
        }

        int chunks, itemsPerChunk;
        calculateTxCheckChunks(chunks, itemsPerChunk);
        m_txChunkLeftToFinish.store(chunks);
        m_txChunkLeftToStart.store(chunks);

        for (int i = 1; i < chunks; ++i)
            Application::instance()->ioService().post(std::bind(&BlockValidationState::checkSignaturesChunk, shared_from_this()));
        checkSignaturesChunk();
    } catch(const Exception &ex) {
        blockFailed(ex.punishment(), ex.what(), ex.rejectCode(), ex.corruptionPossible());
        finishUp();
    } catch(const std::exception &ex) {
        assert(false);
        blockFailed(100, ex.what(), Validation::RejectInternal);
        finishUp();
    }
}

void BlockValidationState::checkSignaturesChunk()
{
#ifdef ENABLE_BENCHMARKS
    int64_t start = GetTimeMicros();
#endif
    const int totalTxCount = (int) m_block.transactions().size();

    int chunkToStart = m_txChunkLeftToStart.fetch_sub(1) - 1;
    assert(chunkToStart >= 0);
    DEBUGBV << chunkToStart << m_block.createHash();

    int chunks, itemsPerChunk;
    calculateTxCheckChunks(chunks, itemsPerChunk);
    bool blockValid = (m_validationStatus.load() & BlockInvalid) == 0;
    int txIndex = itemsPerChunk * chunkToStart;
    const int txMax = std::min(txIndex + itemsPerChunk, totalTxCount);
    CAmount chunkFees = 0;
    try {
        for (;blockValid && txIndex < txMax; ++txIndex) {
            if (txIndex > 0) { // skip coinbase
                CAmount fees = 0;
                validateTransaction(txIndex, fees);
                chunkFees += fees;
            }
        }
    } catch (const Exception &e){
        DEBUGBV << "Failed validation due to" << e.what();
        blockFailed(e.punishment(), e.what(), e.rejectCode(), e.corruptionPossible());
        blockValid = false;
    } catch (const std::runtime_error &e){
        DEBUGBV << "Failed validation due to" << e.what();
        assert(false);
        blockFailed(100, e.what(), Validation::RejectInternal);
        blockValid = false;
    }
    m_blockFees.fetch_add(chunkFees);

#ifdef ENABLE_BENCHMARKS
    int64_t end = GetTimeMicros();
    if (blockValid) {
        auto parent_ = m_parent.lock();
        if (parent_)
            parent_->m_validationTime.fetch_add(end - start);
    }
    logDebug(Log::BlockValidation) << "batch:" << chunkToStart << '/' << chunks << (end - start)/1000. << "ms" << "success so far:" << blockValid;
#endif

    const int chunksLeft = m_txChunkLeftToFinish.fetch_sub(1) - 1;
    if (chunksLeft <= 0) { // I'm the last one to finish
        CBlockIndex *index = m_blockIndex;
        finishUp();
        // saving this is rather slow, but ultimately irrelevant to the processNewBlock, so we do those at the same time.
        storeUndoBlock(index);
    }
}

void BlockValidationState::storeUndoBlock(CBlockIndex *index)
{
    if (m_checkValidityOnly)
        return;
    if (!(m_onResultFlags & Validation::SaveGoodToDisk))
        return;
    // note, we take index from the argument because the processNewBlock runs at the same time and it may set the m_blockindex to null
    if (index->nStatus & BLOCK_HAVE_UNDO)
        return;
    DEBUGBV << "Saving undo data for block" << index->nHeight << "to file" << index->nFile;
    assert(index->nFile >= 0);
    try {
        FastUndoBlock block = FastUndoBlock::fromOldBlock(m_undoBlock);
        Blocks::DB::instance()->writeUndoBlock(block, index->pprev->GetBlockHash(), index->nFile, &index->nUndoPos);
        index->nStatus |= BLOCK_HAVE_UNDO;
        MarkIndexUnsaved(index);
    } catch (std::exception &ex) {
        logFatal(Log::DB) << ex;
        auto parent = m_parent.lock();
        if (parent)
            parent->fatal("Failed to write undo data");
    }
}

void BlockValidationState::validateTransaction(int txIndex, int64_t &fees)
{
    assert(txIndex != 0); // we don't expect to be called for the coinbase.

    auto tx = m_block.transactions().at(txIndex).createOldTransaction();
    const std::vector<CCoins> *coins = &m_coins.at(txIndex);
    assert(coins);
    assert(coins->size() == tx.vin.size());
    assert(tx.vin.size() == coins->size());
    const int spendHeight = m_blockIndex->nHeight;

    // Check that transaction is BIP68 final
    int nLockTimeFlags = 0;
    if (flags.nLocktimeVerifySequence)
        nLockTimeFlags |= LOCKTIME_VERIFY_SEQUENCE;

    std::vector<int> prevheights;
    prevheights.reserve(tx.vin.size());
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        prevheights.push_back(coins->at(i).nHeight);
    }
    if (!SequenceLocks(tx, nLockTimeFlags, &prevheights, *m_blockIndex))
        throw Exception("bad-txns-nonfinal");

    uint32_t txSigops = 0;
    bool spendsCoinBase;
    // TODO check for xthin style block and if the transaction was already checked or not. If so, then skip the next
    ValidationPrivate::validateTransactionInputs(tx, *coins, spendHeight, flags, fees, txSigops, spendsCoinBase);
    m_sigOpsCounted += txSigops;
}

//---------------------------------------------------------


int32_t WarningBitsConditionChecker::computeBlockVersion(const CBlockIndex* pindexPrev)
{
    LOCK(cs_main); // for versionbitscache
    int32_t nVersion = VERSIONBITS_TOP_BITS;

    for (int i = 1; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; i++) {
        ThresholdState state = VersionBitsState(pindexPrev, Params().GetConsensus(), (Consensus::DeploymentPos)i, versionbitscache);
        if (state == THRESHOLD_LOCKED_IN || state == THRESHOLD_STARTED) {
            nVersion |= VersionBitsMask(Params().GetConsensus(), (Consensus::DeploymentPos)i);
        }
    }

    return nVersion;
}
