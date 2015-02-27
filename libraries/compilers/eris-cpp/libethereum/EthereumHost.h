/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file EthereumHost.h
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#pragma once

#include <mutex>
#include <map>
#include <vector>
#include <set>
#include <memory>
#include <utility>
#include <thread>
#include <libdevcore/Guards.h>
#include <libdevcore/Worker.h>
#include <libdevcore/RangeMask.h>
#include <libethcore/CommonEth.h>
#include <libp2p/Common.h>
#include "CommonNet.h"
#include "EthereumPeer.h"
#include "DownloadMan.h"

namespace dev
{

class RLPStream;

namespace eth
{

class TransactionQueue;
class BlockQueue;

/**
 * @brief The EthereumHost class
 * @warning None of this is thread-safe. You have been warned.
 */
class EthereumHost: public p2p::HostCapability<EthereumPeer>, Worker
{
	friend class EthereumPeer;

public:
	/// Start server, but don't listen.
	EthereumHost(BlockChain const& _ch, TransactionQueue& _tq, BlockQueue& _bq, u256 _networkId);

	/// Will block on network process events.
	virtual ~EthereumHost();

	unsigned protocolVersion() const { return c_protocolVersion; }
	u256 networkId() const { return m_networkId; }
	void setNetworkId(u256 _n) { m_networkId = _n; }

	void reset();

	DownloadMan const& downloadMan() const { return m_man; }
	bool isSyncing() const { return m_grabbing == Grabbing::Chain; }

private:
	void noteHavePeerState(EthereumPeer* _who);
	/// Session wants to pass us a block that we might not have.
	/// @returns true if we didn't have it.
	bool noteBlock(h256 _hash, bytesConstRef _data);
	/// Session has finished getting the chain of hashes.
	void noteHaveChain(EthereumPeer* _who);
	/// Called when the peer can no longer provide us with any needed blocks.
	void noteDoneBlocks(EthereumPeer* _who);

	/// Sync with the BlockChain. It might contain one of our mined blocks, we might have new candidates from the network.
	void doWork();

	/// Called by peer to add incoming transactions.
	void addIncomingTransaction(bytes const& _bytes) { std::lock_guard<std::recursive_mutex> l(m_incomingLock); m_incomingTransactions.push_back(_bytes); }

	void maintainTransactions(TransactionQueue& _tq, h256 _currentBlock);
	void maintainBlocks(BlockQueue& _bq, h256 _currentBlock);

	/// Get a bunch of needed blocks.
	/// Removes them from our list of needed blocks.
	/// @returns empty if there's no more blocks left to fetch, otherwise the blocks to fetch.
	h256Set neededBlocks(h256Set const& _exclude);

	///	Check to see if the network peer-state initialisation has happened.
	bool isInitialised() const { return m_latestBlockSent; }

	/// Initialises the network peer-state, doing the stuff that needs to be once-only. @returns true if it really was first.
	bool ensureInitialised(TransactionQueue& _tq);

	virtual void onStarting() { startWorking(); }
	virtual void onStopping() { stopWorking(); }

	void readyForSync();
	void updateGrabbing(Grabbing _g);

	BlockChain const& m_chain;
	TransactionQueue& m_tq;					///< Maintains a list of incoming transactions not yet in a block on the blockchain.
	BlockQueue& m_bq;						///< Maintains a list of incoming blocks not yet on the blockchain (to be imported).

	u256 m_networkId;

	Grabbing m_grabbing = Grabbing::Nothing;	// TODO: needs to be thread-safe & switch to just having a peer id.

	mutable std::recursive_mutex m_incomingLock;
	std::vector<bytes> m_incomingTransactions;
	std::vector<bytes> m_incomingBlocks;

	DownloadMan m_man;

	h256 m_latestBlockSent;
	h256Set m_transactionsSent;
};

}
}
