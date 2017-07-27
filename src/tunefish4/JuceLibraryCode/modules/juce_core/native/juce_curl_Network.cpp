/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2017 - ROLI Ltd.

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

class WebInputStream::Pimpl
{
public:
    Pimpl (WebInputStream& ownerStream, const URL& urlToCopy, bool shouldUsePost)
        : owner (ownerStream), url (urlToCopy), isPost (shouldUsePost),
          httpRequest (isPost ? "POST" : "GET")
    {
        multi = curl_multi_init();

        if (multi != nullptr)
        {
            curl = curl_easy_init();

            if (curl != nullptr)
                if (curl_multi_add_handle (multi, curl) == CURLM_OK)
                    return;
        }

        cleanup();
    }

    ~Pimpl()
    {
        cleanup();
    }

    //==============================================================================
    // Input Stream overrides
    bool isError() const                 { return curl == nullptr || lastError != CURLE_OK; }
    bool isExhausted()                   { return (isError() || finished) && curlBuffer.getSize() == 0; }
    int64 getPosition()                  { return streamPos; }
    int64 getTotalLength()               { return contentLength; }

    int read (void* buffer, int bytesToRead)
    {
        return readOrSkip (buffer, bytesToRead, false);
    }

    bool setPosition (int64 wantedPos)
    {
        const int amountToSkip = static_cast<int> (wantedPos - getPosition());

        if (amountToSkip < 0)
            return false;

        if (amountToSkip == 0)
            return true;

        const int actuallySkipped = readOrSkip (nullptr, amountToSkip, true);

        return actuallySkipped == amountToSkip;
    }

    //==============================================================================
    // WebInputStream methods
    void withExtraHeaders (const String& extraHeaders)
    {
        if (! requestHeaders.endsWithChar ('\n') && requestHeaders.isNotEmpty())
            requestHeaders << "\r\n";

        requestHeaders << extraHeaders;

        if (! requestHeaders.endsWithChar ('\n') && requestHeaders.isNotEmpty())
            requestHeaders << "\r\n";
    }

    void withCustomRequestCommand (const String& customRequestCommand)    { httpRequest = customRequestCommand; }
    void withConnectionTimeout (int timeoutInMs)                          { timeOutMs = timeoutInMs; }
    void withNumRedirectsToFollow (int maxRedirectsToFollow)              { maxRedirects = maxRedirectsToFollow; }
    StringPairArray getRequestHeaders() const                             { return WebInputStream::parseHttpHeaders (requestHeaders); }
    StringPairArray getResponseHeaders() const                            { return WebInputStream::parseHttpHeaders (responseHeaders); }
    int getStatusCode() const                                             { return statusCode; }

    //==============================================================================
    void cleanup()
    {
        const ScopedLock lock (cleanupLock);

        if (curl != nullptr)
        {
            curl_multi_remove_handle (multi, curl);

            if (headerList != nullptr)
            {
                curl_slist_free_all (headerList);
                headerList = nullptr;
            }

            curl_easy_cleanup (curl);
            curl = nullptr;
        }

        if (multi != nullptr)
        {
            curl_multi_cleanup (multi);
            multi = nullptr;
        }
    }

    void cancel()
    {
        cleanup();
    }

