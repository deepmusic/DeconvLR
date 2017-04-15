// corresponded header file
// necessary project headers
#include "DeconvLRCore.cuh"
// 3rd party libraries headers
#include <cuda_runtime.h>
// standard libraries headers
#include <iostream>
// system headers

// Kernel that executes on the CUDA device
__global__
void square_array_kernel(float *a, int N)
{
  int idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx<N) {
      a[idx] = a[idx] * a[idx];
  }
}

__host__
void square_array(float *a, int N) {
    int block_size = 4;
    int n_blocks = N/block_size + (N%block_size == 0 ? 0:1);
    std::cout << "running kernel" << std::endl;
    square_array_kernel<<<n_blocks, block_size>>>(a, N);
}
