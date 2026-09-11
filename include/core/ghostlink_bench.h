#ifndef GHOSTLINK_BENCH_H
#define GHOSTLINK_BENCH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * GhostLink throughput benchmark (glbench).
 *
 * A diagnostic tool that measures real UART link throughput in both directions
 * over a dedicated stream channel. It changes no transport behavior: it only
 * reads the existing counters and pushes payload on an otherwise-unused channel.
 *
 * Registers the receiver sink on COMM_STREAM_CHANNEL_BENCH. Idempotent, and a
 * safe no-op on boards where the comm manager is not initialized.
 */
void ghostlink_bench_init(void);

/* CLI entry point: `glbench [send|recv|both] [kb]`, `glbench stop`, `glbench status`. */
void handle_glbench_cmd(int argc, char **argv);

/*
 * Peer-side command hook for "bench <sub>" commands arriving over GhostLink.
 * Handles `arm`, `tx <kb>` and `report`. Returns true when the command was
 * consumed by the benchmark so the caller stops dispatching it.
 */
bool ghostlink_bench_handle_command(const char *command, const char *data);

/* True while a run is active (for UI gating). */
bool ghostlink_bench_is_running(void);

#endif // GHOSTLINK_BENCH_H
