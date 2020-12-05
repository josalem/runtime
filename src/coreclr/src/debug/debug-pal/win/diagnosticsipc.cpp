// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "diagnosticsipc.h"

#define _ASSERTE assert

#define ___ASSERT(pred) \
    if (!(pred)) \
    { \
        _ASSERTE(pred); \
        while (true) {} \
    }


IpcStream::DiagnosticsIpc::DiagnosticsIpc(const char(&namedPipeName)[MaxNamedPipeNameLength], ConnectionMode mode) : 
    mode(mode),
    _isListening(false)
{
    memcpy(_pNamedPipeName, namedPipeName, sizeof(_pNamedPipeName));
    for (DWORD i = 0; i < INSTANCES; i++)
    {
        memset(&Instances[i]._oOverlap, 0, sizeof(OVERLAPPED));
        Instances[i]._oOverlap.hEvent = INVALID_HANDLE_VALUE;
        Instances[i]._hPipe = INVALID_HANDLE_VALUE;
        Events[i] = INVALID_HANDLE_VALUE;
    }
    memset(&_oOverlap, 0, sizeof(OVERLAPPED));
}

BOOL IpcStream::DiagnosticsIpc::Reinitialize(DWORD instance, ErrorCallback callback)
{
    fprintf(stdout, "::REINITIALIZED\n");
    BOOL fSuccess = true;
    Instances[instance]._isListening = false;
    if (Instances[instance]._hPipe != INVALID_HANDLE_VALUE)
    {
        fSuccess &= DisconnectNamedPipe(Instances[instance]._hPipe);
        fSuccess &= ::CloseHandle(Instances[instance]._hPipe);
        Instances[instance]._hPipe = INVALID_HANDLE_VALUE;
    }

    if (Instances[instance]._oOverlap.hEvent != INVALID_HANDLE_VALUE)
    {
        fSuccess &= ::CloseHandle(Instances[instance]._oOverlap.hEvent);
        memset(&Instances[instance]._oOverlap, 0, sizeof(OVERLAPPED)); // clear the overlapped objects state
        Instances[instance]._oOverlap.hEvent = INVALID_HANDLE_VALUE;
        Events[instance] = INVALID_HANDLE_VALUE;
    }

    return fSuccess;
}

bool IpcStream::DiagnosticsIpc::Reset(ErrorCallback callback)
{
    BOOL fSuccess = true;
    for (DWORD i = 0; i < INSTANCES; i++)
        fSuccess &= DisconnectAndReconnect(i, callback);
    return fSuccess;
}

BOOL IpcStream::DiagnosticsIpc::DisconnectAndReconnect(DWORD instance, ErrorCallback callback)
{
    BOOL fSuccess = DisconnectNamedPipe(Instances[instance]._hPipe);
    if (!fSuccess)
    {
        if (callback != nullptr)
            callback("Failed to DisconnectNamedPipe!", ::GetLastError());
    }

    Instances[instance]._isListening = false;

    fSuccess = ListenInternal(instance, callback);
    if (!fSuccess)
    {
        if (callback != nullptr)
            callback("Failed to ListenInternal!", -1);
    }

    return fSuccess;
}

IpcStream::DiagnosticsIpc::~DiagnosticsIpc()
{
    Close();
}

IpcStream::DiagnosticsIpc *IpcStream::DiagnosticsIpc::Create(const char *const pIpcName, ConnectionMode mode, ErrorCallback callback)
{
    char namedPipeName[MaxNamedPipeNameLength]{};
    int nCharactersWritten = -1;

    if (pIpcName != nullptr)
    {
        nCharactersWritten = sprintf_s(
            namedPipeName,
            sizeof(namedPipeName),
            "\\\\.\\pipe\\%s",
            pIpcName);
    }
    else
    {
        nCharactersWritten = sprintf_s(
            namedPipeName,
            sizeof(namedPipeName),
            "\\\\.\\pipe\\dotnet-diagnostic-%d",
            ::GetCurrentProcessId());
    }

    if (nCharactersWritten == -1)
    {
        if (callback != nullptr)
            callback("Failed to generate the named pipe name", nCharactersWritten);
        _ASSERTE(nCharactersWritten != -1);
        return nullptr;
    }

    return new IpcStream::DiagnosticsIpc(namedPipeName, mode);
}

