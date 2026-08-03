#include <sycl/sycl.hpp>

#include <cstdlib>
#include <iostream>

#include "workload.h"
#include "workload_utils.h"

int main(int argc, char *argv[]) {
  const Config config = ParseCommandLine(argc, argv);

  sycl::queue queue(sycl::gpu_selector_v, sycl::property::queue::in_order{});
  std::cout << "[host] device: "
            << queue.get_device().get_info<sycl::info::device::name>() << "\n\n";
  const std::optional<uint32_t> igaPlatform = GetIgaPlatform(queue.get_device());
  if (!igaPlatform.has_value()) {
    std::cerr << "[host] WARNING: unable to map the Level Zero device IP version to IGA\n";
  }

  InitLevelZeroModuleDebugCollection(config);
  if (!Workload(queue)) {
    std::cerr << "[host] workload validation failed\n";
    ShutdownLevelZeroModuleDebugCollection();
    return EXIT_FAILURE;
  }
  ShutdownLevelZeroModuleDebugCollection();

  if (!WriteCollectedKernelDebugDataJson(config, igaPlatform)) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}