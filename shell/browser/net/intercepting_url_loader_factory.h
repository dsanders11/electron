// Copyright (c) 2020 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef SHELL_BROWSER_NET_INTERCEPTING_URL_LOADER_FACTORY_H_
#define SHELL_BROWSER_NET_INTERCEPTING_URL_LOADER_FACTORY_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "content/public/browser/content_browser_client.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "shell/browser/net/electron_url_loader_factory.h"

namespace electron {

class InterceptingURLLoaderFactory : public network::mojom::URLLoaderFactory {
 public:
  class InterceptedRequest
      : public base::RefCountedThreadSafe<InterceptedRequest> {
   public:
    InterceptedRequest(
        mojo::PendingReceiver<network::mojom::URLLoader> loader,
        int32_t request_id,
        uint32_t options,
        const network::ResourceRequest& request,
        mojo::PendingRemote<network::mojom::URLLoaderClient> client,
        const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
        ProtocolType type,
        mojo::PendingRemote<network::mojom::URLLoaderFactory>
            interceptor_factory,
        mojo::PendingRemote<network::mojom::URLLoaderFactory> target_factory);

    void SendResponse(gin::Arguments* args);
    void ContinueRequest(gin::Arguments* args);

   private:
    ~InterceptedRequest();
    friend class base::RefCountedThreadSafe<InterceptedRequest>;

    mojo::PendingReceiver<network::mojom::URLLoader> loader_;
    int32_t request_id_;
    uint32_t options_;
    network::ResourceRequest request_;
    mojo::PendingRemote<network::mojom::URLLoaderClient> client_;
    net::MutableNetworkTrafficAnnotationTag traffic_annotation_;
    ProtocolType type_;
    mojo::PendingRemote<network::mojom::URLLoaderFactory> interceptor_factory_;
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target_factory_;

    bool callbackFired_ = false;

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
