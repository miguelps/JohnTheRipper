/*
 * This software is Copyright (c) 2012 Lukas Odzioba <ukasz at openwall.net>
 * and Copyright (c) 2012 magnum, and it is hereby released to the general
 * public under the following terms: Redistribution and use in source and
 * binary forms, with or without modification, are permitted.
 *
 * Code was at some point based on Aircrack-ng source
 */
#include <string.h>

#include "arch.h"
#include "formats.h"
#include "common.h"
#include "misc.h"
#include "config.h"
#include "options.h"
#include "common-opencl.h"

static cl_mem mem_in, mem_out, mem_salt, mem_state, pinned_in, pinned_out;
static cl_kernel wpapsk_init, wpapsk_loop, wpapsk_pass2, wpapsk_final_md5, wpapsk_final_sha1;
static unsigned int v_width = 1;	/* Vector width of kernel */
static size_t key_buf_size;
static unsigned int *inbuffer;

#define JOHN_OCL_WPAPSK
#include "wpapsk.h"

#define FORMAT_LABEL		"wpapsk-opencl"
#define FORMAT_NAME		"WPA/WPA2 PSK"
#define ALGORITHM_NAME		"PBKDF2-SHA1 OpenCL"

#define BENCHMARK_LENGTH	-1

#define ITERATIONS		4095
#define HASH_LOOPS		105 /* Must be made from factors 3, 3, 5, 7, 13 */

#define MIN_KEYS_PER_CRYPT	1
#define MAX_KEYS_PER_CRYPT	1

#define OCL_CONFIG		"wpapsk"

#define MIN(a, b)		(((a) > (b)) ? (b) : (a))
#define MAX(a, b)		(((a) > (b)) ? (a) : (b))

/* This handles all sizes */
#define GETPOS(i, index)	(((index) % v_width) * 4 + ((i) & ~3U) * v_width + (((i) & 3) ^ 3) + ((index) / v_width) * 64 * v_width)
/* This is faster but can't handle size 3 */
//#define GETPOS(i, index)	(((index) & (v_width - 1)) * 4 + ((i) & ~3U) * v_width + (((i) & 3) ^ 3) + ((index) / v_width) * 64 * v_width)

extern wpapsk_salt currentsalt;
extern mic_t *mic;
extern hccap_t hccap;

typedef struct {
	cl_uint W[5];
	cl_uint ipad[5];
	cl_uint opad[5];
	cl_uint out[5];
	cl_uint partial[5];
} wpapsk_state;

