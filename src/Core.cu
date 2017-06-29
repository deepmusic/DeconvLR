// corresponded header file
// necessary project headers
#include "DeconvRLImpl.cuh"
#include "Helper.cuh"
// 3rd party libraries headers
#include <cuda_runtime.h>
#include <cuComplex.h>
#include <thrust/device_vector.h>
#include <thrust/transform.h>
#include <thrust/transform_reduce.h>
#include <thrust/functional.h>
#include <thrust/execution_policy.h>
#include <thrust/inner_product.h>
#include <cufft.h>
// standard libraries headers
#include <cstdint>
// system headers

namespace DeconvRL {

namespace Core {

namespace RL {

enum class ConvType {
    PLAIN = 1, CONJUGATE
};

namespace {
// generic complex number operation
struct MultiplyAndScale
    : public thrust::binary_function<cuComplex, cuComplex, cuComplex> {
    MultiplyAndScale(const float c_)
        : c(c_) {
    }

    __host__ __device__
    cuComplex operator()(const cuComplex &a, const cuComplex &b) const {
        return cuCmulf(a, b)/c;
    }

private:
    const float c;
};

void filter(
    cufftReal *odata, const cufftReal *idata, const cufftComplex *otf,
    Core::RL::Parameters &parm
) {
    const size_t nelem = (parm.nx/2+1) * parm.ny * parm.nz;
    cufftComplex *buffer = (cufftComplex *)parm.filterBuffer.complexA;

    // convert to frequency space
    cudaErrChk(cufftExecR2C(
        parm.fftHandle.forward,
        const_cast<cufftReal *>(idata),
        buffer
    ));
    // element-wise multiplication and scale down
    thrust::transform(
        thrust::device,
        buffer, buffer+nelem,       // first input sequence
        otf,                        // second input sequence
        buffer,                     // output sequence
        MultiplyAndScale(1.0f/parm.nelem)
    );
    // convert back to real space
    cudaErrChk(cufftExecC2R(
        parm.fftHandle.reverse,
        buffer,
        odata
    ));
}

thrust::divides<float> DivfOp;
thrust::multiplies<float> MulfOp;

}

void step(
    float *odata, const float *idata,
    Core::RL::Parameters &parms
) {
    fprintf(stderr, "[DBG] +++ ENTER RL::step() +++\n");

    const size_t nelem = parms.nelem;
    cufftReal *buffer = parms.RLBuffer.realA;

    cufftComplex *otf = parms.otf;

    /*
     * \hat{f_{k+1}} =
     *     \hat{f_k} \left(
     *         h \ast \frac{g}{h \otimes \hat{f_k}}
     *     \right)
     */

    // reblur the image
    filter(buffer, idata, otf, parms);
    // error
    thrust::transform(
        thrust::device,
        parms.raw,  parms.raw+nelem,
        buffer,
        buffer, // output
        DivfOp
    );
    filter(buffer, buffer, otf, parms);
    // latent image
    thrust::transform(
        thrust::device,
        idata, idata+nelem,
        buffer,
        odata,  // output
        MulfOp
    );

    fprintf(stderr, "[DBG] +++ EXIT RL::step() +++\n");
}

}

namespace Biggs {

namespace {

struct ScaleAndAdd
    : public thrust::binary_function<float, float, float> {
    ScaleAndAdd(const float c_)
        : c(c_) {
    }

    __host__ __device__
    float operator()(const float &a, const float &b) const {
        return a + b*c;
    }

private:
    const float c;
};

}

void step(
    float *odata, const float *idata,
    Core::RL::Parameters &parm
) {
    // borrow space from odata, rename to avoid confusion
    float* iter = odata;
    // calcualte x_k
    RL::step(iter, idata, parm);

    fprintf(stderr, "153\n");

    // extract the definition
    float *prevIter = parm.predBuffer.prevIter;
    float *prevPred = parm.predBuffer.prevPred;

    fprintf(stderr, "159\n");

    // updateDir borrow buffer from prevIter
    float* updateDir = prevIter;
    // h_k in the paper
    // update_direction = prev_iter - iter;
    thrust::transform(
        thrust::device,
        prevIter, prevIter+parm.nelem,
        iter,
        updateDir,
        thrust::minus<float>()
    );

    fprintf(stderr, "173\n");

    // reuse space of idata
    float *pred = const_cast<float *>(idata);
    // calculate g_{k - 1} = x_k - y_{k - 1}.
    // pred_change = iter - prev_pred;
    thrust::transform(
        thrust::device,
        iter, iter+parm.nelem,
        prevPred,
        pred,
        thrust::minus<float>()
    );

    fprintf(stderr, "187\n");

    // calculate alpha (acceleration factor).
    float alpha =
        thrust::inner_product(
            thrust::device,
            pred, pred+parm.nelem,
            prevPred,
            0
        ) / (
            thrust::inner_product(
                thrust::device,
                prevPred, prevPred+parm.nelem,
                prevPred,
                0
            ) + std::numeric_limits<float>::epsilon()
        );
    fprintf(stderr, "[INF] alpha = %.2f\n", alpha);

    // save current predictions
    cudaErrChk(cudaMemcpy(
        prevIter,
        iter,
        parm.nelem * sizeof(float),
        cudaMemcpyDeviceToDevice
    ));
    cudaErrChk(cudaMemcpy(
        prevPred,
        pred,
        parm.nelem * sizeof(float),
        cudaMemcpyDeviceToDevice
    ));

    // calculate y_k
    // odata = iter + alpha * update_direction;
    thrust::transform(
        thrust::device,
        iter, iter+parm.nelem,
        updateDir,
        odata,
        ScaleAndAdd(alpha)
    );
}

}

}

namespace Common {

namespace {

template <typename T>
struct ToFloat
    : public thrust::unary_function<const T, float> {
    __host__ __device__
    float operator()(const T &v) const {
        return (float)v;
    }
};

}

void ushort2float(float *odata, const uint16_t *idata, const size_t nelem) {
    thrust::transform(
        thrust::device,
        idata, idata + nelem,   // input
        odata,                  // output
        ToFloat<uint16_t>()
    );
}

}

}
