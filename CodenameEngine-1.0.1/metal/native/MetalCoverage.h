// MetalCoverage.h - env-gated call-coverage probe for Lime's `lime_gl_*` primitives.
//
// DIAGNOSTIC ONLY. Every primitive entry point in this backend starts with
//
//     CNE_COV ("lime_gl_<name>");
//
// which increments a counter *if and only if* the process was started with
//
//     CNE_METAL_COVERAGE=1
//
// No rendering semantics are changed: with the env var unset the macro is a
// single predictable branch on a `false` global. With it set, the report is
// written to stderr and (optionally) to $CNE_METAL_COVERAGE_OUT:
//
//   * on normal process exit (atexit),
//   * on SIGTERM / SIGINT (so `kill -TERM <pid>` still produces a report),
//   * on SIGUSR1 (snapshot mid-run without exiting; the file is overwritten).
//
// Two sections are emitted: primitives that were entered (sorted by call count)
// and the registered primitives that were never entered. The never-entered list
// is pre-seeded from MetalCoverageNames.inc, generated from Lime's own
// OpenGLBindings.cpp registration block (the authoritative CFFI surface).
//
// This file is included exactly once, by MetalBindings.cpp, before `namespace
// lime` opens; the .inc fragments are included at the bottom of that same
// translation unit, so the macro is visible to all of them.

#ifndef CNE_METAL_COVERAGE_H
#define CNE_METAL_COVERAGE_H

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace lime {
namespace coverage {

	struct Entry
	{
		const char* name;
		unsigned long long count;
	};

	static const int kMaxEntries = 1024;
	static Entry g_entries[kMaxEntries];
	static int g_entryCount = 0;
	static bool g_enabled = false;
	static bool g_hasDumped = false;

	/**
		Returns the registry slot for `name`, appending a new (zero-count) entry if
		this primitive was not in the pre-seeded list. Called once per primitive
		through a function-local `static`, then only the array increment remains.
	*/
	inline int Slot (const char* name)
	{
		for (int i = 0; i < g_entryCount; ++i)
		{
			if (std::strcmp (g_entries[i].name, name) == 0)
			{
				return i;
			}
		}

		if (g_entryCount >= kMaxEntries)
		{
			return -1;
		}

		int i = g_entryCount++;
		g_entries[i].name = name;
		g_entries[i].count = 0;
		return i;
	}

	inline void Dump (const char* reason)
	{
		if (!g_enabled)
		{
			return;
		}

		bool reDump = g_hasDumped;
		g_hasDumped = true;

		std::vector<Entry> sorted (g_entries, g_entries + g_entryCount);
		std::sort (sorted.begin (), sorted.end (), [] (const Entry& a, const Entry& b)
		{
			return a.count > b.count;
		});

		int called = 0;

		for (const Entry& e : sorted)
		{
			if (e.count > 0)
			{
				++called;
			}
		}

		// Optional machine-readable report file.
		const char* path = std::getenv ("CNE_METAL_COVERAGE_OUT");

		if (path != nullptr && path[0] != 0)
		{
			// SIGUSR1 re-dumps: write a second, distinct snapshot instead of losing
			// the first one. The final (exit/SIGTERM) dump always overwrites the main file.
			std::string filePath = reDump ? (std::string (path) + ".snap") : std::string (path);
			FILE* out = std::fopen (filePath.c_str (), "w");

			if (out != nullptr)
			{
				std::fprintf (out, "# lime_gl_* call coverage (%s)\n", reason);
				std::fprintf (out, "# registry=%d called=%d\n", g_entryCount, called);
				std::fprintf (out, "# count\tprimitive\n");

				for (const Entry& e : sorted)
				{
					if (e.count > 0)
					{
						std::fprintf (out, "%llu\t%s\n", e.count, e.name);
					}
				}

				for (const Entry& e : sorted)
				{
					if (e.count == 0)
					{
						std::fprintf (out, "0\t%s\n", e.name);
					}
				}

				std::fflush (out);
				std::fclose (out);
			}
		}

		// Human-readable copy on stderr (shows up in the normal game log).
		std::fprintf (stderr, "\n[cne-cov] ===== lime_gl_* call coverage (%s) =====\n", reason);
		std::fprintf (stderr, "[cne-cov] registry=%d called=%d\n", g_entryCount, called);

		for (const Entry& e : sorted)
		{
			if (e.count > 0)
			{
				std::fprintf (stderr, "[cne-cov] %10llu  %s\n", e.count, e.name);
			}
		}

		std::fprintf (stderr, "[cne-cov] ----- never called (%d) -----\n", g_entryCount - called);

		for (const Entry& e : sorted)
		{
			if (e.count == 0)
			{
				std::fprintf (stderr, "[cne-cov] %10llu  %s\n", e.count, e.name);
			}
		}

		std::fprintf (stderr, "[cne-cov] ===== end coverage (%s) =====\n\n", reason);
		std::fflush (stderr);
	}

	inline void OnTerm (int)
	{
		Dump ("SIGTERM");
		std::_Exit (0);
	}

	inline void OnInt (int)
	{
		Dump ("SIGINT");
		std::_Exit (0);
	}

	inline void OnUsr1 (int)
	{
		Dump ("SIGUSR1");
	}

	struct Initializer
	{
		Initializer ()
		{
			const char* env = std::getenv ("CNE_METAL_COVERAGE");

			if (env == nullptr || env[0] != '1')
			{
				return;
			}

			g_enabled = true;

			// Pre-seed every primitive Lime registers, so never-called ones show up too.
			#include "MetalCoverageNames.inc"

			for (int i = 0; i < g_allCovNameCount; ++i)
			{
				Slot (g_allCovNames[i]);
			}

			std::atexit ([] () { Dump ("exit"); });
			std::signal (SIGTERM, OnTerm);
			std::signal (SIGINT, OnInt);
			std::signal (SIGUSR1, OnUsr1);

			const char* out = std::getenv ("CNE_METAL_COVERAGE_OUT");
			std::fprintf (stderr, "[cne-cov] enabled: %d primitives pre-registered%s%s\n",
				g_entryCount, out ? "; report file: " : "", out ? out : "");
			std::fflush (stderr);
		}
	};

	static Initializer g_coverageInitializer;

} // namespace coverage
} // namespace lime

/**
	Increment the call counter for `name` when coverage is enabled. This is a
	no-op branch when `CNE_METAL_COVERAGE` is unset; the function-local static
	resolves the registry slot exactly once per primitive.
*/
#define CNE_COV(name) \
	do { \
		if (::lime::coverage::g_enabled) { \
			static int cne_cov_slot = ::lime::coverage::Slot (name); \
			if (cne_cov_slot >= 0) { ++::lime::coverage::g_entries[cne_cov_slot].count; } \
		} \
	} while (0)

#endif // CNE_METAL_COVERAGE_H
