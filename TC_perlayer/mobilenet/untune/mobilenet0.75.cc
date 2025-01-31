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
std::string depthwise_string = R"TC(
    def convolution(float(N,C,H,W) I, float(C,KH,KW) K) -> (O) {
        O(n,c,h,w) +=!
            I(n, c, h * <stride> + r_kh, w * <stride> + r_kw) * K(c, r_kh, r_kw)
    }
)TC";
/**************************** 卷积层参数替换函数 ***************************/
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
TEST(mobilenet, 0_75)
{
    double time_use = 0;
    double time_use_sum = 0;
    
    std::string conv_S1_string = change_string_parameter(convolution_string, 1);
    std::string conv_S2_string = change_string_parameter(convolution_string, 2);
    std::string depthwise_S1_string = change_string_parameter(depthwise_string, 1);
    std::string depthwise_S2_string = change_string_parameter(depthwise_string, 2);

    auto bestOptions_conv_S1 = get_convolution_bestOption<tc::CudaBackend>(conv_S1_string);
    auto bestOptions_conv_S2 = get_convolution_bestOption<tc::CudaBackend>(conv_S2_string);
    auto bestOptions_depthwise_S1 = get_convolution_bestOption<tc::CudaBackend>(depthwise_S1_string);
    auto bestOptions_depthwise_S2 = get_convolution_bestOption<tc::CudaBackend>(depthwise_S2_string);
    // pointwise卷积本质上是常规卷积, 直接复用常规卷积最佳参数

    /******************* conv_1x3x224x224_24x3x3x3_S2P1 ************************/
    time_use_sum = 0;
    at::Tensor input = makeATenTensor<tc::CudaBackend>({1, 3, 226, 226}); 
    at::Tensor kernel = makeATenTensor<tc::CudaBackend>({24, 3, 3, 3});
    auto pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S2_string, "convolution", {input, kernel}, bestOptions_conv_S2);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(conv_S2_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/conv_1x3x224x224_24x3x3x3_S2P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/conv_1x3x224x224_24x3x3x3_S2P1[Time]: " << time_use_sum << " us" << std::endl;

    /******************* depthwise_1x24x112x112_24x3x3_S1P1 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 24, 114, 114}); 
    kernel = makeATenTensor<tc::CudaBackend>({24, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(depthwise_S1_string, "convolution", {input, kernel}, bestOptions_depthwise_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(depthwise_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/depthwise_1x24x112x112_24x3x3_S1P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/depthwise_1x24x112x112_24x3x3_S1P1[Time]: " << time_use_sum << " us" << std::endl;
    
    /******************* conv_1x24x112x112_48x24x1x1_S1P0 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 24, 112, 112}); 
    kernel = makeATenTensor<tc::CudaBackend>({48, 24, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_conv_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/conv_1x24x112x112_48x24x1x1_S1P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/conv_1x24x112x112_48x24x1x1_S1P0[Time]: " << time_use_sum << " us" << std::endl;

    /******************* depthwise_1x48x112x112_48x3x3_S2P1 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 48, 114, 114}); 
    kernel = makeATenTensor<tc::CudaBackend>({48, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(depthwise_S2_string, "convolution", {input, kernel}, bestOptions_depthwise_S2);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(depthwise_S2_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/depthwise_1x48x112x112_48x3x3_S2P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/depthwise_1x48x112x112_48x3x3_S2P1[Time]: " << time_use_sum << " us" << std::endl;

    /******************* conv_1x48x56x56_96x48x1x1_S1P0 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 48, 56, 56}); 
    kernel = makeATenTensor<tc::CudaBackend>({96, 48, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_conv_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/conv_1x48x56x56_96x48x1x1_S1P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/conv_1x48x56x56_96x48x1x1_S1P0[Time]: " << time_use_sum << " us" << std::endl;

    /******************* depthwise_1x96x56x56_96x3x3_S1P1 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 96, 58, 58}); 
    kernel = makeATenTensor<tc::CudaBackend>({96, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(depthwise_S1_string, "convolution", {input, kernel}, bestOptions_depthwise_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(depthwise_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/depthwise_1x96x56x56_96x3x3_S1P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/depthwise_1x96x56x56_96x3x3_S1P1[Time]: " << time_use_sum << " us" << std::endl;

    /******************* conv_1x96x56x56_96x96x1x1_S1P0 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 96, 56, 56}); 
    kernel = makeATenTensor<tc::CudaBackend>({96, 96, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_conv_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/conv_1x96x56x56_96x96x1x1_S1P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/conv_1x96x56x56_96x96x1x1_S1P0[Time]: " << time_use_sum << " us" << std::endl;

    /******************* depthwise_1x96x56x56_96x3x3_S2P1 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 96, 58, 58}); 
    kernel = makeATenTensor<tc::CudaBackend>({96, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(depthwise_S2_string, "convolution", {input, kernel}, bestOptions_depthwise_S2);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(depthwise_S2_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/depthwise_1x96x56x56_96x3x3_S2P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/depthwise_1x96x56x56_96x3x3_S2P1[Time]: " << time_use_sum << " us" << std::endl;

    /******************* conv_1x96x28x28_192x96x1x1_S1P0 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 96, 28, 28}); 
    kernel = makeATenTensor<tc::CudaBackend>({192, 96, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_conv_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/conv_1x96x28x28_192x96x1x1_S1P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/conv_1x96x28x28_192x96x1x1_S1P0[Time]: " << time_use_sum << " us" << std::endl;

    /******************* depthwise_1x192x28x28_192x3x3_S1P1 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 192, 30, 30}); 
    kernel = makeATenTensor<tc::CudaBackend>({192, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(depthwise_S1_string, "convolution", {input, kernel}, bestOptions_depthwise_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(depthwise_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/depthwise_1x192x28x28_192x3x3_S1P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/depthwise_1x192x28x28_192x3x3_S1P1[Time]: " << time_use_sum << " us" << std::endl;

    /******************* conv_1x192x28x28_192x192x1x1_S1P0 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 192, 28, 28}); 
    kernel = makeATenTensor<tc::CudaBackend>({192, 192, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_conv_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/conv_1x192x28x28_192x192x1x1_S1P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/conv_1x192x28x28_192x192x1x1_S1P0[Time]: " << time_use_sum << " us" << std::endl;

    /******************* depthwise_1x192x28x28_192x3x3_S2P1 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 192, 30, 30}); 
    kernel = makeATenTensor<tc::CudaBackend>({192, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(depthwise_S2_string, "convolution", {input, kernel}, bestOptions_depthwise_S2);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(depthwise_S2_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/depthwise_1x192x28x28_192x3x3_S2P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/depthwise_1x192x28x28_192x3x3_S2P1[Time]: " << time_use_sum << " us" << std::endl;

    /******************* conv_1x192x14x14_384x192x1x1_S1P0 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 192, 14, 14}); 
    kernel = makeATenTensor<tc::CudaBackend>({384, 192, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_conv_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/conv_1x192x14x14_384x192x1x1_S1P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/conv_1x192x14x14_384x192x1x1_S1P0[Time]: " << time_use_sum << " us" << std::endl;

    /******************* depthwise_1x384x14x14_384x3x3_S1P1 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 384, 16, 16}); 
    kernel = makeATenTensor<tc::CudaBackend>({384, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(depthwise_S1_string, "convolution", {input, kernel}, bestOptions_depthwise_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(depthwise_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/depthwise_1x384x14x14_384x3x3_S1P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/depthwise_1x384x14x14_384x3x3_S1P1[Time]: " << time_use_sum << " us" << std::endl;

    /******************* conv_1x384x14x14_384x384x1x1_S1P0 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 384, 14, 14}); 
    kernel = makeATenTensor<tc::CudaBackend>({384, 384, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_conv_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/conv_1x128x14x14_128x128x1x1_S1P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/conv_1x128x14x14_128x128x1x1_S1P0[Time]: " << time_use_sum << " us" << std::endl;

    /******************* depthwise_1x384x14x14_384x3x3_S2P1 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 384, 16, 16}); 
    kernel = makeATenTensor<tc::CudaBackend>({384, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(depthwise_S2_string, "convolution", {input, kernel}, bestOptions_depthwise_S2);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(depthwise_S2_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/depthwise_1x384x14x14_384x3x3_S2P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/depthwise_1x384x14x14_384x3x3_S2P1[Time]: " << time_use_sum << " us" << std::endl;

    /******************* conv_1x384x7x7_768x384x1x1_S1P0 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 384, 7, 7}); 
    kernel = makeATenTensor<tc::CudaBackend>({768, 384, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_conv_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/conv_1x384x7x7_768x384x1x1_S1P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/conv_1x384x7x7_768x384x1x1_S1P0[Time]: " << time_use_sum << " us" << std::endl;

    /******************* depthwise_1x768x7x7_768x3x3_S1P1 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 768, 9, 9}); 
    kernel = makeATenTensor<tc::CudaBackend>({768, 3, 3});
    pExecutor = tc::aten::compile<tc::CudaBackend>(depthwise_S1_string, "convolution", {input, kernel}, bestOptions_depthwise_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(depthwise_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/depthwise_1x768x7x7_768x3x3_S1P1[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/depthwise_1x768x7x7_768x3x3_S1P1[Time]: " << time_use_sum << " us" << std::endl;

    /******************* conv_1x768x7x7_768x768x1x1_S1P0 ************************/
    time_use_sum = 0;
    input = makeATenTensor<tc::CudaBackend>({1, 768, 7, 7}); 
    kernel = makeATenTensor<tc::CudaBackend>({768, 768, 1, 1});
    pExecutor = tc::aten::compile<tc::CudaBackend>(conv_S1_string, "convolution", {input, kernel}, bestOptions_conv_S1);
    for (int i = 0; i < 15; i++) {

        time_use = get_convolution_output<tc::CudaBackend>(conv_S1_string, input, kernel, pExecutor);

        if (i >= 5) time_use_sum += time_use;
        std::cout << "mobilenet/conv_1x768x7x7_768x768x1x1_S1P0[Round" << i <<"]: " << time_use << " us" << std::endl;
    }
	time_use_sum /= 10;
	std::cout << "mobilenet/conv_1x768x7x7_768x768x1x1_S1P0[Time]: " << time_use_sum << " us" << std::endl;

}
/***************************** End of GPU Test ********************************/
// From root, run with: ./build/tc/examples/mobile --tuner_threads=10 --tuner_gen_pop_size=10 --tuner_gen_generations=3 --tuner_gen_number_elites=4
int main(int argc, char *argv[])
{
    ::testing::InitGoogleTest(&argc, argv);
    ::gflags::ParseCommandLineFlags(&argc, &argv, true);
    ::google::InitGoogleLogging(argv[0]);
    tc::aten::setAtenSeed(tc::initRandomSeed(), at::Backend::CUDA);
    return RUN_ALL_TESTS();
}