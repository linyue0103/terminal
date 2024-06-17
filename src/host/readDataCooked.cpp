// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "readDataCooked.hpp"

#include "alias.h"
#include "history.h"
#include "resource.h"
#include "stream.h"
#include "_stream.h"
#include "../interactivity/inc/ServiceLocator.hpp"

#pragma warning(disable : 4100 4189)

#define GetTextBufferAcceptable GetTextBuffer
#define esc(x) L"\x1b" x
#define csi(x) L"\x1b[" x

using Microsoft::Console::Interactivity::ServiceLocator;
using Microsoft::Console::VirtualTerminal::VtIo;

// Routine Description:
// - Constructs cooked read data class to hold context across key presses while a user is modifying their 'input line'.
// Arguments:
// - pInputBuffer - Buffer that data will be read from.
// - pInputReadHandleData - Context stored across calls from the same input handle to return partial data appropriately.
// - screenInfo - Output buffer that will be used for 'echoing' the line back to the user so they can see/manipulate it
// - UserBufferSize - The byte count of the buffer presented by the client
// - UserBuffer - The buffer that was presented by the client for filling with input data on read conclusion/return from server/host.
// - CtrlWakeupMask - Special client parameter to interrupt editing, end the wait, and return control to the client application
// - initialData - any text data that should be prepopulated into the buffer
// - pClientProcess - Attached process handle object
COOKED_READ_DATA::COOKED_READ_DATA(_In_ InputBuffer* const pInputBuffer,
                                   _In_ INPUT_READ_HANDLE_DATA* const pInputReadHandleData,
                                   SCREEN_INFORMATION& screenInfo,
                                   _In_ size_t UserBufferSize,
                                   _In_ char* UserBuffer,
                                   _In_ ULONG CtrlWakeupMask,
                                   _In_ const std::wstring_view exeName,
                                   _In_ const std::wstring_view initialData,
                                   _In_ ConsoleProcessHandle* const pClientProcess) :
    ReadData(pInputBuffer, pInputReadHandleData),
    _screenInfo{ screenInfo },
    _userBuffer{ UserBuffer, UserBufferSize },
    _exeName{ exeName },
    _processHandle{ pClientProcess },
    _history{ CommandHistory::s_Find(pClientProcess) },
    _ctrlWakeupMask{ CtrlWakeupMask },
    _insertMode{ ServiceLocator::LocateGlobals().getConsoleInformation().GetInsertMode() }
{
#ifndef UNIT_TESTING
    // The screen buffer instance is basically a reference counted HANDLE given out to the user.
    // We need to ensure that it stays alive for the duration of the read.
    // Coincidentally this serves another important purpose: It checks whether we're allowed to read from
    // the given buffer in the first place. If it's missing the FILE_SHARE_READ flag, we can't read from it.
    //
    // GH#16158: It's important that we hold a handle to the main instead of the alt buffer
    // even if this cooked read targets the latter, because alt buffers are fake
    // SCREEN_INFORMATION objects that are owned by the main buffer.
    THROW_IF_FAILED(_screenInfo.GetMainBuffer().AllocateIoHandle(ConsoleHandleData::HandleType::Output, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, _tempHandle));
#endif

    const auto& textBuffer = _screenInfo.GetTextBufferAcceptable();
    const auto& cursor = textBuffer.GetCursor();
    auto absoluteCursorPos = cursor.GetPosition();

    if (!initialData.empty())
    {
        _replace(initialData);

        // The console API around `nInitialChars` in `CONSOLE_READCONSOLE_CONTROL` is pretty weird.
        // The way it works is that cmd.exe does a ReadConsole() with a `dwCtrlWakeupMask` that includes \t,
        // so when you press tab it can autocomplete the prompt based on the available file names.
        // The weird part is that it's not us who then prints the autocompletion. It's cmd.exe which calls WriteConsoleW().
        // It then initiates another ReadConsole() where the `nInitialChars` is the amount of chars it wrote via WriteConsoleW().
        //
        // In other words, `nInitialChars` is a "trust me bro, I just wrote that in the buffer" API.
        // This unfortunately means that the API is inherently broken: ReadConsole() visualizes control
        // characters like Ctrl+X as "^X" and WriteConsoleW() doesn't and so the column counts don't match.
        // Solving these issues is technically possible, but it's also quite difficult to do so correctly.
        //
        // But unfortunately (or fortunately) the initial (from the 1990s up to 2023) looked something like that:
        //   cursor = cursor.GetPosition();
        //   cursor.x -= initialData.size();
        //   while (cursor.x < 0)
        //   {
        //       cursor.x += textBuffer.Width();
        //       cursor.y -= 1;
        //   }
        //
        // In other words, it assumed that the number of code units in the initial data corresponds 1:1 to
        // the column count. This meant that the API never supported tabs for instance (nor wide glyphs).
        // The new implementation still doesn't support tabs, but it does fix support for wide glyphs.
        // That seemed like a good trade-off.

        // NOTE: You can't just "measure" the length of the string in columns either, because previously written
        // wide glyphs might have resulted in padding whitespace in the text buffer (see ROW::WasDoubleBytePadded).
        til::CoordType columns = 0;
        textBuffer.FitTextIntoColumns(initialData, til::CoordTypeMax, columns);

        const int64_t w = textBuffer.GetSize().Width();
        const int64_t x = absoluteCursorPos.x;
        const int64_t y = absoluteCursorPos.y;

        auto cols = y * w + x - columns;
        cols = std::max<int64_t>(0, cols);

        absoluteCursorPos.x = gsl::narrow_cast<til::CoordType>(cols % w);
        absoluteCursorPos.y = gsl::narrow_cast<til::CoordType>(cols / w);
    }

    _screenInfo.GetVirtualViewport().ConvertToOrigin(&absoluteCursorPos);
    absoluteCursorPos.x = std::max(0, absoluteCursorPos.x);
    absoluteCursorPos.y = std::max(0, absoluteCursorPos.y);

    _originInViewport = absoluteCursorPos;
}