BOOL IpcStream::DiagnosticsIpc::ListenInternal(DWORD instance, ErrorCallback callback)
{
    _ASSERTE(mode == ConnectionMode::LISTEN);
    if (mode != ConnectionMode::LISTEN)
    {
        if (callback != nullptr)
            callback("Cannot call Listen on a client connection", -1);
        return false;
    }

    _ASSERTE(Instances[instance]._hPipe != INVALID_HANDLE_VALUE);
    _ASSERTE(Instances[instance]._oOverlap.hEvent != INVALID_HANDLE_VALUE);

    BOOL fSuccess = ::ConnectNamedPipe(Instances[instance]._hPipe, &Instances[instance]._oOverlap) != 0;
    Instances[instance]._isListening = true;
    if (!fSuccess)
    {
        const DWORD errorCode = ::GetLastError();
        switch (errorCode)
        {
            case ERROR_IO_PENDING:
                // There was a pending connection that can be waited on (will happen in poll)
                break;
            case ERROR_PIPE_CONNECTED:
                // Occurs when a client connects before the function is called.
                // In this case, there is a connection between client and
                // server, even though the function returned zero.
            case ERROR_NO_DATA:
                // The pipe closed before we could connect to it.
                // This was most likely someone inspecting the "file" via
                // an API like GetFileAttributes.  The listen "failed", but
                // we'll realize this on the next Accept.
                SetEvent(Instances[instance]._oOverlap.hEvent);
                fprintf(stdout, "ERROR_PIPE_CONNECTED || ERROR_NO_DATA - setting overlapped event\n");
                break;

            default:
                if (callback != nullptr)
                    callback("A client process failed to connect with an unexpected error.", errorCode);
                ___ASSERT(!errorCode);
                SetEvent(Instances[instance]._oOverlap.hEvent);
                return false;
        }
    }


    return true;
}

bool IpcStream::DiagnosticsIpc::Listen(ErrorCallback callback)
{
    _ASSERTE(mode == ConnectionMode::LISTEN);
    if (mode != ConnectionMode::LISTEN)
    {
        if (callback != nullptr)
            callback("Cannot call Listen on a client connection", -1);
        return false;
    }

    BOOL fSuccess = TRUE;
    for (DWORD i = 0; i < INSTANCES; i++)
    {
        fSuccess &= CreatePipe(i, callback);
        fSuccess &= ListenInternal(i, callback);
    }

    return fSuccess;
}

BOOL IpcStream::DiagnosticsIpc::CreatePipe(DWORD instance, ErrorCallback callback)
{
    if (Instances[instance]._isListening)
        return true;

    _ASSERTE(Instances[instance]._hPipe == INVALID_HANDLE_VALUE);
    _ASSERTE(Events[instance] == INVALID_HANDLE_VALUE);

    const uint32_t nInBufferSize = 16 * 1024;
    const uint32_t nOutBufferSize = 16 * 1024;
    Instances[instance]._hPipe = ::CreateNamedPipeA(
        _pNamedPipeName,                                            // pipe name
        PIPE_ACCESS_DUPLEX |                                        // read/write access
        FILE_FLAG_OVERLAPPED,                                       // async listening
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,    // message type pipe, message-read and blocking mode
        PIPE_UNLIMITED_INSTANCES,                                   // max. instances
        nOutBufferSize,                                             // output buffer size
        nInBufferSize,                                              // input buffer size
        0,                                                          // default client time-out
        NULL);                                                      // default security attribute

    if (Instances[instance]._hPipe == INVALID_HANDLE_VALUE)
    {
        DWORD dwError = ::GetLastError();
        if (callback != nullptr)
            callback("Failed to create an instance of a named pipe.", dwError);
        ___ASSERT(!IsValid());
        return false;
    }

    Events[instance] = CreateEvent(NULL, true, false, NULL);
    if (Events[instance] == NULL)
    {
        DWORD dwError = ::GetLastError();
        if (callback != nullptr)
            callback("Failed to create overlap event", dwError);
        ___ASSERT(IsValid());
        // Reset();
        return false;
    }
    Instances[instance]._oOverlap.hEvent = Events[instance];

    return TRUE;
}

