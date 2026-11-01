#pragma once

// Polls the dashboard for a pending "measure X" button press. See
// web/src/commands.js - GET /api/command is one-shot: the act of polling
// consumes whatever was queued.

typedef enum {
    MEASURE_NONE = 0,
    MEASURE_ECG,
    MEASURE_VITALS,
    MEASURE_TEMP,
    MEASURE_ALL,
} measure_cmd_t;

// Meant to be called about once a second while idle (see main.c's sender
// loop) - the HTTP round-trip itself is the rate limiter, no internal
// rate limiting here. Returns MEASURE_NONE on any network/parse error,
// same as "nothing pending" - a transient failure to poll is never fatal.
measure_cmd_t command_poll_tick(void);
