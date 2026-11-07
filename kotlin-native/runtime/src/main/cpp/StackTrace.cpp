/*
 * Copyright 2010-2021 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "StackTrace.hpp"

#if USE_GCC_UNWIND
// GCC unwinder for backtrace.
#include <unwind.h>
#if __MINGW64__
#error // GCC unwinder in MinGW64/libgcc has a bugfix only in version 12. With previous libgcc versions, use RTL unwinder instead.
#endif

#elif USE_WINAPI_UNWIND
// Use RtlCaptureContext, RtlLookupFunctionEntry, RtlVirtualUnwind
#include <windows.h>
#include <winnt.h>

#else
// Glibc backtrace() function.
#include <execinfo.h>
#endif

#include <algorithm>

#include "Common.h"
#include "ExecFormat.h"
#include "Porting.h"
#include "SourceInfo.h"
#include "Types.h"

#include "utf8.h"

using namespace kotlin;

namespace {

#if USE_GCC_UNWIND
struct Backtrace {
    Backtrace(size_t skip, std_support::span<void*> buffer): currentSize(0), skipCount(skip), buffer(buffer) {}

    void setNextElement(_Unwind_Ptr element) {
        RuntimeAssert(currentSize < buffer.size(), "Buffer overflow");
        buffer[currentSize++] = reinterpret_cast<void*>(element);
    }

    bool full() const { return currentSize >= buffer.size(); }

    size_t currentSize;
    size_t skipCount;
    std_support::span<void*> buffer;
};

_Unwind_Ptr getUnwindPtr(_Unwind_Context* context) {
    return _Unwind_GetIP(context);
}

_Unwind_Reason_Code depthCountCallback(struct _Unwind_Context* context, void* arg) {
    size_t* result = reinterpret_cast<size_t*>(arg);
    (*result)++;
    return _URC_NO_REASON;
}

_Unwind_Reason_Code unwindCallback(struct _Unwind_Context* context, void* arg) {
    Backtrace* backtrace = reinterpret_cast<Backtrace*>(arg);
    if (backtrace->skipCount > 0) {
        backtrace->skipCount--;
        return _URC_NO_REASON;
    }

    // If the buffer is full, skip the remaining frames.
    if (backtrace->full()) {
        return _URC_NO_REASON;
    }

    _Unwind_Ptr address = getUnwindPtr(context);
    backtrace->setNextElement(address);

    return _URC_NO_REASON;
}
#elif USE_WINAPI_UNWIND
// winAPIUnwind() does:
// - if `result` is not empty -> stores IPs of stacktrace(ignoring first `skipCount` entries) into `result`, and returns amount of stored IPs
// - if `result` is empty  -> returns depth of stacktrace(ignoring first `skipCount` entries)
NO_INLINE size_t winAPIUnwind(size_t skipCount, std_support::span<void*> result)
{
    size_t resultSize = result.size();
    bool doStoreIPs = resultSize > 0;
    size_t currentSize = 0;
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_ALL;
    RtlCaptureContext (&context);
    // Kore: one history table for the whole walk (it caches function table lookups for the frames still to come).
    UNWIND_HISTORY_TABLE historyTable = {};
    do {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION FunctionEntry = RtlLookupFunctionEntry (context.Rip, &imageBase, &historyTable);
        if (!FunctionEntry)
            break;
        PVOID handlerData = nullptr;
        ULONG64 establisherFramePointers[2] = { 0, 0 };
        RtlVirtualUnwind (UNW_FLAG_NHANDLER, imageBase, context.Rip, FunctionEntry, &context, &handlerData, establisherFramePointers, nullptr);

        if (skipCount > 0) {
            skipCount--;
        } else {
            if(doStoreIPs)
                result[currentSize] = reinterpret_cast<void*>(context.Rip);
            ++currentSize;
        }
    } while (context.Rip != 0 && (currentSize < resultSize || !doStoreIPs));

    return currentSize;
}

#if defined(__x86_64__)
// Kore: a per-thread cache of what unwinding a frame does, by return address. For a frame without a frame register
// whose unwind codes all apply (the return address is past the prologue, as every caller's is), unwinding restores
// some registers from fixed offsets of the stack pointer, adds a fixed amount to it, and pops the return address: the
// entry replays that, without RtlLookupFunctionEntry (which finds the module under a lock) and RtlVirtualUnwind. Other
// frames are unwound by the system as before. Exceptions capture their stack trace when they are created, so this is
// most of the cost of creating one.
struct FrameUnwind {
    uint64_t rip = 0;
    uint32_t stackBytes = 0; // Added to Rsp before the return address is read.
    uint8_t restoreCount = 0;
    uint8_t cacheable = 0;
    uint8_t registers[8] = {}; // Rax..R15 numbering.
    uint32_t offsets[8] = {}; // From Rsp at the frame's body.
};

constexpr size_t kFrameUnwindCacheSize = 256; // About 12 KiB of thread-local storage.
THREAD_LOCAL_VARIABLE FrameUnwind tFrameUnwindCache[kFrameUnwindCacheSize];

// Parses the unwind info of `function` for a return address `offset` bytes into it. False if it cannot be cached.
bool parseFrameUnwind(uint64_t imageBase, const RUNTIME_FUNCTION* function, uint32_t offset, FrameUnwind& entry) noexcept {
    uint64_t stack = 0;
    bool primary = true;
    while (true) {
        auto info = reinterpret_cast<const uint8_t*>(imageBase + function->UnwindData);
        uint8_t version = info[0] & 7;
        uint8_t flags = info[0] >> 3;
        uint8_t prologSize = info[1];
        uint8_t count = info[2];
        uint8_t frameRegister = info[3] & 0xF;
        auto codes = reinterpret_cast<const uint16_t*>(info + 4);
        if (version != 1 || frameRegister != 0) return false;
        if (primary && offset < prologSize) return false;
        for (uint8_t i = 0; i < count;) {
            uint8_t op = (codes[i] >> 8) & 0xF;
            uint8_t opInfo = codes[i] >> 12;
            switch (op) {
                case 0: // UWOP_PUSH_NONVOL
                    if (entry.restoreCount == 8) return false;
                    entry.registers[entry.restoreCount] = opInfo;
                    entry.offsets[entry.restoreCount++] = static_cast<uint32_t>(stack);
                    stack += 8;
                    i += 1;
                    break;
                case 1: // UWOP_ALLOC_LARGE
                    if (opInfo == 0) {
                        stack += uint64_t(codes[i + 1]) * 8;
                        i += 2;
                    } else {
                        stack += uint64_t(codes[i + 1]) | (uint64_t(codes[i + 2]) << 16);
                        i += 3;
                    }
                    break;
                case 2: // UWOP_ALLOC_SMALL
                    stack += uint64_t(opInfo) * 8 + 8;
                    i += 1;
                    break;
                case 4: // UWOP_SAVE_NONVOL: relative to Rsp at the body (no frame register).
                    if (entry.restoreCount == 8) return false;
                    entry.registers[entry.restoreCount] = opInfo;
                    entry.offsets[entry.restoreCount++] = static_cast<uint32_t>(uint64_t(codes[i + 1]) * 8);
                    i += 2;
                    break;
                case 5: // UWOP_SAVE_NONVOL_FAR
                    if (entry.restoreCount == 8) return false;
                    entry.registers[entry.restoreCount] = opInfo;
                    entry.offsets[entry.restoreCount++] =
                            static_cast<uint32_t>(uint64_t(codes[i + 1]) | (uint64_t(codes[i + 2]) << 16));
                    i += 3;
                    break;
                case 6: // UWOP_SAVE_XMM (legacy)
                case 8: // UWOP_SAVE_XMM128: no XMM register is needed to unwind.
                    i += 2;
                    break;
                case 7: // UWOP_SAVE_XMM_FAR (legacy)
                case 9: // UWOP_SAVE_XMM128_FAR
                    i += 3;
                    break;
                default: // UWOP_SET_FPREG, UWOP_PUSH_MACHFRAME, unknown.
                    return false;
            }
        }
        if (!(flags & UNW_FLAG_CHAININFO)) break;
        function = reinterpret_cast<const RUNTIME_FUNCTION*>(codes + ((count + 1) & ~1));
        primary = false;
    }
    if (stack > UINT32_MAX) return false;
    entry.stackBytes = static_cast<uint32_t>(stack);
    return true;
}
#endif

// Kore: the whole stack trace in one walk (`winAPIUnwind` needs two: one to count the frames, one to store them), into
// a buffer on the stack that spills to `result` only for deep stacks. Exceptions capture their stack trace when they
// are created, so this is most of the cost of throwing one. Frames are unwound from the per-thread cache when they can
// be (see `FrameUnwind`).
NO_INLINE void winAPIUnwindAll(size_t skipCount, std::vector<void*>& result) {
    void* buffer[256];
    size_t buffered = 0;
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_ALL;
    RtlCaptureContext(&context);
    UNWIND_HISTORY_TABLE historyTable = {};
#if defined(__x86_64__)
    // The committed part of this thread's stack, from its thread information block.
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    auto stackLow = reinterpret_cast<uintptr_t>(tib->StackLimit);
    auto stackHigh = reinterpret_cast<uintptr_t>(tib->StackBase);
    auto* registers = &context.Rax; // Rax..R15, in the unwind codes' numbering.
#endif
    do {
        bool unwound = false;
#if defined(__x86_64__)
        FrameUnwind& entry = tFrameUnwindCache[(context.Rip >> 2) % kFrameUnwindCacheSize];
        if (entry.rip == context.Rip && entry.cacheable) {
            uint64_t rsp = context.Rsp;
            // Stay on this thread's stack: a wrong entry must not read elsewhere.
            if (rsp >= stackLow && rsp + entry.stackBytes + 8 <= stackHigh) {
                for (uint8_t i = 0; i < entry.restoreCount; ++i) {
                    registers[entry.registers[i]] = *reinterpret_cast<const uint64_t*>(rsp + entry.offsets[i]);
                }
                rsp += entry.stackBytes;
                context.Rip = *reinterpret_cast<const uint64_t*>(rsp);
                context.Rsp = rsp + 8;
                unwound = true;
            }
        }
#endif
        if (!unwound) {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION functionEntry = RtlLookupFunctionEntry(context.Rip, &imageBase, &historyTable);
            if (!functionEntry) break;
#if defined(__x86_64__)
            FrameUnwind fresh;
            fresh.rip = context.Rip;
            fresh.cacheable = parseFrameUnwind(imageBase, functionEntry,
                                               static_cast<uint32_t>(context.Rip - imageBase - functionEntry->BeginAddress), fresh);
            if (!fresh.cacheable) fresh.restoreCount = 0;
            entry = fresh;
#endif
            PVOID handlerData = nullptr;
            ULONG64 establisherFramePointers[2] = { 0, 0 };
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, functionEntry, &context, &handlerData, establisherFramePointers, nullptr);
        }
        if (skipCount > 0) {
            skipCount--;
            continue;
        }
        if (buffered == sizeof(buffer) / sizeof(buffer[0])) {
            result.insert(result.end(), buffer, buffer + buffered);
            buffered = 0;
        }
        buffer[buffered++] = reinterpret_cast<void*>(context.Rip);
    } while (context.Rip != 0);
    result.insert(result.end(), buffer, buffer + buffered);
}
#endif

THREAD_LOCAL_VARIABLE bool disallowSourceInfo = false;

int getSourceInfo(void* symbol, SourceInfo *result, int result_len) {
    return disallowSourceInfo ? 0 : compiler::getSourceInfo(symbol, result, result_len);
}

} // namespace

// TODO: this implementation is just a hack, e.g. the result is inexact;
// however it is better to have an inexact stacktrace than not to have any.
NO_INLINE std::vector<void*> kotlin::internal::GetCurrentStackTrace(size_t skipFrames) noexcept {
    // Skip GetCurrentStackTrace + anything asked by the caller.
    const size_t kSkipFrames = 1 + skipFrames;

    std::vector<void*> result;
#if USE_GCC_UNWIND
    size_t depth = 0;
    _Unwind_Backtrace(depthCountCallback, static_cast<void*>(&depth));
    if (depth <= kSkipFrames) return {};
    result.resize(depth - kSkipFrames);

    Backtrace traceHolder(kSkipFrames, std_support::span<void*>(result.data(), result.size()));
    _Unwind_Backtrace(unwindCallback, static_cast<void*>(&traceHolder));
    RuntimeAssert(result.size() == traceHolder.currentSize, "Expected and collected sizes of the stacktrace differ");

    return result;
#elif USE_WINAPI_UNWIND
    winAPIUnwindAll(kSkipFrames, result);
    return result;
#else
    // Take into account this function and StackTrace::current.
    constexpr size_t maxSize = GetMaxStackTraceDepth<StackTraceCapacityKind::kDynamic>() + 2;
    result.resize(maxSize);
    auto size = static_cast<size_t>(backtrace(result.data(), static_cast<int>(result.size())));
    if (size <= kSkipFrames) return {};
    result.resize(size);

    // Drop first kSkipFrames elements.
    result.erase(result.begin(), std::next(result.begin(), kSkipFrames));
    return result;
#endif // !USE_GCC_UNWIND
}

// TODO: this implementation is just a hack, e.g. the result is inexact;
// however it is better to have an inexact stacktrace than not to have any.
NO_INLINE size_t kotlin::internal::GetCurrentStackTrace(size_t skipFrames, std_support::span<void*> buffer) noexcept {
    // Skip GetCurrentStackTrace + anything asked by the caller.
    const size_t kSkipFrames = 1 + skipFrames;

#if USE_GCC_UNWIND
    Backtrace traceHolder(kSkipFrames, buffer);
    _Unwind_Backtrace(unwindCallback, static_cast<void*>(&traceHolder));
    return traceHolder.currentSize;
#elif USE_WINAPI_UNWIND
    return winAPIUnwind(kSkipFrames, buffer);
#else
    // Take into account this function and StackTrace::current.
    constexpr size_t maxSize = GetMaxStackTraceDepth<StackTraceCapacityKind::kFixed>() + 2;
    void* tmpBuffer[maxSize];
    size_t size = backtrace(tmpBuffer, static_cast<int>(maxSize));
    if (size <= kSkipFrames) return 0;

    size_t elementsCount = std::min(buffer.size(), size - kSkipFrames);
    std::copy_n(std::begin(tmpBuffer) + kSkipFrames, elementsCount, std::begin(buffer));
    return elementsCount;
#endif // !USE_GCC_UNWIND
}

#include <cstdarg>
#include <cstring>
#include "std_support/Span.hpp"
#include "Format.h"

#if __has_include("dlfcn.h")
#include <dlfcn.h>
#endif

namespace {

__attribute__((format(printf, 5, 6)))
std_support::span<char> snprintf_with_addr(std_support::span<char> buffer, size_t frame, const void* addr, bool is_inline, const char *format, ...) {
    const char* image = "???";
    char symbol[512];
    strcpy(symbol, "0x0");
    ptrdiff_t symbol_offset = reinterpret_cast<ptrdiff_t>(addr);

#if __has_include("dlfcn.h")
    Dl_info info{};
    dladdr(addr, &info);

    if (info.dli_fname) {
        const char* tmp = strrchr(info.dli_fname, '/');
        if (tmp == nullptr) {
            image = info.dli_fname;
        } else {
            image = tmp + 1;
        }
    }
#endif

    AddressToSymbol(addr, symbol, sizeof(symbol), symbol_offset);

    buffer = FormatToSpan(buffer, "%-4zd%-35s %-18p %s + %td ", frame, image, addr, symbol, symbol_offset);
    if (is_inline) {
        buffer = FormatToSpan(buffer, "[inlined] ");
    }
    std::va_list args;
    va_start(args, format);
    buffer = VFormatToSpan(buffer, format, args);
    va_end(args);
    return buffer;
}


/*
 * This is hack for better traces.
 * In some cases backtrace function returns address after call instruction, while address detection need call instruction itself.
 * adjustAddressForSourceInfo function tries to fix it with some heuristics.
 *
 * For honest solution, we should distinguish backtrace symbols got from signal handlers frames, ordinary frames,
 * and addresses got from somewhere else. But for now, we assume all addresses are ordinary backtrace frames.
 */

