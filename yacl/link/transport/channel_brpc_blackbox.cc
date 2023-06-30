// Copyright 2023 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "yacl/link/transport/channel_brpc_blackbox.h"

#include <chrono>
#include <cstddef>
#include <exception>
#include <thread>
#include <utility>

#include "spdlog/spdlog.h"

#include "yacl/base/exception.h"
#include "yacl/link/transport/blackbox_interconnect/blackbox_service_errorcode.h"

#include "interconnection/link/transport.pb.h"
#include "yacl/link/transport/blackbox_interconnect/blackbox_service.pb.h"

namespace yacl::link {

namespace bb_ic = blackbox_interconnect;

namespace ic = org::interconnection;
namespace ic_pb = org::interconnection::link;

namespace {

const auto* const kTransportAddrKey = "system.transport";
const auto* const kTraceIdKey = "config.trace_id";
const auto* const kTokenKey = "config.token";
const auto* const kSessionIdKey = "config.session_id";
// const auto *const kPartyIdKeyPrefix = "config.party_id";
//const auto *const kInstIdKeyPrefix = "config.inst_id";

const auto* const kHttpHeadProviderCode = "x-ptp-tecn-provider-code";
const auto* const kHttpHeadTraceId = "x-ptp-trace-id";
const auto* const kHttpHeadToken = "x-ptp-token";
const auto* const kHttpHeadTargetNodeId = "x-ptp-target-node-id";
const auto* const kHttpHeadSourceNodeId = "x-ptp-source-node-id";

const auto *const kHttpHeadInstId = "x-ptp-target-inst-id";
const auto* const kHttpHeadSessionId = "x-ptp-session-id";
const auto* const kHttpHeadTopic = "x-ptp-topic";
const auto* const kHttpHeadHost = "host";

}  // namespace

namespace util {

class BlackBoxPushDone : public google::protobuf::Closure {
 public:
  explicit BlackBoxPushDone(ChannelBrpcBlackBox& channel, uint32_t push_wait_ms,
                            SendChunkedWindow& window)
      : channel_(channel), push_wait_ms_(push_wait_ms), window_(window) {}
  void Run() override {
    std::unique_ptr<BlackBoxPushDone> self_guard(this);

    if (cntl_.Failed()) {
      SPDLOG_ERROR("send, rpc failed={}, message={}, content: {}, uri: {}",
                   cntl_.ErrorCode(), cntl_.ErrorText(),
                   cntl_.request_attachment().to_string().substr(0, 20),
                   cntl_.http_request().uri().path());
    } else {
      response_.ParseFromString(cntl_.response_attachment().to_string());
      if (response_.code() == bb_ic::error_code::Code("QueueFull")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(push_wait_ms_));
        auto new_done = std::make_unique<BlackBoxPushDone>(
            channel_, push_wait_ms_, window_);
        new_done->cntl_.http_request() =
            std::move(*(cntl_.release_http_request()));
        new_done->cntl_.request_attachment() = cntl_.request_attachment();
        new_done->channel_.DealPushDone(std::move(new_done));
      } /*else if (response_.message() != bb_ic::error_code::Code("OK")) {
        SPDLOG_ERROR("send, peer failed, code={} message={}", response_.code(),
                     response_.message());
        exit(1);
      } */else {
        window_.OnPushDone();
        //        std::cout << "run function over!!!!!!!!!!" << std::endl;
      }
    }
  }

  bb_ic::TransportOutbound response_;

  brpc::Controller cntl_;
  ChannelBrpcBlackBox& channel_;
  uint32_t push_wait_ms_;
  SendChunkedWindow& window_;
};

}  // namespace util
ReceiverLoopBlackBox::~ReceiverLoopBlackBox() { Stop(); }

void ReceiverLoopBlackBox::Stop() {
  for (auto& [_, channel] : listeners_) {
    channel->StopReceive();
  }

}

void* ReceiverLoopBlackBox::ChannelProc(void* param) {
  auto* channel = static_cast<ChannelBrpcBlackBox*>(param);
  channel->StartReceive();
  while (channel->CanReceive()) {
    channel->TryReceive();
  }
  return nullptr;
};

