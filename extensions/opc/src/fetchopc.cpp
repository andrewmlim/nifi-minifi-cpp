/**
 * FetchOPC class definition
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <memory>
#include <string>
#include <list>
#include <map>
#include <mutex>
#include <thread>

#include "opc.h"
#include "fetchopc.h"
#include "utils/ByteArrayCallback.h"
#include "FlowFileRecord.h"
#include "core/Processor.h"
#include "core/ProcessSession.h"
#include "core/Core.h"
#include "core/Property.h"
#include "core/Resource.h"
#include "controllers/SSLContextService.h"
#include "core/logging/LoggerConfiguration.h"
#include "utils/Id.h"
#include "utils/StringUtils.h"

namespace org {
namespace apache {
namespace nifi {
namespace minifi {
namespace processors {
  core::Property FetchOPCProcessor::NodeID(
      core::PropertyBuilder::createProperty("Node ID")
      ->withDescription("Specifies the ID of the root node to traverse")
      ->isRequired(true)->build());


  core::Property FetchOPCProcessor::NodeIDType(
      core::PropertyBuilder::createProperty("Node ID type")
      ->withDescription("Specifies the type of the provided node ID")
      ->isRequired(true)
      ->withAllowableValues<std::string>({"Path", "Int", "String"})->build());

  core::Property FetchOPCProcessor::NameSpaceIndex(
      core::PropertyBuilder::createProperty("Namespace index")
      ->withDescription("The index of the namespace. Used only if node ID type is not path.")
      ->withDefaultValue<int32_t>(0)->build());

  core::Property FetchOPCProcessor::MaxDepth(
      core::PropertyBuilder::createProperty("Max depth")
      ->withDescription("Specifiec the max depth of browsing. 0 means unlimited.")
      ->withDefaultValue<uint64_t>(0)->build());

  core::Relationship FetchOPCProcessor::Success("success", "Successfully retrieved OPC-UA nodes");
  core::Relationship FetchOPCProcessor::Failure("failure", "Retrieved OPC-UA nodes where value cannot be extracted (only if enabled)");


  void FetchOPCProcessor::initialize() {
    // Set the supported properties
    std::set<core::Property> fetchOPCProperties = {OPCServerEndPoint, NodeID, NodeIDType, NameSpaceIndex, MaxDepth};
    std::set<core::Property> baseOPCProperties = BaseOPCProcessor::getSupportedProperties();
    fetchOPCProperties.insert(baseOPCProperties.begin(), baseOPCProperties.end());
    setSupportedProperties(fetchOPCProperties);

    // Set the supported relationships
    setSupportedRelationships({Success, Failure});
  }

  void FetchOPCProcessor::onSchedule(const std::shared_ptr<core::ProcessContext> &context, const std::shared_ptr<core::ProcessSessionFactory> &factory) {
    logger_->log_trace("FetchOPCProcessor::onSchedule");

    translatedNodeIDs_.clear();  // Path might has changed during restart

    BaseOPCProcessor::onSchedule(context, factory);

    if(!configOK_) {
      return;
    }

    configOK_ = false;

    std::string value;
    context->getProperty(NodeID.getName(), nodeID_);
    context->getProperty(NodeIDType.getName(), value);

    maxDepth_ = 0;
    context->getProperty(MaxDepth.getName(), maxDepth_);

    if (value == "String") {
      idType_ = opc::OPCNodeIDType::String;
    } else if (value == "Int") {
      idType_ = opc::OPCNodeIDType::Int;
    } else if (value == "Path") {
      idType_ = opc::OPCNodeIDType::Path;
    } else {
      // Where have our validators gone?
      logger_->log_error("%s is not a valid node ID type!", value.c_str());
    }

    if(idType_ == opc::OPCNodeIDType::Int) {
      try {
        int t = std::stoi(nodeID_);
      } catch(...) {
        logger_->log_error("%s cannot be used as an int type node ID", nodeID_.c_str());
        return;
      }
    }
    if(idType_ != opc::OPCNodeIDType::Path) {
      if(!context->getProperty(NameSpaceIndex.getName(), nameSpaceIdx_)) {
        logger_->log_error("%s is mandatory in case %s is not Path", NameSpaceIndex.getName().c_str(), NodeIDType.getName().c_str());
        return;
      }
    }

    configOK_ = true;
  }

  void FetchOPCProcessor::onTrigger(const std::shared_ptr<core::ProcessContext> &context, const std::shared_ptr<core::ProcessSession> &session){
    if(!configOK_) {
      logger_->log_error("This processor was not configured properly, yielding. Please check for previous errors in the logs!");
      yield();
      return;
    }

    logger_->log_trace("FetchOPCProcessor::onTrigger");

    std::unique_lock<std::mutex> lock(onTriggerMutex_, std::try_to_lock);
    if(!lock.owns_lock()){
      logger_->log_warn("processor was triggered before previous listing finished, configuration should be revised!");
      return;
    }

    if (!reconnect()) {
      yield();
      return;
    }

    nodesFound_ = 0;
    variablesFound_ = 0;

    std::function<opc::nodeFoundCallBackFunc> f = std::bind(&FetchOPCProcessor::nodeFoundCallBack, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, context, session);
    if(idType_ != opc::OPCNodeIDType::Path) {
      UA_NodeId myID;
      myID.namespaceIndex = nameSpaceIdx_;
      if(idType_ == opc::OPCNodeIDType::Int) {
        myID.identifierType = UA_NODEIDTYPE_NUMERIC;
        myID.identifier.numeric = std::stoi(nodeID_);
      } else if (idType_ == opc::OPCNodeIDType::String) {
        myID.identifierType = UA_NODEIDTYPE_STRING;
        myID.identifier.string = UA_STRING_ALLOC(nodeID_.c_str());
      }
      connection_->traverse(myID, f, "", maxDepth_);
    } else {
      if(translatedNodeIDs_.empty()) {
        auto sc = connection_->translateBrowsePathsToNodeIdsRequest(nodeID_, translatedNodeIDs_, logger_);
        if(sc != UA_STATUSCODE_GOOD) {
          logger_->log_error("Failed to translate %s to node id, no flow files will be generated (%s)", nodeID_.c_str(), UA_StatusCode_name(sc));
          yield();
          return;
        }
      }
      for(auto& nodeID: translatedNodeIDs_) {
        connection_->traverse(nodeID, f, nodeID_, maxDepth_);
      }
    }
    if(nodesFound_ == 0) {
      logger_->log_warn("Connected to OPC server, but no variable nodes were not found. Configuration might be incorrect! Yielding...");
      yield();
    } else if (variablesFound_ == 0) {
      logger_->log_warn("Found no variables when traversing the specified node. No flowfiles are generated. Yielding...");
      yield();
    }

  }

  bool FetchOPCProcessor::nodeFoundCallBack(opc::Client& client, const UA_ReferenceDescription *ref, const std::string& path,
      const std::shared_ptr<core::ProcessContext> &context, const std::shared_ptr<core::ProcessSession> &session) {
    nodesFound_++;
    if(ref->nodeClass == UA_NODECLASS_VARIABLE)
    {
      try {
        opc::NodeData nodedata = connection_->getNodeData(ref);
        OPCData2FlowFile(nodedata, context, session);
        variablesFound_++;
      } catch (const std::exception& exception) {
        std::string browsename((char*)ref->browseName.name.data, ref->browseName.name.length);
        logger_->log_warn("Caught Exception while trying to get data from node &s: %s", path + "/" + browsename,  exception.what());
      }
    }
    return true;
  }

  void FetchOPCProcessor::OPCData2FlowFile(const opc::NodeData& opcnode, const std::shared_ptr<core::ProcessContext> &context, const std::shared_ptr<core::ProcessSession> &session) {
    std::shared_ptr<FlowFileRecord> flowFile = std::static_pointer_cast<FlowFileRecord>(session->create());
    if (flowFile == nullptr) {
      logger_->log_error("Failed to create flowfile!");
      return;
    }
    for(const auto& attr: opcnode.attributes) {
      flowFile->setAttribute(attr.first, attr.second);
    }
    if(opcnode.data.size() > 0) {
      try {
        FetchOPCProcessor::WriteCallback callback(opc::nodeValue2String(opcnode));
        session->write(flowFile, &callback);
      } catch (const std::exception& e) {
        std::string browsename;
        flowFile->getAttribute("Browsename", browsename);
        logger_->log_info("Failed to extract data of OPC node %s: %s", browsename, e.what());
        session->transfer(flowFile, Failure);
        return;
      }
    }
    session->transfer(flowFile, Success);
  }

} /* namespace processors */
} /* namespace minifi */
} /* namespace nifi */
} /* namespace apache */
} /* namespace org */