// Routine Description:
// - This routine is called to complete a cooked read that blocked in ReadInputBuffer.
// - The context of the read was saved in the CookedReadData structure.
// - This routine is called when events have been written to the input buffer.
// - It is called in the context of the writing thread.
// - It may be called more than once.
// Arguments:
// - TerminationReason - if this routine is called because a ctrl-c or ctrl-break was seen, this argument
//                      contains CtrlC or CtrlBreak. If the owning thread is exiting, it will have ThreadDying. Otherwise 0.
// - fIsUnicode - Whether to convert the final data to A (using Console Input CP) at the end or treat everything as Unicode (UCS-2)
// - pReplyStatus - The status code to return to the client application that originally called the API (before it was queued to wait)
// - pNumBytes - The number of bytes of data that the server/driver will need to transmit back to the client process
// - pControlKeyState - For certain types of reads, this specifies which modifier keys were held.
// - pOutputData - not used
// Return Value:
// - true if the wait is done and result buffer/status code can be sent back to the client.
// - false if we need to continue to wait until more data is available.
bool COOKED_READ_DATA::Notify(const WaitTerminationReason TerminationReason,
                              const bool fIsUnicode,
                              _Out_ NTSTATUS* const pReplyStatus,
                              _Out_ size_t* const pNumBytes,
                              _Out_ DWORD* const pControlKeyState,
                              _Out_ void* const /*pOutputData*/) noexcept
try
{
    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    *pNumBytes = 0;
    *pControlKeyState = 0;
    *pReplyStatus = STATUS_SUCCESS;

    // if ctrl-c or ctrl-break was seen, terminate read.
    if (WI_IsAnyFlagSet(TerminationReason, (WaitTerminationReason::CtrlC | WaitTerminationReason::CtrlBreak)))
    {
        *pReplyStatus = STATUS_ALERTED;
        gci.SetCookedReadData(nullptr);
        return true;
    }

    // See if we were called because the thread that owns this wait block is exiting.
    if (WI_IsFlagSet(TerminationReason, WaitTerminationReason::ThreadDying))
    {
        *pReplyStatus = STATUS_THREAD_IS_TERMINATING;
        gci.SetCookedReadData(nullptr);
        return true;
    }

    // We must see if we were woken up because the handle is being closed. If
    // so, we decrement the read count. If it goes to zero, we wake up the
    // close thread. Otherwise, we wake up any other thread waiting for data.
    if (WI_IsFlagSet(TerminationReason, WaitTerminationReason::HandleClosing))
    {
        *pReplyStatus = STATUS_ALERTED;
        gci.SetCookedReadData(nullptr);
        return true;
    }

    if (Read(fIsUnicode, *pNumBytes, *pControlKeyState))
    {
        gci.SetCookedReadData(nullptr);
        return true;
    }

    return false;
}
NT_CATCH_RETURN()

void COOKED_READ_DATA::MigrateUserBuffersOnTransitionToBackgroundWait(const void* oldBuffer, void* newBuffer) noexcept
{
    // See the comment in WaitBlock.cpp for more information.
    if (_userBuffer.data() == oldBuffer)
    {
        _userBuffer = { static_cast<char*>(newBuffer), _userBuffer.size() };
    }
}

// Routine Description:
// - Method that actually retrieves a character/input record from the buffer (key press form)
//   and determines the next action based on the various possible cooked read modes.
// - Mode options include the F-keys popup menus, keyboard manipulation of the edit line, etc.
// - This method also does the actual copying of the final manipulated data into the return buffer.
// Arguments:
// - isUnicode - Treat as UCS-2 unicode or use Input CP to convert when done.
// - numBytes - On in, the number of bytes available in the client
// buffer. On out, the number of bytes consumed in the client buffer.
// - controlKeyState - For some types of reads, this is the modifier key state with the last button press.
bool COOKED_READ_DATA::Read(const bool isUnicode, size_t& numBytes, ULONG& controlKeyState)
{
    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    VtIo::CorkLock corkLock;
    if (gci.IsInVtIoMode())
    {
        corkLock = gci.GetVtIo()->Cork();
    }

    controlKeyState = 0;

    _readCharInputLoop();

    // NOTE: Don't call _flushBuffer in a wil::scope_exit/defer.
    // It may throw and throwing during an ongoing exception is a bad idea.
    _flushBuffer();

    if (_state == State::Accumulating)
    {
        return false;
    }

    _handlePostCharInputLoop(isUnicode, numBytes, controlKeyState);
    return true;
}

// Printing wide glyphs at the end of a row results in a forced line wrap and a padding whitespace to be inserted.
// When the text buffer resizes these padding spaces may vanish and the _distanceCursor and _distanceEnd measurements become inaccurate.
// To fix this, this function is called before a resize and will clear the input line. Afterwards, RedrawAfterResize() will restore it.
void COOKED_READ_DATA::EraseBeforeResize() const
{
    std::wstring output;
    _appendCUP(output, _originInViewport);
    output.append(csi("J"));
    WriteCharsVT(_screenInfo, output);
}

// The counter-part to EraseBeforeResize().
void COOKED_READ_DATA::RedrawAfterResize()
{
    _bufferDirtyBeg = 0;
    _dirty = true;
    _flushBuffer();
}

void COOKED_READ_DATA::SetInsertMode(bool insertMode) noexcept
{
    _insertMode = insertMode;
}

bool COOKED_READ_DATA::IsEmpty() const noexcept
{
    return _buffer.empty() && _popups.empty();
}

bool COOKED_READ_DATA::PresentingPopup() const noexcept
{
    return !_popups.empty();
}

til::point_span COOKED_READ_DATA::GetBoundaries() const noexcept
{
    const auto& textBuffer = _screenInfo.GetTextBuffer();
    const auto& cursor = textBuffer.GetCursor();
    // TODO
    const auto beg = til::point{}; //_offsetPosition(cursor.GetPosition(), -_distanceCursor);
    const auto end = til::point{}; //_offsetPosition(beg, _distanceEnd);
    return { beg, end };
}

// _wordPrev and _wordNext implement the classic Windows word-wise cursor movement algorithm, as traditionally used by
// conhost, notepad, Visual Studio and other "old" applications. If you look closely you can see how they're the exact
// same "skip 1 char, skip x, skip not-x", but since the "x" between them is different (non-words for _wordPrev and
// words for _wordNext) it results in the inconsistent feeling that these have compared to more modern algorithms.
// TODO: GH#15787
size_t COOKED_READ_DATA::_wordPrev(const std::wstring_view& chars, size_t position)
{
    if (position != 0)
    {
        --position;
        while (position != 0 && chars[position] == L' ')
        {
            --position;
        }

        const auto dc = DelimiterClass(chars[position]);
        while (position != 0 && DelimiterClass(chars[position - 1]) == dc)
        {
            --position;
        }
    }
    return position;
}

