/*
 * From Augustus (https://github.com/Keriew/augustus), commit 63813587869eda5557fe5f1e0e8cea024ff8bef7,
 * file src/core/time.h. Copyright (C) the Augustus contributors.
 * Licensed under the GNU Affero General Public License v3.0 (see LICENSE in the project root).
 * The original file has no copyright header of its own; this notice is the only change (docs/third_party.md).
 */
#ifndef CORE_TIME_H
#define CORE_TIME_H

#include <stdint.h>

/**
 * @file
 * Time tracking functions.
 */

/**
 * Time in millisecond-precision. Use only for time difference calculations.
 */
typedef uint64_t time_millis;

/**
 * Gets the current time
 * @return Current time in milliseconds
 */
time_millis time_get_millis(void);

/**
 * Sets the current time
 * @param millis Current milliseconds
 */
void time_set_millis(time_millis millis);

#endif // CORE_TIME_H
