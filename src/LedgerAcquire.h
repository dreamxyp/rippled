#ifndef __LEDGERACQUIRE__
#define __LEDGERACQUIRE__

#include <vector>
#include <map>

#include <boost/enable_shared_from_this.hpp>
#include <boost/function.hpp>
#include <boost/asio.hpp>
#include <boost/thread/mutex.hpp>

#include "Ledger.h"
#include "Peer.h"
#include "../obj/src/newcoin.pb.h"

class LedgerAcquire : public boost::enable_shared_from_this<LedgerAcquire>
{ // A ledger we are trying to acquire
public:
	typedef boost::shared_ptr<LedgerAcquire> pointer;

protected:
	boost::recursive_mutex mLock;
	Ledger::pointer mLedger;
	uint256 mHash;
	bool mComplete, mFailed, mHaveBase, mHaveState, mHaveTransactions;

	boost::asio::deadline_timer mTimer;

	std::vector< boost::function<void (LedgerAcquire::pointer)> > mOnComplete;

	std::list<boost::weak_ptr<Peer> > mPeers; // peers known to have this ledger

	void done();
	void trigger();

	static void timerEntry(boost::weak_ptr<LedgerAcquire>, const boost::system::error_code&);
	void sendRequest(boost::shared_ptr<newcoin::TMGetLedger> message);

public:
	LedgerAcquire(const uint256& hash);

	const uint256& getHash() const		{ return mHash; }
	bool isComplete() const				{ return mComplete; }
	bool isFailed() const				{ return mFailed; }
	bool isBase() const					{ return mHaveBase; }
	bool isAcctStComplete() const		{ return mHaveState; }
	bool isTransComplete() const		{ return mHaveTransactions; }
	Ledger::pointer getLedger()			{ return mLedger; }

	void addOnComplete(boost::function<void (LedgerAcquire::pointer)>);

	void peerHas(Peer::pointer);
	void badPeer(Peer::pointer);
	bool takeBase(const std::string& data);
	bool takeTxNode(const std::list<SHAMapNode>& IDs, const std::list<std::vector<unsigned char> >& data);
	bool takeAsNode(const std::list<SHAMapNode>& IDs, const std::list<std::vector<unsigned char> >& data);
	void resetTimer();
};

class LedgerAcquireMaster
{
protected:
	boost::mutex mLock;
	std::map<uint256, LedgerAcquire::pointer> mLedgers;

public:
	LedgerAcquireMaster() { ; }

	LedgerAcquire::pointer findCreate(const uint256& hash);
	LedgerAcquire::pointer find(const uint256& hash);
	bool hasLedger(const uint256& ledgerHash);
	bool dropLedger(const uint256& ledgerHash);
	bool gotLedgerData(newcoin::TMLedgerData& packet);
};

#endif
// vim:ts=4
