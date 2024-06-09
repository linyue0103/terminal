// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "arena.h"
#include "conhost.h"
#include "utils.h"

#define ENABLE_TEST_WRITE 1
#define ENABLE_TEST_SCROLL 1
#define ENABLE_TEST_READ 1
#define ENABLE_TEST_CLIPBOARD 1

using Measurements = std::span<int32_t>;
using MeasurementsPerBenchmark = std::span<Measurements>;

struct BenchmarkContext
{
    HWND hwnd;
    HANDLE input;
    HANDLE output;
    int64_t time_limit;
    mem::Arena& arena;
    std::string_view utf8_4Ki;
    std::string_view utf8_128Ki;
    std::wstring_view utf16_4Ki;
    std::wstring_view utf16_128Ki;
};

struct Benchmark
{
    const char* title;
    void (*exec)(const BenchmarkContext& ctx, Measurements measurements);
};

struct AccumulatedResults
{
    size_t trace_count;
    // These are arrays of size trace_count
    std::string_view* trace_names;
    MeasurementsPerBenchmark* measurments;
};

constexpr int32_t perf_delta(int64_t beg, int64_t end)
{
    return static_cast<int32_t>(end - beg);
}

constexpr size_t rng(size_t v) noexcept
{
    // These constants are the same as used by the PCG family of random number generators.
    // The 32-Bit version is described in https://doi.org/10.1090/S0025-5718-99-00996-5, Table 5.
    // The 64-Bit version is the multiplier as used by Donald Knuth for MMIX and found by C. E. Haynes.
#ifdef _WIN64
    return v * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
#else
    return v * UINT32_C(747796405) + UINT32_C(2891336453);
#endif
}

