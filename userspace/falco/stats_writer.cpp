// SPDX-License-Identifier: Apache-2.0
/*
Copyright (C) 2023 The Falco Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef _WIN32
#include <sys/time.h>
#endif
#include <ctime>
#include <csignal>
#include <atomic>

#include <nlohmann/json.hpp>

#include "falco_common.h"
#include "stats_writer.h"
#include "logger.h"
#include "config_falco.h"
#include <libscap/strl.h>
#include <libscap/scap_vtable.h>

// note: ticker_t is an uint16_t, which is enough because we don't care about
// overflows here. Threads calling stats_writer::handle() will just
// check that this value changed since their last observation.
static std::atomic<stats_writer::ticker_t> s_timer((stats_writer::ticker_t) 0);
#if !defined(__APPLE__) && !defined(_WIN32)
static timer_t s_timerid;
#else
static uint16_t s_timerid;
#endif
// note: Workaround for older GLIBC versions (< 2.35), where calling timer_delete()
// with an invalid timer ID not returned by timer_create() causes a segfault because of
// a bug in GLIBC (https://sourceware.org/bugzilla/show_bug.cgi?id=28257).
// Just performing a nullptr check is not enough as even after creating the timer, s_timerid
// remains a nullptr somehow.
bool s_timerid_exists = false;

static void timer_handler(int signum)
{
	s_timer.fetch_add(1, std::memory_order_relaxed);
}

#if defined(_WIN32)
bool stats_writer::init_ticker(uint32_t interval_msec, std::string &err)
{
	return true;
}
#endif

#if defined(__APPLE__)
bool stats_writer::init_ticker(uint32_t interval_msec, std::string &err)
{
	struct sigaction handler = {};

	memset (&handler, 0, sizeof(handler));
	handler.sa_handler = &timer_handler;
	if (sigaction(SIGALRM, &handler, NULL) == -1)
	{
		err = std::string("Could not set up signal handler for periodic timer: ") + strerror(errno);
		return false;
	}

	struct sigevent sev = {};
	/* Create the timer */
	sev.sigev_notify = SIGEV_SIGNAL;
	sev.sigev_signo = SIGALRM;
	sev.sigev_value.sival_ptr = &s_timerid;

	return true;
}
#endif

#if defined(EMSCRIPTEN)
bool stats_writer::init_ticker(uint32_t interval_msec, std::string &err)
{
	struct itimerspec timer = {};
	struct sigaction handler = {};

	memset (&handler, 0, sizeof(handler));
	handler.sa_handler = &timer_handler;
	if (sigaction(SIGALRM, &handler, NULL) == -1)
	{
		err = std::string("Could not set up signal handler for periodic timer: ") + strerror(errno);
		return false;
	}

	struct sigevent sev = {};
	/* Create the timer */
	sev.sigev_notify = SIGEV_SIGNAL;
	sev.sigev_signo = SIGALRM;
	sev.sigev_value.sival_ptr = &s_timerid;

	timer.it_value.tv_sec = interval_msec / 1000;
	timer.it_value.tv_nsec = (interval_msec % 1000) * 1000 * 1000;
	timer.it_interval = timer.it_value;

	return true;
}
#endif

#if defined(__linux__)
bool stats_writer::init_ticker(uint32_t interval_msec, std::string &err)
{
	struct itimerspec timer = {};
	struct sigaction handler = {};

	memset (&handler, 0, sizeof(handler));
	handler.sa_handler = &timer_handler;
	if (sigaction(SIGALRM, &handler, NULL) == -1)
	{
		err = std::string("Could not set up signal handler for periodic timer: ") + strerror(errno);
		return false;
	}

	struct sigevent sev = {};
	/* Create the timer */
	sev.sigev_notify = SIGEV_SIGNAL;
	sev.sigev_signo = SIGALRM;
	sev.sigev_value.sival_ptr = &s_timerid;
	// delete any previously set timer
	if (s_timerid_exists)
	{
		if (timer_delete(s_timerid) == -1)
		{
			err = std::string("Could not delete previous timer: ") + strerror(errno);
			return false;
		}
		s_timerid_exists = false;
	}

	if (timer_create(CLOCK_MONOTONIC, &sev, &s_timerid) == -1)
	{
		err = std::string("Could not create periodic timer: ") + strerror(errno);
		return false;
	}
	s_timerid_exists = true;

	timer.it_value.tv_sec = interval_msec / 1000;
	timer.it_value.tv_nsec = (interval_msec % 1000) * 1000 * 1000;
	timer.it_interval = timer.it_value;

	if (timer_settime(s_timerid, 0, &timer, NULL) == -1)
	{
		err = std::string("Could not set up periodic timer: ") + strerror(errno);
		return false;
	}

	return true;
}
#endif

