//===-- LaunchRequestHandler.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DAP.h"
#include "EventHelper.h"
#include "JSONUtils.h"
#include "LLDBUtils.h"
#include "Protocol/ProtocolRequests.h"
#include "RequestHandler.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"

using namespace llvm;
using namespace lldb_dap::protocol;

namespace lldb_dap {

/// Launch request; value of command field is 'launch'.
Error LaunchRequestHandler::Run(const LaunchRequestArguments &arguments) const {
  // Validate that we have a well formed launch request.
  if (!arguments.launchCommands.empty() &&
      arguments.console != protocol::eConsoleInternal)
    return make_error<DAPError>(
        "'launchCommands' and non-internal 'console' are mutually exclusive");

  dap.SetConfiguration(arguments.configuration, /*is_attach=*/false);
  dap.last_launch_request = arguments;

  PrintWelcomeMessage();

  // This is a hack for loading DWARF in .o files on Mac where the .o files
  // in the debug map of the main executable have relative paths which
  // require the lldb-dap binary to have its working directory set to that
  // relative root for the .o files in order to be able to load debug info.
  if (!dap.configuration.debuggerRoot.empty())
    sys::fs::set_current_path(dap.configuration.debuggerRoot);

  // Run any initialize LLDB commands the user specified in the launch.json.
  // This is run before target is created, so commands can't do anything with
  // the targets - preRunCommands are run with the target.
  if (Error err = dap.RunInitCommands())
    return err;

  dap.ConfigureSourceMaps();

  lldb::SBError error;
  lldb::SBTarget target = dap.CreateTarget(error);
  if (error.Fail())
    return ToError(error);

  dap.SetTarget(target);

  // Run any pre run LLDB commands the user specified in the launch.json
  if (Error err = dap.RunPreRunCommands())
    return err;

  if (Error err = LaunchProcess(arguments))
    return err;

  dap.RunPostRunCommands();

  return Error::success();
}

void LaunchRequestHandler::PostRun() const {
  dap.SendJSON(CreateEventObject("initialized"));
}

} // namespace lldb_dap