void ReceiverLoopBlackBox::Start() {
  for (auto& [_, channel] : listeners_) {
    bthread_t tid;
    if (bthread_start_background(&tid, nullptr,
                                 ReceiverLoopBlackBox::ChannelProc,
                                 channel.get()) != 0) {
      YACL_THROW("failed to push async sending job to bthread");
    }
  }
}

void ChannelBrpcBlackBox::StartReceive() { is_recv_.store(true); }

bool ChannelBrpcBlackBox::CanReceive() { return is_recv_.load(); }

void ChannelBrpcBlackBox::StopReceive() {
  is_recv_.store(false);
  std::this_thread::sleep_for(std::chrono::seconds(pop_timeout_s_));
  blackbox_interconnect::TransportOutbound response;
  brpc::Controller cntl;
  SetHttpHeader(&cntl, recv_topic_);
  cntl.http_request().uri() = host_ + kUrlPrefix + "release";
  channel_->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);

  blackbox_interconnect::TransportOutbound response2;
  brpc::Controller cntl2;
  SetHttpHeader(&cntl2, send_topic_);
  cntl2.http_request().uri() = host_ + kUrlPrefix + "release";
  channel_->CallMethod(nullptr, &cntl2, nullptr, nullptr, nullptr);
}

void ChannelBrpcBlackBox::TryReceive() {
  bb_ic::TransportOutbound response;

  brpc::Controller cntl;
  std::string recv_topic_tmp = recv_topic_ + std::to_string(l_recv_topic_++);
  SetHttpHeader(&cntl, recv_topic_);
  //SetHttpHeader(&cntl, recv_topic_tmp);
  //std::cout << "Poptopic:" << recv_topic_ << std::endl;
  // cntl.http_request().uri() =
  //     host_ + kUrlPrefix + "pop?timeout=" + std::to_string(pop_timeout_s_);
  cntl.http_request().uri() =
      host_ + kUrlPrefix + "pop";
  //std::this_thread::sleep_for(std::chrono::milliseconds(500));
  channel_->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);

  /*if (cntl.Failed())
{
  int retry = 5;
  while(retry > 0)
  {
    cntl.Reset();
    SetHttpHeader(&cntl, recv_topic_tmp);
    cntl.http_request().uri() = host_ + kUrlPrefix + "pop";
    std::cout << "Pop retry times  ------>" << retry << std::endl;
    channel_->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
    if(cntl.Failed())
    {
      retry--;
      std::this_thread::sleep_for(std::chrono::milliseconds( 1000 * (5 - retry)) );
      continue;
    }
    else
    {
      break;
    }
  }
}*/
  if (cntl.Failed()) {
    SPDLOG_ERROR("Rpc faliled, error_code: {}, error_info: {}",
                 cntl.ErrorCode(), cntl.ErrorText());
    exit(-1);
  } else {
    response.ParseFromString(cntl.response_attachment().to_string());
    if (response.code() == bb_ic::error_code::Code("ResourceNotFound") ||
        response.payload().empty()) {
      SPDLOG_INFO("We will wait for topic: {}", recv_topic_tmp);
    } else if (response.code() != bb_ic::error_code::Code("OK")) {
      SPDLOG_ERROR("pop faliled, error_code: {}, error_info: {}",
                   response.code(), response.message());
      exit(-1);
    } else {
      OnPopResponse(&response);
      return;
    }
  }
}

void ChannelBrpcBlackBox::OnPopResponse(bb_ic::TransportOutbound* response) {
  ic_pb::PushRequest request;
  if (!request.ParseFromString(response->payload())) {
    SPDLOG_ERROR("response cannot be parsed.");
    return;
  }

  ic_pb::PushResponse msg_response;
  OnRequest(&request, &msg_response);
  if (msg_response.mutable_header()->error_code() != ic::ErrorCode::OK) {
    SPDLOG_ERROR("OnRequest failed, error_code: {}, error_info: {}",
                 msg_response.mutable_header()->error_code(),
                 msg_response.mutable_header()->error_msg());
  }
}

