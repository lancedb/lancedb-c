/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Base_lock - Abstract interface for distributed/local locking mechanisms.
 *
 * Implementations:
 *   - FileLock:  flock-based, for local/testing use (s3vector_concurrent_service.cpp)
 *   - NoOpLock:  no-op, always succeeds (s3vector_concurrent_service.cpp)
 *   - S3Lock:    S3 conditional-write based distributed lock (s3_lock.h / s3_lock.cpp)
 */

#pragma once

class Base_lock {
public:
    virtual ~Base_lock() = default;
    virtual bool lock_exclusive() = 0;
    virtual bool try_lock_exclusive() = 0;
    virtual void unlock() = 0;
    virtual bool is_locked() const = 0;
    virtual bool is_valid() const = 0;
};
