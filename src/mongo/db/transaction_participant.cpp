/**
 *    Copyright (C) 2018 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_participant.h"

#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/session.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

// Server parameter that dictates the max number of milliseconds that any transaction lock request
// will wait for lock acquisition. If an operation provides a greater timeout in a lock request,
// maxTransactionLockRequestTimeoutMillis will override it. If this is set to a negative value, it
// is inactive and nothing will be overridden.
//
// 5 milliseconds will help avoid deadlocks, but will still allow fast-running metadata operations
// to run without aborting transactions.
MONGO_EXPORT_SERVER_PARAMETER(maxTransactionLockRequestTimeoutMillis, int, 5);

// Server parameter that dictates the lifetime given to each transaction.
// Transactions must eventually expire to preempt storage cache pressure immobilizing the system.
MONGO_EXPORT_SERVER_PARAMETER(transactionLifetimeLimitSeconds, std::int32_t, 60)
    ->withValidator([](const auto& potentialNewValue) {
        if (potentialNewValue < 1) {
            return Status(ErrorCodes::BadValue,
                          "transactionLifetimeLimitSeconds must be greater than or equal to 1s");
        }

        return Status::OK();
    });

namespace {

// Failpoint which will pause an operation just after allocating a point-in-time storage engine
// transaction.
MONGO_FAIL_POINT_DEFINE(hangAfterPreallocateSnapshot);

MONGO_FAIL_POINT_DEFINE(hangAfterReservingPrepareTimestamp);

const auto getTransactionParticipant = Session::declareDecoration<TransactionParticipant>();

// The command names that are allowed in a multi-document transaction.
const StringMap<int> txnCmdWhitelist = {{"abortTransaction", 1},
                                        {"aggregate", 1},
                                        {"commitTransaction", 1},
                                        {"coordinateCommitTransaction", 1},
                                        {"delete", 1},
                                        {"distinct", 1},
                                        {"doTxn", 1},
                                        {"find", 1},
                                        {"findandmodify", 1},
                                        {"findAndModify", 1},
                                        {"geoSearch", 1},
                                        {"getMore", 1},
                                        {"insert", 1},
                                        {"killCursors", 1},
                                        {"prepareTransaction", 1},
                                        {"update", 1},
                                        {"voteAbortTransaction", 1},
                                        {"voteCommitTransaction", 1}};

// The command names that are allowed in a multi-document transaction only when test commands are
// enabled.
const StringMap<int> txnCmdForTestingWhitelist = {{"dbHash", 1}};

// The commands that can be run on the 'admin' database in multi-document transactions.
const StringMap<int> txnAdminCommands = {{"abortTransaction", 1},
                                         {"commitTransaction", 1},
                                         {"coordinateCommitTransaction", 1},
                                         {"doTxn", 1},
                                         {"prepareTransaction", 1},
                                         {"voteAbortTransaction", 1},
                                         {"voteCommitTransaction", 1}};

// The command names that are allowed in a prepared transaction.
const StringMap<int> preparedTxnCmdWhitelist = {
    {"abortTransaction", 1}, {"commitTransaction", 1}, {"prepareTransaction", 1}};

}  // unnamed namespace

TransactionParticipant* TransactionParticipant::get(OperationContext* opCtx) {
    auto session = OperationContextSession::get(opCtx);
    if (!session) {
        return nullptr;
    }

    return &getTransactionParticipant(session);
}

TransactionParticipant* TransactionParticipant::getFromNonCheckedOutSession(Session* session) {
    return &getTransactionParticipant(session);
}

const Session* TransactionParticipant::_getSession() const {
    return getTransactionParticipant.owner(this);
}

Session* TransactionParticipant::_getSession() {
    return getTransactionParticipant.owner(this);
}

void TransactionParticipant::_beginOrContinueRetryableWrite(WithLock wl, TxnNumber txnNumber) {
    if (txnNumber > _activeTxnNumber) {
        // New retryable write.
        _setNewTxnNumber(wl, txnNumber);
        _autoCommit = boost::none;
    } else {
        // Retrying a retryable write.
        uassert(ErrorCodes::InvalidOptions,
                "Must specify autocommit=false on all operations of a multi-statement transaction.",
                _txnState.isNone(wl));
        invariant(_autoCommit == boost::none);
    }
}

void TransactionParticipant::_continueMultiDocumentTransaction(WithLock wl, TxnNumber txnNumber) {
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream()
                << "Given transaction number "
                << txnNumber
                << " does not match any in-progress transactions. The active transaction number is "
                << _activeTxnNumber,
            txnNumber == _activeTxnNumber && !_txnState.isNone(wl));

    if (_txnState.isInProgress(wl) && !_txnResourceStash) {
        // This indicates that the first command in the transaction failed but did not
        // implicitly abort the transaction. It is not safe to continue the transaction, in
        // particular because we have not saved the readConcern from the first statement of
        // the transaction.
        _abortTransactionOnSession(wl);
        uasserted(ErrorCodes::NoSuchTransaction,
                  str::stream() << "Transaction " << txnNumber << " has been aborted.");
    }

    return;
}

void TransactionParticipant::_beginMultiDocumentTransaction(WithLock wl, TxnNumber txnNumber) {
    // Aborts any in-progress txns.
    _setNewTxnNumber(wl, txnNumber);
    _autoCommit = false;

    _txnState.transitionTo(wl, TransactionState::kInProgress);

    // Start tracking various transactions metrics.
    auto curTime = curTimeMicros64();
    _transactionExpireDate = Date_t::fromMillisSinceEpoch(curTime / 1000) +
        stdx::chrono::seconds{transactionLifetimeLimitSeconds.load()};

    {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onStart(
            ServerTransactionsMetrics::get(getGlobalServiceContext()),
            *_autoCommit,
            curTime,
            *_transactionExpireDate);
    }
    invariant(_transactionOperations.empty());
}

void TransactionParticipant::beginOrContinue(TxnNumber txnNumber,
                                             boost::optional<bool> autocommit,
                                             boost::optional<bool> startTransaction) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    if (auto newState = _getSession()->getLastRefreshState()) {
        _updateState(lg, *newState);
    }

    // Requests without an autocommit field are interpreted as retryable writes. They cannot specify
    // startTransaction, which is verified earlier when parsing the request.
    if (!autocommit) {
        invariant(!startTransaction);
        _beginOrContinueRetryableWrite(lg, txnNumber);
        return;
    }

    // Attempt to continue a multi-statement transaction. In this case, it is required that
    // autocommit be given as an argument on the request, and currently it can only be false, which
    // is verified earlier when parsing the request.
    invariant(*autocommit == false);

    if (!startTransaction) {
        _continueMultiDocumentTransaction(lg, txnNumber);
        return;
    }

    // Attempt to start a multi-statement transaction, which requires startTransaction be given as
    // an argument on the request. startTransaction can only be specified as true, which is verified
    // earlier when parsing the request.
    invariant(*startTransaction);

    // Servers in a sharded cluster can start a new transaction at the active transaction number to
    // allow internal retries by routers on re-targeting errors, like StaleShardVersion or
    // SnapshotTooOld.
    if (txnNumber == _activeTxnNumber) {
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Only servers in a sharded cluster can start a new transaction at the active "
                "transaction number",
                serverGlobalParams.clusterRole != ClusterRole::None);

        // The active transaction number can only be reused if the transaction is not in a state
        // that indicates it has been involved in a two phase commit. In normal operation this check
        // should never fail.
        //
        // TODO SERVER-36639: Ensure the active transaction number cannot be reused if the
        // transaction is in the abort after prepare state (or any state indicating the participant
        // has been involved in a two phase commit).
        const auto restartableStates = TransactionState::kInProgress | TransactionState::kAborted;
        uassert(50911,
                str::stream() << "Cannot start a transaction at given transaction number "
                              << txnNumber
                              << " a transaction with the same number is in state "
                              << _txnState.toString(),
                _txnState.isInSet(lg, restartableStates));
    }

    _beginMultiDocumentTransaction(lg, txnNumber);
}

void TransactionParticipant::beginTransactionUnconditionally(TxnNumber txnNumber) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _beginMultiDocumentTransaction(lg, txnNumber);
}

void TransactionParticipant::setSpeculativeTransactionOpTime(
    OperationContext* opCtx, SpeculativeTransactionOpTime opTimeChoice) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
    opCtx->recoveryUnit()->setTimestampReadSource(
        opTimeChoice == SpeculativeTransactionOpTime::kAllCommitted
            ? RecoveryUnit::ReadSource::kAllCommittedSnapshot
            : RecoveryUnit::ReadSource::kLastAppliedSnapshot);
    opCtx->recoveryUnit()->preallocateSnapshot();
    auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
    invariant(readTimestamp);
    // Transactions do not survive term changes, so combining "getTerm" here with the
    // recovery unit timestamp does not cause races.
    _speculativeTransactionReadOpTime = {*readTimestamp, replCoord->getTerm()};
    stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
    _transactionMetricsObserver.onChooseReadTimestamp(*readTimestamp);
}

TransactionParticipant::OplogSlotReserver::OplogSlotReserver(OperationContext* opCtx) {
    // Stash the transaction on the OperationContext on the stack. At the end of this function it
    // will be unstashed onto the OperationContext.
    TransactionParticipant::SideTransactionBlock sideTxn(opCtx);

    // Begin a new WUOW and reserve a slot in the oplog.
    WriteUnitOfWork wuow(opCtx);
    _oplogSlot = repl::getNextOpTime(opCtx);

    // Release the WUOW state since this WUOW is no longer in use.
    wuow.release();

    // We must lock the Client to change the Locker on the OperationContext.
    stdx::lock_guard<Client> lk(*opCtx->getClient());

    // The new transaction should have an empty locker, and thus we do not need to save it.
    invariant(opCtx->lockState()->getClientState() == Locker::ClientState::kInactive);
    _locker = opCtx->swapLockState(stdx::make_unique<LockerImpl>());
    _locker->unsetThreadId();

    // This thread must still respect the transaction lock timeout, since it can prevent the
    // transaction from making progress.
    auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
    if (maxTransactionLockMillis >= 0) {
        opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
    }

    // Save the RecoveryUnit from the new transaction and replace it with an empty one.
    _recoveryUnit = opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
}

TransactionParticipant::OplogSlotReserver::~OplogSlotReserver() {
    // If the constructor did not complete, we do not attempt to abort the units of work.
    if (_recoveryUnit) {
        // We should be at WUOW nesting level 1, only the top level WUOW for the oplog reservation
        // side transaction.
        _locker->endWriteUnitOfWork();
        invariant(!_locker->inAWriteUnitOfWork());
        _recoveryUnit->abortUnitOfWork();
    }
}

TransactionParticipant::TxnResources::TxnResources(OperationContext* opCtx, bool keepTicket) {
    // We must lock the Client to change the Locker on the OperationContext.
    stdx::lock_guard<Client> lk(*opCtx->getClient());

    _ruState = opCtx->getWriteUnitOfWork()->release();
    opCtx->setWriteUnitOfWork(nullptr);

    _locker = opCtx->swapLockState(stdx::make_unique<LockerImpl>());
    if (!keepTicket) {
        _locker->releaseTicket();
    }
    _locker->unsetThreadId();

    // This thread must still respect the transaction lock timeout, since it can prevent the
    // transaction from making progress.
    auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
    if (maxTransactionLockMillis >= 0) {
        opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
    }

    _recoveryUnit = opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    _readConcernArgs = repl::ReadConcernArgs::get(opCtx);
}

TransactionParticipant::TxnResources::~TxnResources() {
    if (!_released && _recoveryUnit) {
        // This should only be reached when aborting a transaction that isn't active, i.e.
        // when starting a new transaction before completing an old one.  So we should
        // be at WUOW nesting level 1 (only the top level WriteUnitOfWork).
        _locker->endWriteUnitOfWork();
        invariant(!_locker->inAWriteUnitOfWork());
        _recoveryUnit->abortUnitOfWork();
    }
}

void TransactionParticipant::TxnResources::release(OperationContext* opCtx) {
    // Perform operations that can fail the release before marking the TxnResources as released.
    _locker->reacquireTicket(opCtx);

    invariant(!_released);
    _released = true;

    // We intentionally do not capture the return value of swapLockState(), which is just an empty
    // locker. At the end of the operation, if the transaction is not complete, we will stash the
    // operation context's locker and replace it with a new empty locker.

    // It is necessary to lock the client to change the Locker on the OperationContext.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    invariant(opCtx->lockState()->getClientState() == Locker::ClientState::kInactive);
    opCtx->swapLockState(std::move(_locker));
    opCtx->lockState()->updateThreadIdToCurrentThread();

    auto oldState = opCtx->setRecoveryUnit(std::move(_recoveryUnit),
                                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    invariant(oldState == WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork,
              str::stream() << "RecoveryUnit state was " << oldState);

    opCtx->setWriteUnitOfWork(WriteUnitOfWork::createForSnapshotResume(opCtx, _ruState));

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    readConcernArgs = _readConcernArgs;
}

TransactionParticipant::SideTransactionBlock::SideTransactionBlock(OperationContext* opCtx)
    : _opCtx(opCtx) {
    if (_opCtx->getWriteUnitOfWork()) {
        _txnResources = TransactionParticipant::TxnResources(_opCtx, true /* keepTicket*/);
    }
}

