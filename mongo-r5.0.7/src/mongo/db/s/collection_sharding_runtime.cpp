/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_sharding_runtime.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/type_collection_timeseries_fields_gen.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

class UnshardedCollection : public ScopedCollectionDescription::Impl {
public:
    UnshardedCollection() = default;

    const CollectionMetadata& get() override {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
};

const auto kUnshardedCollection = std::make_shared<UnshardedCollection>();

boost::optional<ChunkVersion> getOperationReceivedVersion(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    // If there is a version attached to the OperationContext, use it as the received version.
    if (OperationShardingState::isOperationVersioned(opCtx)) {
        return OperationShardingState::get(opCtx).getShardVersion(nss);
    }

    // There is no shard version information on the 'opCtx'. This means that the operation
    // represented by 'opCtx' is unversioned, and the shard version is always OK for unversioned
    // operations.
    return boost::none;
}

}  // namespace

CollectionShardingRuntime::CollectionShardingRuntime(
    ServiceContext* service,
    NamespaceString nss,
    std::shared_ptr<executor::TaskExecutor> rangeDeleterExecutor)
    : _serviceContext(service),
      _nss(std::move(nss)),
      _rangeDeleterExecutor(std::move(rangeDeleterExecutor)),
      _stateChangeMutex(_nss.toString()),
      _metadataType(_nss.isNamespaceAlwaysUnsharded() ? MetadataType::kUnsharded
                                                      : MetadataType::kUnknown) {}

CollectionShardingRuntime* CollectionShardingRuntime::get(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    //每个表对应一个CollectionShardingState，所有表最终存入CollectionShardingStateMap
    auto* const css = CollectionShardingState::get(opCtx, nss);
	//CollectionShardingRuntime继承css
    return checked_cast<CollectionShardingRuntime*>(css);
}

CollectionShardingRuntime* CollectionShardingRuntime::get(CollectionShardingState* css) {
    return checked_cast<CollectionShardingRuntime*>(css);
}

CollectionShardingRuntime* CollectionShardingRuntime::get_UNSAFE(ServiceContext* svcCtx,
                                                                 const NamespaceString& nss) {
    auto* const css = CollectionShardingState::get_UNSAFE(svcCtx, nss);
    return checked_cast<CollectionShardingRuntime*>(css);
}

ScopedCollectionFilter CollectionShardingRuntime::getOwnershipFilter(
    OperationContext* opCtx, OrphanCleanupPolicy orphanCleanupPolicy) {
    const auto optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);
    // No operations should be calling getOwnershipFilter without a shard version
    invariant(optReceivedShardVersion,
              "getOwnershipFilter called by operation that doesn't specify shard version");

    auto metadata = _getMetadataWithVersionCheckAt(
        opCtx, repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime());
    invariant(!ChunkVersion::isIgnoredVersion(*optReceivedShardVersion) ||
                  !metadata->get().allowMigrations() || !metadata->get().isSharded(),
              "For sharded collections getOwnershipFilter cannot be relied on without a valid "
              "shard version");

    return {std::move(metadata)};
}

ScopedCollectionDescription CollectionShardingRuntime::getCollectionDescription(
    OperationContext* opCtx) {
    auto& oss = OperationShardingState::get(opCtx);
    // If the server has been started with --shardsvr, but hasn't been added to a cluster we should
    // consider all collections as unsharded. Also, return unsharded if no shard version or db
    // version is present on the context.
    if (!ShardingState::get(_serviceContext)->enabled() ||
        (!OperationShardingState::isOperationVersioned(opCtx) && !oss.hasDbVersion())) {
        return {kUnshardedCollection};
    }

    auto optMetadata = _getCurrentMetadataIfKnown(boost::none);
    uassert(
        StaleConfigInfo(_nss,
                        ChunkVersion::UNSHARDED(),
                        boost::none,
                        ShardingState::get(_serviceContext)->shardId()),
        str::stream() << "sharding status of collection " << _nss.ns()
                      << " is not currently available for description and needs to be recovered "
                      << "from the config server",
        optMetadata);

    return {std::move(optMetadata)};
}