BOOL IpcStream::DiagnosticsIpc::RecreatePipe(DWORD instance, ErrorCallback callback)
{
    BOOL fSuccess = Reinitialize(instance, callback);
    fSuccess &= CreatePipe(instance, callback);
    fSuccess &= ListenInternal(instance, callback);
    fprintf(stdout, "RECREATING? %d - pipe %p\n", fSuccess, _hPipe);
    return fSuccess;
}

IpcStream *IpcStream::DiagnosticsIpc::Accept(ErrorCallback callback)
{
    // _ASSERTE(_isListening);
    _ASSERTE(mode == ConnectionMode::LISTEN);

    // DWORD dwDummy = 0;
    IpcStream *pStream = nullptr;
    bool fSuccess = true;
    // bool fSuccess = GetOverlappedResult(
    //     _hPipe,     // handle
    //     &_oOverlap, // overlapped
    //     &dwDummy,   // throw-away dword
    //     true);      // wait till event signals

    DWORD dwWait = WaitForMultipleObjects(INSTANCES, Events, false, INFINITE);

    if (dwWait >= WAIT_OBJECT_0 && dwWait < WAIT_OBJECT_0 + INSTANCES)
    {
        DWORD instance = dwWait - WAIT_OBJECT_0;
        HANDLE newPipeConnection = Instances[instance]._hPipe;
        Instances[instance]._hPipe = INVALID_HANDLE_VALUE;
        pStream = new IpcStream(newPipeConnection, ConnectionMode::LISTEN);
        fSuccess = RecreatePipe(instance, callback);
        return pStream;
    }
    else
    {
        // TODO
        return nullptr;
    }
    

    // if (!fSuccess)
    // {
    //     DWORD dwError = ::GetLastError();
    //     if (callback != nullptr)
    //         callback("Failed to GetOverlappedResults for NamedPipe server", dwError);
    //     ___ASSERT(fSuccess);
    //     fprintf(stdout, "failed to GetOverlappedResults!! error %d, pipe %p\n", dwError, _hPipe);
    //     // close the pipe (cleaned up and reset below)
    //     //::CloseHandle(_hPipe);
    //     //DisconnectAndReconnect();
    //     return nullptr;
    // }
    // else
    // {
    //     // create new IpcStream using handle (passes ownership to pStream)
    //     HANDLE newPipeConnection = _hPipe;
    //     _hPipe = INVALID_HANDLE_VALUE;
    //     pStream = new IpcStream(newPipeConnection, ConnectionMode::LISTEN);
    // }

    // // reset the server
    // fSuccess = RecreatePipe(callback);
    // // ___ASSERT(IsValid());
    // // if (!fSuccess)
    // // {
    // //     delete pStream;
    // //     pStream = nullptr;
    // // }

    // return pStream;
}

