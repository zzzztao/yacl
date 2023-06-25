// Copyright 2019 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "yacl/link/transport/channel_brpc_base.h"

#include <exception>

#include "absl/strings/match.h"
#include "spdlog/spdlog.h"

#include "yacl/base/exception.h"

#include "interconnection/link/transport.pb.h"

namespace brpc::policy {

DECLARE_int32(h2_client_stream_window_size);

}

namespace yacl::link {

namespace ic = org::interconnection;
namespace ic_pb = org::interconnection::link;

void ChannelBrpcBase::SendImpl(const std::string& key,
                               ByteContainerView value) {
  SendImpl(key, value, 0);
}

void ChannelBrpcBase::SendImpl(const std::string& key, ByteContainerView value,
                               uint32_t timeout) {
  if (value.size() > options_.http_max_payload_size) {
    SendChunked(key, value);
    return;
  }

  ic_pb::PushRequest request;
  {
    request.set_sender_rank(self_rank_);
    request.set_key(key);
    request.set_value(value.data(), value.size());
    request.set_trans_type(ic_pb::TransType::MONO);
  }

  PushRequest(request, timeout);
}

void ChannelBrpcBase::SendChunked(const std::string& key,
                                  ByteContainerView value) {
  const size_t bytes_per_chunk = options_.http_max_payload_size;
  const size_t num_bytes = value.size();
  const size_t num_chunks = (num_bytes + bytes_per_chunk - 1) / bytes_per_chunk;

  constexpr uint32_t kParallelSize = 8;
  SendChunkedWindow window(kParallelSize);

  for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
    const size_t chunk_offset = chunk_idx * bytes_per_chunk;

    ic_pb::PushRequest request;
    {
      request.set_sender_rank(self_rank_);
      request.set_key(key);
      request.set_value(value.data() + chunk_offset,
                        std::min(bytes_per_chunk, value.size() - chunk_offset));
      request.set_trans_type(ic_pb::TransType::CHUNKED);
      request.mutable_chunk_info()->set_chunk_offset(chunk_offset);
      request.mutable_chunk_info()->set_message_length(num_bytes);
    }

    AsyncSendChunked(request, window,
                     fmt::format("send key={} (chunked {} out of {})", key,
                                 chunk_idx, num_chunks));
  }
  window.Finished();
}

void ChannelBrpcBase::OnRequest(const ic_pb::PushRequest* request,
                                ic_pb::PushResponse* response) {
  auto trans_type = request->trans_type();

  response->mutable_header()->set_error_code(ic::ErrorCode::OK);
  response->mutable_header()->set_error_msg("");
  // dispatch the message
  if (trans_type == ic_pb::TransType::MONO) {
    OnMessage(request->key(), request->value());
  } else if (trans_type == ic_pb::TransType::CHUNKED) {
    const auto& chunk = request->chunk_info();
    OnChunkedMessage(request->key(), request->value(), chunk.chunk_offset(),
                     chunk.message_length());
  } else {
    response->mutable_header()->set_error_code(ic::ErrorCode::INVALID_REQUEST);
    response->mutable_header()->set_error_msg(
        fmt::format("unrecongnized trans type={}, from rank={}", trans_type,
                    request->sender_rank()));
  }
}

auto ChannelBrpcBase::MakeOptions(
    Options& default_opt, uint32_t http_timeout_ms,
    uint32_t http_max_payload_size, const std::string& brpc_channel_protocol,
    const std::string& brpc_channel_connection_type) -> Options {
  auto opts = default_opt;
  if (http_timeout_ms != 0) {
    opts.http_timeout_ms = http_timeout_ms;
  }
  if (http_max_payload_size != 0) {
    opts.http_max_payload_size = http_max_payload_size;
  }
  if (!brpc_channel_protocol.empty()) {
    opts.channel_protocol = brpc_channel_protocol;
  }

  if (absl::StartsWith(opts.channel_protocol, "h2")) {
    YACL_ENFORCE(opts.http_max_payload_size > 4096,
                 "http_max_payload_size is too small");
    YACL_ENFORCE(
        opts.http_max_payload_size < std::numeric_limits<int32_t>::max(),
        "http_max_payload_size is too large");
    // if use h2 protocol (h2 or h2:grpc), need to change h2 window size too,
    // use http_max_payload_size as h2's window size, then reserve 4kb buffer
    // for protobuf header
    brpc::policy::FLAGS_h2_client_stream_window_size =
        static_cast<int32_t>(opts.http_max_payload_size);
    opts.http_max_payload_size -= 4096;
  }

  if (!brpc_channel_connection_type.empty()) {
    opts.channel_connection_type = brpc_channel_connection_type;
  }

  return opts;
}

}  // namespace yacl::link
