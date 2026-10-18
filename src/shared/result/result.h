#pragma once

/* ssb64::Result<T> — the C++ error channel for port-internal APIs.
 *
 * Policy (docs/architecture/error-model.md):
 *   - public module APIs return Result<T>/Status, not bool+out-params
 *   - no exceptions across the port_* ABI or module boundaries
 *   - decomp-facing C keeps int/NULL conventions; translate at the edge
 *
 * Header-only, dependency-free beyond <variant>/<string>. */

#include "error_code.h"

#include <string>
#include <variant>
#include <utility>

namespace ssb64 {

struct Error {
    PortErrorCategory category;
    int code;          /* module-specific detail code (errno, HRESULT, 0) */
    std::string message;

    const char* category_name() const {
        return port_error_category_name(category);
    }
};

namespace detail {

struct OkTag {};

} // namespace detail

inline constexpr detail::OkTag kOk{};

template <typename T>
class Result {
  public:
    Result(T value) : mStorage(std::move(value)) {}
    Result(Error error) : mStorage(std::move(error)) {}

    bool ok() const { return std::holds_alternative<T>(mStorage); }
    explicit operator bool() const { return ok(); }

    T& value() & { return std::get<T>(mStorage); }
    const T& value() const& { return std::get<T>(mStorage); }
    T&& value() && { return std::move(std::get<T>(mStorage)); }

    /* Precondition: !ok(). */
    const Error& error() const { return std::get<Error>(mStorage); }

    /* value() or fallback — the escape hatch for callers that can only
     * degrade, e.g. menu widgets showing a default. */
    template <typename U>
    T value_or(U&& fallback) const& {
        return ok() ? std::get<T>(mStorage) : T(std::forward<U>(fallback));
    }

  private:
    std::variant<T, Error> mStorage;
};

/* Result<void> — success or Error. `return kOk` or `return Error{...}`. */
using Status = Result<detail::OkTag>;

} // namespace ssb64