IpcStream *IpcStream::DiagnosticsIpc::Connect(ErrorCallback callback)
{
    _ASSERTE(mode == ConnectionMode::CONNECT);
    if (mode != ConnectionMode::CONNECT)
    {
        if (callback != nullptr)
            callback("Cannot call connect on a server connection", 0);
        return nullptr;
    }

    HANDLE hPipe = ::CreateFileA( 
        _pNamedPipeName,                    // pipe name 
        PIPE_ACCESS_DUPLEX,                 // read/write access
        0,                                  // no sharing 
        NULL,                               // default security attributes
        OPEN_EXISTING,                      // opens existing pipe 
        FILE_FLAG_OVERLAPPED,               // Overlapped
        NULL);                              // no template file

    if (hPipe == INVALID_HANDLE_VALUE)
    {
        if (callback != nullptr)
            callback("Failed to connect to named pipe.", ::GetLastError());
        return nullptr;
    }

    return new IpcStream(hPipe, mode);
}

void IpcStream::DiagnosticsIpc::Close(bool isShutdown, ErrorCallback callback)
{
    // TODO
    // don't attempt cleanup on shutdown and let the OS handle it
    if (isShutdown)
    {
        if (callback != nullptr)
            callback("Closing without cleaning underlying handles", 100);
        return;
    }

    if (_hPipe != INVALID_HANDLE_VALUE)
    {
        if (mode == DiagnosticsIpc::ConnectionMode::LISTEN)
        {
            const BOOL fSuccessDisconnectNamedPipe = ::DisconnectNamedPipe(_hPipe);
            _ASSERTE(fSuccessDisconnectNamedPipe != 0);
            if (fSuccessDisconnectNamedPipe != 0 && callback != nullptr)
                callback("Failed to disconnect NamedPipe", ::GetLastError());
        }

        const BOOL fSuccessCloseHandle = ::CloseHandle(_hPipe);
        _ASSERTE(fSuccessCloseHandle != 0);
        if (fSuccessCloseHandle != 0 && callback != nullptr)
            callback("Failed to close pipe handle", ::GetLastError());
    }

    if (_oOverlap.hEvent != INVALID_HANDLE_VALUE)
    {
        const BOOL fSuccessCloseEvent = ::CloseHandle(_oOverlap.hEvent);
        _ASSERTE(fSuccessCloseEvent != 0);
        if (fSuccessCloseEvent != 0 && callback != nullptr)
            callback("Failed to close overlap event handle", ::GetLastError());
    }
}

IpcStream::IpcStream(HANDLE hPipe, DiagnosticsIpc::ConnectionMode mode) :
    _hPipe(hPipe), 
    _mode(mode) 
{
    memset(&_oOverlap, 0, sizeof(OVERLAPPED));
    _oOverlap.hEvent = CreateEvent(NULL, true, false, NULL);
}

IpcStream::~IpcStream()
{
    Close();
}

void IpcStream::Close(ErrorCallback callback)
{
    if (_hPipe != INVALID_HANDLE_VALUE)
    {
        Flush();

        if (_mode == DiagnosticsIpc::ConnectionMode::LISTEN)
        {
            const BOOL fSuccessDisconnectNamedPipe = ::DisconnectNamedPipe(_hPipe);
            _ASSERTE(fSuccessDisconnectNamedPipe != 0);
            if (fSuccessDisconnectNamedPipe != 0 && callback != nullptr)
                callback("Failed to disconnect NamedPipe", ::GetLastError());
        }

        const BOOL fSuccessCloseHandle = ::CloseHandle(_hPipe);
        _ASSERTE(fSuccessCloseHandle != 0);
        if (fSuccessCloseHandle != 0 && callback != nullptr)
            callback("Failed to close pipe handle", ::GetLastError());
    }

    if (_oOverlap.hEvent != INVALID_HANDLE_VALUE)
    {
        const BOOL fSuccessCloseEvent = ::CloseHandle(_oOverlap.hEvent);
        _ASSERTE(fSuccessCloseEvent != 0);
        if (fSuccessCloseEvent != 0 && callback != nullptr)
            callback("Failed to close overlapped event handle", ::GetLastError());
    }
}