brpc::ChannelOptions ChannelBrpcBlackBox::GetChannelOption(
    const SSLOptions* ssl_opts) {
  brpc::ChannelOptions options;
  {
    if (options_.channel_protocol != "http" &&
        options_.channel_protocol != "h2") {
      YACL_THROW_LOGIC_ERROR(
          "channel protocol {} is not valid for blackbox channel",
          options_.channel_protocol);
    }
    options.protocol = options_.channel_protocol;
    options.connection_type = options_.channel_connection_type;
    options.connect_timeout_ms = 20000;
    options.timeout_ms = options_.http_timeout_ms;
    options.max_retry = 3;
    if (ssl_opts != nullptr) {
      options.mutable_ssl_options()->client_cert.certificate =
          ssl_opts->cert.certificate_path;
      options.mutable_ssl_options()->client_cert.private_key =
          ssl_opts->cert.private_key_path;
      options.mutable_ssl_options()->verify.verify_depth =
          ssl_opts->verify.verify_depth;
      options.mutable_ssl_options()->verify.ca_file_path =
          ssl_opts->verify.ca_file_path;
    }
  }
  return options;
}

void ChannelBrpcBlackBox::SetPeerHost(const std::string& self_id,
                                      const std::string& self_node_id,
                                      const std::string& peer_id,
                                      const std::string& peer_node_id,
                                      const SSLOptions* ssl_opts) {
  auto* host = std::getenv(kTransportAddrKey);
  YACL_ENFORCE(host != nullptr, "environment variable {} is not found",
               kTransportAddrKey);
  host_ = host;
  auto options = GetChannelOption(ssl_opts);
  const char* load_balancer = "";
  auto brpc_channel = std::make_unique<brpc::Channel>();

  int res = brpc_channel->Init(host_.c_str(), load_balancer, &options);
  if (res != 0) {
    YACL_THROW_NETWORK_ERROR(
        "Fail to connect to transport service, host={}, err_code={}", host_,
        res);
  }

  auto* trace_id = std::getenv(kTraceIdKey);
  YACL_ENFORCE(trace_id != nullptr, "environment variable {} is not found",
               kTraceIdKey);
  auto* token = std::getenv(kTokenKey);
  YACL_ENFORCE(token != nullptr, "environment variable {} is not found",
               kTokenKey);
  auto* session_id = std::getenv(kSessionIdKey);
  YACL_ENFORCE(session_id != nullptr, "environment variable {} is not found",
               kSessionIdKey);
//  auto* ins_id = std::getenv(kInstIdKeyPrefix);
//  YACL_ENFORCE(ins_id != nullptr, "environment variable {} is not found",
//               kInstIdKeyPrefix);

  channel_ = std::move(brpc_channel);
  send_topic_ = self_id + peer_id;
  recv_topic_ = peer_id + self_id;
  peer_host_ = peer_id;

  http_headers_[kHttpHeadInstId] = peer_node_id;
  http_headers_[kHttpHeadProviderCode] = "InsightOne";
  http_headers_[kHttpHeadTraceId] = trace_id;
  http_headers_[kHttpHeadToken] = token;
  http_headers_[kHttpHeadTargetNodeId] = peer_node_id;
  http_headers_[kHttpHeadSourceNodeId] = self_node_id;
  http_headers_[kHttpHeadSessionId] = session_id;
  http_headers_[kHttpHeadHost] = host_;
  // << "ins_id:" << ins_id << " tar peer_node_id:" << peer_node_id << " souce self_node_id:" << self_node_id << std::endl;
}

void ChannelBrpcBlackBox::SetHttpHeader(brpc::Controller* controller,
                                        const std::string& topic) {
  for (auto& [k, v] : http_headers_) {
    controller->http_request().SetHeader(k, v);
  }
  controller->http_request().SetHeader(kHttpHeadTopic, topic);
  controller->http_request().set_method(brpc::HTTP_METHOD_POST);
}