stats_writer::ticker_t stats_writer::get_ticker()
{
	return s_timer.load(std::memory_order_relaxed);
}

stats_writer::stats_writer(
		const std::shared_ptr<falco_outputs>& outputs,
		const std::shared_ptr<const falco_configuration>& config)
	: m_config(config)
{
	if (config->m_metrics_enabled)
	{
		/* m_outputs should always be initialized because we use it
		 * to extract output-queue stats in both cases: rule output and file output.
		 */
		m_outputs = outputs;

		if (!config->m_metrics_output_file.empty())
		{
			m_file_output.exceptions(std::ofstream::failbit | std::ofstream::badbit);
			m_file_output.open(config->m_metrics_output_file, std::ios_base::app);
			m_initialized = true;
		}

		if (config->m_metrics_stats_rule_enabled)
		{
			m_initialized = true;
		}
	}

	if (m_initialized)
	{
#ifndef __EMSCRIPTEN__
		// Adopt capacity for completeness, even if it's likely not relevant
		m_queue.set_capacity(config->m_outputs_queue_capacity);
		m_worker = std::thread(&stats_writer::worker, this);
#endif
	}
}

stats_writer::~stats_writer()
{
	if (m_initialized)
	{
#ifndef __EMSCRIPTEN__
		stop_worker();
#endif
		if (!m_config->m_metrics_output_file.empty())
		{
			m_file_output.close();
		}
		// delete timerID and reset timer
#ifdef __linux__
		if (s_timerid_exists)
		{
			timer_delete(s_timerid);
			s_timerid_exists = false;
		}
#endif
	}
}

void stats_writer::stop_worker()
{
	stats_writer::msg msg;
	msg.stop = true;
	push(msg);
	if(m_worker.joinable())
	{
		m_worker.join();
	}
}

inline void stats_writer::push(const stats_writer::msg& m)
{
	#ifndef __EMSCRIPTEN__
	if (!m_queue.try_push(m))
	{
		fprintf(stderr, "Fatal error: Stats queue reached maximum capacity. Exiting.\n");
		exit(EXIT_FAILURE);
	}
	#endif
}

void stats_writer::worker() noexcept
{
	stats_writer::msg m;
	bool use_outputs = m_config->m_metrics_stats_rule_enabled;
	bool use_file = !m_config->m_metrics_output_file.empty();
	auto tick = stats_writer::get_ticker();
	auto last_tick = tick;
	auto first_tick = tick;

	while(true)
	{
		// blocks until a message becomes availables
		#ifndef __EMSCRIPTEN__
		m_queue.pop(m);
		#endif
		if (m.stop)
		{
			return;
		}

		// this helps waiting for the first tick
		tick = stats_writer::get_ticker();
		if (first_tick != tick)
		{
			if (last_tick != tick)
			{
				m_total_samples++;
			}
			last_tick = tick;

			try
			{
				if (use_outputs)
				{
					std::string rule = "Falco internal: metrics snapshot";
					std::string msg = "Falco metrics snapshot";
					m_outputs->handle_msg(m.ts, falco_common::PRIORITY_INFORMATIONAL, msg, rule, m.output_fields);
				}

				if (use_file)
				{
					nlohmann::json jmsg;
					jmsg["sample"] = m_total_samples;
					jmsg["output_fields"] = m.output_fields;
					m_file_output << jmsg.dump() << std::endl;
				}
			}
			catch(const std::exception &e)
			{
				falco_logger::log(falco_logger::level::ERR, "stats_writer (worker): " + std::string(e.what()) + "\n");
			}
		}
	}
}