TransactionParticipant::SideTransactionBlock::~SideTransactionBlock() {
    if (_txnResources) {
        // Restore the transaction state onto '_opCtx'.
        _txnResources->release(_opCtx);
    }
}
void TransactionParticipant::_stashActiveTransaction(WithLock, OperationContext* opCtx) {
    if (_inShutdown) {
        return;
    }

    invariant(_activeTxnNumber == opCtx->getTxnNumber());
    {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onStash(ServerTransactionsMetrics::get(opCtx),
                                            curTimeMicros64());
        _transactionMetricsObserver.onTransactionOperation(
            opCtx->getClient(), CurOp::get(opCtx)->debug().additiveMetrics);
    }

    invariant(!_txnResourceStash);
    _txnResourceStash = TxnResources(opCtx);
}


void TransactionParticipant::stashTransactionResources(OperationContext* opCtx) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(opCtx->getTxnNumber());
    stdx::unique_lock<stdx::mutex> lg(_mutex);

    // Always check session's txnNumber, since it can be modified by migration, which does not
    // check out the session. We intentionally do not error if _txnState=kAborted, since we
    // expect this function to be called at the end of the 'abortTransaction' command.
    _checkIsActiveTransaction(lg, *opCtx->getTxnNumber(), false);

    if (!_txnState.inMultiDocumentTransaction(lg)) {
        // Not in a multi-document transaction: nothing to do.
        return;
    }

    _stashActiveTransaction(lg, opCtx);
}