int32_t IpcStream::DiagnosticsIpc::Poll(IpcPollHandle *rgIpcPollHandles, uint32_t nHandles, int32_t timeoutMs, ErrorCallback callback)
{
    uint32_t nHandlesActual = 0;
    DWORD *pRanges = new DWORD[nHandles];
    for (uint32_t i = 0; i < nHandles; i++)
    {
        if (rgIpcPollHandles[i].pIpc != nullptr)
        {
            pRanges[i] = i + rgIpcPollHandles[i].pIpc->INSTANCES;
            // expand count for additional instances
            nHandlesActual += rgIpcPollHandles[i].pIpc->INSTANCES;
        }
        else
        {
            pRanges[i] = i + 1;
            nHandlesActual++;
        }
    }

    // load up an array of handles
    HANDLE *pHandles = new HANDLE[nHandlesActual];
    uint32_t actualIndex = 0;
    for (uint32_t i = 0; i < nHandles; i++)
    {
        rgIpcPollHandles[i].revents = 0; // ignore any inputs on revents
        if (rgIpcPollHandles[i].pIpc != nullptr)
        {
            // SERVER
            _ASSERTE(rgIpcPollHandles[i].pIpc->mode == DiagnosticsIpc::ConnectionMode::LISTEN);
            for (uint32_t j = 0; j < rgIpcPollHandles[i].pIpc->INSTANCES; j++)
            {
                pHandles[actualIndex++] = rgIpcPollHandles[i].pIpc->Events[j];
            }
            // pHandles[i] = rgIpcPollHandles[i].pIpc->_oOverlap.hEvent;
        }
        else
        {
            // CLIENT
            bool fSuccess = false;
            DWORD dwDummy = 0;
            if (!rgIpcPollHandles[i].pStream->_isTestReading)
            {
                // check for data by doing an asynchronous 0 byte read.
                // This will signal if the pipe closes (hangup) or the server
                // sends new data
                fSuccess = ::ReadFile(
                    rgIpcPollHandles[i].pStream->_hPipe,      // handle
                    nullptr,                                    // null buffer
                    0,                                          // read 0 bytes
                    &dwDummy,                                   // dummy variable
                    &rgIpcPollHandles[i].pStream->_oOverlap); // overlap object to use
                rgIpcPollHandles[i].pStream->_isTestReading = true;
                if (!fSuccess)
                {
                    DWORD error = ::GetLastError();
                    switch (error)
                    {
                        case ERROR_IO_PENDING:
                            pHandles[actualIndex++] = rgIpcPollHandles[i].pStream->_oOverlap.hEvent;
                            break;
                        case ERROR_PIPE_NOT_CONNECTED:
                            // hangup
                            rgIpcPollHandles[i].revents = (uint8_t)PollEvents::HANGUP;
                            delete[] pHandles;
                            delete[] pRanges;
                            return -1;
                        default:
                            if (callback != nullptr)
                                callback("0 byte async read on client connection failed", error);
                            delete[] pHandles;
                            delete[] pRanges;
                            return -1;
                    }
                }
                else
                {
                    // there's already data to be read
                    pHandles[actualIndex++] = rgIpcPollHandles[i].pStream->_oOverlap.hEvent;
                }
            }
            else
            {
                pHandles[actualIndex++] = rgIpcPollHandles[i].pStream->_oOverlap.hEvent;
            }
        }
    }

    // call wait for multiple obj
    DWORD dwWait = WaitForMultipleObjects(
        nHandlesActual, // count
        pHandles,       // handles
        false,          // Don't wait-all
        timeoutMs);
    
    if (dwWait == WAIT_TIMEOUT)
    {
        // we timed out
        delete[] pHandles;
        delete[] pRanges;
        return 0;
    }

    if (dwWait == WAIT_FAILED)
    {
        // we errored
        DWORD dwError = GetLastError();
        fprintf(stdout, "WFMO failed - %d\n", dwError);
        for (uint32_t i = 0; i < nHandlesActual; i++)
        {
            fprintf(stdout, "\thandle (0x%p)\n", pHandles[i]);
        }
        _ASSERTE(!"Failed wait for multiple objects!!" && dwError);
        if (callback != nullptr)
            callback("WaitForMultipleObjects failed", dwError);
        delete[] pHandles;
        delete[] pRanges;
        return -1;
    }

    // determine which of the streams signaled
    DWORD index = dwWait - WAIT_OBJECT_0;
    // error check the index
    if (index < 0 || index > (nHandlesActual - 1))
    {
        // check if we abandoned something
        // TODO this logic is wrong now
        DWORD abandonedIndex = dwWait - WAIT_ABANDONED_0;
        if (abandonedIndex > 0 || abandonedIndex < (nHandles - 1))
        {
            rgIpcPollHandles[abandonedIndex].revents = (uint8_t)IpcStream::DiagnosticsIpc::PollEvents::HANGUP;
            delete[] pHandles;
            delete[] pRanges;
            return -1;
        }
        else
        {
            if (callback != nullptr)
                callback("WaitForMultipleObjects failed", ::GetLastError());
            delete[] pHandles;
            delete[] pRanges;
            return -1;
        }
    }

    DWORD signaledIndexActual = 0;
    for (DWORD i = 0; i < nHandles; i++)
    {
        if (i <= index && index < pRanges[i])
        {
            signaledIndexActual = i;
            break;
        }
    }

    // Set revents depending on what signaled the stream
    if (rgIpcPollHandles[signaledIndexActual].pIpc == nullptr)
    {
        // CLIENT
        // check if the connection got hung up
        DWORD dwDummy = 0;
        bool fSuccess = GetOverlappedResult(rgIpcPollHandles[signaledIndexActual].pStream->_hPipe,
                                            &rgIpcPollHandles[signaledIndexActual].pStream->_oOverlap,
                                            &dwDummy,
                                            true);
        rgIpcPollHandles[signaledIndexActual].pStream->_isTestReading = false;
        if (!fSuccess)
        {
            DWORD error = ::GetLastError();
            if (error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_BROKEN_PIPE)
                rgIpcPollHandles[signaledIndexActual].revents = (uint8_t)IpcStream::DiagnosticsIpc::PollEvents::HANGUP;
            else
            {
                if (callback != nullptr)
                    callback("Client connection error", error);
                rgIpcPollHandles[signaledIndexActual].revents = (uint8_t)IpcStream::DiagnosticsIpc::PollEvents::ERR;
                delete[] pHandles;
                delete[] pRanges;
                return -1;
            }
        }
        else
        {
            rgIpcPollHandles[signaledIndexActual].revents = (uint8_t)IpcStream::DiagnosticsIpc::PollEvents::SIGNALED;
        }
    }
    else
    {
        // SERVER
        rgIpcPollHandles[signaledIndexActual].revents = (uint8_t)IpcStream::DiagnosticsIpc::PollEvents::SIGNALED;
    }

    delete[] pHandles;
    delete[] pRanges;
    return 1;
}

