/*
 * This file is part of the flowee project
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

#include "test_bitcoin.h"
#include <boost/test/unit_test.hpp>

#include <validation/BlockValidation_p.h>
#include <chainparams.h>
#include <key.h>
#include <consensus/merkle.h>
#include <main.h>

BOOST_FIXTURE_TEST_SUITE(blockvalidation, TestingSetup)

BOOST_AUTO_TEST_CASE(reorderblocks)
{
    bv.appendChain(4);
    BOOST_CHECK_EQUAL(bv.blockchain()->Height(), 4);
    CBlockIndex *oldBlock3 = (*bv.blockchain())[3];
    assert(oldBlock3);
    BOOST_CHECK_EQUAL(oldBlock3->nHeight, 3);
    CBlockIndex *oldBlock4 = (*bv.blockchain())[4];
    assert(oldBlock4);
    BOOST_CHECK_EQUAL(oldBlock4->nHeight, 4);
    BOOST_CHECK(Blocks::DB::instance()->headerChain().Contains(oldBlock3));
    BOOST_CHECK(Blocks::DB::instance()->headerChain().Contains(oldBlock4));

    // Now, build on top of block 3 a 2 block chain. But only register them at the headersChain
    // in the Blocks::DB, so I can test reorgs.
    CKey coinbaseKey;
    coinbaseKey.MakeNewKey(true);
    CScript scriptPubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    FastBlock b4 = bv.createBlock(oldBlock3, scriptPubKey);
    // printf("B4: %s\n", b4.createHash().ToString().c_str());
    BOOST_CHECK(b4.previousBlockId() == *oldBlock3->phashBlock);
    std::shared_ptr<BlockValidationState> state4(new BlockValidationState(bv.priv(), b4));
    // let it create me a CBlockIndex
    bv.priv().lock()->createBlockIndexFor(state4);
    BOOST_CHECK_EQUAL(state4->m_blockIndex->nHeight, 4);

    // work around optimization of phashblock coming from the hash table.
    uint256 hash4 = state4->m_block.createHash();
    state4->m_blockIndex->phashBlock = &hash4;
    bool changed = Blocks::DB::instance()->appendHeader(state4->m_blockIndex);

    // no reorgs yet.
    BOOST_CHECK_EQUAL(changed, false);
    BOOST_CHECK(Blocks::DB::instance()->headerChain().Contains(oldBlock3));
    BOOST_CHECK(Blocks::DB::instance()->headerChain().Contains(oldBlock4));
    BOOST_CHECK_EQUAL(Blocks::DB::instance()->headerChainTips().size(), 2);

    // The method that does reorgs is the BlocksValidtionPrivate::prepareChainForBlock()
    // We now have two chains as known by the headersChain.
    // the tips have exactly the same POW and as such the new chain should not cause a reorg.
    // (first seen principle)
    bv.priv().lock()->prepareChain();
    BOOST_CHECK_EQUAL(bv.blockchain()->Height(), 4);
    BOOST_CHECK_EQUAL((*bv.blockchain())[3], oldBlock3); // unchanged.
    BOOST_CHECK_EQUAL((*bv.blockchain())[4], oldBlock4);

    FastBlock b5 = bv.createBlock(state4->m_blockIndex, scriptPubKey);
    // printf("B5: %s\n", b5.createHash().ToString().c_str());
    BOOST_CHECK(b5.previousBlockId() == *state4->m_blockIndex->phashBlock);
    std::shared_ptr<BlockValidationState> state5(new BlockValidationState(bv.priv(), b5));
    bv.priv().lock()->createBlockIndexFor(state5);
    BOOST_CHECK_EQUAL(state5->m_blockIndex->pprev, state4->m_blockIndex);
    uint256 hash5 = state5->m_block.createHash();
    state5->m_blockIndex->phashBlock = &hash5;
    changed = Blocks::DB::instance()->appendHeader(state5->m_blockIndex);
    BOOST_CHECK_EQUAL(changed, true);
    BOOST_CHECK_EQUAL(Blocks::DB::instance()->headerChainTips().size(), 2);
    BOOST_CHECK(Blocks::DB::instance()->headerChain().Contains(state4->m_blockIndex));
    BOOST_CHECK(Blocks::DB::instance()->headerChain().Contains(state5->m_blockIndex));

    // We should now get a simple removal of block 4 from the original chain because our
    // new chain has more POW.

    bv.priv().lock()->prepareChain();

    BOOST_CHECK_EQUAL(bv.blockchain()->Height(), 3);
    BOOST_CHECK_EQUAL((*bv.blockchain())[3], oldBlock3); // unchanged.
    CBlockIndex *null = 0;
    BOOST_CHECK_EQUAL((*bv.blockchain())[4], null);

    bv.shutdown(); // avoid our validation-states being deleted here causing issues.
}

BOOST_AUTO_TEST_CASE(reorderblocks2)
{
    bv.appendChain(20);
    BOOST_CHECK_EQUAL(bv.blockchain()->Height(), 20);

    // create a chain of 8 blocks, forked off after 11.
    CBlockIndex *oldBlock11 = (*bv.blockchain())[11];
    std::vector<FastBlock> blocks = bv.createChain(oldBlock11, 10);
    BOOST_CHECK_EQUAL(blocks.size(), (size_t) 10);
    for (const FastBlock &block : blocks) {
        bv.addBlock(block, Validation::SaveGoodToDisk, 0);
    }
    bv.waitValidationFinished();
    BOOST_CHECK_EQUAL(bv.blockchain()->Height(), 21);
    BOOST_CHECK_EQUAL(oldBlock11, (*bv.blockchain())[11]);
    BOOST_CHECK(*(*bv.blockchain())[21]->phashBlock == blocks.back().createHash());
}

BOOST_AUTO_TEST_CASE(detectOrder)
{
    // create a chain of 20 blocks.
    std::vector<FastBlock> blocks = bv.createChain(bv.blockchain()->Tip(), 20);
    // add them all, in reverse order, in order to test if the code is capable of finding the proper ordering of the blocks
    BOOST_REVERSE_FOREACH (const FastBlock &block, blocks) {
        bv.addBlock(block, Validation::SaveGoodToDisk, 0);
    }
    bv.waitValidationFinished();
    BOOST_CHECK_EQUAL(bv.blockchain()->Height(), 20);
}

static FastBlock createHeader(const FastBlock &full) {
    return  FastBlock(Streaming::ConstBuffer(full.data().internal_buffer(),
                                             full.data().begin(), full.data().begin() + 80));
}

BOOST_AUTO_TEST_CASE(detectOrder2)
{
    // create a chain of 10 blocks.
    std::vector<FastBlock> blocks = bv.createChain(bv.blockchain()->Tip(), 10);

    // replace one block with a block header.
    FastBlock full = blocks[8];
    FastBlock header = createHeader(full);
    blocks[8] = header;
    for (const FastBlock &block : blocks) {
        bv.addBlock(block, Validation::SaveGoodToDisk, 0);
    }
    bv.waitValidationFinished();
    BOOST_CHECK_EQUAL(bv.blockchain()->Height(), 8); // it stopped at the header, not processing the last block because of that.
    bv.addBlock(full, Validation::SaveGoodToDisk, 0);
    bv.waitValidationFinished();
    CBlockIndex *dummy = new CBlockIndex();
    uint256 dummySha;
    dummy->phashBlock = &dummySha;
    dummy->pprev = bv.blockchain()->Tip();
    bv.invalidateBlock(dummy); // this is quite irrelevant to the feature we test, except that we know this syncs on the strand
    bv.waitValidationFinished();
    BOOST_CHECK_EQUAL(bv.blockchain()->Height(), 10);

    // now again, but with a bigger gap than 1
    blocks = bv.createChain(bv.blockchain()->Tip(), 10);
    std::vector<FastBlock> copy(blocks);
    for (int i = 3; i < 7; ++i) {
        blocks[i] = createHeader(blocks[i]);
    }
    for (const FastBlock &block : blocks) {
        bv.addBlock(block, Validation::SaveGoodToDisk, 0);
    }
    bv.waitValidationFinished();
    BOOST_CHECK_EQUAL(bv.blockchain()->Height(), 13);

    logDebug() << "again";
    // add them again, in reverse order, in order to test if the code is capable of finding the proper ordering of the blocks
    BOOST_REVERSE_FOREACH (const FastBlock &block, copy) {
        bv.addBlock(block, Validation::SaveGoodToDisk, 0);
    }
    bv.waitValidationFinished();
    bv.invalidateBlock(dummy); // this is quite irrelevant to the feature we test, except that we know this syncs on the strand
    bv.waitValidationFinished();
    BOOST_CHECK_EQUAL(bv.blockchain()->Height(), 20);
}

BOOST_AUTO_TEST_SUITE_END()
