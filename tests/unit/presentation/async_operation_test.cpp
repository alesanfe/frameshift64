/* AsyncOperation — the explicit lifecycle every UI-facing async flow polls.
 * Pins the transition table, the generation-ticket stale-answer rejection,
 * and Reset semantics. */
#include "framework/AsyncOperation.h"

#include <smash_test/check.h>

using ssb64::AsyncOperation;
using ssb64::AsyncOpState;

int main() {
    /* Idle is the only start state with no pending ticket. */
    AsyncOperation<int> op;
    SMASH_CHECK(op.State() == AsyncOpState::Idle);
    SMASH_CHECK(!op.IsPending());
    SMASH_CHECK(!op.IsSettled());

    /* A completion presented against Idle — or carrying no live ticket —
     * is rejected rather than silently mutating state. */
    SMASH_CHECK(!op.Succeed(0, 1));
    SMASH_CHECK(!op.Fail(0, "early"));
    SMASH_CHECK(!op.Cancel(0));
    SMASH_CHECK(op.State() == AsyncOpState::Idle);

    /* Happy path: Start -> Succeed, result readable, settled. */
    auto ticket = op.Start();
    SMASH_CHECK(op.IsPending());
    SMASH_CHECK(ticket == op.Generation());
    SMASH_CHECK(op.Succeed(ticket, 42));
    SMASH_CHECK(op.State() == AsyncOpState::Succeeded);
    SMASH_CHECK(op.IsSettled());
    SMASH_CHECK(op.Result() == 42);

    /* Once settled, the dead ticket is done — nothing else applies. */
    SMASH_CHECK(!op.Succeed(ticket, 7));
    SMASH_CHECK(!op.Fail(ticket, "late"));
    SMASH_CHECK(op.Result() == 42);

    /* Restart bumps the generation: the ticket of the previous attempt
     * can no longer complete. This is the stale-answer guard — an async
     * reply for attempt N must never paint over attempt N+1. */
    AsyncOperation<int> racing;
    auto first = racing.Start();
    auto second = racing.Start();
    SMASH_CHECK(second != first);
    SMASH_CHECK(!racing.Succeed(first, 1));          // stale reply dropped
    SMASH_CHECK(racing.Succeed(second, 2));          // live reply lands
    SMASH_CHECK(racing.Result() == 2);

    /* Failure path stores the error and settles. */
    AsyncOperation<int> failing;
    auto ft = failing.Start();
    SMASH_CHECK(failing.Fail(ft, "connection refused"));
    SMASH_CHECK(failing.State() == AsyncOpState::Failed);
    SMASH_CHECK(failing.Error() == "connection refused");

    /* Cancellation is its own terminal state, distinct from failure. */
    AsyncOperation<int> cancelled;
    auto ct = cancelled.Start();
    SMASH_CHECK(cancelled.Cancel(ct));
    SMASH_CHECK(cancelled.State() == AsyncOpState::Cancelled);
    SMASH_CHECK(cancelled.IsSettled());
    SMASH_CHECK(!cancelled.Succeed(ct, 1));

    /* Reset returns any state to Idle and clears result/error. */
    op.Reset();
    SMASH_CHECK(op.State() == AsyncOpState::Idle);
    SMASH_CHECK(op.Result() == 0);
    cancelled.Reset();
    SMASH_CHECK(cancelled.State() == AsyncOpState::Idle);

    /* After Reset the operation is fully reusable. */
    auto again = op.Start();
    SMASH_CHECK(op.Succeed(again, 9));
    SMASH_CHECK(op.Result() == 9);

    return smash_test_report("async_operation_test");
}
