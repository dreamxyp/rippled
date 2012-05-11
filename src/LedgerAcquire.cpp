
#include "LedgerAcquire.h"

#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#include <boost/bind.hpp>

#include "Application.h"

#define LEDGER_ACQUIRE_TIMEOUT 2

LedgerAcquire::LedgerAcquire(const uint256& hash) : mHash(hash),
	mComplete(false), mFailed(false), mHaveBase(false), mHaveState(false), mHaveTransactions(false),
	mTimer(theApp->getIOService())
{
#ifdef DEBUG
	std::cerr << "Acquiring ledger " << mHash.GetHex() << std::endl;
#endif
}

void LedgerAcquire::done()
{
#ifdef DEBUG
	std::cerr << "Done acquiring ledger " << mHash.GetHex() << std::endl;
#endif
	std::vector< boost::function<void (LedgerAcquire::pointer)> > triggers;

	mLock.lock();
	triggers = mOnComplete;
	mOnComplete.empty();
	mLock.unlock();

	for (int i = 0; i<triggers.size(); ++i)
		triggers[i](shared_from_this());
}

void LedgerAcquire::resetTimer()
{
	mTimer.expires_from_now(boost::posix_time::seconds(LEDGER_ACQUIRE_TIMEOUT));
	mTimer.async_wait(boost::bind(&LedgerAcquire::timerEntry,
		boost::weak_ptr<LedgerAcquire>(shared_from_this()), boost::asio::placeholders::error));	
}

void LedgerAcquire::timerEntry(boost::weak_ptr<LedgerAcquire> wptr, const boost::system::error_code& result)
{
	if (result == boost::asio::error::operation_aborted) return;
	LedgerAcquire::pointer ptr = wptr.lock();
	if (!!ptr) ptr->trigger();
}

void LedgerAcquire::addOnComplete(boost::function<void (LedgerAcquire::pointer)> trigger)
{
	mLock.lock();
	mOnComplete.push_back(trigger);
	mLock.unlock();
}

void LedgerAcquire::trigger()
{
#ifdef DEBUG
	std::cerr << "Trigger acquiring ledger " << mHash.GetHex() << std::endl;
#endif
	if (mComplete || mFailed) return;

	if (!mHaveBase)
	{
#ifdef DEBUG
		std::cerr << "need base" << std::endl;
#endif
		boost::shared_ptr<newcoin::TMGetLedger> tmGL = boost::make_shared<newcoin::TMGetLedger>();
		tmGL->set_ledgerhash(mHash.begin(), mHash.size());
		tmGL->set_itype(newcoin::liBASE);
		sendRequest(tmGL);
	}

	if (mHaveBase && !mHaveTransactions)
	{
#ifdef DEBUG
		std::cerr << "need tx" << std::endl;
#endif
		assert(mLedger);
		if (mLedger->peekTransactionMap()->getHash().isZero())
		{ // we need the root node
			boost::shared_ptr<newcoin::TMGetLedger> tmGL = boost::make_shared<newcoin::TMGetLedger>();
			tmGL->set_ledgerhash(mHash.begin(), mHash.size());
			tmGL->set_ledgerseq(mLedger->getLedgerSeq());
			tmGL->set_itype(newcoin::liTX_NODE);
			*(tmGL->add_nodeids()) = SHAMapNode().getRawString();
			sendRequest(tmGL);
		}
		else
		{
			std::vector<SHAMapNode> nodeIDs;
			std::vector<uint256> nodeHashes;
			mLedger->peekTransactionMap()->getMissingNodes(nodeIDs, nodeHashes, 128);
			if (nodeIDs.empty())
			{
				if (!mLedger->peekTransactionMap()->isValid()) mFailed = true;
				else
				{
					mHaveTransactions = true;
					if (mHaveState) mComplete = true;
				}
			}
			else
			{
				boost::shared_ptr<newcoin::TMGetLedger> tmGL = boost::make_shared<newcoin::TMGetLedger>();
				tmGL->set_ledgerhash(mHash.begin(), mHash.size());
				tmGL->set_ledgerseq(mLedger->getLedgerSeq());
				tmGL->set_itype(newcoin::liTX_NODE);
				for (std::vector<SHAMapNode>::iterator it = nodeIDs.begin(); it != nodeIDs.end(); ++it)
					*(tmGL->add_nodeids()) = it->getRawString();
				sendRequest(tmGL);
			}
		}
	}

	if (mHaveBase && !mHaveState)
	{
#ifdef DEBUG
		std::cerr << "need as" << std::endl;
#endif
		assert(mLedger);
		if (mLedger->peekAccountStateMap()->getHash().isZero())
		{ // we need the root node
			boost::shared_ptr<newcoin::TMGetLedger> tmGL = boost::make_shared<newcoin::TMGetLedger>();
			tmGL->set_ledgerhash(mHash.begin(), mHash.size());
			tmGL->set_ledgerseq(mLedger->getLedgerSeq());
			tmGL->set_itype(newcoin::liAS_NODE);
			*(tmGL->add_nodeids()) = SHAMapNode().getRawString();
			sendRequest(tmGL);
		}
		else
		{
			std::vector<SHAMapNode> nodeIDs;
			std::vector<uint256> nodeHashes;
			mLedger->peekAccountStateMap()->getMissingNodes(nodeIDs, nodeHashes, 128);
			if (nodeIDs.empty())
			{
 				if (!mLedger->peekAccountStateMap()->isValid()) mFailed = true;
				else
				{
					mHaveState = true;
					if (mHaveTransactions) mComplete = true;
				}
			}
			else
			{
				boost::shared_ptr<newcoin::TMGetLedger> tmGL = boost::make_shared<newcoin::TMGetLedger>();
				tmGL->set_ledgerhash(mHash.begin(), mHash.size());
				tmGL->set_ledgerseq(mLedger->getLedgerSeq());
				tmGL->set_itype(newcoin::liAS_NODE);
				for (std::vector<SHAMapNode>::iterator it =nodeIDs.begin(); it != nodeIDs.end(); ++it)
					*(tmGL->add_nodeids()) = it->getRawString();
				sendRequest(tmGL);
			}
		}
	}

	if (mComplete || mFailed)
		done();
	else
		resetTimer();
}