bool IpcStream::Read(void *lpBuffer, const uint32_t nBytesToRead, uint32_t &nBytesRead, const int32_t timeoutMs)
{
    _ASSERTE(lpBuffer != nullptr);

    DWORD nNumberOfBytesRead = 0;
    LPOVERLAPPED overlap = &_oOverlap;
    bool fSuccess = ::ReadFile(
        _hPipe,                 // handle to pipe
        lpBuffer,               // buffer to receive data
        nBytesToRead,           // size of buffer
        &nNumberOfBytesRead,    // number of bytes read
        overlap) != 0;          // overlapped I/O

    if (!fSuccess)
    {
        DWORD dwError = GetLastError();

        // if we're waiting infinitely, only make one syscall
        if (timeoutMs == InfiniteTimeout && dwError == ERROR_IO_PENDING)
        {
            fSuccess = GetOverlappedResult(_hPipe,              // pipe
                                           overlap,             // overlapped
                                           &nNumberOfBytesRead, // out actual number of bytes read
                                           true) != 0;          // block until async IO completes
        }
        else if (dwError == ERROR_IO_PENDING)
        {
            // Wait on overlapped IO event (triggers when async IO is complete regardless of success)
            // or timeout
            DWORD dwWait = WaitForSingleObject(_oOverlap.hEvent, (DWORD)timeoutMs);
            if (dwWait == WAIT_OBJECT_0)
            {
                // async IO compelted, get the result
                fSuccess = GetOverlappedResult(_hPipe,              // pipe
                                               overlap,             // overlapped
                                               &nNumberOfBytesRead, // out actual number of bytes read
                                               true) != 0;          // block until async IO completes
            }
            else
            {
                // We either timed out or something else went wrong.
                // For any error, attempt to cancel IO and ensure the cancel happened
                if (CancelIoEx(_hPipe, overlap) != 0)
                {
                    // check if the async write beat the cancellation
                    fSuccess = GetOverlappedResult(_hPipe, overlap, &nNumberOfBytesRead, true) != 0;
                    // Failure here isn't recoverable, so return as such
                }
            }
        }
        // error is unrecoverable, so return as such
    }

    nBytesRead = static_cast<uint32_t>(nNumberOfBytesRead);
    return fSuccess;
}