stats_writer::collector::collector(const std::shared_ptr<stats_writer>& writer)
	: m_writer(writer)
{
}

void stats_writer::collector::get_metrics_output_fields_wrapper(
		nlohmann::json& output_fields,
		const std::shared_ptr<sinsp>& inspector, uint64_t now,
		const std::string& src, uint64_t num_evts, double stats_snapshot_time_delta_sec)
{
	static const char* all_driver_engines[] = {
		BPF_ENGINE, KMOD_ENGINE, MODERN_BPF_ENGINE,
		SOURCE_PLUGIN_ENGINE, NODRIVER_ENGINE, GVISOR_ENGINE };
	const scap_agent_info* agent_info = inspector->get_agent_info();
	const scap_machine_info* machine_info = inspector->get_machine_info();

	/* Wrapper fields useful for statistical analyses and attributions. Always enabled. */
	output_fields["evt.time"] = now; /* Some ETLs may prefer a consistent timestamp within output_fields. */
	output_fields["falco.version"] = FALCO_VERSION;
	output_fields["falco.start_ts"] = agent_info->start_ts_epoch;
	output_fields["falco.duration_sec"] = (uint64_t)((now - agent_info->start_ts_epoch) / ONE_SECOND_IN_NS);
	output_fields["falco.kernel_release"] = agent_info->uname_r;
	output_fields["falco.host_boot_ts"] = machine_info->boot_ts_epoch;
	output_fields["falco.hostname"] = machine_info->hostname; /* Explicitly add hostname to log msg in case hostname rule output field is disabled. */
	output_fields["falco.host_num_cpus"] = machine_info->num_cpus;
	output_fields["falco.outputs_queue_num_drops"] = m_writer->m_outputs->get_outputs_queue_num_drops();

	output_fields["evt.source"] = src;
	for (size_t i = 0; i < sizeof(all_driver_engines) / sizeof(const char*); i++)
	{
		if (inspector->check_current_engine(all_driver_engines[i]))
		{
			output_fields["scap.engine_name"] = all_driver_engines[i];
			break;
		}
	}

	/* Falco userspace event counters. Always enabled. */
	if (m_last_num_evts != 0 && stats_snapshot_time_delta_sec > 0)
	{
		/* Successfully processed userspace event rate. */
		output_fields["falco.evts_rate_sec"] = std::round((double)((num_evts - m_last_num_evts) / (double)stats_snapshot_time_delta_sec) * 10.0) / 10.0; // round to 1 decimal
	}
	output_fields["falco.num_evts"] = num_evts;
	output_fields["falco.num_evts_prev"] = m_last_num_evts;
	m_last_num_evts = num_evts;
}

