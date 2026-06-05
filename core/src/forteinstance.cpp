/*******************************************************************************
 * Copyright (c) 2024, 2025 Primetals Technologies Austria GmbH,
 *                          Martin Erich Jobst
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Contributors:
 *    Alois Zoitl, Martin Jobst - initial implementation and rework communication
 *                                infrastructure
 *******************************************************************************/

#include "forte/forteinstance.h"
#include "forte/devicefactory.h"

#ifdef FORTE_EET_MONITORING
// Called on Ctrl+C or VSCode stop (SIGTERM/SIGINT).
// Exports whatever samples exist at that moment.
static void onSignal(int) {
  CEETMonitor::getInstance().stopPeriodicExport();
  CEETMonitor::getInstance().exportAllCSV("eet_results");
  std::exit(0);
}
#endif

namespace forte {
  C4diacFORTEInstance::~C4diacFORTEInstance() {
    if (mActiveDevice) {
#ifdef FORTE_EET_MONITORING
      CEETMonitor::getInstance().stopPeriodicExport();
      CEETMonitor::getInstance().exportAllCSV("eet_results");
#endif
      mActiveDevice->deinitialize();
    }
  }

  bool C4diacFORTEInstance::startupNewDevice(const std::string &paMGRID) {
    if (mActiveDevice) {
#ifdef FORTE_EET_MONITORING
      // Stop the periodic export thread before tearing down the current
      // device. The export thread calls exportAllCSV which reads mSamples
      // under its own lock — stopping it first avoids a race with clearAllData
      // called during device teardown.
      CEETMonitor::getInstance().stopPeriodicExport();

      // Flush whatever samples the last periodic export did not yet capture.
      CEETMonitor::getInstance().exportAllCSV("eet_results");
#endif

      // we have a current active device stop it
      triggerDeviceShutdown();
      awaitDeviceShutdown();
      mActiveDevice->deinitialize();
    }
    mActiveDevice = DeviceFactory::create(paMGRID);
    if (mActiveDevice) {
      mActiveDevice->initialize();
      mActiveDevice->startDevice();

#ifdef FORTE_EET_MONITORING
      // Start periodic CSV export after the device is running so there are
      // FBs already producing measurements before the first export fires.
      // Interval: 10 s — adjust as needed for the evaluation setup.
      // paTargetSamples: defaults to 0 = run inde

      // Scenario 1:
      // CEETMonitor::getInstance().startPeriodicExport("eet_results", std::chrono::seconds(60), 5000);
      // Scenario 2:
      CEETMonitor::getInstance().mDefaultStrategy = CEETMonitor::DeadlineStrategy::P90;
      CEETMonitor::getInstance().mDeadlineMultiplier = 1.2;
      // CEETMonitor::getInstance().startPeriodicExport("eet_results", std::chrono::seconds(120), 1000);
      CEETMonitor::getInstance().startPeriodicExport("eet_results", std::chrono::seconds(120), 3000);
#endif
    }
    return mActiveDevice.operator bool();
  }

  void C4diacFORTEInstance::triggerDeviceShutdown() {
    if (mActiveDevice) {
#ifdef FORTE_EET_MONITORING
      CEETMonitor::getInstance().stopPeriodicExport();
      CEETMonitor::getInstance().exportAllCSV("eet_results");
#endif
      mActiveDevice->changeExecutionState(EMGMCommandType::Kill);
    }
  }

  void C4diacFORTEInstance::awaitDeviceShutdown() {
    if (mActiveDevice) {
      mActiveDevice->awaitShutdown();
    }
  }
} // namespace forte