void TransactionParticipant::unstashTransactionResources(OperationContext* opCtx,
                                                         const std::string& cmdName) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(opCtx->getTxnNumber());

    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        // Always check session's txnNumber and '_txnState', since they can be modified by session
        // kill and migration, which do not check out the session.
        _checkIsActiveTransaction(lg, *opCtx->getTxnNumber(), false);
        // If this is not a multi-document transaction, there is nothing to unstash.
        if (_txnState.isNone(lg)) {
            invariant(!_txnResourceStash);
            return;
        }

        _checkIsCommandValidWithTxnState(lg, opCtx, cmdName);

        if (_txnResourceStash) {
            // Transaction resources already exist for this transaction.  Transfer them from the
            // stash to the operation context.
            auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "Only the first command in a transaction may specify a readConcern",
                    readConcernArgs.isEmpty());
            _txnResourceStash->release(opCtx);
            _txnResourceStash = boost::none;
            stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
            _transactionMetricsObserver.onUnstash(ServerTransactionsMetrics::get(opCtx),
                                                  curTimeMicros64());
            return;
        }

        // If we have no transaction resources then we cannot be prepared. If we're not in progress,
        // we don't do anything else.
        invariant(!_txnState.isPrepared(lg));
        if (!_txnState.isInProgress(lg)) {
            // At this point we're either committed and this is a 'commitTransaction' command, or we
            // are in the process of committing.
            return;
        }

        // Stashed transaction resources do not exist for this in-progress multi-document
        // transaction. Set up the transaction resources on the opCtx.
        opCtx->setWriteUnitOfWork(std::make_unique<WriteUnitOfWork>(opCtx));

        // If maxTransactionLockRequestTimeoutMillis is set, then we will ensure no
        // future lock request waits longer than maxTransactionLockRequestTimeoutMillis
        // to acquire a lock. This is to avoid deadlocks and minimize non-transaction
        // operation performance degradations.
        auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
        if (maxTransactionLockMillis >= 0) {
            opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
        }

        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onUnstash(ServerTransactionsMetrics::get(opCtx),
                                              curTimeMicros64());
    }

    // Storage engine transactions may be started in a lazy manner. By explicitly
    // starting here we ensure that a point-in-time snapshot is established during the
    // first operation of a transaction.
    //
    // Active transactions are protected by the locking subsystem, so we must always hold at least a
    // Global intent lock before starting a transaction.  We pessimistically acquire an intent
    // exclusive lock here because we might be doing writes in this transaction, and it is currently
    // not deadlock-safe to upgrade IS to IX.
    Lock::GlobalLock(opCtx, MODE_IX);
    opCtx->recoveryUnit()->preallocateSnapshot();

    // The Client lock must not be held when executing this failpoint as it will block currentOp
    // execution.
    if (MONGO_FAIL_POINT(hangAfterPreallocateSnapshot)) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangAfterPreallocateSnapshot, opCtx, "hangAfterPreallocateSnapshot");
    }
}

