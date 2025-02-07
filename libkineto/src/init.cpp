/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * This library performs basic cupti event collection and reporting.
 *
 * Usage:
 * Library can be built as a standalone shared library or for inclusion in a
 * cuda binary using the libkineto.so and kineto build targets respectively.
 *
 * When included in a cuda binary, the library is initialized upon loading
 * by dlopen().
 * When used as a standalone library, it can be loaded by setting the
 * CUDA_INJECTION64_PATH environment variable (for the target process) to point
 * at the library, and the cuda driver will load it.
 *
 * Which events to profile can be specified in the config file pointed to
 * by KINETO_CONFIG as a comma separated list. See cupti documentation for
 * event names.
 *
 * The library will fail to initialize when no GPU is present on the system
 * (most likely because libcupti.so will not be found by the lazy loading
 * mechanism), but allows the application to continue.
 */

#include <memory>
#include <mutex>

#include "ActivityProfilerProxy.h"
#include "Config.h"
#ifdef HAS_CUPTI
#include "CuptiCallbackApi.h"
#include "EventProfilerController.h"
#endif
#include "cupti_call.h"
#include "libkineto.h"

#include "Logger.h"

namespace KINETO_NAMESPACE {

#ifdef HAS_CUPTI
static bool initialized = false;
static std::mutex initMutex;

static void initProfilers(
    CUpti_CallbackDomain /*domain*/,
    CUpti_CallbackId /*cbid*/,
    const CUpti_CallbackData* cbInfo) {
  CUpti_ResourceData* d = (CUpti_ResourceData*)cbInfo;
  CUcontext ctx = d->context;

  VLOG(0) << "CUDA Context created";
  std::lock_guard<std::mutex> lock(initMutex);

  if (!initialized) {
    libkineto::api().initProfilerIfRegistered();
    initialized = true;
    VLOG(0) << "libkineto profilers activated";
  }
  if (getenv("KINETO_DISABLE_EVENT_PROFILER") != nullptr) {
    VLOG(0) << "Event profiler disabled via env var";
  } else {
    ConfigLoader& config_loader = libkineto::api().configLoader();
    config_loader.initBaseConfig();
    EventProfilerController::start(ctx, config_loader);
  }
}

static void stopProfiler(
    CUpti_CallbackDomain /*domain*/,
    CUpti_CallbackId /*cbid*/,
    const CUpti_CallbackData* cbInfo) {
  CUpti_ResourceData* d = (CUpti_ResourceData*)cbInfo;
  CUcontext ctx = d->context;

  LOG(INFO) << "CUDA Context destroyed";
  std::lock_guard<std::mutex> lock(initMutex);
  EventProfilerController::stop(ctx);
}
#endif // HAS_CUPTI

} // namespace KINETO_NAMESPACE

// Callback interface with CUPTI and library constructors
using namespace KINETO_NAMESPACE;
extern "C" {

// Return true if no CUPTI errors occurred during init
bool libkineto_init(bool cpuOnly, bool logOnError) {
  bool success = true;
#ifdef HAS_CUPTI
  if (!cpuOnly) {
    // libcupti will be lazily loaded on this call.
    // If it is not available (e.g. CUDA is not installed),
    // then this call will return an error and we just abort init.
    auto& cbapi = CuptiCallbackApi::singleton();
    bool status = false;

    if (cbapi.initSuccess()){
      const CUpti_CallbackDomain domain = CUPTI_CB_DOMAIN_RESOURCE;
      status = cbapi.registerCallback(
          domain, CuptiCallbackApi::RESOURCE_CONTEXT_CREATED, initProfilers);
      status = status && cbapi.registerCallback(
          domain, CuptiCallbackApi::RESOURCE_CONTEXT_DESTROYED, stopProfiler);

      if (status) {
        status = cbapi.enableCallback(
            domain, CuptiCallbackApi::RESOURCE_CONTEXT_CREATED);
        status = status && cbapi.enableCallback(
            domain, CuptiCallbackApi::RESOURCE_CONTEXT_DESTROYED);
        }
    }

    if (!cbapi.initSuccess() || !status) {
      success = false;
      cpuOnly = true;
      if (logOnError) {
        CUPTI_CALL(cbapi.getCuptiStatus());
        LOG(WARNING) << "CUPTI initialization failed - "
                     << "CUDA profiler activities will be missing";
        LOG(INFO) << "If you see CUPTI_ERROR_INSUFFICIENT_PRIVILEGES, refer to "
                  << "https://developer.nvidia.com/nvidia-development-tools-solutions-err-nvgpuctrperm-cupti";
      }
    }
  }
#endif // HAS_CUPTI

  ConfigLoader& config_loader = libkineto::api().configLoader();
  libkineto::api().registerProfiler(
      std::make_unique<ActivityProfilerProxy>(cpuOnly, config_loader));

  return success;
}

// The cuda driver calls this function if the CUDA_INJECTION64_PATH environment
// variable is set
int InitializeInjection(void) {
  LOG(INFO) << "Injection mode: Initializing libkineto";
  libkineto_init(false /*cpuOnly*/, true /*logOnError*/);
  return 1;
}

void suppressLibkinetoLogMessages() {
  SET_LOG_SEVERITY_LEVEL(ERROR);
}

} // extern C