static constexpr Benchmark s_benchmarks[] = {
#if ENABLE_TEST_WRITE
    Benchmark{
        .title = "WriteConsoleA 4Ki",
        .exec = [](const BenchmarkContext& ctx, Measurements measurements) {
            for (auto& d : measurements)
            {
                const auto beg = query_perf_counter();
                WriteConsoleA(ctx.output, ctx.utf8_4Ki.data(), static_cast<DWORD>(ctx.utf8_4Ki.size()), nullptr, nullptr);
                const auto end = query_perf_counter();
                d = perf_delta(beg, end);

                if (end >= ctx.time_limit)
                {
                    break;
                }
            }
        },
    },
    Benchmark{
        .title = "WriteConsoleW 4Ki",
        .exec = [](const BenchmarkContext& ctx, Measurements measurements) {
            for (auto& d : measurements)
            {
                const auto beg = query_perf_counter();
                WriteConsoleW(ctx.output, ctx.utf16_4Ki.data(), static_cast<DWORD>(ctx.utf16_4Ki.size()), nullptr, nullptr);
                const auto end = query_perf_counter();
                d = perf_delta(beg, end);

                if (end >= ctx.time_limit)
                {
                    break;
                }
            }
        },
    },
    Benchmark{
        .title = "WriteConsoleA 128Ki",
        .exec = [](const BenchmarkContext& ctx, Measurements measurements) {
            for (auto& d : measurements)
            {
                const auto beg = query_perf_counter();
                WriteConsoleA(ctx.output, ctx.utf8_128Ki.data(), static_cast<DWORD>(ctx.utf8_128Ki.size()), nullptr, nullptr);
                const auto end = query_perf_counter();
                d = perf_delta(beg, end);

                if (end >= ctx.time_limit)
                {
                    break;
                }
            }
        },
    },
    Benchmark{
        .title = "WriteConsoleW 128Ki",
        .exec = [](const BenchmarkContext& ctx, Measurements measurements) {
            for (auto& d : measurements)
            {
                const auto beg = query_perf_counter();
                WriteConsoleW(ctx.output, ctx.utf16_128Ki.data(), static_cast<DWORD>(ctx.utf16_128Ki.size()), nullptr, nullptr);
                const auto end = query_perf_counter();
                d = perf_delta(beg, end);

                if (end >= ctx.time_limit)
                {
                    break;
                }
            }
        },
    },
#endif
#if ENABLE_TEST_SCROLL
    Benchmark{
        .title = "ScrollRect 4K",
        .exec = [](const BenchmarkContext& ctx, Measurements measurements) {
            WriteConsoleW(ctx.output, ctx.utf16_128Ki.data(), static_cast<DWORD>(ctx.utf16_128Ki.size()), nullptr, nullptr);

            static constexpr CHAR_INFO fill{ L' ', FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED };
            static constexpr size_t w = 40;
            static constexpr size_t h = 100;
            size_t r = rng(0);

            for (auto& d : measurements)
            {
                r = rng(r);
                const auto srcLeft = (r >> 0) % (120 - w);
                const auto srcTop = (r >> 16) % (9001 - h);

                size_t dstLeft;
                size_t dstTop;
                do
                {
                    r = rng(r);
                    dstLeft = (r >> 0) % (120 - w);
                    dstTop = (r >> 16) % (9001 - h);
                } while (srcLeft == dstLeft && srcTop == dstTop);

                const SMALL_RECT scrollRect{
                    .Left = static_cast<SHORT>(srcLeft),
                    .Top = static_cast<SHORT>(srcTop),
                    .Right = static_cast<SHORT>(srcLeft + w - 1),
                    .Bottom = static_cast<SHORT>(srcTop + h - 1),
                };
                const COORD destOrigin{
                    .X = static_cast<SHORT>(dstLeft),
                    .Y = static_cast<SHORT>(dstTop),
                };

                const auto beg = query_perf_counter();
                ScrollConsoleScreenBufferW(ctx.output, &scrollRect, nullptr, destOrigin, &fill);
                const auto end = query_perf_counter();
                d = perf_delta(beg, end);

                if (end >= ctx.time_limit)
                {
                    break;
                }
            }
        },
    },
    Benchmark{
        .title = "ScrollRect Vertical",
        .exec = [](const BenchmarkContext& ctx, Measurements measurements) {
            WriteConsoleW(ctx.output, ctx.utf16_128Ki.data(), static_cast<DWORD>(ctx.utf16_128Ki.size()), nullptr, nullptr);

            static constexpr CHAR_INFO fill{ L' ', FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED };
            static constexpr size_t w = 120;
            static constexpr size_t h = 33;
            size_t r = rng(0);

            for (auto& d : measurements)
            {
                r = rng(r);
                const auto srcTop = r % (9001 - h);

                size_t dstTop;
                do
                {
                    r = rng(r);
                    dstTop = r % (9001 - h);
                } while (srcTop == dstTop);

                const SMALL_RECT scrollRect{
                    .Left = 0,
                    .Top = static_cast<SHORT>(srcTop),
                    .Right = 119,
                    .Bottom = static_cast<SHORT>(srcTop + h - 1),
                };
                const COORD destOrigin{
                    .X = 0,
                    .Y = static_cast<SHORT>(dstTop),
                };

                const auto beg = query_perf_counter();
                ScrollConsoleScreenBufferW(ctx.output, &scrollRect, nullptr, destOrigin, &fill);
                const auto end = query_perf_counter();
                d = perf_delta(beg, end);

                if (end >= ctx.time_limit)
                {
                    break;
                }
            }
        },
    },
#endif
#if ENABLE_TEST_READ
    Benchmark{
        .title = "ReadConsoleInputW clipboard 4Ki",
        .exec = [](const BenchmarkContext& ctx, Measurements measurements) {
            static constexpr DWORD cap = 16 * 1024;

            const auto scratch = mem::get_scratch_arena(ctx.arena);
            const auto buf = scratch.arena.push_uninitialized<INPUT_RECORD>(cap);
            DWORD read;

            set_clipboard(ctx.hwnd, ctx.utf16_4Ki);
            FlushConsoleInputBuffer(ctx.input);

            for (auto& d : measurements)
            {
                SendMessageW(ctx.hwnd, WM_SYSCOMMAND, 0xFFF1 /* ID_CONSOLE_PASTE */, 0);

                const auto beg = query_perf_counter();
                ReadConsoleInputW(ctx.input, buf, cap, &read);
                debugAssert(read >= 1024 && read < cap);
                const auto end = query_perf_counter();
                d = perf_delta(beg, end);

                if (end >= ctx.time_limit)
                {
                    break;
                }
            }
        },
    },
#endif
#if ENABLE_TEST_CLIPBOARD
    Benchmark{
        .title = "Copy to clipboard 4Ki",
        .exec = [](const BenchmarkContext& ctx, Measurements measurements) {
            WriteConsoleW(ctx.output, ctx.utf16_4Ki.data(), static_cast<DWORD>(ctx.utf8_4Ki.size()), nullptr, nullptr);

            for (auto& d : measurements)
            {
                SendMessageW(ctx.hwnd, WM_SYSCOMMAND, 0xFFF5 /* ID_CONSOLE_SELECTALL */, 0);

                const auto beg = query_perf_counter();
                SendMessageW(ctx.hwnd, WM_SYSCOMMAND, 0xFFF0 /* ID_CONSOLE_COPY */, 0);
                const auto end = query_perf_counter();
                d = perf_delta(beg, end);

                if (end >= ctx.time_limit)
                {
                    break;
                }
            }
        },
    },
    Benchmark{
        .title = "Paste from clipboard 4Ki",
        .exec = [](const BenchmarkContext& ctx, Measurements measurements) {
            set_clipboard(ctx.hwnd, ctx.utf16_4Ki);
            FlushConsoleInputBuffer(ctx.input);

            for (auto& d : measurements)
            {
                const auto beg = query_perf_counter();
                SendMessageW(ctx.hwnd, WM_SYSCOMMAND, 0xFFF1 /* ID_CONSOLE_PASTE */, 0);
                const auto end = query_perf_counter();
                d = perf_delta(beg, end);

                FlushConsoleInputBuffer(ctx.input);

                if (end >= ctx.time_limit)
                {
                    break;
                }
            }
        },
    },
#endif
};
static constexpr size_t s_benchmarks_count = _countof(s_benchmarks);

