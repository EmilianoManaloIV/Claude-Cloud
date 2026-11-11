#pragma once

#include "core/object/worker_thread_pool.h"

#include <type_traits>

// Runs p_func(index) for every index in [0, p_count) on the worker thread
// pool and waits for completion. Small jobs run inline.
template <typename F>
void tetra_parallel_for(int p_count, int p_min_parallel, F &&p_func) {
	typedef std::remove_reference_t<F> Function;
	if (p_count < p_min_parallel || p_count < 2) {
		for (int i = 0; i < p_count; i++) {
			p_func(i);
		}
		return;
	}
	struct Trampoline {
		static void run(void *p_userdata, uint32_t p_index) {
			(*static_cast<Function *>(p_userdata))(int(p_index));
		}
	};
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	WorkerThreadPool::GroupID group = pool->add_native_group_task(&Trampoline::run, &p_func, p_count, -1, true, "TetraPaint");
	pool->wait_for_group_task_completion(group);
}

// Rows handed to one worker task when a loop over image rows is split up.
static const int TETRA_ROW_BLOCK = 32;

// Runs p_func(first_row, end_row) over [0, p_rows) in blocks on the pool.
template <typename F>
void tetra_parallel_rows(int p_rows, int p_width, F &&p_func) {
	const int blocks = (p_rows + TETRA_ROW_BLOCK - 1) / TETRA_ROW_BLOCK;
	const bool small = int64_t(p_rows) * p_width < 64 * 64;
	tetra_parallel_for(blocks, small ? blocks + 1 : 2, [&](int p_block) {
		const int first = p_block * TETRA_ROW_BLOCK;
		const int end = first + TETRA_ROW_BLOCK < p_rows ? first + TETRA_ROW_BLOCK : p_rows;
		p_func(first, end);
	});
}

// Linear float to 8-bit sRGB with correct rounding.
uint8_t tetra_linear_to_srgb8(float p_value);

// Separable Gaussian blur of an interleaved float image with p_channels
// components; edges are clamped. p_src and p_dst may be the same buffer.
void tetra_gaussian_blur(const float *p_src, float *p_dst, int p_width, int p_height, int p_channels, float p_sigma_x, float p_sigma_y);