static void create_clobj(size_t gws, struct fmt_main *self)
{
	global_work_size = gws;
	gws *= v_width;
	self->params.max_keys_per_crypt = gws;

	key_buf_size = 64 * gws;

	/// Allocate memory
	pinned_in = clCreateBuffer(context[gpu_id], CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR, key_buf_size, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating pinned in");
	mem_in = clCreateBuffer(context[gpu_id], CL_MEM_READ_ONLY, key_buf_size, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating mem in");
	inbuffer = clEnqueueMapBuffer(queue[gpu_id], pinned_in, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE, 0, key_buf_size, 0, NULL, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error mapping page-locked memory");

	mem_state = clCreateBuffer(context[gpu_id], CL_MEM_READ_WRITE, sizeof(wpapsk_state) * gws, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating mem_state");

	mem_salt = clCreateBuffer(context[gpu_id], CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(wpapsk_salt), &currentsalt, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating mem setting");

	pinned_out = clCreateBuffer(context[gpu_id], CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR, sizeof(mic_t) * gws, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating pinned out");
	mem_out = clCreateBuffer(context[gpu_id], CL_MEM_WRITE_ONLY, sizeof(mic_t) * gws, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating mem out");
	mic = clEnqueueMapBuffer(queue[gpu_id], pinned_out, CL_TRUE, CL_MAP_READ, 0, sizeof(mic_t) * gws, 0, NULL, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error mapping page-locked memory");

	HANDLE_CLERROR(clSetKernelArg(wpapsk_init, 0, sizeof(mem_in), &mem_in), "Error while setting mem_in kernel argument");
	HANDLE_CLERROR(clSetKernelArg(wpapsk_init, 1, sizeof(mem_salt), &mem_salt), "Error while setting mem_salt kernel argument");
	HANDLE_CLERROR(clSetKernelArg(wpapsk_init, 2, sizeof(mem_state), &mem_state), "Error while setting mem_state kernel argument");

	HANDLE_CLERROR(clSetKernelArg(wpapsk_loop, 0, sizeof(mem_state), &mem_state), "Error while setting mem_state kernel argument");

	HANDLE_CLERROR(clSetKernelArg(wpapsk_pass2, 0, sizeof(mem_salt), &mem_salt), "Error while setting mem_salt kernel argument");
	HANDLE_CLERROR(clSetKernelArg(wpapsk_pass2, 1, sizeof(mem_state), &mem_state), "Error while setting mem_state kernel argument");

	HANDLE_CLERROR(clSetKernelArg(wpapsk_final_md5, 0, sizeof(mem_state), &mem_state), "Error while setting mem_state kernel argument");
	HANDLE_CLERROR(clSetKernelArg(wpapsk_final_md5, 1, sizeof(mem_salt), &mem_salt), "Error while setting mem_salt kernel argument");
	HANDLE_CLERROR(clSetKernelArg(wpapsk_final_md5, 2, sizeof(mem_out), &mem_out), "Error while setting mem_out kernel argument");

	HANDLE_CLERROR(clSetKernelArg(wpapsk_final_sha1, 0, sizeof(mem_state), &mem_state), "Error while setting mem_state kernel argument");
	HANDLE_CLERROR(clSetKernelArg(wpapsk_final_sha1, 1, sizeof(mem_salt), &mem_salt), "Error while setting mem_salt kernel argument");
	HANDLE_CLERROR(clSetKernelArg(wpapsk_final_sha1, 2, sizeof(mem_out), &mem_out), "Error while setting mem_out kernel argument");
}

static void release_clobj(void)
{
	HANDLE_CLERROR(clEnqueueUnmapMemObject(queue[gpu_id], pinned_in, inbuffer, 0, NULL, NULL), "Error Unmapping mem in");
	HANDLE_CLERROR(clEnqueueUnmapMemObject(queue[gpu_id], pinned_out, mic, 0, NULL, NULL), "Error Unmapping mem in");
	HANDLE_CLERROR(clFinish(queue[gpu_id]), "Error releasing memory mappings");

	HANDLE_CLERROR(clReleaseMemObject(pinned_in), "Release pinned_in");
	HANDLE_CLERROR(clReleaseMemObject(pinned_out), "Release pinned_out");
	HANDLE_CLERROR(clReleaseMemObject(mem_in), "Release pinned_in");
	HANDLE_CLERROR(clReleaseMemObject(mem_out), "Release mem_out");
	HANDLE_CLERROR(clReleaseMemObject(mem_salt), "Release mem_salt");
	HANDLE_CLERROR(clReleaseMemObject(mem_state), "Release mem state");
}

static void done(void)
{
	release_clobj();

	HANDLE_CLERROR(clReleaseKernel(wpapsk_init), "Release Kernel");
	HANDLE_CLERROR(clReleaseKernel(wpapsk_loop), "Release Kernel");
	HANDLE_CLERROR(clReleaseKernel(wpapsk_pass2), "Release Kernel");
	HANDLE_CLERROR(clReleaseKernel(wpapsk_final_md5), "Release Kernel");
	HANDLE_CLERROR(clReleaseKernel(wpapsk_final_sha1), "Release Kernel");

	HANDLE_CLERROR(clReleaseProgram(program[gpu_id]), "Release Program");
}

static void clear_keys(void) {
	memset(inbuffer, 0, key_buf_size);
	new_keys = 1;
}

static void set_key(char *key, int index)
{
	int i;
	int length = strlen(key);

	for (i = 0; i < length; i++)
		((char*)inbuffer)[GETPOS(i, index)] = key[i];
	new_keys = 1;
}

static char* get_key(int index)
{
	static char ret[PLAINTEXT_LENGTH + 1];
	int i = 0;

	while ((ret[i] = ((char*)inbuffer)[GETPOS(i, index)]))
		i++;

	return ret;
}

static void *salt(char *ciphertext);
static void set_salt(void *salt);

static cl_ulong gws_test(size_t gws, struct fmt_main *self)
{
	cl_ulong startTime, endTime;
	cl_command_queue queue_prof;
	cl_event Event[7];
	cl_int ret_code;
	int i;
	size_t scalar_gws = v_width * gws;
	size_t *lws = local_work_size ? &local_work_size : NULL;

	create_clobj(gws, self);
	queue_prof = clCreateCommandQueue(context[gpu_id], devices[gpu_id], CL_QUEUE_PROFILING_ENABLE, &ret_code);
	for (i = 0; i < scalar_gws; i++)
		self->methods.set_key(tests[0].plaintext, i);
	self->methods.set_salt(self->methods.salt(tests[0].ciphertext));

	/// Copy data to gpu
	HANDLE_CLERROR(clEnqueueWriteBuffer(queue_prof, mem_in, CL_FALSE, 0, key_buf_size, inbuffer, 0, NULL, &Event[0]), "Copy data to gpu");
	HANDLE_CLERROR(clEnqueueWriteBuffer(queue_prof, mem_salt, CL_FALSE, 0, sizeof(wpapsk_salt), &currentsalt, 0, NULL, &Event[1]), "Copy setting to gpu");

	/// Run kernels
	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue_prof, wpapsk_init, 1, NULL, &global_work_size, lws, 0, NULL, &Event[2]), "Run initial kernel");

	//for (i = 0; i < ITERATIONS / HASH_LOOPS - 1; i++)
	// warm-up run without measuring
	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue_prof, wpapsk_loop, 1, NULL, &global_work_size, lws, 0, NULL, NULL), "Run loop kernel");
	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue_prof, wpapsk_loop, 1, NULL, &global_work_size, lws, 0, NULL, &Event[3]), "Run loop kernel");

	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue_prof, wpapsk_pass2, 1, NULL, &global_work_size, lws, 0, NULL, &Event[4]), "Run intermediate kernel");

	//for (i = 0; i < ITERATIONS / HASH_LOOPS; i++)
	//	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue_prof, wpapsk_loop, 1, NULL, &global_work_size, lws, 0, NULL, NULL), "Run loop kernel (2nd)");

	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue_prof, wpapsk_final_sha1, 1, NULL, &global_work_size, lws, 0, NULL, &Event[5]), "Run final kernel");

	/// Read the result back
	HANDLE_CLERROR(clEnqueueReadBuffer(queue_prof, mem_out, CL_TRUE, 0, sizeof(mic_t) * scalar_gws, mic, 0, NULL, &Event[6]), "Copy result back");

