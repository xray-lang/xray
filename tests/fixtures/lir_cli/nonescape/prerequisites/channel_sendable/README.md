# Channel function-value prerequisites

Status is `NOT_READY`. This directory preserves five Source inputs and four
target escape-diagnostic goldens.

`Channel<T>` requires `T` to satisfy the existing `Sendable` proof. The current
proof rejects function types, so each `Channel<fn() -> i64>` declaration reaches
that prerequisite before its function-value argument can reach an escape sink.

The ordinary noescape qualification set excludes these five inputs. Their
future target goldens remain preserved here, and their prerequisite count is
five. Existing generic-value fixtures exercise the definition-site `T` escape
contract through admitted function calls.
