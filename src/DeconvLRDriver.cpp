// corresponded header file
#include "DeconvLRDriver.hpp"
// necessary project headers
#include "DeconvLRCore.cuh"
#include "Helper.cuh"
// 3rd party libraries headers
#include <cuda_runtime.h>
// standard libraries headers
#include <exception>
#include <cstdio>
// system headers

struct DeconvLR::Impl {
    Impl()
        : iterations(10) {

    }

    ~Impl() {
        // TODO free iterParms
    }

    // volume size
    dim3 volumeSize;
    // voxel size
    struct {
        float3 raw;
        float3 psf;
    } voxelSize;

    /*
     * Algorithm configurations.
     */
    int iterations;
    Core::RL::Parameters iterParms;
};

// C++14 feature
template<typename T, typename ... Args>
std::unique_ptr<T> make_unique(Args&& ... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args) ...));
}

DeconvLR::DeconvLR()
    : pimpl(make_unique<Impl>()) {
}

DeconvLR::~DeconvLR() {

}

void DeconvLR::setResolution(
    const float dx, const float dy, const float dz,
    const float dpx, const float dpy, const float dpz
) {
    /*
     * Spatial frequency ratio (along one dimension)
     *
     *       1/(NS * DS)   NP   DP   NP
     *   R = ----------- = -- * -- = -- * r
     *       1/(NP * DP)   NS   DS   NS
     *
     *   NS, sample size
     *   DS, sample voxel size
     *   NP, PSF size
     *   DP, PSF voxel size
     *   r, voxel ratio
     */
    pimpl->voxelSize.raw = make_float3(dx, dy, dz);
    pimpl->voxelSize.psf = make_float3(dpx, dpy, dpz);
}

void DeconvLR::setVolumeSize(
    const size_t nx, const size_t ny, const size_t nz
) {
    //TODO probe for device specification
    if (nx > 2048 or ny > 2048 or nz > 2048) {
        throw std::range_error("volume size exceeds maximum constraints");
    }
    pimpl->volumeSize.x = nx;
    pimpl->volumeSize.y = ny;
    pimpl->volumeSize.z = nz;

    fprintf(
        stderr,
        "[INFO] volume size = %ux%ux%u\n",
        pimpl->volumeSize.x, pimpl->volumeSize.y, pimpl->volumeSize.z
    );
}

/*
 * ===========
 * PSF AND OTF
 * ===========
 */
void DeconvLR::setPSF(const ImageStack<uint16_t> &psf_u16) {
    fprintf(stderr, "[DEBUG] +++ ENTER setPSF() +++\n");

    /*
     * Ensure we are working with floating points.
     */
    ImageStack<float> psf(psf_u16);
    fprintf(
        stderr,
        "[INFO] PSF size = %ldx%ldx%ld\n",
        psf.nx(), psf.ny(), psf.nz()
    );

    /*
     * Align the PSF to center.
     */
    PSF::removeBackground(
        psf.data(),
        psf.nx(), psf.ny(), psf.nz()
    );
    float3 centroid = PSF::findCentroid(
        psf.data(),
        psf.nx(), psf.ny(), psf.nz()
    );
    fprintf(
        stderr,
        "[INFO] centroid = (%.2f, %.2f, %.2f)\n",
        centroid.x, centroid.y, centroid.z
    );

    /*
     * Shift the PSF around the centroid.
     */
    PSF::bindData(
        psf.data(),
        psf.nx(), psf.ny(), psf.nz()
    );
    PSF::alignCenter(
        psf.data(),
        psf.nx(), psf.ny(), psf.nz(),
        centroid
    );
    fprintf(stderr, "[DEBUG] PSF aligned to center\n");
    PSF::release();

    psf.saveAs("psf_aligned.tif");

    /*
     * Generate OTF texture.
     */
    OTF::fromPSF(
        psf.data(),
        psf.nx(), psf.ny(), psf.nz()
    );
    fprintf(stderr, "[DEBUG] template OTF generated\n");

    // allocate OTF memory
    cudaErrChk(cudaMalloc(
        &pimpl->iterParms.otf,
        (pimpl->volumeSize.x/2+1) * pimpl->volumeSize.y * pimpl->volumeSize.z * sizeof(cufftComplex)
    ));
    // start the interpolation
    OTF::interpolate(
        pimpl->iterParms.otf,
        pimpl->volumeSize.x/2+1, pimpl->volumeSize.y, pimpl->volumeSize.z,
        psf.nx()/2+1, psf.ny(), psf.nz(),
        pimpl->voxelSize.raw.x, pimpl->voxelSize.raw.y, pimpl->voxelSize.raw.z,
        pimpl->voxelSize.psf.x, pimpl->voxelSize.psf.y, pimpl->voxelSize.psf.z
    );
    OTF::release();
    fprintf(stderr, "[INFO] OTF established\n");

    CImg<float> otfCalc(pimpl->volumeSize.x/2+1, pimpl->volumeSize.y, pimpl->volumeSize.z);
    OTF::dumpComplex(
        otfCalc.data(),
        pimpl->iterParms.otf,
        otfCalc.width(), otfCalc.height(), otfCalc.depth()
    );
    otfCalc.save_tiff("otf_interp.tif");

	fprintf(stderr, "[DEBUG] +++ EXIT setPSF() +++\n");
}