// Each of these strings is 128 columns.
static constexpr std::string_view payload_utf8{ "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labor眠い子猫はマグロ狩りの夢を見る" };
static constexpr std::wstring_view payload_utf16{ L"Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labor眠い子猫はマグロ狩りの夢を見る" };

static bool print_warning();
static AccumulatedResults* prepare_results(mem::Arena& arena, std::span<const wchar_t*> paths);
static std::span<Measurements> run_benchmarks_for_path(mem::Arena& arena, const wchar_t* path);
static void generate_html(mem::Arena& arena, const AccumulatedResults* results);

int wmain(int argc, const wchar_t* argv[])
try
{
    if (argc < 2)
    {
        mem::print_literal("Usage: %s [paths to conhost.exe]...");
        return 1;
    }

    const auto cp = GetConsoleCP();
    const auto output_cp = GetConsoleOutputCP();
    const auto restore_cp = wil::scope_exit([&]() {
        SetConsoleCP(cp);
        SetConsoleOutputCP(output_cp);
    });
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    const auto scratch = mem::get_scratch_arena();

    const std::span paths{ &argv[1], static_cast<size_t>(argc) - 1 };
    const auto results = prepare_results(scratch.arena, paths);
    if (!results)
    {
        return 1;
    }

    if (!print_warning())
    {
        return 0;
    }

    for (size_t trace_idx = 0; trace_idx < results->trace_count; ++trace_idx)
    {
        const auto title = results->trace_names[trace_idx];
        print_format(scratch.arena, "\r\n# %.*s\r\n", title.size(), title.data());
        results->measurments[trace_idx] = run_benchmarks_for_path(scratch.arena, paths[trace_idx]);
    }

    generate_html(scratch.arena, results);
    return 0;
}
catch (const wil::ResultException& e)
{
    printf("Exception: %08x\n", e.GetErrorCode());
    return 1;
}
catch (...)
{
    printf("Unknown exception\n");
    return 1;
}

static bool print_warning()
{
    mem::print_literal(
        "This will overwrite any existing measurements.html in your current working directory.\r\n"
        "\r\n"
        "For best test results:\r\n"
        "* Make sure your system is fully idle and your CPU cool\r\n"
        "* Move your cursor to a corner of your screen and don't move it over the conhost window(s)\r\n"
        "* Exit or stop any background applications, including Windows Defender (if possible)\r\n"
        "\r\n"
        "Continue? [Yn] ");

    for (;;)
    {
        INPUT_RECORD rec;
        DWORD read;
        if (!ReadConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &rec, 1, &read) || read == 0)
        {
            return false;
        }

        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown)
        {
            // Transforms the character to uppercase if it's lowercase.
            const auto ch = rec.Event.KeyEvent.uChar.UnicodeChar & 0xDF;
            if (ch == L'N')
            {
                return false;
            }
            if (ch == L'\r' || ch == L'Y')
            {
                break;
            }
        }
    }

    mem::print_literal("\r\n");
    return true;
}