#if (defined(KONAN_X64) || defined(KONAN_X86)) && !defined(KONAN_WINDOWS)
KNativePtr adjustAddressForSourceInfo(KNativePtr address) {
    return reinterpret_cast<KNativePtr>(reinterpret_cast<uintptr_t>(address) - 1);
}
#elif (defined(KONAN_ARM32) || defined(KONAN_ARM64)) && !defined(KONAN_WINDOWS)
KNativePtr adjustAddressForSourceInfo(KNativePtr address) {
    /*
     * On arm instructions are always 2-bytes aligned. But odd bit can be used to encode instruction set.
     * Not sure, if this can happen in our code, but let's just clear it.
     */
    return reinterpret_cast<KNativePtr>((reinterpret_cast<uintptr_t>(address) & ~1) - 1);
}
#else
KNativePtr adjustAddressForSourceInfo(KNativePtr address) { return address; }
#endif

void formatSourceInfo(std_support::span<char> buffer, size_t frame, void* address, bool isInline, const SourceInfo& sourceInfo) {
    const char* fileName = sourceInfo.getFileName().c_str();
    if (sourceInfo.lineNumber == -1) {
        snprintf_with_addr(buffer, frame, address, isInline, "(%s:<unknown>)", fileName);
    } else if (sourceInfo.column == -1) {
        snprintf_with_addr(buffer, frame, address, isInline, "(%s:%d)", fileName, sourceInfo.lineNumber);
    } else {
        snprintf_with_addr(buffer, frame, address, isInline, "(%s:%d:%d)", fileName, sourceInfo.lineNumber, sourceInfo.column);
    }
}

}