void DeconvLR::initialize() {
    const dim3 volumeSize = pimpl->volumeSize;
    Core::RL::Parameters &iterParms = pimpl->iterParms;

    /*
     * Load dimension information into the iteration parameter.
     */
    iterParms.nx = volumeSize.x;
    iterParms.ny = volumeSize.y;
    iterParms.nz = volumeSize.z;
    iterParms.nelem = volumeSize.x * volumeSize.y * volumeSize.z;

    /*
     * Create FFT plans.
     */
     // FFT plans for estimation
     cudaErrChk(cufftPlan3d(
         &iterParms.fftHandle.forward,
         volumeSize.z, volumeSize.y, volumeSize.x,
         CUFFT_R2C
     ));
     cudaErrChk(cufftPlan3d(
         &iterParms.fftHandle.reverse,
         volumeSize.z, volumeSize.y, volumeSize.x,
         CUFFT_C2R
     ));

     //TODO attach callback device functions

     /*
      * Estimate memory usage from FFT procedures.
      */

     /*
      * Allocate device staging area.
      */
      size_t realSize =
          volumeSize.x * volumeSize.y * volumeSize.z * sizeof(cufftReal);
      size_t complexSize =
          (volumeSize.x/2+1) * volumeSize.y * volumeSize.z * sizeof(cufftComplex);

     // template
     cudaErrChk(cudaMalloc((void **)&iterParms.raw, realSize));

     // IO buffer
     cudaErrChk(cudaMalloc((void **)&iterParms.ioBuffer.input, realSize));
     cudaErrChk(cudaMalloc((void **)&iterParms.ioBuffer.output, realSize));

     // FFT Buffer
     cudaErrChk(cudaMalloc((void **)&iterParms.filterBuffer.complexA, complexSize));

     // RL Buffer
     cudaErrChk(cudaMalloc((void **)&iterParms.RLBuffer.realA, realSize));
}

//TODO scale output from float to uint16
void DeconvLR::process(
	ImageStack<float> &odata,
	const ImageStack<uint16_t> &idata
) {
    Core::RL::Parameters &iterParms = pimpl->iterParms;
    const size_t nelem = iterParms.nelem;

    // register the input data memory region on host as pinned
    cudaErrChk(cudaHostRegister(
        idata.data(),
        nelem * sizeof(float),
        cudaHostRegisterMapped
    ));

    // retrieve the host pointer
    uint16_t *d_idata = nullptr;
    cudaErrChk(cudaHostGetDevicePointer(&d_idata, idata.data(), 0));

    /*
     * Copy the data to buffer area along with type casts.
     */
    fprintf(stderr, "[DEBUG] %ld elements to type cast\n", nelem);
    Common::ushort2float(
        iterParms.ioBuffer.input,   // output
        d_idata,                    // input
        nelem
    );

    // duplicate the to store a copy of raw data
    cudaErrChk(cudaMemcpy(
        iterParms.raw,
        iterParms.ioBuffer.input,
        nelem * sizeof(float),
        cudaMemcpyDeviceToDevice
    ));

    /*
     * Release the pinned memory region.
     */
    cudaErrChk(cudaHostUnregister(idata.data()));

    /*
     * Execute the core functions.
     */
    const int nIter = 10; //pimpl->iterations;
    for (int iIter = 1; iIter <= nIter; iIter++) {
        Core::RL::step(
            iterParms.ioBuffer.output,  // output
            iterParms.ioBuffer.input,   // input
            iterParms
        );
        // swap A, B buffer
        std::swap(iterParms.ioBuffer.input, iterParms.ioBuffer.output);

        fprintf(stderr, "[DEBUG] %d/%d\n", iIter, nIter);
    }

    // swap back to avoid confusion
    std::swap(iterParms.ioBuffer.input, iterParms.ioBuffer.output);
    // copy back to host
    cudaErrChk(cudaMemcpy(
        odata.data(),
        iterParms.ioBuffer.output,
        nelem * sizeof(cufftReal),
        cudaMemcpyDeviceToHost
    ));
}