void ChannelBrpcBlackBox::TransResponse(
    const bb_ic::TransportOutbound* new_response,
    ic_pb::PushResponse* response) {
  if (new_response->message() == bb_ic::error_code::Code("OK")) {
    response->mutable_header()->set_error_code(ic::ErrorCode::OK);
  } else {
    response->mutable_header()->set_error_code(ic::ErrorCode::NETWORK_ERROR);
    response->mutable_header()->set_error_msg(
        fmt::format("blackbox response error, code {}, msg {}",
                    new_response->code(), new_response->message()));
  }
}

void ChannelBrpcBlackBox::DealPushDone(
    std::unique_ptr<util::BlackBoxPushDone> done) {
  done->cntl_.ignore_eovercrowded();
  //std::this_thread::sleep_for(std::chrono::milliseconds(500));
  channel_->CallMethod(nullptr, &done->cntl_, nullptr, nullptr, done.get());
  static_cast<void>(done.release());
}

void ChannelBrpcBlackBox::PushRequest(ic_pb::PushRequest& request,
                                      uint32_t timeout) {
  brpc::Controller cntl;
  if (timeout != 0) {
    cntl.set_timeout_ms(timeout);
  }

  cntl.http_request().uri() = host_ + kUrlPrefix + "push";
  std::string send_topic_tmp = send_topic_ + std::to_string(l_send_topic_++);
  SetHttpHeader(&cntl, send_topic_);
  //SetHttpHeader(&cntl, send_topic_tmp);
  //std::cout << "Pushtopic:" << send_topic_ << std::endl;

  cntl.request_attachment().append(request.SerializeAsString());

  //std::this_thread::sleep_for(std::chrono::milliseconds(500));
  channel_->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
  //std::cout << "cntl.request_attachment():" << cntl.request_attachment() << std::endl;
  /*if (cntl.Failed())
{
  int retry = 5;
  while(retry > 0)
  {
    cntl.Reset();
    SetHttpHeader(&cntl, send_topic_tmp);
    cntl.request_attachment().append(request.SerializeAsString());
    channel_->CallMethod(nullptr, &cntl, nullptr, nullptr, nullptr);
    std::cout << "Push retry times  ------>" << retry << std::endl;
    if(cntl.Failed())
    {
      retry--;
      std::this_thread::sleep_for(std::chrono::milliseconds( 1000 * (5 - retry)) );
      continue;
    }
    else
    {
      break;
    }
  }
}*/
  if (cntl.Failed()) {
    SPDLOG_ERROR("send, rpc failed={}, message={}", cntl.ErrorCode(),
                 cntl.ErrorText());
  }

  bb_ic::TransportOutbound new_response;
  new_response.ParseFromString(cntl.response_attachment().to_string());

  ic_pb::PushResponse response;
  TransResponse(&new_response, &response);
  if (response.header().error_code() != ic::ErrorCode::OK) {
    std::cout << "cntl.response_attachment().to_string():" << cntl.response_attachment().to_string() << std::endl;
    YACL_THROW("send, peer failed message={}", response.header().error_msg());
  }
}

void ChannelBrpcBlackBox::AsyncSendChunked(ic_pb::PushRequest& request,
                                           SendChunkedWindow& window,
                                           std::string chunk_info) {
  auto done =
      std::make_unique<util::BlackBoxPushDone>(*this, push_wait_ms_, window);
  done->cntl_.http_request().uri() = host_ + kUrlPrefix + "push";
  //std::string send_topic_tmp = send_topic_ + std::to_string(l_send_topic_++);
  //SetHttpHeader(&done->cntl_, send_topic_tmp);
  SetHttpHeader(&done->cntl_, send_topic_);
  done->cntl_.request_attachment().append(request.SerializeAsString());
  DealPushDone(std::move(done));

  //  std::cout << "AsyncSendChunked over!!!!!!!!!!" << std::endl;
  window.Wait();
}

}  // namespace yacl::link