std::vector<std::string> kotlin::GetStackTraceStrings(std_support::span<void* const> stackTrace) noexcept {
    std::vector<std::string> strings;
    strings.reserve(stackTrace.size());

    // outside the loop to avoid calling constructors and destructors each time
    SourceInfo framesInfoBuffer[10];

    bool errorReported = false;

    for (void* address : stackTrace) {
        if (!address || reinterpret_cast<uintptr_t>(address) == 1) continue;

        bool isSomethingPrinted = false;
        bool isSomethingHidden = false;
        char line[1024];
        std_support::span<char> lineBuffer(line);

        address = adjustAddressForSourceInfo(address);
        int framesCount = getSourceInfo(address, framesInfoBuffer, std::size(framesInfoBuffer));
        if (framesCount >= 0) {
            int framesToPrint = std::min<int>(framesCount, std::size(framesInfoBuffer));
            for (int i = 0; i < framesToPrint; ++i) {
                const SourceInfo& sourceInfo = framesInfoBuffer[i];
                if (sourceInfo.nodebug) {
                    isSomethingHidden = true;
                    continue;
                }
                if (sourceInfo.getFileName().empty()) {
                    continue;
                }
                isSomethingPrinted = true;

                bool isLast = (i == framesToPrint - 1);
                if (isLast && framesCount != framesToPrint) {
                    snprintf_with_addr(lineBuffer, strings.size(), address, false, "[some inlined frames skipped]");
                    strings.emplace_back(line);
                }

                formatSourceInfo(lineBuffer, strings.size(), address, !isLast, sourceInfo);
                strings.emplace_back(line);
            }
        }

        if (!isSomethingPrinted && !isSomethingHidden) {
            bool errorOccurred = framesCount == -1;
            auto message = "";
            if (errorOccurred && !errorReported) {
                message = "<NOTE: unable to find file names and line numbers, "
                          "see https://youtrack.jetbrains.com/issue/KT-85559>";
                errorReported = true;
            }
            snprintf_with_addr(lineBuffer, strings.size(), address, false, "%s", message);
            strings.emplace_back(line);
        }
    }

    return strings;
}

void kotlin::DisallowSourceInfo() {
    disallowSourceInfo = true;
}

NO_INLINE void kotlin::PrintStackTraceStderr() {
    // NOTE: This might be called from both runnable and native states (including in uninitialized runtime)
    // TODO: This is intended for runtime use. Try to avoid memory allocations and signal unsafe functions.

    // Skip this function.
    constexpr int kSkipFrames = 1;
    StackTrace trace = StackTrace<>::current(kSkipFrames);
    auto stackTraceStrings = GetStackTraceStrings(trace.data());
    for (auto& frame : stackTraceStrings) {
        konan::consoleErrorUtf8(frame.c_str(), frame.size());
        konan::consoleErrorf("\n");
    }
}