Timestamp TransactionParticipant::prepareTransaction(OperationContext* opCtx,
                                                     boost::optional<repl::OpTime> prepareOptime) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    // Always check session's txnNumber and '_txnState', since they can be modified by
    // session kill and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    _getSession()->lockTxnNumber(
        _activeTxnNumber,
        {ErrorCodes::PreparedTransactionInProgress,
         "cannot change transaction number while the session has a prepared transaction"});

    ScopeGuard abortGuard = MakeGuard([&] {
        // Prepare transaction on secondaries should always succeed.
        invariant(!prepareOptime);

        if (lk.owns_lock()) {
            lk.unlock();
        }
        abortActiveTransaction(opCtx);
    });

    _txnState.transitionTo(lk, TransactionState::kPrepared);

    boost::optional<OplogSlotReserver> oplogSlotReserver;
    OplogSlot prepareOplogSlot;
    if (prepareOptime) {
        // On secondary, we just prepare the transaction and discard the buffered ops.
        prepareOplogSlot = OplogSlot(*prepareOptime, 0);
    } else {
        // On primary, we reserve an optime, prepare the transaction and write the oplog entry.
        //
        // Reserve an optime for the 'prepareTimestamp'. This will create a hole in the oplog and
        // cause 'snapshot' and 'afterClusterTime' readers to block until this transaction is done
        // being prepared. When the OplogSlotReserver goes out of scope and is destroyed, the
        // storage-transaction it uses to keep the hole open will abort and the slot (and
        // corresponding oplog hole) will vanish.
        oplogSlotReserver.emplace(opCtx);
        prepareOplogSlot = oplogSlotReserver->getReservedOplogSlot();
        invariant(_prepareOpTime.isNull(),
                  str::stream() << "This transaction has already reserved a prepareOpTime at: "
                                << _prepareOpTime.toString());
        _prepareOpTime = prepareOplogSlot.opTime;

        if (MONGO_FAIL_POINT(hangAfterReservingPrepareTimestamp)) {
            // This log output is used in js tests so please leave it.
            log() << "transaction - hangAfterReservingPrepareTimestamp fail point "
                     "enabled. Blocking until fail point is disabled. Prepare OpTime: "
                  << prepareOplogSlot.opTime;
            MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterReservingPrepareTimestamp);
        }
    }
    opCtx->recoveryUnit()->setPrepareTimestamp(prepareOplogSlot.opTime.getTimestamp());
    opCtx->getWriteUnitOfWork()->prepare();

    // We need to unlock the session to run the opObserver onTransactionPrepare, which calls back
    // into the session.
    lk.unlock();
    opCtx->getServiceContext()->getOpObserver()->onTransactionPrepare(opCtx, prepareOplogSlot);

    abortGuard.Dismiss();

    invariant(!_oldestOplogEntryTS,
              str::stream() << "This transaction's oldest oplog entry Timestamp has already "
                            << "been set to: "
                            << _oldestOplogEntryTS->toString());
    // Keep track of the Timestamp from the first oplog entry written by this transaction.
    _oldestOplogEntryTS = prepareOplogSlot.opTime.getTimestamp();

    // Maintain the Timestamp of the oldest active oplog entry for this transaction. We currently
    // only write an oplog entry for an in progress transaction when it is in the prepare state
    // but this will change when we allow multiple oplog entries per transaction.
    {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onPrepare(ServerTransactionsMetrics::get(opCtx),
                                              *_oldestOplogEntryTS);
    }

    return prepareOplogSlot.opTime.getTimestamp();
}

void TransactionParticipant::addTransactionOperation(OperationContext* opCtx,
                                                     const repl::ReplOperation& operation) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check _getSession()'s txnNumber and '_txnState', since they can be modified by session
    // kill and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // Ensure that we only ever add operations to an in progress transaction.
    invariant(_txnState.isInProgress(lk), str::stream() << "Current state: " << _txnState);

    invariant(_autoCommit && !*_autoCommit && _activeTxnNumber != kUninitializedTxnNumber);
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    _transactionOperations.push_back(operation);
    _transactionOperationBytes += repl::OplogEntry::getReplOperationSize(operation);
    // _transactionOperationBytes is based on the in-memory size of the operation.  With overhead,
    // we expect the BSON size of the operation to be larger, so it's possible to make a transaction
    // just a bit too large and have it fail only in the commit.  It's still useful to fail early
    // when possible (e.g. to avoid exhausting server memory).
    uassert(ErrorCodes::TransactionTooLarge,
            str::stream() << "Total size of all transaction operations must be less than "
                          << BSONObjMaxInternalSize
                          << ". Actual size is "
                          << _transactionOperationBytes,
            _transactionOperationBytes <= BSONObjMaxInternalSize);
}

std::vector<repl::ReplOperation> TransactionParticipant::endTransactionAndRetrieveOperations(
    OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check session's txnNumber and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // Ensure that we only ever end a transaction when prepared or in progress.
    invariant(_txnState.isInSet(lk, TransactionState::kPrepared | TransactionState::kInProgress),
              str::stream() << "Current state: " << _txnState);

    invariant(_autoCommit);
    _transactionOperationBytes = 0;
    return std::move(_transactionOperations);
}

void TransactionParticipant::commitUnpreparedTransaction(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    uassert(ErrorCodes::InvalidOptions,
            "commitTransaction must provide commitTimestamp to prepared transaction.",
            !_txnState.isPrepared(lk));

    // TODO SERVER-37129: Remove this invariant once we allow transactions larger than 16MB.
    invariant(!_oldestOplogEntryTS,
              str::stream() << "The oldest oplog entry Timestamp should not have been set because "
                            << "this transaction is not prepared. But, it is currently "
                            << _oldestOplogEntryTS->toString());

    // We need to unlock the session to run the opObserver onTransactionCommit, which calls back
    // into the session.
    lk.unlock();
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionCommit(opCtx, boost::none, boost::none);
    lk.lock();
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // The oplog entry is written in the same WUOW with the data change for unprepared transactions.
    // We can still consider the state is InProgress until now, since no externally visible changes
    // have been made yet by the commit operation. If anything throws before this point in the
    // function, entry point will abort the transaction.
    _txnState.transitionTo(lk, TransactionState::kCommittingWithoutPrepare);

    lk.unlock();
    _commitStorageTransaction(opCtx);
    lk.lock();
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), false);
    invariant(_txnState.isCommittingWithoutPrepare(lk),
              str::stream() << "Current State: " << _txnState);

    _finishCommitTransaction(lk, opCtx);
}