//onShardVersionMismatch mongod版本检查不匹配的时候通过这里获取缓存的meta得到路由信息
boost::optional<CollectionMetadata> CollectionShardingRuntime::getCurrentMetadataIfKnown() {
    auto optMetadata = _getCurrentMetadataIfKnown(boost::none);
    if (!optMetadata)
        return boost::none;
    return optMetadata->get();
}

//mongod读路由版本检查AutoGetCollectionForReadCommandBase->checkShardVersionOrThrow，
//mongod写路由版本检查assertCanWrite_inlock->checkShardVersionOrThrow
//checkShardVersionOrThrow->CollectionShardingRuntime::checkShardVersionOrThrow
void CollectionShardingRuntime::checkShardVersionOrThrow(OperationContext* opCtx) {
    (void)_getMetadataWithVersionCheckAt(opCtx, boost::none);
}

void CollectionShardingRuntime::enterCriticalSectionCatchUpPhase(const CSRLock&,
                                                                 const BSONObj& reason) {
    _critSec.enterCriticalSectionCatchUpPhase(reason);
}

void CollectionShardingRuntime::enterCriticalSectionCommitPhase(const CSRLock&,
                                                                const BSONObj& reason) {
    _critSec.enterCriticalSectionCommitPhase(reason);
}

void CollectionShardingRuntime::rollbackCriticalSectionCommitPhaseToCatchUpPhase(
    const CSRLock&, const BSONObj& reason) {
    _critSec.rollbackCriticalSectionCommitPhaseToCatchUpPhase(reason);
}

void CollectionShardingRuntime::exitCriticalSection(const CSRLock&, const BSONObj& reason) {
    _critSec.exitCriticalSection(reason);
}

void CollectionShardingRuntime::exitCriticalSectionNoChecks(const CSRLock&) {
    _critSec.exitCriticalSectionNoChecks();
}

boost::optional<SharedSemiFuture<void>> CollectionShardingRuntime::getCriticalSectionSignal(
    OperationContext* opCtx, ShardingMigrationCriticalSection::Operation op) {
    auto csrLock = CSRLock::lockShared(opCtx, this);
    return _critSec.getSignal(op);
}

//recoverRefreshShardVersion forceShardFilteringMetadataRefresh 
//获取了新的路由元数据信息，则需要把最新的元数据信息刷新到_metadataManager
void CollectionShardingRuntime::setFilteringMetadata(OperationContext* opCtx,
                                                     CollectionMetadata newMetadata) {
    invariant(!newMetadata.isSharded() || !_nss.isNamespaceAlwaysUnsharded(),
              str::stream() << "Namespace " << _nss.ns() << " must never be sharded.");

    auto csrLock = CSRLock::lockExclusive(opCtx, this);
    stdx::lock_guard lk(_metadataManagerLock);

    if (!newMetadata.isSharded()) {
        LOGV2(21917,
              "Marking collection {namespace} as unsharded",
              "Marking collection as unsharded",
              "namespace"_attr = _nss.ns());
        _metadataType = MetadataType::kUnsharded;
        _metadataManager.reset();
        ++_numMetadataManagerChanges;
    } else if (!_metadataManager ||
               !newMetadata.uuidMatches(_metadataManager->getCollectionUuid())) {
        //第一次获取到meta元数据
        _metadataType = MetadataType::kSharded;
        _metadataManager = std::make_shared<MetadataManager>(
            opCtx->getServiceContext(), _nss, _rangeDeleterExecutor, newMetadata);
        ++_numMetadataManagerChanges;
    } else {
    	//MetadataManager::setFilteringMetadata  
    	//最新的版本添加到_metadata队列，老的从_metadata清除
        _metadataManager->setFilteringMetadata(std::move(newMetadata));
    }
}

//recoverRefreshShardVersion
//释放该表的metadata，内存中清理掉