#if 1
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[0],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[1],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	if (options.verbosity > 3)
		fprintf(stderr, "keys xfer %.2f us, ", (endTime-startTime)/1000.);

	HANDLE_CLERROR(clGetEventProfilingInfo(Event[2],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[2],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	if (options.verbosity > 3)
		fprintf(stderr, "1st kernel %.2f ms, ", (endTime-startTime)/1000000.);
#endif

	HANDLE_CLERROR(clGetEventProfilingInfo(Event[3],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[3],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");

	if (options.verbosity > 3)
		fprintf(stderr, "loop kernel %.2f ms x %u = %.2f s, ", (endTime - startTime)/1000000., 2 * ITERATIONS/HASH_LOOPS, 2 * (ITERATIONS/HASH_LOOPS) * (endTime - startTime) / 1000000000.);

	/* 200 ms duration limit */
	if ((endTime - startTime) > 200000000) {
		if (options.verbosity > 3)
			fprintf(stderr, "- exceeds 200 ms\n");
		clReleaseCommandQueue(queue_prof);
		release_clobj();
		return 0;
	}

#if 1
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[4],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[4],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	if (options.verbosity > 3)
		fprintf(stderr, "pass2 kernel %.2f ms, ", (endTime-startTime)/1000000.);

	HANDLE_CLERROR(clGetEventProfilingInfo(Event[5],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[5],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	if (options.verbosity > 3)
		fprintf(stderr, "result xfer %.2f us\n", (endTime-startTime)/1000.);

	HANDLE_CLERROR(clGetEventProfilingInfo(Event[6],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[6],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	if (options.verbosity > 3)
		fprintf(stderr, "final kernel %.2f ms, ", (endTime-startTime)/1000000.);
#endif

	HANDLE_CLERROR(clGetEventProfilingInfo(Event[3],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[3],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	clReleaseCommandQueue(queue_prof);
	release_clobj();

	return (endTime - startTime) * 2 * (ITERATIONS / HASH_LOOPS - 1);
}

static void find_best_gws(struct fmt_main *self)
{
	int num;
	cl_ulong run_time, min_time = CL_ULONG_MAX;
	unsigned int SHAspeed, bestSHAspeed = 0;
	int optimal_gws = get_kernel_preferred_multiple(gpu_id,
	                                                crypt_kernel);
	const int sha1perkey = 2 * ITERATIONS * 2 + 6 + 10;

	if (options.verbosity > 3) {
		fprintf(stderr, "Calculating best keys per crypt (GWS) for LWS=%zd and max. 200ms duration.\n\n", local_work_size);
		fprintf(stderr, "Raw GPU speed figures including buffer transfers:\n");
	}

	for (num = optimal_gws; num; num *= 2) {
		if (!(run_time = gws_test(num, self)))
			break;

		SHAspeed = sha1perkey * (1000000000UL * v_width * num / run_time);

		if (run_time < min_time)
			min_time = run_time;

		if (options.verbosity > 3)
			fprintf(stderr, "gws %6d%8llu c/s%14u sha1/s%8.2f sec per crypt_all()", num, (1000000000ULL * v_width * num / run_time), SHAspeed, (float)run_time / 1000000000.);
		else
			advance_cursor();

		if (((float)run_time / (float)min_time) < ((float)SHAspeed / (float)bestSHAspeed)) {
			if (options.verbosity > 3)
				fprintf(stderr, "!\n");
			bestSHAspeed = SHAspeed;
			optimal_gws = num;
		} else {
			if (SHAspeed > (bestSHAspeed)) {
				if (options.verbosity > 3)
					fprintf(stderr, "+\n");
				bestSHAspeed = SHAspeed;
				optimal_gws = num;
				continue;
			}
			if (options.verbosity > 3)
				fprintf(stderr, "\n");
		}
	}
	global_work_size = optimal_gws;
}

static int crypt_all(int *pcount, struct db_salt *salt);
static int crypt_all_benchmark(int *pcount, struct db_salt *salt);

static void init(struct fmt_main *self)
{
	char build_opts[128];
	cl_ulong maxsize, maxsize2, max_mem;
	static char valgo[32] = "";

	assert(sizeof(hccap_t) == HCCAP_SIZE);

	if ((v_width = opencl_get_vector_width(gpu_id,
	                                       sizeof(cl_int))) > 1) {
		/* Run vectorized kernel */
		snprintf(valgo, sizeof(valgo),
		         ALGORITHM_NAME " %ux", v_width);
		self->params.algorithm_name = valgo;
	}

	snprintf(build_opts, sizeof(build_opts),
	         "-DHASH_LOOPS=%u -DITERATIONS=%u "
	         "-DPLAINTEXT_LENGTH=%u -DV_WIDTH=%u",
	         HASH_LOOPS, ITERATIONS,
	         PLAINTEXT_LENGTH, v_width);
	opencl_init("$JOHN/kernels/wpapsk_kernel.cl", gpu_id, build_opts);

	/* Read LWS/GWS prefs from config or environment */
	opencl_get_user_preferences(OCL_CONFIG);

	// create kernels to execute
	crypt_kernel = wpapsk_init = clCreateKernel(program[gpu_id], "wpapsk_init", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel");
	wpapsk_loop = clCreateKernel(program[gpu_id], "wpapsk_loop", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel");
	wpapsk_pass2 = clCreateKernel(program[gpu_id], "wpapsk_pass2", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel");
	wpapsk_final_md5 = clCreateKernel(program[gpu_id], "wpapsk_final_md5", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel");
	wpapsk_final_sha1 = clCreateKernel(program[gpu_id], "wpapsk_final_sha1", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel");

	/* Enumerate GWS using *LWS=NULL (unless it was set explicitly) */
	if (!global_work_size)
		find_best_gws(self);

	/* Note: we ask for the kernels' max sizes, not the device's! */
	maxsize = get_kernel_max_lws(gpu_id, wpapsk_init);
	maxsize2 = get_kernel_max_lws(gpu_id, wpapsk_loop);
	if (maxsize2 < maxsize) maxsize = maxsize2;
	maxsize2 = get_kernel_max_lws(gpu_id, wpapsk_pass2);
	if (maxsize2 < maxsize) maxsize = maxsize2;
	maxsize2 = get_kernel_max_lws(gpu_id, wpapsk_final_md5);
	if (maxsize2 < maxsize) maxsize = maxsize2;
	maxsize2 = get_kernel_max_lws(gpu_id, wpapsk_final_sha1);
	if (maxsize2 < maxsize) maxsize = maxsize2;

	// Obey device limits
	max_mem = get_max_mem_alloc_size(gpu_id);
	while (global_work_size * v_width > max_mem / PLAINTEXT_LENGTH)
		global_work_size -= get_kernel_preferred_multiple(gpu_id,
		                                                  crypt_kernel);

	if (options.verbosity > 3)
		fprintf(stderr, "Max LWS %d\n", (int)maxsize);

	if (local_work_size > maxsize)
		local_work_size = maxsize;

	if (!local_work_size) {
		if (cpu(device_info[gpu_id])) {
			if (get_platform_vendor_id(platform_id) == DEV_INTEL)
				local_work_size = MIN(maxsize, 8);
			else
				local_work_size = 1;
		} else {
			create_clobj(global_work_size, self);
			self->methods.crypt_all = crypt_all_benchmark;
			opencl_find_best_workgroup_limit(self, maxsize,
			                                 gpu_id,
			                                 crypt_kernel);
			self->methods.crypt_all = crypt_all;
			release_clobj();
		}
	}

	self->params.min_keys_per_crypt = local_work_size < 8 ?
		8 * v_width : local_work_size * v_width;

	if (global_work_size < local_work_size)
		global_work_size = local_work_size;

	if (options.verbosity > 2)
		fprintf(stderr, "Local worksize (LWS) %d, Global worksize (GWS) %d\n", (int)local_work_size, (int)global_work_size);
	create_clobj(global_work_size, self);
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
	int i;
	size_t scalar_gws;

	global_work_size = ((count + (v_width * local_work_size - 1)) / (v_width * local_work_size)) * local_work_size;
	scalar_gws = global_work_size * v_width;

	/// Copy data to gpu
	HANDLE_CLERROR(clEnqueueWriteBuffer(queue[gpu_id], mem_in, CL_FALSE, 0, scalar_gws * 64, inbuffer, 0, NULL, NULL), "Copy data to gpu");

	/// Run kernel
	if (new_keys || strcmp(last_ssid, hccap.essid)) {
		HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], wpapsk_init, 1, NULL, &global_work_size, &local_work_size, 0, NULL, firstEvent), "Run initial kernel");

		for (i = 0; i < ITERATIONS / HASH_LOOPS; i++) {
			HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], wpapsk_loop, 1, NULL, &global_work_size, &local_work_size, 0, NULL, NULL), "Run loop kernel");
			HANDLE_CLERROR(clFinish(queue[gpu_id]), "Error running loop kernel");
			opencl_process_event();
		}

		HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], wpapsk_pass2, 1, NULL, &global_work_size, &local_work_size, 0, NULL, NULL), "Run intermediate kernel");

		for (i = 0; i < ITERATIONS / HASH_LOOPS; i++) {
			HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], wpapsk_loop, 1, NULL, &global_work_size, &local_work_size, 0, NULL, NULL), "Run loop kernel (2nd pass)");
			HANDLE_CLERROR(clFinish(queue[gpu_id]), "Error running loop kernel");
			opencl_process_event();
		}

		new_keys = 0;
		strcpy(last_ssid, hccap.essid);
	}

	if (hccap.keyver == 1)
		HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], wpapsk_final_md5, 1, NULL, &global_work_size, &local_work_size, 0, NULL, lastEvent), "Run final kernel (MD5)");
	else
		HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], wpapsk_final_sha1, 1, NULL, &global_work_size, &local_work_size, 0, NULL, lastEvent), "Run final kernel (SHA1)");
	HANDLE_CLERROR(clFinish(queue[gpu_id]), "Failed running final kernel");

	/// Read the result back
	HANDLE_CLERROR(clEnqueueReadBuffer(queue[gpu_id], mem_out, CL_TRUE, 0, sizeof(mic_t) * scalar_gws, mic, 0, NULL, NULL), "Copy result back");

	return count;
}

