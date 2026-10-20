#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace ssb64 {

/* Explicit lifecycle for UI-facing asynchronous work.
 *
 * ImGui repaints every frame, so "an operation is in flight" cannot live
 * in a lambda capture or a callback's tail — it has to be state the Draw
 * path can poll. This is that state, as a closed machine:
 *
 *            ┌──────────── Start ────────────┐
 *            ▼                               │
 *   Idle ──Start──▶ Pending ──Succeed──▶ Succeeded
 *     ▲             │      ╰──Fail───▶ Failed     │
 *     │             │      ╰──Cancel─▶ Cancelled  │
 *     ╰────────── Reset (from any state) ─────────╯
 *
 * Start returns a Ticket; a completion is only accepted while it carries
 * the live ticket. That makes the classic immediate-mode bug — a stale
 * async answer arriving after the user already restarted the operation —
 * a rejected no-op instead of a silent overwrite.
 */
enum class AsyncOpState { Idle, Pending, Succeeded, Failed, Cancelled };

template <typename T>
class AsyncOperation {
  public:
    using Ticket = std::uint32_t;

    /* Idle/settled/Pending -> Pending; every Start supersedes the previous
     * attempt by bumping the generation. Returns the ticket completions
     * for this attempt must present. */
    Ticket Start() {
        mState = AsyncOpState::Pending;
        mResult = T{};
        mError.clear();
        return ++mGeneration;
    }

    /* Completion entry points — accepted only while pending AND the ticket
     * matches the live generation. Return false = the caller's result was
     * stale and dropped. */
    bool Succeed(Ticket ticket, T result) {
        if (!Accepts(ticket)) {
            return false;
        }
        mResult = std::move(result);
        mState = AsyncOpState::Succeeded;
        return true;
    }

    bool Fail(Ticket ticket, std::string error) {
        if (!Accepts(ticket)) {
            return false;
        }
        mError = std::move(error);
        mState = AsyncOpState::Failed;
        return true;
    }

    bool Cancel(Ticket ticket) {
        if (!Accepts(ticket)) {
            return false;
        }
        mState = AsyncOpState::Cancelled;
        return true;
    }

    /* Any state -> Idle. The generation is NOT bumped: pending callbacks
     * are already dead because Accepts() requires Pending state. */
    void Reset() {
        mState = AsyncOpState::Idle;
        mResult = T{};
        mError.clear();
    }

    AsyncOpState State() const { return mState; }
    bool IsPending() const { return mState == AsyncOpState::Pending; }
    bool IsSettled() const {
        return mState == AsyncOpState::Succeeded || mState == AsyncOpState::Failed ||
               mState == AsyncOpState::Cancelled;
    }

    /* Valid only while State() == Succeeded. */
    const T& Result() const { return mResult; }
    /* Valid only while State() == Failed. */
    const std::string& Error() const { return mError; }
    Ticket Generation() const { return mGeneration; }

  private:
    bool Accepts(Ticket ticket) const {
        return mState == AsyncOpState::Pending && ticket == mGeneration;
    }

    AsyncOpState mState = AsyncOpState::Idle;
    Ticket mGeneration = 0;
    T mResult{};
    std::string mError;
};

} // namespace ssb64
