// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Drain a pipe a frame at a time, never waiting: the receive side of
 *         a Wayland drop (wl_data_offer.receive writes the payload into a
 *         pipe the CLIENT reads). A drop must never stall a frame, so the fd
 *         is non-blocking and each pump takes what is there; the transfer
 *         ends at EOF, or fails at a size cap or a time cap.
 *         Header-only, POSIX only; unit tested with a real pipe.
 */
#pragma once

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace dxr_input {

class FdDrain
{
public:
	enum class State
	{
		Idle,    //!< nothing in flight
		Reading, //!< more may come
		Done,    //!< EOF: data() is the whole payload
		Failed,  //!< read error, size cap or time cap; data() is partial
	};

	FdDrain() = default;
	~FdDrain()
	{
		reset();
	}
	FdDrain(const FdDrain &) = delete;
	FdDrain &
	operator=(const FdDrain &) = delete;

	/*!
	 * Take ownership of @p fd (made non-blocking here).
	 * @param max_bytes cap on the payload; more fails the transfer
	 * @param max_ms    cap on the transfer's wall time
	 */
	void
	start(int fd, size_t max_bytes, int max_ms)
	{
		reset();
		m_fd = fd;
		m_max = max_bytes;
		m_max_ms = max_ms;
		m_start = std::chrono::steady_clock::now();
		const int fl = fcntl(fd, F_GETFL, 0);
		if (fl >= 0) {
			fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		}
		m_state = State::Reading;
		m_why = nullptr;
	}

	//! Read what is available now. Never blocks. Returns the new state.
	State
	step()
	{
		if (m_state != State::Reading) {
			return m_state;
		}
		char buf[4096];
		for (;;) {
			const ssize_t n = read(m_fd, buf, sizeof(buf));
			if (n > 0) {
				if (m_data.size() + (size_t)n > m_max) {
					return finish(State::Failed, "size cap");
				}
				m_data.append(buf, (size_t)n);
				continue;
			}
			if (n == 0) {
				return finish(State::Done, nullptr);
			}
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}
			return finish(State::Failed, "read error");
		}
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
		                                                                      m_start)
		                    .count();
		if (ms > m_max_ms) {
			return finish(State::Failed, "time cap");
		}
		return m_state;
	}

	State
	state() const
	{
		return m_state;
	}
	const std::string &
	data() const
	{
		return m_data;
	}
	//! Why it failed ("size cap" / "time cap" / "read error"), else null.
	const char *
	why() const
	{
		return m_why;
	}

	//! Close the fd (if any) and go Idle.
	void
	reset()
	{
		if (m_fd >= 0) {
			close(m_fd);
			m_fd = -1;
		}
		m_data.clear();
		m_state = State::Idle;
	}

private:
	State
	finish(State s, const char *why)
	{
		if (m_fd >= 0) {
			close(m_fd);
			m_fd = -1;
		}
		m_state = s;
		m_why = why;
		return s;
	}

	int m_fd = -1;
	size_t m_max = 0;
	int m_max_ms = 0;
	std::chrono::steady_clock::time_point m_start{};
	std::string m_data;
	State m_state = State::Idle;
	const char *m_why = nullptr;
};

} // namespace dxr_input
