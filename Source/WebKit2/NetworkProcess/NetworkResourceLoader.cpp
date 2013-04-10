/*
 * Copyright (C) 2012 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "NetworkResourceLoader.h"

#if ENABLE(NETWORK_PROCESS)

#include "AuthenticationManager.h"
#include "DataReference.h"
#include "Logging.h"
#include "NetworkConnectionToWebProcess.h"
#include "NetworkProcess.h"
#include "NetworkProcessConnectionMessages.h"
#include "NetworkResourceLoadParameters.h"
#include "PlatformCertificateInfo.h"
#include "RemoteNetworkingContext.h"
#include "ShareableResource.h"
#include "SharedMemory.h"
#include "WebCoreArgumentCoders.h"
#include "WebResourceLoaderMessages.h"
#include <WebCore/NotImplemented.h>
#include <WebCore/ResourceBuffer.h>
#include <WebCore/ResourceHandle.h>
#include <wtf/CurrentTime.h>
#include <wtf/MainThread.h>

using namespace WebCore;

namespace WebKit {

NetworkResourceLoader::NetworkResourceLoader(const NetworkResourceLoadParameters& loadParameters, NetworkConnectionToWebProcess* connection)
    : SchedulableLoader(loadParameters, connection)
    , m_bytesReceived(0)
{
    ASSERT(isMainThread());
}

NetworkResourceLoader::~NetworkResourceLoader()
{
    ASSERT(isMainThread());
    ASSERT(!m_handle);
}

CoreIPC::Connection* NetworkResourceLoader::connection() const
{
    return connectionToWebProcess()->connection();
}

uint64_t NetworkResourceLoader::destinationID() const
{
    return identifier();
}

void NetworkResourceLoader::start()
{
    ASSERT(isMainThread());

    // Explicit ref() balanced by a deref() in NetworkResourceLoader::resourceHandleStopped()
    ref();
    
    // FIXME (NetworkProcess): Create RemoteNetworkingContext with actual settings.
    m_networkingContext = RemoteNetworkingContext::create(false, false, inPrivateBrowsingMode(), shouldClearReferrerOnHTTPSToHTTPRedirect());

    consumeSandboxExtensions();

    // FIXME (NetworkProcess): Pass an actual value for defersLoading
    m_handle = ResourceHandle::create(m_networkingContext.get(), request(), this, false /* defersLoading */, contentSniffingPolicy() == SniffContent);
}

static bool performCleanupsCalled = false;

static Mutex& requestsToCleanupMutex()
{
    DEFINE_STATIC_LOCAL(Mutex, mutex, ());
    return mutex;
}

static HashSet<NetworkResourceLoader*>& requestsToCleanup()
{
    DEFINE_STATIC_LOCAL(HashSet<NetworkResourceLoader*>, requests, ());
    return requests;
}

void NetworkResourceLoader::scheduleCleanupOnMainThread()
{
    MutexLocker locker(requestsToCleanupMutex());

    requestsToCleanup().add(this);
    if (!performCleanupsCalled) {
        performCleanupsCalled = true;
        callOnMainThread(NetworkResourceLoader::performCleanups, 0);
    }
}

void NetworkResourceLoader::performCleanups(void*)
{
    ASSERT(performCleanupsCalled);

    Vector<NetworkResourceLoader*> requests;
    {
        MutexLocker locker(requestsToCleanupMutex());
        copyToVector(requestsToCleanup(), requests);
        requestsToCleanup().clear();
        performCleanupsCalled = false;
    }
    
    for (size_t i = 0; i < requests.size(); ++i)
        requests[i]->cleanup();
}

void NetworkResourceLoader::cleanup()
{
    ASSERT(isMainThread());

    if (FormData* formData = request().httpBody())
        formData->removeGeneratedFilesIfNeeded();

    // Tell the scheduler about this finished loader soon so it can start more network requests.
    NetworkProcess::shared().networkResourceLoadScheduler().scheduleRemoveLoader(this);

    if (m_handle) {
        // Explicit deref() balanced by a ref() in NetworkResourceLoader::start()
        // This might cause the NetworkResourceLoader to be destroyed and therefore we do it last.
        m_handle = 0;
        deref();
    }
}

