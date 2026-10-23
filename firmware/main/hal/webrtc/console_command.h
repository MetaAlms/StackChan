#pragma once

#include "session_run_control.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace webrtc_media {

enum class CommandKind { None, Start, Stop, Status };

struct ConsoleCommand {
    CommandKind kind = CommandKind::None;
    std::string run_id;  // Populated only after validation; at most 32 bytes.
    RunMode mode = RunMode::NearEnd;
    uint32_t duration_seconds = 0;
};

// One owner feeds bytes and consumes Command results. No command is exposed
// before LF. Overlong or invalid lines are discarded through their final LF,
// so neither a valid prefix nor a suffix can become an accidental START.
class ConsoleLineReader {
public:
    enum class Result { None, Command, Rejected };
    static constexpr size_t kMaxLineBytes = 128;  // Excludes LF or CRLF.

    Result Feed(char ch)
    {
        last_command_ = ConsoleCommand{};
        if (ch == '\n') {
            const bool valid = !discarding_ && Parse(std::string_view(line_.data(), size_));
            size_ = 0;
            discarding_ = false;
            pending_cr_ = false;
            return valid ? Result::Command : Result::Rejected;
        }
        if (discarding_) return Result::None;
        if (pending_cr_) {
            discarding_ = true;  // CR is allowed only immediately before LF.
            return Result::None;
        }
        if (ch == '\r') {
            pending_cr_ = true;
            return Result::None;
        }
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 32 || byte > 126 || size_ == kMaxLineBytes) {
            discarding_ = true;
            return Result::None;
        }
        line_[size_++] = ch;
        return Result::None;
    }

    // Valid only until the next Feed; non-Command results expose kind=None.
    const ConsoleCommand& last_command() const { return last_command_; }

private:
    static bool ValidRunId(std::string_view value)
    {
        if (value.empty() || value.size() > 32) return false;
        for (unsigned char ch : value) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
        }
        return true;
    }

    static bool Duration(std::string_view text, uint32_t& seconds)
    {
        if (text.empty() || text.size() > 3 || text.front() == '0') return false;
        uint32_t value = 0;
        for (unsigned char ch : text) {
            if (ch < '0' || ch > '9') return false;
            value = value * 10 + ch - '0';
        }
        if (value < 5 || value > 180) return false;
        seconds = value;
        return true;
    }

    bool Parse(std::string_view line)
    {
        std::array<std::string_view, 4> tokens{};
        size_t count = 0;
        size_t begin = 0;
        for (size_t end = 0; end <= line.size(); ++end) {
            if (end != line.size() && line[end] != ' ') continue;
            if (end == begin || count == tokens.size()) return false;
            tokens[count++] = line.substr(begin, end - begin);
            begin = end + 1;
        }
        if (count == 1 && tokens[0] == "STATUS") {
            last_command_.kind = CommandKind::Status;
            return true;
        }
        if (count == 2 && tokens[0] == "STOP" && ValidRunId(tokens[1])) {
            last_command_.kind = CommandKind::Stop;
            last_command_.run_id.assign(tokens[1].data(), tokens[1].size());
            return true;
        }
        uint32_t seconds = 0;
        if (count != 4 || tokens[0] != "START" || !ValidRunId(tokens[1]) ||
            (tokens[2] != "near" && tokens[2] != "duplex") || !Duration(tokens[3], seconds)) return false;
        last_command_.kind = CommandKind::Start;
        last_command_.run_id.assign(tokens[1].data(), tokens[1].size());
        last_command_.mode = tokens[2] == "near" ? RunMode::NearEnd : RunMode::Duplex;
        last_command_.duration_seconds = seconds;
        return true;
    }

    std::array<char, kMaxLineBytes> line_{};
    size_t size_ = 0;
    bool discarding_ = false;
    bool pending_cr_ = false;
    ConsoleCommand last_command_;
};

}  // namespace webrtc_media