void stats_writer::collector::get_metrics_output_fields_additional(
		nlohmann::json& output_fields,
		const std::shared_ptr<sinsp>& inspector,
		double stats_snapshot_time_delta_sec, const std::string& src)
{
	const scap_agent_info* agent_info = inspector->get_agent_info();

#if !defined(MINIMAL_BUILD) and !defined(__EMSCRIPTEN__)
	uint32_t nstats = 0;
	int32_t rc = 0;
	uint32_t flags = m_writer->m_config->m_metrics_flags;

	auto buffer = inspector->get_sinsp_stats_v2_buffer();
	auto sinsp_stats_v2 = inspector->get_sinsp_stats_v2();
	sinsp_thread_manager* thread_manager = inspector->m_thread_manager.get();
	const scap_stats_v2* sinsp_stats_v2_snapshot = libsinsp::stats::get_sinsp_stats_v2(flags, agent_info, thread_manager, sinsp_stats_v2, buffer, &nstats, &rc);

	uint32_t base_stat = 0;
	// todo @incertum this needs to become better with the next proper stats refactor in libs 0.15.0
	if ((flags & PPM_SCAP_STATS_STATE_COUNTERS) && !(flags & PPM_SCAP_STATS_RESOURCE_UTILIZATION))
	{
		base_stat = SINSP_STATS_V2_N_THREADS;
	}

	if (sinsp_stats_v2_snapshot && rc == 0 && nstats > 0)
	{
		for(uint32_t stat = base_stat; stat < nstats; stat++)
		{
			if (sinsp_stats_v2_snapshot[stat].name[0] == '\0')
			{
				break;
			}
			char metric_name[STATS_NAME_MAX] = "falco.";
			strlcat(metric_name, sinsp_stats_v2_snapshot[stat].name, sizeof(metric_name));
			// todo @incertum temporary fix for n_fds and n_threads, type assignment was missed in libs, will be fixed in libs 0.15.0
			if (strncmp(sinsp_stats_v2_snapshot[stat].name, "n_fds", 6) == 0 || strncmp(sinsp_stats_v2_snapshot[stat].name, "n_threads", 10) == 0)
			{
				output_fields[metric_name] = sinsp_stats_v2_snapshot[stat].value.u64;
			}

			switch(sinsp_stats_v2_snapshot[stat].type)
			{
			case STATS_VALUE_TYPE_U64:
				if (sinsp_stats_v2_snapshot[stat].value.u64 == 0 && !m_writer->m_config->m_metrics_include_empty_values)
				{
					break;
				}
				if (m_writer->m_config->m_metrics_convert_memory_to_mb)
				{
					if (strncmp(sinsp_stats_v2_snapshot[stat].name, "container_memory_used", 22) == 0) // exact str match
					{
						output_fields[metric_name] = (uint64_t)(sinsp_stats_v2_snapshot[stat].value.u64 / (double)1024 / (double)1024);

					} else if (strncmp(sinsp_stats_v2_snapshot[stat].name, "memory_", 7) == 0) // prefix match
					{
						output_fields[metric_name] = (uint64_t)(sinsp_stats_v2_snapshot[stat].value.u64 / (double)1024);
					} else
					{
						output_fields[metric_name] = sinsp_stats_v2_snapshot[stat].value.u64;
					}
				}
				else
				{
					output_fields[metric_name] = sinsp_stats_v2_snapshot[stat].value.u64;
				}
				break;
			case STATS_VALUE_TYPE_U32:
				if (sinsp_stats_v2_snapshot[stat].value.u32 == 0 && !m_writer->m_config->m_metrics_include_empty_values)
				{
					break;
				}
				if (m_writer->m_config->m_metrics_convert_memory_to_mb && strncmp(sinsp_stats_v2_snapshot[stat].name, "memory_", 7) == 0) // prefix match
				{
					output_fields[metric_name] = (uint32_t)(sinsp_stats_v2_snapshot[stat].value.u32 / (double)1024);
				}
				else
				{
					output_fields[metric_name] = sinsp_stats_v2_snapshot[stat].value.u32;
				}
				break;
			case STATS_VALUE_TYPE_D:
				if (sinsp_stats_v2_snapshot[stat].value.d == 0 && !m_writer->m_config->m_metrics_include_empty_values)
				{
					break;
				}
				output_fields[metric_name] = sinsp_stats_v2_snapshot[stat].value.d;
				break;
			default:
				break;
			}
		}
	}

	if (src != falco_common::syscall_source)
	{
		return;
	}

	/* Kernel side stats counters and libbpf stats if applicable. */
	nstats = 0;
	rc = 0;
	if (!(inspector->check_current_engine(BPF_ENGINE) || inspector->check_current_engine(MODERN_BPF_ENGINE)))
	{
		flags &= ~PPM_SCAP_STATS_LIBBPF_STATS;
	}

	// Note: ENGINE_FLAG_BPF_STATS_ENABLED check has been moved to libs, that is, when libbpf stats is not enabled
	// in the kernel settings we won't collect them even if the end user enabled the libbpf stats option

	const scap_stats_v2* scap_stats_v2_snapshot = inspector->get_capture_stats_v2(flags, &nstats, &rc);
	if (scap_stats_v2_snapshot && nstats > 0 && rc == 0)
	{
		/* Cache n_evts and n_drops to derive n_drops_perc. */
		uint64_t n_evts = 0;
		uint64_t n_drops = 0;
		uint64_t n_evts_delta = 0;
		uint64_t n_drops_delta = 0;
		for(uint32_t stat = 0; stat < nstats; stat++)
		{
			if (scap_stats_v2_snapshot[stat].name[0] == '\0')
			{
				break;
			}
			// todo: as we expand scap_stats_v2 prefix may be pushed to scap or we may need to expand
			// functionality here for example if we add userspace syscall counters that should be prefixed w/ `falco.`
			char metric_name[STATS_NAME_MAX] = "scap.";
			strlcat(metric_name, scap_stats_v2_snapshot[stat].name, sizeof(metric_name));
			switch(scap_stats_v2_snapshot[stat].type)
			{
			case STATS_VALUE_TYPE_U64:
				/* Always send high level n_evts related fields, even if zero. */
				if (strncmp(scap_stats_v2_snapshot[stat].name, "n_evts", 7) == 0) // exact not prefix match here
				{
					n_evts = scap_stats_v2_snapshot[stat].value.u64;
					output_fields[metric_name] = n_evts;
					output_fields["scap.n_evts_prev"] = m_last_n_evts;
					n_evts_delta = n_evts - m_last_n_evts;
					if (n_evts_delta != 0 && stats_snapshot_time_delta_sec > 0)
					{
						/* n_evts is total number of kernel side events. */
						output_fields["scap.evts_rate_sec"] = std::round((double)(n_evts_delta / stats_snapshot_time_delta_sec) * 10.0) / 10.0; // round to 1 decimal
					}
					else
					{
						output_fields["scap.evts_rate_sec"] = (double)(0);
					}
					m_last_n_evts = n_evts;
				}
				/* Always send high level n_drops related fields, even if zero. */
				else if (strncmp(scap_stats_v2_snapshot[stat].name, "n_drops", 8) == 0) // exact not prefix match here
				{
					n_drops = scap_stats_v2_snapshot[stat].value.u64;
					output_fields[metric_name] = n_drops;
					output_fields["scap.n_drops_prev"] = m_last_n_drops;
					n_drops_delta = n_drops - m_last_n_drops;
					if (n_drops_delta != 0 && stats_snapshot_time_delta_sec > 0)
					{
						/* n_drops is total number of kernel side event drops. */
						output_fields["scap.evts_drop_rate_sec"] = std::round((double)(n_drops_delta / stats_snapshot_time_delta_sec) * 10.0) / 10.0; // round to 1 decimal
					}
					else
					{
						output_fields["scap.evts_drop_rate_sec"] = (double)(0);
					}
					m_last_n_drops = n_drops;
				}
				if (scap_stats_v2_snapshot[stat].value.u64 == 0 && !m_writer->m_config->m_metrics_include_empty_values)
				{
					break;
				}
				output_fields[metric_name] = scap_stats_v2_snapshot[stat].value.u64;
				break;
			default:
				break;
			}
		}
		/* n_drops_perc needs to be calculated outside the loop given no field ordering guarantees.
		 * Always send n_drops_perc, even if zero. */
		if(n_evts_delta > 0)
		{
			output_fields["scap.n_drops_perc"] = (double)((100.0 * n_drops_delta) / n_evts_delta);
		}
		else
		{
			output_fields["scap.n_drops_perc"] = (double)(0);
		}
	}
#endif
}

void stats_writer::collector::collect(const std::shared_ptr<sinsp>& inspector, const std::string &src, uint64_t num_evts)
{
	if (m_writer->has_output())
	{
		/* Collect stats / metrics once per ticker period. */
		auto tick = stats_writer::get_ticker();
		if (tick != m_last_tick)
		{
			m_last_tick = tick;
			auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			uint64_t stats_snapshot_time_delta = 0;
			if (m_last_now != 0)
			{
				stats_snapshot_time_delta = now - m_last_now;
			}
			m_last_now = now;
			double stats_snapshot_time_delta_sec = (stats_snapshot_time_delta / (double)ONE_SECOND_IN_NS);

			/* Get respective metrics output_fields. */
			nlohmann::json output_fields;
			get_metrics_output_fields_wrapper(output_fields, inspector, now, src, num_evts, stats_snapshot_time_delta_sec);
			get_metrics_output_fields_additional(output_fields, inspector, stats_snapshot_time_delta_sec, src);

			/* Send message in the queue */
			stats_writer::msg msg;
			msg.ts = now;
			msg.source = src;
			msg.output_fields = std::move(output_fields);
			m_writer->push(msg);
		}
	}
}
