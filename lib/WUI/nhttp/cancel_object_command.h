#pragma once

#include "status_page.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace nhttp {
namespace handler {
    struct Step;
}
} // namespace nhttp

namespace nhttp::printer {

/**
 * @brief Handler for cancel object commands
 *
 * Processes POST/PUT requests to /api/v1/print/cancel_object endpoint
 * Allows canceling or uncanceling specific objects during multi-object prints
 */
class CancelObjectCommand final {
public:
    CancelObjectCommand(size_t content_length, bool can_keep_alive, bool json_errors);

    bool want_read() const { return true; }
    bool want_write() const { return false; }
    void step(std::string_view input, bool terminated_by_client, uint8_t *output, size_t output_size, handler::Step &out);

private:
    static constexpr size_t BUFFER_SIZE = 128; // Small buffer for simple JSON
    std::array<uint8_t, BUFFER_SIZE> buffer;
    size_t buffer_used = 0;
    size_t content_length;
    bool can_keep_alive;
    bool json_errors;

    handler::StatusPage process();

    /**
     * @brief Cancel an object by index
     * @param object_index Index of the object to cancel (-1 for current object)
     * @return true on success, false if operation not possible
     */
    bool cancel_object(int object_index);

    /**
     * @brief Uncancel an object by index
     * @param object_index Index of the object to uncancel
     * @return true on success, false if operation not possible
     */
    bool uncancel_object(int object_index);

    /**
     * @brief Check if printer is in a state where cancel object is allowed
     * @return true if printing
     */
    bool can_modify_objects() const;
};

} // namespace nhttp::printer