void NetworkResourceLoader::connectionToWebProcessDidClose()
{
    ASSERT(isMainThread());

    // If this loader already has a resource handle then it is already in progress on a background thread.
    // On that thread it will notice that its connection to its WebProcess has been invalidated and it will "gracefully" abort.
    if (m_handle)
        return;

#if !ASSERT_DISABLED
    // Since there's no handle, this loader should never have been started, and therefore it should never be in the
    // set of loaders to cleanup on the main thread.
    // Let's make sure that's true.
    {
        MutexLocker locker(requestsToCleanupMutex());
        ASSERT(!requestsToCleanup().contains(this));
    }
#endif

    cleanup();
}

template<typename U> bool NetworkResourceLoader::sendAbortingOnFailure(const U& message)
{
    bool result = send(message);
    if (!result)
        abortInProgressLoad();
    return result;
}

template<typename U> bool NetworkResourceLoader::sendSyncAbortingOnFailure(const U& message, const typename U::Reply& reply)
{
    bool result = sendSync(message, reply);
    if (!result)
        abortInProgressLoad();
    return result;
}

void NetworkResourceLoader::abortInProgressLoad()
{
    ASSERT(m_handle);
    ASSERT(isMainThread());

    m_handle->cancel();

    scheduleCleanupOnMainThread();
}

void NetworkResourceLoader::didReceiveResponse(ResourceHandle*, const ResourceResponse& response)
{
    // FIXME (NetworkProcess): Cache the response.
    if (FormData* formData = request().httpBody())
        formData->removeGeneratedFilesIfNeeded();

    sendAbortingOnFailure(Messages::WebResourceLoader::DidReceiveResponseWithCertificateInfo(response, PlatformCertificateInfo(response)));
}

void NetworkResourceLoader::didReceiveData(ResourceHandle*, const char* data, int length, int encodedDataLength)
{
    // The NetworkProcess should never get a didReceiveData callback.
    // We should always be using didReceiveBuffer.
    ASSERT_NOT_REACHED();
}

void NetworkResourceLoader::didReceiveBuffer(WebCore::ResourceHandle*, PassRefPtr<WebCore::SharedBuffer> buffer, int encodedDataLength)
{
    // FIXME (NetworkProcess): For the memory cache we'll also need to cache the response data here.
    // Such buffering will need to be thread safe, as this callback is happening on a background thread.
    
    m_bytesReceived += buffer->size();
    
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 1090
    ShareableResource::Handle handle;
    tryGetShareableHandleFromSharedBuffer(handle, buffer.get());
    if (!handle.isNull()) {
        // Since we're delivering this resource by ourselves all at once, we'll abort the resource handle since we don't need anymore callbacks from ResourceHandle.
        abortInProgressLoad();
        send(Messages::WebResourceLoader::DidReceiveResource(handle, currentTime()));
        return;
    }
#endif // __MAC_OS_X_VERSION_MIN_REQUIRED >= 1090

    CoreIPC::DataReference dataReference(reinterpret_cast<const uint8_t*>(buffer->data()), buffer->size());
    sendAbortingOnFailure(Messages::WebResourceLoader::DidReceiveData(dataReference, encodedDataLength));
}

void NetworkResourceLoader::didFinishLoading(ResourceHandle*, double finishTime)
{
    // FIXME (NetworkProcess): For the memory cache we'll need to update the finished status of the cached resource here.
    // Such bookkeeping will need to be thread safe, as this callback is happening on a background thread.
    invalidateSandboxExtensions();
    send(Messages::WebResourceLoader::DidFinishResourceLoad(finishTime));
    
    scheduleCleanupOnMainThread();
}

void NetworkResourceLoader::didFail(ResourceHandle*, const ResourceError& error)
{
    // FIXME (NetworkProcess): For the memory cache we'll need to update the finished status of the cached resource here.
    // Such bookkeeping will need to be thread safe, as this callback is happening on a background thread.
    invalidateSandboxExtensions();
    send(Messages::WebResourceLoader::DidFailResourceLoad(error));
    scheduleCleanupOnMainThread();
}

void NetworkResourceLoader::willSendRequestAsync(ResourceHandle*, const ResourceRequest& request, const ResourceResponse& redirectResponse)
{
    // We only expect to get the willSendRequest callback from ResourceHandle as the result of a redirect.
    ASSERT(!redirectResponse.isNull());
    ASSERT(isMainThread());

    m_suggestedRequestForWillSendRequest = request;

    // This message is DispatchMessageEvenWhenWaitingForSyncReply to avoid a situation where the NetworkProcess is deadlocked waiting for 6 connections
    // to complete while the WebProcess is waiting for a 7th to complete.
    connection()->send(Messages::WebResourceLoader::WillSendRequest(request, redirectResponse), destinationID(), CoreIPC::DispatchMessageEvenWhenWaitingForSyncReply);
}