//CollectionVersionLogOpHandler::commit和ShardServerOpObserver::onUpdate会调用，主节点版本信息变化通过oplog通知从节点清理，然后从新获取最新的
void CollectionShardingRuntime::clearFilteringMetadata(OperationContext* opCtx) {
    const auto csrLock = CSRLock::lockExclusive(opCtx, this);
    stdx::lock_guard lk(_metadataManagerLock);
    if (!_nss.isNamespaceAlwaysUnsharded()) {
        LOGV2_DEBUG(4798530,
                    1,
                    "Clearing metadata for collection {namespace}",
                    "Clearing collection metadata",
                    "namespace"_attr = _nss);
        _metadataType = MetadataType::kUnknown;
        _metadataManager.reset();
    }
}

SharedSemiFuture<void> CollectionShardingRuntime::cleanUpRange(ChunkRange const& range,
                                                               boost::optional<UUID> migrationId,
                                                               CleanWhen when) {
    stdx::lock_guard lk(_metadataManagerLock);
    invariant(_metadataType == MetadataType::kSharded);
    return _metadataManager->cleanUpRange(range, std::move(migrationId), when == kDelayed);
}

Status CollectionShardingRuntime::waitForClean(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const UUID& collectionUuid,
                                               ChunkRange orphanRange,
                                               Milliseconds waitTimeout) {
    auto rangeDeletionWaitDeadline = waitTimeout == Milliseconds::max()
        ? Date_t::max()
        : opCtx->getServiceContext()->getFastClockSource()->now() + waitTimeout;
    while (true) {
        boost::optional<SharedSemiFuture<void>> stillScheduled;

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            auto* const self = CollectionShardingRuntime::get(opCtx, nss);
            stdx::lock_guard lk(self->_metadataManagerLock);

            // If the metadata was reset, or the collection was dropped and recreated since the
            // metadata manager was created, return an error.
            if (!self->_metadataManager ||
                (collectionUuid != self->_metadataManager->getCollectionUuid())) {
                return {ErrorCodes::ConflictingOperationInProgress,
                        "Collection being migrated was dropped and created or otherwise had its "
                        "metadata reset"};
            }

            stillScheduled = self->_metadataManager->trackOrphanedDataCleanup(orphanRange);
            if (!stillScheduled) {
                LOGV2_OPTIONS(21918,
                              {logv2::LogComponent::kShardingMigration},
                              "Finished waiting for deletion of {namespace} range {orphanRange}",
                              "Finished waiting for deletion of orphans",
                              "namespace"_attr = nss.ns(),
                              "orphanRange"_attr = redact(orphanRange.toString()));
                return Status::OK();
            }
        }

        LOGV2_OPTIONS(21919,
                      {logv2::LogComponent::kShardingMigration},
                      "Waiting for deletion of {namespace} range {orphanRange}",
                      "Waiting for deletion of orphans",
                      "namespace"_attr = nss.ns(),
                      "orphanRange"_attr = orphanRange);
        try {
            opCtx->runWithDeadline(rangeDeletionWaitDeadline, ErrorCodes::ExceededTimeLimit, [&] {
                stillScheduled->get(opCtx);
            });
        } catch (const DBException& ex) {
            auto result = ex.toStatus();
            // Swallow RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist error since the
            // collection could either never exist or get dropped directly from the shard after
            // the range deletion task got scheduled.
            if (result != ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist) {
                return result.withContext(str::stream() << "Failed to delete orphaned " << nss.ns()
                                                        << " range " << orphanRange.toString());
            }
        }
    }

    MONGO_UNREACHABLE;
}

//获取mongod对应路由shardversion通过该接口获取
std::shared_ptr<ScopedCollectionDescription::Impl>
CollectionShardingRuntime::_getCurrentMetadataIfKnown(
    const boost::optional<LogicalTime>& atClusterTime) {
    stdx::lock_guard lk(_metadataManagerLock);
    switch (_metadataType) {
        case MetadataType::kUnknown:
            return nullptr;
        case MetadataType::kUnsharded:
            return kUnshardedCollection;
        case MetadataType::kSharded:
            return _metadataManager->getActiveMetadata(atClusterTime);
    };
    MONGO_UNREACHABLE;
}