void LedgerAcquire::sendRequest(boost::shared_ptr<newcoin::TMGetLedger> tmGL)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	if (mPeers.empty()) return;

	PackedMessage::pointer packet = boost::make_shared<PackedMessage>(tmGL, newcoin::mtGET_LEDGER);

	std::list<boost::weak_ptr<Peer> >::iterator it = mPeers.begin();
	while(it != mPeers.end())
	{
		if (it->expired())
			mPeers.erase(it++);
		else
		{
			// FIXME: Track last peer sent to and time sent
			it->lock()->sendPacket(packet);
			return;
		}
	}
}

void LedgerAcquire::peerHas(Peer::pointer ptr)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	std::list<boost::weak_ptr<Peer> >::iterator it=mPeers.begin();
	while (it != mPeers.end())
	{
		Peer::pointer pr = it->lock();
		if (!pr) // we have a dead entry, remove it
			it = mPeers.erase(it);
		else
		{
			if (pr->samePeer(ptr)) return;	// we already have this peer
			++it;
		}
	}
	mPeers.push_back(ptr);
}

void LedgerAcquire::badPeer(Peer::pointer ptr)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	std::list<boost::weak_ptr<Peer> >::iterator it=mPeers.begin();
	while (it != mPeers.end())
	{
		Peer::pointer pr = it->lock();
		if (!pr) // we have a dead entry, remove it
			it = mPeers.erase(it);
		else
		{
			if (ptr->samePeer(pr))
			{ // We found a pointer to the bad peer
				mPeers.erase(it);
				return;
			}
			++it;
		}
	}
}

bool LedgerAcquire::takeBase(const std::string& data)
{ // Return value: true=normal, false=bad data
#ifdef DEBUG
	std::cerr << "got base acquiring ledger " << mHash.GetHex() << std::endl;
#endif
	boost::recursive_mutex::scoped_lock sl(mLock);
	if (mHaveBase) return true;
	mLedger = boost::make_shared<Ledger>(data);
	if (mLedger->getHash() != mHash)
	{
		mLedger = Ledger::pointer();
		return false;
	}
	mHaveBase = true;
	if (!mLedger->getTransHash()) mHaveTransactions = true;
	if (!mLedger->getAccountHash()) mHaveState = true;
	mLedger->setAcquiring();
	return true;
}

bool LedgerAcquire::takeTxNode(const std::list<SHAMapNode>& nodeIDs,
	const std::list<std::vector<unsigned char> >& data)
{
	if (!mHaveBase) return false;
	std::list<SHAMapNode>::const_iterator nodeIDit = nodeIDs.begin();
	std::list<std::vector<unsigned char> >::const_iterator nodeDatait = data.begin();
	while (nodeIDit != nodeIDs.end())
	{
		if (nodeIDit->isRoot())
		{
			if (!mLedger->peekTransactionMap()->addRootNode(mLedger->getTransHash(), *nodeDatait))
				return false;
		}
		else if (!mLedger->peekTransactionMap()->addKnownNode(*nodeIDit, *nodeDatait))
			return false;
		++nodeIDit;
		++nodeDatait;
	}
	if (!mLedger->peekTransactionMap()->isSynching()) mHaveTransactions = true;
	return true;
}

