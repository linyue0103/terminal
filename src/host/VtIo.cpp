// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "VtIo.hpp"
#include "../interactivity/inc/ServiceLocator.hpp"

#include "../renderer/base/renderer.hpp"
#include "../types/inc/utils.hpp"
#include "handle.h" // LockConsole
#include "output.h" // CloseConsoleProcessState

using namespace Microsoft::Console;
using namespace Microsoft::Console::Render;
using namespace Microsoft::Console::VirtualTerminal;
using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::Utils;
using namespace Microsoft::Console::Interactivity;

VtIo::CorkLock::CorkLock(VtIo* io) noexcept :
    _io{ io }
{
}

VtIo::CorkLock::~CorkLock() noexcept
{
    if (_io)
    {
        _io->_uncork();
    }
}

VtIo::CorkLock::CorkLock(CorkLock&& other) :
    _io{ std::exchange(other._io, nullptr) }
{
}

VtIo::CorkLock& VtIo::CorkLock::operator=(CorkLock&& other)
{
    if (this != &other)
    {
        this->~CorkLock();
        _io = std::exchange(other._io, nullptr);
    }
    return *this;
}

[[nodiscard]] HRESULT VtIo::Initialize(const ConsoleArguments* const pArgs)
{
    _lookingForCursorPosition = pArgs->GetInheritCursor();

    // If we were already given VT handles, set up the VT IO engine to use those.
    if (pArgs->InConptyMode())
    {
        return _Initialize(pArgs->GetVtInHandle(), pArgs->GetVtOutHandle(), pArgs->GetSignalHandle());
    }
    // Didn't need to initialize if we didn't have VT stuff. It's still OK, but report we did nothing.
    else
    {
        return S_FALSE;
    }
}

// Routine Description:
//  Tries to initialize this VtIo instance from the given pipe handles and
//      VtIoMode. The pipes should have been created already (by the caller of
//      conhost), in non-overlapped mode.
//  The VtIoMode string can be the empty string as a default value.
// Arguments:
//  InHandle: a valid file handle. The console will
//      read VT sequences from this pipe to generate INPUT_RECORDs and other
//      input events.
//  OutHandle: a valid file handle. The console
//      will be "rendered" to this pipe using VT sequences
//  SignalHandle: an optional file handle that will be used to send signals into the console.
//      This represents the ability to send signals to a *nix tty/pty.
// Return Value:
//  S_OK if we initialized successfully, otherwise an appropriate HRESULT
//      indicating failure.
[[nodiscard]] HRESULT VtIo::_Initialize(const HANDLE InHandle,
                                        const HANDLE OutHandle,
                                        _In_opt_ const HANDLE SignalHandle)
{
    FAIL_FAST_IF_MSG(_initialized, "Someone attempted to double-_Initialize VtIo");

    _hInput.reset(InHandle);
    _hOutput.reset(OutHandle);
    _hSignal.reset(SignalHandle);

    // The only way we're initialized is if the args said we're in conpty mode.
    // If the args say so, then at least one of in, out, or signal was specified
    _initialized = true;
    return S_OK;
}

void VtIo::_uncork()
{
    _corked -= 1;
    _flush();
}

void VtIo::_flush()
{
    if (_corked == 0)
    {
        WriteUTF8(_buffer);
        _buffer.clear();
    }
}

// Method Description:
// - Create the VtEngine and the VtInputThread for this console.
// MUST BE DONE AFTER CONSOLE IS INITIALIZED, to make sure we've gotten the
//  buffer size from the attached client application.
// Arguments:
// - <none>
// Return Value:
//  S_OK if we initialized successfully,
//  S_FALSE if VtIo hasn't been initialized (or we're not in conpty mode)
//  otherwise an appropriate HRESULT indicating failure.
[[nodiscard]] HRESULT VtIo::CreateIoHandlers() noexcept
{
    if (!_initialized)
    {
        return S_FALSE;
    }

    // SetWindowVisibility uses the console lock to protect access to _pVtRenderEngine.
    assert(ServiceLocator::LocateGlobals().getConsoleInformation().IsConsoleLocked());

    try
    {
        if (IsValidHandle(_hInput.get()))
        {
            _pVtInputThread = std::make_unique<VtInputThread>(std::move(_hInput), _lookingForCursorPosition);
        }
    }
    CATCH_RETURN();

    return S_OK;
}

bool VtIo::IsUsingVt() const
{
    return _initialized;
}

