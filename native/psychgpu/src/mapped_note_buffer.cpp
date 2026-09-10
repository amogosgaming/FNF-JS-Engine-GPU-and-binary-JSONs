#include "mapped_note_buffer.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace psychgpu {
namespace {

#if defined(_WIN32)
std::string windowsErrorMessage(const char* operation, DWORD code) {
    LPSTR systemMessage = nullptr;
    DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&systemMessage),
        0,
        nullptr);

    std::ostringstream result;
    result << operation << " failed (Win32 error " << code << ')';
    if (length != 0 && systemMessage != nullptr) {
        while (length > 0 &&
               (systemMessage[length - 1] == '\r' ||
                systemMessage[length - 1] == '\n')) {
            --length;
        }
        result << ": " << std::string(systemMessage, length);
    }
    if (systemMessage != nullptr) {
        ::LocalFree(systemMessage);
    }
    return result.str();
}

bool utf8ToWide(const std::string& input, std::wstring& output,
                std::string& error) {
    if (input.empty()) {
        error = "file path is empty";
        return false;
    }
    if (input.size() > static_cast<std::size_t>(INT_MAX)) {
        error = "UTF-8 file path is too long";
        return false;
    }

    const int required = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0) {
        error = windowsErrorMessage("UTF-8 path conversion", ::GetLastError());
        return false;
    }

    output.resize(static_cast<std::size_t>(required));
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), output.data(), required);
    if (written != required) {
        error = windowsErrorMessage("UTF-8 path conversion", ::GetLastError());
        output.clear();
        return false;
    }
    return true;
}
#else
std::string posixErrorMessage(const char* operation, int code) {
    std::ostringstream result;
    result << operation << " failed (errno " << code << "): "
           << std::strerror(code);
    return result.str();
}
#endif

} // namespace

MappedNoteBuffer::~MappedNoteBuffer() {
    close();
}

MappedNoteBuffer::MappedNoteBuffer(MappedNoteBuffer&& other) noexcept {
    *this = std::move(other);
}

MappedNoteBuffer& MappedNoteBuffer::operator=(
    MappedNoteBuffer&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    close();
    data_ = other.data_;
    fileSize_ = other.fileSize_;
    noteCount_ = other.noteCount_;
    open_ = other.open_;
#if defined(_WIN32)
    fileHandle_ = other.fileHandle_;
    mappingHandle_ = other.mappingHandle_;
    other.fileHandle_ = nullptr;
    other.mappingHandle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.data_ = nullptr;
    other.fileSize_ = 0;
    other.noteCount_ = 0;
    other.open_ = false;
    return *this;
}

bool MappedNoteBuffer::open(const std::string& filepath,
                            std::string& outError) {
    close();
    outError.clear();

#if defined(_WIN32)
    std::wstring widePath;
    if (!utf8ToWide(filepath, widePath, outError)) {
        return false;
    }

    HANDLE file = ::CreateFileW(
        widePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        outError = windowsErrorMessage("CreateFileW", ::GetLastError()) +
                   ": " + filepath;
        return false;
    }
    fileHandle_ = file;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size)) {
        outError = windowsErrorMessage("GetFileSizeEx", ::GetLastError()) +
                   ": " + filepath;
        close();
        return false;
    }
    if (size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) >
            static_cast<unsigned long long>(
                std::numeric_limits<std::size_t>::max())) {
        outError = "notes.bin is too large for this process address space: " +
                   filepath;
        close();
        return false;
    }
    fileSize_ = static_cast<std::size_t>(size.QuadPart);
#else
    fd_ = ::open(filepath.c_str(), O_RDONLY
#ifdef O_CLOEXEC
                 | O_CLOEXEC
#endif
    );
    if (fd_ < 0) {
        const int code = errno;
        outError = posixErrorMessage("open", code) + ": " + filepath;
        return false;
    }

    struct stat status {};
    if (::fstat(fd_, &status) != 0) {
        const int code = errno;
        outError = posixErrorMessage("fstat", code) + ": " + filepath;
        close();
        return false;
    }
    if (status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::size_t>::max())) {
        outError = "notes.bin is too large for this process address space: " +
                   filepath;
        close();
        return false;
    }
    fileSize_ = static_cast<std::size_t>(status.st_size);
#endif

    if (fileSize_ % NOTE_RECORD_SIZE != 0) {
        outError = "invalid notes.bin size (not divisible by 12): " + filepath;
        close();
        return false;
    }
    noteCount_ = fileSize_ / NOTE_RECORD_SIZE;

    if (fileSize_ == 0) {
        open_ = true;
        return true;
    }

#if defined(_WIN32)
    HANDLE mapping = ::CreateFileMappingW(
        static_cast<HANDLE>(fileHandle_), nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        outError = windowsErrorMessage("CreateFileMappingW", ::GetLastError()) +
                   ": " + filepath;
        close();
        return false;
    }
    mappingHandle_ = mapping;

    void* mapped = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (mapped == nullptr) {
        outError = windowsErrorMessage("MapViewOfFile", ::GetLastError()) +
                   ": " + filepath;
        close();
        return false;
    }
#else
    void* mapped = ::mmap(nullptr, fileSize_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped == MAP_FAILED) {
        const int code = errno;
        outError = posixErrorMessage("mmap", code) + ": " + filepath;
        close();
        return false;
    }
#endif

    data_ = static_cast<const NoteData*>(mapped);
    open_ = true;
    return true;
}

void MappedNoteBuffer::close() noexcept {
#if defined(_WIN32)
    if (data_ != nullptr) {
        ::UnmapViewOfFile(data_);
    }
    if (mappingHandle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(mappingHandle_));
    }
    if (fileHandle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(fileHandle_));
    }
    mappingHandle_ = nullptr;
    fileHandle_ = nullptr;
#else
    if (data_ != nullptr && fileSize_ != 0) {
        ::munmap(const_cast<NoteData*>(data_), fileSize_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
#endif
    data_ = nullptr;
    fileSize_ = 0;
    noteCount_ = 0;
    open_ = false;
}

std::size_t MappedNoteBuffer::findFirstAtOrAfter(
    std::uint64_t targetMs) const noexcept {
    std::size_t low = 0;
    std::size_t high = noteCount_;
    while (low < high) {
        const std::size_t middle = low + ((high - low) >> 1U);
        if (data_[middle].timestamp_ms < targetMs) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

std::size_t MappedNoteBuffer::findFirstAfter(
    std::uint64_t targetMs) const noexcept {
    std::size_t low = 0;
    std::size_t high = noteCount_;
    while (low < high) {
        const std::size_t middle = low + ((high - low) >> 1U);
        if (data_[middle].timestamp_ms <= targetMs) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

std::pair<std::size_t, std::size_t> MappedNoteBuffer::findVisibleRange(
    std::uint64_t startMs,
    std::uint64_t endMs) const noexcept {
    if (startMs > endMs || noteCount_ == 0) {
        return {0, 0};
    }
    return {findFirstAtOrAfter(startMs), findFirstAfter(endMs)};
}

} // namespace psychgpu
