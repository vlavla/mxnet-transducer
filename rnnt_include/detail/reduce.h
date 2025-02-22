#include "rnnt_helper.h"

namespace warp_rnnt {

const int warp_size = 32;

template<int NT, typename T, typename Rop>
struct CTAReduce;

template<int NT, typename T, typename Rop>
struct CTAReduce {
    enum { Size = NT, Capacity = NT };
    struct Storage { T shared[Capacity]; };

    __device__ static T reduce(int tid, T x, Storage& storage, int count, Rop g) {
        T* s = storage.shared;
        s[tid] = x;
        __syncthreads();

        // Fold the data in half with each pass.
#pragma unroll
        for(int offset = NT / 2; offset >= warp_size; offset /= 2) {
            if(tid + offset < count && tid < offset) {
                // Read from the right half and store to the left half.
                x = g(x, s[offset + tid]);
                s[tid] = x;
            }
            __syncthreads();
        }

        T shuff;
        for (int offset = warp_size / 2; offset > 0; offset /= 2) {
            shuff = __shfl_down_sync(0xFFFFFFFF, x, offset);
            if (tid + offset < count && tid < offset)
                x = g(x, shuff);
        }
        return x;
    }
};

template <int NT, typename Iop, typename Rop, typename T>
__global__ void reduce_rows(Iop f, Rop g, const T* const acts, T* output, int num_rows) {

    typedef CTAReduce<NT, T, Rop> R;
    __shared__ typename R::Storage storage;

    int tid = threadIdx.x;
    int idx = tid;
    int col = blockIdx.x;
    T curr;

    // Each block works on a column
    if (idx < num_rows) {
        curr = f(acts[col * num_rows + idx]);
    }
    idx += NT;

    while (idx < num_rows) {
        curr = g(curr, f(acts[col * num_rows + idx]));
        idx += NT;
    }

    // Sum thread-totals over the CTA.
    curr = R::reduce(tid, curr, storage, num_rows, g);

    // Store result in out
    if (tid == 0)
        output[col] = curr;
}

template <int NT, typename Iop, typename Rop, typename T>
__global__ void reduce_minus(Iop f, Rop g, const T* const acts, T* output, int num_rows) {

    typedef CTAReduce<NT, T, Rop> R;
    __shared__ typename R::Storage storage;

    int tid = threadIdx.x;
    int idx = tid;
    int col = blockIdx.x;
    T curr;
    T max = output[col];

    // Each block works on a column
    if (idx < num_rows) {
        curr = f(acts[col * num_rows + idx] - max);
    }
    idx += NT;

    while (idx < num_rows) {
        curr = g(curr, f(acts[col * num_rows + idx] - max));
        idx += NT;
    }

    // Sum thread-totals over the CTA.
    curr = R::reduce(tid, curr, storage, num_rows, g);

    // Store result in out
    if (tid == 0)
        output[col] = -max - log(curr);
}

struct ReduceHelper {

    template<typename T, typename Iof, typename Rof>
    static void impl(Iof f, Rof g, const T* const acts, T* output, int num_rows, int num_cols, bool minus, cudaStream_t stream) {

        int grid_size;

        if (minus) {
            grid_size = num_cols;
            reduce_minus<128><<<grid_size, 128, 0, stream>>>
               (f, g, acts, output, num_rows);

        } else {
            grid_size = num_cols;
            reduce_rows<128><<<grid_size, 128, 0, stream>>>
               (f, g, acts, output, num_rows);
        }
    }
};


template<typename T, typename Iof, typename  Rof>
rnntStatus_t reduce(Iof f, Rof g, const T* const acts, T* output, int rows, int cols, bool minus, cudaStream_t stream) {
    ReduceHelper::impl(f, g, acts, output, rows, cols, minus, stream);
    cudaStreamSynchronize(stream);
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess)
        return RNNT_STATUS_EXECUTION_FAILED;

    return RNNT_STATUS_SUCCESS;
}

rnntStatus_t reduce_exp(const float* const acts, float *denom, int rows, int cols, bool minus, cudaStream_t stream) {
    return reduce(rnnt_helper::exponential<float>(), rnnt_helper::add<float>(), acts, denom, rows, cols, minus, stream);
}

rnntStatus_t reduce_max(const float* const acts, float *denom, int rows, int cols, bool minus, cudaStream_t stream) {
    return reduce(rnnt_helper::identity<float>(), rnnt_helper::maximum<float>(), acts, denom, rows, cols, minus, stream);
}

} // warp_rnnt
