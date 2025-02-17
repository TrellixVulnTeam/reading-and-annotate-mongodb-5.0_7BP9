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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/grid.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

//mongos通过getCollectionRoutingInfo获取版本信息,mongod通过getCurrentMetadataIfKnown获取版本信息
class GetShardVersion : public BasicCommand {
public:
    GetShardVersion() : BasicCommand("getShardVersion") {}

    std::string help() const override {
        return " example: { getShardVersion : 'alleyinsider.foo'  } ";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::getShardVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }
	
	//mongos通过getCollectionRoutingInfo获取版本信息,mongod通过getCurrentMetadataIfKnown获取版本信息
    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));

        uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

        result.append(
            "configServer",
            Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString().toString());

        AutoGetCollection autoColl(opCtx, nss, MODE_IS, AutoGetCollectionViewMode::kViewsPermitted);
        auto* const csr = CollectionShardingRuntime::get(opCtx, nss);


		//如果启用了分片，则类型为CollectionMetadata    
        const auto optMetadata = csr->getCurrentMetadataIfKnown();
        if (!optMetadata) {
            result.append("global", "UNKNOWN");

            if (cmdObj["fullMetadata"].trueValue()) {
                result.append("metadata", BSONObj());
            }
        } else {
            const auto& metadata = *optMetadata;
			//如果启用了分片，则类型为CollectionMetadata::getShardVersion    
            result.appendTimestamp("global", metadata.getShardVersion().toLong());

			//如果chunk过多，这里会不会超过BSONObjMaxUserSize 16M限制,是不是应该参考GetShardVersion::run做下限制
            if (cmdObj["fullMetadata"].trueValue()) {
                BSONObjBuilder metadataBuilder(result.subobjStart("metadata"));
                if (metadata.isSharded()) {
                    metadata.toBSONBasic(metadataBuilder);

                    BSONArrayBuilder chunksArr(metadataBuilder.subarrayStart("chunks"));
                    metadata.toBSONChunks(&chunksArr);
                    chunksArr.doneFast();
                }
                metadataBuilder.doneFast();
            }
        }

        return true;
    }

} getShardVersionCmd;

}  // namespace
}  // namespace mongo