    //==============================================================================
    bool setOptions()
    {
        const String address = url.toString (! isPost);

        curl_version_info_data* data = curl_version_info (CURLVERSION_NOW);
        jassert (data != nullptr);

        if (! requestHeaders.endsWithChar ('\n'))
            requestHeaders << "\r\n";

        if (isPost)
            WebInputStream::createHeadersAndPostData (url, requestHeaders, headersAndPostData);

        if (! requestHeaders.endsWithChar ('\n'))
            requestHeaders << "\r\n";

        String userAgent = String ("curl/") + data->version;

        if (curl_easy_setopt (curl, CURLOPT_URL, address.toRawUTF8()) == CURLE_OK
            && curl_easy_setopt (curl, CURLOPT_WRITEDATA, this) == CURLE_OK
            && curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, StaticCurlWrite) == CURLE_OK
            && curl_easy_setopt (curl, CURLOPT_MAXREDIRS, static_cast<long> (maxRedirects)) == CURLE_OK
            && curl_easy_setopt (curl, CURLOPT_USERAGENT, userAgent.toRawUTF8()) == CURLE_OK
            && curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, (maxRedirects > 0 ? 1 : 0)) == CURLE_OK)
        {
            if (isPost)
            {
                if (curl_easy_setopt (curl, CURLOPT_READDATA, this) != CURLE_OK
                    || curl_easy_setopt (curl, CURLOPT_READFUNCTION, StaticCurlRead) != CURLE_OK)
                    return false;

                if (curl_easy_setopt (curl, CURLOPT_POST, 1) != CURLE_OK
                    || curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t> (headersAndPostData.getSize())) != CURLE_OK)
                    return false;
            }

            // handle special http request commands
            bool hasSpecialRequestCmd = isPost ? (httpRequest != "POST") : (httpRequest != "GET");
            if (hasSpecialRequestCmd)
            {
                if (curl_easy_setopt (curl, CURLOPT_CUSTOMREQUEST, httpRequest.toRawUTF8()) != CURLE_OK)
                    return false;
            }

            if (curl_easy_setopt (curl, CURLOPT_HEADERDATA, this) != CURLE_OK
                || curl_easy_setopt (curl, CURLOPT_HEADERFUNCTION, StaticCurlHeader) != CURLE_OK)
                return false;

            if (timeOutMs > 0)
            {
                long timeOutSecs = ((long) timeOutMs + 999) / 1000;

                if (curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, timeOutSecs) != CURLE_OK
                    || curl_easy_setopt (curl, CURLOPT_LOW_SPEED_LIMIT, 100) != CURLE_OK
                    || curl_easy_setopt (curl, CURLOPT_LOW_SPEED_TIME, timeOutSecs) != CURLE_OK)
                    return false;
            }

            return true;
        }

        return false;
    }

    bool connect (WebInputStream::Listener* webInputListener)
    {
        {
            const ScopedLock lock (cleanupLock);

            if (curl == nullptr)
                return false;

            if (! setOptions())
            {
                cleanup();
                return false;
            }

            if (requestHeaders.isNotEmpty())
            {
                const StringArray headerLines = StringArray::fromLines (requestHeaders);

                // fromLines will always return at least one line if the string is not empty
                jassert (headerLines.size() > 0);
                headerList = curl_slist_append (headerList, headerLines [0].toRawUTF8());

                for (int i = 1; (i < headerLines.size() && headerList != nullptr); ++i)
                    headerList = curl_slist_append (headerList, headerLines [i].toRawUTF8());

                if (headerList == nullptr)
                {
                    cleanup();
                    return false;
                }

                if (curl_easy_setopt (curl, CURLOPT_HTTPHEADER, headerList) != CURLE_OK)
                {
                    cleanup();
                    return false;
                }
            }
        }

        listener = webInputListener;

        if (isPost)
            postBuffer = &headersAndPostData;

        size_t lastPos = static_cast<size_t> (-1);

        // step until either: 1) there is an error 2) the transaction is complete
        // or 3) data is in the in buffer
        while ((! finished) && curlBuffer.getSize() == 0)
        {
            {
                const ScopedLock lock (cleanupLock);

                if (curl == nullptr)
                    return false;
            }

            singleStep();

            // call callbacks if this is a post request
            if (isPost && listener != nullptr && lastPos != postPosition)
            {
                lastPos = postPosition;

                if (! listener->postDataSendProgress (owner, static_cast<int> (lastPos), static_cast<int> (headersAndPostData.getSize())))
                {
                    // user has decided to abort the transaction
                    cleanup();
                    return false;
                }
            }
        }

        {
            const ScopedLock lock (cleanupLock);

            if (curl == nullptr)
                return false;

            long responseCode;
            if (curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &responseCode) == CURLE_OK)
                statusCode = static_cast<int> (responseCode);

            // get content length size
            double curlLength;
            if (curl_easy_getinfo (curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &curlLength) == CURLE_OK)
                contentLength = static_cast<int64> (curlLength);
        }

        return true;
    }

    void finish()
    {
        const ScopedLock lock (cleanupLock);

        if (curl == nullptr)
            return;

        for (;;)
        {
            int cnt = 0;

            if (CURLMsg* msg = curl_multi_info_read (multi, &cnt))
            {
                if (msg->msg == CURLMSG_DONE && msg->easy_handle == curl)
                {
                    lastError = msg->data.result; // this is the error that stopped our process from continuing
                    break;
                }
            }
            else
            {
                break;
            }
        }

        finished = true;
    }

    //==============================================================================
    void singleStep()
    {
        if (lastError != CURLE_OK)
            return;

        fd_set fdread, fdwrite, fdexcep;
        int maxfd = -1;
        long curl_timeo;

        {
            const ScopedLock lock (cleanupLock);

            if (multi == nullptr)
                return;

            if ((lastError = (int) curl_multi_timeout (multi, &curl_timeo)) != CURLM_OK)
                return;
        }

        // why 980? see http://curl.haxx.se/libcurl/c/curl_multi_timeout.html
        if (curl_timeo < 0)
            curl_timeo = 980;

        struct timeval tv;
        tv.tv_sec = curl_timeo / 1000;
        tv.tv_usec = (curl_timeo % 1000) * 1000;

        FD_ZERO (&fdread);
        FD_ZERO (&fdwrite);
        FD_ZERO (&fdexcep);

        {
            const ScopedLock lock (cleanupLock);

            if (multi == nullptr)
                return;

            if ((lastError = (int) curl_multi_fdset (multi, &fdread, &fdwrite, &fdexcep, &maxfd)) != CURLM_OK)
                return;
        }

        if (maxfd != -1)
        {
            if (select (maxfd + 1, &fdread, &fdwrite, &fdexcep, &tv) < 0)
            {
                lastError = -1;
                return;
            }
        }
        else
        {
            // if curl does not return any sockets for to wait on, then the doc says to wait 100 ms
            Thread::sleep (100);
        }

        int still_running = 0;
        int curlRet;

        {
            const ScopedLock lock (cleanupLock);

            while ((curlRet = (int) curl_multi_perform (multi, &still_running)) == CURLM_CALL_MULTI_PERFORM)
            {}
        }

        if ((lastError = curlRet) != CURLM_OK)
            return;

        if (still_running <= 0)
            finish();
    }

    int readOrSkip (void* buffer, int bytesToRead, bool skip)
    {
        if (bytesToRead <= 0)
            return 0;

        size_t pos = 0;
        size_t len = static_cast<size_t> (bytesToRead);

        while (len > 0)
        {
            size_t bufferBytes = curlBuffer.getSize();
            bool removeSection = true;

            if (bufferBytes == 0)
            {
                // do not call curl again if we are finished
                {
                    const ScopedLock lock (cleanupLock);

                    if (finished || curl == nullptr)
                        return static_cast<int> (pos);
                }

                skipBytes = skip ? len : 0;
                singleStep();

                // update the amount that was read/skipped from curl
                bufferBytes = skip ? len - skipBytes : curlBuffer.getSize();
                removeSection = ! skip;
            }

            // can we copy data from the internal buffer?
            if (bufferBytes > 0)
            {
                size_t max = jmin (len, bufferBytes);

                if (! skip)
                    memcpy (addBytesToPointer (buffer, pos), curlBuffer.getData(), max);

                pos += max;
                streamPos += static_cast<int64> (max);
                len -= max;

                if (removeSection)
                    curlBuffer.removeSection (0, max);
            }
        }

        return static_cast<int> (pos);
    }

    //==============================================================================
    // CURL callbacks
    size_t curlWriteCallback (char* ptr, size_t size, size_t nmemb)
    {
        if (curl == nullptr || lastError != CURLE_OK)
            return 0;

        const size_t len = size * nmemb;

        // skip bytes if necessary
        size_t max = jmin (skipBytes, len);
        skipBytes -= max;

        if (len > max)
            curlBuffer.append (ptr + max, len - max);

        return len;
    }

    size_t curlReadCallback (char* ptr, size_t size, size_t nmemb)
    {
        if (curl == nullptr || postBuffer == nullptr || lastError != CURLE_OK)
            return 0;

        const size_t len = size * nmemb;

        size_t max = jmin (postBuffer->getSize() - postPosition, len);
        memcpy (ptr, (char*)postBuffer->getData() + postPosition, max);
        postPosition += max;

        return max;
    }

    size_t curlHeaderCallback (char* ptr, size_t size, size_t nmemb)
    {
        if (curl == nullptr || lastError != CURLE_OK)
            return 0;

        size_t len = size * nmemb;

        String header (ptr, len);

        if (! header.contains (":") && header.startsWithIgnoreCase ("HTTP/"))
            responseHeaders.clear();
        else
            responseHeaders += header;

        return len;
    }


    //==============================================================================
    // Static method wrappers
    static size_t StaticCurlWrite (char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        WebInputStream::Pimpl* wi = reinterpret_cast<WebInputStream::Pimpl*> (userdata);
        return wi->curlWriteCallback (ptr, size, nmemb);
    }

    static size_t StaticCurlRead (char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        WebInputStream::Pimpl* wi = reinterpret_cast<WebInputStream::Pimpl*> (userdata);
        return wi->curlReadCallback (ptr, size, nmemb);
    }

    static size_t StaticCurlHeader (char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        WebInputStream::Pimpl* wi = reinterpret_cast<WebInputStream::Pimpl*> (userdata);
        return wi->curlHeaderCallback (ptr, size, nmemb);
    }

    //==============================================================================
    WebInputStream& owner;
    const URL url;

    //==============================================================================
    // curl stuff
    CURLM* multi = nullptr;
    CURL* curl = nullptr;
    struct curl_slist* headerList = nullptr;
    int lastError = CURLE_OK;

    //==============================================================================
    // Options
    int timeOutMs = 0;
    int maxRedirects = 5;
    const bool isPost;
    String httpRequest;

    //==============================================================================
    // internal buffers and buffer positions
    int64 contentLength = -1, streamPos = 0;
    MemoryBlock curlBuffer;
    MemoryBlock headersAndPostData;
    String responseHeaders, requestHeaders;
    int statusCode = -1;

    //==============================================================================
    bool finished = false;
    size_t skipBytes = 0;

    //==============================================================================
    // Http POST variables
    const MemoryBlock* postBuffer = nullptr;
    size_t postPosition = 0;

    //==============================================================================
    WebInputStream::Listener* listener = nullptr;

    //==============================================================================
    CriticalSection cleanupLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Pimpl)
};

URL::DownloadTask* URL::downloadToFile (const File& targetLocation, String extraHeaders, DownloadTask::Listener* listener)
{
    return URL::DownloadTask::createFallbackDownloader (*this, targetLocation, extraHeaders, listener);
}