void TransactionParticipant::commitPreparedTransaction(OperationContext* opCtx,
                                                       Timestamp commitTimestamp) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    uassert(ErrorCodes::InvalidOptions,
            "commitTransaction cannot provide commitTimestamp to unprepared transaction.",
            _txnState.isPrepared(lk));
    uassert(
        ErrorCodes::InvalidOptions, "'commitTimestamp' cannot be null", !commitTimestamp.isNull());
    uassert(ErrorCodes::InvalidOptions,
            "'commitTimestamp' must be greater than or equal to 'prepareTimestamp'",
            commitTimestamp >= _prepareOpTime.getTimestamp());

    _txnState.transitionTo(lk, TransactionState::kCommittingWithPrepare);
    opCtx->recoveryUnit()->setCommitTimestamp(commitTimestamp);

    try {
        // We reserve an oplog slot before committing the transaction so that no writes that are
        // causally related to the transaction commit enter the oplog at a timestamp earlier than
        // the commit oplog entry.
        OplogSlotReserver oplogSlotReserver(opCtx);
        const auto commitOplogSlot = oplogSlotReserver.getReservedOplogSlot();
        invariant(commitOplogSlot.opTime.getTimestamp() >= commitTimestamp,
                  str::stream() << "Commit oplog entry must be greater than or equal to commit "
                                   "timestamp due to causal consistency. commit timestamp: "
                                << commitTimestamp.toBSON()
                                << ", commit oplog entry optime: "
                                << commitOplogSlot.opTime.toBSON());

        // We need to unlock the session to run the opObserver onTransactionCommit, which calls back
        // into the session. We also do not want to write to storage with the mutex locked.
        lk.unlock();
        _commitStorageTransaction(opCtx);

        auto opObserver = opCtx->getServiceContext()->getOpObserver();
        invariant(opObserver);
        opObserver->onTransactionCommit(opCtx, commitOplogSlot, commitTimestamp);

        lk.lock();
        _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

        _finishCommitTransaction(lk, opCtx);
        _getSession()->unlockTxnNumber();

    } catch (...) {
        // It is illegal for committing a prepared transaction to fail for any reason, other than an
        // invalid command, so we crash instead.
        severe() << "Caught exception during commit of prepared transaction "
                 << opCtx->getTxnNumber() << " on " << _getSession()->getSessionId().toBSON()
                 << ": " << exceptionToStatus();
        std::terminate();
    }
}

void TransactionParticipant::_commitStorageTransaction(OperationContext* opCtx) try {
    invariant(opCtx->getWriteUnitOfWork());
    opCtx->getWriteUnitOfWork()->commit();
    opCtx->setWriteUnitOfWork(nullptr);

    // We must clear the recovery unit and locker for the 'config.transactions' and oplog entry
    // writes.
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    opCtx->lockState()->unsetMaxLockTimeout();
} catch (...) {
    // It is illegal for committing a storage-transaction to fail so we crash instead.
    severe() << "Caught exception during commit of storage-transaction " << opCtx->getTxnNumber()
             << " on " << _getSession()->getSessionId().toBSON() << ": " << exceptionToStatus();
    std::terminate();
}

void TransactionParticipant::_finishCommitTransaction(WithLock lk, OperationContext* opCtx) {
    // If no writes have been done, set the client optime forward to the read timestamp so waiting
    // for write concern will ensure all read data was committed.
    //
    // TODO(SERVER-34881): Once the default read concern is speculative majority, only set the
    // client optime forward if the original read concern level is "majority" or "snapshot".
    auto& clientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
    if (_speculativeTransactionReadOpTime > clientInfo.getLastOp()) {
        clientInfo.setLastOp(_speculativeTransactionReadOpTime);
    }

    _txnState.transitionTo(lk, TransactionState::kCommitted);

    const auto curTime = curTimeMicros64();
    {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onCommit(ServerTransactionsMetrics::get(opCtx),
                                             curTime,
                                             _oldestOplogEntryTS,
                                             &Top::get(getGlobalServiceContext()));
        _transactionMetricsObserver.onTransactionOperation(
            opCtx->getClient(), CurOp::get(opCtx)->debug().additiveMetrics);
    }

    // We must clear the recovery unit and locker so any post-transaction writes can run without
    // transactional settings such as a read timestamp.
    _cleanUpTxnResourceOnOpCtx(lk, opCtx, TransactionState::kCommitted);
}

void TransactionParticipant::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    _inShutdown = true;
    _txnResourceStash = boost::none;
}

void TransactionParticipant::abortArbitraryTransaction() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (!_txnState.isInProgress(lock)) {
        // We do not want to abort transactions that are prepared unless we get an
        // 'abortTransaction' command.
        return;
    }

    _abortTransactionOnSession(lock);
}

void TransactionParticipant::abortArbitraryTransactionIfExpired() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (!_txnState.isInProgress(lock) || !_transactionExpireDate ||
        _transactionExpireDate >= Date_t::now()) {
        return;
    }

    const auto session = _getSession();
    auto currentOperation = session->getCurrentOperation();
    if (currentOperation) {
        // If an operation is still running for this transaction when it expires, kill the currently
        // running operation.
        stdx::lock_guard<Client> clientLock(*currentOperation->getClient());
        getGlobalServiceContext()->killOperation(currentOperation, ErrorCodes::ExceededTimeLimit);
    }

    // Log after killing the current operation because jstests may wait to see this log message to
    // imply that the operation has been killed.
    log() << "Aborting transaction with txnNumber " << _activeTxnNumber << " on session with lsid "
          << session->getSessionId().getId()
          << " because it has been running for longer than 'transactionLifetimeLimitSeconds'";

    _abortTransactionOnSession(lock);
}

