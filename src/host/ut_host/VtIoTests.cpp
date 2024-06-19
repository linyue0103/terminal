// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "CommonState.hpp"
#include "../VtIo.hpp"
#include "../../interactivity/inc/ServiceLocator.hpp"
#include "../../renderer/base/Renderer.hpp"
#include "../../types/inc/Viewport.hpp"

using namespace WEX::Common;
using namespace WEX::Logging;
using namespace WEX::TestExecution;

using namespace Microsoft::Console::Interactivity;
using namespace Microsoft::Console;
using namespace Microsoft::Console::VirtualTerminal;
using namespace Microsoft::Console::Render;
using namespace Microsoft::Console::Types;

class ::Microsoft::Console::VirtualTerminal::VtIoTests
{
    CommonState commonState;
    ApiRoutines routines;
    VtIo vtio;
    wil::unique_handle rx;

    static constexpr CHAR_INFO red(wchar_t ch) noexcept
    {
        return { ch, FOREGROUND_RED };
    }

    static constexpr CHAR_INFO blu(wchar_t ch) noexcept
    {
        return { ch, FOREGROUND_BLUE };
    }

    static constexpr std::array<CHAR_INFO, 8 * 4> s_initialContent{
        // clang-format off
        red('0'), red('1'), blu('2'), blu('3'), red('4'), red('5'), blu('6'), blu('7'),
        red('8'), red('9'), blu(':'), blu(';'), red('<'), red('='), blu('>'), blu('?'),
        blu('@'), blu('A'), red('B'), red('C'), blu('D'), blu('E'), red('F'), red('G'),
        blu('H'), blu('I'), red('J'), red('K'), blu('L'), blu('M'), red('N'), red('O'),
        // clang-format on
    };

    BEGIN_TEST_CLASS(VtIoTests)
        TEST_CLASS_PROPERTY(L"IsolationLevel", L"Class")
    END_TEST_CLASS()

    TEST_CLASS_SETUP(ClassSetup)
    {
        wil::unique_handle tx;
        VERIFY_WIN32_BOOL_SUCCEEDED(CreatePipe(rx.addressof(), tx.addressof(), nullptr, 4096));

        commonState.PrepareGlobalScreenBuffer(8, 4, 8, 4);

        auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        gci.LockConsole();
        VERIFY_SUCCEEDED(vtio._Initialize(nullptr, tx.release(), nullptr));
        gci.UnlockConsole();

        return false;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        routines.WriteConsoleOutputWImpl(gci.GetActiveOutputBuffer(), {}, );
    }

    TEST_METHOD(BasicAnonymousPipeOpeningWithSignalChannelTest)
    {
        auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        routines.ReadConsoleImpl(gci.GetActiveOutputBuffer(), {}, );
    }
};
