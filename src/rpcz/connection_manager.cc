// Copyright 2011 Google Inc. All Rights Reserved.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: nadavs@google.com <Nadav Samet>

#include "rpcz/connection_manager.h"

#include <algorithm>
#include "boost/lexical_cast.hpp"
#include "boost/thread/thread.hpp"
#include "boost/thread/tss.hpp"
#include <map>
#include <ostream>
#include <pthread.h>
#include <sstream>
#include <stddef.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#include "zmq.h"
#include "zmq.hpp"

#include "google/protobuf/stubs/common.h"
#include "rpcz/callback.h"
#include "rpcz/clock.h"
#include "rpcz/event_manager.h"
#include "rpcz/logging.h"
#include "rpcz/macros.h"
#include "rpcz/reactor.h"
#include "rpcz/remote_response.h"
#include "rpcz/sync_event.h"

namespace rpcz {
namespace {
static const uint64 kLargePrime = (1ULL << 63) - 165;
static const uint64 kGenerator = 2;

typedef uint64 EventId;

class EventIdGenerator {
 public:
  EventIdGenerator() {
    state_ = (reinterpret_cast<uint64>(this) << 32) + getpid();
  }

  EventId GetNext() {
    state_ = (state_ * kGenerator) % kLargePrime;
    return state_;
  }

 private:
  uint64 state_;
  DISALLOW_COPY_AND_ASSIGN(EventIdGenerator);
};
}  // unnamed namespace

struct RemoteResponseWrapper {
  RemoteResponse* remote_response;
  int64 deadline_ms;
  uint64 start_time;
  Closure* closure;
  MessageVector return_path;
};

void Connection::SendRequest(
    MessageVector* request,
    RemoteResponse* response,
    int64 deadline_ms,
    Closure* closure) {
  RemoteResponseWrapper* wrapper = new RemoteResponseWrapper;
  wrapper->remote_response = response;
  wrapper->start_time = zclock_time();
  wrapper->deadline_ms = deadline_ms;
  wrapper->closure = closure;

  zmq::socket_t& socket = manager_->GetFrontendSocket();
  SendEmptyMessage(&socket, ZMQ_SNDMORE);
  SendString(&socket, "REQUEST", ZMQ_SNDMORE);
  SendUint64(&socket, connection_id_, ZMQ_SNDMORE);
  SendPointer(&socket, wrapper, ZMQ_SNDMORE);
  WriteVectorToSocket(&socket, *request);
}

class ConnectionManagerThread {
 public:
  ConnectionManagerThread(
      zmq::context_t* context,
      EventManager* external_event_manager,
      ConnectionManager* connection_manager) {
    context_ = context;
    external_event_manager_ = external_event_manager;
    connection_manager_ = connection_manager;
  }

  static void Run(zmq::context_t* context,
                  EventManager* external_event_manager,
                  zmq::socket_t* frontend_socket,
                  ConnectionManager* connection_manager) {
    ConnectionManagerThread cmt(context, external_event_manager,
                                connection_manager);
    cmt.reactor_.AddSocket(frontend_socket, NewPermanentCallback(
            &cmt, &ConnectionManagerThread::HandleFrontendSocket,
            frontend_socket));
    cmt.reactor_.Loop();
  }

  void HandleFrontendSocket(zmq::socket_t* frontend_socket) {
    MessageIterator iter(*frontend_socket);
    std::string sender = MessageToString(iter.next());
    CHECK_EQ(0, iter.next().size());
    std::string command(MessageToString(iter.next()));
    if (command == "QUIT") {
      reactor_.SetShouldQuit();
      return;
    } else if (command == "CONNECT") {
      std::string endpoint(MessageToString(iter.next()));
      zmq::socket_t* socket = new zmq::socket_t(*context_, ZMQ_DEALER);
      connections_.push_back(socket);
      int linger_ms = 0;
      socket->setsockopt(ZMQ_LINGER, &linger_ms, sizeof(linger_ms));
      socket->connect(endpoint.c_str());
      reactor_.AddSocket(socket, NewPermanentCallback(
              this, &ConnectionManagerThread::HandleClientSocket,
              socket));
                                                             
      SendString(frontend_socket, sender, ZMQ_SNDMORE);
      SendEmptyMessage(frontend_socket, ZMQ_SNDMORE);
      SendUint64(frontend_socket, connections_.size() - 1, 0);
    } else if (command == "REQUEST") {
      uint64 connection_id = InterpretMessage<uint64>(iter.next());
      RemoteResponseWrapper* remote_response =
          InterpretMessage<RemoteResponseWrapper*>(iter.next());
      SendRequest(connections_[connection_id], iter, remote_response);
    }
  }

