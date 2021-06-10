// Copyright (c) 2021 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef SHELL_BROWSER_NET_INTERCEPTING_URL_LOADER_FACTORY_H_
#define SHELL_BROWSER_NET_INTERCEPTING_URL_LOADER_FACTORY_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "content/public/browser/content_browser_client.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "shell/browser/net/electron_url_loader_factory.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace electron {

class InterceptingURLLoaderFactory : public network::mojom::URLLoaderFactory {
 public:
  class InterceptedRequest : public network::mojom::URLLoader,
                             public network::mojom::URLLoaderClient {
   public:
    InterceptedRequest(
        const InterceptHandlersMap& intercepted_handlers,
        mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
        int32_t request_id,
        uint32_t options,
        const network::ResourceRequest& request,
        mojo::PendingRemote<network::mojom::URLLoaderClient> client,
        const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
        mojo::PendingRemote<network::mojom::URLLoaderFactory>
            target_factory_remote);

    // network::mojom::URLLoader:
    void FollowRedirect(
        const std::vector<std::string>& removed_headers,
        const net::HttpRequestHeaders& modified_headers,
        const net::HttpRequestHeaders& modified_cors_exempt_headers,
        const absl::optional<GURL>& new_url) override;
    void SetPriority(net::RequestPriority priority,
                     int32_t intra_priority_value) override;
    void PauseReadingBodyFromNet() override;
    void ResumeReadingBodyFromNet() override;

    // network::mojom::URLLoaderClient:
    void OnReceiveEarlyHints(
        network::mojom::EarlyHintsPtr early_hints) override;
    void OnReceiveResponse(network::mojom::URLResponseHeadPtr head) override;
    void OnReceiveRedirect(const net::RedirectInfo& redirect_info,
                           network::mojom::URLResponseHeadPtr head) override;
    void OnUploadProgress(int64_t current_position,
                          int64_t total_size,
                          OnUploadProgressCallback callback) override;
    void OnReceiveCachedMetadata(mojo_base::BigBuffer data) override;
    void OnTransferSizeUpdated(int32_t transfer_size_diff) override;
    void OnStartLoadingResponseBody(
        mojo::ScopedDataPipeConsumerHandle body) override;
    void OnComplete(const network::URLLoaderCompletionStatus& status) override;

    void MaybeRunInterceptHandler();
    void ContinueRequest();

    void SendResponse(ProtocolType type, gin::Arguments* args);
    void ContinueRequestFromHandler(gin::Arguments* args);

    void OnClientDisconnect();
    void OnLoaderDisconnect();
    void MaybeDeleteThis();

   private:
    ~InterceptedRequest() override;
    friend class base::RefCountedThreadSafe<InterceptedRequest>;

    const InterceptHandlersMap& intercepted_handlers_;

    mojo::Receiver<network::mojom::URLLoader> loader_receiver_{this};
    mojo::Receiver<network::mojom::URLLoaderClient> client_receiver_{this};

    int32_t request_id_;
    uint32_t options_;
    network::ResourceRequest request_;
    net::MutableNetworkTrafficAnnotationTag traffic_annotation_;

    mojo::Remote<network::mojom::URLLoaderClient> client_;
    mojo::Remote<network::mojom::URLLoader> target_loader_;
    mojo::Remote<network::mojom::URLLoaderFactory> target_factory_remote_;

    bool callbackFired_ = false;

    absl::optional<net::RedirectInfo> redirect_info_;
    // This stores the parameters to FollowRedirect that came from the client,
    // so that they can be sent to |target_loader_| if request is continued.
    struct FollowRedirectParams {
      FollowRedirectParams();
      ~FollowRedirectParams();
      std::vector<std::string> removed_headers;
      net::HttpRequestHeaders modified_headers;
      net::HttpRequestHeaders modified_cors_exempt_headers;
      absl::optional<GURL> new_url;

      DISALLOW_COPY_AND_ASSIGN(FollowRedirectParams);
    };
    std::unique_ptr<FollowRedirectParams> pending_follow_redirect_params_;

    DISALLOW_COPY_AND_ASSIGN(InterceptedRequest);
  };

  InterceptingURLLoaderFactory(
      const InterceptHandlersMap& intercepted_handlers,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> loader_receiver,
      mojo::PendingRemote<network::mojom::URLLoaderFactory>
          target_factory_remote);
  ~InterceptingURLLoaderFactory() override;

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override;
  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory>
                 loader_receiver) override;

 private:
  void OnProxyingFactoryError();
  void OnInterceptorBindingError();
  void MaybeDeleteThis();

  bool ShouldIgnoreConnectionsLimit(const network::ResourceRequest& request);

  // This is passed from api::Protocol.
  //
  // The Protocol instance lives through the lifetime of BrowserContext,
  // which is guaranteed to cover the lifetime of URLLoaderFactory, so the
  // reference is guaranteed to be valid.
  //
  // In this way we can avoid using code from api namespace in this file.
  const InterceptHandlersMap& intercepted_handlers_;

  mojo::ReceiverSet<network::mojom::URLLoaderFactory> interceptor_receivers_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_factory_remote_;

  std::vector<std::string> ignore_connections_limit_domains_;

  DISALLOW_COPY_AND_ASSIGN(InterceptingURLLoaderFactory);
};

}  // namespace electron

#endif  // SHELL_BROWSER_NET_INTERCEPTING_URL_LOADER_FACTORY_H_
