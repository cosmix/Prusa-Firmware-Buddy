#include "cancel_object_command.h"
#include "handler.h"
#include "json_parser.h"

#include <marlin_client.hpp>
#include <marlin_vars.hpp>
#include <state/printer_state.hpp>
#include <inc/MarlinConfig.h>

#include <cassert>
#include <cstring>
#include <algorithm>

namespace nhttp::printer {

using namespace handler;
using http::Status;
using json::Event;
using json::Type;
using printer_state::DeviceState;
using std::nullopt;
using std::string_view;

namespace {

    enum class Action {
        Cancel,
        Uncancel,
    };

    struct ParsedCommand {
        int object_index = -1; // -1 means current object
        Action action = Action::Cancel;
        bool has_index = false;
    };

} // namespace

CancelObjectCommand::CancelObjectCommand(size_t content_length, bool can_keep_alive, bool json_errors)
    : content_length(content_length)
    , can_keep_alive(can_keep_alive)
    , json_errors(json_errors) {
    memset(buffer.data(), 0, buffer.size());
}

void CancelObjectCommand::step(std::string_view input, bool terminated_by_client, uint8_t *, size_t, Step &out) {
    if (content_length > buffer.size()) {
        // Refuse early, without reading the body -> drop the connection too.
        out.read = 0;
        out.written = 0;
        out.next = StatusPage(Status::PayloadTooLarge, StatusPage::CloseHandling::ErrorClose, json_errors);
        return;
    }

    const size_t rest = content_length - buffer_used;
    const size_t to_read = std::min(input.size(), rest);

    memcpy(buffer.data() + buffer_used, input.data(), to_read);
    buffer_used += to_read;

    if (content_length > buffer_used) {
        // Still waiting for more data.
        if (terminated_by_client) {
            out.read = to_read;
            out.written = 0;
            out.next = StatusPage(Status::BadRequest, StatusPage::CloseHandling::ErrorClose, json_errors, nullopt, "Truncated request");
            return;
        } else {
            out.read = to_read;
            out.written = 0;
            out.next = Continue();
            return;
        }
    }

    out.read = to_read;
    out.written = 0;
    out.next = process();
}

StatusPage CancelObjectCommand::process() {
#if ENABLED(CANCEL_OBJECTS)
    ParsedCommand parsed;

    const auto parse_result = parse_command(reinterpret_cast<char *>(buffer.data()), buffer_used, [&](const Event &event) {
        if (event.depth != 1) {
            return;
        }

        // Handle both String and Primitive (number) types
        if (event.type == Type::Primitive && event.key.has_value() && event.key.value() == "object_index") {
            // Parse the number as integer
            if (event.value.has_value()) {
                const auto value_str = event.value.value();
                char *end;
                long index = strtol(value_str.data(), &end, 10);

                // Check if parsing was successful and index is valid
                if (end != value_str.data() && index >= 0 && index < 64) { // 64 is max objects (64-bit mask)
                    parsed.object_index = static_cast<int>(index);
                    parsed.has_index = true;
                }
            }
            return;
        }

        if (event.type != Type::String) {
            return;
        }

        if (!event.key.has_value() || !event.value.has_value()) {
            return;
        }

        const auto &key = event.key.value();
        const auto &value = event.value.value();

        if (key == "action") {
            if (value == "cancel") {
                parsed.action = Action::Cancel;
            } else if (value == "uncancel") {
                parsed.action = Action::Uncancel;
            }
        }
    });

    switch (parse_result) {
    case JsonParseResult::ErrMem:
        return StatusPage(Status::PayloadTooLarge, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "Too many JSON tokens");
    case JsonParseResult::ErrReq:
        return StatusPage(Status::BadRequest, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "Couldn't parse JSON");
    case JsonParseResult::Ok:
        break;
    }

    // Validate the request
    if (!can_modify_objects()) {
        return StatusPage(Status::Conflict, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "Not printing");
    }

    // If no index provided and action is cancel, cancel current object
    if (!parsed.has_index && parsed.action == Action::Cancel) {
        if (cancel_object(-1)) {
            return StatusPage(Status::NoContent, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors);
        } else {
            return StatusPage(Status::Conflict, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "Failed to cancel current object");
        }
    }

    // Validate object index if provided
    if (parsed.has_index) {
        const int object_count = marlin_vars().cancel_object_count;
        if (parsed.object_index >= object_count) {
            return StatusPage(Status::BadRequest, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "Invalid object index");
        }

        bool success = false;
        if (parsed.action == Action::Cancel) {
            success = cancel_object(parsed.object_index);
        } else {
            success = uncancel_object(parsed.object_index);
        }

        if (success) {
            return StatusPage(Status::NoContent, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors);
        } else {
            return StatusPage(Status::Conflict, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "Operation failed");
        }
    }

    return StatusPage(Status::BadRequest, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "Missing object_index for uncancel action");
#else
    return StatusPage(Status::NotImplemented, can_keep_alive ? StatusPage::CloseHandling::KeepAlive : StatusPage::CloseHandling::Close, json_errors, nullopt, "Cancel objects feature not available");
#endif // ENABLED(CANCEL_OBJECTS)
}

bool CancelObjectCommand::cancel_object(int object_index) {
#if ENABLED(CANCEL_OBJECTS)
    if (object_index == -1) {
        // Cancel current object
        marlin_client::cancel_current_object();
    } else {
        marlin_client::cancel_object(object_index);
    }
    return true;
#else
    return false;
#endif
}

bool CancelObjectCommand::uncancel_object(int object_index) {
#if ENABLED(CANCEL_OBJECTS)
    if (object_index < 0) {
        return false; // Cannot uncancel "current" object
    }
    marlin_client::uncancel_object(object_index);
    return true;
#else
    return false;
#endif
}

bool CancelObjectCommand::can_modify_objects() const {
    const auto state = printer_state::get_state(false);
    return state == DeviceState::Printing || state == DeviceState::Paused;
}

} // namespace nhttp::printer
