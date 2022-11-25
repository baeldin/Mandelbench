#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS

#define NOMINMAX
#include <Windows.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "util/stb_image_write.h"

#include "maths/vec.h"

#include <algorithm>
#include <vector>
#include <cmath>
#include <thread>
#include <atomic>

#include <stdio.h>


enum RenderQuality
{
	TEST   = 6,
	LOW    = 6*6,
	MEDIUM = 6*6*6,
	FINAL  = 6*6*6*6
};

constexpr int res_multi = 8;
constexpr int res_div   = 3;
constexpr int xres = res_multi * 480 / res_div;
constexpr int yres = res_multi * 270 / res_div;
constexpr int num_frames  = 30 * 12 * 1;
constexpr int noise_size  = 1 << 8;
constexpr int num_samples = RenderQuality::MEDIUM;
constexpr double inv_samples = 1.0 / num_samples;
constexpr double aspect = (double)yres / (double)xres;

const bool save_frames = true;
const bool ramdrive = false;
const char * dir_prefix = (ramdrive ? "r:" : ".");


using u8 = unsigned char;
using rgb8u = vec<3, u8>;
using vec4f = vec<4, float>;



 // Excessively verbose function of didactic origin :)
inline double LinearMapping(double a, double b, double c, double d, double x) { return (x - a) / (b - a) * (d - c) + c; }

inline vec4f ImageFunction(double x, double y, double frame, int xres, int yres, int num_frames)
{
	constexpr int num_iters = 255;

	const double time  = LinearMapping(0, num_frames, 0.0, 3.141592653589793238 * 2, frame);
	const double time2 = std::cos(3.141592653589793238 - time) * 0.5 + 0.5;
	const double scale = 3.*std::exp(time2 * -11.0);
	const vec2d centre = vec2d(0, 0);
	const vec2d z0 = vec2d(
		LinearMapping(0, xres, -scale, scale, x),
		LinearMapping(0, yres, scale, -scale, y) * aspect) + centre;

	// Fast early out for main cardioid and period 2 bulb
	// Ref: https://en.wikipedia.org/wiki/Plotting_algorithms_for_the_Mandelbrot_set#Cardioid_/_bulb_checking
	/*const double q = (z0.x() - 0.25) * (z0.x() - 0.25) + z0.y() * z0.y();
	const bool cardioid = (q * (q + (z0.x() - 0.25)) <= 0.25 * z0.y() * z0.y());
	const bool bulb2 = ((z0.x() + 1) * (z0.x() + 1) + z0.y() * z0.y() < 0.0625);

	if (cardioid || bulb2)
		return vec4f(0);
	else*/
	{
		//constexpr double R = 25000000; // escape radius
		constexpr double R_smol = 0.000000001; // convergent bailout limit

		vec2d z = z0;
		vec2d z_old = vec2d(0., 0.); 
		int iteration = 0;
		real div_sum = 0.; // exp smoothing outside
		real conv_sum = 0.; // exp smoothing inside
		//for (; iteration < num_iters && (dot(z, z) < R * R); iteration++)
		for (; iteration < num_iters && (dot(z - z_old, z - z_old) > R_smol * R_smol); iteration++)
		{
			//z = vec2d(z.x() * z.x() - z.y() * z.y(), 2 * z.x() * z.y()) + z0;
			z_old = z;
			z = z - div_complex((cube_complex(z) - 1), square_complex(z) * 3.); // Newton
			//z = z - div_complex((cube_complex(z) - 1), square_complex(z) * 3.) + z0; // Nova
			div_sum += std::exp(-length(z));
			conv_sum += std::exp(-1./length(z_old - z));
		}
		// Binary decomposition colouring, see https://mathr.co.uk/mandelbrot/book-draft/#binary-decomposition
		/*const int binary = iteration == num_iters ? 0 : (z.y() > 0);

		const double log_r2 = std::log(dot(z, z));
		const double dwell = (log_r2 <= 0) ? 0 : num_iters - iteration + std::log2(log_r2);
		const float  dwell_loop = std::sin((float)dwell * 3.141592653589793238f * 2 * 0.125f) * 0.5f + 0.5f;

		const vec4f colours[2] = { vec4f(220, 80, 40, 256) / 256, vec4f(80, 50, 220, 256) / 256 };
		const vec4f col_out = colours[iteration % 2];
		return col_out * col_out * (3.0f * binary) - dwell_loop * 0.05f;*/
		// Simple grayscale exponential smoothing attempt
		if (iteration == num_iters)
			return vec4f(int(256 - 10 * conv_sum) % 256, int(256 - 10 * conv_sum) % 256, int(256 - 10 * conv_sum) % 256, 256) / 256;
		else
			return vec4f(int(256 - 50 * div_sum) % 256, int(256 - 50 * div_sum) % 256, int(256 - 50 * div_sum) % 256, 256) / 256;
	}
}