void TransactionParticipant::abortActiveTransaction(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    // This function shouldn't throw if the transaction is already aborted.
    _checkIsActiveTransaction(lock, *opCtx->getTxnNumber(), false);
    _abortActiveTransaction(
        std::move(lock), opCtx, TransactionState::kInProgress | TransactionState::kPrepared);
}

void TransactionParticipant::abortActiveUnpreparedOrStashPreparedTransaction(
    OperationContext* opCtx) try {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (_txnState.isInSet(lock, TransactionState::kNone)) {
        // If there is no active transaction, do nothing.
        return;
    }

    // We do this check to follow convention and maintain safety. If this were to throw we should
    // have returned in the check above. As a result, throwing here is fatal.
    _checkIsActiveTransaction(lock, *opCtx->getTxnNumber(), false);

    // Stash the transaction if it's in prepared state.
    if (_txnState.isInSet(lock, TransactionState::kPrepared)) {
        _stashActiveTransaction(lock, opCtx);
        return;
    }

    // TODO SERVER-37129: Remove this invariant once we allow transactions larger than 16MB.
    invariant(!_oldestOplogEntryTS,
              str::stream() << "The oldest oplog entry Timestamp should not have been set because "
                            << "this transaction is not prepared. But, it is currently "
                            << _oldestOplogEntryTS->toString());

    _abortActiveTransaction(std::move(lock), opCtx, TransactionState::kInProgress);
} catch (...) {
    // It is illegal for this to throw so we catch and log this here for diagnosability.
    severe() << "Caught exception during transaction " << opCtx->getTxnNumber()
             << " abort or stash on " << _getSession()->getSessionId().toBSON() << " in state "
             << _txnState << ": " << exceptionToStatus();
    std::terminate();
}

void TransactionParticipant::_abortActiveTransaction(stdx::unique_lock<stdx::mutex> lock,
                                                     OperationContext* opCtx,
                                                     TransactionState::StateSet expectedStates) {
    invariant(!_txnResourceStash);
    invariant(!_txnState.isCommittingWithPrepare(lock));

    if (!_txnState.isNone(lock)) {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onTransactionOperation(
            opCtx->getClient(), CurOp::get(opCtx)->debug().additiveMetrics);
    }

    // We write the abort oplog entry before aborting the transaction so that no writes that are
    // causally related to the transaction aborting enter the oplog with a timestamp earlier
    // than the abort oplog entry's timestamp. This is required so that secondaries apply subsequent
    // operations on a document with a prepared update after the prepared update is aborted.
    // We need to unlock the mutex to run the opObserver onTransactionAbort, which calls back
    // into the TransactionParticipant.
    lock.unlock();
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionAbort(opCtx);
    lock.lock();
    // We do not check if the active transaction number is correct here because we handle it below.

    // Only abort the transaction in session if it's in expected states.
    // When the state of active transaction on session is not expected, it means another
    // thread has already aborted the transaction on session.
    if (_txnState.isInSet(lock, expectedStates)) {
        invariant(opCtx->getTxnNumber() == _activeTxnNumber);
        _abortTransactionOnSession(lock);
    } else if (opCtx->getTxnNumber() == _activeTxnNumber) {
        if (_txnState.isNone(lock)) {
            // The active transaction is not a multi-document transaction.
            invariant(opCtx->getWriteUnitOfWork() == nullptr);
            return;
        }

        // Cannot abort these states unless they are specified in expectedStates explicitly.
        const auto unabortableStates = TransactionState::kPrepared  //
            | TransactionState::kCommittingWithPrepare              //
            | TransactionState::kCommittingWithoutPrepare           //
            | TransactionState::kCommitted;                         //
        invariant(!_txnState.isInSet(lock, unabortableStates),
                  str::stream() << "Cannot abort transaction in " << _txnState.toString());
    } else {
        // If _activeTxnNumber is higher than ours, it means the transaction is already aborted.
        invariant(_txnState.isInSet(lock, TransactionState::kNone | TransactionState::kAborted));
    }

    // Clean up the transaction resources on the opCtx even if the transaction resources on the
    // session were not aborted. This actually aborts the storage-transaction.
    _cleanUpTxnResourceOnOpCtx(lock, opCtx, TransactionState::kAborted);
}

void TransactionParticipant::_abortTransactionOnSession(WithLock wl) {
    const auto curTime = curTimeMicros64();
    // If the transaction is stashed, then we have aborted an inactive transaction.
    if (_txnResourceStash) {
        // The transaction is stashed, so we abort the inactive transaction on session.
        {
            stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
            _transactionMetricsObserver.onAbortInactive(
                ServerTransactionsMetrics::get(getGlobalServiceContext()),
                curTime,
                _oldestOplogEntryTS,
                &Top::get(getGlobalServiceContext()));
        }
        _logSlowTransaction(wl,
                            &(_txnResourceStash->locker()->getLockerInfo(boost::none))->stats,
                            TransactionState::kAborted,
                            _txnResourceStash->getReadConcernArgs());
        _txnResourceStash = boost::none;
    } else {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.onAbortActive(
            ServerTransactionsMetrics::get(getGlobalServiceContext()),
            curTime,
            _oldestOplogEntryTS,
            &Top::get(getGlobalServiceContext()));
    }

    _transactionOperationBytes = 0;
    _transactionOperations.clear();
    _txnState.transitionTo(wl, TransactionState::kAborted);
    _prepareOpTime = repl::OpTime();
    _oldestOplogEntryTS = boost::none;
    _speculativeTransactionReadOpTime = repl::OpTime();

    _getSession()->unlockTxnNumber();
}