size_t COOKED_READ_DATA::_wordNext(const std::wstring_view& chars, size_t position)
{
    if (position < chars.size())
    {
        ++position;
        const auto dc = DelimiterClass(chars[position - 1]);
        while (position != chars.size() && dc == DelimiterClass(chars[position]))
        {
            ++position;
        }
        while (position != chars.size() && chars[position] == L' ')
        {
            ++position;
        }
    }
    return position;
}

// Reads text off of the InputBuffer and dispatches it to the current popup or otherwise into the _buffer contents.
void COOKED_READ_DATA::_readCharInputLoop()
{
    while (_state == State::Accumulating)
    {
        const auto hasPopup = !_popups.empty();
        auto charOrVkey = UNICODE_NULL;
        auto commandLineEditingKeys = false;
        auto popupKeys = false;
        const auto pCommandLineEditingKeys = hasPopup ? nullptr : &commandLineEditingKeys;
        const auto pPopupKeys = hasPopup ? &popupKeys : nullptr;
        DWORD modifiers = 0;

        const auto status = GetChar(_pInputBuffer, &charOrVkey, true, pCommandLineEditingKeys, pPopupKeys, &modifiers);
        if (status == CONSOLE_STATUS_WAIT)
        {
            break;
        }
        THROW_IF_NTSTATUS_FAILED(status);

        if (hasPopup)
        {
            const auto wch = static_cast<wchar_t>(popupKeys ? 0 : charOrVkey);
            const auto vkey = static_cast<uint16_t>(popupKeys ? charOrVkey : 0);
            _popupHandleInput(wch, vkey, modifiers);
        }
        else
        {
            if (commandLineEditingKeys)
            {
                _handleVkey(charOrVkey, modifiers);
            }
            else
            {
                _handleChar(charOrVkey, modifiers);
            }
        }
    }
}

// Handles character input for _readCharInputLoop() when no popups exist.
void COOKED_READ_DATA::_handleChar(wchar_t wch, const DWORD modifiers)
{
    // All paths in this function modify the buffer.

    if (_ctrlWakeupMask != 0 && wch < L' ' && (_ctrlWakeupMask & (1 << wch)))
    {
        // The old implementation (all the way since the 90s) overwrote the character at the current cursor position with the given wch.
        // But simultaneously it incremented the buffer length, which would have only worked if it was written at the end of the buffer.
        // Press tab past the "f" in the string "foo" and you'd get "f\to " (a trailing whitespace; the initial contents of the buffer back then).
        // It's unclear whether the original intention was to write at the end of the buffer at all times or to implement an insert mode.
        // I went with insert mode.
        //
        // The old implementation also failed to clear the end of the prompt if you pressed tab in the middle of it.
        // You can reproduce this issue by launching cmd in an old conhost build and writing "<command that doesn't exist> foo",
        // moving your cursor to the space past the <command> and pressing tab. Nothing will happen but the "foo" will be inaccessible.
        // I've now fixed this behavior by adding an additional Replace() before the _flushBuffer() call that removes the tail end.
        //
        // It is important that we don't actually print that character out though, as it's only for the calling application to see.
        // That's why we flush the contents before the insertion and then ensure that the _flushBuffer() call in Read() exits early.
        _replace(_bufferCursor, npos, nullptr, 0);
        _dirty = true;
        _flushBuffer();
        _replace(_bufferCursor, 0, &wch, 1);
        _dirty = false;

        _controlKeyState = modifiers;
        _transitionState(State::DoneWithWakeupMask);
        return;
    }

    switch (wch)
    {
    case UNICODE_CARRIAGERETURN:
    {
        // NOTE: Don't append newlines to the buffer just yet! See _handlePostCharInputLoop for more information.
        _setCursorPosition(npos);
        _transitionState(State::DoneWithCarriageReturn);
        return;
    }
    case EXTKEY_ERASE_PREV_WORD: // Ctrl+Backspace
    case UNICODE_BACKSPACE:
        if (WI_IsFlagSet(_pInputBuffer->InputMode, ENABLE_PROCESSED_INPUT))
        {
            const auto cursor = _bufferCursor;
            const auto pos = wch == EXTKEY_ERASE_PREV_WORD ? _wordPrev(_buffer, cursor) : TextBuffer::GraphemePrev(_buffer, cursor);
            _replace(pos, cursor - pos, nullptr, 0);
            return;
        }
        // If processed mode is disabled, control characters like backspace are treated like any other character.
        break;
    default:
        break;
    }

    size_t remove = 0;
    if (!_insertMode)
    {
        // TODO GH#15875: If the input grapheme is >1 char, then this will replace >1 grapheme
        // --> We should accumulate input text as much as possible and then call _processInput with wstring_view.
        const auto cursor = _bufferCursor;
        remove = TextBuffer::GraphemeNext(_buffer, cursor) - cursor;
    }

    _replace(_bufferCursor, remove, &wch, 1);
}