inline double sign(double v) { return (v == 0) ? 0 : (v > 0) ? 1.0 : -1.0; }

// Convert uniform distribution into triangle-shaped distribution, from https://www.shadertoy.com/view/4t2SDh
inline double triDist(double v)
{
	const double orig = v * 2 - 1;
	v = orig / std::sqrt(std::abs(orig));
	v = std::max((double)-1, v); // Nerf the NaN generated by 0*rsqrt(0). Thanks @FioraAeterna!
	v = v - sign(orig);
	return v;
}


template<int b>
constexpr inline double RadicalInverse(int i)
{
	constexpr double inv_b = 1.0 / b;
	double f = 1, r = 0;
	while (i > 0)
	{
		const int i_div_b = i / b;
		const int i_mod_b = i - b * i_div_b;
		f *= inv_b;
		r += i_mod_b * f;
		i  = i_div_b;
	}
	return r;
}


void Hilbert(const vec2i dx, const vec2i dy, vec2i p, int size, std::vector<int> & ordering_out)
{
	if (size > 1)
	{
		size >>= 1;
		Hilbert( dy,  dx, p, size, ordering_out); p += dy * size;
		Hilbert( dx,  dy, p, size, ordering_out); p += dx * size;
		Hilbert( dx,  dy, p, size, ordering_out); p += dx * (size - 1) - dy;
		Hilbert(-dy, -dx, p, size, ordering_out);
	}
	else ordering_out.push_back(p.y() * noise_size + p.x());
}


void RenderThreadFunc(
	const int frame,
	const vec2d * const samples,
	const uint16_t * const noise,
	std::atomic<int> * const counter,
	rgb8u * const image_out)
{
	// Get rounded up number of buckets in x and y
	constexpr int
		bucket_size = 4,
		x_buckets = (xres + bucket_size - 1) / bucket_size,
		y_buckets = (yres + bucket_size - 1) / bucket_size,
		loop_frames = num_frames / 2 + 1,
		num_buckets = x_buckets * y_buckets;

	while (true)
	{
		// Get the next bucket index atomically and exit if we're done
		const int bucket_i = counter->fetch_add(1);
		if (bucket_i >= num_buckets * loop_frames)
				break;

		// Get pixel ranges for current bucket
		const int
			bucket_y  = bucket_i / x_buckets,
			bucket_x  = bucket_i - x_buckets * bucket_y,
			bucket_x0 = bucket_x * bucket_size, bucket_x1 = std::min(bucket_x0 + bucket_size, xres),
			bucket_y0 = bucket_y * bucket_size, bucket_y1 = std::min(bucket_y0 + bucket_size, yres);

		for (int pix_y = bucket_y0; pix_y < bucket_y1; ++pix_y)
		for (int pix_x = bucket_x0; pix_x < bucket_x1; ++pix_x)
		{
			const double n = noise[(pix_y % noise_size) * noise_size + (pix_x % noise_size)] * (1.0 / 65536);

			vec4f sum = 0;
			for (int s = 0; s < num_samples; ++s)
			{
				// Randomise Hammserley sequence using per-pixel approximate blue noise
				double i = s * inv_samples + n; i = (i < 1) ? i : i - 1;
				double j = samples[s].x()  + n; j = (j < 1) ? j : j - 1;
				double k = samples[s].y()  + n; k = (k < 1) ? k : k - 1;

				sum += ImageFunction(
					pix_x + 0.5 + triDist(i),
					pix_y + 0.5 + triDist(j),
					frame + 0.5 + triDist(k),
					xres, yres, num_frames);
			}
			sum *= inv_samples;

			// Sqrt for approximate linear->sRGB conversion
			image_out[pix_y * xres + pix_x] =
			{
				std::max(0, std::min(255, (int)(std::sqrt(sum.x()) * 256))),
				std::max(0, std::min(255, (int)(std::sqrt(sum.y()) * 256))),
				std::max(0, std::min(255, (int)(std::sqrt(sum.z()) * 256))),
			};
		}
	}
}


