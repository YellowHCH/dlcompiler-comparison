#include <iostream>
#include <string>
#include <vector>
#include <cuda_runtime.h>
 
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <sys/time.h>

#include "common.h"

#include "tc/aten/aten.h"
#include "tc/aten/aten_autotuner.h"
#include "tc/aten/aten_compiler.h"
#include "tc/autotuner/genetic_search.h"
#include "tc/core/check.h"
#include "tc/core/cpu/cpu_mapping_options.h"
#include "tc/core/cpu/cpu_tc_executor.h"
#include "tc/core/cuda/cuda_mapping_options.h"
#include "tc/core/cuda/cuda_tc_executor.h"
#include "tc/core/flags.h"

std::string convolution_string = R"TC(
    def convolution(float(N,C,H,W) I, float(M,C,KH,KW) W1) -> (O) {
        O(n, m, h, w) +=! 
            I(n, r_c, h * <stride> + r_kh, w * <stride>+ r_kw) * W1(m, r_c, r_kh, r_kw)
    }
)TC";

/**************************** 卷积层/池化层参数替换函数 ***************************/
std::string change_string_parameter(std::string kernel_string, int stride)
{
    while (true) {
        auto pos = kernel_string.find(std::string("<stride>"));
        if (pos == std::string::npos)
            break;
        kernel_string = kernel_string.replace(pos, std::string("<stride>").size(), std::to_string(stride));
    }

    return kernel_string;
}
/************************ End of 卷积层/池化层参数替换函数 ************************/
/******************************* 最佳参数获取 ***********************************/
// 卷积层最佳参数获取
template <typename Backend>
typename Backend::MappingOptionsType
get_convolution_bestOption(std::string kernel_string)
{
    auto options = Backend::MappingOptionsType::makeNaiveMappingOptions();
    return options;
}
/*********************** End of 最佳参数获取 ************************************/
/************************ 获取对应的kernel并得到输出 *****************************/
template <typename Backend>
double
get_convolution_output(std::string kernel_string, at::Tensor I0, at::Tensor I1, std::unique_ptr<typename Backend::ExecutorType> & pExecutor)
{
    auto outputs = tc::aten::prepareOutputs(kernel_string, "convolution", {I0, I1});
    tc::aten::profile(*pExecutor, {I0, I1}, outputs);
    auto timings = tc::aten::profile(*pExecutor, {I0, I1}, outputs);
    // std::cout << " GPU convolution ran in: " << timings.kernelRuntime.toMicroSeconds() << "us\n";

    return timings.kernelRuntime.toMicroSeconds();
}
/********************** End of 获取对应kernel并得到输出 **************************/
/********************************** GPU 测试 ***********************************/
TEST(resnet, conv)
{
    double time_use = 0;
    double time_use_sum = 0;
    // float time_use = 0;
    // float time_use_sum = 0;
    // cudaEvent_t start;
    // cudaEvent_t end;
    // cudaEventCreate(&start);
    // cudaEventCreate(&end);

    std::string conv_S1_string = change_string_parameter(convolution_string, 1);
    std::string conv_S2_string = change_string_parameter(convolution_string, 2);
    auto bestOptions_S1 = get_convolution_bestOption<tc::CudaBackend>(conv_S1_string);
    auto bestOptions_S2 = get_convolution_bestOption<tc::CudaBackend>(conv_S2_string);

    /***********************conv_1x3x224x224_64x3x7x7_S2P3*********************/
    time_use_sum = 0;
    at::Tensor input = makeATenTensor<tc::CudaBackend>({1, 3, 230, 230}); // 1x3x224, padding = 3
    at::Tensor kernel = makeATenTensor<tc::CudaBackend>({64, 3, 7, 7});
    auto pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S2_string, "convolution", {input, kernel}, bestOptions_S2);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S2_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x3x224x224_64x3x7x7_S2P3[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x3x224x224_64x3x7x7_S2P3[Time]: " << time_use_sum << " us" << std::endl;

    /***********************conv_1x64x56x56_64x64x3x3_S1P1*********************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 64, 58, 58}); // 1x64x56x56, padding = 1
    kernel = makeATenTensor<tc::CudaBackend>({64, 64, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_S1);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x64x56x56_64x64x3x3_S1P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x64x56x56_64x64x3x3_S1P1[Time]: " << time_use_sum << " us" << std::endl;

    /***********************conv_1x64x56x56_128x64x3x3_S2P1*********************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 64, 58, 58}); // 1x64x56x56, padding = 1
    kernel = makeATenTensor<tc::CudaBackend>({128, 64, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S2_string, "convolution", {input, kernel}, bestOptions_S2);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S2_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x64x56x56_128x64x3x3_S2P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x64x56x56_128x64x3x3_S2P1[Time]: " << time_use_sum << " us" << std::endl;

    /***********************conv_1x64x56x56_128x64x1x1_S2P0*********************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 64, 56, 56}); // 1x64x56x56, padding = 0
    kernel = makeATenTensor<tc::CudaBackend>({128, 64, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S2_string, "convolution", {input, kernel}, bestOptions_S2);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S2_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x64x56x56_128x64x1x1_S2P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x64x56x56_128x64x1x1_S2P0[Time]: " << time_use_sum << " us" << std::endl;

    /***********************conv_1x128x28x28_128x128x3x3_S1P1*********************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 128, 30, 30}); // 1x128x28x28, padding = 1
    kernel = makeATenTensor<tc::CudaBackend>({128, 128, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_S1);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x128x28x28_128x128x3x3_S1P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x128x28x28_128x128x3x3_S1P1[Time]: " << time_use_sum << " us" << std::endl;

    /***********************conv_1x128x28x28_256x128x3x3_S2P1*********************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 128, 30, 30}); // 1x128x28x28, padding = 1
    kernel = makeATenTensor<tc::CudaBackend>({256, 128, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S2_string, "convolution", {input, kernel}, bestOptions_S2);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S2_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x128x28x28_256x128x3x3_S2P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x128x28x28_256x128x3x3_S2P1[Time]: " << time_use_sum << " us" << std::endl;

    /***********************conv_1x128x28x28_256x128x1x1_S2P0*********************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 128, 28, 28}); // 1x128x28x28, padding = 0
    kernel = makeATenTensor<tc::CudaBackend>({256, 128, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S2_string, "convolution", {input, kernel}, bestOptions_S2);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S2_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x128x28x28_256x128x1x1_S2P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x128x28x28_256x128x1x1_S2P0[Time]: " << time_use_sum << " us" << std::endl;

    /***********************conv_1x256x14x14_256x256x3x3_S1P1*********************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 256, 16, 16}); // 1x128x14x14, padding = 1
    kernel = makeATenTensor<tc::CudaBackend>({256, 256, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_S1);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x256x14x14_256x256x3x3_S1P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x256x14x14_256x256x3x3_S1P1[Time]: " << time_use_sum << " us" << std::endl;

    /***********************conv_1x256x14x14_512x256x3x3_S2P1*********************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 256, 16, 16}); // 1x128x14x14, padding = 1
    kernel = makeATenTensor<tc::CudaBackend>({512, 256, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S2_string, "convolution", {input, kernel}, bestOptions_S2);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S2_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x256x14x14_512x256x3x3_S2P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x256x14x14_512x256x3x3_S2P1[Time]: " << time_use_sum << " us" << std::endl;

    /***********************conv_1x256x14x14_512x256x1x1_S2P0*********************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 256, 14, 14}); // 1x256x14x14, padding = 0
    kernel = makeATenTensor<tc::CudaBackend>({512, 256, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S2_string, "convolution", {input, kernel}, bestOptions_S2);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S2_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x256x14x14_512x256x1x1_S2P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x256x14x14_512x256x1x1_S2P0[Time]: " << time_use_sum << " us" << std::endl;

    /***********************conv_1x512x7x7_512x512x3x3_S1P1*********************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 512, 9, 9}); // 1x512x7x7, padding = 1
    kernel = makeATenTensor<tc::CudaBackend>({512, 512, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_S1);
    for (int i = 0; i < 15; i++) {
        // cudaEventRecord(start, 0);

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);
        
        // cudaEventRecord(end, 0);
        // cudaEventSynchronize(start);
        // cudaEventSynchronize(end);
        // cudaEventElapsedTime(&time_use, start, end);
        if (i >= 5) time_use_sum += time_use;
        std::cout << "resnet/conv_1x512x7x7_512x512x3x3_S1P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "resnet/conv_1x512x7x7_512x512x3x3_S1P1[Time]: " << time_use_sum << " us" << std::endl;

    

    // cudaEventDestroy(start);
    // cudaEventDestroy(end);
}

/***************************** End of GPU Test ********************************/
// From root, run with: ./build/tc/examples/resnet
int main(int argc, char *argv[])
{
    ::testing::InitGoogleTest(&argc, argv);
    ::gflags::ParseCommandLineFlags(&argc, &argv, true);
    ::google::InitGoogleLogging(argv[0]);
    tc::aten::setAtenSeed(tc::initRandomSeed(), at::Backend::CUDA);
    return RUN_ALL_TESTS();
}