bool IpcStream::Write(const void *lpBuffer, const uint32_t nBytesToWrite, uint32_t &nBytesWritten, const int32_t timeoutMs)
{
    _ASSERTE(lpBuffer != nullptr);

    DWORD nNumberOfBytesWritten = 0;
    LPOVERLAPPED overlap = &_oOverlap;
    bool fSuccess = ::WriteFile(
        _hPipe,                 // handle to pipe
        lpBuffer,               // buffer to write from
        nBytesToWrite,          // number of bytes to write
        &nNumberOfBytesWritten, // number of bytes written
        overlap) != 0;          // overlapped I/O

    if (!fSuccess)
    {
        DWORD dwError = GetLastError();

        // if we're waiting infinitely, only make one syscall
        if (timeoutMs == InfiniteTimeout && dwError == ERROR_IO_PENDING)
        {
            fSuccess = GetOverlappedResult(_hPipe,                 // pipe
                                           overlap,                // overlapped
                                           &nNumberOfBytesWritten, // out actual number of bytes written
                                           true) != 0;             // block until async IO completes
        }
        else if (dwError == ERROR_IO_PENDING)
        {
            // Wait on overlapped IO event (triggers when async IO is complete regardless of success)
            // or timeout
            DWORD dwWait = WaitForSingleObject(_oOverlap.hEvent, (DWORD)timeoutMs);
            if (dwWait == WAIT_OBJECT_0)
            {
                // async IO compelted, get the result
                fSuccess = GetOverlappedResult(_hPipe,                 // pipe
                                               overlap,                // overlapped
                                               &nNumberOfBytesWritten, // out actual number of bytes written
                                               true) != 0;             // block until async IO completes
            }
            else
            {
                // We either timed out or something else went wrong.
                // For any error, attempt to cancel IO and ensure the cancel happened
                if (CancelIoEx(_hPipe, overlap) != 0)
                {
                    // check if the async write beat the cancellation
                    fSuccess = GetOverlappedResult(_hPipe, overlap, &nNumberOfBytesWritten, true) != 0;
                    // Failure here isn't recoverable, so return as such
                }
            }
        }
        // error is unrecoverable, so return as such
    }

    nBytesWritten = static_cast<uint32_t>(nNumberOfBytesWritten);
    return fSuccess;
}

bool IpcStream::Flush() const
{
    const bool fSuccess = ::FlushFileBuffers(_hPipe) != 0;
    if (!fSuccess)
    {
        // TODO: Add error handling.
    }
    return fSuccess;
}
