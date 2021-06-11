// Copyright (c) 2021 GitHub, Inc.
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
#include "net/url_request/redirect_util.h"
#include "services/network/public/cpp/features.h"
#include "shell/common/options_switches.h"

namespace electron {

InterceptingURLLoaderFactory::InterceptedRequest::FollowRedirectParams::
    FollowRedirectParams() = default;
InterceptingURLLoaderFactory::InterceptedRequest::FollowRedirectParams::
    ~FollowRedirectParams() = default;

InterceptingURLLoaderFactory::InterceptedRequest::InterceptedRequest(
    const InterceptHandlersMap& intercepted_handlers,
    mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target_factory_remote)
    : intercepted_handlers_(intercepted_handlers),
      request_id_(request_id),
      options_(options),
      request_(request),
      traffic_annotation_(traffic_annotation) {
  loader_receiver_.Bind(std::move(loader_receiver));
  loader_receiver_.Pause();
  loader_receiver_.set_disconnect_handler(base::BindOnce(
      &InterceptingURLLoaderFactory::InterceptedRequest::OnLoaderDisconnect,
      base::Unretained(this)));
  client_.Bind(std::move(client));
  client_.set_disconnect_handler(base::BindOnce(
      &InterceptingURLLoaderFactory::InterceptedRequest::OnClientDisconnect,
      base::Unretained(this)));
  target_factory_remote_.Bind(std::move(target_factory_remote));
  // TODO - Disconnect handler for target_factory_remote_?

  // TODO - Set disconnect handlers for the dynamically bound stuff - actually
  // this needs to be done on each binding

  MaybeRunInterceptHandler();
}

InterceptingURLLoaderFactory::InterceptedRequest::~InterceptedRequest() =
    default;

void InterceptingURLLoaderFactory::InterceptedRequest::FollowRedirect(
    const std::vector<std::string>& removed_headers,
    const net::HttpRequestHeaders& modified_headers,
    const net::HttpRequestHeaders& modified_cors_exempt_headers,
    const absl::optional<GURL>& new_url) {
  // Save the follow redirect params, if the scheme is intercepted then
  // the handler needs to be run before deciding whether to continue.
  // References code in
  // WebRequestProxyingURLLoaderFactory::InProgressRequest::FollowRedirect
  auto params = std::make_unique<FollowRedirectParams>();
  params->removed_headers = removed_headers;
  params->modified_headers = modified_headers;
  params->modified_cors_exempt_headers = modified_cors_exempt_headers;
  params->new_url = new_url;
  pending_follow_redirect_params_ = std::move(params);

  // Update |request_| with info from the redirect, so that it's accurate
  // when provided to a new loader if not following redirect with original
  // The following references code in WorkerScriptLoader::FollowRedirect
  DCHECK(redirect_info_);

  bool should_clear_upload = false;
  net::RedirectUtil::UpdateHttpRequest(
      request_.url, request_.method, *redirect_info_, removed_headers,
      modified_headers, &request_.headers, &should_clear_upload);
  request_.cors_exempt_headers.MergeFrom(modified_cors_exempt_headers);
  for (const std::string& name : removed_headers)
    request_.cors_exempt_headers.RemoveHeader(name);

  if (should_clear_upload)
    request_.request_body = nullptr;

  request_.url = redirect_info_->new_url;
  request_.method = redirect_info_->new_method;
  request_.site_for_cookies = redirect_info_->new_site_for_cookies;
  request_.referrer = GURL(redirect_info_->new_referrer);
  request_.referrer_policy = redirect_info_->new_referrer_policy;

  redirect_info_.reset();

  // This scheme may or may not be intercepted currently
  MaybeRunInterceptHandler();
}

void InterceptingURLLoaderFactory::InterceptedRequest::SetPriority(
    net::RequestPriority priority,
    int32_t intra_priority_value) {
  DCHECK(target_loader_.is_bound());
  target_loader_->SetPriority(priority, intra_priority_value);
}

void InterceptingURLLoaderFactory::InterceptedRequest::
    PauseReadingBodyFromNet() {
  DCHECK(target_loader_.is_bound());
  target_loader_->PauseReadingBodyFromNet();
}

void InterceptingURLLoaderFactory::InterceptedRequest::
    ResumeReadingBodyFromNet() {
  DCHECK(target_loader_.is_bound());
  target_loader_->ResumeReadingBodyFromNet();
}

void InterceptingURLLoaderFactory::InterceptedRequest::OnReceiveEarlyHints(
    network::mojom::EarlyHintsPtr early_hints) {
  DCHECK(client_.is_bound());
  client_->OnReceiveEarlyHints(std::move(early_hints));
}

void InterceptingURLLoaderFactory::InterceptedRequest::OnReceiveResponse(
    network::mojom::URLResponseHeadPtr head) {
  DCHECK(client_.is_bound());
  client_->OnReceiveResponse(std::move(head));
}

void InterceptingURLLoaderFactory::InterceptedRequest::OnReceiveRedirect(
    const net::RedirectInfo& redirect_info,
    network::mojom::URLResponseHeadPtr head) {
  // Save the redirect info so the request can be updated in |FollowRedirect|
  redirect_info_ = redirect_info;
  DCHECK(client_.is_bound());
  client_->OnReceiveRedirect(redirect_info, std::move(head));
}

void InterceptingURLLoaderFactory::InterceptedRequest::OnUploadProgress(
    int64_t current_position,
    int64_t total_size,
    OnUploadProgressCallback callback) {
  DCHECK(client_.is_bound());
  client_->OnUploadProgress(current_position, total_size, std::move(callback));
}

void InterceptingURLLoaderFactory::InterceptedRequest::OnReceiveCachedMetadata(
    mojo_base::BigBuffer data) {
  DCHECK(client_.is_bound());
  client_->OnReceiveCachedMetadata(std::move(data));
}

void InterceptingURLLoaderFactory::InterceptedRequest::OnTransferSizeUpdated(
    int32_t transfer_size_diff) {
  DCHECK(client_.is_bound());
  client_->OnTransferSizeUpdated(transfer_size_diff);
}

void InterceptingURLLoaderFactory::InterceptedRequest::
    OnStartLoadingResponseBody(mojo::ScopedDataPipeConsumerHandle body) {
  DCHECK(client_.is_bound());
  client_->OnStartLoadingResponseBody(std::move(body));
}

void InterceptingURLLoaderFactory::InterceptedRequest::OnComplete(
    const network::URLLoaderCompletionStatus& status) {
  DCHECK(client_.is_bound());
  client_->OnComplete(status);
}

void InterceptingURLLoaderFactory::InterceptedRequest::
    MaybeRunInterceptHandler() {
  // Check if user has intercepted this scheme.
  auto it = intercepted_handlers_.find(request_.url.scheme());
  if (it != intercepted_handlers_.end()) {
    // Reset callback fired since this is a fresh invocation of the handler
    callbackFired_ = false;

    // <scheme, <type, handler>>
    it->second.second.Run(
        request_,
        base::BindOnce(
            &InterceptingURLLoaderFactory::InterceptedRequest::SendResponse,
            base::Unretained(this), it->second.first),
        base::BindOnce(&InterceptingURLLoaderFactory::InterceptedRequest::
                           ContinueRequestFromHandler,
                       base::Unretained(this)));
  } else {
    // Not intercepted, continue on to the target factory
    ContinueRequest();
  }
}

void InterceptingURLLoaderFactory::InterceptedRequest::ContinueRequest() {
  if (pending_follow_redirect_params_ && target_loader_.is_bound()) {
    target_loader_->FollowRedirect(
        pending_follow_redirect_params_->removed_headers,
        pending_follow_redirect_params_->modified_headers,
        pending_follow_redirect_params_->modified_cors_exempt_headers,
        pending_follow_redirect_params_->new_url);
  } else {
    // Reset these if they're bound
    target_loader_.reset();
    client_receiver_.reset();

    // Continue on to the target factory
    DCHECK(target_factory_remote_.is_bound());
    target_factory_remote_->CreateLoaderAndStart(
        target_loader_.BindNewPipeAndPassReceiver(), request_id_, options_,
        request_, client_receiver_.BindNewPipeAndPassRemote(),
        traffic_annotation_);
    loader_receiver_.Resume();
  }

  pending_follow_redirect_params_.reset();
}

void InterceptingURLLoaderFactory::InterceptedRequest::SendResponse(
    ProtocolType type,
    gin::Arguments* args) {
  if (callbackFired_) {
    gin_helper::ErrorThrower(args->isolate())
        .ThrowError("Intercepted request was already continued");
    return;
  }

  callbackFired_ = true;

  // Reset these if they're bound
  target_loader_.reset();
  client_receiver_.reset();

  // ElectronURLLoaderFactory::StartLoading is used for both intercepted and
  // registered protocols, and on redirects it needs a factory to use to
  // create a loader for the new request, so provide it the target factory.
  mojo::PendingRemote<network::mojom::URLLoaderFactory>
      target_factory_pending_remote;
  target_factory_remote_->Clone(
      target_factory_pending_remote.InitWithNewPipeAndPassReceiver());

  ElectronURLLoaderFactory::StartLoading(
      target_loader_.BindNewPipeAndPassReceiver(), request_id_, options_,
      request_, client_receiver_.BindNewPipeAndPassRemote(),
      traffic_annotation_, std::move(target_factory_pending_remote), type,
      args);
  loader_receiver_.Resume();
}

void InterceptingURLLoaderFactory::InterceptedRequest::
    ContinueRequestFromHandler(gin::Arguments* args) {
  if (callbackFired_) {
    gin_helper::ErrorThrower(args->isolate())
        .ThrowError("Response already sent for intercepted request");
    return;
  }

  callbackFired_ = true;
  ContinueRequest();
}

void InterceptingURLLoaderFactory::InterceptedRequest::OnClientDisconnect() {
  client_.reset();
  client_receiver_.reset();

  MaybeDeleteThis();
}

void InterceptingURLLoaderFactory::InterceptedRequest::OnLoaderDisconnect() {
  target_loader_.reset();
  loader_receiver_.reset();

  MaybeDeleteThis();
}

void InterceptingURLLoaderFactory::InterceptedRequest::MaybeDeleteThis() {
  // We can delete this factory when it is impossible for a new request to
  // arrive in the future.
  if (client_receiver_.is_bound() || loader_receiver_.is_bound()) {
    return;
  }

  delete this;
}

InterceptingURLLoaderFactory::InterceptingURLLoaderFactory(
    const InterceptHandlersMap& intercepted_handlers,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> loader_receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target_factory_remote)
    : network::SelfDeletingURLLoaderFactory(std::move(loader_receiver)),
      intercepted_handlers_(intercepted_handlers) {
  target_factory_remote_.Bind(std::move(target_factory_remote));
  target_factory_remote_.set_disconnect_handler(
      base::BindOnce(&InterceptingURLLoaderFactory::OnTargetFactoryDisconnect,
                     base::Unretained(this)));

  ignore_connections_limit_domains_ = base::SplitString(
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kIgnoreConnectionsLimit),
      ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
}

InterceptingURLLoaderFactory::~InterceptingURLLoaderFactory() = default;

bool InterceptingURLLoaderFactory::ShouldIgnoreConnectionsLimit(
    const network::ResourceRequest& request) const {
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

  mojo::PendingRemote<network::mojom::URLLoaderFactory> target_factory_remote;
  target_factory_remote_->Clone(
      target_factory_remote.InitWithNewPipeAndPassReceiver());

  // Lifetime is tied to its receivers
  new InterceptedRequest(intercepted_handlers_, std::move(loader), request_id,
                         options, request, std::move(client),
                         traffic_annotation, std::move(target_factory_remote));
}

void InterceptingURLLoaderFactory::OnTargetFactoryDisconnect() {
  // Without a bound |target_factory_remote_| this can't intercept a request
  DisconnectReceiversAndDestroy();
}

}  // namespace electron