void NetworkResourceLoader::continueWillSendRequest(const ResourceRequest& newRequest)
{
    m_suggestedRequestForWillSendRequest.updateFromDelegatePreservingOldHTTPBody(newRequest.nsURLRequest(DoNotUpdateHTTPBody));

    RunLoop::main()->dispatch(bind(&NetworkResourceLoadScheduler::receivedRedirect, &NetworkProcess::shared().networkResourceLoadScheduler(), this, m_suggestedRequestForWillSendRequest.url()));
    m_handle->continueWillSendRequest(m_suggestedRequestForWillSendRequest);

    m_suggestedRequestForWillSendRequest = ResourceRequest();
}

// FIXME (NetworkProcess): Many of the following ResourceHandleClient methods definitely need implementations. A few will not.
// Once we know what they are they can be removed.

void NetworkResourceLoader::didSendData(ResourceHandle*, unsigned long long /*bytesSent*/, unsigned long long /*totalBytesToBeSent*/)
{
    notImplemented();
}

void NetworkResourceLoader::didReceiveCachedMetadata(ResourceHandle*, const char*, int)
{
    notImplemented();
}

void NetworkResourceLoader::wasBlocked(ResourceHandle*)
{
    notImplemented();
}

void NetworkResourceLoader::cannotShowURL(ResourceHandle*)
{
    notImplemented();
}

bool NetworkResourceLoader::shouldUseCredentialStorage(WebCore::ResourceHandle*)
{
    // When the WebProcess is handling loading a client is consulted each time this shouldUseCredentialStorage question is asked.
    // In NetworkProcess mode we ask the WebProcess client up front once and then reuse the cached answer.

    // We still need this sync version, because ResourceHandle itself uses it internally, even when the delegate uses an async one.

    return allowStoredCredentials() == AllowStoredCredentials;
}

void NetworkResourceLoader::shouldUseCredentialStorageAsync(ResourceHandle* handle)
{
    handle->continueShouldUseCredentialStorage(shouldUseCredentialStorage(handle));
}

void NetworkResourceLoader::didReceiveAuthenticationChallenge(ResourceHandle*, const AuthenticationChallenge& challenge)
{
    NetworkProcess::shared().authenticationManager().didReceiveAuthenticationChallenge(webPageID(), webFrameID(), challenge);
}

void NetworkResourceLoader::didCancelAuthenticationChallenge(ResourceHandle*, const AuthenticationChallenge& challenge)
{
    // FIXME (NetworkProcess): Tell AuthenticationManager.
}

#if USE(PROTECTION_SPACE_AUTH_CALLBACK)
void NetworkResourceLoader::canAuthenticateAgainstProtectionSpaceAsync(ResourceHandle*, const ProtectionSpace& protectionSpace)
{
    ASSERT(isMainThread());

    // This message is DispatchMessageEvenWhenWaitingForSyncReply to avoid a situation where the NetworkProcess is deadlocked
    // waiting for 6 connections to complete while the WebProcess is waiting for a 7th to complete.
    connection()->send(Messages::WebResourceLoader::CanAuthenticateAgainstProtectionSpace(protectionSpace), destinationID(), CoreIPC::DispatchMessageEvenWhenWaitingForSyncReply);
}

void NetworkResourceLoader::continueCanAuthenticateAgainstProtectionSpace(bool result)
{
    m_handle->continueCanAuthenticateAgainstProtectionSpace(result);
}

#endif

#if USE(NETWORK_CFDATA_ARRAY_CALLBACK)
bool NetworkResourceLoader::supportsDataArray()
{
    notImplemented();
    return false;
}

void NetworkResourceLoader::didReceiveDataArray(ResourceHandle*, CFArrayRef)
{
    ASSERT_NOT_REACHED();
    notImplemented();
}
#endif

#if PLATFORM(MAC)
void NetworkResourceLoader::willStopBufferingData(ResourceHandle*, const char*, int)
{
    notImplemented();
}
#endif // PLATFORM(MAC)

} // namespace WebKit

#endif // ENABLE(NETWORK_PROCESS)