// Handles non-character input for _readCharInputLoop() when no popups exist.
void COOKED_READ_DATA::_handleVkey(uint16_t vkey, DWORD modifiers)
{
    const auto ctrlPressed = WI_IsAnyFlagSet(modifiers, LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
    const auto altPressed = WI_IsAnyFlagSet(modifiers, LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);

    switch (vkey)
    {
    case VK_ESCAPE:
        if (!_buffer.empty())
        {
            _replace(0, npos, nullptr, 0);
        }
        break;
    case VK_HOME:
        if (_bufferCursor > 0)
        {
            if (ctrlPressed)
            {
                _replace(0, _bufferCursor, nullptr, 0);
            }
            _setCursorPosition(0);
        }
        break;
    case VK_END:
        if (_bufferCursor < _buffer.size())
        {
            if (ctrlPressed)
            {
                _replace(_bufferCursor, npos, nullptr, 0);
            }
            _setCursorPosition(npos);
        }
        break;
    case VK_LEFT:
        if (_bufferCursor != 0)
        {
            if (ctrlPressed)
            {
                _setCursorPosition(_wordPrev(_buffer, _bufferCursor));
            }
            else
            {
                _setCursorPosition(TextBuffer::GraphemePrev(_buffer, _bufferCursor));
            }
        }
        break;
    case VK_F1:
    case VK_RIGHT:
        if (_bufferCursor != _buffer.size())
        {
            if (ctrlPressed && vkey == VK_RIGHT)
            {
                _setCursorPosition(_wordNext(_buffer, _bufferCursor));
            }
            else
            {
                _setCursorPosition(TextBuffer::GraphemeNext(_buffer, _bufferCursor));
            }
        }
        else if (_history)
        {
            // Traditionally pressing right at the end of an input line would paste characters from the previous command.
            const auto cmd = _history->GetLastCommand();
            const auto bufferSize = _buffer.size();
            const auto cmdSize = cmd.size();
            size_t bufferBeg = 0;
            size_t cmdBeg = 0;

            // We cannot just check if the cmd is longer than the _buffer, because we want to copy graphemes,
            // not characters and there's no correlation between the number of graphemes and their byte length.
            while (cmdBeg < cmdSize)
            {
                const auto cmdEnd = TextBuffer::GraphemeNext(cmd, cmdBeg);

                if (bufferBeg >= bufferSize)
                {
                    _replace(npos, 0, cmd.data() + cmdBeg, cmdEnd - cmdBeg);
                    break;
                }

                bufferBeg = TextBuffer::GraphemeNext(_buffer, bufferBeg);
                cmdBeg = cmdEnd;
            }
        }
        break;
    case VK_INSERT:
        _insertMode = !_insertMode;
        _screenInfo.SetCursorDBMode(_insertMode != ServiceLocator::LocateGlobals().getConsoleInformation().GetInsertMode());
        break;
    case VK_DELETE:
        if (_bufferCursor < _buffer.size())
        {
            const auto beg = _bufferCursor;
            const auto end = TextBuffer::GraphemeNext(_buffer, beg);
            _replace(beg, end - beg, nullptr, 0);
        }
        break;
    case VK_UP:
    case VK_F5:
        if (_history && !_history->AtFirstCommand())
        {
            _replace(_history->Retrieve(CommandHistory::SearchDirection::Previous));
        }
        break;
    case VK_DOWN:
        if (_history && !_history->AtLastCommand())
        {
            _replace(_history->Retrieve(CommandHistory::SearchDirection::Next));
        }
        break;
    case VK_PRIOR:
        if (_history && !_history->AtFirstCommand())
        {
            _replace(_history->RetrieveNth(0));
        }
        break;
    case VK_NEXT:
        if (_history && !_history->AtLastCommand())
        {
            _replace(_history->RetrieveNth(INT_MAX));
        }
        break;
    case VK_F2:
        if (_history)
        {
            _popupPush(PopupKind::CopyToChar);
        }
        break;
    case VK_F3:
        if (_history)
        {
            const auto last = _history->GetLastCommand();
            if (last.size() > _bufferCursor)
            {
                const auto count = last.size() - _bufferCursor;
                _replace(_bufferCursor, npos, last.data() + _bufferCursor, count);
            }
        }
        break;
    case VK_F4:
        // Historically the CopyFromChar popup was constrained to only work when a history exists,
        // but I don't see why that should be. It doesn't depend on _history at all.
        _popupPush(PopupKind::CopyFromChar);
        break;
    case VK_F6:
        // Don't ask me why but F6 is an alias for ^Z.
        _handleChar(0x1a, modifiers);
        break;
    case VK_F7:
        if (!ctrlPressed && !altPressed)
        {
            if (_history && _history->GetNumberOfCommands())
            {
                _popupPush(PopupKind::CommandList);
            }
        }
        else if (altPressed)
        {
            if (_history)
            {
                _history->Empty();
                _history->Flags |= CommandHistory::CLE_ALLOCATED;
            }
        }
        break;
    case VK_F8:
        if (_history)
        {
            CommandHistory::Index index = 0;
            const auto cursorPos = _bufferCursor;
            const auto prefix = std::wstring_view{ _buffer }.substr(0, cursorPos);
            if (_history->FindMatchingCommand(prefix, _history->LastDisplayed, index, CommandHistory::MatchOptions::None))
            {
                _replace(_history->RetrieveNth(index));
                _setCursorPosition(cursorPos);
            }
        }
        break;
    case VK_F9:
        if (_history && _history->GetNumberOfCommands())
        {
            _popupPush(PopupKind::CommandNumber);
        }
        break;
    case VK_F10:
        // Alt+F10 clears the aliases for specifically cmd.exe.
        if (altPressed)
        {
            Alias::s_ClearCmdExeAliases();
        }
        break;
    default:
        assert(false); // Unrecognized VK. Fix or don't call this function?
        break;
    }
}

// Handles any tasks that need to be completed after the read input loop finishes,
// like handling doskey aliases and converting the input to non-UTF16.
void COOKED_READ_DATA::_handlePostCharInputLoop(const bool isUnicode, size_t& numBytes, ULONG& controlKeyState)
{
    auto writer = _userBuffer;
    auto buffer = std::move(_buffer);
    std::wstring_view input{ buffer };
    size_t lineCount = 1;

    if (_state == State::DoneWithCarriageReturn)
    {
        static constexpr std::wstring_view cr{ L"\r" };
        static constexpr std::wstring_view crlf{ L"\r\n" };
        const auto newlineSuffix = WI_IsFlagSet(_pInputBuffer->InputMode, ENABLE_PROCESSED_INPUT) ? crlf : cr;
        std::wstring alias;

        // Here's why we can't easily use _flushBuffer() to handle newlines:
        //
        // A carriage return (enter key) will increase the _distanceEnd by up to viewport-width many columns,
        // since it increases the Y distance between the start and end by 1 (it's a newline after all).
        // This will make _flushBuffer() think that the new _buffer is way longer than the old one and so
        // _erase() ends up not erasing the tail end of the prompt, even if the new prompt is actually shorter.
        //
        // If you were to break this (remove this code and then append \r\n in _handleChar())
        // you can reproduce the issue easily if you do this:
        // * Run cmd.exe
        // * Write "echo hello" and press Enter
        // * Write "foobar foo bar" (don't press Enter)
        // * Press F7, select "echo hello" and press Enter
        //
        // It'll print "hello" but the previous prompt will say "echo hello bar" because the _distanceEnd
        // ended up being well over 14 leading it to believe that "bar" got overwritten during WriteCharsLegacy().

        WriteCharsLegacy(_screenInfo, newlineSuffix, nullptr);

        if (WI_IsFlagSet(_pInputBuffer->InputMode, ENABLE_ECHO_INPUT))
        {
            if (_history)
            {
                auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
                LOG_IF_FAILED(_history->Add(input, WI_IsFlagSet(gci.Flags, CONSOLE_HISTORY_NODUP)));
            }

            Tracing::s_TraceCookedRead(_processHandle, input);
            alias = Alias::s_MatchAndCopyAlias(input, _exeName, lineCount);
        }

        if (!alias.empty())
        {
            buffer = std::move(alias);
        }
        else
        {
            buffer.append(newlineSuffix);
        }

        input = std::wstring_view{ buffer };

        // doskey aliases may result in multiple lines of output (for instance `doskey test=echo foo$Techo bar$Techo baz`).
        // We need to emit them as multiple cooked reads as well, so that each read completes at a \r\n.
        if (lineCount > 1)
        {
            // ProcessAliases() is supposed to end each line with \r\n. If it doesn't we might as well fail-fast.
            const auto firstLineEnd = input.find(UNICODE_LINEFEED) + 1;
            input = input.substr(0, std::min(input.size(), firstLineEnd));
        }
    }

    const auto inputSizeBefore = input.size();
    _pInputBuffer->Consume(isUnicode, input, writer);

    if (lineCount > 1)
    {
        // This is a continuation of the above identical if condition.
        // We've truncated the `input` slice and now we need to restore it.
        const auto inputSizeAfter = input.size();
        const auto amountConsumed = inputSizeBefore - inputSizeAfter;
        input = std::wstring_view{ buffer };
        input = input.substr(std::min(input.size(), amountConsumed));
        GetInputReadHandleData()->SaveMultilinePendingInput(input);
    }
    else if (!input.empty())
    {
        GetInputReadHandleData()->SavePendingInput(input);
    }

    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.Flags |= CONSOLE_IGNORE_NEXT_KEYUP;

    // If we previously called SetCursorDBMode() with true,
    // this will ensure that the cursor returns to its normal look.
    _screenInfo.SetCursorDBMode(false);

    numBytes = _userBuffer.size() - writer.size();
    controlKeyState = _controlKeyState;
}

void COOKED_READ_DATA::_transitionState(State state) noexcept
{
    assert(_state == State::Accumulating);
    _state = state;
}

void COOKED_READ_DATA::_replace(size_t offset, size_t remove, const wchar_t* input, size_t count)
{
    const auto size = _buffer.size();
    offset = std::min(offset, size);
    remove = std::min(remove, size - offset);

    _buffer.replace(offset, remove, input, count);
    _bufferCursor = offset + count;
    _bufferDirtyBeg = std::min(_bufferDirtyBeg, offset);
    _dirty = true;
}

void COOKED_READ_DATA::_replace(const std::wstring_view& str)
{
    _buffer.assign(str);
    _bufferCursor = _buffer.size();
    _bufferDirtyBeg = 0;
    _dirty = true;
}

void COOKED_READ_DATA::_setCursorPosition(size_t position) noexcept
{
    _bufferCursor = std::min(position, _buffer.size());
    _dirty = true;
}

std::wstring_view COOKED_READ_DATA::_slice(size_t from, size_t to) const noexcept
{
    to = std::min(to, _buffer.size());
    from = std::min(from, to);
    return std::wstring_view{ _buffer.data() + from, to - from };
}

// Draws the contents of _buffer onto the screen.
//
// By using _bufferDirtyBeg to avoid redrawing the buffer unless needed, we turn the amortized
// time complexity of _readCharInputLoop() from O(n^2) (n(n+1)/2 redraws) into O(n).
// Pasting text would quickly turn into "accidentally quadratic" meme material otherwise.
//
// NOTE: Don't call _flushBuffer() after appending newlines to the buffer! See _handlePostCharInputLoop for more information.
void COOKED_READ_DATA::_flushBuffer()
{
    if (!_dirty || WI_IsFlagClear(_pInputBuffer->InputMode, ENABLE_ECHO_INPUT))
    {
        return;
    }

    const auto size = _screenInfo.GetVirtualViewport().Dimensions();
    auto originInViewport = _originInViewport;
    til::point cursorPosition;
    til::point dirtyBegPosition;
    til::point pagerEnd;
    std::vector<Line> lines;

    // FYI: This loop does not loop. It exists because goto is considered evil
    // and if MSVC says that then that must be true.
    for (;;)
    {
        cursorPosition = { _originInViewport.x, 0 };
        dirtyBegPosition = cursorPosition;

        const size_t offsets[]{
            0,
            std::min(_bufferDirtyBeg, _bufferCursor),
            std::max(_bufferDirtyBeg, _bufferCursor),
            npos,
        };

        LayoutResult res{ .column = cursorPosition.x };
        lines.emplace_back().columns = cursorPosition.x;

        for (int i = 0; i < 3; i++)
        {
            const auto& segment = til::safe_slice_abs(_buffer, offsets[i], offsets[i + 1]);
            if (segment.empty())
            {
                continue;
            }

            const auto dirty = offsets[i] >= _bufferDirtyBeg;

            // Layout the _buffer contents into lines.
            for (size_t beg = 0; beg < segment.size();)
            {
                if (res.column >= size.width)
                {
                    lines.emplace_back();
                }

                auto& line = lines.back();

                res = _layoutLine(line.text, segment, beg, line.columns, size.width);

                line.dirtyBeg = dirty ? line.dirtyBeg : line.text.size();
                line.columns = res.column;

                beg = res.offset;
            }

            const til::point pos{ res.column, gsl::narrow_cast<til::CoordType>(lines.size() - 1) };
            const auto endOffset = offsets[i + 1];
            if (endOffset == _bufferCursor)
            {
                cursorPosition = pos;
            }
            if (endOffset == _bufferDirtyBeg)
            {
                dirtyBegPosition = pos;
            }
        }

        pagerEnd = { res.column, gsl::narrow_cast<til::CoordType>(lines.size() - 1) };
        

        // bbbbbbbbbbbbbbbbbbb aaaaaaaaaaaaaaaaaaaaaa
        {
            if (pagerEnd.y <= _pagerEnd.y)
            {
                auto& line = lines.back();
                const auto endX = _pagerEnd.y == pagerEnd.y ? _pagerEnd.x : size.width;
                const auto remaining = endX - line.columns;
                if (remaining > 0)
                {
                    // CSI K may be expensive, so use spaces if we can.
                    if (remaining <= 8)
                    {
                        line.text.append(remaining, L' ');
                    }
                    else
                    {
                        line.text.append(csi("K"));
                    }
                }
            }
        }

        // Render the popups, if there are any.
        if (!_popups.empty())
        {
            auto& popup = _popups.back();

            // But it should not be right on the line where the prompt starts.
            // That would look goofy otherwise, since there's where the text is supposed to go.
            if (lines.empty())
            {
                lines.emplace_back();
            }

            // Ensure that the popup is not considered part of the prompt line. That is, if someone double-clicks
            // to select the last word in the prompt, it should not select the first word in the popup.
            lines.back().forceWrap = true;

            switch (popup.kind)
            {
            case PopupKind::CopyToChar:
                _popupDrawPrompt(lines, size, ID_CONSOLE_MSGCMDLINEF2, {});
                break;
            case PopupKind::CopyFromChar:
                _popupDrawPrompt(lines, size, ID_CONSOLE_MSGCMDLINEF4, {});
                break;
            case PopupKind::CommandNumber:
                _popupDrawPrompt(lines, size, ID_CONSOLE_MSGCMDLINEF9, { popup.commandNumber.buffer.data(), CommandNumberMaxInputLength });
                break;
            case PopupKind::CommandList:
                _popupDrawCommandList(lines, size, popup);
                break;
            default:
                assert(false);
            }

            // Put the cursor at the end of the contents. This ensures we scroll all the way down.
            cursorPosition.x = lines.back().columns;
            cursorPosition.y = gsl::narrow_cast<til::CoordType>(lines.size()) - 1;
        }
        // If the cursor is at a delay-wrapped position, wrap it explicitly.
        // This ensures that the cursor is always "after" the insertion position.
        // We don't need to do this when popups are present, because they're not supposed to end in a newline.
        else if (cursorPosition.x >= size.width)
        {
            cursorPosition.x = 0;
            cursorPosition.y++;

            // If the cursor is at the end of the buffer however, we have to insert
            // a line because otherwise it won't have any space to be visible.
            if (_bufferCursor == _buffer.size())
            {
                lines.emplace_back(L" \b");
            }
        }

        if (dirtyBegPosition.x >= size.width)
        {
            dirtyBegPosition.x = 0;
            dirtyBegPosition.y++;
        }

        // Usually we'll be on a "prompt> ..." line and behave like a regular single-line-editor.
        // But once the entire viewport is full of text, we need to behave more like a pager (= scrolling, etc.).
        // This code retries the layout process if needed, because then the cursor starts at origin {0, 0}.
        if (gsl::narrow_cast<til::CoordType>(lines.size()) > size.height && originInViewport.x != 0)
        {
            lines.clear();
            _originInViewport.x = 0;
            _bufferDirtyBeg = 0;
            originInViewport = {};
            continue;
        }

        break;
    }

    const auto lineCount = gsl::narrow_cast<til::CoordType>(lines.size());
    const auto pagerHeight = std::min(lineCount, size.height);

    // If the contents of the prompt are longer than the remaining number of lines in the viewport,
    // we need to reduce originInViewportNext.y towards 0 to account for that. In other words,
    // as the viewport fills itself with text the _originInViewport will slowly move towards 0.
    originInViewport.y = std::min(originInViewport.y, size.height - pagerHeight);

    auto pagerTop = _pagerTop;
    // If the cursor is above the viewport, we go up...
    pagerTop = std::min(pagerTop, cursorPosition.y);
    // and if the cursor is below it, we go down.
    pagerTop = std::max(pagerTop, cursorPosition.y - size.height + 1);
    // The value may be out of bounds, because the above min/max doesn't ensure this on its own.
    pagerTop = std::clamp(pagerTop, 0, lineCount - pagerHeight);

    // Transform the recorded position from the lines vector coordinate space into VT screen space.
    // Due to the above scrolling of pagerTop, cursorPosition should now always be within the viewport.
    // dirtyBegPosition however could be outside of it.
    cursorPosition.y += originInViewport.y - pagerTop;
    dirtyBegPosition.y += originInViewport.y - pagerTop;

    std::wstring output;

    // Disable the cursor when opening a popup, reenable it when closing them.
    if (const auto popupOpened = !_popups.empty(); _popupOpened != popupOpened)
    {
        wchar_t buf[] = L"\x1b[?25l";
        buf[5] = popupOpened ? 'l' : 'h';
        output.append(&buf[0], 6);
        _popupOpened = popupOpened;
    }

    auto anyDirty = _bufferDirtyBeg != npos;
    if (const auto delta = pagerTop - _pagerTop; delta != 0)
    {
        const auto deltaAbs = abs(delta);
        til::CoordType beg = 0;
        til::CoordType end = pagerHeight;

        if (deltaAbs < size.height)
        {
            beg = delta >= 0 ? pagerHeight - deltaAbs : 0;
            end = delta >= 0 ? pagerHeight : deltaAbs;
            const auto cmd = delta >= 0 ? L'S' : L'T';
            fmt::format_to(std::back_inserter(output), FMT_COMPILE(L"\x1b[H\x1b[{}{}"), deltaAbs, cmd);
        }

        for (auto i = beg; i < end; i++)
        {
            auto& line = lines.at(i + pagerTop);
            line.dirtyBeg = 0;
        }

        dirtyBegPosition = { 0, beg };
        anyDirty = true;
    }

    if (anyDirty)
    {
        if (dirtyBegPosition.x < 0 && dirtyBegPosition.x >= size.width &&
            dirtyBegPosition.y < 0 && dirtyBegPosition.y >= size.height)
        {
            dirtyBegPosition = {};
        }

        // Restore the cursor position where we previously stopped writing text.
        _appendCUP(output, dirtyBegPosition);

        constexpr uint32_t dark2[]{
            0x1b9e77,
            0xd95f02,
            0x7570b3,
            0xe7298a,
            0x66a61e,
            0xe6ab02,
            0xa6761d,
            0x666666,
        };
        static size_t debugColorIndex = 0;
        const auto color = dark2[++debugColorIndex % std::size(dark2)];

        // Accumulate all affected lines in a single string.
        for (til::CoordType i = 0; i < pagerHeight; i++)
        {
            auto& line = lines.at(i + pagerTop);
            if (line.dirtyBeg >= line.text.size())
            {
                continue;
            }

            fmt::format_to(std::back_inserter(output), FMT_COMPILE(L"\x1b[48;2;{};{};{}m"), GetRValue(color), GetGValue(color), GetBValue(color));

            output.append(line.text, line.dirtyBeg);

            output.append(L"\x1b[m");

            // Since the line ended in the middle of the viewport and because
            // we have lines after this one, we need to insert a line break.
            if (line.forceWrap)
            {
                output.append(L"\r\n");
            }
        }

        // Clear any lines that we previously filled and are now empty.
        {
            const auto pagerHeightPrevious = std::min(_pagerEnd.y + 1, size.height);
            for (til::CoordType i = pagerHeight; i < pagerHeightPrevious; i++)
            {
                if (i != 0)
                {
                    output.append(csi("E"));
                }
                output.append(csi("K"));
            }
        }
    }

    _appendCUP(output, cursorPosition);
    WriteCharsVT(_screenInfo, output);

    _originInViewport = originInViewport;
    _pagerTop = pagerTop;
    _pagerEnd = pagerEnd;
    _bufferDirtyBeg = _buffer.size();
    _dirty = false;
}

COOKED_READ_DATA::LayoutResult COOKED_READ_DATA::_layoutLine(std::wstring& output, const std::wstring_view& input, const size_t inputOffset, const til::CoordType columnBegin, const til::CoordType columnLimit) const
{
    const auto& textBuffer = _screenInfo.GetTextBufferAcceptable();
    const auto beg = input.data();
    const auto end = beg + input.size();
    auto it = beg + std::min(inputOffset, input.size());
    auto column = std::min(columnBegin, columnLimit);

    output.reserve(output.size() + columnLimit - column);

    while (it != end)
    {
        const auto nextControlChar = std::find_if(it, end, [](const auto& wch) { return wch < L' '; });
        if (it != nextControlChar)
        {
            std::wstring_view text{ it, nextControlChar };
            til::CoordType cols = 0;
            const auto len = textBuffer.FitTextIntoColumns(text, columnLimit - column, cols);

            output.append(text, 0, len);
            column += cols;
            it += len;

            if (len < text.size() || it == end)
            {
                break;
            }
        }

        const auto wch = *it++;
        wchar_t buf[8];
        til::CoordType len = 0;

        if (wch == UNICODE_TAB)
        {
            const auto remaining = columnLimit - column;
            len = std::min(8 - (column & 7), remaining);
            std::fill_n(&buf[0], len, L' ');
        }
        else
        {
            buf[0] = L'^';
            buf[1] = wch + L'@';
            len = 2;
        }

        if (column + len <= columnLimit)
        {
            column += len;
            output.append(buf, len);
        }
    }

    const size_t offset = it - beg;

    if (offset < input.size() && column < columnLimit)
    {
        column = columnLimit;
        output.append(columnLimit - column, L' ');
    }

    return {
        .offset = offset,
        .column = column,
    };
}

void COOKED_READ_DATA::_appendCUP(std::wstring& output, til::point pos)
{
    fmt::format_to(std::back_inserter(output), FMT_COMPILE(L"\x1b[{};{}H"), pos.y + 1, pos.x + 1);
}

void COOKED_READ_DATA::_appendPopupAttr(std::wstring& output) const
{
    VtIo::FormatAttributes(output, _screenInfo.GetPopupAttributes().GetLegacyAttributes());
}

void COOKED_READ_DATA::_popupPush(const PopupKind kind)
try
{
    auto& popup = _popups.emplace_back(kind);
    _dirty = true;

    switch (kind)
    {
    case PopupKind::CommandNumber:
        popup.commandNumber.buffer.fill(' ');
        popup.commandNumber.bufferSize = 0;
        break;
    case PopupKind::CommandList:
        popup.commandList.top = -1;
        popup.commandList.height = 10;
        popup.commandList.selected = _history->LastDisplayed;
        break;
    default:
        break;
    }
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    _popupsDone();
}

// Dismisses all current popups at once. Right now we don't need support for just dismissing the topmost popup.
// In fact, there's only a single situation right now where there can be >1 popup:
// Pressing F7 followed by F9 (CommandNumber on top of CommandList).
void COOKED_READ_DATA::_popupsDone()
{
    _popups.clear();
    _dirty = true;
}

void COOKED_READ_DATA::_popupHandleInput(wchar_t wch, uint16_t vkey, DWORD modifiers)
{
    if (_popups.empty())
    {
        assert(false); // Don't call this function.
        return;
    }

    auto& popup = _popups.back();
    switch (popup.kind)
    {
    case PopupKind::CopyToChar:
        _popupHandleCopyToCharInput(popup, wch, vkey, modifiers);
        break;
    case PopupKind::CopyFromChar:
        _popupHandleCopyFromCharInput(popup, wch, vkey, modifiers);
        break;
    case PopupKind::CommandNumber:
        _popupHandleCommandNumberInput(popup, wch, vkey, modifiers);
        break;
    case PopupKind::CommandList:
        _popupHandleCommandListInput(popup, wch, vkey, modifiers);
        break;
    default:
        break;
    }
}

void COOKED_READ_DATA::_popupHandleCopyToCharInput(Popup& /*popup*/, const wchar_t wch, const uint16_t vkey, const DWORD /*modifiers*/)
{
    if (vkey)
    {
        if (vkey == VK_ESCAPE)
        {
            _popupsDone();
        }
    }
    else
    {
        // See PopupKind::CopyToChar for more information about this code.
        const auto cmd = _history->GetLastCommand();
        const auto cursor = _bufferCursor;
        const auto idx = cmd.find(wch, cursor);

        if (idx != decltype(cmd)::npos)
        {
            // When we enter this if condition it's guaranteed that _bufferCursor must be
            // smaller than idx, which in turn implies that it's smaller than cmd.size().
            // As such, calculating length is safe and str.size() == length.
            const auto count = idx - cursor;
            _replace(cursor, count, cmd.data() + cursor, count);
        }

        _popupsDone();
    }
}

void COOKED_READ_DATA::_popupHandleCopyFromCharInput(Popup& /*popup*/, const wchar_t wch, const uint16_t vkey, const DWORD /*modifiers*/)
{
    if (vkey)
    {
        if (vkey == VK_ESCAPE)
        {
            _popupsDone();
        }
    }
    else
    {
        // See PopupKind::CopyFromChar for more information about this code.
        const auto cursor = _bufferCursor;
        auto idx = _buffer.find(wch, cursor);
        idx = std::min(idx, _buffer.size());
        _replace(cursor, idx - cursor, nullptr, 0);
        _popupsDone();
    }
}

void COOKED_READ_DATA::_popupHandleCommandNumberInput(Popup& popup, const wchar_t wch, const uint16_t vkey, const DWORD /*modifiers*/)
{
    if (vkey)
    {
        if (vkey == VK_ESCAPE)
        {
            _popupsDone();
        }
    }
    else
    {
        if (wch == UNICODE_CARRIAGERETURN)
        {
            popup.commandNumber.buffer[popup.commandNumber.bufferSize++] = L'\0';
            _replace(_history->RetrieveNth(std::stoi(popup.commandNumber.buffer.data())));
            _popupsDone();
        }
        else if (wch >= L'0' && wch <= L'9')
        {
            if (popup.commandNumber.bufferSize < CommandNumberMaxInputLength)
            {
                popup.commandNumber.buffer[popup.commandNumber.bufferSize++] = wch;
                _dirty = true;
            }
        }
        else if (wch == UNICODE_BACKSPACE)
        {
            if (popup.commandNumber.bufferSize > 0)
            {
                popup.commandNumber.buffer[--popup.commandNumber.bufferSize] = L' ';
                _dirty = true;
            }
        }
    }
}

void COOKED_READ_DATA::_popupHandleCommandListInput(Popup& popup, const wchar_t wch, const uint16_t vkey, const DWORD modifiers)
{
    auto& cl = popup.commandList;

    if (wch == UNICODE_CARRIAGERETURN)
    {
        _replace(_history->RetrieveNth(cl.selected));
        _popupsDone();
        _handleChar(UNICODE_CARRIAGERETURN, modifiers);
        return;
    }

    switch (vkey)
    {
    case VK_ESCAPE:
        _popupsDone();
        return;
    case VK_F9:
        _popupPush(PopupKind::CommandNumber);
        return;
    case VK_DELETE:
        _history->Remove(cl.selected);
        if (_history->GetNumberOfCommands() <= 0)
        {
            _popupsDone();
            return;
        }
        break;
    case VK_LEFT:
    case VK_RIGHT:
        _replace(_history->RetrieveNth(cl.selected));
        _popupsDone();
        return;
    case VK_UP:
        if (WI_IsFlagSet(modifiers, SHIFT_PRESSED))
        {
            _history->Swap(cl.selected, cl.selected - 1);
        }
        // _popupDrawCommandList() clamps all values to valid ranges in `cl`.
        cl.selected--;
        break;
    case VK_DOWN:
        if (WI_IsFlagSet(modifiers, SHIFT_PRESSED))
        {
            _history->Swap(cl.selected, cl.selected + 1);
        }
        // _popupDrawCommandList() clamps all values to valid ranges in `cl`.
        cl.selected++;
        break;
    case VK_HOME:
        cl.selected = 0;
        break;
    case VK_END:
        // _popupDrawCommandList() clamps all values to valid ranges in `cl`.
        cl.selected = INT_MAX;
        break;
    case VK_PRIOR:
        // _popupDrawCommandList() clamps all values to valid ranges in `cl`.
        cl.selected -= cl.height;
        break;
    case VK_NEXT:
        // _popupDrawCommandList() clamps all values to valid ranges in `cl`.
        cl.selected += cl.height;
        break;
    default:
        return;
    }

    _dirty = true;
}

void COOKED_READ_DATA::_popupDrawPrompt(std::vector<Line>& lines, const til::size size, const UINT id, const std::wstring_view suffix) const
{
    auto str = _LoadString(id);
    str.append(suffix);

    std::wstring line;
    _appendPopupAttr(line);
    const auto res = _layoutLine(line, str, 0, 0, size.width);
    line.append(csi("m"));

    lines.emplace_back(std::move(line), 0, res.column);
}

void COOKED_READ_DATA::_popupDrawCommandList(std::vector<Line>& lines, const til::size size, Popup& popup) const
{
    assert(popup.kind == PopupKind::CommandList);

    auto& cl = popup.commandList;
    const auto historySize = _history->GetNumberOfCommands();
    const auto indexWidth = gsl::narrow_cast<til::CoordType>(fmt::formatted_size(FMT_COMPILE(L"{}"), historySize));

    // The popup is half the height of the viewport, but at least 1 and at most 20 lines.
    // Unless of course the history size is less than that.
    const auto height = std::min(historySize, std::clamp(size.height / 2, 1, 20));

    // cl.selected may be out of bounds after a page up/down, etc., so we need to clamp it.
    cl.selected = std::clamp(cl.selected, 0, historySize - 1);

    // If it hasn't been initialized it yet, center the selected item.
    if (cl.top < 0)
    {
        cl.top = std::max(0, cl.selected - height / 2);
    }

    // If the selection is above the viewport, we go up...
    cl.top = std::min(cl.top, cl.selected);
    // and if the selection is below it, we go down.
    cl.top = std::max(cl.top, cl.selected - height + 1);
    // The value may be out of bounds, because the above min/max doesn't ensure this on its own.
    cl.top = std::clamp(cl.top, 0, historySize - height);

    // We also need to update the height for future page up/down movements.
    cl.height = height;

    // Calculate the position of the █ track in the scrollbar among all the ▒.
    // The position is offset by +1 because at off == 0 we draw the ▲.
    // We add historyMax/2 to round the division result to the nearest value.
    const auto historyMax = historySize - 1;
    const auto trackPositionMax = height - 3;
    const auto trackPosition = historyMax <= 0 ? 0 : 1 + (trackPositionMax * cl.selected + historyMax / 2) / historyMax;

    for (til::CoordType off = 0; off < height; ++off)
    {
        const auto index = cl.top + off;
        const auto str = _history->GetNth(index);
        const auto selected = index == cl.selected;

        std::wstring line;
        _appendPopupAttr(line);

        wchar_t scrollbarChar = L' ';
        if (historySize > height)
        {
            if (off == 0)
            {
                scrollbarChar = L'▲';
            }
            else if (off == height - 1)
            {
                scrollbarChar = L'▼';
            }
            else
            {
                scrollbarChar = off == trackPosition ? L'█' : L'▒';
            }
        }
        line.push_back(scrollbarChar);

        if (selected)
        {
            line.push_back(L'▸');
        }
        else
        {
            line.append(csi("m") " ");
        }

        fmt::format_to(std::back_inserter(line), FMT_COMPILE(L"{:{}}: "), index, indexWidth);

        auto res = _layoutLine(line, str, 0, indexWidth + 4, size.width - 1);
        if (res.offset < str.size())
        {
            line.push_back(L'…');
            res.column++;
        }

        if (selected)
        {
            line.append(csi("m"));
        }

        lines.emplace_back(std::move(line), 0, res.column, size.width, true);
    }

    lines.back().forceWrap = false;
}