static AccumulatedResults* prepare_results(mem::Arena& arena, std::span<const wchar_t*> paths)
{
    for (const auto path : paths)
    {
        const auto attr = GetFileAttributesW(path);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            print_format(arena, "Invalid path: %S\r\n", path);
            return nullptr;
        }
    }

    const auto trace_count = paths.size();
    const auto results = arena.push_zeroed<AccumulatedResults>();

    results->trace_count = trace_count;
    results->trace_names = arena.push_zeroed<std::string_view>(trace_count);
    results->measurments = arena.push_zeroed<MeasurementsPerBenchmark>(trace_count);

    for (size_t trace_idx = 0; trace_idx < trace_count; ++trace_idx)
    {
        const auto path = paths[trace_idx];
        auto trace_name = get_file_version(arena, path);

        if (trace_name.empty())
        {
            const auto end = path + wcsnlen(path, SIZE_MAX);
            auto filename = end;
            for (; filename > path && filename[-1] != L'\\' && filename[-1] != L'/'; --filename)
            {
            }
            trace_name = u16u8(arena, filename, end - filename);
        }

        results->trace_names[trace_idx] = trace_name;
    }

    return results;
}

static void prepare_conhost(const BenchmarkContext& ctx, HWND parent_hwnd)
{
    const auto scratch = mem::get_scratch_arena(ctx.arena);

    SetForegroundWindow(parent_hwnd);

    // Ensure conhost is in a consistent state with identical fonts and window sizes.
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleMode(ctx.output, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    {
        CONSOLE_FONT_INFOEX info{
            .cbSize = sizeof(info),
            .dwFontSize = { 0, 16 },
            .FontFamily = 54,
            .FontWeight = 400,
            .FaceName = L"Consolas",
        };
        SetCurrentConsoleFontEx(ctx.output, FALSE, &info);
    }
    {
        SMALL_RECT info{
            .Left = 0,
            .Top = 0,
            .Right = 119,
            .Bottom = 29,
        };
        SetConsoleScreenBufferSize(ctx.output, { 120, 9001 });
        SetConsoleWindowInfo(ctx.output, TRUE, &info);
    }

    // Ensure conhost's backing TextBuffer is fully committed and initialized. There's currently no way
    // to un-commit it and so not committing it now would be unfair for the first test that runs.
    const auto buf = scratch.arena.push_uninitialized<char>(9001);
    memset(buf, '\n', 9001);
    WriteFile(ctx.output, buf, 9001, nullptr, nullptr);
}

static std::span<Measurements> run_benchmarks_for_path(mem::Arena& arena, const wchar_t* path)
{
    const auto scratch = mem::get_scratch_arena(arena);
    const auto parent_connection = get_active_connection();
    const auto parent_hwnd = GetConsoleWindow();
    const auto freq = query_perf_freq();

    auto handle = spawn_conhost(scratch.arena, path);
    set_active_connection(handle.connection.get());

    const auto print_with_parent_connection = [&](auto&&... args) {
        set_active_connection(parent_connection);
        mem::print_format(scratch.arena, args...);
        set_active_connection(handle.connection.get());
    };

    BenchmarkContext ctx{
        .hwnd = GetConsoleWindow(),
        .input = GetStdHandle(STD_INPUT_HANDLE),
        .output = GetStdHandle(STD_OUTPUT_HANDLE),
        .arena = scratch.arena,
        .utf8_4Ki = mem::repeat_string(scratch.arena, payload_utf8, 4 * 1024 / 128),
        .utf8_128Ki = mem::repeat_string(scratch.arena, payload_utf8, 128 * 1024 / 128),
        .utf16_4Ki = mem::repeat_string(scratch.arena, payload_utf16, 4 * 1024 / 128),
        .utf16_128Ki = mem::repeat_string(scratch.arena, payload_utf16, 128 * 1024 / 128),
    };

    prepare_conhost(ctx, parent_hwnd);
    Sleep(1000);

    const auto results = arena.push_uninitialized_span<Measurements>(s_benchmarks_count);
    for (auto& measurements : results)
    {
        measurements = arena.push_zeroed_span<int32_t>(2048);
    }

    for (size_t bench_idx = 0; bench_idx < s_benchmarks_count; ++bench_idx)
    {
        const auto& bench = s_benchmarks[bench_idx];
        auto& measurements = results[bench_idx];

        print_with_parent_connection("- %s", bench.title);

        static constexpr SMALL_RECT scrollRect{ 0, 0, 119, 9000 };
        static constexpr COORD scrollTarget{ 0, -9001 };
        static constexpr CHAR_INFO scrollFill{ L' ', FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED };

        // Warmup for 0.1s max.
        ScrollConsoleScreenBufferW(ctx.output, &scrollRect, nullptr, scrollTarget, &scrollFill);
        ctx.time_limit = query_perf_counter() + freq / 10;
        bench.exec(ctx, measurements);

        // Actual run for 1s max.
        ScrollConsoleScreenBufferW(ctx.output, &scrollRect, nullptr, scrollTarget, &scrollFill);
        ctx.time_limit = query_perf_counter() + freq;
        bench.exec(ctx, measurements);

        // Trim off trailing 0s that resulted from the time_limit.
        size_t len = measurements.size();
        for (; len > 0 && measurements[len - 1] == 0; --len)
        {
        }
        measurements = measurements.subspan(0, len);

        print_with_parent_connection(", done\r\n");
    }

    set_active_connection(parent_connection);
    return results;
}

static void generate_html(mem::Arena& arena, const AccumulatedResults* results)
{
    const auto scratch = mem::get_scratch_arena(arena);

    const wil::unique_hfile file{ THROW_LAST_ERROR_IF_NULL(CreateFileW(L"measurements.html", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)) };
    const auto sec_per_tick = 1.0f / query_perf_freq();
    BufferedWriter writer{ file.get(), scratch.arena.push_uninitialized_span<char>(1024 * 1024) };

    writer.write(R"(<!DOCTYPE html>
<!DOCTYPE html>
<html lang="en-US">

<head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width,initial-scale=1" />
    <style>
        html {
            overflow-x: hidden;
        }

        html, body {
            margin: 0;
            padding: 0;
        }

        body {
            display: flex;
            flex-direction: row;
            flex-wrap: wrap;
        }

        .view {
            width: 1024px;
            height: 600px;
        }
    </style>
</head>

<body>
    <script src="https://cdn.plot.ly/plotly-2.27.0.min.js" charset="utf-8"></script>
    <script>
)");

    {
        writer.write("        const results = [");

        for (size_t bench_idx = 0; bench_idx < s_benchmarks_count; ++bench_idx)
        {
            const auto& bench = s_benchmarks[bench_idx];

            writer.write("{title:'");
            writer.write(bench.title);
            writer.write("',results:[");

            for (size_t trace_idx = 0; trace_idx < results->trace_count; ++trace_idx)
            {
                writer.write("{basename:'");
                writer.write(results->trace_names[trace_idx]);
                writer.write("',measurements:[");

                const auto measurements = results->measurments[trace_idx][bench_idx];
                if (!measurements.empty())
                {
                    std::sort(measurements.begin(), measurements.end());

                    // Console calls have a high tail latency. Whatever the reason is (it's probably scheduling latency)
                    // it's not particularly interesting at the moment when the median latency is intolerable high anyway.
                    const auto p25 = measurements[(measurements.size() * 25 + 50) / 100];
                    const auto p75 = measurements[(measurements.size() * 75 + 50) / 100];
                    const auto iqr3 = (p75 - p25) * 3;
                    const auto outlier_max = p75 + iqr3;

                    const auto beg = measurements.data();
                    auto end = beg + measurements.size();

                    for (; end[-1] > outlier_max; --end)
                    {
                    }

                    for (auto it = beg; it < end; ++it)
                    {
                        char buffer[32];
                        const auto res = std::to_chars(&buffer[0], &buffer[64], *it * sec_per_tick, std::chars_format::scientific, 3);
                        writer.write({ &buffer[0], res.ptr });
                        writer.write(",");
                    }
                }

                writer.write("]},");
            }

            writer.write("]},");
        }

        writer.write("];\n");
    }

    writer.write(R"(
        for (const r of results) {
            const div = document.createElement('div');
            div.className = 'view';
            document.body.insertAdjacentElement('beforeend', div)

            Plotly.newPlot(div, r.results.map(tcr => ({
                type: 'violin',
                name: tcr.basename,
                y: tcr.measurements,
                meanline: { visible: true },
                points: false,
                spanmode : 'hard',
            })), {
                showlegend: false,
                title: r.title,
                yaxis: {
                    minexponent: 0,
                    showgrid: true,
                    showline: true,
                    ticksuffix: 's',
                },
            }, {
                responsive: true,
            });
        }
    </script>
</body>

</html>
)");
}
