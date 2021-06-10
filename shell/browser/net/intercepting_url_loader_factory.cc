// Copyright (c) 2019 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/net/intercepting_url_loader_factory.h"

#include <utility>

#include "base/command_line.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "content/public/browser/browser_context.h"
#include "net/base/completion_repeating_callback.h"
#include "net/base/load_flags.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/features.h"
#include "shell/common/options_switches.h"

namespace electron {

InterceptingURLLoaderFactory::InterceptedRequest::InterceptedRequest(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
    ProtocolType type,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> interceptor_factory,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target_factory)
    : loader_(std::move(loader)),
      request_id_(request_id),
      options_(options),
      request_(request),
      client_(std::move(client)),
      traffic_annotation_(traffic_annotation),
      type_(type),
      interceptor_factory_(std::move(interceptor_factory)),
      target_factory_(std::move(target_factory)) {}

InterceptingURLLoaderFactory::InterceptedRequest::~InterceptedRequest() =
    default;

void InterceptingURLLoaderFactory::InterceptedRequest::SendResponse(
    gin::Arguments* args) {
  if (callbackFired_) {
    gin_helper::ErrorThrower(args->isolate())
        .ThrowError("Intercepted request was already continued");
    return;
  }

  callbackFired_ = true;

  ElectronURLLoaderFactory::StartLoading(std::move(loader_), request_id_,
                                         options_, request_, std::move(client_),
                                         traffic_annotation_, type_, args);
}

void InterceptingURLLoaderFactory::InterceptedRequest::ContinueRequest(
    gin::Arguments* args) {
  if (callbackFired_) {
    gin_helper::ErrorThrower(args->isolate())
        .ThrowError("Response already sent for intercepted request");
    return;
  }

  callbackFired_ = true;

  // Continue on to the target factory
  mojo::Remote<network::mojom::URLLoaderFactory> target_factory_remote(
      std::move(target_factory_));
  target_factory_remote->CreateLoaderAndStart(
      std::move(loader_), request_id_, options_, request_, std::move(client_),
      traffic_annotation_);
}

InterceptingURLLoaderFactory::InterceptingURLLoaderFactory(
    const InterceptHandlersMap& intercepted_handlers,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> loader_receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target_factory_remote)
    : intercepted_handlers_(intercepted_handlers) {
  target_factory_remote_.Bind(std::move(target_factory_remote));
  target_factory_remote_.set_disconnect_handler(
      base::BindOnce(&InterceptingURLLoaderFactory::OnProxyingFactoryError,
                     base::Unretained(this)));
  interceptor_receivers_.Add(this, std::move(loader_receiver));
  interceptor_receivers_.set_disconnect_handler(base::BindRepeating(
      &InterceptingURLLoaderFactory::OnInterceptorBindingError,
      base::Unretained(this)));

  ignore_connections_limit_domains_ = base::SplitString(
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kIgnoreConnectionsLimit),
      ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
}

InterceptingURLLoaderFactory::~InterceptingURLLoaderFactory() = default;

bool InterceptingURLLoaderFactory::ShouldIgnoreConnectionsLimit(
    const network::ResourceRequest& request) {
  for (const auto& domain : ignore_connections_limit_domains_) {
    if (request.url.DomainIs(domain)) {
      return true;
    }
  }
  return false;
}

void InterceptingURLLoaderFactory::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& original_request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  // Take a copy so we can mutate the request.
  network::ResourceRequest request = original_request;

  if (ShouldIgnoreConnectionsLimit(request)) {
    request.priority = net::RequestPriority::MAXIMUM_PRIORITY;
    request.load_flags |= net::LOAD_IGNORE_LIMITS;
  }

  // Check if user has intercepted this scheme.
  auto it = intercepted_handlers_.find(request.url.scheme());
  if (it != intercepted_handlers_.end()) {
    mojo::PendingRemote<network::mojom::URLLoaderFactory> this_remote;
    this->Clone(this_remote.InitWithNewPipeAndPassReceiver());

    mojo::PendingRemote<network::mojom::URLLoaderFactory> target_factory_remote;
    target_factory_remote_->Clone(
        target_factory_remote.InitWithNewPipeAndPassReceiver());

    scoped_refptr<InterceptedRequest> intercepting_loader =
        base::MakeRefCounted<InterceptedRequest>(
            std::move(loader), request_id, options, request, std::move(client),
            traffic_annotation, it->second.first, std::move(this_remote),
            std::move(target_factory_remote));

    // <scheme, <type, handler>>
    it->second.second.Run(
        request,
        base::BindOnce(&InterceptedRequest::SendResponse, intercepting_loader),
        base::BindOnce(&InterceptedRequest::ContinueRequest,
                       intercepting_loader));
  } else {
    // Not intercepted, continue on to the target factory
    target_factory_remote_->CreateLoaderAndStart(
        std::move(loader), request_id, options, request, std::move(client),
        traffic_annotation);
  }
}

void InterceptingURLLoaderFactory::Clone(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> loader_receiver) {
  interceptor_receivers_.Add(this, std::move(loader_receiver));
}

void InterceptingURLLoaderFactory::OnProxyingFactoryError() {
  target_factory_remote_.reset();
  interceptor_receivers_.Clear();

  MaybeDeleteThis();
}

void InterceptingURLLoaderFactory::OnInterceptorBindingError() {
  if (interceptor_receivers_.empty())
    target_factory_remote_.reset();

  MaybeDeleteThis();
}

void InterceptingURLLoaderFactory::MaybeDeleteThis() {
  if (target_factory_remote_.is_bound() || !interceptor_receivers_.empty())
    return;

  delete this;
}

}  // namespace electron