void TransactionParticipant::_cleanUpTxnResourceOnOpCtx(
    WithLock wl, OperationContext* opCtx, TransactionState::StateFlag terminationCause) {
    // Log the transaction if its duration is longer than the slowMS command threshold.
    _logSlowTransaction(
        wl,
        &(opCtx->lockState()->getLockerInfo(CurOp::get(*opCtx)->getLockStatsBase()))->stats,
        terminationCause,
        repl::ReadConcernArgs::get(opCtx));

    // Reset the WUOW. We should be able to abort empty transactions that don't have WUOW.
    if (opCtx->getWriteUnitOfWork()) {
        opCtx->setWriteUnitOfWork(nullptr);
    }

    // We must clear the recovery unit and locker so any post-transaction writes can run without
    // transactional settings such as a read timestamp.
    opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(
                               opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    opCtx->lockState()->unsetMaxLockTimeout();
}

void TransactionParticipant::_checkIsActiveTransaction(WithLock wl,
                                                       const TxnNumber& requestTxnNumber,
                                                       bool checkAbort) const {
    const auto txnNumber = _getSession()->getActiveTxnNumber();
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot perform operations on active transaction " << _activeTxnNumber
                          << " on session "
                          << _getSession()->getSessionId()
                          << " because a different transaction "
                          << txnNumber
                          << " is now active.",
            txnNumber == _activeTxnNumber);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot perform operations on requested transaction "
                          << requestTxnNumber
                          << " on session "
                          << _getSession()->getSessionId()
                          << " because a different transaction "
                          << _activeTxnNumber
                          << " is now active.",
            requestTxnNumber == _activeTxnNumber);

    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << txnNumber << " has been aborted.",
            !checkAbort || !_txnState.isAborted(wl));
}

void TransactionParticipant::_checkIsCommandValidWithTxnState(WithLock wl,
                                                              OperationContext* opCtx,
                                                              const std::string& cmdName) {
    // Throw NoSuchTransaction error instead of TransactionAborted error since this is the entry
    // point of transaction execution.
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << *opCtx->getTxnNumber() << " has been aborted.",
            !_txnState.isAborted(wl));

    // Cannot change committed transaction but allow retrying commitTransaction command.
    uassert(ErrorCodes::TransactionCommitted,
            str::stream() << "Transaction " << *opCtx->getTxnNumber() << " has been committed.",
            cmdName == "commitTransaction" || !_txnState.isCommitted(wl));

    // Disallow operations other than abort, prepare or commit on a prepared transaction
    uassert(ErrorCodes::PreparedTransactionInProgress,
            str::stream() << "Cannot call any operation other than abort, prepare or commit on"
                          << " a prepared transaction",
            !_txnState.isPrepared(wl) ||
                preparedTxnCmdWhitelist.find(cmdName) != preparedTxnCmdWhitelist.cend());
}

