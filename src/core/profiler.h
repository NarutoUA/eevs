#pragma once

/**
 * @file profiler.h
 * @brief Zero-overhead compile-time wrapper for Tracy Profiler (`-DEEVS_ENABLE_TRACY=ON`).
 *
 * When `EEVS_ENABLE_TRACY` is disabled (default), all `EEVS_PROFILE_*` macros compile
 * to no-ops with zero runtime overhead.
 */

#if defined(EEVS_ENABLE_TRACY) && __has_include(<tracy/Tracy.hpp>)
#ifndef TRACY_ENABLE
#define TRACY_ENABLE
#endif
#ifndef TRACY_ON_DEMAND
#define TRACY_ON_DEMAND
#endif
#include <tracy/Tracy.hpp>

#define EEVS_PROFILE_SCOPE(name)        ZoneScopedN(name)
#define EEVS_PROFILE_FRAME(name)        FrameMarkNamed(name)
#define EEVS_PROFILE_PLOT(name, value)  TracyPlot(name, value)
#define EEVS_PROFILE_THREAD(name)       tracy::SetThreadName(name)
#else
#define EEVS_PROFILE_SCOPE(name)        ((void)0)
#define EEVS_PROFILE_FRAME(name)        ((void)0)
#define EEVS_PROFILE_PLOT(name, value)  ((void)0)
#define EEVS_PROFILE_THREAD(name)       ((void)0)
#endif
