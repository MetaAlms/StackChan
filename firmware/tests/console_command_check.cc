#include "console_command.h"
#include "session_run_control.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {
using namespace webrtc_media;
using Result = ConsoleLineReader::Result;
unsigned checks = 0;
void Require(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(expression) Require(bool(expression), #expression, __LINE__)

Result FeedLine(ConsoleLineReader& reader, std::string_view line)
{
    CHECK(!line.empty() && line.back() == '\n');
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        CHECK(reader.Feed(line[i]) == Result::None);
        CHECK(reader.last_command().kind == CommandKind::None);
    }
    return reader.Feed(line.back());
}

void ValidCommandsAndFragments()
{
    ConsoleLineReader reader;
    CHECK(reader.last_command().kind == CommandKind::None);
    for (std::string_view chunk : {"STA", "RT Run_1-A ", "ne", "ar 5"}) {
        for (char ch : chunk) {
            CHECK(reader.Feed(ch) == Result::None);
            CHECK(reader.last_command().kind == CommandKind::None);
        }
    }
    CHECK(reader.Feed('\n') == Result::Command);
    CHECK(reader.last_command().kind == CommandKind::Start);
    CHECK(reader.last_command().run_id == "Run_1-A");
    CHECK(reader.last_command().mode == RunMode::NearEnd);
    CHECK(reader.last_command().duration_seconds == 5);
    const ConsoleCommand command = reader.last_command();
    SessionRunControl run;
    CHECK(run.Start(command.run_id, command.mode, command.duration_seconds, 100));
    CHECK(run.state() == RunState::Preparing);

    CHECK(FeedLine(reader, "START second duplex 180\r\n") == Result::Command);
    CHECK(reader.last_command().kind == CommandKind::Start);
    CHECK(reader.last_command().mode == RunMode::Duplex);
    CHECK(reader.last_command().duration_seconds == 180);
    CHECK(FeedLine(reader, "STOP Run_1-A\n") == Result::Command);
    CHECK(reader.last_command().kind == CommandKind::Stop);
    CHECK(reader.last_command().run_id == "Run_1-A");
    CHECK(reader.last_command().duration_seconds == 0);
    CHECK(run.Stop(reader.last_command().run_id, StopReason::Requested));
    CHECK(run.state() == RunState::Stopping);
    CHECK(FeedLine(reader, "STATUS\r\n") == Result::Command);
    CHECK(reader.last_command().kind == CommandKind::Status);
    CHECK(reader.last_command().run_id.empty());

    const std::string maximum_id = "aZ_09-" + std::string(26, 'x');
    CHECK(maximum_id.size() == 32);
    CHECK(FeedLine(reader, "START " + maximum_id + " near 60\n") == Result::Command);
    CHECK(reader.last_command().run_id == maximum_id);
    CHECK(FeedLine(reader, "STOP " + maximum_id + "\r\n") == Result::Command);
    for (uint32_t seconds : {5u, 9u, 10u, 99u, 100u, 179u, 180u}) {
        CHECK(FeedLine(reader, "START boundary near " + std::to_string(seconds) + "\n") == Result::Command);
        CHECK(reader.last_command().duration_seconds == seconds);
    }
    for (uint32_t seconds : {3u, 4u, 9u, 10u}) {
        CHECK(FeedLine(reader, "AUDIT " + std::to_string(seconds) + "\n") == Result::Command);
        CHECK(reader.last_command().kind == CommandKind::Audit);
        CHECK(reader.last_command().audit_seconds == seconds);
        CHECK(reader.last_command().run_id.empty());
        CHECK(reader.last_command().duration_seconds == 0);
    }
    CHECK(FeedLine(reader, "AUDIT 5\r\n") == Result::Command);
    CHECK(reader.last_command().kind == CommandKind::Audit);
}

void InvalidLines()
{
    ConsoleLineReader reader;
    for (const std::string& text : {
            "", " ", "START", "STOP", "status", "start r near 5", "stop r", "RESTART r near 5",
            "STATUS now", "STATUS ", " STATUS", "START r near 5 extra", "STOP r extra",
            "START r near", "START r near 5 ", " START r near 5", "START  r near 5",
            "START r  near 5", "START r near  5", "START r near\t5", "START\tr near 5",
            "START r NEAR 5", "START r Duplex 5", "START r other 5", "START r/dir near 5",
            "STOP r/dir", "START r.id near 5", "START r:id near 5", "START r\"x near 5",
            "START r near 0", "START r near 4", "START r near 181", "START r near 999",
            "START r near 1000", "START r near 05", "START r near 005", "START r near +5",
            "START r near -5", "START r near 5.0", "START r near 5e0", "START r near 0x5",
            "START r near 5x", "START r near 5;STATUS", "START r near 4294967301",
            "START r near 5\rSTATUS", "START r near 5\r\r", "STATUS\rjunk", "\tSTATUS",
            "AUDIT", "AUDIT ", " AUDIT 5", "AUDIT 0", "AUDIT 1", "AUDIT 2", "AUDIT 11",
            "AUDIT 99", "AUDIT 05", "AUDIT 005", "AUDIT +5", "AUDIT -5", "AUDIT 5.0",
            "AUDIT 5e0", "AUDIT 0x5", "AUDIT 5x", "AUDIT 5 6", "AUDIT 5 extra",
            "AUDIT r 5", "audit 5", "AUDIT\t5"}) {
        CHECK(FeedLine(reader, text + "\n") == Result::Rejected);
        CHECK(reader.last_command().kind == CommandKind::None);
    }
    CHECK(FeedLine(reader, "START " + std::string(33, 'a') + " near 5\n") == Result::Rejected);
    CHECK(FeedLine(reader, "STOP " + std::string(33, 'a') + "\n") == Result::Rejected);
    CHECK(FeedLine(reader, "START \xE4\xB8\xAD near 5\n") == Result::Rejected);
    for (unsigned byte = 0; byte <= 255; ++byte) {
        if (byte >= 32 && byte <= 126) continue;
        if (byte == '\n' || byte == '\r') continue;
        std::string line = "START r near 5";
        line.push_back(static_cast<char>(byte));
        line += "\n";
        CHECK(FeedLine(reader, line) == Result::Rejected);
        CHECK(reader.last_command().kind == CommandKind::None);
    }
    CHECK(FeedLine(reader, "STATUS\n") == Result::Command);  // Rejection does not poison the next line.
}

void DiscardEntireOverlongLine()
{
    ConsoleLineReader reader;
    const std::string valid_prefix = "START unsafe duplex 5";
    for (char ch : valid_prefix) CHECK(reader.Feed(ch) == Result::None);
    for (size_t i = valid_prefix.size(); i < ConsoleLineReader::kMaxLineBytes; ++i) {
        CHECK(reader.Feed(' ') == Result::None);
    }
    CHECK(reader.Feed('X') == Result::None);  // Byte 129 enters discard mode.
    for (char ch : std::string_view("START suffix near 5\r")) {
        CHECK(reader.Feed(ch) == Result::None);
        CHECK(reader.last_command().kind == CommandKind::None);
    }
    CHECK(reader.Feed('\n') == Result::Rejected);
    CHECK(reader.last_command().kind == CommandKind::None);
    CHECK(FeedLine(reader, "START safe near 5\n") == Result::Command);
    CHECK(reader.last_command().run_id == "safe");

    CHECK(FeedLine(reader, std::string(128, 'A') + "\n") == Result::Rejected);
    CHECK(FeedLine(reader, std::string(129, 'A') + "\r\n") == Result::Rejected);
    CHECK(FeedLine(reader, std::string(4096, 'B') + "START no near 5\n") == Result::Rejected);
    CHECK(FeedLine(reader, "STATUS\n") == Result::Command);
}

void NoCompleteCommandCannotStart()
{
    ConsoleLineReader reader;
    SessionRunControl run;
    CHECK(run.state() == RunState::Idle);
    CHECK(reader.last_command().kind == CommandKind::None);
    for (char ch : std::string_view("START pending near 5\r")) {
        CHECK(reader.Feed(ch) == Result::None);
        CHECK(reader.last_command().kind == CommandKind::None);
        CHECK(run.state() == RunState::Idle);
    }
    CHECK(reader.Feed('x') == Result::None);  // Invalid CR continuation cannot complete the START.
    CHECK(reader.Feed('\n') == Result::Rejected);
    CHECK(reader.last_command().kind == CommandKind::None);
    CHECK(run.state() == RunState::Idle);
    CHECK(FeedLine(reader, "STATUS\n") == Result::Command);
    CHECK(reader.last_command().kind == CommandKind::Status);
    CHECK(run.state() == RunState::Idle);
    CHECK(FeedLine(reader, "STOP never-started\n") == Result::Command);
    CHECK(!run.Stop(reader.last_command().run_id, StopReason::Requested));
    CHECK(run.state() == RunState::Idle);
    CHECK(reader.Feed('S') == Result::None);
    CHECK(reader.last_command().kind == CommandKind::None);
}
}  // namespace

int main()
{
    ValidCommandsAndFragments();
    InvalidLines();
    DiscardEntireOverlongLine();
    NoCompleteCommandCannotStart();
    std::printf("console_command_check: PASS (%u assertions)\n", checks);
}