//mongod收到的mongos路由版本信息mongos<mongod: "error":{"code":13388,"codeName":"StaleConfig","errmsg":"version mismatch detected for test.test2","ns":"test.test2","vReceived
//checkShardVersionOrThrow->CollectionShardingRuntime::checkShardVersionOrThrow ()
//版本检查，版本不一致则会携带"version mismatch detected for"，在外层的以下逻辑开始获取路由信息
// 这个逻辑只会对小版本进行检查，如果大版本不一致，则在外层的下面的调用逻辑进行meta元数据刷新

//请求得外层会判断上面的StaleConfig异常,然后重新从config获取最新的路由信息
//ExecCommandDatabase::_commandExec()->refreshDatabase->onShardVersionMismatchNoExcept->onShardVersionMismatch
//  ->recoverRefreshShardVersion->forceGetCurrentMetadata


//mongod收到的mongos路由版本信息mongos>mongod:  刷路由完成后，才进行对应请求
//shard version不匹配路由刷新流程: ExecCommandDatabase::_commandExec()->refreshCollection->onShardVersionMismatchNoExcept
//db version不匹配流程: ExecCommandDatabase::_commandExec()->refreshDatabase->onDbVersionMismatch


//shard version版本检查，如果版本检查不一致，则重新获取路由信息，然后重新执行SQL，参考ExecCommandDatabase::_commandExec() 中会使用


//mongod读路由版本检查AutoGetCollectionForReadCommandBase->checkShardVersionOrThrow，
//mongod写路由版本检查assertCanWrite_inlock->checkShardVersionOrThrow

//注意CollectionShardingRuntime::_getMetadataWithVersionCheckAt和onShardVersionMismatch的区别，都会进行版本检查
//_getMetadataWithVersionCheckAt作用是请求进来后进行路由版本检查，路由检查不通过才会继续走onShardVersionMismatch确定了路由刷新

//非事务mongos获取到客户端请求后流程:ClusterFind::runQuery->getCollectionRoutingInfoForTxnCmd->CatalogCache::getCollectionRoutingInfo 调用该接口获取路由信息
//mongod获取路由实际上最终调用的是_getCurrentMetadataIfKnown，从缓存的_metadata获取
std::shared_ptr<ScopedCollectionDescription::Impl>
CollectionShardingRuntime::_getMetadataWithVersionCheckAt(
    OperationContext* opCtx, const boost::optional<mongo::LogicalTime>& atClusterTime) {
    const auto optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);
    if (!optReceivedShardVersion)
        return kUnshardedCollection;

	//接受到的mongos携带的版本信息
    const auto& receivedShardVersion = *optReceivedShardVersion;

    // An operation with read concern 'available' should never have shardVersion set.
    invariant(repl::ReadConcernArgs::get(opCtx).getLevel() !=
              repl::ReadConcernLevel::kAvailableReadConcern);

    auto csrLock = CSRLock::lockShared(opCtx, this);

	//这是本地缓存的_metadata中获取
    auto optCurrentMetadata = _getCurrentMetadataIfKnown(atClusterTime);
    uassert(StaleConfigInfo(
                _nss, receivedShardVersion, boost::none, ShardingState::get(opCtx)->shardId()),
            str::stream() << "sharding status of collection " << _nss.ns()
                          << " is not currently known and needs to be recovered",
            optCurrentMetadata);

    const auto& currentMetadata = optCurrentMetadata->get();

    auto wantedShardVersion = currentMetadata.getShardVersion();

    {
        auto criticalSectionSignal = _critSec.getSignal(
            opCtx->lockState()->isWriteLocked() ? ShardingMigrationCriticalSection::kWrite
                                                : ShardingMigrationCriticalSection::kRead);

        uassert(StaleConfigInfo(_nss,
                                receivedShardVersion,
                                wantedShardVersion,
                                ShardingState::get(opCtx)->shardId(),
                                std::move(criticalSectionSignal)),
                str::stream() << "migration commit in progress for " << _nss.ns(),
                !criticalSectionSignal);
    }

    if (wantedShardVersion.isWriteCompatibleWith(receivedShardVersion) ||
        ChunkVersion::isIgnoredVersion(receivedShardVersion))
        return optCurrentMetadata;

    StaleConfigInfo sci(
        _nss, receivedShardVersion, wantedShardVersion, ShardingState::get(opCtx)->shardId());

    uassert(std::move(sci),
            str::stream() << "epoch mismatch detected for " << _nss.ns(),
            wantedShardVersion.epoch() == receivedShardVersion.epoch());

    if (!wantedShardVersion.isSet() && receivedShardVersion.isSet()) {
        uasserted(std::move(sci),
                  str::stream() << "this shard no longer contains chunks for " << _nss.ns() << ", "
                                << "the collection may have been dropped");
    }

    if (wantedShardVersion.isSet() && !receivedShardVersion.isSet()) {
        uasserted(std::move(sci),
                  str::stream() << "this shard contains chunks for " << _nss.ns() << ", "
                                << "but the client expects unsharded collection");
    }

	//主版本不一致，则返回会携带该信息，shard version版本检查在这里
    if (wantedShardVersion.majorVersion() != receivedShardVersion.majorVersion()) {
        // Could be > or < - wanted is > if this is the source of a migration, wanted < if this is
        // the target of a migration
        uasserted(std::move(sci), str::stream() << "version mismatch detected for " << _nss.ns());
    }

    // Those are all the reasons the versions can mismatch
    MONGO_UNREACHABLE;
}