// Routine Description:
//  Potentially starts this VtIo's input thread and render engine.
//      If the VtIo hasn't yet been given pipes, then this function will
//      silently do nothing. It's the responsibility of the caller to make sure
//      that the pipes are initialized first with VtIo::Initialize
// Arguments:
//  <none>
// Return Value:
//  S_OK if we started successfully or had nothing to start, otherwise an
//      appropriate HRESULT indicating failure.
[[nodiscard]] HRESULT VtIo::StartIfNeeded()
{
    // If we haven't been set up, do nothing (because there's nothing to start)
    if (!_initialized)
    {
        return S_FALSE;
    }

    // MSFT: 15813316
    // If the terminal application wants us to inherit the cursor position,
    //  we're going to emit a VT sequence to ask for the cursor position, then
    //  read input until we get a response. Terminals who request this behavior
    //  but don't respond will hang.
    // If we get a response, the InteractDispatch will call SetCursorPosition,
    //      which will call to our VtIo::SetCursorPosition method.
    // We need both handles for this initialization to work. If we don't have
    //      both, we'll skip it. They either aren't going to be reading output
    //      (so they can't get the DSR) or they can't write the response to us.
    if (_lookingForCursorPosition && _pVtInputThread)
    {
        //LOG_IF_FAILED(_pVtRenderEngine->RequestCursor());
        while (_lookingForCursorPosition && _pVtInputThread->DoReadInput())
        {
        }
    }

    // GH#4999 - Send a sequence to the connected terminal to request
    // win32-input-mode from them. This will enable the connected terminal to
    // send us full INPUT_RECORDs as input. If the terminal doesn't understand
    // this sequence, it'll just ignore it.
    //LOG_IF_FAILED(_pVtRenderEngine->RequestWin32Input());
    WriteUTF8("\x1b[20h\033[?9001h\033[?1004h");

    if (_pVtInputThread)
    {
        LOG_IF_FAILED(_pVtInputThread->Start());
    }

    if (_pPtySignalInputThread)
    {
        // Let the signal thread know that the console is connected.
        //
        // By this point, the pseudo window should have already been created, by
        // ConsoleInputThreadProcWin32. That thread has a message pump, which is
        // needed to ensure that DPI change messages to the owning terminal
        // window don't end up hanging because the pty didn't also process it.
        _pPtySignalInputThread->ConnectConsole();
    }

    return S_OK;
}

// Method Description:
// - Create our pseudo window. This is exclusively called by
//   ConsoleInputThreadProcWin32 on the console input thread.
//    * It needs to be called on that thread, before any other calls to
//      LocatePseudoWindow, to make sure that the input thread is the HWND's
//      message thread.
//    * It needs to be plumbed through the signal thread, because the signal
//      thread knows if someone should be marked as the window's owner. It's
//      VERY IMPORTANT that any initial owners are set up when the window is
//      first created.
// - Refer to GH#13066 for details.
void VtIo::CreatePseudoWindow()
{
    if (_pPtySignalInputThread)
    {
        _pPtySignalInputThread->CreatePseudoWindow();
    }
    else
    {
        ServiceLocator::LocatePseudoWindow();
    }
}

// Method Description:
// - Create and start the signal thread. The signal thread can be created
//      independent of the i/o threads, and doesn't require a client first
//      attaching to the console. We need to create it first and foremost,
//      because it's possible that a terminal application could
//      CreatePseudoConsole, then ClosePseudoConsole without ever attaching a
//      client. Should that happen, we still need to exit.
// Arguments:
// - <none>
// Return Value:
// - S_FALSE if we're not in VtIo mode,
//   S_OK if we succeeded,
//   otherwise an appropriate HRESULT indicating failure.
[[nodiscard]] HRESULT VtIo::CreateAndStartSignalThread() noexcept
{
    if (!_initialized)
    {
        return S_FALSE;
    }

    // If we were passed a signal handle, try to open it and make a signal reading thread.
    if (IsValidHandle(_hSignal.get()))
    {
        try
        {
            _pPtySignalInputThread = std::make_unique<PtySignalInputThread>(std::move(_hSignal));

            // Start it if it was successfully created.
            RETURN_IF_FAILED(_pPtySignalInputThread->Start());
        }
        CATCH_RETURN();
    }

    return S_OK;
}

void VtIo::CloseInput()
{
    _pVtInputThread = nullptr;
    SendCloseEvent();
}

void VtIo::CloseOutput()
{
    _hOutput.reset();
}

void VtIo::SendCloseEvent()
{
    LockConsole();
    const auto unlock = wil::scope_exit([] { UnlockConsole(); });

    // This function is called when the ConPTY signal pipe is closed (PtySignalInputThread) and when the input
    // pipe is closed (VtIo). Usually these two happen at about the same time. This if condition is a bit of
    // a premature optimization and prevents us from sending out a CTRL_CLOSE_EVENT right after another.
    if (!std::exchange(_closeEventSent, true))
    {
        CloseConsoleProcessState();
    }
}

VtIo::CorkLock VtIo::Cork() noexcept
{
    _corked += 1;
    return CorkLock{ this };
}

void VtIo::WriteUTF8(const std::string_view& str)
{
    if (str.empty() || !_hOutput)
    {
        return;
    }

    if (_corked > 0)
    {
        _buffer.append(str);
        return;
    }

    const auto fSuccess = WriteFile(_hOutput.get(), str.data(), gsl::narrow_cast<DWORD>(str.size()), nullptr, nullptr);
    if (!fSuccess)
    {
        LOG_LAST_ERROR();
        CloseOutput();
    }
}

void VtIo::WriteUTF16(const std::wstring_view& str)
{
    if (str.empty() || !_hOutput)
    {
        return;
    }

    const auto str8 = til::u16u8(str);
    WriteUTF8(str8);
}