  void HandleClientSocket(zmq::socket_t* socket) {
    MessageVector messages;
    CHECK(ReadMessageToVector(socket, &messages));
    CHECK(messages.size() >= 1);
    CHECK(messages[0].size() == 0);
    EventId event_id(InterpretMessage<EventId>(messages[1]));
    RemoteResponseMap::iterator iter = remote_response_map_.find(event_id);
    if (iter == remote_response_map_.end()) {
      return;
    }
    RemoteResponseWrapper*& remote_response_wrapper = iter->second;
    RemoteResponse*& remote_response = remote_response_wrapper->remote_response;
    remote_response->reply.transfer(2, messages.size(), messages);
    remote_response->status = RemoteResponse::DONE;
    if (remote_response_wrapper->closure) {
      if (external_event_manager_) {
        external_event_manager_->Add(remote_response_wrapper->closure);
      } else {
        LOG(ERROR) << "Can't run closure: no event manager supplied.";
        delete remote_response_wrapper->closure;
      }
    }
    delete remote_response_wrapper;
    remote_response_map_.erase(iter);
  }

  void SendRequest(zmq::socket_t* socket,
                   MessageIterator& iter,
                   RemoteResponseWrapper* remote_response_wrapper) {
    EventId event_id = event_id_generator_.GetNext();
    remote_response_map_[event_id] = remote_response_wrapper;
    if (remote_response_wrapper->deadline_ms != -1) {
      reactor_.RunClosureAt(
          remote_response_wrapper->start_time +
              remote_response_wrapper->deadline_ms,
          NewCallback(this, &ConnectionManagerThread::HandleTimeout, event_id));
    }

    SendString(socket, "", ZMQ_SNDMORE);
    SendUint64(socket, event_id, ZMQ_SNDMORE);
    while (iter.has_more()) {
      socket->send(iter.next(), iter.has_more() ? ZMQ_SNDMORE : 0);
    }
  }

  void HandleTimeout(EventId event_id) {
    RemoteResponseMap::iterator iter = remote_response_map_.find(event_id);
    if (iter == remote_response_map_.end()) {
      return;
    }
    RemoteResponseWrapper*& remote_response_wrapper = iter->second;
    RemoteResponse*& remote_response = remote_response_wrapper->remote_response;
    remote_response->status = RemoteResponse::DEADLINE_EXCEEDED;
    if (remote_response_wrapper->closure) {
      if (external_event_manager_) {
        external_event_manager_->Add(remote_response_wrapper->closure);
      } else {
        LOG(ERROR) << "Can't run closure: no event manager supplied.";
        delete remote_response_wrapper->closure;
      }
    }
    delete remote_response_wrapper;
    remote_response_map_.erase(iter);
  }

 private:
  typedef std::map<EventId, RemoteResponseWrapper*> RemoteResponseMap;
  typedef std::map<uint64, EventId> DeadlineMap;
  ConnectionManager* connection_manager_;
  RemoteResponseMap remote_response_map_;
  DeadlineMap deadline_map_;
  EventIdGenerator event_id_generator_;
  EventManager* external_event_manager_;
  Reactor reactor_;
  std::vector<zmq::socket_t*> connections_;
  zmq::context_t* context_;
};

ConnectionManager::ConnectionManager(
    zmq::context_t* context, EventManager* event_manager)
  : context_(context),
    external_event_manager_(event_manager),
    frontend_endpoint_("inproc://" +
                       boost::lexical_cast<std::string>(this) + ".cm.frontend")
{
  zmq::socket_t* frontend_socket = new zmq::socket_t(*context, ZMQ_ROUTER);
  frontend_socket->bind(frontend_endpoint_.c_str());
  thread_ = boost::thread(&ConnectionManagerThread::Run,
                          context, external_event_manager_,
                          frontend_socket, this);
}

zmq::socket_t& ConnectionManager::GetFrontendSocket() {
  zmq::socket_t* socket = socket_.get();
  if (socket == NULL) {
    LOG(INFO) << "Creating socket. Context_=" << (size_t)context_;
    socket = new zmq::socket_t(*context_, ZMQ_DEALER);
    socket->connect(frontend_endpoint_.c_str());
    socket_.reset(socket);
  }
  return *socket;
}

Connection ConnectionManager::Connect(const std::string& endpoint) {
  zmq::socket_t& socket = GetFrontendSocket();
  SendEmptyMessage(&socket, ZMQ_SNDMORE);
  SendString(&socket, "CONNECT", ZMQ_SNDMORE);
  SendString(&socket, endpoint, 0);
  zmq::message_t msg;
  socket.recv(&msg);
  socket.recv(&msg);
  uint64 connection_id = InterpretMessage<uint64>(msg);
  return Connection(this, connection_id);
}
 
ConnectionManager::~ConnectionManager() {
  LOG(INFO) << "Tearing down";
  zmq::socket_t& socket = GetFrontendSocket();
  SendEmptyMessage(&socket, ZMQ_SNDMORE);
  SendString(&socket, "QUIT", 0);
  thread_.join();
  socket_.reset(NULL);
}
}  // namespace rpcz