bool LedgerAcquire::takeAsNode(const std::list<SHAMapNode>& nodeIDs,
	const std::list<std::vector<unsigned char> >& data)
{
#ifdef DEBUG
	std::cerr << "got ASdata acquiring ledger " << mHash.GetHex() << std::endl;
#endif
	if (!mHaveBase) return false;
	std::list<SHAMapNode>::const_iterator nodeIDit = nodeIDs.begin();
	std::list<std::vector<unsigned char> >::const_iterator nodeDatait = data.begin();
	while (nodeIDit != nodeIDs.end())
	{
		if (nodeIDit->isRoot())
		{
			if (!mLedger->peekAccountStateMap()->addRootNode(mLedger->getAccountHash(), *nodeDatait))
				return false;
		}
		else if (!mLedger->peekAccountStateMap()->addKnownNode(*nodeIDit, *nodeDatait))
			return false;
		++nodeIDit;
		++nodeDatait;
	}
	if (!mLedger->peekAccountStateMap()->isSynching()) mHaveState = true;
	return true;
}

LedgerAcquire::pointer LedgerAcquireMaster::findCreate(const uint256& hash)
{
	boost::mutex::scoped_lock sl(mLock);
	LedgerAcquire::pointer& ptr = mLedgers[hash];
	if (ptr) return ptr;
	ptr = boost::make_shared<LedgerAcquire>(hash);
	assert(mLedgers[hash] == ptr);
	ptr->resetTimer(); // Cannot call in constructor
	return ptr;
}

LedgerAcquire::pointer LedgerAcquireMaster::find(const uint256& hash)
{
	boost::mutex::scoped_lock sl(mLock);
	std::map<uint256, LedgerAcquire::pointer>::iterator it = mLedgers.find(hash);
	if (it != mLedgers.end()) return it->second;
	return LedgerAcquire::pointer();
}

bool LedgerAcquireMaster::hasLedger(const uint256& hash)
{
	boost::mutex::scoped_lock sl(mLock);
	return mLedgers.find(hash) != mLedgers.end();
}

bool LedgerAcquireMaster::dropLedger(const uint256& hash)
{
	boost::mutex::scoped_lock sl(mLock);
	return mLedgers.erase(hash);
}

bool LedgerAcquireMaster::gotLedgerData(newcoin::TMLedgerData& packet)
{
#ifdef DEBUG
	std::cerr << "got data for acquiring ledger ";
#endif
	uint256 hash;
	if (packet.ledgerhash().size() != 32)
	{
#ifdef DEBUG
		std::cerr << "error" << std::endl;
#endif
		return false;
	}
	memcpy(&hash, packet.ledgerhash().data(), 32);
#ifdef DEBUG
	std::cerr << hash.GetHex() << std::endl;
#endif

	LedgerAcquire::pointer ledger=find(hash);
	if (!ledger) return false;

	if (packet.type() == newcoin::liBASE)
	{
		if (packet.nodes_size() != 1) return false;
		const newcoin::TMLedgerNode& node = packet.nodes(0);
		if (!node.has_nodedata()) return false;
		return ledger->takeBase(node.nodedata());
	}
	else if ((packet.type() == newcoin::liTX_NODE) || (packet.type() == newcoin::liAS_NODE) )
	{
		std::list<SHAMapNode> nodeIDs;
		std::list<std::vector<unsigned char> > nodeData;

		if (packet.nodes().size()<=0) return false;
		for (int i = 0; i<packet.nodes().size(); ++i)
		{
			const newcoin::TMLedgerNode& node=packet.nodes(i);
			if (!node.has_nodeid() || !node.has_nodedata()) return false;

			nodeIDs.push_back(SHAMapNode(node.nodeid().data(), node.nodeid().size()));
			nodeData.push_back(std::vector<unsigned char>(node.nodedata().begin(), node.nodedata().end()));
		}
		if (packet.type() == newcoin::liTX_NODE) return ledger->takeTxNode(nodeIDs, nodeData);
		else return ledger->takeAsNode(nodeIDs, nodeData);
	}
	else return false;
}