int main(int argc, char ** argv)
{
#ifdef _WIN32
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif
#if _DEBUG
	const int num_threads = 1;
#else
	const int num_threads = std::thread::hardware_concurrency();
#endif
	printf("Rendering %d frames at res %d x %d with %d samples per pixel\n", num_frames, xres, yres, num_samples);

	std::vector<rgb8u> image(xres * yres);

	// Hammsersley sequence: https://en.wikipedia.org/wiki/Low-discrepancy_sequence#Hammersley_set
	std::vector<vec2d> samples(num_samples);
	for (int s = 0; s < num_samples; s++)
		samples[s] = { RadicalInverse<2>(s), RadicalInverse<3>(s) };

	// Approximate blue noise by recursive addition of golden ratio
	std::vector<uint16_t> noise(noise_size * noise_size);
	{
		std::vector<int> hilbert_vals;
		hilbert_vals.reserve(noise_size * noise_size);
		Hilbert(vec2i(1, 0), vec2i(0, 1), 0, noise_size, hilbert_vals);

		uint64_t v = 0;
		for (int i = 0; i < noise_size * noise_size; ++i)
		{
			v += 11400714819323198487ull;
			noise[hilbert_vals[i]] = (uint16_t)(v >> 48);
		}
	}

	std::vector<std::thread> render_threads(num_threads);

	FILE * frame_times_file = fopen("frame_times.csv", "w");

	const auto bench_start = std::chrono::system_clock::now();
	double total_render_time = 0;

	for (int frame = 0; frame < num_frames / 2 + 1; ++frame)
	{
		const auto t_start = std::chrono::system_clock::now();
		{
			std::atomic<int> counter = { 0 };
			for (int z = 0; z < num_threads; ++z) render_threads[z] = std::thread(RenderThreadFunc, frame, &samples[0], &noise[0], &counter, &image[0]);
			for (int z = 0; z < num_threads; ++z) render_threads[z].join();
		}
		const std::chrono::duration<double> elapsed_time = std::chrono::system_clock::now() - t_start;
		printf("Frame %d took %.2f seconds\n", frame, elapsed_time.count());
		total_render_time += elapsed_time.count();

		char filename[256];
		sprintf(filename, "%s/frames/frame%04d.png", dir_prefix, frame);
		if (save_frames) stbi_write_png(filename, xres, yres, 3, &image[0], xres * 3);

		fprintf(frame_times_file, "%d, %f\n", frame, elapsed_time.count());
	}
	printf("\n");

	fclose(frame_times_file);

	const std::chrono::duration<double> elapsed_time = std::chrono::system_clock::now() - bench_start;
	printf("Rendering animation took %.2f seconds (%.2f including saving)\n", total_render_time, elapsed_time.count());

	FILE * stats_f = fopen("benchmark_stats.txt", "w");
	fprintf(stats_f, "%f\n%f\n", total_render_time, elapsed_time.count());
	fclose(stats_f);

	for (int dst_frame = num_frames / 2 + 1; dst_frame < num_frames; ++dst_frame)
	{
		const int src_frame = num_frames - dst_frame;
		char cmd_str[512];

#if _WIN32
		sprintf(cmd_str, "copy \"%s\\frames\\frame%04d.png\" \"%s\\frames\\frame%04d.png\" /Y", dir_prefix, src_frame, dir_prefix, dst_frame);
		system(cmd_str);
#endif
	}

	return 0;
}