static int crypt_all_benchmark(int *pcount, struct db_salt *salt)
{
	int count = *pcount;

	/// Copy data to gpu
	BENCH_CLERROR(clEnqueueWriteBuffer(queue[gpu_id], mem_in, CL_FALSE, 0, key_buf_size, inbuffer, 0, NULL, NULL), "Copy data to gpu");

	/// Run kernels, no iterations for fast enumeration
	BENCH_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], wpapsk_init, 1, NULL, &global_work_size, &local_work_size, 0, NULL, NULL), "Run initial kernel");

	BENCH_CLERROR(clFinish(queue[gpu_id]), "Error running kernel");

	BENCH_CLERROR(clEnqueueNDRangeKernel(queue[gpu_id], wpapsk_loop, 1, NULL, &global_work_size, &local_work_size, 0, NULL, profilingEvent), "Run loop kernel");

	BENCH_CLERROR(clFinish(queue[gpu_id]), "Error running loop kernel");
	return count;
}

struct fmt_main fmt_opencl_wpapsk = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		BINARY_ALIGN,
		SALT_SIZE,
		SALT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_CASE | FMT_OMP,
		tests
	}, {
		init,
		done,
		fmt_default_reset,
		fmt_default_prepare,
		valid,
		fmt_default_split,
		binary,
		salt,
		fmt_default_source,
		{
			binary_hash_0,
			fmt_default_binary_hash_1,
			fmt_default_binary_hash_2,
			fmt_default_binary_hash_3,
			fmt_default_binary_hash_4,
			fmt_default_binary_hash_5,
			fmt_default_binary_hash_6
		},
		fmt_default_salt_hash,
		set_salt,
		set_key,
		get_key,
		clear_keys,
		crypt_all,
		{
			get_hash_0,
			get_hash_1,
			get_hash_2,
			get_hash_3,
			get_hash_4,
			get_hash_5,
			get_hash_6
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};