void CollectionShardingRuntime::appendShardVersion(BSONObjBuilder* builder) {
    auto optCollDescr = getCurrentMetadataIfKnown();
    if (optCollDescr) {
        builder->appendTimestamp(_nss.ns(), optCollDescr->getShardVersion().toLong());
    }
}

size_t CollectionShardingRuntime::numberOfRangesScheduledForDeletion() const {
    stdx::lock_guard lk(_metadataManagerLock);
    if (_metadataManager) {
        return _metadataManager->numberOfRangesScheduledForDeletion();
    }
    return 0;
}

//onShardVersionMismatch 
void CollectionShardingRuntime::setShardVersionRecoverRefreshFuture(SharedSemiFuture<void> future,
                                                                    const CSRLock&) {
    invariant(!_shardVersionInRecoverOrRefresh);
    _shardVersionInRecoverOrRefresh.emplace(std::move(future));
}

boost::optional<SharedSemiFuture<void>>
CollectionShardingRuntime::getShardVersionRecoverRefreshFuture(OperationContext* opCtx) {
    auto csrLock = CSRLock::lockShared(opCtx, this);
    return _shardVersionInRecoverOrRefresh;
}

void CollectionShardingRuntime::resetShardVersionRecoverRefreshFuture(const CSRLock&) {
    invariant(_shardVersionInRecoverOrRefresh);
    _shardVersionInRecoverOrRefresh = boost::none;
}

CollectionCriticalSection::CollectionCriticalSection(OperationContext* opCtx,
                                                     NamespaceString nss,
                                                     BSONObj reason)
    : _opCtx(opCtx), _nss(std::move(nss)), _reason(std::move(reason)) {
    // This acquisition is performed with collection lock MODE_S in order to ensure that any ongoing
    // writes have completed and become visible
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_S,
                               AutoGetCollectionViewMode::kViewsForbidden,
                               _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
    invariant(csr->getCurrentMetadataIfKnown());
    csr->enterCriticalSectionCatchUpPhase(csrLock, _reason);
}

CollectionCriticalSection::~CollectionCriticalSection() {
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    AutoGetCollection autoColl(_opCtx, _nss, MODE_IX);
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);
    csr->exitCriticalSection(csrLock, _reason);
}

void CollectionCriticalSection::enterCommitPhase() {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_X,
                               AutoGetCollectionViewMode::kViewsForbidden,
                               _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);
    invariant(csr->getCurrentMetadataIfKnown());
    csr->enterCriticalSectionCommitPhase(csrLock, _reason);
}

}  // namespace mongo
