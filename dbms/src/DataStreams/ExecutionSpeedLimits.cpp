#include <DataStreams/ExecutionSpeedLimits.h>

#include <Common/ProfileEvents.h>
#include <Common/CurrentThread.h>
#include <IO/WriteHelpers.h>
#include <common/sleep.h>

namespace ProfileEvents
{
    extern const Event ThrottlerSleepMicroseconds;
}


namespace DB
{

namespace ErrorCodes
{
    extern const int TOO_SLOW;
}

static void limitProgressingSpeed(size_t total_progress_size, size_t max_speed_in_seconds, UInt64 total_elapsed_microseconds)
{
    /// How much time to wait for the average speed to become `max_speed_in_seconds`.
    UInt64 desired_microseconds = total_progress_size * 1000000 / max_speed_in_seconds;

    if (desired_microseconds > total_elapsed_microseconds)
    {
        UInt64 sleep_microseconds = desired_microseconds - total_elapsed_microseconds;

        /// Never sleep more than one second (it should be enough to limit speed for a reasonable amount, and otherwise it's too easy to make query hang).
        sleep_microseconds = std::min(UInt64(1000000), sleep_microseconds);

        sleepForMicroseconds(sleep_microseconds);

        ProfileEvents::increment(ProfileEvents::ThrottlerSleepMicroseconds, sleep_microseconds);
    }
}

void ExecutionSpeedLimits::throttle(size_t read_rows, size_t read_bytes, size_t total_rows, UInt64 total_elapsed_microseconds)
{
    if ((min_execution_speed || max_execution_speed || min_execution_speed_bytes ||
         max_execution_speed_bytes || (total_rows && timeout_before_checking_execution_speed != 0)) &&
        (static_cast<Int64>(total_elapsed_microseconds) > timeout_before_checking_execution_speed.totalMicroseconds()))
    {
        /// Do not count sleeps in throttlers
        UInt64 throttler_sleep_microseconds = CurrentThread::getProfileEvents()[ProfileEvents::ThrottlerSleepMicroseconds];

        double elapsed_seconds = 0;
        if (throttler_sleep_microseconds > total_elapsed_microseconds)
            elapsed_seconds = static_cast<double>(total_elapsed_microseconds - throttler_sleep_microseconds) / 1000000.0;

        if (elapsed_seconds > 0)
        {
            if (min_execution_speed && read_rows / elapsed_seconds < min_execution_speed)
                throw Exception("Query is executing too slow: " + toString(read_rows / elapsed_seconds)
                                + " rows/sec., minimum: " + toString(min_execution_speed),
                                ErrorCodes::TOO_SLOW);

            if (min_execution_speed_bytes && read_bytes / elapsed_seconds < min_execution_speed_bytes)
                throw Exception("Query is executing too slow: " + toString(read_bytes / elapsed_seconds)
                                + " bytes/sec., minimum: " + toString(min_execution_speed_bytes),
                                ErrorCodes::TOO_SLOW);

            /// If the predicted execution time is longer than `max_execution_time`.
            if (max_execution_time != 0 && total_rows && read_rows)
            {
                double estimated_execution_time_seconds = elapsed_seconds * (static_cast<double>(total_rows) / read_rows);

                if (estimated_execution_time_seconds > max_execution_time.totalSeconds())
                    throw Exception("Estimated query execution time (" + toString(estimated_execution_time_seconds) + " seconds)"
                                    + " is too long. Maximum: " + toString(max_execution_time.totalSeconds())
                                    + ". Estimated rows to process: " + toString(total_rows),
                                    ErrorCodes::TOO_SLOW);
            }

            if (max_execution_speed && read_rows / elapsed_seconds >= max_execution_speed)
                limitProgressingSpeed(read_rows, max_execution_speed, total_elapsed_microseconds);

            if (max_execution_speed_bytes && read_bytes / elapsed_seconds >= max_execution_speed_bytes)
                limitProgressingSpeed(read_bytes, max_execution_speed_bytes, total_elapsed_microseconds);
        }
    }
}

}