Status TransactionParticipant::isValid(StringData dbName, StringData cmdName) {
    if (cmdName == "count"_sd) {
        return {ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot run 'count' in a multi-document transaction. Please see "
                "http://dochub.mongodb.org/core/transaction-count for a recommended alternative."};
    }

    if (txnCmdWhitelist.find(cmdName) == txnCmdWhitelist.cend() &&
        !(getTestCommandsEnabled() &&
          txnCmdForTestingWhitelist.find(cmdName) != txnCmdForTestingWhitelist.cend())) {
        return {ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot run '" << cmdName << "' in a multi-document transaction."};
    }

    if (dbName == "config"_sd || dbName == "local"_sd ||
        (dbName == "admin"_sd && txnAdminCommands.find(cmdName) == txnAdminCommands.cend())) {
        return {ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot run command against the '" << dbName
                              << "' database in a transaction"};
    }

    return Status::OK();
}

BSONObj TransactionParticipant::reportStashedState() const {
    BSONObjBuilder builder;
    reportStashedState(&builder);
    return builder.obj();
}

void TransactionParticipant::reportStashedState(BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> lm(_mutex);

    if (_txnResourceStash && _txnResourceStash->locker()) {
        if (auto lockerInfo = _txnResourceStash->locker()->getLockerInfo(boost::none)) {
            invariant(_activeTxnNumber != kUninitializedTxnNumber);
            builder->append("host", getHostNameCachedAndPort());
            builder->append("desc", "inactive transaction");

            const auto& lastClientInfo =
                _transactionMetricsObserver.getSingleTransactionStats().getLastClientInfo();
            builder->append("client", lastClientInfo.clientHostAndPort);
            builder->append("connectionId", lastClientInfo.connectionId);
            builder->append("appName", lastClientInfo.appName);
            builder->append("clientMetadata", lastClientInfo.clientMetadata);

            {
                BSONObjBuilder lsid(builder->subobjStart("lsid"));
                _getSession()->getSessionId().serialize(&lsid);
            }

            BSONObjBuilder transactionBuilder;
            _reportTransactionStats(
                lm, &transactionBuilder, _txnResourceStash->getReadConcernArgs());

            builder->append("transaction", transactionBuilder.obj());
            builder->append("waitingForLock", false);
            builder->append("active", false);

            fillLockerInfo(*lockerInfo, *builder);
        }
    }
}

void TransactionParticipant::reportUnstashedState(repl::ReadConcernArgs readConcernArgs,
                                                  BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> lm(_metricsMutex);

    // This method may only take the metrics mutex, as it is called with the Client mutex held.  So
    // we cannot check the stashed state directly.  Instead, a transaction is considered unstashed
    // if it is not actually a transaction (retryable write, no stash used), or is active (not
    // stashed), or has ended (any stash would be cleared).

    const auto& singleTransactionStats = _transactionMetricsObserver.getSingleTransactionStats();
    if (!singleTransactionStats.isForMultiDocumentTransaction() ||
        singleTransactionStats.isActive() || singleTransactionStats.isEnded()) {
        BSONObjBuilder transactionBuilder;
        _reportTransactionStats(lm, &transactionBuilder, readConcernArgs);
        builder->append("transaction", transactionBuilder.obj());
    }
}

std::string TransactionParticipant::TransactionState::toString(StateFlag state) {
    switch (state) {
        case TransactionParticipant::TransactionState::kNone:
            return "TxnState::None";
        case TransactionParticipant::TransactionState::kInProgress:
            return "TxnState::InProgress";
        case TransactionParticipant::TransactionState::kPrepared:
            return "TxnState::Prepared";
        case TransactionParticipant::TransactionState::kCommittingWithoutPrepare:
            return "TxnState::CommittingWithoutPrepare";
        case TransactionParticipant::TransactionState::kCommittingWithPrepare:
            return "TxnState::CommittingWithPrepare";
        case TransactionParticipant::TransactionState::kCommitted:
            return "TxnState::Committed";
        case TransactionParticipant::TransactionState::kAborted:
            return "TxnState::Aborted";
    }
    MONGO_UNREACHABLE;
}

bool TransactionParticipant::TransactionState::_isLegalTransition(StateFlag oldState,
                                                                  StateFlag newState) {
    switch (oldState) {
        case kNone:
            switch (newState) {
                case kNone:
                case kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kInProgress:
            switch (newState) {
                case kNone:
                case kPrepared:
                case kCommittingWithoutPrepare:
                case kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kPrepared:
            switch (newState) {
                case kCommittingWithPrepare:
                case kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kCommittingWithPrepare:
        case kCommittingWithoutPrepare:
            switch (newState) {
                case kNone:
                case kCommitted:
                case kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kCommitted:
            switch (newState) {
                case kNone:
                case kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kAborted:
            switch (newState) {
                case kNone:
                case kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

void TransactionParticipant::TransactionState::transitionTo(WithLock,
                                                            StateFlag newState,
                                                            TransitionValidation shouldValidate) {
    if (shouldValidate == TransitionValidation::kValidateTransition) {
        invariant(TransactionState::_isLegalTransition(_state, newState),
                  str::stream() << "Current state: " << toString(_state)
                                << ", Illegal attempted next state: "
                                << toString(newState));
    }

    _state = newState;
}

void TransactionParticipant::_reportTransactionStats(WithLock wl,
                                                     BSONObjBuilder* builder,
                                                     repl::ReadConcernArgs readConcernArgs) const {
    _transactionMetricsObserver.getSingleTransactionStats().report(builder, readConcernArgs);
}

void TransactionParticipant::_updateState(WithLock wl, const Session::RefreshState& newState) {
    if (newState.refreshCount <= _lastStateRefreshCount) {
        return;
    }

    _activeTxnNumber = newState.txnNumber;
    if (newState.isCommitted) {
        _txnState.transitionTo(wl,
                               TransactionState::kCommitted,
                               TransactionState::TransitionValidation::kRelaxTransitionValidation);
    }

    _lastStateRefreshCount = newState.refreshCount;
}

std::string TransactionParticipant::_transactionInfoForLog(
    const SingleThreadedLockStats* lockStats,
    TransactionState::StateFlag terminationCause,
    repl::ReadConcernArgs readConcernArgs) {
    invariant(lockStats);
    invariant(terminationCause == TransactionState::kCommitted ||
              terminationCause == TransactionState::kAborted);

    StringBuilder s;

    // User specified transaction parameters.
    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _getSession()->getSessionId().serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", _activeTxnNumber);
    parametersBuilder.append("autocommit", _autoCommit ? *_autoCommit : true);
    readConcernArgs.appendInfo(&parametersBuilder);

    s << "parameters:" << parametersBuilder.obj().toString() << ",";

    s << " readTimestamp:" << _speculativeTransactionReadOpTime.getTimestamp().toString() << ",";

    auto singleTransactionStats = _transactionMetricsObserver.getSingleTransactionStats();

    s << singleTransactionStats.getOpDebug()->additiveMetrics.report();

    std::string terminationCauseString =
        terminationCause == TransactionState::kCommitted ? "committed" : "aborted";
    s << " terminationCause:" << terminationCauseString;

    auto curTime = curTimeMicros64();
    s << " timeActiveMicros:"
      << durationCount<Microseconds>(singleTransactionStats.getTimeActiveMicros(curTime));
    s << " timeInactiveMicros:"
      << durationCount<Microseconds>(singleTransactionStats.getTimeInactiveMicros(curTime));

    // Number of yields is always 0 in multi-document transactions, but it is included mainly to
    // match the format with other slow operation logging messages.
    s << " numYields:" << 0;
    // Aggregate lock statistics.

    BSONObjBuilder locks;
    lockStats->report(&locks);
    s << " locks:" << locks.obj().toString();

    // Total duration of the transaction.
    s << " "
      << Milliseconds{static_cast<long long>(singleTransactionStats.getDuration(curTime)) / 1000};

    return s.str();
}

void TransactionParticipant::_logSlowTransaction(WithLock wl,
                                                 const SingleThreadedLockStats* lockStats,
                                                 TransactionState::StateFlag terminationCause,
                                                 repl::ReadConcernArgs readConcernArgs) {
    // Only log multi-document transactions.
    if (!_txnState.isNone(wl)) {
        // Log the transaction if its duration is longer than the slowMS command threshold.
        if (_transactionMetricsObserver.getSingleTransactionStats().getDuration(curTimeMicros64()) >
            serverGlobalParams.slowMS * 1000ULL) {
            log(logger::LogComponent::kTransaction)
                << "transaction "
                << _transactionInfoForLog(lockStats, terminationCause, readConcernArgs);
        }
    }
}

void TransactionParticipant::checkForNewTxnNumber() {
    auto txnNumber = _getSession()->getActiveTxnNumber();

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    if (txnNumber > _activeTxnNumber) {
        _setNewTxnNumber(lg, txnNumber);
    }
}

void TransactionParticipant::_setNewTxnNumber(WithLock wl, const TxnNumber& txnNumber) {
    invariant(!_txnState.isInSet(
        wl, TransactionState::kPrepared | TransactionState::kCommittingWithPrepare));

    // Abort the existing transaction if it's not prepared, committed, or aborted.
    if (_txnState.isInProgress(wl)) {
        _abortTransactionOnSession(wl);
    }

    _activeTxnNumber = txnNumber;
    _txnState.transitionTo(wl, TransactionState::kNone);
    {
        stdx::lock_guard<stdx::mutex> lm(_metricsMutex);
        _transactionMetricsObserver.resetSingleTransactionStats(txnNumber);
    }
    _prepareOpTime = repl::OpTime();
    _oldestOplogEntryTS = boost::none;
    _speculativeTransactionReadOpTime = repl::OpTime();
    _multikeyPathInfo.clear();
    _autoCommit = boost::none;
}

}  // namespace mongo
